#ifndef _HAPROXY_CPU_TOPO_T_H
#define _HAPROXY_CPU_TOPO_T_H

#include <haproxy/api-t.h>
#include <haproxy/cpuset-t.h>

/* CPU state flags used with CPU topology detection (ha_cpu_topo.st). We try
 * hard to rely on known info. For example we don't claim a CPU is bound or
 * online if we don't know, reason why instead we store offline or excluded.
 */
#define HA_CPU_F_EXCLUDED     0x0001  // this CPU was excluded at boot
#define HA_CPU_F_OFFLINE      0x0002  // this CPU is known to be offline

/* CPU topology descriptor. All the ID and IDX fields are initialized to -1
 * when not known. The identifiers there are mostly assigned on the fly using
 * increments and have no particular representation except the fact that CPUs
 * having the same ID there share the same designated resource. The flags are
 * preset to zero.
 */
struct ha_cpu_topo {
	ushort st;    // state flags (HA_CPU_F_*)
	short idx;    // CPU index as passed to the OS. Initially the entry index.
	short ca_id[5]; // cache ID for each level (L0 to L4)
	short ts_id;  // thread-set identifier (generally core number)
	short cl_id;  // cluster identifier (group of more intimate cores)
	short di_id;  // die ID (per package)
	short no_id;  // NUMA node identifier
	short pk_id;  // package identifier
	short tg_id;  // thread group ID
	short th_cnt; // number of siblings threads
	short th_id;  // thread ID among siblings of the same core
	short capa;   // estimated CPU relative capacity; more is better
};

/* Description of a CPU selection strategy. For now it only associates an
 * option name with a sorting function. More settings might come later.
 */
struct ha_cpu_selection {
	const char *name;          /* option name in the configuration */
	const char *desc;       /* short description for help messages */
	int (*cmp_cpu)(const void *a, const void *b); /* sort function */
};

#endif /* _HAPROXY_CPU_TOPO_T_H */
