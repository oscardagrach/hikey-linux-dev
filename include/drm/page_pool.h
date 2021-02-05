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

#ifndef _DRM_PAGE_POOL_H_
#define _DRM_PAGE_POOL_H_

#include <linux/mmzone.h>
#include <linux/llist.h>
#include <linux/spinlock.h>

struct drm_page_pool {
	int count;
	struct list_head items;

	int order;
	int (*free)(struct page *p, unsigned int order);

	spinlock_t lock;
	struct list_head list;
};

void drm_page_pool_set_max(unsigned long max);
int drm_page_pool_get_size(struct drm_page_pool *pool);
struct page *drm_page_pool_fetch(struct drm_page_pool *pool);
void drm_page_pool_add(struct drm_page_pool *pool, struct page *page);
struct drm_page_pool *drm_page_pool_create(unsigned int order,
					   int (*free_page)(struct page *p, unsigned int order));
void drm_page_pool_destroy(struct drm_page_pool *pool);

#endif
