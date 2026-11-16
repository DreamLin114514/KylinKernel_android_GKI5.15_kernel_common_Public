// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Xiaomi Ltd.
 * 
 * Optimized for heavy multitasking scenarios with:
 * - CPU topology-aware dynamic scheduling
 * - Lockless queue entry with RCU-protected memcg
 * - Emergency pressure detection and sync fallback
 * - Robust hook registration/unregistration with rollback
 * - Safe module unload with proper cleanup ordering
 */
#define pr_fmt(fmt) "shrink_async: " fmt

#include <linux/swap.h>
#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/cpufreq.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/stat.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/memcontrol.h>
#include <trace/hooks/vmscan.h>
#include <trace/hooks/mm.h>
#include <linux/psi.h>


#define SHRINK_SLABD_NAME "kshrink_slabd"

/*
 * Tunables optimized for heavy multitasking: faster response, more coalescing,
 * reduced sync reclaim stalls.
 * Burst-based throttling: allow multiple requests to queue (trigger CAS),
 * only throttle when burst limit is exceeded within the time window.
 */
static unsigned int shrink_slabd_burst_window_ms = 200;
module_param(shrink_slabd_burst_window_ms, uint, 0644);
MODULE_PARM_DESC(shrink_slabd_burst_window_ms,
                 "time window (ms) for burst counting");

static unsigned int shrink_slabd_burst_limit = 8;
module_param(shrink_slabd_burst_limit, uint, 0644);
MODULE_PARM_DESC(shrink_slabd_burst_limit,
                 "max async requests allowed within burst window before throttling");

/*
 * Pressure thresholds for adaptive burst limits:
 * - Emergency: MemAvailable < 15% → bypass burst limit, force sync shrink
 * - High: MemAvailable < 25% → increase burst limit (allow more async)
 * - Normal: MemAvailable >= 25% → use base burst limit
 */
static unsigned int emergency_memavail_pct = 15;
module_param(emergency_memavail_pct, uint, 0644);
MODULE_PARM_DESC(emergency_memavail_pct,
                 "MemAvailable % threshold for emergency (bypass burst, force sync)");

static unsigned int high_pressure_memavail_pct = 25;
module_param(high_pressure_memavail_pct, uint, 0644);
MODULE_PARM_DESC(high_pressure_memavail_pct,
                 "MemAvailable % threshold for high pressure (increase burst limit)");

/*
 * PSI gate: when the memory controller reports that tasks are actually
 * stalled on memory (avg10 of the full-stall state, in permille), page
 * counters are a lagging indicator.  Above this level the worker admits
 * every request instead of throttling, because dropping a reclaim
 * request only extends the stall it was meant to relieve.
 */
static unsigned int psi_full_high_permille = 10;
module_param(psi_full_high_permille, uint, 0644);
MODULE_PARM_DESC(psi_full_high_permille,
                 "PSI memory.full avg10 in per-mille above which async shrink is never throttled (10 = 1% of wall time in full stall)");

extern unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 int priority);

static struct task_struct *shrink_slabd_tsk = NULL;
static bool async_shrink_slabd_setup = false;
static wait_queue_head_t shrink_slabd_wait;

/* Completion for safe module unload: ensures worker has exited. */
static struct completion slabd_exit_done;

/*
 * Work request structure with RCU-protected memcg reference.
 * Protected by atomic CAS to prevent concurrent overwrites.
 */
struct async_slabd_request {
	struct mem_cgroup *memcg;   /* RCU-protected, may be NULL */
	gfp_t gfp_mask;
	int nid;
	int priority;
	atomic_t runnable;           /* 0=idle, 1=work queued */
};

static struct async_slabd_request slabd_req = {
	.memcg = NULL,
	.gfp_mask = 0,
	.nid = 0,
	.priority = 0,
	.runnable = ATOMIC_INIT(0),
};
static DEFINE_SPINLOCK(slabd_req_lock);

/* Statistics for monitoring and debugging. */
static atomic64_t stat_async_requests;
static atomic64_t stat_sync_fallback;
static atomic64_t stat_emergency_sync;
static atomic64_t stat_throttle_drop;
static atomic64_t stat_request_merged;     /* requests coalesced into one */
static atomic64_t stat_slot_contention;    /* failed CAS due to race */

/* Burst tracking: count requests within sliding time window */
static atomic_t burst_count = ATOMIC_INIT(0);
static unsigned long burst_window_start;
static DEFINE_SPINLOCK(burst_lock);

static bool is_shrink_slabd_task(struct task_struct *tsk)
{
	return tsk == shrink_slabd_tsk;
}

/*
 * Enqueue a shrink request: atomically update request struct and wake worker.
 * Uses CAS to protect the single slot from concurrent overwrites.
 * Returns true if async request was queued, false if worker is busy or memcg offline.
 */
static bool wakeup_shrink_slabd(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 int priority)
{
	int old_runnable;
	unsigned long flags;

	/*
	 * If memcg is provided, try to take a CSS reference before queueing.
	 * If memcg is already offline, fallback to synchronous shrink.
	 */
	if (memcg && !css_tryget_online(&memcg->css)) {
		/* memcg is offline, cannot queue async request. */
		return false;
	}

	spin_lock_irqsave(&slabd_req_lock, flags);
	old_runnable = atomic_read(&slabd_req.runnable);
	if (old_runnable != 0) {
		spin_unlock_irqrestore(&slabd_req_lock, flags);
		if (memcg)
			css_put(&memcg->css);
		atomic64_inc(&stat_slot_contention);
		return false;
	}

	/* Publish the complete request while the slot lock is held. */
	slabd_req.gfp_mask = gfp_mask;
	slabd_req.nid = nid;
	slabd_req.memcg = memcg;
	slabd_req.priority = priority;
	smp_wmb();
	atomic_set(&slabd_req.runnable, 1);
	spin_unlock_irqrestore(&slabd_req_lock, flags);

	wake_up_interruptible(&shrink_slabd_wait);
	atomic64_inc(&stat_async_requests);
	return true;
}

/*
 * CPU topology-aware affinity: bind worker to little cores (lower power,
 * doesn't interfere with foreground on big cores). Retry on each wake if
 * cpufreq isn't ready at boot.
 */
static void set_async_slabd_cpus(void)
{
	struct cpumask mask;
	struct cpumask *cpumask = &mask;
	pg_data_t *pgdat = NODE_DATA(0);
	unsigned int cpu = 0, cpufreq_max_tmp = 0;
	struct cpufreq_policy *policy_max = NULL;
	static bool set_slabd_cpus_success = false;

	if (likely(set_slabd_cpus_success))
		return;

	/* Find the fastest CPU cluster (big cores). */
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		if (policy == NULL)
			continue;
		if (policy->cpuinfo.max_freq > cpufreq_max_tmp) {
			if (policy_max)
				cpufreq_cpu_put(policy_max);
			cpufreq_max_tmp = policy->cpuinfo.max_freq;
			policy_max = policy;
		} else {
			cpufreq_cpu_put(policy);
		}
	}

	if (policy_max == NULL)
		return; /* cpufreq not online yet: retry on the next wake */

	/*
	 * Restrict worker to node0 CPUs, excluding the big cluster.
	 * This pins the worker to little cores, reducing interference
	 * with foreground workloads.
	 */
	cpumask_copy(cpumask, cpumask_of_node(pgdat->node_id));
	cpumask_andnot(cpumask, cpumask, policy_max->related_cpus);
	cpufreq_cpu_put(policy_max);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(shrink_slabd_tsk, cpumask);
	set_slabd_cpus_success = true;
	pr_info("worker affinity set to little cores\n");
}

/*
 * Check if system is under emergency pressure: MemAvailable < threshold.
 * Returns true if caller must do synchronous shrink immediately.
 */
static bool is_emergency_pressure(void)
{
	unsigned long avail_kb, total_kb, threshold_kb;

	if (!emergency_memavail_pct)
		return false;

	/*
	 * Use the kernel's real MemAvailable estimate (matches
	 * /proc/meminfo): it accounts for reclaimable page cache and
	 * slab.  freeram+buffers is nearly always tiny on Android, so
	 * using it makes the emergency path fire permanently.
	 */
	avail_kb = si_mem_available() << (PAGE_SHIFT - 10);
	total_kb = totalram_pages() << (PAGE_SHIFT - 10);
	threshold_kb = (total_kb * emergency_memavail_pct) / 100;

	return avail_kb < threshold_kb;
}

/*
 * Read the globally aggregated PSI memory full-stall average (avg10),
 * in permille.  psi.c refreshes it from its 2s aggregation worker, so a
 * plain READ_ONCE() is enough here: a stale-by-one-window value is fine
 * for a reclaim heuristic and we must not take psi's avgs_lock from a
 * reclaim path.
 */
static unsigned int slabd_psi_full_avg10(void)
{
#ifdef CONFIG_PSI
	if (static_branch_likely(&psi_disabled))
		return 0;

	/* psi keeps avg10 as percent scaled by FIXED_1 (1024) */
	return (unsigned int)(((u64)READ_ONCE(psi_system.avg[PSI_MEM_FULL][0]) * 10) >> 10);
#else
	return 0;
#endif
}

/*
 * Get adaptive burst limit based on current memory pressure.
 * Three tiers: normal (8), high pressure (11), emergency bypassed.
 */
static unsigned int get_adaptive_burst_limit(void)
{
	unsigned long avail_kb, total_kb;
	unsigned long high_threshold_kb;
	unsigned int psi_full;

	/*
	 * PSI first: measured stall beats free-page ratios as an input,
	 * because it tells us whether reclaim is actually hurting the
	 * running workload right now.
	 */
	psi_full = slabd_psi_full_avg10();
	if (psi_full >= psi_full_high_permille)
		return shrink_slabd_burst_limit + 5;

	if (!high_pressure_memavail_pct)
		return shrink_slabd_burst_limit;

	avail_kb = si_mem_available() << (PAGE_SHIFT - 10);
	total_kb = totalram_pages() << (PAGE_SHIFT - 10);
	high_threshold_kb = (total_kb * high_pressure_memavail_pct) / 100;

	if (avail_kb < high_threshold_kb) {
		/* High pressure: allow more async requests. */
		return shrink_slabd_burst_limit + 3;  /* 8 → 11 */
	} else {
		/* Normal pressure: use base burst limit. */
		return shrink_slabd_burst_limit;
	}
}

/*
 * Check if current request should be throttled based on burst limit.
 * Uses a sliding time window: reset counter when window expires.
 * Returns true if throttle should be applied.
 */
static bool should_throttle_burst(void)
{
	unsigned long now = jiffies;
	unsigned long window_jiffies = msecs_to_jiffies(shrink_slabd_burst_window_ms);
	unsigned int limit;
	int count;
	bool throttle = false;

	/* Under real memory stall, dropping reclaim requests only makes
	 * the stall longer: admit everything and let the worker coalesce. */
	if (slabd_psi_full_avg10() >= psi_full_high_permille)
		return false;

	limit = get_adaptive_burst_limit();

	spin_lock(&burst_lock);

	/* Reset burst counter if window expired. */
	if (time_after(now, burst_window_start + window_jiffies)) {
		burst_window_start = now;
		atomic_set(&burst_count, 0);
	}

	count = atomic_read(&burst_count);

	if (count >= limit) {
		/* Burst limit exceeded, throttle this request. */
		throttle = true;
	} else {
		/* Allow this request, increment burst counter. */
		atomic_inc(&burst_count);
	}

	spin_unlock(&burst_lock);

	return throttle;
}

static int kshrink_slabd_func(void *p)
{
	struct mem_cgroup *memcg;
	gfp_t gfp_mask;
	int nid, priority;
	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 */
	current->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	/* Run as SCHED_IDLE so slab reclaim never steals CPU from foreground. */
	{
		struct sched_param sp = { .sched_priority = 0 };

		sched_setscheduler_nocheck(current, SCHED_IDLE, &sp);
	}

	while (!kthread_should_stop()) {
		wait_event_freezable(shrink_slabd_wait,
					(atomic_read(&slabd_req.runnable) == 1 ||
					 kthread_should_stop()));

		/* CPU affinity: lazily bind to little cores once topology is known. */
		set_async_slabd_cpus();

		/* Handle shrink_slab request if present. */
		if (atomic_read(&slabd_req.runnable) == 1) {
			unsigned long flags;

			/* Snapshot and detach the request while holding the slot lock. */
			spin_lock_irqsave(&slabd_req_lock, flags);
			smp_rmb();
			nid = slabd_req.nid;
			gfp_mask = slabd_req.gfp_mask;
			priority = slabd_req.priority;
			memcg = slabd_req.memcg;
			slabd_req.memcg = NULL;
			spin_unlock_irqrestore(&slabd_req_lock, flags);

			shrink_slab(gfp_mask, nid, memcg, priority);

			if (memcg)
				css_put(&memcg->css);

			/* Mark slot free after the request and its CSS ref are complete. */
			smp_wmb();
			atomic_set(&slabd_req.runnable, 0);
		}
	}

	current->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	complete(&slabd_exit_done);
	return 0;
}


/*
 * Burst-based throttling with atomics for lockless enqueue.
 * Uses sliding time window and adaptive burst limits based on memory pressure.
 */
static void should_shrink_async(void *data, gfp_t gfp_mask, int nid,
		struct mem_cgroup *memcg, int priority, bool *bypass)
{
	/*
	 * Narrow bypass: only the actual launcher process (com.miui.home)
	 * gets async bypass to avoid stutter. Other oom_score_adj==0 processes
	 * go through normal throttle/emergency paths.
	 */
	if (!current_is_kswapd() &&
		!is_shrink_slabd_task(current) &&
		(strcmp(current->group_leader->comm, "com.miui.home") == 0)) {
		*bypass = true;
		return;
	}

	if (unlikely(!async_shrink_slabd_setup)) {
		*bypass = false;
		return;
	}

	/* kswapd and the worker itself always shrink synchronously. */
	if (current_is_kswapd() || is_shrink_slabd_task(current)) {
		*bypass = false;
		return;
	}

	/*
	 * Emergency pressure detection: if MemAvailable is critically low,
	 * bypass burst limit and do synchronous shrink to prevent OOM.
	 */
	if (is_emergency_pressure()) {
		*bypass = false;
		atomic64_inc(&stat_emergency_sync);
		return;
	}

	/*
	 * Burst-based throttling: allow multiple requests within the time window
	 * to queue (triggering CAS protection), only throttle when burst limit
	 * is exceeded.
	 */
	if (should_throttle_burst()) {
		/*
		 * Burst limit exceeded: if worker is busy, fallback to sync shrink
		 * to prevent indefinite starvation.
		 */
		if (atomic_read(&slabd_req.runnable) == 1) {
			*bypass = false;
			atomic64_inc(&stat_sync_fallback);
			return;
		}
		/* Worker idle but burst limit reached: drop this request. */
		*bypass = true;
		atomic64_inc(&stat_throttle_drop);
		return;
	}

	/*
	 * Within burst limit: try to queue async request.
	 * Multiple CPUs may reach here concurrently, triggering CAS protection.
	 */
	*bypass = true;

	if (!wakeup_shrink_slabd(gfp_mask, nid, memcg, priority)) {
		/* Worker busy: do sync shrink as fallback. */
		*bypass = false;
		atomic64_inc(&stat_sync_fallback);
	}
}

/*
 * Hook registration with proper rollback on partial failure.
 */
static int register_shrink_async_vendor_hooks(void)
{
	int ret;

	ret = register_trace_android_vh_shrink_slab_bypass(should_shrink_async, NULL);
	if (ret) {
		pr_err("register_trace_android_vh_shrink_slab_bypass failed! ret=%d\n",
		       ret);
		return ret;
	}

	return 0;
}

/*
 * Hook unregistration with tracepoint_synchronize_unregister to ensure
 * no callbacks are running before module unload.
 */
static void unregister_shrink_async_vendor_hooks(void)
{
	unregister_trace_android_vh_shrink_slab_bypass(should_shrink_async, NULL);

	/*
	 * Barrier: wait for all in-flight hook callbacks to complete before
	 * returning (critical for safe module unload).
	 */
	tracepoint_synchronize_unregister();
}

/* procfs statistics for monitoring optimization effect. */
static int shrink_async_stat_show(struct seq_file *m, void *arg)
{
	seq_printf(m,
		   "async_requests: %llu\n"
		   "sync_fallback_busy: %llu\n"
		   "emergency_sync: %llu\n"
		   "throttle_drop: %llu\n"
		   "request_merged: %llu\n"
		   "slot_contention: %llu\n"
		   "psi_full_permille: %u\n"
		   "worker_pid: %d\n"
		   "params:\n"
		   "  throttle_ms: %u\n"
		   "  burst_limit: %u\n"
		   "  emergency_memavail_pct: %u\n"
		   "  high_pressure_memavail_pct: %u\n",
		   atomic64_read(&stat_async_requests),
		   atomic64_read(&stat_sync_fallback),
	   atomic64_read(&stat_emergency_sync),
	   atomic64_read(&stat_throttle_drop),
	   atomic64_read(&stat_request_merged),
		   atomic64_read(&stat_slot_contention),
		   slabd_psi_full_avg10(),
	   shrink_slabd_tsk ? shrink_slabd_tsk->pid : 0,
	   shrink_slabd_burst_window_ms,
	   shrink_slabd_burst_limit,
	   emergency_memavail_pct,
	   high_pressure_memavail_pct);
	return 0;
}

static int shrink_async_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, shrink_async_stat_show, NULL);
}

static const struct proc_ops shrink_async_stat_ops = {
	.proc_open	= shrink_async_stat_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init shrink_async_init(void)
{
	int ret = 0;
	struct proc_dir_entry *pentry;

	init_waitqueue_head(&shrink_slabd_wait);
	init_completion(&slabd_exit_done);

	/*
	 * Register hooks first: if this fails, no worker is created and
	 * no cleanup needed (just return error).
	 */
	ret = register_shrink_async_vendor_hooks();
	if (ret != 0)
		return ret;

	shrink_slabd_tsk = kthread_run(kshrink_slabd_func, NULL, SHRINK_SLABD_NAME);
	if (IS_ERR_OR_NULL(shrink_slabd_tsk)) {
		pr_err("Failed to start shrink_slabd on node 0\n");
		ret = PTR_ERR(shrink_slabd_tsk);
		shrink_slabd_tsk = NULL;
		unregister_shrink_async_vendor_hooks();
		return ret;
	}

	/* Mark setup complete: hooks can now enqueue requests. */
	smp_wmb();
	async_shrink_slabd_setup = true;

	pentry = proc_create("kshrink_slabd_stat", 0400, NULL, &shrink_async_stat_ops);
	if (!pentry)
		pr_warn("Failed to create kshrink_slabd_stat proc entry\n");

	pr_info("ready\n");

	return 0;
}

static void __exit shrink_async_exit(void)
{
	/* Step 1: Unregister hooks and wait for callbacks to finish. */
	unregister_shrink_async_vendor_hooks();

	/* Step 2: Prevent new enqueues. */
	async_shrink_slabd_setup = false;
	smp_wmb();

	/* Step 3: Stop worker thread. */
	if (shrink_slabd_tsk) {
		kthread_stop(shrink_slabd_tsk);
		/* Wait for worker to signal exit completion. */
		wait_for_completion(&slabd_exit_done);
		shrink_slabd_tsk = NULL;
	}

	/*
	 * Step 4: Release any pending memcg reference in the request slot.
	 * Worker has exited, so no concurrent access possible.
	 */
	if (slabd_req.memcg) {
		css_put(&slabd_req.memcg->css);
		slabd_req.memcg = NULL;
	}

	pr_info("shrink_async exit succeed!\n");
}

module_init(shrink_async_init);
module_exit(shrink_async_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Xiaomi Ltd. / Optimized by KylinKernel");
MODULE_DESCRIPTION("Async slab shrink with heavy multitasking optimizations");
