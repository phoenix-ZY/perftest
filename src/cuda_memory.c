/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2023 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "cuda_memory.h"
#include "perftest_parameters.h"
#include CUDA_PATH

#define CUCHECK(stmt) \
	do { \
	CUresult result = (stmt); \
	ASSERT(CUDA_SUCCESS == result); \
} while (0)

#define ACCEL_PAGE_SIZE (64 * 1024)


struct cuda_memory_ctx {
	struct memory_ctx base;
	int device_id;
	char *device_bus_id;
	CUdevice cuDevice;
	CUcontext cuContext;
	bool use_dmabuf;
};


static int init_gpu(struct cuda_memory_ctx *ctx)
{
	int cuda_device_id = ctx->device_id;
	int cuda_pci_bus_id;
	int cuda_pci_device_id;
	int index;
	CUdevice cu_device;

	printf("initializing CUDA\n");
	CUresult error = cuInit(0);
	if (error != CUDA_SUCCESS) {
		printf("cuInit(0) returned %d\n", error);
		return FAILURE;
	}

	int deviceCount = 0;
	error = cuDeviceGetCount(&deviceCount);
	if (error != CUDA_SUCCESS) {
		printf("cuDeviceGetCount() returned %d\n", error);
		return FAILURE;
	}
	/* This function call returns 0 if there are no CUDA capable devices. */
	if (deviceCount == 0) {
		printf("There are no available device(s) that support CUDA\n");
		return FAILURE;
	}
	if (cuda_device_id >= deviceCount) {
		fprintf(stderr, "No such device ID (%d) exists in system\n", cuda_device_id);
		return FAILURE;
	}

	printf("Listing all CUDA devices in system:\n");
	for (index = 0; index < deviceCount; index++) {
		CUCHECK(cuDeviceGet(&cu_device, index));
		cuDeviceGetAttribute(&cuda_pci_bus_id, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID , cu_device);
		cuDeviceGetAttribute(&cuda_pci_device_id, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID , cu_device);
		printf("CUDA device %d: PCIe address is %02X:%02X\n", index, (unsigned int)cuda_pci_bus_id, (unsigned int)cuda_pci_device_id);
	}

	printf("\nPicking device No. %d\n", cuda_device_id);

	CUCHECK(cuDeviceGet(&ctx->cuDevice, cuda_device_id));

	char name[128];
	CUCHECK(cuDeviceGetName(name, sizeof(name), cuda_device_id));
	printf("[pid = %d, dev = %d] device name = [%s]\n", getpid(), ctx->cuDevice, name);
	printf("creating CUDA Ctx\n");

	/* Create context */
	error = cuCtxCreate(&ctx->cuContext, CU_CTX_MAP_HOST, ctx->cuDevice);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxCreate() error=%d\n", error);
		return FAILURE;
	}

	printf("making it the current CUDA Ctx\n");
	error = cuCtxSetCurrent(ctx->cuContext);
	if (error != CUDA_SUCCESS) {
		printf("cuCtxSetCurrent() error=%d\n", error);
		return FAILURE;
	}

	return SUCCESS;
}

static void free_gpu(struct cuda_memory_ctx *ctx)
{
	printf("destroying current CUDA Ctx\n");
	CUCHECK(cuCtxDestroy(ctx->cuContext));
}

int cuda_memory_init(struct memory_ctx *ctx) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);
	int return_value = 0;

	if (cuda_ctx->device_bus_id) {
		int err;

		printf("initializing CUDA\n");
		CUresult error = cuInit(0);
		if (error != CUDA_SUCCESS) {
			printf("cuInit(0) returned %d\n", error);
			return FAILURE;
		}

		printf("Finding PCIe BUS %s\n", cuda_ctx->device_bus_id);
		err = cuDeviceGetByPCIBusId(&cuda_ctx->device_id, cuda_ctx->device_bus_id);
		if (err != 0) {
			fprintf(stderr, "We have an error from cuDeviceGetByPCIBusId: %d\n", err);
		}
		printf("Picking GPU number %d\n", cuda_ctx->device_id);
	}

	return_value = init_gpu(cuda_ctx);
	if (return_value) {
		fprintf(stderr, "Couldn't init GPU context: %d\n", return_value);
		return FAILURE;
	}

#ifdef HAVE_CUDA_DMABUF
	if (cuda_ctx->use_dmabuf) {
		int is_supported = 0;

		CUCHECK(cuDeviceGetAttribute(&is_supported, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, cuda_ctx->cuDevice));
		if (!is_supported) {
			fprintf(stderr, "DMA-BUF is not supported on this GPU\n");
			return FAILURE;
		}
	}
#endif

	return SUCCESS;
}

int cuda_memory_destroy(struct memory_ctx *ctx) {
	struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

	free_gpu(cuda_ctx);
	free(cuda_ctx);
	return SUCCESS;
}

int cuda_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size, int *dmabuf_fd,
				uint64_t *dmabuf_offset,  void **addr, bool *can_init) {
	float *x;

	CUdeviceptr d_A;
	int error;
	size_t buf_size = (size + ACCEL_PAGE_SIZE - 1) & ~(ACCEL_PAGE_SIZE - 1);

	// x = (uint64_t *)malloc(buf_size);
	// x[0] = 100;
	// x[1] = 200;
	// x[2] = 300;
	// x[3] = 400;
	x = (float *)malloc(buf_size);
	x[0] = 10.0;
	x[1] = 20.0;
	x[2] = 30.0;

	void *h_A;

	printf("cuMemAllocHost() of a %lu bytes GPU buffer\n", size);

	error = cuMemAllocHost(&h_A, buf_size);

	if (error != CUDA_SUCCESS)
	{
		printf("cuMemAllocHost error=%d\n", error);
		return FAILURE;
	}
	error = cuMemHostGetDevicePointer(&d_A, h_A, 0);
	if (error != CUDA_SUCCESS)
	{
		printf("cuMemHostGetDevicePointer error=%d\n", error);
		return FAILURE;
	}
	printf("allocated GPU buffer address at %016llx pointer=%p\n", d_A, (void *)d_A);

	cuMemcpy((CUdeviceptr)d_A, (CUdeviceptr)x, buf_size);
	free(x);
	*addr = (void *)d_A;
	*can_init = false;

#ifdef HAVE_CUDA_DMABUF
	{
		struct cuda_memory_ctx *cuda_ctx = container_of(ctx, struct cuda_memory_ctx, base);

		if (cuda_ctx->use_dmabuf) {
			CUdeviceptr aligned_ptr;
			const size_t host_page_size = sysconf(_SC_PAGESIZE);
			uint64_t offset;
			size_t aligned_size;

			// Round down to host page size
			aligned_ptr = d_A & ~(host_page_size - 1);
			offset = d_A - aligned_ptr;
			aligned_size = (size + offset + host_page_size - 1) & ~(host_page_size - 1);

			printf("using DMA-BUF for GPU buffer address at %#llx aligned at %#llx with aligned size %zu\n", d_A, aligned_ptr, aligned_size);
			*dmabuf_fd = 0;
			error = cuMemGetHandleForAddressRange((void *)dmabuf_fd, aligned_ptr, aligned_size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
			if (error != CUDA_SUCCESS) {
				printf("cuMemGetHandleForAddressRange error=%d\n", error);
				return FAILURE;
			}

			*dmabuf_offset = offset;
		}
	}
#endif

	return SUCCESS;
}

int cuda_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr, uint64_t size) {
	CUdeviceptr d_A = (CUdeviceptr)addr;

	printf("deallocating RX GPU buffer %016llx\n", d_A);
	cuMemFree(d_A);
	return SUCCESS;
}

void *cuda_memory_copy_host_buffer(void *dest, const void *src, size_t size) {
	cuMemcpy((CUdeviceptr)dest, (CUdeviceptr)src, size);
	return dest;
}

void *cuda_memory_copy_buffer_to_buffer(void *dest, const void *src, size_t size) {
	cuMemcpyDtoD((CUdeviceptr)dest, (CUdeviceptr)src, size);
	return dest;
}

bool cuda_memory_supported() {
	return true;
}

bool cuda_memory_dmabuf_supported() {
#ifdef HAVE_CUDA_DMABUF
	return true;
#else
	return false;
#endif
}

struct memory_ctx *cuda_memory_create(struct perftest_parameters *params) {
	struct cuda_memory_ctx *ctx;

	ALLOCATE(ctx, struct cuda_memory_ctx, 1);
	ctx->base.init = cuda_memory_init;
	ctx->base.destroy = cuda_memory_destroy;
	ctx->base.allocate_buffer = cuda_memory_allocate_buffer;
	ctx->base.free_buffer = cuda_memory_free_buffer;
	ctx->base.copy_host_to_buffer = cuda_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_host = cuda_memory_copy_host_buffer;
	ctx->base.copy_buffer_to_buffer = cuda_memory_copy_buffer_to_buffer;
	ctx->device_id = params->cuda_device_id;
	ctx->device_bus_id = params->cuda_device_bus_id;
	ctx->use_dmabuf = params->use_cuda_dmabuf;

	return &ctx->base;
}
