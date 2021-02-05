// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Christian König
 */

/* Pooling of allocated pages is necessary because changing the caching
 * attributes on x86 of the linear mapping requires a costly cross CPU TLB
 * invalidate for those addresses.
 *
 * Additional to that allocations from the DMA coherent API are pooled as well
 * cause they are rather slow compared to alloc_pages+map.
 */

#include <linux/module.h>
#include <linux/dma-mapping.h>

#ifdef CONFIG_X86
#include <asm/set_memory.h>
#endif
#include <drm/page_pool.h>
#include <drm/ttm/ttm_pool.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_tt.h>

/**
 * struct ttm_pool_page_dat - Helper object for coherent DMA mappings
 *
 * @pool: ttm_pool pointer the page was allocated by
 * @caching: the caching value the allocated page was configured for
 * @addr: original DMA address returned for the mapping
 * @vaddr: original vaddr return for the mapping and order in the lower bits
 */
struct ttm_pool_page_dat {
	struct ttm_pool *pool;
	enum ttm_caching caching;
	dma_addr_t addr;
	unsigned long vaddr;
};

static unsigned long page_pool_size;

MODULE_PARM_DESC(page_pool_size, "Number of pages in the WC/UC/DMA pool");
module_param(page_pool_size, ulong, 0644);

static atomic_long_t allocated_pages;

static struct drm_page_pool *global_write_combined[MAX_ORDER];
static struct drm_page_pool *global_uncached[MAX_ORDER];

static struct drm_page_pool *global_dma32_write_combined[MAX_ORDER];
static struct drm_page_pool *global_dma32_uncached[MAX_ORDER];

static struct mutex shrinker_lock;

/* Allocate pages of size 1 << order with the given gfp_flags */
static struct page *ttm_pool_alloc_page(struct ttm_pool *pool, gfp_t gfp_flags,
					unsigned int order, enum ttm_caching caching)
{
	unsigned long attr = DMA_ATTR_FORCE_CONTIGUOUS;
	struct ttm_pool_page_dat *dat;
	struct page *p;
	void *vaddr;

	dat = kmalloc(sizeof(*dat), GFP_KERNEL);
	if (!dat)
		return NULL;

	dat->pool = pool;
	dat->caching = caching;

	/* Don't set the __GFP_COMP flag for higher order allocations.
	 * Mapping pages directly into an userspace process and calling
	 * put_page() on a TTM allocated page is illegal.
	 */
	if (order)
		gfp_flags |= __GFP_NOMEMALLOC | __GFP_NORETRY |
			__GFP_KSWAPD_RECLAIM;

	if (!pool->use_dma_alloc) {
		p = alloc_pages(gfp_flags, order);
		if (!p)
			goto error_free;
		dat->vaddr = order;
		p->private = (unsigned long)dat;
		return p;
	}

	if (order)
		attr |= DMA_ATTR_NO_WARN;

	vaddr = dma_alloc_attrs(pool->dev, (1ULL << order) * PAGE_SIZE,
				&dat->addr, gfp_flags, attr);
	if (!vaddr)
		goto error_free;

	/* TODO: This is an illegal abuse of the DMA API, but we need to rework
	 * TTM page fault handling and extend the DMA API to clean this up.
	 */
	if (is_vmalloc_addr(vaddr))
		p = vmalloc_to_page(vaddr);
	else
		p = virt_to_page(vaddr);

	dat->vaddr = (unsigned long)vaddr | order;
	p->private = (unsigned long)dat;
	return p;

error_free:
	kfree(dat);
	return NULL;
}

/* Reset the caching and pages of size 1 << order */
static int ttm_pool_free_page(struct page *p, unsigned int order)
{
	unsigned long attr = DMA_ATTR_FORCE_CONTIGUOUS;
	struct ttm_pool_page_dat *dat = (void *)p->private;
	void *vaddr;

#ifdef CONFIG_X86
	/* We don't care that set_pages_wb is inefficient here. This is only
	 * used when we have to shrink and CPU overhead is irrelevant then.
	 */
	if (dat->caching != ttm_cached && !PageHighMem(p))
		set_pages_wb(p, 1 << order);
#endif

	if (!dat->pool || !dat->pool->use_dma_alloc) {
		__free_pages(p, order);
		goto out;
	}

	if (order)
		attr |= DMA_ATTR_NO_WARN;

	vaddr = (void *)(dat->vaddr & PAGE_MASK);
	dma_free_attrs(dat->pool->dev, (1UL << order) * PAGE_SIZE, vaddr, dat->addr,
		       attr);
out:
	kfree(dat);
	return 1 << order;
}

/* Apply a new caching to an array of pages */
static int ttm_pool_apply_caching(struct page **first, struct page **last,
				  enum ttm_caching caching)
{
#ifdef CONFIG_X86
	unsigned int num_pages = last - first;

	if (!num_pages)
		return 0;

	switch (caching) {
	case ttm_cached:
		break;
	case ttm_write_combined:
		return set_pages_array_wc(first, num_pages);
	case ttm_uncached:
		return set_pages_array_uc(first, num_pages);
	}
#endif
	return 0;
}

/* Map pages of 1 << order size and fill the DMA address array  */
static int ttm_pool_map(struct ttm_pool *pool, unsigned int order,
			struct page *p, dma_addr_t **dma_addr)
{
	dma_addr_t addr;
	unsigned int i;

	if (pool->use_dma_alloc) {
		struct ttm_pool_page_dat *dat = (void *)p->private;

		addr = dat->addr;
	} else {
		size_t size = (1ULL << order) * PAGE_SIZE;

		addr = dma_map_page(pool->dev, p, 0, size, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(pool->dev, addr))
			return -EFAULT;
	}

	for (i = 1 << order; i ; --i) {
		*(*dma_addr)++ = addr;
		addr += PAGE_SIZE;
	}

	return 0;
}

/* Unmap pages of 1 << order size */
static void ttm_pool_unmap(struct ttm_pool *pool, dma_addr_t dma_addr,
			   unsigned int num_pages)
{
	/* Unmapped while freeing the page */
	if (pool->use_dma_alloc)
		return;

	dma_unmap_page(pool->dev, dma_addr, (long)num_pages << PAGE_SHIFT,
		       DMA_BIDIRECTIONAL);
}

/* Return the pool_type to use for the given caching and order */
static struct drm_page_pool *ttm_pool_select_type(struct ttm_pool *pool,
						  enum ttm_caching caching,
						  unsigned int order)
{
	if (pool->use_dma_alloc)
		return pool->caching[caching].orders[order];

#ifdef CONFIG_X86
	switch (caching) {
	case ttm_write_combined:
		if (pool->use_dma32)
			return global_dma32_write_combined[order];

		return global_write_combined[order];
	case ttm_uncached:
		if (pool->use_dma32)
			return global_dma32_uncached[order];

		return global_uncached[order];
	default:
		break;
	}
#endif

	return NULL;
}

/* Free pages using the global shrinker list */
static unsigned int ttm_pool_shrink(void)
{
	struct drm_page_pool *pt;
	unsigned int num_freed;
#if 0
	struct page *p;

	mutex_lock(&shrinker_lock);
	pt = list_first_entry(&shrinker_list, typeof(*pt), shrinker_list);

	p = ttm_pool_type_take(pt);
	if (p) {
		ttm_pool_free_page(p, pt->order);
		num_freed = 1 << pt->order;
	} else {
		num_freed = 0;
	}

	list_move_tail(&pt->shrinker_list, &shrinker_list);
	mutex_unlock(&shrinker_lock);
#endif
	return num_freed;
}

/* Return the allocation order based for a page */
static unsigned int ttm_pool_page_order(struct ttm_pool *pool, struct page *p)
{
	struct ttm_pool_page_dat *dat = (void *)p->private;

	return dat->vaddr & ~PAGE_MASK;
}

/**
 * ttm_pool_alloc - Fill a ttm_tt object
 *
 * @pool: ttm_pool to use
 * @tt: ttm_tt object to fill
 * @ctx: operation context
 *
 * Fill the ttm_tt object with pages and also make sure to DMA map them when
 * necessary.
 *
 * Returns: 0 on successe, negative error code otherwise.
 */
int ttm_pool_alloc(struct ttm_pool *pool, struct ttm_tt *tt,
		   struct ttm_operation_ctx *ctx)
{
	unsigned long num_pages = tt->num_pages;
	dma_addr_t *dma_addr = tt->dma_address;
	struct page **caching = tt->pages;
	struct page **pages = tt->pages;
	gfp_t gfp_flags = GFP_USER;
	unsigned int i, order;
	struct page *p;
	int r;

	WARN_ON(!num_pages || ttm_tt_is_populated(tt));
	WARN_ON(dma_addr && !pool->dev);

	if (tt->page_flags & TTM_PAGE_FLAG_ZERO_ALLOC)
		gfp_flags |= __GFP_ZERO;

	if (ctx->gfp_retry_mayfail)
		gfp_flags |= __GFP_RETRY_MAYFAIL;

	if (pool->use_dma32)
		gfp_flags |= GFP_DMA32;
	else
		gfp_flags |= GFP_HIGHUSER;

	for (order = min(MAX_ORDER - 1UL, __fls(num_pages)); num_pages;
	     order = min_t(unsigned int, order, __fls(num_pages))) {
		bool apply_caching = false;
		struct drm_page_pool *pt;

		pt = ttm_pool_select_type(pool, tt->caching, order);
		p = pt ? drm_page_pool_fetch(pt) : NULL;
		if (p) {
			apply_caching = true;
		} else {
			p = ttm_pool_alloc_page(pool, gfp_flags, order, tt->caching);
			if (p && PageHighMem(p))
				apply_caching = true;
		}

		if (!p) {
			if (order) {
				--order;
				continue;
			}
			r = -ENOMEM;
			goto error_free_all;
		}

		if (apply_caching) {
			r = ttm_pool_apply_caching(caching, pages,
						   tt->caching);
			if (r)
				goto error_free_page;
			caching = pages + (1 << order);
		}

		r = ttm_mem_global_alloc_page(&ttm_mem_glob, p,
					      (1 << order) * PAGE_SIZE,
					      ctx);
		if (r)
			goto error_free_page;

		if (dma_addr) {
			r = ttm_pool_map(pool, order, p, &dma_addr);
			if (r)
				goto error_global_free;
		}

		num_pages -= 1 << order;
		for (i = 1 << order; i; --i)
			*(pages++) = p++;
	}

	r = ttm_pool_apply_caching(caching, pages, tt->caching);
	if (r)
		goto error_free_all;

	return 0;

error_global_free:
	ttm_mem_global_free_page(&ttm_mem_glob, p, (1 << order) * PAGE_SIZE);

error_free_page:
	ttm_pool_free_page(p, order);

error_free_all:
	num_pages = tt->num_pages - num_pages;
	for (i = 0; i < num_pages; ) {
		order = ttm_pool_page_order(pool, tt->pages[i]);
		ttm_pool_free_page(tt->pages[i], order);
		i += 1 << order;
	}

	return r;
}
EXPORT_SYMBOL(ttm_pool_alloc);

/**
 * ttm_pool_free - Free the backing pages from a ttm_tt object
 *
 * @pool: Pool to give pages back to.
 * @tt: ttm_tt object to unpopulate
 *
 * Give the packing pages back to a pool or free them
 */
void ttm_pool_free(struct ttm_pool *pool, struct ttm_tt *tt)
{
	unsigned int i;

	for (i = 0; i < tt->num_pages; ) {
		struct page *p = tt->pages[i];
		unsigned int order, num_pages;
		struct drm_page_pool *pt;

		order = ttm_pool_page_order(pool, p);
		num_pages = 1ULL << order;
		ttm_mem_global_free_page(&ttm_mem_glob, p,
					 num_pages * PAGE_SIZE);
		if (tt->dma_address)
			ttm_pool_unmap(pool, tt->dma_address[i], num_pages);

		pt = ttm_pool_select_type(pool, tt->caching, order);
		if (pt)
			drm_page_pool_add(pt, tt->pages[i]);
		else
			ttm_pool_free_page(tt->pages[i], order);

		i += num_pages;
	}

	while (atomic_long_read(&allocated_pages) > page_pool_size)
		ttm_pool_shrink();
}
EXPORT_SYMBOL(ttm_pool_free);

/**
 * ttm_pool_init - Initialize a pool
 *
 * @pool: the pool to initialize
 * @dev: device for DMA allocations and mappings
 * @use_dma_alloc: true if coherent DMA alloc should be used
 * @use_dma32: true if GFP_DMA32 should be used
 *
 * Initialize the pool and its pool types.
 */
void ttm_pool_init(struct ttm_pool *pool, struct device *dev,
		   bool use_dma_alloc, bool use_dma32)
{
	unsigned int i, j;

	WARN_ON(!dev && use_dma_alloc);

	pool->dev = dev;
	pool->use_dma_alloc = use_dma_alloc;
	pool->use_dma32 = use_dma32;

	for (i = 0; i < TTM_NUM_CACHING_TYPES; ++i)
		for (j = 0; j < MAX_ORDER; ++j)
			pool->caching[i].orders[j] = drm_page_pool_create(j,
							ttm_pool_free_page);
}

/**
 * ttm_pool_fini - Cleanup a pool
 *
 * @pool: the pool to clean up
 *
 * Free all pages in the pool and unregister the types from the global
 * shrinker.
 */
void ttm_pool_fini(struct ttm_pool *pool)
{
	unsigned int i, j;

	for (i = 0; i < TTM_NUM_CACHING_TYPES; ++i)
		for (j = 0; j < MAX_ORDER; ++j)
			drm_page_pool_destroy(pool->caching[i].orders[j]);
}

#ifdef CONFIG_DEBUG_FS
/* Dump information about the different pool types */
static void ttm_pool_debugfs_orders(struct drm_page_pool **pt,
				    struct seq_file *m)
{
	unsigned int i;

	for (i = 0; i < MAX_ORDER; ++i)
		seq_printf(m, " %8u", drm_page_pool_get_size(pt[i]));
	seq_puts(m, "\n");
}

/**
 * ttm_pool_debugfs - Debugfs dump function for a pool
 *
 * @pool: the pool to dump the information for
 * @m: seq_file to dump to
 *
 * Make a debugfs dump with the per pool and global information.
 */
int ttm_pool_debugfs(struct ttm_pool *pool, struct seq_file *m)
{
	unsigned int i;

	mutex_lock(&shrinker_lock);

	seq_puts(m, "\t ");
	for (i = 0; i < MAX_ORDER; ++i)
		seq_printf(m, " ---%2u---", i);
	seq_puts(m, "\n");

	seq_puts(m, "wc\t:");
	ttm_pool_debugfs_orders(global_write_combined, m);
	seq_puts(m, "uc\t:");
	ttm_pool_debugfs_orders(global_uncached, m);

	seq_puts(m, "wc 32\t:");
	ttm_pool_debugfs_orders(global_dma32_write_combined, m);
	seq_puts(m, "uc 32\t:");
	ttm_pool_debugfs_orders(global_dma32_uncached, m);

	for (i = 0; i < TTM_NUM_CACHING_TYPES; ++i) {
		seq_puts(m, "DMA ");
		switch (i) {
		case ttm_cached:
			seq_puts(m, "\t:");
			break;
		case ttm_write_combined:
			seq_puts(m, "wc\t:");
			break;
		case ttm_uncached:
			seq_puts(m, "uc\t:");
			break;
		}
		ttm_pool_debugfs_orders(pool->caching[i].orders, m);
	}

	seq_printf(m, "\ntotal\t: %8lu of %8lu\n",
		   atomic_long_read(&allocated_pages), page_pool_size);

	mutex_unlock(&shrinker_lock);

	return 0;
}
EXPORT_SYMBOL(ttm_pool_debugfs);

#endif

/**
 * ttm_pool_mgr_init - Initialize globals
 *
 * @num_pages: default number of pages
 *
 * Initialize the global locks and lists for the MM shrinker.
 */
int ttm_pool_mgr_init(unsigned long num_pages)
{
	unsigned int i;

	if (!page_pool_size)
		page_pool_size = num_pages;

	mutex_init(&shrinker_lock);

	for (i = 0; i < MAX_ORDER; ++i) {
		global_write_combined[i] = drm_page_pool_create(i,
							ttm_pool_free_page);
		global_uncached[i] = drm_page_pool_create(i,
							ttm_pool_free_page);
		global_dma32_write_combined[i] = drm_page_pool_create(i,
							ttm_pool_free_page);
		global_dma32_uncached[i] = drm_page_pool_create(i,
							ttm_pool_free_page);
	}
	return 0;
}

/**
 * ttm_pool_mgr_fini - Finalize globals
 *
 * Cleanup the global pools and unregister the MM shrinker.
 */
void ttm_pool_mgr_fini(void)
{
	unsigned int i;

	for (i = 0; i < MAX_ORDER; ++i) {
		drm_page_pool_destroy(global_write_combined[i]);
		drm_page_pool_destroy(global_uncached[i]);

		drm_page_pool_destroy(global_dma32_write_combined[i]);
		drm_page_pool_destroy(global_dma32_uncached[i]);
	}
}
