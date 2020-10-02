/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ION Memory Allocator kernel interface header
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2020 Linaro Ltd.
 */

#ifndef _DYN_PAGE_POOL_H
#define _DYN_PAGE_POOL_H

#include <linux/device.h>
#include <linux/kref.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/shrinker.h>
#include <linux/types.h>

/**
 * struct dynamic_page_pool - pagepool struct
 * @count[]:		array of number of pages of that type in the pool
 * @items[]:		array of list of pages of the specific type
 * @mutex:		lock protecting this struct and especially the count
 *			item list
 * @gfp_mask:		gfp_mask to use from alloc
 * @order:		order of pages in the pool
 * @list:		list node for list of pools
 *
 * Allows you to keep a pool of pre allocated pages to use
 * Keeping a pool of pages that is ready for dma, ie any cached mapping have
 * been invalidated from the cache, provides a significant performance benefit
 * on many systems
 */

/* page types we track in the pool */
enum {
	POOL_LOWPAGE,      /* Clean lowmem pages */
	POOL_HIGHPAGE,     /* Clean highmem pages */
	POOL_LOWDEFERRED,  /* Dirty lowmem pages */
	POOL_HIGHDEFERRED, /* Dirty highmem pages */

	POOL_TYPE_SIZE,
};

struct dynamic_page_pool {
	int count[POOL_TYPE_SIZE];
	struct list_head items[POOL_TYPE_SIZE];
	struct mutex mutex;
	gfp_t gfp_mask;
	unsigned int order;
	struct list_head list;

	wait_queue_head_t waitqueue;
	struct task_struct *task;
};

struct dynamic_page_pool *dynamic_page_pool_create(gfp_t gfp_mask,
						   unsigned int order);
void dynamic_page_pool_destroy(struct dynamic_page_pool *pool);
struct page *dynamic_page_pool_alloc(struct dynamic_page_pool *pool);
void dynamic_page_pool_free(struct dynamic_page_pool *pool, struct page *page);

#endif /* _DYN_PAGE_POOL_H */
