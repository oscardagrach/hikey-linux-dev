// SPDX-License-Identifier: GPL-2.0
/*
 * Uncached DMA-Heap exporter
 *
 * Copyright (C) 2020 Linaro Ltd.
 *
 * Based off of Andrew Davis' SRAM heap:
 * Copyright (C) 2019 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>


struct uncached_heap {
	struct dma_heap *heap;
};

struct uncached_heap_buffer {
	struct dma_heap *heap;
	struct list_head attachments;
	struct mutex attachments_lock;
	unsigned long len;
	struct sg_table sg_table;

	void *vaddr;
	dma_addr_t dma_addr;
	unsigned long attr;
};

struct dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
};


static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sg(table->sgl, sg, table->nents, i) {
		memcpy(new_sg, sg, sizeof(*sg));
		new_sg->dma_address = 0;
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static int dma_heap_attach(struct dma_buf *dmabuf,
			   struct dma_buf_attachment *attachment)
{
	struct uncached_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct sg_table *table;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(&buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);

	attachment->priv = a;

	mutex_lock(&buffer->attachments_lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->attachments_lock);

	return 0;
}

static void dma_heap_detatch(struct dma_buf *dmabuf,
			     struct dma_buf_attachment *attachment)
{
	struct uncached_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	mutex_lock(&buffer->attachments_lock);
	list_del(&a->list);
	mutex_unlock(&buffer->attachments_lock);

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *dma_heap_map_dma_buf(struct dma_buf_attachment *attachment,
					     enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	struct sg_table *table = a->table;

	if (!dma_map_sg_attrs(attachment->dev, table->sgl, table->nents,
			      direction, DMA_ATTR_SKIP_CPU_SYNC))
		return ERR_PTR(-ENOMEM);

	return table;
}

static void dma_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
				   struct sg_table *table,
				   enum dma_data_direction direction)
{
	dma_unmap_sg_attrs(attachment->dev, table->sgl, table->nents,
			   direction, DMA_ATTR_SKIP_CPU_SYNC);
}

static void dma_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct uncached_heap_buffer *buffer = dmabuf->priv;

	dma_free_attrs(dma_heap_get_dev(buffer->heap), buffer->len,
			buffer->vaddr, buffer->dma_addr,
			buffer->attr);

	kfree(buffer);
}

static int dma_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct uncached_heap_buffer *buffer = dmabuf->priv;

	return dma_mmap_attrs(dma_heap_get_dev(buffer->heap), vma, buffer->vaddr,
			buffer->dma_addr, buffer->len, buffer->attr);
}

static void *dma_heap_vmap(struct dma_buf *dmabuf)
{
	struct uncached_heap_buffer *buffer = dmabuf->priv;

	return buffer->vaddr;
}

const struct dma_buf_ops uncached_heap_buf_ops = {
	.attach = dma_heap_attach,
	.detach = dma_heap_detatch,
	.map_dma_buf = dma_heap_map_dma_buf,
	.unmap_dma_buf = dma_heap_unmap_dma_buf,
	.release = dma_heap_dma_buf_release,
	.mmap = dma_heap_mmap,
	.vmap = dma_heap_vmap,
};

static int uncached_heap_allocate(struct dma_heap *heap,
				  unsigned long len,
				  unsigned long fd_flags,
				  unsigned long heap_flags)
{
	struct uncached_heap_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int ret;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->attachments_lock);
	buffer->heap = heap;
	buffer->len = len;
	buffer->attr = DMA_ATTR_WRITE_COMBINE|DMA_ATTR_SKIP_CPU_SYNC;
	buffer->vaddr = dma_alloc_attrs(dma_heap_get_dev(heap), buffer->len,
					&buffer->dma_addr, GFP_KERNEL,
					buffer->attr);
	if (!buffer->vaddr) {
		ret = -ENOMEM;
		goto free_buffer;
	}

	if (dma_get_sgtable_attrs(dma_heap_get_dev(heap), &buffer->sg_table,
				  buffer->vaddr, buffer->dma_addr, buffer->len,
				  buffer->attr)) {
		ret = -ENOMEM; /*??*/
		goto free_other;
	}

	/* create the dmabuf */
	exp_info.ops = &uncached_heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto free_other;
	}

	ret = dma_buf_fd(dmabuf, fd_flags);
	if (ret < 0) {
		dma_buf_put(dmabuf);
		/* just return, as put will call release and that will free */
		return ret;
	}

	return ret;

free_other:
	dma_free_attrs(dma_heap_get_dev(heap), buffer->len, buffer->vaddr, buffer->dma_addr,
			buffer->attr);

free_buffer:
	kfree(buffer);

	return ret;
}

static struct dma_heap_ops uncached_heap_ops = {
	.allocate = uncached_heap_allocate,
};

static int uncached_heap_create(void)
{
	struct uncached_heap *heap;
	struct dma_heap_export_info exp_info;


	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return -ENOMEM;

	/* Initialize any custom my_dma_heap structure values here */

	exp_info.name = "uncached";
	exp_info.ops = &uncached_heap_ops;
	exp_info.priv = heap;
	heap->heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap->heap)) {
		int ret = PTR_ERR(heap->heap);
		kfree(heap);
		return ret;
	}

	dma_set_mask(dma_heap_get_dev(heap->heap), DMA_BIT_MASK(32));

	return 0;
}
device_initcall(uncached_heap_create);

