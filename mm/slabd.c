// SPDX-License-Identifier: GPL-2.0
/*
 * kshrink_slabd - CPU topology-aware asynchronous slab reclaim.
 *
 * Reworked from the Xiaomi kshrink_slabd built-in port. Design points:
 *
 *  1. CPU topology-aware dynamic scheduling
 *     - slots are built from cpufreq policies (each policy's related_cpus
 *       forms one cluster); every CPU is mapped to its cluster slot.
 *     - each slot owns a worker affined to the cluster; requests are
 *       routed by the caller's current CPU.
 *  2. Concurrent-safe enqueue
 *     - per-slot spinlock guards a single pending request (order
 *       preserved, no queue unbounded growth).
 *  3. memcg reference lifetime
 *     - non-root memcgs are pinned with css_tryget_online() at enqueue
 *       and released with css_put() after the worker consumes them, and
 *       again at teardown for any leftover pending request.
 *  4. worker init/exit
 *     - workers are kthread_create() + wake_up_process() after the whole
 *       slot set is built; teardown uses kthread_stop().
 *  5. module teardown safety
 *     - exit order: disable -> unregister hook -> synchronize_rcu() ->
 *       stop workers -> release leftover memcg refs -> free slot array.
 *  6. hook registration rollback
 *     - register_trace failure (or worker creation failure) unwinds every
 *       already-created worker and frees all resources.
 *  7. emergency / busy-slot synchronous fallback
 *     - atomic allocations, high-priority reclaim and a busy (pending)
 *       slot all fall back to *bypass = false so the caller reclaims
 *       synchronously; the request is never silently dropped.
 *  8. trace callback unregister sync
 *     - unregister is followed by synchronize_rcu() so no callback can
 *       still be running when workers are stopped.
 *  9. atomic global throttle
 *     - a single atomic_long_t gate admits at most one request per
 *       HZ window across all slots; losers run synchronously.
 */
#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/freezer.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/topology.h>
#include <linux/wait.h>

#include <trace/hooks/vmscan.h>

#include "slabd.h"

#define KSHRINK_SLABD_MAX_SLOTS	8
#define KSHRINK_SLABD_THROTTLE	HZ

struct kshrink_slabd_request {
	gfp_t gfp_mask;
	int nid;
	int priority;
	struct mem_cgroup *memcg;
	bool memcg_pinned;
};

struct kshrink_slabd_slot {
	spinlock_t lock;
	struct kshrink_slabd_request request;
	bool pending;
	struct task_struct *worker;
	cpumask_t cpumask;
	unsigned long queued;
	unsigned long completed;
	int index;
};

static struct kshrink_slabd_slot *kshrink_slabd_slots;
static int kshrink_slabd_nslots;
static DEFINE_PER_CPU(int, kshrink_slabd_slot_idx);
static DECLARE_WAIT_QUEUE_HEAD(kshrink_slabd_wait);
static bool kshrink_slabd_enabled;
static struct proc_dir_entry *kshrink_slabd_proc;

/* Global admission gate (point 9): one accepted request per window. */
static atomic_long_t kshrink_slabd_last_pass = ATOMIC_LONG_INIT(0);

extern unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg, int priority);

static struct kshrink_slabd_slot *kshrink_slabd_current_slot(void)
{
	int idx = __this_cpu_read(kshrink_slabd_slot_idx);

	if (idx >= 0 && idx < READ_ONCE(kshrink_slabd_nslots))
		return &kshrink_slabd_slots[idx];
	return NULL;
}

/*
 * Worker body (points 1, 4). Runs with PF_MEMALLOC | PF_KSWAPD so its own
 * shrink_slab() pass is treated as the kswapd-class synchronous path and is
 * never re-routed back into the async machinery.
 */
static int kshrink_slabd_worker(void *arg)
{
	struct kshrink_slabd_slot *slot = arg;
	struct kshrink_slabd_request request;
	bool pending;

	current->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	if (!cpumask_empty(&slot->cpumask))
		set_cpus_allowed_ptr(current, &slot->cpumask);

	while (!kthread_should_stop()) {
		wait_event_freezable(kshrink_slabd_wait,
			kthread_should_stop() ||
			READ_ONCE(slot->pending));
		if (kthread_should_stop())
			break;

		spin_lock_irq(&slot->lock);
		pending = slot->pending;
		if (pending) {
			request = slot->request;
			slot->pending = false;
		}
		spin_unlock_irq(&slot->lock);

		if (!pending)
			continue;

		shrink_slab(request.gfp_mask, request.nid, request.memcg,
			    request.priority);
		if (request.memcg_pinned)
			css_put(&request.memcg->css);
		slot->completed++;
	}

	current->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	return 0;
}

/*
 * Enqueue onto the caller's cluster slot. The slot is single-slot so the
 * return value tells the hook whether the request was accepted (false =
 * busy, caller must reclaim synchronously).
 */
bool kshrink_slabd_queue(gfp_t gfp_mask, int nid,
			 struct mem_cgroup *memcg, int priority)
{
	struct kshrink_slabd_slot *slot;
	unsigned long flags;
	bool queued = false;

	if (!READ_ONCE(kshrink_slabd_enabled))
		return false;

	slot = kshrink_slabd_current_slot();
	if (!slot)
		return false;

	spin_lock_irqsave(&slot->lock, flags);
	if (!slot->pending) {
		struct mem_cgroup *pinned = memcg;
		bool memcg_pinned = false;

		if (pinned && !mem_cgroup_is_root(pinned)) {
			if (!css_tryget_online(&pinned->css))
				pinned = NULL;
			else
				memcg_pinned = true;
		}

		if (pinned || !memcg) {
			slot->request.gfp_mask = gfp_mask;
			slot->request.nid = nid;
			slot->request.priority = priority;
			slot->request.memcg = pinned;
			slot->request.memcg_pinned = memcg_pinned;
			slot->pending = true;
			slot->queued++;
			queued = true;
		}
	}
	spin_unlock_irqrestore(&slot->lock, flags);

	if (queued)
		wake_up_interruptible(&kshrink_slabd_wait);
	return queued;
}

/*
 * Hook (points 7, 9). kswapd and the workers themselves (both PF_KSWAPD)
 * always take the synchronous path. Everyone else goes through the global
 * atomic throttle; emergency conditions and a busy slot fall back to the
 * synchronous path instead of dropping the request.
 */
static void kshrink_slabd_bypass(void *data, gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg, int priority,
				 bool *bypass)
{
	unsigned long prev, curr;

	if (!READ_ONCE(kshrink_slabd_enabled)) {
		*bypass = false;
		return;
	}

	if (current->flags & PF_KSWAPD) {
		*bypass = false;
		return;
	}

	/* Emergency: atomic context or high-priority reclaim runs now. */
	if (gfp_mask & __GFP_ATOMIC) {
		*bypass = false;
		return;
	}
	if (priority < DEF_PRIORITY - 3) {
		*bypass = false;
		return;
	}

	/* Global throttle: admit one request per HZ window, atomically. */
	prev = (unsigned long)atomic_long_read(&kshrink_slabd_last_pass);
	curr = jiffies;
	if (time_before(curr, prev + KSHRINK_SLABD_THROTTLE)) {
		*bypass = false;
		return;
	}
	if (atomic_long_cmpxchg(&kshrink_slabd_last_pass,
			       (long)prev, (long)curr) != (long)prev) {
		*bypass = false;
		return;
	}

	/* Busy slot: fall back synchronously instead of dropping. */
	if (!kshrink_slabd_queue(gfp_mask, nid, memcg, priority)) {
		*bypass = false;
		return;
	}

	*bypass = true;
}

/*
 * Topology-aware slot construction (point 1). One slot per cpufreq policy
 * (cluster); CPUs without a policy are mapped to -1 and simply reclaim
 * synchronously.
 */
static int kshrink_slabd_build_slots(void)
{
	struct cpufreq_policy *policy;
	int cpu, n = 0, i;

	kshrink_slabd_slots = kcalloc(KSHRINK_SLABD_MAX_SLOTS,
				      sizeof(*kshrink_slabd_slots),
				      GFP_KERNEL);
	if (!kshrink_slabd_slots)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		int idx = -1;

		policy = cpufreq_cpu_get(cpu);
		if (policy) {
			for (i = 0; i < n; i++) {
				if (cpumask_intersects(&kshrink_slabd_slots[i].cpumask,
						       policy->related_cpus)) {
					idx = i;
					break;
				}
			}
			if (idx < 0 && n < KSHRINK_SLABD_MAX_SLOTS) {
				idx = n++;
				kshrink_slabd_slots[idx].index = idx;
				spin_lock_init(&kshrink_slabd_slots[idx].lock);
				cpumask_copy(&kshrink_slabd_slots[idx].cpumask,
					     policy->related_cpus);
			}
			cpufreq_cpu_put(policy);
		}
		per_cpu(kshrink_slabd_slot_idx, cpu) = idx;
	}

	if (!n) {
		/* No cpufreq policies (rare): one slot over all possible CPUs. */
		n = 1;
		kshrink_slabd_slots[0].index = 0;
		spin_lock_init(&kshrink_slabd_slots[0].lock);
		cpumask_copy(&kshrink_slabd_slots[0].cpumask,
			     cpu_possible_mask);
		for_each_possible_cpu(cpu)
			per_cpu(kshrink_slabd_slot_idx, cpu) = 0;
	}

	WRITE_ONCE(kshrink_slabd_nslots, n);
	return 0;
}

static void kshrink_slabd_stop_workers(int first, int count)
{
	int i;

	for (i = first; i < count; i++) {
		struct kshrink_slabd_slot *slot = &kshrink_slabd_slots[i];

		if (!slot->worker)
			continue;
		wake_up_interruptible(&kshrink_slabd_wait);
		kthread_stop(slot->worker);
		slot->worker = NULL;
	}
}

static void kshrink_slabd_release_pending(void)
{
	int i;

	for (i = 0; i < READ_ONCE(kshrink_slabd_nslots); i++) {
		struct kshrink_slabd_slot *slot = &kshrink_slabd_slots[i];

		if (slot->pending && slot->request.memcg_pinned)
			css_put(&slot->request.memcg->css);
		slot->pending = false;
		slot->request.memcg = NULL;
		slot->request.memcg_pinned = false;
	}
}

static int kshrink_slabd_proc_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "enable = %d\nslots = %d\nthrottle_jiffies = %u\n",
		   READ_ONCE(kshrink_slabd_enabled) ? 1 : 0,
		   READ_ONCE(kshrink_slabd_nslots),
		   KSHRINK_SLABD_THROTTLE);
	for (i = 0; i < READ_ONCE(kshrink_slabd_nslots); i++) {
		struct kshrink_slabd_slot *slot = &kshrink_slabd_slots[i];

		seq_printf(m, "slot[%d] cpus=%*pbl queued=%lu completed=%lu pending=%u\n",
			   i, cpumask_pr_args(&slot->cpumask),
			   slot->queued, slot->completed,
			   READ_ONCE(slot->pending) ? 1 : 0);
	}
	return 0;
}

static int __init kshrink_slabd_init(void)
{
	int ret, i;

	ret = kshrink_slabd_build_slots();
	if (ret)
		return ret;

	/* Workers are created only after the full slot set is valid. */
	for (i = 0; i < READ_ONCE(kshrink_slabd_nslots); i++) {
		struct kshrink_slabd_slot *slot = &kshrink_slabd_slots[i];
		struct task_struct *t;

		t = kthread_create(kshrink_slabd_worker, slot,
				   "kshrink_slabd%d", i);
		if (IS_ERR(t)) {
			ret = PTR_ERR(t);
			goto out_free_workers;
		}
		slot->worker = t;
		wake_up_process(t);
	}

	ret = register_trace_android_vh_shrink_slab_bypass(
		kshrink_slabd_bypass, NULL);
	if (ret)
		goto out_free_workers;

	kshrink_slabd_proc = proc_create_single("kshrink_slabd", 0444, NULL,
						kshrink_slabd_proc_show);
	WRITE_ONCE(kshrink_slabd_enabled, true);
	pr_info("kshrink_slabd: %d topology slots\n",
		READ_ONCE(kshrink_slabd_nslots));
	return 0;

out_free_workers:
	kshrink_slabd_stop_workers(0, READ_ONCE(kshrink_slabd_nslots));
	kfree(kshrink_slabd_slots);
	kshrink_slabd_slots = NULL;
	WRITE_ONCE(kshrink_slabd_nslots, 0);
	return ret;
}

/*
 * Teardown (points 5, 8): disable the gate, unregister + RCU-sync the
 * callback, stop every worker, then release leftover memcg references.
 */
static void __exit kshrink_slabd_exit(void)
{
	WRITE_ONCE(kshrink_slabd_enabled, false);

	if (kshrink_slabd_proc) {
		proc_remove(kshrink_slabd_proc);
		kshrink_slabd_proc = NULL;
	}

	unregister_trace_android_vh_shrink_slab_bypass(kshrink_slabd_bypass,
						       NULL);
	synchronize_rcu();

	kshrink_slabd_stop_workers(0, READ_ONCE(kshrink_slabd_nslots));
	kshrink_slabd_release_pending();

	kfree(kshrink_slabd_slots);
	kshrink_slabd_slots = NULL;
	WRITE_ONCE(kshrink_slabd_nslots, 0);
}

late_initcall(kshrink_slabd_init);
module_exit(kshrink_slabd_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CPU topology-aware asynchronous slab reclaim");
MODULE_AUTHOR("KylinKernel");
