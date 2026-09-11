/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kylin_used_engine - unified kernel<->userspace tuning/info interface.
 *
 * Read channel:  /proc/kylin_engine/<feature>/<stat>
 *                One bare value per node (machine friendly), or for list
 *                nodes a homogeneous one-entry-per-line format.  The
 *                optional "all" node carries a human readable aggregate.
 * Write channel: /sys/kernel/kylin_engine/<feature>/<attr>
 *                Tunable knobs, default 0644.
 *
 * Features register through kylin_engine_register_feature() and get all
 * nodes created automatically.  Built-in only (calls internal mm/sched
 * helpers that are not exported to modules).
 */
#ifndef _LINUX_KYLIN_USED_ENGINE_H
#define _LINUX_KYLIN_USED_ENGINE_H

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>

struct kylin_engine_feature;

/*
 * One read-only stat node: /proc/kylin_engine/<feature>/<name>.
 * @show must print a single bare value (no labels) so userspace can
 * fopen()+fscanf() it; list nodes print one entry per line.
 * @data is optional per-node context handed back to @show.
 */
struct kylin_engine_stat {
	const char *name;
	void (*show)(struct kylin_engine_feature *f, struct seq_file *m,
		     void *data);
	void *data;
};

/*
 * One tunable knob: /sys/kernel/kylin_engine/<feature>/<name>.
 * @show prints the current value into buf (sysfs page sized).
 * @store parses buf (len) and returns 0 or negative errno; the framework
 * converts success into @len.
 */
struct kylin_engine_attr {
	const char *name;
	umode_t mode;			/* 0 -> 0644 */
	ssize_t (*show)(struct kylin_engine_feature *f, char *buf);
	ssize_t (*store)(struct kylin_engine_feature *f, const char *buf,
			 size_t len);
};

/*
 * A feature: a directory on both channels plus an optional "all" node.
 * Caller keeps @name/@stats/@attrs memory alive while registered.
 * Private fields are managed by the framework; zero the whole struct.
 */
struct kylin_engine_feature {
	const char *name;
	const struct kylin_engine_stat *stats;
	int nr_stats;
	const struct kylin_engine_attr *attrs;
	int nr_attrs;
	/* optional human readable aggregate, exposed as "all" */
	void (*all_show)(struct kylin_engine_feature *f, struct seq_file *m);
	void *data;

	/* private */
	struct list_head list;
	struct kobject *kobj;
	struct proc_dir_entry *pde;
	void *_nodes;
	void *_wraps;
	bool registered;
};

#ifdef CONFIG_KYLIN_USED_ENGINE
int kylin_engine_register_feature(struct kylin_engine_feature *f);
void kylin_engine_unregister_feature(struct kylin_engine_feature *f);
#else
static inline int kylin_engine_register_feature(struct kylin_engine_feature *f)
{
	return 0;
}

static inline void kylin_engine_unregister_feature(struct kylin_engine_feature *f)
{
}
#endif

#endif /* _LINUX_KYLIN_USED_ENGINE_H */
