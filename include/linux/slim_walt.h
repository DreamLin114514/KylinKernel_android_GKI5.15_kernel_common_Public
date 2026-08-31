/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SLIM_WALT_H
#define _LINUX_SLIM_WALT_H

/*
 * slim_walt - per-CPU WALT-style windowed load tracking.
 *
 * Only reads existing scheduler state (rq->nr_running, rq->clock) and
 * keeps all of its own state in per-CPU storage.  It never changes the
 * layout of task_struct or struct rq, so prebuilt GKI vendor modules
 * keep their ABI.
 */

#ifdef CONFIG_SLIM_WALT

/* busy percentage of the last completed window, 0-100 */
unsigned int slim_walt_get_busy(int cpu);

/* runnable load of the last completed window, 0-1024 */
unsigned long slim_walt_get_load(int cpu);

bool slim_walt_enabled(void);

#else /* !CONFIG_SLIM_WALT */

static inline unsigned int slim_walt_get_busy(int cpu) { return 0; }
static inline unsigned long slim_walt_get_load(int cpu) { return 0; }
static inline bool slim_walt_enabled(void) { return false; }

#endif /* CONFIG_SLIM_WALT */

#endif /* _LINUX_SLIM_WALT_H */
