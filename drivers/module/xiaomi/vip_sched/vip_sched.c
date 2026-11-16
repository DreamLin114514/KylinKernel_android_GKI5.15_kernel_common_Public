// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2024, X-Ring technologies Inc., All rights reserved.
 *
 * VIP scheduler ported to KylinKernel 5.15 from xring dijun-v-oss
 * xring-modules/drivers/cpu_sched/walt/vip_sched.c.
 *
 * 5.15 adaptations (all original function/struct names preserved):
 *   - per-task vip state lives in the vip_nodes xarray because the
 *     task_struct layout is KMI-frozen (CONFIG_MODVERSIONS=y with
 *     prebuilt vendor modules); a node exists iff prio != 0, stale
 *     nodes are dropped from the fork hook on slab address reuse;
 *   - private scheduler header included directly (built-in only);
 *   - cpu_util()/task_util() are PELT versions replicated from
 *     kernel/sched/fair.c (they are static there);
 *   - xring walt helpers (find_idle_cpu, freq domain cpumask,
 *     is_max_possible_cluster_cpu) are re-implemented locally;
 *   - set_vip_prio user interface is exposed through /proc/vip_sched
 *     (xring used the perf_actuator ioctl; the binary vip_io_cfg ABI
 *     is kept intact on the proc write path);
 *   - vip_sched_newidle_balance() is kept but NOT registered: in 5.15
 *     the android_rvh_sched_newidle_balance hook fires with this_rq
 *     locked, vip_balance_commit() would self-deadlock on dst_rq.
 *     VIP pulls are driven by the schedule()/tick kick paths instead.
 */
#include <linux/module.h>
#include <linux/err.h>
#include <linux/trace.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/cpuhotplug.h>
#include <linux/smp.h>
#include <trace/hooks/sched.h>
#include "../../../../kernel/sched/sched.h"
#include "vip_sched.h"
#define CREATE_TRACE_POINTS
#include "trace_vip_sched.h"

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

DEFINE_PER_CPU(struct vip_rq, vip_rq);

/*
 * 5.15 port: per-task vip state is externalized into this xarray
 * (keyed by the task_struct pointer) because the task_struct layout
 * is KMI-frozen for prebuilt vendor modules. A node exists iff
 * prio != 0; stale nodes of dead tasks are dropped from the fork
 * hook when the slab reuses the address.
 */
DEFINE_XARRAY(vip_nodes);

struct vip_stats {
	atomic_t nr_running;
};

static struct vip_stats sys_vip_stats;
static unsigned long max_possible_capacity;

extern void set_task_cpu(struct task_struct *p, unsigned int new_cpu);

/***********************************************
 * VIP Sched helper methods
 */
static inline bool is_fair_task(struct task_struct *p)
{
	return p->prio >= MAX_RT_PRIO && !is_idle_task(p);
}

static inline bool is_dl_rt_task(struct task_struct *p)
{
	return p->prio < MAX_RT_PRIO;
}

bool is_vip_task(struct task_struct *p)
{
	return !!(vip_prio(p));
}

static inline int has_vip_running(void)
{
	return atomic_read(&sys_vip_stats.nr_running);
}

/*
 * 5.15 port: PELT cpu_util()/task_util(), replicated from
 * kernel/sched/fair.c where they are static.
 */
static inline unsigned long task_util(struct task_struct *p)
{
	return READ_ONCE(p->se.avg.util_avg);
}

static inline unsigned long cpu_util(int cpu)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg);

	if (sched_feat(UTIL_EST))
		util = max(util, READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return min(util, capacity_orig_of(cpu));
}

/*
 * cpu_util_without: compute cpu utilization without any contributions from *p
 * @cpu: the CPU which utilization is requested
 * @p: the task which utilization should be discounted
 *
 * The utilization of a CPU is defined by the utilization of tasks currently
 * enqueued on that CPU as well as tasks which are currently sleeping after an
 * execution on that CPU.
 *
 * This method returns the utilization of the specified CPU by discounting the
 * utilization of the specified task, whenever the task is currently
 * contributing to the CPU utilization.
 */
static unsigned long __maybe_unused cpu_util_without(int cpu, struct task_struct *p)
{
	unsigned int util;

	/*
	 * WALT does not decay idle tasks in the same manner
	 * as PELT, so it makes little sense to subtract task
	 * utilization from cpu utilization. Instead just use
	 * cpu_util for this case.
	 */
	if (likely(READ_ONCE(p->__state) == TASK_WAKING))
		return cpu_util(cpu);

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_util(cpu);

	util = max_t(long, cpu_util(cpu) - task_util(p), 0);

	/*
	 * Utilization (estimated) can exceed the CPU capacity, thus let's
	 * clamp to the maximum CPU capacity to ensure consistency with the
	 * cpu_util call.
	 */
	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

/* 5.15 port: local replacement of the xring walt helper */
static int find_idle_cpu(const struct cpumask *mask)
{
	int cpu;

	for_each_cpu(cpu, mask) {
		if (available_idle_cpu(cpu))
			return cpu;
	}

	return -1;
}

/* 5.15 port: max capacity cpu of the platform */
static bool is_max_possible_cluster_cpu(int cpu)
{
	return capacity_orig_of(cpu) == max_possible_capacity;
}

/* 5.15 port: cpufreq related_cpus == xring walt freq domain cpumask */
static const struct cpumask *vip_freq_domain_cpumask(int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	if (policy)
		return policy->related_cpus;

	return cpu_online_mask;
}

static int vip_rq_highest_prio(struct vip_rq *vrq)
{
	struct vip_task *vip;

	/* The highest prio in an empty vip_list is 0 */
	if (list_empty(&vrq->vip_list))
		return 0;

	/* Get the fist entry in rq's vip_list */
	vip = list_first_entry(&vrq->vip_list, struct vip_task, entry);

	return vip->prio;
}

static bool optimistic_wait(struct task_struct *next)
{
	/* If the preemptor is a small task (util < 25%), no need to migrate
	 * the preempted vip task.
	 */
	if (task_util(next) < (SCHED_CAPACITY_SCALE >> 2))
		return true;

	return false;
}

static enum vip_preempt_type vip_preempt_classify(struct task_struct *prev,
		struct task_struct *next, struct rq *rq)
{
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu_of(rq));
	int p_prio = vip_prio(prev);
	int n_prio = vip_prio(next);

	if (prev == next || !vrq->nr_running)
		return VIP_PREEMPT_NONE;

	if (!n_prio && !is_dl_rt_task(next))
		return VIP_PREEMPT_NONE;

	if (prev->on_rq && p_prio &&
		!optimistic_wait(next))
		return VIP_PREEMPT_VIP;

	if (prev->on_rq)
		return VIP_PREEMPT_OTHER;

	return vrq->vip_preempt;
}

/***********************************************
 * VIP Sched task list manipulation methods
 */
static void vip_detach_task(struct task_struct *p, struct rq *src_rq,
							struct rq *dst_rq)
{
	deactivate_task(src_rq, p, 0);
	set_task_cpu(p, cpu_of(dst_rq));
}

static void vip_attach_task(struct task_struct *p, struct rq *rq)
{
	WARN_ON(task_rq(p) != rq);

	activate_task(rq, p, 0);
	check_preempt_curr(rq, p, 0);
}

static void find_relaxed_target(struct cpumask *target,
		struct task_struct *p)
{
	struct cpumask tmp;
	struct vip_rq *vrq;
	int i, l_prio = INT_MAX;
	int p_prio = vip_prio(p);

	if (!p_prio || cpumask_empty(target))
		return;

	cpumask_clear(&tmp);
	for_each_cpu(i, target) {
		vrq = &per_cpu(vip_rq, i);
		if (vrq->h_prio > p_prio)
			continue;
		if (vrq->h_prio < l_prio) {
			l_prio = vrq->h_prio;
			cpumask_clear(&tmp);
		}
		cpumask_set_cpu(i, &tmp);
	}

	cpumask_copy(target, &tmp);
}

static void find_fits_target(struct cpumask *target,
		struct task_struct *p)
{
	struct cpumask tmp;
	int i;

	if (cpumask_empty(target))
		return;

	cpumask_clear(&tmp);
	for_each_cpu(i, target) {
		if (arch_scale_cpu_capacity(i) <
			xr_get_group_prefer_util(p))
			continue;
		cpumask_set_cpu(i, &tmp);
	}

	cpumask_copy(target, &tmp);
}

static int find_vip_cpu(struct task_struct *p)
{
	int target_cpu = -1;
	struct cpumask target_mask, rtg_mask, trace_mask;
	bool repick = false;

	if (!p) {
		WARN_ONCE(1, "task is NULL\n");
		return -1;
	}

again:
	/* The candidate should be task's allowed cpu */
	cpumask_and(&target_mask, p->cpus_ptr, cpu_online_mask);

	/* The candidate should be unhalted cpus */
	cpumask_andnot(&target_mask, &target_mask, cpu_halt_mask());

	if (repick) {
		/* The candidate should be task fits cpu */
		find_fits_target(&target_mask, p);
		goto pick_idle;
	}

	/* RTG prefer cpus would be a better choice */
	xr_get_group_prefer_cpus(p, &rtg_mask);
	if (trace_find_vip_cpu_enabled())
		cpumask_copy(&trace_mask, &rtg_mask);

	cpumask_and(&rtg_mask, &target_mask, &rtg_mask);
	if (cpumask_empty(&rtg_mask)) {
		repick = true;
		goto again;
	}

	cpumask_copy(&target_mask, &rtg_mask);

pick_idle:
	/* Idle cpu would be a prefer choice */
	target_cpu = find_idle_cpu(&target_mask);
	if (target_cpu >= 0)
		goto done;

	/* The candidate rq should have a lower vip prio */
	find_relaxed_target(&target_mask, p);
	if (cpumask_empty(&target_mask) && !repick) {
		repick = true;
		goto again;
	}

	/*
	 * No idle cpu in target mask, should pick relaxed
	 * cpu instead.
	 */
	if (!cpumask_empty(&target_mask))
		target_cpu = cpumask_any_distribute(&target_mask);

done:
	trace_find_vip_cpu(p->pid, &target_mask, target_cpu, p->cpus_ptr,
			cpu_halt_mask(), &trace_mask, repick);

	return target_cpu;
}

static int find_emerg_cpu(void)
{
	struct vip_rq *vrq;
	int cpu, emerg = -1;
	int trace_preempt, trace_nr_running;
	unsigned long flags;

	for_each_online_cpu(cpu) {
		raw_spin_lock_irqsave(&cpu_rq(cpu)->__lock, flags);
		vrq = &per_cpu(vip_rq, cpu);
		/* We should help the preempted vip task firstly */
		if (vrq->vip_preempt == VIP_PREEMPT_VIP) {
			trace_find_emerg_cpu(cpu, vrq->vip_preempt, vrq->nr_running);
			raw_spin_unlock_irqrestore(&cpu_rq(cpu)->__lock, flags);
			return cpu;
		}

		if (vrq->nr_running <= 1)
			goto next;

		emerg = cpu;
		if (trace_find_emerg_cpu_enabled()) {
			trace_preempt = vrq->vip_preempt;
			trace_nr_running = vrq->nr_running;
		}
next:
		raw_spin_unlock_irqrestore(&cpu_rq(cpu)->__lock, flags);
	}

	trace_find_emerg_cpu(emerg, trace_preempt, trace_nr_running);

	return emerg;
}

enum {
	MIGRATE_OK,
	MIGRATE_NOT_ALLOW,
	MIGRATE_RUNNING,
	MIGRATE_H_PRIO,
	MIGRATE_L_CAP,
	MIGRATE_RTG,
};

static bool can_migrate_vip(struct task_struct *p, struct vip_task *vip,
							struct vb_env *env)
{
	int cpu = -1;
	int migrate = true;
	int reason = MIGRATE_OK;
	struct cpumask rtg_mask;

	if (!p || !vip || !env)
		return 0;

	cpu = env->dst_cpu;
	/* Can't migrate task to a not allowed cpu */
	if (!cpumask_test_cpu(cpu, p->cpus_ptr)) {
		reason = MIGRATE_NOT_ALLOW;
		goto done;
	}

	/* Can't migrate a running task */
	if (p->on_cpu == 1) {
		reason = MIGRATE_RUNNING;
		goto done;
	}

	/* No need to migrate a lower prio vip to this cpu */
	if (vip->prio <= env->dst_vrq->h_prio) {
		reason = MIGRATE_H_PRIO;
		goto done;
	}

	if (capacity_orig_of(cpu) < task_util(p)) {
		reason = MIGRATE_L_CAP;
		goto done;
	}

	xr_get_group_prefer_cpus(p, &rtg_mask);
	if (!cpumask_test_cpu(cpu, &rtg_mask) &&
			env->src_vrq->vip_preempt != VIP_PREEMPT_VIP) {
		reason = MIGRATE_RTG;
		goto done;
	}

done:
	if (reason != MIGRATE_OK)
		migrate = false;

	trace_can_migrate_vip(cpu, p->pid, vip->prio, migrate, reason);

	return migrate;
}

static int vip_balance(int this_cpu, struct rq *this_rq)
{
	struct task_struct *p;
	struct vip_task *tmp;
	unsigned long flags;
	struct vb_env env = {
		.dst_rq = this_rq,
		.dst_cpu = this_cpu,
		.dst_vrq = &per_cpu(vip_rq, this_cpu),
		.task = NULL,
	};

	env.src_cpu = find_emerg_cpu();
	if (env.src_cpu < 0 || env.src_cpu == this_cpu)
		return 0;

	env.src_rq = cpu_rq(env.src_cpu);

	raw_spin_lock_irqsave(&env.src_rq->__lock, flags);
	env.src_vrq = &per_cpu(vip_rq, env.src_cpu);
	/* Migate vip task from low to high priority */
	list_for_each_entry_reverse(tmp, &env.src_vrq->vip_list,
						entry) {
		p = vip_to_task(tmp);
		if (can_migrate_vip(p, tmp, &env)) {
			env.task = p;
			break;
		}
	}

	if (env.task)
		vip_detach_task(env.task, env.src_rq, env.dst_rq);

	raw_spin_unlock_irqrestore(&env.src_rq->__lock, flags);

	raw_spin_lock_irqsave(&env.dst_rq->__lock, flags);
	if (env.task)
		vip_attach_task(env.task, env.dst_rq);
	raw_spin_unlock_irqrestore(&env.dst_rq->__lock, flags);

	trace_vip_balance(&env);

	return env.task ? 1 : 0;
}

#define VIP_KICK_IDLE  0

static void kick_vip_balance(struct vip_rq *vrq, struct task_struct *p)
{
	struct vip_rq *t_vrq;
	int cpu;
	unsigned int flags;

	if (!vrq || !is_vip_task(p) || !cpu_active(vrq->cpu))
		return;

#if VIP_KICK_IDLE
	/* Find a idle cpu */
	for_each_online_cpu(cpu) {
		if (available_idle_cpu(cpu) && !cpu_halted(cpu) &&
			cpu != vrq->cpu) {
			t_vrq = &per_cpu(vip_rq, cpu);
			flags = atomic_fetch_or(VIP_BALANCE_KICK, &t_vrq->kick_flags);
			if (flags & VIP_BALANCE_KICK)
				return;
			trace_kick_vip_balance(vrq->cpu, cpu, p->pid, true);
			smp_call_function_single_async(cpu, &t_vrq->csd);
			return;
		}
	}
#else
	cpu = find_vip_cpu(p);
	if (cpu >= 0 && cpu != vrq->cpu) {
		t_vrq = &per_cpu(vip_rq, cpu);
		flags = atomic_fetch_or(VIP_BALANCE_KICK, &t_vrq->kick_flags);
		if (flags & VIP_BALANCE_KICK)
			return;
		trace_kick_vip_balance(vrq->cpu, cpu, p->pid, false);
		smp_call_function_single_async(cpu, &t_vrq->csd);
	}
#endif
}

static int vip_balance_commit(struct vip_rq *vrq)
{
	int ret = 0;

	if (!has_vip_running() || vrq->doing_balance)
		return ret;

	vrq->doing_balance = true;
	ret = vip_balance(vrq->cpu, cpu_rq(vrq->cpu));
	vrq->doing_balance = false;

	return ret;
}

static void vip_csd_func(void *info)
{
	struct vip_rq *vrq = info;
	unsigned int flags;

	flags = atomic_fetch_andnot(VIP_BALANCE_KICK, &vrq->kick_flags);
	WARN_ON(!(flags & VIP_BALANCE_KICK));

	vip_balance_commit(vrq);
}

static void enqueue_vip_task(struct rq *rq, struct task_struct *p)
{
	struct vip_rq *vrq;
	struct vip_task *tmp, *vip;

	if (!rq || !p)
		return;

	vip = task_to_vip(p);
	if (!vip || !vip->prio || !list_empty(&vip->entry))
		return;

	vrq = &per_cpu(vip_rq, cpu_of(rq));
	list_for_each_entry(tmp, &vrq->vip_list, entry) {
		if (vip->prio > tmp->prio)
			break;
	}
	list_add(&vip->entry, tmp->entry.prev);

	vrq->h_prio = vip_rq_highest_prio(vrq);
	vrq->nr_running++;
	atomic_inc(&sys_vip_stats.nr_running);

	trace_queue_vip_task(true, cpu_of(rq), vrq, p->pid);
}

static void dequeue_vip_task(struct rq *rq, struct task_struct *p)
{
	struct vip_rq *vrq;
	struct vip_task *vip;

	if (!rq || !p)
		return;

	vip = task_to_vip(p);
	if (!vip || !vip->prio || list_empty(&vip->entry))
		return;

	vrq = &per_cpu(vip_rq, cpu_of(rq));
	list_del_init(&vip->entry);

	vrq->h_prio = vip_rq_highest_prio(vrq);
	vrq->nr_running--;
	atomic_dec(&sys_vip_stats.nr_running);

	trace_queue_vip_task(false, cpu_of(rq), vrq, p->pid);
}

int set_vip_prio_locked(struct task_struct *p, int prio, bool set_orig)
{
	struct rq *rq = task_rq(p);
	struct vip_task *vip, *old;
	int orig_prio, ret = 0;
	bool queued;

	/* rq lock must been held here */
	lockdep_assert_rq_held(rq);

	queued = is_fair_task(p) && task_on_rq_queued(p);
	if (queued) {
		update_rq_clock(rq);
		dequeue_vip_task(rq, p);
	}

	vip = task_to_vip(p);
	orig_prio = vip ? vip->orig_prio : 0;

	if (!vip && prio) {
		/* KMI-frozen task_struct: allocate the node externally */
		vip = kzalloc(sizeof(*vip), GFP_ATOMIC);
		if (!vip) {
			ret = -ENOMEM;
			goto out;
		}
		vip->p = p;
		INIT_LIST_HEAD(&vip->entry);
		old = xa_store(&vip_nodes, (unsigned long)p, vip, GFP_ATOMIC);
		if (xa_err(old)) {
			kfree(vip);
			vip = NULL;
			ret = xa_err(old);
			goto out;
		}
		/*
		 * The fork hook drops stale nodes on slab address reuse;
		 * free defensively here too should one slip through.
		 */
		if (old)
			kfree_rcu(old, rcu);
	}

	if (vip) {
		vip->prio = prio;
		if (set_orig)
			vip->orig_prio = prio;
	}

	/* prio 0: a node exists iff prio != 0, so drop the node */
	if (!prio && vip) {
		xa_erase(&vip_nodes, (unsigned long)p);
		kfree_rcu(vip, rcu);
		vip = NULL;
	}

out:
	if (queued)
		enqueue_vip_task(rq, p);

	trace_set_vip_prio(p->pid, prio, set_orig ? prio : orig_prio);

	return ret;
}

int set_vip_prio(struct task_struct *p, int prio, bool set_orig)
{
	struct rq_flags rf;
	struct rq *rq = NULL;

	rq = task_rq_lock(p, &rf);

	set_vip_prio_locked(p, prio, set_orig);

	task_rq_unlock(rq, p, &rf);

	return 0;
}
EXPORT_SYMBOL_GPL(set_vip_prio);

/* read-only accessor for the MI QOS (os_cpu_qi/qs) module */
int get_vip_prio(struct task_struct *p)
{
	return vip_prio(p);
}
EXPORT_SYMBOL_GPL(get_vip_prio);

void inherit_vip_prio(struct task_struct *src_p,
		struct task_struct *dst_p)
{
	struct vip_task *tmp;

	if (!src_p || !dst_p)
		return;

	tmp = task_to_vip(src_p);
	if (!tmp || !tmp->prio)
		return;

	set_vip_prio(dst_p, tmp->prio, false);
}
EXPORT_SYMBOL_GPL(inherit_vip_prio);

void restore_vip_prio(struct task_struct *p)
{
	struct vip_task *tmp;

	if (!p)
		return;

	tmp = task_to_vip(p);
	if (!tmp)
		return;

	set_vip_prio(p, tmp->orig_prio, false);
}
EXPORT_SYMBOL_GPL(restore_vip_prio);

/***********************************************
 * VIP Sched vendor hooks
 */
void vip_sched_enqueue_task_fair(void *data, struct rq *rq,
		struct task_struct *p, int flags)
{
	enqueue_vip_task(rq, p);
}

void vip_sched_dequeue_task_fair(void *data, struct rq *rq,
		struct task_struct *p, int flags)
{
	dequeue_vip_task(rq, p);
}

void vip_sched_select_task_rq_fair(void *data, struct task_struct *p,
		int prev_cpu, int sd_flag, int wake_flags, int *target_cpu)
{
	if (vip_prio(p))
		*target_cpu = find_vip_cpu(p);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/* Walk up scheduling entities hierarchy */
#define for_each_sched_entity(se) \
			for (; se; se = se->parent)
#else   /* !CONFIG_FAIR_GROUP_SCHED */
#define for_each_sched_entity(se) \
			for (; se; se = NULL)
#endif

void vip_sched_replace_next_task_fair(void *data, struct rq *rq,
		struct task_struct **p, struct sched_entity **se, bool *repick,
		bool simple, struct task_struct *prev)
{
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu_of(rq));
	struct vip_task *vip;
	struct task_struct *task;
	struct cfs_rq *cfs_rq;

	if (list_empty(&vrq->vip_list))
		return;

	/*
	 * If the task picked by CFS scheduler is cpu binding, keep it
	 * to prevent starving issue.
	 */
	task = *p;
	if (task && task->nr_cpus_allowed == 1) {
		trace_vip_replace_next_task(task, 0);
		return;
	}

	vip = list_first_entry(&vrq->vip_list, struct vip_task, entry);
	task = vip_to_task(vip);

	if (task && task != prev && (task->on_cpu == 1 || task->on_rq == 0 ||
		task->on_rq == TASK_ON_RQ_MIGRATING ||
		task_cpu(task) != cpu_of(rq))) {
		panic("replace task error, pid=%d, on_cpu=%d, on_rq=%d, task_cpu=%d, cpu=%d\n",
				task->pid, task->on_cpu, task->on_rq, task_cpu(task), cpu_of(rq));
	}

	*p = task;
	*se = &task->se;
	*repick = true;

	if (simple) {
		for_each_sched_entity((*se)) {
			cfs_rq = cfs_rq_of(*se);
			set_next_entity(cfs_rq, *se);
		}
	}

	trace_vip_replace_next_task(task, vip->prio);
}

void vip_sched_check_preempt_wakeup(void *data, struct rq *rq,
		struct task_struct *p, bool *preempt, bool *nopreempt, int wake_flags,
		struct sched_entity *se, struct sched_entity *pse,
		int next_buddy_marked, unsigned int granularity)
{
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu_of(rq));
	int p_prio = vip_prio(p);
	int c_prio = vip_prio(rq->curr);

	if (vrq->h_prio)
		*nopreempt = true;

	if (p_prio > c_prio) {
		*preempt = true;
		trace_vip_preempt_curr(rq->curr->pid, p->pid, p_prio);
	}
}

void vip_sched_schedule(void *data, struct task_struct *prev,
		struct task_struct *next, struct rq *rq)
{
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu_of(rq));
	struct vip_task *last;

	vrq->vip_preempt = vip_preempt_classify(prev, next, rq);
	trace_vip_schedule(prev->pid, vip_prio(prev), next->pid,
			vip_prio(next), vrq->vip_preempt, task_util(next));

	if (vrq->vip_preempt == VIP_PREEMPT_VIP) {
		if (unlikely(list_empty(&vrq->vip_list)))
			return;

		/* Get the last entry in rq's vip_list */
		last = list_last_entry(&vrq->vip_list, struct vip_task, entry);
		kick_vip_balance(vrq, vip_to_task(last));
		return;
	}

	/*
         * Heavy-yield heuristic: a VIP task yielded the cpu to a heavy
         * non-VIP next task (task_util > 255, ~25% of 1024). The
         * remaining VIP queue members should look for another cpu
         * instead of waiting behind the heavy runner.
         */
	if (is_vip_task(prev) && !is_vip_task(next) && is_fair_task(next) &&
	    task_util(next) > 255 && !list_empty(&vrq->vip_list)) {
		last = list_last_entry(&vrq->vip_list, struct vip_task, entry);
		kick_vip_balance(vrq, vip_to_task(last));
	}
}

void vip_sched_scheduler_tick(void *data, struct rq *rq)
{
	int cpu = cpu_of(rq);
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu);
	struct vip_task *last;

	if (!vrq->nr_running) {
		vrq->vip_preempt = VIP_PREEMPT_NONE;
		if (!cpu_halted(cpu))
			vip_balance_commit(vrq);
	}

	raw_spin_lock(&rq->__lock);
	if (vrq->nr_running && !is_vip_task(rq->curr) &&
			!is_idle_task(rq->curr))
		vrq->vip_preempt = VIP_PREEMPT_VIP;

	if (is_max_possible_cluster_cpu(cpu) &&
			vrq->nr_running >= 2) {
		/* No available idle sibling, let non-prefer cpu to help */
		if (find_idle_cpu(vip_freq_domain_cpumask(cpu)) < 0)
			vrq->vip_preempt = VIP_PREEMPT_VIP;

		if (!list_empty(&vrq->vip_list)) {
			/* Get the last entry in rq's vip_list */
			last = list_last_entry(&vrq->vip_list,
					struct vip_task, entry);
			kick_vip_balance(vrq, vip_to_task(last));
		}
	}
	raw_spin_unlock(&rq->__lock);
}

/*
 * 5.15 port: kept for API completeness, not registered - see the
 * file header comment for the reason.
 */
int vip_sched_newidle_balance(struct rq *rq)
{
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu_of(rq));

	return vip_balance_commit(vrq);
}

/***********************************************
 * VIP Sched user interface
 */

struct vip_io_cfg {
	pid_t pid;
	unsigned int prio;
};

static int vip_sched_set_prio_by_pid(pid_t pid, unsigned int prio)
{
	struct task_struct *p = NULL;
	int ret;

	rcu_read_lock();

	p = find_process_by_pid(pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}

	get_task_struct(p);
	rcu_read_unlock();

	ret = set_vip_prio(p, prio, true);
	if (ret)
		pr_err("set vip prio failed\n");

	put_task_struct(p);

	return ret;
}

/*
 * 5.15 port: xring wires this into the perf_actuator ioctl, here it
 * is called from the /proc/vip_sched write path with the same binary
 * vip_io_cfg ABI.
 */
static int perf_actuator_set_vip_prio(void __user *uarg)
{
	struct vip_io_cfg cfg;

	if (copy_from_user(&cfg, uarg, sizeof(struct vip_io_cfg)))
		return -EFAULT;

	return vip_sched_set_prio_by_pid(cfg.pid, cfg.prio);
}

static void vip_sched_show_status(struct seq_file *m)
{
	int cpu;
	struct vip_rq *vrq;

	seq_printf(m, "vip_nr_running: %d\n", atomic_read(&sys_vip_stats.nr_running));
	for_each_online_cpu(cpu) {
		vrq = &per_cpu(vip_rq, cpu);
		seq_printf(m, "cpu%d: h_prio=%d nr_running=%d preempt=%d\n",
				cpu, vrq->h_prio, vrq->nr_running, vrq->vip_preempt);
	}
}

static int vip_sched_proc_show(struct seq_file *m, void *v)
{
	vip_sched_show_status(m);
	return 0;
}

static ssize_t vip_sched_proc_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	char kbuf[32];
	int pid, prio;

	/* Keep the xring perf_actuator binary ABI intact */
	if (count == sizeof(struct vip_io_cfg))
		return perf_actuator_set_vip_prio((void __user *)ubuf) ? : count;

	if (count > sizeof(kbuf) - 1)
		return -EINVAL;

	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (sscanf(kbuf, "%d %d", &pid, &prio) != 2)
		return -EINVAL;

	return vip_sched_set_prio_by_pid(pid, prio) ? : count;
}

static int vip_sched_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, vip_sched_proc_show, NULL);
}

static const struct proc_ops vip_sched_proc_ops = {
	.proc_open		= vip_sched_proc_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release		= single_release,
	.proc_write		= vip_sched_proc_write,
};

/***********************************************
 * VIP Sched init methods
 */

void vip_init_rq(int cpu)
{
	struct vip_rq *vrq = &per_cpu(vip_rq, cpu);

	vrq->cpu = cpu;
	vrq->h_prio = 0;
	vrq->nr_running = 0;
	vrq->vip_preempt = VIP_PREEMPT_NONE;
	vrq->sync_waiting = false;

	INIT_LIST_HEAD(&vrq->vip_list);

	/* vip balance */
	vrq->doing_balance = false;
	INIT_CSD(&vrq->csd, vip_csd_func, vrq);
	atomic_set(&vrq->kick_flags, 0);
}

/*
 * 5.15 port: vip state lives in the vip_nodes xarray (task_struct is
 * KMI-frozen). When the slab reuses a dead task's address, drop the
 * stale node here so the new task never sees a leftover vip state
 * (a stale node with prio != 0 would be picked up by enqueue_vip_task
 * and later dereferenced through vip_to_task()).
 */
static void vip_sched_fork_init(void *data, struct task_struct *p)
{
	struct vip_task *stale;

	stale = xa_load(&vip_nodes, (unsigned long)p);
	if (stale) {
		xa_erase(&vip_nodes, (unsigned long)p);
		kfree_rcu(stale, rcu);
	}
}

static void init_sys_vip_stats(void)
{
	atomic_set(&sys_vip_stats.nr_running, 0);
}

#if IS_ENABLED(CONFIG_MI_QOS)
/*
 * perf_actuator user-space ABI: the VIP module serves cmd 0x4008781e
 * (set_vip_prio, struct vip_io_cfg) through the XR function table. On
 * 5.15 the same handler is reachable via the /proc/vip_sched write
 * path; register it here as well so that the /dev/perf_actuator ABI
 * provided by mi_qos (xr_fbt) stays complete.
 */
extern int register_perf_actuator(u32 cmd, int (*fn)(unsigned long uarg));

static int vip_xr_set_vip_prio(unsigned long uarg)
{
	return perf_actuator_set_vip_prio((void __user *)uarg);
}

static void vip_xr_fbt_register(void)
{
	int ret = register_perf_actuator(0x4008781e, vip_xr_set_vip_prio);

	if (ret)
		pr_warn("vip_sched: perf_actuator 0x4008781e register failed: %d\n",
			ret);
}
#else
static void vip_xr_fbt_register(void) { }
#endif /* CONFIG_MI_QOS */

static int vip_cpu_online(unsigned int cpu)
{
	vip_init_rq(cpu);
	return 0;
}

int vip_sched_init(void)
{
	int cpu, ret;

	init_sys_vip_stats();

	for_each_possible_cpu(cpu)
		max_possible_capacity = max(max_possible_capacity,
				capacity_orig_of(cpu));

	for_each_online_cpu(cpu)
		vip_init_rq(cpu);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"xiaomi/vip_sched:online", vip_cpu_online, NULL);
	if (ret < 0) {
		pr_err("cpuhp setup failed, ret=%d\n", ret);
		return ret;
	}

	ret  = register_trace_android_rvh_sched_fork_init(
			vip_sched_fork_init, NULL);
	ret |= register_trace_android_rvh_enqueue_task_fair(
			vip_sched_enqueue_task_fair, NULL);
	ret |= register_trace_android_rvh_dequeue_task_fair(
			vip_sched_dequeue_task_fair, NULL);
	ret |= register_trace_android_rvh_select_task_rq_fair(
			vip_sched_select_task_rq_fair, NULL);
	ret |= register_trace_android_rvh_replace_next_task_fair(
			vip_sched_replace_next_task_fair, NULL);
	ret |= register_trace_android_rvh_check_preempt_wakeup(
			vip_sched_check_preempt_wakeup, NULL);
	ret |= register_trace_android_rvh_schedule(vip_sched_schedule, NULL);
	ret |= register_trace_android_vh_scheduler_tick(
			vip_sched_scheduler_tick, NULL);
	if (ret) {
		pr_err("register vendor hooks failed, ret=%d\n", ret);
		return ret;
	}

	if (!proc_create("vip_sched", 0666, NULL, &vip_sched_proc_ops)) {
		pr_err("create /proc/vip_sched failed\n");
		return -ENOMEM;
	}

	/* xr_fbt (/dev/perf_actuator) ABI add-on, best effort */
	vip_xr_fbt_register();

	pr_info("vip_sched initialized\n");

	return 0;
}
/* Must run after scheduler topology is ready (capacity_orig_of valid) */
late_initcall(vip_sched_init);
