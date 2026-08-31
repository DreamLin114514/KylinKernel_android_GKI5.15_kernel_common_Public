/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kylin_mm_opt - sensitive memory page pool API (OnePlus-style).
 *
 * A small pre-allocated page pool that latency-sensitive drivers (display,
 * camera, GPU) can draw from instead of hitting the allocator mid-frame.
 * Pages are reclaimed back into the pool only while memory is comfortable,
 * mirroring the water-mark protection of oplus_sensitive_mem_alloc.
 *
 * All functions are safe to call from any context; the pool is
 * spinlock-protected.
 */
#ifndef _KYLIN_MM_OPT_H_
#define _KYLIN_MM_OPT_H_

#include <linux/types.h>

struct page;

/*
 * Take one page of @order from the pool. Returns NULL if the pool is
 * disabled, the order does not match the pool order, or the pool is empty.
 */
struct page *kylin_mm_opt_pool_alloc(unsigned int order);

/*
 * Return one page of @order to the pool. Refuses to keep the page when
 * free memory is below the zone's WMARK_LOW or the pool is full, in
 * which case the caller must free the page normally.
 */
bool kylin_mm_opt_pool_refill(struct page *page, unsigned int order);

/* Number of pages of @order currently sitting in the pool. */
unsigned int kylin_mm_opt_pool_count(unsigned int order);

#endif /* _KYLIN_MM_OPT_H_ */
