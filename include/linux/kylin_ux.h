/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kylin_ux - per-task UX state store shared by KylinUsedEngine features.
 *
 * 5.15 GKI task_struct carries no vendor data pointer, so per-task state
 * lives in a pid-keyed hash table here.  Entries are validated against the
 * task pointer and reaped through the android_vh_free_task hook.
 *
 * Consumers:
 *   kylin_binder_ux   - binder transaction UX inheritance (oplus style)
 *   kylin_ux_locking  - lock-holder protection + waiter reordering
 */
#ifndef _LINUX_KYLIN_UX_H
#define _LINUX_KYLIN_UX_H

#include <linux/list.h>
#include <linux/types.h>

struct task_struct;

/* inheritance sources */
#define KYLIN_UX_INHERIT_BINDER		0x1UL
#define KYLIN_UX_INHERIT_RWSEM		0x2UL
#define KYLIN_UX_INHERIT_MUTEX		0x4UL
#define KYLIN_UX_INHERIT_FUTEX		0x8UL
#define KYLIN_UX_INHERIT_ANIM		0x10UL

/* direct ux states (oplus SA_TYPE_* subset) */
#define KYLIN_UX_TYPE_NONE		0x0
#define KYLIN_UX_TYPE_LIGHT		0x1
#define KYLIN_UX_TYPE_HEAVY		0x2

enum kylin_ux_state_type {
	KYLIN_UX_TYPE_NONE_ = 0,
	KYLIN_UX_TYPE_INHERIT,
	KYLIN_UX_TYPE_DIRECT,
};

#ifdef CONFIG_KYLIN_UX

/* classification */
bool kylin_ux_test(struct task_struct *p);       /* direct or inherited */
bool kylin_ux_test_rt(struct task_struct *p);
int kylin_ux_state_type(struct task_struct *p);
int kylin_ux_get_state(struct task_struct *p);
int kylin_ux_get_depth(struct task_struct *p);

/* inheritance (binder / locking consumers) */
bool kylin_ux_test_inherit(struct task_struct *p, unsigned long bit);
void kylin_ux_set_inherit(struct task_struct *p, unsigned long bit,
			  int depth, int state);
void kylin_ux_unset_inherit(struct task_struct *p, unsigned long bit);
unsigned long kylin_ux_get_inherit(struct task_struct *p);

/* direct marking (servicemanager style, sticky) */
void kylin_ux_set_direct(struct task_struct *p, int state);
void kylin_ux_clear_direct(struct task_struct *p);

/* binder async ux (userspace controlled, per pid) */
bool kylin_ux_async_enabled(struct task_struct *p);
void kylin_ux_set_async_enable(pid_t pid, int enable);
bool kylin_ux_async_sts(struct task_struct *p);
void kylin_ux_set_async_sts(struct task_struct *p, bool sts);

/* locking protection */
void kylin_ux_locking_acquire(struct task_struct *p, unsigned long settime);
void kylin_ux_locking_wait_mark(struct task_struct *p, unsigned long time);
bool kylin_ux_locking_inlock(struct task_struct *p);
int kylin_ux_locking_depth(struct task_struct *p);
void kylin_ux_locking_tick_expire(struct task_struct *p);
struct list_head *kylin_ux_locking_entry(struct task_struct *p);
bool kylin_ux_locking_onrq(struct task_struct *p);
void kylin_ux_locking_set_onrq(struct task_struct *p, bool on);
struct task_struct *kylin_ux_locking_entry_task(struct list_head *entry);

/* stats for the engine */
unsigned int kylin_ux_nr_tasks(void);

#else /* !CONFIG_KYLIN_UX */

static inline bool kylin_ux_test(struct task_struct *p) { return false; }
static inline bool kylin_ux_test_rt(struct task_struct *p) { return false; }
static inline int kylin_ux_state_type(struct task_struct *p) { return 0; }
static inline int kylin_ux_get_state(struct task_struct *p) { return 0; }
static inline int kylin_ux_get_depth(struct task_struct *p) { return 0; }
static inline bool kylin_ux_test_inherit(struct task_struct *p,
					 unsigned long bit) { return false; }
static inline void kylin_ux_set_inherit(struct task_struct *p,
					unsigned long bit, int depth,
					int state) { }
static inline void kylin_ux_unset_inherit(struct task_struct *p,
					  unsigned long bit) { }
static inline unsigned long kylin_ux_get_inherit(struct task_struct *p)
{
	return 0;
}
static inline void kylin_ux_set_direct(struct task_struct *p, int state) { }
static inline void kylin_ux_clear_direct(struct task_struct *p) { }
static inline bool kylin_ux_async_enabled(struct task_struct *p)
{
	return false;
}
static inline void kylin_ux_set_async_enable(pid_t pid, int enable) { }
static inline bool kylin_ux_async_sts(struct task_struct *p) { return false; }
static inline void kylin_ux_set_async_sts(struct task_struct *p, bool sts) { }
static inline void kylin_ux_locking_acquire(struct task_struct *p,
					    unsigned long settime) { }
static inline void kylin_ux_locking_wait_mark(struct task_struct *p,
					      unsigned long time) { }
static inline bool kylin_ux_locking_inlock(struct task_struct *p)
{
	return false;
}
static inline int kylin_ux_locking_depth(struct task_struct *p) { return 0; }
static inline void kylin_ux_locking_tick_expire(struct task_struct *p) { }
static inline struct list_head *kylin_ux_locking_entry(struct task_struct *p)
{
	return NULL;
}
static inline bool kylin_ux_locking_onrq(struct task_struct *p)
{
	return false;
}
static inline void kylin_ux_locking_set_onrq(struct task_struct *p, bool on) { }
static inline struct task_struct *kylin_ux_locking_entry_task(
	struct list_head *entry) { return NULL; }
static inline unsigned int kylin_ux_nr_tasks(void) { return 0; }

#endif /* CONFIG_KYLIN_UX */

#endif /* _LINUX_KYLIN_UX_H */
