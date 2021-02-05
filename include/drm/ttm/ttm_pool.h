/* SPDX-License-Identifier: GPL-2.0 OR MIT */
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

#ifndef _TTM_PAGE_POOL_H_
#define _TTM_PAGE_POOL_H_

#include <linux/mmzone.h>
#include <linux/llist.h>
#include <linux/spinlock.h>
#include <drm/ttm/ttm_caching.h>

struct device;
struct ttm_tt;
struct ttm_pool;
struct ttm_operation_ctx;

/**
 * ttm_pool - Pool for all caching and orders
 *
 * @use_dma_alloc: if coherent DMA allocations should be used
 * @use_dma32: if GFP_DMA32 should be used
 * @caching: pools for each caching/order
 */
struct ttm_pool {
	struct device *dev;

	bool use_dma_alloc;
	bool use_dma32;

	struct {
		struct drm_page_pool *orders[MAX_ORDER];
	} caching[TTM_NUM_CACHING_TYPES];
};

int ttm_pool_alloc(struct ttm_pool *pool, struct ttm_tt *tt,
		   struct ttm_operation_ctx *ctx);
void ttm_pool_free(struct ttm_pool *pool, struct ttm_tt *tt);

void ttm_pool_init(struct ttm_pool *pool, struct device *dev,
		   bool use_dma_alloc, bool use_dma32);
void ttm_pool_fini(struct ttm_pool *pool);

int ttm_pool_debugfs(struct ttm_pool *pool, struct seq_file *m);

int ttm_pool_mgr_init(unsigned long num_pages);
void ttm_pool_mgr_fini(void);

#endif
