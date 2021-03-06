/*
 * Copyright © 2012, 2013 Thierry Reding
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
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#include <sys/mman.h>

#include <xf86drm.h>

#include <tegra_drm.h>

#include "private.h"

static void drm_tegra_bo_free(struct drm_tegra_bo *bo)
{
	struct drm_tegra *drm = bo->drm;
	struct drm_gem_close args;

	DRMLISTDEL(&bo->list);

	if (bo->map)
		munmap(bo->map, bo->size);

	memset(&args, 0, sizeof(args));
	args.handle = bo->handle;

	TRACE_IOCTL("gem_close: %x\n", args.handle);

	drmIoctl(drm->fd, DRM_IOCTL_GEM_CLOSE, &args);

	free(bo);
}

int drm_tegra_open(int fd, struct drm_tegra **drmp)
{
	struct drm_tegra *drm;
	int err;

	if (fd < 0 || !drmp)
		return -EINVAL;

	drm = calloc(1, sizeof(*drm));
	if (!drm)
		return -ENOMEM;

	DRMINITLISTHEAD(&drm->bo_list);
	drm->fd = fd;

	*drmp = drm;

	return 0;
}

void drm_tegra_close(struct drm_tegra *drm)
{
	if (drm)
		free(drm);
}

int drm_tegra_submit(struct drm_tegra *drm, struct host1x_job *job,
		     struct host1x_fence **fencep)
{
	struct drm_tegra_reloc *relocs, *reloc;
	unsigned int i, j, num_relocs = 0;
	struct drm_tegra_cmdbuf *cmdbufs;
	struct drm_tegra_syncpt syncpt;
	struct drm_tegra_submit args;
	unsigned int increments = 0;
	struct host1x_fence *fence;
	int err;

	if (!drm || !job || !fencep)
		return -EINVAL;

	if (job->num_pushbufs == 0)
		return 0;

	fence = calloc(1, sizeof(*fence));
	if (!fence)
		return -ENOMEM;

	cmdbufs = calloc(job->num_pushbufs, sizeof(*cmdbufs));
	if (!cmdbufs) {
		free(fence);
		return -ENOMEM;
	}

	for (i = 0; i < job->num_pushbufs; i++) {
		struct host1x_pushbuf *pushbuf = &job->pushbufs[i];
		struct drm_tegra_cmdbuf *cmdbuf = &cmdbufs[i];

		cmdbuf->handle = pushbuf->bo->handle;
		cmdbuf->offset = pushbuf->offset;
		cmdbuf->words = pushbuf->length;

		TRACE_IOCTL("cmdbuf: %x %d %d\n", cmdbuf->handle, cmdbuf->offset, cmdbuf->words);

		increments += pushbuf->increments;
		num_relocs += pushbuf->num_relocs;
	}

	memset(&syncpt, 0, sizeof(syncpt));
	syncpt.id = job->channel->syncpt;
	syncpt.incrs = increments;

	TRACE_IOCTL("syncpt: %d %d\n", syncpt.id, syncpt.incrs);

	relocs = calloc(num_relocs, sizeof(*relocs));
	if (!relocs) {
		free(cmdbufs);
		free(fence);
		return -ENOMEM;
	}

	reloc = relocs;

	for (i = 0; i < job->num_pushbufs; i++) {
		struct host1x_pushbuf *pushbuf = &job->pushbufs[i];

		for (j = 0; j < pushbuf->num_relocs; j++) {
			struct host1x_pushbuf_reloc *r = &pushbuf->relocs[j];

			reloc->cmdbuf.handle = pushbuf->bo->handle;
			reloc->cmdbuf.offset = r->source_offset;
			reloc->target.handle = r->target_handle;
			reloc->target.offset = r->target_offset;
			reloc->shift = r->shift;

			TRACE_IOCTL("reloc: %x %d %x %d %d\n", reloc->cmdbuf.handle, reloc->cmdbuf.offset, reloc->target.handle, reloc->target.offset, reloc->shift);

			reloc++;
		}
	}

	memset(&args, 0, sizeof(args));
	args.context = job->channel->context;
	args.num_syncpts = 1;
	args.num_cmdbufs = job->num_pushbufs;
	args.num_relocs = num_relocs;
	args.num_waitchks = 0;
	args.waitchk_mask = 0;
	args.timeout = 1000;

	args.syncpts = (unsigned long)&syncpt;
	args.cmdbufs = (unsigned long)cmdbufs;
	args.relocs = (unsigned long)relocs;
	args.waitchks = 0;

	TRACE_IOCTL("submit: %llx %d %d %d %d %x %d\n", args.context, args.num_syncpts, args.num_cmdbufs, args.num_relocs, args.num_waitchks, args.waitchk_mask, args.timeout);

	err = drmCommandWriteRead(drm->fd, DRM_TEGRA_SUBMIT, &args, sizeof(args));
	if (err < 0) {
		drmMsg("ioctl(DRM_IOCTL_TEGRA_SUBMIT) failed: %d\n",
			errno);
		err = -errno;
	}

	host1x_job_reset(job);
	free(relocs);
	free(cmdbufs);

	if (!err) {
		fence->syncpt = job->channel->syncpt;
		fence->value = args.fence;
		*fencep = fence;
	} else {
		free(fence);
		*fencep = NULL;
	}

	return err;
}

int drm_tegra_wait(struct drm_tegra *drm, struct host1x_fence *fence,
		   uint32_t timeout)
{
	struct drm_tegra_syncpt_wait args;
	int err;

	if (!drm)
		return -EINVAL;

	if (!fence)
		return 0;

	memset(&args, 0, sizeof(args));
	args.id = fence->syncpt;
	args.thresh = fence->value;
	args.timeout = timeout;

	TRACE_IOCTL("wait: %d %d %d\n", args.id, args.thresh, args.timeout);

	err = drmCommandWrite(drm->fd, DRM_TEGRA_SYNCPT_WAIT, &args, sizeof(args));
	if (err < 0) {
		drmMsg("ioctl(DRM_IOCTL_TEGRA_SYNCPT_WAIT) failed: %d\n",
			errno);
		return -errno;
	}

	return 0;
}

int drm_tegra_signaled(struct drm_tegra *drm, struct host1x_fence *fence)
{
	struct drm_tegra_syncpt_read args;
	int err;

	if (!drm)
		return -EINVAL;

	if (!fence)
		return 0;

	memset(&args, 0, sizeof(args));
	args.id = fence->syncpt;

	TRACE_IOCTL("syncpt_read: %d", args.id);

	err = drmCommandWriteRead(drm->fd, DRM_TEGRA_SYNCPT_READ, &args, sizeof(args));
	if (err < 0) {
		drmMsg("ioctl(DRM_IOCTL_TEGRA_SYNCPT_READ) failed: %d\n",
			errno);
		return -errno;
	}

	return fence->value <= args.value;
}

int drm_tegra_bo_create(struct drm_tegra *drm, uint32_t flags, uint32_t size,
			struct drm_tegra_bo **bop)
{
	struct drm_tegra_gem_create args;
	struct drm_tegra_bo *bo;
	int err;

	if (!drm || size == 0 || !bop)
		return -EINVAL;

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return -ENOMEM;

	DRMINITLISTHEAD(&bo->list);
	atomic_set(&bo->ref, 1);
	bo->flags = flags;
	bo->size = size;
	bo->drm = drm;

	memset(&args, 0, sizeof(args));
	args.flags = flags;
	args.size = size;

	TRACE_IOCTL("gem_create: %d %x\n", size, flags);

	err = drmCommandWriteRead(drm->fd, DRM_TEGRA_GEM_CREATE, &args,
				  sizeof(args));
	if (err < 0) {
		free(bo);
		return -errno;
	}

	DRMLISTADD(&bo->list, &drm->bo_list);
	bo->handle = args.handle;

	*bop = bo;

	return 0;
}

int drm_tegra_bo_open(struct drm_tegra *drm, uint32_t name,
		      struct drm_tegra_bo **bop)
{
	struct drm_gem_open args;
	struct drm_tegra_bo *bo;
	int err;

	if (!drm || !name || !bop)
		return -EINVAL;

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return -ENOMEM;

	DRMINITLISTHEAD(&bo->list);
	atomic_set(&bo->ref, 1);
	bo->flags = 0;
	bo->drm = drm;

	memset(&args, 0, sizeof(args));
	args.name = name;

	TRACE_IOCTL("gem_open: %x\n", name);

	err = drmIoctl(drm->fd, DRM_IOCTL_GEM_OPEN, &args);
	if (err < 0) {
		free(bo);
		return -errno;
	}

	DRMLISTADD(&bo->list, &drm->bo_list);
	bo->handle = args.handle;
	bo->size = args.size;

	*bop = bo;

	return 0;
}

int drm_tegra_bo_get_handle(struct drm_tegra_bo *bo, uint32_t *handle)
{
	if (!bo || !handle)
		return -EINVAL;

	*handle = bo->handle;

	return 0;
}

int drm_tegra_bo_get_name(struct drm_tegra_bo *bo, uint32_t *name)
{
	if (!bo || !name)
		return -EINVAL;

	if (!bo->name) {
		struct drm_gem_flink args;
		int err;

		memset(&args, 0, sizeof(args));
		args.handle = bo->handle;

		TRACE_IOCTL("gem_flink: %x\n", args.handle);

		err = drmIoctl(bo->drm->fd, DRM_IOCTL_GEM_FLINK, &args);
		if (err < 0)
			return -errno;

		bo->name = args.name;
	}

	*name = bo->name;

	return 0;
}

struct drm_tegra_bo *drm_tegra_bo_get(struct drm_tegra_bo *bo)
{
	if (bo)
		atomic_inc(&bo->ref);

	return bo;
}

void drm_tegra_bo_put(struct drm_tegra_bo *bo)
{
	if (bo && atomic_dec_and_test(&bo->ref))
		drm_tegra_bo_free(bo);
}

int drm_tegra_bo_map(struct drm_tegra_bo *bo, void **ptr)
{
	if (!bo)
		return -EINVAL;

	if (!bo->map) {
		struct drm_tegra_gem_mmap args;
		struct drm_tegra *drm = bo->drm;
		int err;

		memset(&args, 0, sizeof(args));
		args.handle = bo->handle;

		TRACE_IOCTL("gem_mmap: %x\n", args.handle);

		err = drmCommandWriteRead(drm->fd, DRM_TEGRA_GEM_MMAP, &args,
					  sizeof(args));
		if (err < 0)
			return -errno;

		bo->offset = args.offset;

		bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			       drm->fd, bo->offset);
		if (bo->map == MAP_FAILED) {
			bo->map = NULL;
			return -errno;
		}
	}

	if (ptr)
		*ptr = bo->map;

	return 0;
}

int drm_tegra_bo_unmap(struct drm_tegra_bo *bo)
{
	if (!bo || !bo->map)
		return -EINVAL;

	if (munmap(bo->map, bo->size))
		return -errno;

	bo->map = NULL;
	return 0;
}
