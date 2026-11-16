/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024-2024, X-Ring technologies Inc., All rights reserved.
 *
 * Ported to KylinKernel 5.15 from xring dijun-v-oss xring-modules.
 * 5.15 adaptations (keep original names otherwise):
 *   - this tree's task_struct layout is KMI-frozen for prebuilt vendor
 *     modules (CONFIG_MODVERSIONS=y), so the per-task vip state CANNOT
 *     be embedded in task_struct (the xring android_vendor_data1 trick
 *     is unavailable anyway). It lives in an xarray keyed by the task
 *     pointer instead; a node exists iff prio != 0. Stale nodes of
 *     dead tasks are dropped from the fork hook when the slab reuses
 *     the address (see vip_sched.c).
 */
#ifndef _VIP_SCHED_H
#define _VIP_SCHED_H

#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/rcupdate.h>
#include <linux/smp.h>
#include <linux/xarray.h>

#define	VIP_BALANCE_KICK	(1)

enum vip_preempt_type {
	VIP_PREEMPT_NONE = 0,
	/* Normal task was preempted by vip task */
	VIP_PREEMPT_OTHER,
	/* Low prio vip task was preempted by
	 * high prio vip task, or vip task was
	 * preempted by rt/dl task
	 */
	VIP_PREEMPT_VIP,
};

struct vip_task {
	int prio;
	int orig_prio;
	struct task_struct *p;
	struct list_head entry;
	struct rcu_head rcu;
};

struct vb_env {
	int src_cpu, dst_cpu;
	struct rq *src_rq, *dst_rq;
	struct vip_rq *src_vrq, *dst_vrq;
	struct task_struct *task;
};

struct vip_rq {
	int cpu;
	int h_prio; /* highest prio */
	int nr_running;
	bool sync_waiting;
	enum vip_preempt_type vip_preempt;
	struct list_head vip_list;
	/* vip balance */
	bool doing_balance;
	call_single_data_t csd;
	atomic_t kick_flags;
};

extern int vip_sched_init(void);
extern void vip_init_rq(int cpu);
extern void vip_sched_enqueue_task_fair(void *data, struct rq *rq,
		struct task_struct *p, int flags);
extern void vip_sched_dequeue_task_fair(void *data, struct rq *rq,
		struct task_struct *p, int flags);
extern void vip_sched_select_task_rq_fair(void *data, struct task_struct *p,
		int prev_cpu, int sd_flag, int wake_flags, int *target_cpu);
extern void vip_sched_replace_next_task_fair(void *data, struct rq *rq,
		struct task_struct **p, struct sched_entity **se,
		bool *repick, bool simple, struct task_struct *prev);
extern void vip_sched_check_preempt_wakeup(void *data, struct rq *rq,
		struct task_struct *p, bool *preempt, bool *nopreempt,
		int wake_flags, struct sched_entity *se, struct sched_entity *pse,
		int next_buddy_marked, unsigned int granularity);
extern void vip_sched_schedule(void *data, struct task_struct *prev,
		struct task_struct *next, struct rq *rq);
extern void vip_sched_scheduler_tick(void *data, struct rq *rq);
extern int vip_sched_newidle_balance(struct rq *rq);
extern bool is_vip_task(struct task_struct *p);
extern int set_vip_prio(struct task_struct *p, int prio, bool set_orig);
extern int set_vip_prio_locked(struct task_struct *p, int prio, bool set_orig);
extern void set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);

extern struct xarray vip_nodes;

/* Returns the vip node of @p, or NULL when @p is not a vip task */
static inline struct vip_task *task_to_vip(struct task_struct *p)
{
	return xa_load(&vip_nodes, (unsigned long)p);
}

static inline struct task_struct *vip_to_task(struct vip_task *vip)
{
	return vip->p;
}

/*
 * NULL-safe vip prio lookup. The node can be concurrently erased and
 * kfree_rcu()'d by set_vip_prio_locked() / the fork hook, so protect
 * the dereference with an RCU read-side critical section.
 */
static inline int vip_prio(struct task_struct *p)
{
	struct vip_task *vip;
	int prio;

	rcu_read_lock();
	vip = task_to_vip(p);
	prio = vip ? vip->prio : 0;
	rcu_read_unlock();

	return prio;
}

static inline struct task_struct *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_vpid(pid) : current;
}

/*
 * 5.15 port shims: this tree has neither WALT halt state nor RTG.
 * - no task is ever "halted";
 * - no RTG prefer cpus (empty mask -> find_vip_cpu() falls back to
 *   fits/idle/relaxed selection);
 * - RTG prefer util is mapped to the task's uclamp.min.
 */
static inline bool cpu_halted(int cpu)
{
	return false;
}

static inline struct cpumask *cpu_halt_mask(void)
{
	return (struct cpumask *)cpu_none_mask;
}

static inline void xr_get_group_prefer_cpus(struct task_struct *p,
		struct cpumask *mask)
{
	cpumask_clear(mask);
}

static inline unsigned long xr_get_group_prefer_util(struct task_struct *p)
{
#ifdef CONFIG_UCLAMP_TASK
	return uclamp_eff_value(p, UCLAMP_MIN);
#else
	return 0;
#endif
}

#endif /* _VIP_SCHED_H */
