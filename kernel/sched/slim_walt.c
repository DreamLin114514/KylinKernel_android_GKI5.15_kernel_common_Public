/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * slim_walt - per-CPU windowed load tracking (WALT-style, safe subset).
 *
 * Tracks per-CPU runnable busy time over fixed windows.  Deliberately does
 * NOT touch task_struct / struct rq layout so prebuilt GKI vendor modules
 * keep their ABI (an earlier task_struct-embedded port reboot-looped the
 * device for exactly that reason).  Samples arrive from the
 * android_vh_scheduler_tick vendor hook; no core scheduler edits needed.
 *
 * Control (/proc/slim_sched/):
 *   walt_ctrl    0/1  enable windowed tracking (default 0)
 *   walt_busy         per-CPU window stats (read-only)
 *   window_ms         window length in ms (read-only)
 */
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/slim_walt.h>
#include <trace/hooks/sched.h>
#include "sched.h"

#define SLIM_WALT_DEFAULT_WINDOW_MS	20
#define SLIM_WALT_STALE              0xffffffffUL

static unsigned int slim_walt_ctrl = 1;	/* 1 = enabled by default (cheap, ABI-safe) */
module_param(slim_walt_ctrl, uint, 0644);
MODULE_PARM_DESC(slim_walt_ctrl,
		 "Enable slim_walt windowed load tracking (0/1)");

static unsigned int slim_walt_window_ms = SLIM_WALT_DEFAULT_WINDOW_MS;
module_param(slim_walt_window_ms, uint, 0644);
MODULE_PARM_DESC(slim_walt_window_ms, "Load window length in ms");

struct slim_walt_cpu {
	u64	window_start;	/* ns, current window start (rq clock) */
	u64	last_tick;	/* ns, last sample time */
	u64	busy_ns;	/* accumulated runnable time in window */
	u64	wall_ns;	/* accumulated wall time in window */
	u64	run_sum;	/* accumulated nr_running samples */
	u32	run_peak;	/* peak nr_running in window */
	u32	run_samples;
	u64	prev_busy_ns;	/* completed window values */
	u64	prev_wall_ns;
	u32	prev_run_avg;	/* avg nr_running, x1024 */
	u32	prev_run_peak;
	u64	window_count;
};

static DEFINE_PER_CPU(struct slim_walt_cpu, slim_walt_cpu);

bool slim_walt_enabled(void)
{
	return READ_ONCE(slim_walt_ctrl) != 0;
}
EXPORT_SYMBOL_GPL(slim_walt_enabled);

/*
 * Age the last completed window against the time since this CPU last
 * ticked.  A NO_HZ idle CPU gets no scheduler ticks, so its window
 * freezes holding the pre-idle busy ratio; without decay a busy->idle
 * transition would leave the reading stuck high forever and pin the
 * cpufreq governor at max frequency while the device is idle.
 * Halve the ratio per elapsed window; report stale after 10 windows.
 * Returns the number of halvings, or SLIM_WALT_STALE when too old.
 */
static unsigned long slim_walt_age_decay(int cpu)
{
	struct slim_walt_cpu *sw = per_cpu_ptr(&slim_walt_cpu, cpu);
	u64 win = (u64)slim_walt_window_ms * NSEC_PER_MSEC;
	u64 age;

	if (!sw->last_tick)
		return SLIM_WALT_STALE;

	age = ktime_get_ns() - sw->last_tick;
	if (age < win)
		return 0;	/* current window: no decay */

	age = div_u64(age, win);
	if (age >= 10)
		return SLIM_WALT_STALE;
	return age;
}

static void slim_walt_tick_hook(void *data, struct rq *rq)
{
	struct slim_walt_cpu *sw;
	u64 now, delta;
	u32 nr;

	if (unlikely(!slim_walt_enabled()))
		return;

	sw = this_cpu_ptr(&slim_walt_cpu);
	/* ktime (not rq_clock) so the age decay in the read path can
	 * compare against ktime_get_ns() on the same time base */
	now = ktime_get_ns();

	if (unlikely(!sw->last_tick)) {
		sw->last_tick = now;
		sw->window_start = now;
		return;
	}

	delta = now - sw->last_tick;
	if (unlikely(delta > (u64)slim_walt_window_ms * NSEC_PER_MSEC * 8)) {
		/* long gap (suspend / deep idle): discard the partial window so
		 * the stale pre-idle busy ratio is not committed on wakeup */
		sw->busy_ns = 0;
		sw->wall_ns = 0;
		sw->run_sum = 0;
		sw->run_peak = 0;
		sw->run_samples = 0;
		delta = 0;
	}

	nr = READ_ONCE(rq->nr_running);
	if (nr)
		sw->busy_ns += delta;
	sw->wall_ns += delta;
	sw->run_sum += nr;
	sw->run_samples++;
	if (nr > sw->run_peak)
		sw->run_peak = nr;

	sw->last_tick = now;

	if (now - sw->window_start >= (u64)slim_walt_window_ms * NSEC_PER_MSEC) {
		sw->prev_busy_ns = sw->busy_ns;
		sw->prev_wall_ns = sw->wall_ns;
		sw->prev_run_avg = sw->run_samples ?
			div_u64(sw->run_sum * 1024, sw->run_samples) : 0;
		sw->prev_run_peak = sw->run_peak;
		sw->window_count++;
		sw->window_start = now;
		sw->busy_ns = 0;
		sw->wall_ns = 0;
		sw->run_sum = 0;
		sw->run_peak = 0;
		sw->run_samples = 0;
	}
}

/* busy percentage of the last completed window, 0-100 */
unsigned int slim_walt_get_busy(int cpu)
{
	struct slim_walt_cpu *sw;
	unsigned long decay;

	if (!slim_walt_enabled())
		return 0;

	sw = per_cpu_ptr(&slim_walt_cpu, cpu);
	if (!sw->prev_wall_ns)
		return 0;

	decay = slim_walt_age_decay(cpu);
	if (decay == SLIM_WALT_STALE)
		return 0;

	return div_u64((sw->prev_busy_ns >> decay) * 100, sw->prev_wall_ns);
}
EXPORT_SYMBOL_GPL(slim_walt_get_busy);

/* runnable load of the last completed window, 0-1024 */
unsigned long slim_walt_get_load(int cpu)
{
	struct slim_walt_cpu *sw;
	unsigned long decay;

	if (!slim_walt_enabled())
		return 0;

	sw = per_cpu_ptr(&slim_walt_cpu, cpu);
	if (!sw->prev_wall_ns)
		return 0;

	decay = slim_walt_age_decay(cpu);
	if (decay == SLIM_WALT_STALE)
		return 0;

	return div_u64((sw->prev_busy_ns >> decay) * 1024, sw->prev_wall_ns);
}
EXPORT_SYMBOL_GPL(slim_walt_get_load);

/* ------------------- proc interface ------------------- */

static int slim_walt_ctrl_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", slim_walt_ctrl);
	return 0;
}

static int slim_walt_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, slim_walt_ctrl_show, NULL);
}

static ssize_t slim_walt_ctrl_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[8];
	unsigned int val;
	int ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	ret = kstrtouint(kbuf, 10, &val);
	if (ret)
		return -EINVAL;

	slim_walt_ctrl = val ? 1 : 0;
	return count;
}

static const struct proc_ops slim_walt_ctrl_ops = {
	.proc_open	= slim_walt_ctrl_open,
	.proc_read	= seq_read,
	.proc_write	= slim_walt_ctrl_write,
	.proc_release	= single_release,
};

static int slim_walt_busy_show(struct seq_file *m, void *v)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct slim_walt_cpu *sw = per_cpu_ptr(&slim_walt_cpu, cpu);

		seq_printf(m, "%d %u %lu %u %u %llu\n",
			   cpu,
			   slim_walt_get_busy(cpu),
			   slim_walt_get_load(cpu),
			   sw->prev_run_avg,
			   sw->prev_run_peak,
			   sw->window_count);
	}
	return 0;
}

static int slim_walt_busy_open(struct inode *inode, struct file *file)
{
	return single_open(file, slim_walt_busy_show, NULL);
}

static const struct proc_ops slim_walt_busy_ops = {
	.proc_open	= slim_walt_busy_open,
	.proc_read	= seq_read,
	.proc_release	= single_release,
};

static int slim_walt_window_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", slim_walt_window_ms);
	return 0;
}

static int slim_walt_window_open(struct inode *inode, struct file *file)
{
	return single_open(file, slim_walt_window_show, NULL);
}

static const struct proc_ops slim_walt_window_ops = {
	.proc_open	= slim_walt_window_open,
	.proc_read	= seq_read,
	.proc_release	= single_release,
};

static struct proc_dir_entry *slim_walt_dir;

static int __init slim_walt_init(void)
{
	int ret;

	ret = register_trace_android_vh_scheduler_tick(slim_walt_tick_hook,
						       NULL);
	if (ret) {
		pr_err("slim_walt: register tick hook failed %d\n", ret);
		return ret;
	}

	slim_walt_dir = proc_mkdir("slim_sched", NULL);
	if (slim_walt_dir) {
		proc_create("walt_ctrl", 0644, slim_walt_dir,
			    &slim_walt_ctrl_ops);
		proc_create("walt_busy", 0444, slim_walt_dir,
			    &slim_walt_busy_ops);
		proc_create("window_ms", 0444, slim_walt_dir,
			    &slim_walt_window_ops);
	}

	pr_info("slim_walt: windowed load tracking ready (ctrl=%u, "
		"window=%ums)\n", slim_walt_ctrl, slim_walt_window_ms);
	return 0;
}

static void __exit slim_walt_exit(void)
{
	unregister_trace_android_vh_scheduler_tick(slim_walt_tick_hook, NULL);

	if (slim_walt_dir) {
		remove_proc_entry("walt_ctrl", slim_walt_dir);
		remove_proc_entry("walt_busy", slim_walt_dir);
		remove_proc_entry("window_ms", slim_walt_dir);
		remove_proc_entry("slim_sched", NULL);
		slim_walt_dir = NULL;
	}
}

late_initcall(slim_walt_init);
module_exit(slim_walt_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Per-CPU windowed load tracking (WALT-style, ABI-safe)");
MODULE_AUTHOR("KylinKernel");
