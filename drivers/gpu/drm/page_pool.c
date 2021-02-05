// SPDX-License-Identifier: GPL-2.0
/*
 * DMA BUF page pool system
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
#include <drm/page_pool.h>

static LIST_HEAD(pool_list);
static DEFINE_MUTEX(pool_list_lock);
static atomic_long_t total_pages;

int drm_page_pool_get_size(struct drm_page_pool *pool)
{
	int ret;

	spin_lock(&pool->lock);
	ret = pool->count;
	spin_unlock(&pool->lock);
	return ret;
}

static inline unsigned int drm_page_pool_free_pages(struct drm_page_pool *pool,
						    struct page *page)
{
	return pool->free(page, pool->order);
}

void drm_page_pool_add(struct drm_page_pool *pool, struct page *page)
{
	spin_lock(&pool->lock);
	list_add_tail(&page->lru, &pool->items);
	pool->count++;
	atomic_long_add(1 << pool->order, &total_pages);
	spin_unlock(&pool->lock);

/*
	mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE,
			    1 << pool->order);
*/
}
EXPORT_SYMBOL_GPL(drm_page_pool_add);

static struct page *drm_page_pool_remove(struct drm_page_pool *pool)
{
	struct page *page;

	if (!pool->count)
		return NULL;

	page = list_first_entry(&pool->items, struct page, lru);
	pool->count--;
	atomic_long_sub(1 << pool->order, &total_pages);

	list_del(&page->lru);
/*
	mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE,
			    -(1 << pool->order));
*/
	return page;
}

struct page *drm_page_pool_fetch(struct drm_page_pool *pool)
{
	struct page *page = NULL;

	if (!pool) {
		WARN_ON(!pool);
		return NULL;
	}

	spin_lock(&pool->lock);
	page = drm_page_pool_remove(pool);
	spin_unlock(&pool->lock);

	return page;
}
EXPORT_SYMBOL_GPL(drm_page_pool_fetch);

struct drm_page_pool *drm_page_pool_create(unsigned int order,
					   int (*free_page)(struct page *p, unsigned int order))
{
	struct drm_page_pool *pool = kmalloc(sizeof(*pool), GFP_KERNEL);

	if (!pool)
		return NULL;

	pool->count = 0;
	INIT_LIST_HEAD(&pool->items);
	pool->order = order;
	pool->free = free_page;
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->list);

	mutex_lock(&pool_list_lock);
	list_add(&pool->list, &pool_list);
	mutex_unlock(&pool_list_lock);

	return pool;
}
EXPORT_SYMBOL_GPL(drm_page_pool_create);

void drm_page_pool_destroy(struct drm_page_pool *pool)
{
	struct page *page;

	/* Remove us from the pool list */
	mutex_lock(&pool_list_lock);
	list_del(&pool->list);
	mutex_unlock(&pool_list_lock);

	/* Free any remaining pages in the pool */
	spin_lock(&pool->lock);
	while (pool->count) {
		page = drm_page_pool_remove(pool);
		spin_unlock(&pool->lock);
		drm_page_pool_free_pages(pool, page);
		spin_lock(&pool->lock);
	}
	spin_unlock(&pool->lock);

	kfree(pool);
}
EXPORT_SYMBOL_GPL(drm_page_pool_destroy);

static int drm_page_pool_shrink_one(void)
{
	struct drm_page_pool *pool;
	struct page *page;
	int nr_freed = 0;

	mutex_lock(&pool_list_lock);
	pool = list_first_entry(&pool_list, typeof(*pool), list);

	spin_lock(&pool->lock);
	page = drm_page_pool_remove(pool);
	spin_unlock(&pool->lock);

	if (page)
		nr_freed = drm_page_pool_free_pages(pool, page);

	list_move_tail(&pool->list, &pool_list);
	mutex_unlock(&pool_list_lock);

	return nr_freed;
}

static unsigned long drm_page_pool_shrink_count(struct shrinker *shrinker,
						struct shrink_control *sc)
{
	unsigned long count =  atomic_long_read(&total_pages);

	return count ? count : SHRINK_EMPTY;
}

static unsigned long drm_page_pool_shrink_scan(struct shrinker *shrinker,
					       struct shrink_control *sc)
{
	int to_scan = sc->nr_to_scan;
	int nr_total = 0;

	if (to_scan == 0)
		return 0;

	do {
		int nr_freed = drm_page_pool_shrink_one();

		to_scan -= nr_freed;
		nr_total += nr_freed;
	} while (to_scan >= 0 && atomic_long_read(&total_pages));

	return nr_total;
}

static struct shrinker pool_shrinker = {
	.count_objects = drm_page_pool_shrink_count,
	.scan_objects = drm_page_pool_shrink_scan,
	.seeks = 1,
	.batch = 0,
};

int drm_page_pool_init_shrinker(void)
{
	return register_shrinker(&pool_shrinker);
}
module_init(drm_page_pool_init_shrinker);
MODULE_LICENSE("GPL v2");
