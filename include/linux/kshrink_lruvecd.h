/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kshrink_lruvecd - async LRU reclaim of skipped pages (OnePlus port)
 *
 * The vmscan page-reclaim trylock hooks let a background kthread reclaim
 * pages whose trylock was contended/skipped, instead of stalling the
 * reclaim path. Declared here so mm/vmscan.c can call them.
 */
#ifndef _KSHRINK_LRUVECD_H_
#define _KSHRINK_LRUVECD_H_

#include <linux/types.h>
#include <linux/rwsem.h>

struct page;
struct list_head;

bool wakeup_kshrink_lruvecd(struct list_head *page_list);
void setpage_reclaim_trylock(struct page *page);
bool page_reclaim_trylock_fail(struct page *page);
void clearpage_reclaim_trylock(struct page *page);
bool reclaim_page_trylock(struct page *page, struct rw_semaphore *sem,
			  bool *got_lock);

#endif /* _KSHRINK_LRUVECD_H_ */
