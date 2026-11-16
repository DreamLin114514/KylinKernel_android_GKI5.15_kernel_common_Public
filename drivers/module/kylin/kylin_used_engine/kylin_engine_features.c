// SPDX-License-Identifier: GPL-2.0-only
/*
 * kylin_used_engine - built-in features.
 *
 * info : system identity/load/memory snapshot (single-value nodes)
 * sched: per-CPU frequency
 * mem  : slab/cache digest + trim & swappiness knobs
 * top  : heaviest tasks (one "pid comm ticks" per line)
 * psi  : pressure stall info per metric (percent * 1000)
 */
#define pr_fmt(fmt) "kylin_engine: " fmt

#include <linux/kylin_used_engine.h>
#include <linux/compaction.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/psi.h>
#include <linux/sched.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
#include <linux/vmstat.h>

extern void drop_slab(void);		/* mm/vmscan.c */

/* =============================== info =============================== */

static void info_release_show(struct kylin_engine_feature *f,
			      struct seq_file *m, void *data)
{
	seq_printf(m, "%s\n", init_utsname()->release);
}

static void info_uptime_show(struct kylin_engine_feature *f,
			     struct seq_file *m, void *data)
{
	seq_printf(m, "%llu\n", ktime_get_boottime_seconds());
}

static void info_nr_threads_show(struct kylin_engine_feature *f,
				 struct seq_file *m, void *data)
{
	seq_printf(m, "%d\n", nr_threads);
}

static void info_procs_running_show(struct kylin_engine_feature *f,
				    struct seq_file *m, void *data)
{
	seq_printf(m, "%u\n", nr_running());
}

static void info_procs_iowait_show(struct kylin_engine_feature *f,
				   struct seq_file *m, void *data)
{
	seq_printf(m, "%u\n", nr_iowait());
}

static void info_load_show(struct kylin_engine_feature *f,
			   struct seq_file *m, void *data)
{
	unsigned long idx = (unsigned long)data;

	/* avenrun is FIXED_1 fixed point; report centi-load (12.34 -> 1234) */
	seq_printf(m, "%lu\n", (avenrun[idx] * 100) >> FSHIFT);
}

static void info_mem_total_show(struct kylin_engine_feature *f,
				struct seq_file *m, void *data)
{
	struct sysinfo si;

	si_meminfo(&si);
	seq_printf(m, "%llu\n",
		   ((u64)si.totalram * si.mem_unit) >> 10);
}

static void info_mem_free_show(struct kylin_engine_feature *f,
			       struct seq_file *m, void *data)
{
	struct sysinfo si;

	si_meminfo(&si);
	seq_printf(m, "%llu\n",
		   ((u64)si.freeram * si.mem_unit) >> 10);
}

static void info_mem_available_show(struct kylin_engine_feature *f,
				    struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n",
		   (unsigned long)si_mem_available() << (PAGE_SHIFT - 10));
}

static void info_swap_total_show(struct kylin_engine_feature *f,
				 struct seq_file *m, void *data)
{
	struct sysinfo si;

	si_swapinfo(&si);
	seq_printf(m, "%llu\n",
		   ((u64)si.totalswap * si.mem_unit) >> 10);
}

static void info_swap_free_show(struct kylin_engine_feature *f,
				struct seq_file *m, void *data)
{
	struct sysinfo si;

	si_swapinfo(&si);
	seq_printf(m, "%llu\n",
		   ((u64)si.freeswap * si.mem_unit) >> 10);
}

static struct kylin_engine_stat ke_info_stats[] = {
	{ "release",		info_release_show },
	{ "uptime_s",		info_uptime_show },
	{ "nr_threads",		info_nr_threads_show },
	{ "procs_running",	info_procs_running_show },
	{ "procs_iowait",	info_procs_iowait_show },
	{ "load1_x100",		info_load_show, (void *)0 },
	{ "load5_x100",		info_load_show, (void *)1 },
	{ "load15_x100",	info_load_show, (void *)2 },
	{ "mem_total_kb",	info_mem_total_show },
	{ "mem_free_kb",	info_mem_free_show },
	{ "mem_available_kb",	info_mem_available_show },
	{ "swap_total_kb",	info_swap_total_show },
	{ "swap_free_kb",	info_swap_free_show },
};

static void info_all_show(struct kylin_engine_feature *f, struct seq_file *m)
{
	struct sysinfo si;
	int i;

	si_meminfo(&si);
	si_swapinfo(&si);
	seq_printf(m, "release %s\n", init_utsname()->release);
	seq_printf(m, "uptime_s %llu\n", ktime_get_boottime_seconds());
	seq_printf(m, "nr_threads %d\n", nr_threads);
	seq_printf(m, "procs_running %u\n", nr_running());
	seq_printf(m, "procs_iowait %u\n", nr_iowait());
	for (i = 0; i < 3; i++)
		seq_printf(m, "load%d_x100 %lu\n",
			   i ? i * 5 : 1, (avenrun[i] * 100) >> FSHIFT);
	seq_printf(m, "mem_total_kb %llu\n", ((u64)si.totalram * si.mem_unit) >> 10);
	seq_printf(m, "mem_free_kb %llu\n", ((u64)si.freeram * si.mem_unit) >> 10);
	seq_printf(m, "mem_available_kb %lu\n",
		   (unsigned long)si_mem_available() << (PAGE_SHIFT - 10));
	seq_printf(m, "swap_total_kb %llu\n", ((u64)si.totalswap * si.mem_unit) >> 10);
	seq_printf(m, "swap_free_kb %llu\n", ((u64)si.freeswap * si.mem_unit) >> 10);
}

static struct kylin_engine_feature ke_info_feature = {
	.name		= "info",
	.stats		= ke_info_stats,
	.nr_stats	= ARRAY_SIZE(ke_info_stats),
	.all_show	= info_all_show,
};

/* =============================== sched =============================== */

static void sched_freq_show(struct kylin_engine_feature *f,
			    struct seq_file *m, void *data)
{
	unsigned int cpu = (unsigned long)data;
	unsigned int khz = 0;

	if (cpu_online(cpu)) {
#ifdef CONFIG_CPU_FREQ
		khz = cpufreq_quick_get(cpu);
#endif
	}
	/* kHz -> MHz, 0 when offline or no policy */
	seq_printf(m, "%u\n", khz / 1000);
}

static struct kylin_engine_stat *sched_stats;
static unsigned int sched_nr_cpu;

static void sched_all_show(struct kylin_engine_feature *f, struct seq_file *m)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		seq_printf(m, "cpu%u %u\n", cpu,
			   cpu_online(cpu) ?
#ifdef CONFIG_CPU_FREQ
				cpufreq_quick_get(cpu) / 1000 :
#else
				0 :
#endif
				0);
	}
}

static int sched_feature_init(void)
{
	struct kylin_engine_feature *feat;
	struct kylin_engine_stat *st;
	char **names;
	unsigned int cpu, i, n;
	int ret;

	sched_nr_cpu = nr_cpu_ids;
	n = sched_nr_cpu;

	sched_stats = kcalloc(n, sizeof(*sched_stats), GFP_KERNEL);
	names = kcalloc(n, sizeof(*names), GFP_KERNEL);
	feat = kzalloc(sizeof(*feat), GFP_KERNEL);
	if (!sched_stats || !names || !feat) {
		kfree(sched_stats);
		kfree(names);
		kfree(feat);
		return -ENOMEM;
	}

	st = sched_stats;
	for_each_possible_cpu(cpu) {
		names[cpu] = kasprintf(GFP_KERNEL, "cpu%u_freq_mhz", cpu);
		if (!names[cpu]) {
			ret = -ENOMEM;
			goto err_names;
		}
		st[cpu].name = names[cpu];
		st[cpu].show = sched_freq_show;
		st[cpu].data = (void *)(unsigned long)cpu;
	}

	feat->name = "sched";
	feat->stats = sched_stats;
	feat->nr_stats = n;
	feat->all_show = sched_all_show;
	feat->data = names;		/* for future teardown */

	ret = kylin_engine_register_feature(feat);
	if (ret)
		goto err_names;

	return 0;

err_names:
	for (i = 0; i < n; i++)
		kfree(names[i]);
	kfree(sched_stats);
	kfree(names);
	kfree(feat);
	return ret;
}

/* ================================ mem ================================ */

static void mem_slab_reclaim_show(struct kylin_engine_feature *f,
				  struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n", global_node_page_state(NR_SLAB_RECLAIMABLE_B)
				   >> 10);
}

static void mem_slab_unreclaim_show(struct kylin_engine_feature *f,
				    struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n", global_node_page_state(NR_SLAB_UNRECLAIMABLE_B)
				   >> 10);
}

static void mem_pagecache_show(struct kylin_engine_feature *f,
			       struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n", global_node_page_state(NR_FILE_PAGES)
				   << (PAGE_SHIFT - 10));
}

static void mem_shmem_show(struct kylin_engine_feature *f,
			   struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n", global_node_page_state(NR_SHMEM)
				   << (PAGE_SHIFT - 10));
}

static void mem_pagetables_show(struct kylin_engine_feature *f,
				struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n", global_node_page_state(NR_PAGETABLE)
				   << (PAGE_SHIFT - 10));
}

static void mem_swapcache_show(struct kylin_engine_feature *f,
			       struct seq_file *m, void *data)
{
	seq_printf(m, "%lu\n", total_swapcache_pages()
				   << (PAGE_SHIFT - 10));
}

static unsigned int mem_trim_count;

static ssize_t mem_trim_show(struct kylin_engine_feature *f, char *buf)
{
	return sysfs_emit(buf, "%u\n", mem_trim_count);
}

static ssize_t mem_trim_store(struct kylin_engine_feature *f,
			      const char *buf, size_t len)
{
	char cmd[16];
	size_t n = min(len, sizeof(cmd) - 1);

	memcpy(cmd, buf, n);
	cmd[n] = '\0';
	strim(cmd);

	if (sysfs_streq(cmd, "slab")) {
		drop_slab();
	} else if (sysfs_streq(cmd, "compact")) {
		/*
		 * compact_nodes() is static in 5.15; wake kcompactd with a
		 * high order target instead (async proactive compaction).
		 */
#ifdef CONFIG_COMPACTION
		wakeup_kcompactd(NODE_DATA(numa_node_id()), pageblock_order,
				 ZONE_MOVABLE);
#else
		return -EOPNOTSUPP;
#endif
	} else if (sysfs_streq(cmd, "all")) {
		drop_slab();
#ifdef CONFIG_COMPACTION
		wakeup_kcompactd(NODE_DATA(numa_node_id()), pageblock_order,
				 ZONE_MOVABLE);
#endif
	} else {
		return -EINVAL;
	}

	mem_trim_count++;
	return 0;
}

static ssize_t mem_swappiness_show(struct kylin_engine_feature *f, char *buf)
{
	return sysfs_emit(buf, "%d\n", vm_swappiness);
}

static ssize_t mem_swappiness_store(struct kylin_engine_feature *f,
				    const char *buf, size_t len)
{
	char cmd[16];
	size_t n = min(len, sizeof(cmd) - 1);
	int val;

	memcpy(cmd, buf, n);
	cmd[n] = '\0';
	if (kstrtoint(strim(cmd), 0, &val))
		return -EINVAL;
	if (val < 0 || val > 200)
		return -EINVAL;

	vm_swappiness = val;
	return 0;
}

static struct kylin_engine_stat ke_mem_stats[] = {
	{ "slab_reclaim_kb",	mem_slab_reclaim_show },
	{ "slab_unreclaim_kb",	mem_slab_unreclaim_show },
	{ "pagecache_kb",	mem_pagecache_show },
	{ "shmem_kb",		mem_shmem_show },
	{ "pagetables_kb",	mem_pagetables_show },
	{ "swapcache_kb",       mem_swapcache_show },
};

static struct kylin_engine_attr ke_mem_attrs[] = {
	{ "trim",	0644,	mem_trim_show,		mem_trim_store },
	{ "swappiness",	0644,	mem_swappiness_show,	mem_swappiness_store },
};

static void mem_all_show(struct kylin_engine_feature *f, struct seq_file *m)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ke_mem_stats); i++) {
		seq_printf(m, "%s ", ke_mem_stats[i].name);
		ke_mem_stats[i].show(f, m, NULL);
	}
	seq_printf(m, "trims %u\n", mem_trim_count);
	seq_printf(m, "swappiness %d\n", vm_swappiness);
}

static struct kylin_engine_feature ke_mem_feature = {
	.name		= "mem",
	.stats		= ke_mem_stats,
	.nr_stats	= ARRAY_SIZE(ke_mem_stats),
	.attrs		= ke_mem_attrs,
	.nr_attrs	= ARRAY_SIZE(ke_mem_attrs),
	.all_show	= mem_all_show,
};

/* ================================ top ================================ */

#define KE_TOP_N	16

struct ke_top_entry {
	pid_t pid;
	u64 ticks;		/* utime + stime, ns */
	char comm[TASK_COMM_LEN];
};

static void top_tasks_show(struct kylin_engine_feature *f,
			   struct seq_file *m, void *data)
{
	struct ke_top_entry top[KE_TOP_N];
	struct task_struct *g, *p;
	int n = 0, i, j;

	memset(top, 0, sizeof(top));

	rcu_read_lock();
	for_each_process(g) {
		for_each_thread(g, p) {
			u64 ticks;
			struct ke_top_entry e;

			task_lock(p);
			ticks = p->utime + p->stime;
			e.pid = p->pid;
			e.ticks = ticks;
			/* p->alloc_lock is already held here; get_task_comm()
			 * would take it again -> recursive spinlock deadlock */
			memcpy(e.comm, p->comm, TASK_COMM_LEN);
			task_unlock(p);

			if (!e.ticks)
				continue;

			/* insertion into descending top-N */
			for (i = 0; i < n && top[i].ticks >= e.ticks; i++)
				;
			if (i >= KE_TOP_N)
				continue;
			for (j = min(n, KE_TOP_N - 1); j > i; j--)
				top[j] = top[j - 1];
			top[i] = e;
			if (n < KE_TOP_N)
				n++;
		}
	}
	rcu_read_unlock();

	for (i = 0; i < n; i++)
		seq_printf(m, "%d %s %llu\n", top[i].pid, top[i].comm,
			   nsec_to_clock_t(top[i].ticks));
}

static struct kylin_engine_stat ke_top_stats[] = {
	{ "tasks",	top_tasks_show },	/* lines: pid comm ticks(USER_HZ) */
};

static struct kylin_engine_feature ke_top_feature = {
	.name		= "top",
	.stats		= ke_top_stats,
	.nr_stats	= ARRAY_SIZE(ke_top_stats),
};

/* ================================ psi ================================ */

/*
 * group->avg[state][window] is FIXED_1 (1<<11) scaled fraction.
 * Report percent * 1000: 12.5% -> 12500.
 */
static void psi_show_pct(struct kylin_engine_feature *f,
			 struct seq_file *m, void *data)
{
	unsigned long enc = (unsigned long)data;
	unsigned int idx = enc >> 2, win = enc & 3;
	unsigned long v = READ_ONCE(psi_system.avg[idx][win]);

	seq_printf(m, "%lu\n", (v * 100000) >> FSHIFT);
}

#define KE_PSI_NODE(strname, state, win) \
	{ strname, psi_show_pct, (void *)((state) * 4 + (win)) }

static struct kylin_engine_stat ke_psi_stats[] = {
	KE_PSI_NODE("cpu_some10",	PSI_CPU_SOME, 0),
	KE_PSI_NODE("cpu_some60",	PSI_CPU_SOME, 1),
	KE_PSI_NODE("mem_some10",	PSI_MEM_SOME, 0),
	KE_PSI_NODE("mem_some60",	PSI_MEM_SOME, 1),
	KE_PSI_NODE("mem_full10",	PSI_MEM_FULL, 0),
	KE_PSI_NODE("mem_full60",	PSI_MEM_FULL, 1),
	KE_PSI_NODE("io_some10",	PSI_IO_SOME, 0),
	KE_PSI_NODE("io_some60",	PSI_IO_SOME, 1),
	KE_PSI_NODE("io_full10",	PSI_IO_FULL, 0),
	KE_PSI_NODE("io_full60",	PSI_IO_FULL, 1),
};

static void psi_all_show(struct kylin_engine_feature *f, struct seq_file *m)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ke_psi_stats); i++) {
		seq_printf(m, "%s ", ke_psi_stats[i].name);
		ke_psi_stats[i].show(f, m, ke_psi_stats[i].data);
	}
}

static struct kylin_engine_feature ke_psi_feature = {
	.name		= "psi",
	.stats		= ke_psi_stats,
	.nr_stats	= ARRAY_SIZE(ke_psi_stats),
	.all_show	= psi_all_show,
};


/* ============================== register ============================= */

static int __init ke_features_init(void)
{
	int ret;

	ret = kylin_engine_register_feature(&ke_info_feature);
	if (ret)
		pr_warn("info register failed %d\n", ret);

	ret = sched_feature_init();
	if (ret)
		pr_warn("sched register failed %d\n", ret);

	ret = kylin_engine_register_feature(&ke_mem_feature);
	if (ret)
		pr_warn("mem register failed %d\n", ret);

	ret = kylin_engine_register_feature(&ke_top_feature);
	if (ret)
		pr_warn("top register failed %d\n", ret);

	if (IS_ENABLED(CONFIG_PSI)) {
		ret = kylin_engine_register_feature(&ke_psi_feature);
		if (ret)
			pr_warn("psi register failed %d\n", ret);
	}


	return 0;
}
device_initcall(ke_features_init);
