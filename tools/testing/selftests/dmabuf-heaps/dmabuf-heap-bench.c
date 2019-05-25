// SPDX-License-Identifier: GPL-2.0

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/dma-buf.h>
#include <drm/drm.h>

#include "ion.h"

#include "../../../../include/uapi/linux/dma-heap.h"

#define HEAP_DEVPATH "/dev/dma_heap"
#define ION_DEVPATH "/dev/ion"

#define ONE_MEG (1024*1024)
#define NUM_SIZES 5
int sizes[NUM_SIZES] = {1024, 4*1024, ONE_MEG, 8*ONE_MEG, 32*ONE_MEG };

#define NUM_ITERS 5000
#define NSEC_PER_SEC 1000000000LL
#define MAX_HEAP_COUNT        ION_HEAP_TYPE_CUSTOM

int ion_heap_open(void)
{
	int ret, fd;
	char buf[256];

	ret = sprintf(buf, "%s", ION_DEVPATH);
	if (ret < 0) {
		printf("sprintf failed!\n");
		return ret;
	}

	fd = open(buf, O_RDWR);
	if (fd < 0)
		printf("open %s failed!\n", buf);
	return fd;
}

int ion_heap_alloc(int ionfd, int heap_id, size_t len, unsigned int flags, int *ion_fd)
{
	struct ion_allocation_data alloc_data;
	int ret;

        alloc_data.heap_id_mask = 1 << heap_id;
        alloc_data.flags = flags;
        alloc_data.len = len;

        /* Allocate memory for this ION client as per heap_type */
        ret = ioctl(ionfd, ION_IOC_ALLOC, &alloc_data);

	*ion_fd =  alloc_data.fd;

	return ret;
}


int dmabuf_heap_open(char *name)
{
	int ret, fd;
	char buf[256];

	ret = sprintf(buf, "%s/%s", HEAP_DEVPATH, name);
	if (ret < 0) {
		printf("sprintf failed!\n");
		return ret;
	}

	fd = open(buf, O_RDWR);
	if (fd < 0)
		printf("open %s failed!\n", buf);
	return fd;
}


int dmabuf_heap_alloc(int fd, size_t len, unsigned int flags, int *dmabuf_fd)
{
	struct dma_heap_allocation_data data = {
		.len = len,
		.fd_flags = O_RDWR | O_CLOEXEC,
		.heap_flags = flags,
	};
	int ret;

	if (dmabuf_fd == NULL)
		return -EINVAL;

	ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &data);
	if (ret < 0)
		return ret;
	*dmabuf_fd = (int)data.fd;
	return ret;
}

void dmabuf_sync(int fd, int start_stop)
{
	struct dma_buf_sync sync = { 0 };
	int ret;

	sync.flags = start_stop | DMA_BUF_SYNC_RW;
	ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	if (ret)
		printf("sync failed %d\n", errno);

}

void ion_heap_bench(int heap_type, int size, int flags)
{
	int heap_id;
	int ionfd = -1, dmabuf_fd = -1;
	struct ion_heap_query query;
	struct ion_heap_data heap_data[MAX_HEAP_COUNT];
	struct timespec ts_start, ts_end;
	long long start, end;
	int ret;
	int i;

	ionfd = ion_heap_open();
	if (ionfd < 0)
		return;

	memset(&query, 0, sizeof(query));
	query.cnt = MAX_HEAP_COUNT;
	query.heaps = (unsigned long int)&heap_data[0];
	/* Query ION heap_id_mask from ION heap */
	ret = ioctl(ionfd, ION_IOC_HEAP_QUERY, &query);
	if (ret < 0) {
		printf("<%s>: Failed: ION_IOC_HEAP_QUERY: %s\n", __func__,
			strerror(errno));
		goto out;
	}
	heap_id = MAX_HEAP_COUNT + 1;
	for (i = 0; i < query.cnt; i++) {
		if (heap_data[i].type == heap_type) {
			heap_id = heap_data[i].heap_id;
			break;
		}
	}
	if (heap_id > MAX_HEAP_COUNT) {
		printf("<%s>: ERROR: heap type does not exists\n", __func__);
		goto out;
	}
	
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	for (i=0; i < NUM_ITERS; i++) {
		ret = ion_heap_alloc(ionfd, heap_id, size, flags, &dmabuf_fd);
		if (ret)
			goto out;
		close(dmabuf_fd);
	}
	clock_gettime(CLOCK_MONOTONIC, &ts_end);

	start = ts_start.tv_sec * NSEC_PER_SEC + ts_start.tv_nsec;
	end= ts_end.tv_sec * NSEC_PER_SEC + ts_end.tv_nsec;

	printf("ion heap:    alloc %d bytes %i times in %lld ns \t %lld ns/call\n", size, NUM_ITERS, end-start, (end-start)/NUM_ITERS);
out:
	if (ionfd >= 0)
		close(ionfd);
}


void dmabuf_heap_bench(char *heap_name, int size)
{
	int heap_fd = -1, dmabuf_fd = -1;
	struct timespec ts_start, ts_end;
	long long start, end;
	int ret;
	int i;


	heap_fd = dmabuf_heap_open(heap_name);
	if (heap_fd < 0)
		return;
	
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	for (i=0; i < NUM_ITERS; i++) {
		ret = dmabuf_heap_alloc(heap_fd, size, 0, &dmabuf_fd);
		if (ret)
			goto out;
		close(dmabuf_fd);
	}
	clock_gettime(CLOCK_MONOTONIC, &ts_end);

	start = ts_start.tv_sec * NSEC_PER_SEC + ts_start.tv_nsec;
	end= ts_end.tv_sec * NSEC_PER_SEC + ts_end.tv_nsec;

	printf("dmabuf heap: alloc %d bytes %i times in %lld ns \t %lld ns/call\n", size, NUM_ITERS, end-start, (end-start)/NUM_ITERS);
out:
	if (heap_fd >= 0)
		close(heap_fd);
}


int main(int argc, char* argv[])
{
	int i;
	if (argc < 2) {
		printf("Usage %s [<cma heap name>|none]\n", argv[0]);
		return -1;
	}

	printf("Testing dmabuf system vs ion system (cached):\n");
	printf("---------------------------------------------\n");
	for(i=0; i< NUM_SIZES; i++) {
		dmabuf_heap_bench("system", sizes[i]);
		ion_heap_bench(ION_HEAP_TYPE_SYSTEM, sizes[i], ION_FLAG_CACHED);
	}
	if (!strncmp(argv[1], "none",4))
		return 0;

	printf("\nTesting dmabuf %s vs ion CMA:\n", argv[1]);
	printf("---------------------------------------------\n");
	for(i=0; i< NUM_SIZES; i++) {
		dmabuf_heap_bench(argv[1], sizes[i]);
		ion_heap_bench(ION_HEAP_TYPE_DMA, sizes[i], 0);
	}

	return 0;
}
