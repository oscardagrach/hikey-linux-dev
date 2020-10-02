// SPDX-License-Identifier: GPL-2.0
/*
 * Dynamic page pool system
 *
 * Copyright (C) 2020 Linaro Ltd.
 *
 * Based on the ION page pool code
 * Copyright (C) 2011 Google, Inc.
 */

#include <linux/freezer.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/sched/signal.h>
#include <linux/dynamic_page_pool.h>

static LIST_HEAD(pool_list);
static DEFINE_MUTEX(pool_list_lock);

static void dynamic_page_pool_clean(struct dynamic_page_pool *pool);

static inline
struct page *dynamic_page_pool_alloc_pages(struct dynamic_page_pool *pool)
{
	if (fatal_signal_pending(current))
		return NULL;
	return alloc_pages(pool->gfp_mask, pool->order);
}

static inline void dynamic_page_pool_free_pages(struct dynamic_page_pool *pool,
						struct page *page)
{
	__free_pages(page, pool->order);
}

static void dynamic_page_pool_add_clean(struct dynamic_page_pool *pool, struct page *page)
{
	int index;

	if (PageHighMem(page))
		index = POOL_HIGHPAGE;
	else
		index = POOL_LOWPAGE;

	mutex_lock(&pool->mutex);
	list_add_tail(&page->lru, &pool->items[index]);
	pool->count[index]++;
	mutex_unlock(&pool->mutex);
	mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE,
			    1 << pool->order);
}

static void dynamic_page_pool_add_dirty(struct dynamic_page_pool *pool, struct page *page)
{
	int index;

	if (PageHighMem(page))
		index = POOL_HIGHDEFERRED;
	else
		index = POOL_LOWDEFERRED;

	mutex_lock(&pool->mutex);
	list_add_tail(&page->lru, &pool->items[index]);
	pool->count[index]++;
	mutex_unlock(&pool->mutex);

	mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE,
			    1 << pool->order);
}

static struct page *dynamic_page_pool_remove(struct dynamic_page_pool *pool, int index)
{
	struct page *page;

	if (!pool->count[index]) {
		WARN_ON(!pool->count[index]);
		return NULL;
	}
	page = list_first_entry(&pool->items[index], struct page, lru);
	pool->count[index]--;

	list_del(&page->lru);
	mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE,
			    -(1 << pool->order));
	return page;
}

struct page *dynamic_page_pool_fetch(struct dynamic_page_pool *pool)
{
	struct page *page = NULL;

	mutex_lock(&pool->mutex);
	if (pool->count[POOL_HIGHPAGE])
		page = dynamic_page_pool_remove(pool, POOL_HIGHPAGE);
	else if (pool->count[POOL_LOWPAGE])
		page = dynamic_page_pool_remove(pool, POOL_LOWPAGE);
	mutex_unlock(&pool->mutex);

	return page;
}

struct page *dynamic_page_pool_alloc(struct dynamic_page_pool *pool)
{
	struct page *page = NULL;

	if (!pool) {
		WARN_ON(!pool);
		return NULL;
	}

	page = dynamic_page_pool_fetch(pool);

	if (!page) {
		/* Try pulling from the deferred freelist */
		dynamic_page_pool_clean(pool);
		page = dynamic_page_pool_fetch(pool);
	}

	if (!page)
		page = dynamic_page_pool_alloc_pages(pool);

	return page;
}

void dynamic_page_pool_free(struct dynamic_page_pool *pool, struct page *page)
{
	if (pool->order != compound_order(page)) {
		WARN_ON(pool->order != compound_order(page));
		return;
	}

	dynamic_page_pool_add_dirty(pool, page);
	wake_up(&pool->waitqueue);
}

static int dynamic_page_pool_total(struct dynamic_page_pool *pool, bool high)
{
	int count = pool->count[POOL_LOWPAGE] + pool->count[POOL_LOWDEFERRED];

	if (high) {
		count += pool->count[POOL_HIGHPAGE];
		count += pool->count[POOL_HIGHDEFERRED];
	}
	return count << pool->order;
}

static int dynamic_page_pool_deferred_total(struct dynamic_page_pool *pool)
{
	int count;

	count = pool->count[POOL_LOWDEFERRED];
	count += pool->count[POOL_HIGHDEFERRED];
	return count << pool->order;
}

static void dynamic_page_pool_zero_and_add(struct dynamic_page_pool *pool,
					   struct page **pages, int num)
{
	void *addr;
	int p;

	/*
	 * Release the pool->mutex here as we may trigger
	 * the shinker and deadlock.
	 */
	mutex_unlock(&pool->mutex);

	addr = vmap(pages, num, VM_MAP, PAGE_KERNEL);
	if (!addr) {
		/* If there was an error, just free the pages */
		for (p = 0; p < num; p++)
			dynamic_page_pool_free_pages(pool, pages[p]);

		goto out;
	}
	memset(addr, 0, PAGE_SIZE * num);
	vunmap(addr);

	for (p = 0; p < num; p++)
		dynamic_page_pool_add_clean(pool, pages[p]);

out:
	/* Reaquire the lock */
	mutex_lock(&pool->mutex);

}

static void dynamic_page_pool_clean_pages(struct dynamic_page_pool *pool, int index)
{
	struct page *pages[32];
	int p;

	while (pool->count[index]) {
		pages[p++] = dynamic_page_pool_remove(pool, index);
		if (p == ARRAY_SIZE(pages)) {
			dynamic_page_pool_zero_and_add(pool, pages, p);
			p = 0;
		}
	}
	if (p)
		dynamic_page_pool_zero_and_add(pool, pages, p);
}

static void dynamic_page_pool_clean(struct dynamic_page_pool *pool)
{
	int passes = 4; /* This may need to be tuned */

	mutex_lock(&pool->mutex);
	while (passes--) {
		if (pool->count[POOL_HIGHDEFERRED])
			dynamic_page_pool_clean_pages(pool, POOL_HIGHDEFERRED);
		else if (pool->count[POOL_LOWDEFERRED])
			dynamic_page_pool_clean_pages(pool, POOL_LOWDEFERRED);
	}
	mutex_unlock(&pool->mutex);
}

static int dynamic_page_pool_deferred_free(void *data)
{
	struct dynamic_page_pool *pool = data;

	while (true) {
		wait_event_freezable(pool->waitqueue,
				     dynamic_page_pool_deferred_total(pool) > 0);

		dynamic_page_pool_clean(pool);
	}

	return 0;
}

struct dynamic_page_pool *dynamic_page_pool_create(gfp_t gfp_mask, unsigned int order)
{
	struct dynamic_page_pool *pool = kmalloc(sizeof(*pool), GFP_KERNEL);
	int i;

	if (!pool)
		return NULL;

	for (i = 0; i < POOL_TYPE_SIZE; i++) {
		pool->count[i] = 0;
		INIT_LIST_HEAD(&pool->items[i]);
	}
	pool->gfp_mask = gfp_mask | __GFP_COMP;
	pool->order = order;
	mutex_init(&pool->mutex);

	init_waitqueue_head(&pool->waitqueue);
	pool->task = kthread_run(dynamic_page_pool_deferred_free, pool,
				 "%s", "dynamic_page_pool_cleaner");
	if (IS_ERR(pool->task)) {
		pr_err("%s: creating thread for deferred free failed\n",
		       __func__);
		return NULL;
	}
	sched_set_normal(pool->task, 19);

	mutex_lock(&pool_list_lock);
	list_add(&pool->list, &pool_list);
	mutex_unlock(&pool_list_lock);

	return pool;
}

void dynamic_page_pool_destroy(struct dynamic_page_pool *pool)
{
	struct page *page;
	int i;

	/* Remove us from the pool list */
	mutex_lock(&pool_list_lock);
	list_del(&pool->list);
	mutex_unlock(&pool_list_lock);

	/* Free any remaining pages in the pool */
	mutex_lock(&pool->mutex);
	for (i = 0; i < POOL_TYPE_SIZE; i++) {
		while (pool->count[i]) {
			page = dynamic_page_pool_remove(pool, i);
			dynamic_page_pool_free_pages(pool, page);
		}
	}
	mutex_unlock(&pool->mutex);

	kfree(pool);
}

int dynamic_page_pool_do_shrink(struct dynamic_page_pool *pool, gfp_t gfp_mask,
				int nr_to_scan)
{
	int freed = 0;
	bool high;

	if (current_is_kswapd())
		high = true;
	else
		high = !!(gfp_mask & __GFP_HIGHMEM);

	if (nr_to_scan == 0)
		return dynamic_page_pool_total(pool, high);

	while (freed < nr_to_scan) {
		struct page *page;

		mutex_lock(&pool->mutex);
		/* Try to free un-cleaned pages first */
		if (pool->count[POOL_LOWDEFERRED]) {
			page = dynamic_page_pool_remove(pool, POOL_LOWDEFERRED);
		} else if (high && pool->count[POOL_HIGHDEFERRED]) {
			page = dynamic_page_pool_remove(pool, POOL_HIGHPAGE);
		} else if (pool->count[POOL_LOWPAGE]) {
			page = dynamic_page_pool_remove(pool, POOL_LOWPAGE);
		} else if (high && pool->count[POOL_HIGHPAGE]) {
			page = dynamic_page_pool_remove(pool, POOL_HIGHPAGE);
		} else {
			mutex_unlock(&pool->mutex);
			break;
		}
		mutex_unlock(&pool->mutex);
		dynamic_page_pool_free_pages(pool, page);
		freed += (1 << pool->order);
	}

	return freed;
}

static int dynamic_page_pool_shrink(gfp_t gfp_mask, int nr_to_scan)
{
	struct dynamic_page_pool *pool;
	int nr_total = 0;
	int nr_freed;
	int only_scan = 0;

	if (!nr_to_scan)
		only_scan = 1;

	mutex_lock(&pool_list_lock);
	list_for_each_entry(pool, &pool_list, list) {
		if (only_scan) {
			nr_total += dynamic_page_pool_do_shrink(pool,
								gfp_mask,
								nr_to_scan);
		} else {
			nr_freed = dynamic_page_pool_do_shrink(pool,
							       gfp_mask,
							       nr_to_scan);
			nr_to_scan -= nr_freed;
			nr_total += nr_freed;
			if (nr_to_scan <= 0)
				break;
		}
	}
	mutex_unlock(&pool_list_lock);

	return nr_total;
}

static unsigned long dynamic_page_pool_shrink_count(struct shrinker *shrinker,
						    struct shrink_control *sc)
{
	return dynamic_page_pool_shrink(sc->gfp_mask, 0);
}

static unsigned long dynamic_page_pool_shrink_scan(struct shrinker *shrinker,
						   struct shrink_control *sc)
{
	int to_scan = sc->nr_to_scan;

	if (to_scan == 0)
		return 0;

	return dynamic_page_pool_shrink(sc->gfp_mask, to_scan);
}

struct shrinker pool_shrinker = {
	.count_objects = dynamic_page_pool_shrink_count,
	.scan_objects = dynamic_page_pool_shrink_scan,
	.seeks = DEFAULT_SEEKS,
	.batch = 0,
};

int dynamic_page_pool_init_shrinker(void)
{
	return register_shrinker(&pool_shrinker);
}
device_initcall(dynamic_page_pool_init_shrinker);
