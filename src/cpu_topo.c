#define _GNU_SOURCE

#include <dirent.h>
#include <sched.h>
#include <string.h>

#include <haproxy/api.h>
#include <haproxy/cpuset.h>
#include <haproxy/cpu_topo.h>
#include <haproxy/global.h>
#include <haproxy/tools.h>

/* CPU topology information, ha_cpuset_size() entries, allocated at boot */
struct ha_cpu_topo *ha_cpu_topo = NULL;
struct cpu_map *cpu_map;


/* Detects CPUs that are online on the system. It may rely on FS access (e.g.
 * /sys on Linux). Returns the number of CPUs detected or 0 if the detection
 * failed.
 */
int ha_cpuset_detect_online(struct hap_cpuset *set)
{
#if defined(__linux__)

	ha_cpuset_zero(set);

	/* contains a list of CPUs in the format <low>[-<high>][,...] */
	if (read_line_to_trash("%s/cpu/online", NUMA_DETECT_SYSTEM_SYSFS_PATH) >= 0) {
		const char *parse_cpu_set_args[2] = { trash.area, "\0" };

		if (parse_cpu_set(parse_cpu_set_args, set, NULL) != 0)
			ha_cpuset_zero(set);
	}

#elif defined(__FreeBSD__)

	struct hap_cpuset node_cpu_set;
	int ndomains, domain;
	size_t len = sizeof(ndomains);

	ha_cpuset_zero(set);

	/* retrieve the union of NUMA nodes as online CPUs */
	if (sysctlbyname("vm.ndomains", &ndomains, &len, NULL, 0) == 0) {
		BUG_ON(ndomains > MAXMEMDOM);

		for (domain = 0; domain < ndomains; domain++) {
			ha_cpuset_zero(&node_cpu_set);

			if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_DOMAIN, domain,
					       sizeof(node_cpu_set.cpuset), &node_cpu_set.cpuset) == -1)
				continue;

			ha_cpuset_or(set, &node_cpu_set);
		}
	}

#else // !__linux__, !__FreeBSD__

	ha_cpuset_zero(set);

#endif
	return ha_cpuset_count(set);
}

/* Detects the CPUs that will be used based on the ones the process is bound to
 * at boot. The principle is the following: all CPUs from the boot cpuset will
 * be used since we don't know upfront how individual threads will be mapped to
 * groups and CPUs.
 *
 * Returns non-zero on success, zero on failure. Note that it may not be
 * performed in the function above because some calls may rely on other items
 * being allocated (e.g. trash).
 */
int cpu_detect_usable(void)
{
	struct hap_cpuset boot_set = { };
	int maxcpus = ha_cpuset_size();
	int cpu;

	/* update the list with the CPUs currently bound to the current process */
	ha_cpuset_detect_bound(&boot_set);

	/* remove the known-excluded CPUs */
	for (cpu = 0; cpu < maxcpus; cpu++)
		if (!ha_cpuset_isset(&boot_set, cpu))
			ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;

	/* Update the list of currently offline CPUs. Normally it's a subset
	 * of the unbound ones, but we cannot infer anything if we don't have
	 * the info so we only update what we know.
	 */
	if (ha_cpuset_detect_online(&boot_set)) {
		for (cpu = 0; cpu < maxcpus; cpu++) {
			if (!ha_cpuset_isset(&boot_set, cpu))
				ha_cpu_topo[cpu].st |= HA_CPU_F_OFFLINE;
		}
	}

	return 0;
}

/* Detects CPUs that are bound to the current process. Returns the number of
 * CPUs detected or 0 if the detection failed.
 */
int ha_cpuset_detect_bound(struct hap_cpuset *set)
{
	ha_cpuset_zero(set);

	/* detect bound CPUs depending on the OS's API */
	if (0
#if defined(__linux__)
	    || sched_getaffinity(0, sizeof(set->cpuset), &set->cpuset) != 0
#elif defined(__FreeBSD__)
	    || cpuset_getaffinity(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1, sizeof(set->cpuset), &set->cpuset) != 0
#else
	    || 1 // unhandled platform
#endif
	    ) {
		/* detection failed */
		return 0;
	}

	return ha_cpuset_count(set);
}

/* Returns true if at least one cpu-map directive was configured, otherwise
 * false.
 */
int cpu_map_configured(void)
{
	int grp, thr;

	for (grp = 0; grp < MAX_TGROUPS; grp++) {
		for (thr = 0; thr < MAX_THREADS_PER_GROUP; thr++)
			if (ha_cpuset_count(&cpu_map[grp].thread[thr]))
				return 1;
	}
	return 0;
}

/* Dump the CPU topology <topo> for up to <maxcpus> for debugging purposes.
 * Offline CPUs are skipped.
 */
void cpu_dump_topology(const struct ha_cpu_topo *topo, int maxcpus)
{
	int lastcpu = 0;
	int has_smt = 0;
	int cpu, lvl;

	for (cpu = 0; cpu < maxcpus; cpu++) {
		if (!(ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE))
			lastcpu = cpu;
		if (ha_cpu_topo[cpu].th_cnt > 1)
			has_smt = 1;
	}

	for (cpu = 0; cpu <= lastcpu; cpu++) {
		if (ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE)
			continue;

		printf("[%s] thr=%3d -> cpu=%3d pk=%02d no=%02d di=%02d cl=%03d",
		       (ha_cpu_topo[cpu].st & HA_CPU_F_EXCLUDED) ? "----" : "keep",
		       cpu, ha_cpu_topo[cpu].idx,
		       ha_cpu_topo[cpu].pk_id,
		       ha_cpu_topo[cpu].di_id,
		       ha_cpu_topo[cpu].no_id,
		       ha_cpu_topo[cpu].cl_id);

		/* list only relevant cache levels */
		for (lvl = 4; lvl >= 0; lvl--) {
			if (ha_cpu_topo[cpu].ca_id[lvl] < 0)
				continue;
			printf(lvl < 3 ? " l%d=%02d" : " l%d=%03d", lvl, ha_cpu_topo[cpu].ca_id[lvl]);
		}

		printf(" ts=%03d capa=%d",
		       ha_cpu_topo[cpu].ts_id,
		       ha_cpu_topo[cpu].capa);

		if (has_smt) {
			if (ha_cpu_topo[cpu].th_cnt > 1)
				printf(" smt=%d/%d",
				       ha_cpu_topo[cpu].th_id,
				       ha_cpu_topo[cpu].th_cnt);
			else
				printf(" smt=%d",
				       ha_cpu_topo[cpu].th_cnt);
		}
		putchar('\n');
	}
}


/* function used by qsort to re-arrange CPUs by index only, to restore original
 * ordering.
 */
int _cmp_cpu_index(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent, should not happend) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by vicinity
 * only. -1 says a<b, 1 says a>b. The goal is to arrange the closest CPUs
 * together, preferring locality over performance in order to keep latency
 * as low as possible, so that when picking a fixed number of threads, the
 * closest ones are used in priority. It's also used to help arranging groups
 * at the end.
 */
int _cmp_cpu_locality(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, CCD */
	if (l->di_id >= 0 && l->di_id < r->di_id)
		return -1;
	if (l->di_id > r->di_id && r->di_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_id >= 0 && l->cl_id < r->cl_id)
		return -1;
	if (l->cl_id > r->cl_id && r->cl_id >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by capacity
 * first, then by vicinity. -1 says a<b, 1 says a>b. The goal is to use the
 * biggest CPUs and memory channels first before using the smallest ones, so
 * that when picking a fixed number of threads, the best ones are used in
 * priority. It's almost a reversal of the low-latency one that tries to avoid
 * as much as possible to share resources (noisy neighbors).
 */
int _cmp_cpu_performance(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, CPU capacity, used by big.little arm/arm64. Higher is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones.
	 */
	if (l->th_cnt > r->th_cnt)
		return -1;
	if (l->th_cnt < r->th_cnt)
		return  1;

	/* next, sibling ID: by keeping SMT threads apart, we can arrange to
	 * favor the maximum number of cores for a small thread count.
	 */
	if (l->th_id >= 0 && l->th_id < r->th_id)
		return -1;
	if (l->th_id > r->th_id && r->th_id >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_id >= 0 && l->cl_id < r->cl_id)
		return -1;
	if (l->cl_id > r->cl_id && r->cl_id >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, CCD */
	if (l->di_id >= 0 && l->di_id < r->di_id)
		return -1;
	if (l->di_id > r->di_id && r->di_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by vicinity
 * and capacity -1 says a<b, 1 says a>b. The goal is to arrange the closest
 * CPUs together, preferring locality over performance in order to keep latency
 * as low as possible, so that when picking a fixed number of threads, the
 * closest ones are used in priority.
 */
int _cmp_cpu_low_latency(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, CCD */
	if (l->di_id >= 0 && l->di_id < r->di_id)
		return -1;
	if (l->di_id > r->di_id && r->di_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, CPU capacity, used by big.little arm/arm64. Higher is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones.
	 */
	if (l->th_cnt > r->th_cnt)
		return -1;
	if (l->th_cnt < r->th_cnt)
		return  1;

	/* next, cluster */
	if (l->cl_id >= 0 && l->cl_id < r->cl_id)
		return -1;
	if (l->cl_id > r->cl_id && r->cl_id >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by capacity
 * and vicinity. -1 says a<b, 1 says a>b. The goal is to use the biggest CPUs
 * from the first CCDs first before using the ones from the second node, and
 * finally the smallest ones, so that when picking a fixed number of threads,
 * the best ones are used in priority, and from the same node if possible.
 */
int _cmp_cpu_balanced(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, CPU capacity, used by big.little arm/arm64. Higher is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones.
	 */
	if (l->th_cnt > r->th_cnt)
		return -1;
	if (l->th_cnt < r->th_cnt)
		return  1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, CCD */
	if (l->di_id >= 0 && l->di_id < r->di_id)
		return -1;
	if (l->di_id > r->di_id && r->di_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, sibling ID: by keeping SMT threads apart, we can arrange to
	 * favor the maximum number of cores for a small thread count.
	 */
	if (l->th_id >= 0 && l->th_id < r->th_id)
		return -1;
	if (l->th_id > r->th_id && r->th_id >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_id >= 0 && l->cl_id < r->cl_id)
		return -1;
	if (l->cl_id > r->cl_id && r->cl_id >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by capacity
 * and vicinity. -1 says a<b, 1 says a>b. The goal is to use the smallest
 * number of CPUs and the cheapest ones from the first CCDs first before using
 * the ones from the second node, so that when picking a fixed number of
 * threads, the lowest costs are applied.
 */
int _cmp_cpu_resource(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, CPU capacity, used by big.little arm/arm64. Lower is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return 1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  -1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones, so prefer
	 * the ones without.
	 */
	if (l->th_cnt > r->th_cnt)
		return 1;
	if (l->th_cnt < r->th_cnt)
		return  -1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, CCD */
	if (l->di_id >= 0 && l->di_id < r->di_id)
		return -1;
	if (l->di_id > r->di_id && r->di_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_id >= 0 && l->cl_id < r->cl_id)
		return -1;
	if (l->cl_id > r->cl_id && r->cl_id >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by cluster to
 * make sure no cluster crosses L3 boundaries. -1 says a<b, 1 says a>b. It's
 * only used during topology detection.
 */
int _cmp_cpu_cluster(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, cluster */
	if (l->cl_id >= 0 && l->cl_id < r->cl_id)
		return -1;
	if (l->cl_id > r->cl_id && r->cl_id >= 0)
		return  1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, CCD */
	if (l->di_id >= 0 && l->di_id < r->di_id)
		return -1;
	if (l->di_id > r->di_id && r->di_id >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* if no L3, then L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* re-order a CPU topology array by CPU index only. This is mostly used before
 * listing CPUs regardless of their characteristics.
 */
void cpu_reorder_by_index(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_index);
}

/* arrange a CPU topology array optimally to consider vicinity and performance
 * so that cutting this into thread groups can be done linearly.
 */
void cpu_reorder_by_performance(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_performance);
}

/* re-order a CPU topology array by locality to help form groups. */
void cpu_reorder_by_locality(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_locality);
}

/* re-order a CPU topology array by cluster id. */
void cpu_reorder_by_cluster(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_cluster);
}

/* CPU topology detection below, OS-specific */

#if defined(__linux__)

/* detect the CPU topology based on info in /sys */
int cpu_detect_topology(void)
{
	const char *parse_cpu_set_args[2];
	struct ha_cpu_topo cpu_id = { }; /* all zeroes */
	struct hap_cpuset node_cpu_set;
	struct dirent *de;
	int no_cache, no_topo, no_capa, no_clust;
	int no_die, no_cppc, no_freq;
	int maxcpus = 0;
	int lastcpu = 0;
	DIR *dir;
	int cpu;

	maxcpus = ha_cpuset_size();

	for (cpu = 0; cpu < maxcpus; cpu++)
		if (!(ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE))
			lastcpu = cpu;

	/* now let's only focus on bound CPUs to learn more about their
	 * topology, their siblings, their cache affinity etc. We can stop
	 * at lastcpu which matches the ID of the last known bound CPU
	 * when it's set. We'll pre-assign and auto-increment indexes for
	 * thread_set_id, cluster_id, l1/l2/l3 id, etc. We don't revisit entries
	 * already filled from the list provided by another CPU.
	 */

	if (!is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu"))
		goto skip_cpu;

	/* detect the presence of some kernel-specific fields */
	no_cache = no_topo = no_capa = no_clust = no_die = no_freq = no_cppc = -1;
	for (cpu = 0; cpu <= lastcpu; cpu++) {
		struct hap_cpuset siblings_list = { 0 };
		struct hap_cpuset cpus_list;
		int next_level = 1; // assume L1 if unknown
		int idx, level;
		int cpu2;

		if (ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE)
			continue;

		if (!is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d", cpu))
			continue;

		/* First, let's check the cache hierarchy. On systems exposing
		 * it, index0 generally is the L1D cache, index1 the L1I, index2
		 * the L2 and index3 the L3. But sometimes L1I/D are reversed,
		 * and some CPUs also have L0 or L4. Maybe some heterogenous
		 * SoCs even have inconsistent levels between clusters... Thus
		 * we'll scan all entries that we can find for each CPU and
		 * assign levels based on what is reported. The types generally
		 * are "Data", "Instruction", "Unified". We just ignore inst if
		 * found.
		 */
		if (no_cache < 0)
			no_cache = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cache", cpu);

		if (no_cache)
			goto skip_cache;

		for (idx = 0; idx < 10; idx++) {
			if (!is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cache/index%d", cpu, idx))
				break;

			if (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH
			                       "/cpu/cpu%d/cache/index%d/type", cpu, idx) >= 0 &&
			    strcmp(trash.area, "Instruction") == 0)
				continue;

			level = next_level;
			if (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH
			                       "/cpu/cpu%d/cache/index%d/level", cpu, idx) >= 0) {
				level = atoi(trash.area);
				next_level = level + 1;
			}

			if (level < 0 || level > 4)
				continue; // level out of bounds

			if (ha_cpu_topo[cpu].ca_id[level] >= 0)
				continue; // already filled

			if (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH
			                       "/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, idx) >= 0) {
				parse_cpu_set_args[0] = trash.area;
				parse_cpu_set_args[1] = "\0";
				if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0) {
					for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
						if (ha_cpuset_isset(&cpus_list, cpu2))
							ha_cpu_topo[cpu2].ca_id[level] = cpu_id.ca_id[level];
					}
					cpu_id.ca_id[level]++;
				}
			}
		}

	skip_cache:
		if (no_topo < 0)
			no_topo = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology", cpu);

		if (no_topo)
			goto skip_topo;

		/* Now let's try to get more info about how the cores are
		 * arranged in packages, clusters, cores, threads etc. It
		 * overlaps a bit with the cache above, but as not all systems
		 * provide all of these, they're quite complementary in fact.
		 */

		/* thread siblings list will allow to figure which CPU threads
		 * share the same cores, and also to tell apart cores that
		 * support SMT from those which do not. When mixed, generally
		 * the ones with SMT are big cores and the ones without are the
		 * small ones. We also read the entry if the cluster_id is not
		 * known because we'll have to compare both values.
		 */
		if ((ha_cpu_topo[cpu].ts_id < 0 || ha_cpu_topo[cpu].cl_id < 0) &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/thread_siblings_list", cpu) >= 0) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &siblings_list, NULL) == 0) {
				int sib_id = 0;

				cpu_id.th_cnt = ha_cpuset_count(&siblings_list);
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&siblings_list, cpu2)) {
						ha_cpu_topo[cpu2].ts_id  = cpu_id.ts_id;
						ha_cpu_topo[cpu2].th_cnt = cpu_id.th_cnt;
						ha_cpu_topo[cpu2].th_id  = sib_id++;
					}
				}
				cpu_id.ts_id++;
			}
		}

		/* clusters of cores when they exist, can be smaller and more
		 * precise than core lists (e.g. big.little), otherwise use
		 * core lists. Note that we purposely ignore clusters that are
		 * reportedly equal to the siblings list, because some machines
		 * report one distinct cluster per core (e.g. some armv7 and
		 * intel 14900).
		 */
		if (no_clust < 0)
			no_clust = !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/cluster_cpus_list", cpu);

		if (!no_clust && ha_cpu_topo[cpu].cl_id < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/cluster_cpus_list", cpu) >= 0) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0 &&
			    (memcmp(&cpus_list, &siblings_list, sizeof(cpus_list)) != 0)) {
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&cpus_list, cpu2))
						ha_cpu_topo[cpu2].cl_id = cpu_id.cl_id;
				}
				cpu_id.cl_id++;
			}
		} else if (ha_cpu_topo[cpu].cl_id < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/core_siblings_list", cpu) >= 0) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0) {
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&cpus_list, cpu2))
						ha_cpu_topo[cpu2].cl_id = cpu_id.cl_id;
				}
				cpu_id.cl_id++;
			}
		}

		/* Dies when they exist ("ccd"). On modern CPUs there can be
		 * multiple dies each with multiple L3 inside a single package.
		 */
		if (no_die < 0)
			no_die = !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/die_cpus_list", cpu);

		if (!no_die && ha_cpu_topo[cpu].di_id < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/die_cpus_list", cpu) >= 0) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0) {
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&cpus_list, cpu2))
						ha_cpu_topo[cpu2].di_id = cpu_id.di_id;
				}
				cpu_id.di_id++;
			}
		}

		/* package CPUs list, like nodes, are generally a hard limit
		 * for groups, which must not span over multiple of them. On
		 * some systems, the package_cpus_list is not always provided,
		 * so we may fall back to the physical package id from each
		 * CPU, whose number starts at 0. The first one is preferred
		 * because it provides a list in a single read().
		 */
		if (ha_cpu_topo[cpu].pk_id < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/package_cpus_list", cpu) >= 0) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0) {
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&cpus_list, cpu2))
						ha_cpu_topo[cpu2].pk_id = cpu_id.pk_id;
				}
				cpu_id.pk_id++;
			}
		}

		if (ha_cpu_topo[cpu].pk_id < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/physical_package_id", cpu) >= 0) {
			if (trash.data)
				ha_cpu_topo[cpu].pk_id = str2uic(trash.area);
		}

	skip_topo:
		if (no_capa < 0)
			no_capa = !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpu_capacity", cpu);

		/* CPU capacity is a relative notion to compare little and big
		 * cores. Usually the values encountered in field set the big
		 * CPU's nominal capacity to 1024 and the other ones below.
		 */
		if (!no_capa && ha_cpu_topo[cpu].capa < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpu_capacity", cpu) >= 0) {
			if (trash.data)
				ha_cpu_topo[cpu].capa = str2uic(trash.area);
		}

		/* When cpu_capacity is not available, sometimes acpi_cppc is
		 * available on servers to provide an equivalent metric allowing
		 * to distinguish big from small cores. Values as low as 15 and
		 * as high as 260 were seen there. Note that only nominal_perf
		 * is trustable, as nominal_freq may return zero. It's also
		 * more reliable than the max cpufreq values because it doesn't
		 * seem to take into account the die quality.
		 */
		if (no_cppc < 0)
			no_cppc = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/acpi_cppc", cpu);

		if (!no_cppc && ha_cpu_topo[cpu].capa < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/acpi_cppc/nominal_perf", cpu) >= 0) {
			if (trash.data)
				ha_cpu_topo[cpu].capa = str2uic(trash.area);
		}

		/* Finally if none of them is available we can have a look at
		 * cpufreq's max cpu frequency.
		 */
		if (no_freq < 0)
			no_freq = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpufreq", cpu);

		if (!no_freq && ha_cpu_topo[cpu].capa < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpufreq/scaling_max_freq", cpu) >= 0) {
			/* This is in kHz turn it to MHz to stay below 32k */
			if (trash.data)
				ha_cpu_topo[cpu].capa = (str2uic(trash.area) + 999U) / 1000U;
		}
	}

 skip_cpu:
	/* Now locate NUMA node IDs if any */

	dir = opendir(NUMA_DETECT_SYSTEM_SYSFS_PATH "/node");
	if (dir) {
		while ((de = readdir(dir))) {
			long node_id;
			char *endptr;

			/* dir name must start with "node" prefix */
			if (strncmp(de->d_name, "node", 4) != 0)
				continue;

			/* dir name must be at least 5 characters long */
			if (!de->d_name[4])
				continue;

			/* dir name must end with a non-negative numeric id */
			node_id = strtol(&de->d_name[4], &endptr, 10);
			if (*endptr || node_id < 0)
				continue;

			/* all tests succeeded, it's in the form "node%d" */
			if (read_line_to_trash("%s/node/%s/cpulist", NUMA_DETECT_SYSTEM_SYSFS_PATH, de->d_name) >= 0) {
				parse_cpu_set_args[0] = trash.area;
				parse_cpu_set_args[1] = "\0";
				if (parse_cpu_set(parse_cpu_set_args, &node_cpu_set, NULL) == 0) {
					for (cpu = 0; cpu < maxcpus; cpu++)
						if (ha_cpuset_isset(&node_cpu_set, cpu))
							ha_cpu_topo[cpu].no_id = node_id;
				}
			}
		}
		/* done */
		closedir(dir);
	}

	/* Now we'll sort CPUs by topology and assign cluster IDs to those that
	 * don't yet have one, based on the die/pkg/llc
	 */
	cpu_reorder_by_locality(ha_cpu_topo, maxcpus);
	for (cpu = 0; cpu <= lastcpu; cpu++) {
		if (ha_cpu_topo[cpu].cl_id < 0) {
			/* cluster not assigned, we'll compare pkg/die/llc with
			 * the last CPU's and verify if we need to create a new
			 * cluster ID. Note that some platforms don't report
			 * cache.
			 */
			if (cpu &&
			    ((ha_cpu_topo[cpu].pk_id != ha_cpu_topo[cpu-1].pk_id) ||
			     (ha_cpu_topo[cpu].no_id != ha_cpu_topo[cpu-1].no_id) ||
			     (ha_cpu_topo[cpu].di_id != ha_cpu_topo[cpu-1].di_id) ||
			     (ha_cpu_topo[cpu].ca_id[4] != ha_cpu_topo[cpu-1].ca_id[4]) ||
			     (ha_cpu_topo[cpu].ca_id[4] < 0 && // no l4 ? check L3
			      ((ha_cpu_topo[cpu].ca_id[3] != ha_cpu_topo[cpu-1].ca_id[3]) ||
			       (ha_cpu_topo[cpu].ca_id[3] < 0 && // no l3 ? check L2
				(ha_cpu_topo[cpu].ca_id[2] != ha_cpu_topo[cpu-1].ca_id[2]))))))
				cpu_id.cl_id++;
			ha_cpu_topo[cpu].cl_id = cpu_id.cl_id;
		}
	}

	/* and now we must check that CPUs having different LLCs have different
	 * cluster IDs, otherwise we assign new ones.
	 */
	cpu_reorder_by_cluster(ha_cpu_topo, maxcpus);

	for (cpu = 0; cpu < lastcpu; cpu++) {
		int changed = 0;
		int cpu2 = cpu + 1;

		while (cpu2 <= lastcpu &&
		       (ha_cpu_topo[cpu2].cl_id == ha_cpu_topo[cpu].cl_id) &&
		       ((ha_cpu_topo[cpu2].pk_id != ha_cpu_topo[cpu].pk_id) ||
		        (ha_cpu_topo[cpu2].no_id != ha_cpu_topo[cpu].no_id) ||
		        (ha_cpu_topo[cpu2].di_id != ha_cpu_topo[cpu].di_id) ||
			(ha_cpu_topo[cpu].ca_id[4] != ha_cpu_topo[cpu].ca_id[4]) ||
			(ha_cpu_topo[cpu].ca_id[4] < 0 && // no l4 ? check L3
			 ((ha_cpu_topo[cpu].ca_id[3] != ha_cpu_topo[cpu].ca_id[3]) ||
			  (ha_cpu_topo[cpu].ca_id[3] < 0 && // no l3 ? check L2
			   (ha_cpu_topo[cpu].ca_id[2] != ha_cpu_topo[cpu].ca_id[2])))))) {
			ha_cpu_topo[cpu2].cl_id = cpu_id.cl_id;
			cpu2++;
			changed = 1;
		}

		if (changed)
			cpu_id.cl_id++;
	}

	cpu_reorder_by_index(ha_cpu_topo, maxcpus);

	return 1;
}

#elif defined(__FreeBSD__)

int cpu_detect_topology(void)
{
	struct hap_cpuset node_cpu_set;
	int maxcpus = ha_cpuset_size();
	int ndomains, domain, cpu;
	size_t len = sizeof(ndomains);

	/* Try to detect NUMA nodes */
	if (sysctlbyname("vm.ndomains", &ndomains, &len, NULL, 0) == 0) {
		BUG_ON(ndomains > MAXMEMDOM);

		/* For each domain we'll reference the domain ID in the belonging
		 * CPUs.
		 */
		for (domain = 0; domain < ndomains; domain++) {
			ha_cpuset_zero(&node_cpu_set);

			if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_DOMAIN, domain,
					       sizeof(node_cpu_set.cpuset), &node_cpu_set.cpuset) == -1)
				continue;

			for (cpu = 0; cpu < maxcpus; cpu++)
				if (ha_cpuset_isset(&node_cpu_set, cpu))
					ha_cpu_topo[cpu].no_id = domain;
		}
	}
	return 1;
}

#else // !__linux__, !__FreeBSD__

int cpu_detect_topology(void)
{
	return 1;
}

#endif // OS-specific cpu_detect_topology()

/* Allocates everything needed to store CPU topology at boot.
 * Returns non-zero on success, zero on failure.
 */
static int cpu_topo_alloc(void)
{
	int maxcpus = ha_cpuset_size();
	int cpu;

	/* allocate the structures used to store CPU topology info */
	cpu_map = calloc(MAX_TGROUPS, sizeof(*cpu_map));
	if (!cpu_map)
		return 0;

	/* allocate the structures used to store CPU topology info */
	ha_cpu_topo = (struct ha_cpu_topo*)malloc(maxcpus * sizeof(*ha_cpu_topo));
	if (!ha_cpu_topo)
		return 0;

	/* preset all fields to -1 except the index and the state flags which
	 * are assumed to all be bound and online unless detected otherwise.
	 */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		memset(&ha_cpu_topo[cpu], 0xff, sizeof(*ha_cpu_topo));
		ha_cpu_topo[cpu].st  = 0;
		ha_cpu_topo[cpu].idx = cpu;
	}

	return 1;
}

static void cpu_topo_deinit(void)
{
	ha_free(&ha_cpu_topo);
	ha_free(&cpu_map);
}

INITCALL0(STG_ALLOC, cpu_topo_alloc);
REGISTER_POST_DEINIT(cpu_topo_deinit);
