#ifndef TEGRA_PRIVATE_H
#define TEGRA_PRIVATE_H

#include <libdrm_lists.h>
#include <xf86atomic.h>

#include "tegra.h"

struct drm_tegra_bo {
	struct drm_tegra *drm;
	drmMMListHead list;
	uint32_t handle;
	uint32_t offset;
	uint32_t flags;
	uint32_t size;
	uint32_t name;
	atomic_t ref;
	void *map;
};

struct drm_tegra {
	drmMMListHead bo_list;
	int fd;
};

typedef uint32_t host1x_syncpt_t;

struct drm_tegra_channel {
	struct drm_tegra *drm;

	enum host1x_class client;
	uint64_t context;

	host1x_syncpt_t syncpt;
};

struct host1x_pushbuf_reloc {
	unsigned long source_offset;
	unsigned long target_handle;
	unsigned long target_offset;
	unsigned long shift;
};

struct host1x_pushbuf {
	host1x_syncpt_t syncpt;
	unsigned int increments;

	struct drm_tegra_bo *bo;
	unsigned long offset;
	unsigned long length;

	struct host1x_pushbuf_reloc *relocs;
	unsigned int num_relocs;

	uint32_t *ptr, *end;
};

struct host1x_fence {
	host1x_syncpt_t syncpt;
	uint32_t value;
};

struct host1x_job {
	struct drm_tegra_channel *channel;

	unsigned int increments;

	struct host1x_pushbuf *pushbufs;
	unsigned int num_pushbufs;
};

#if 1
# define TRACE_IOCTL(fmt, ...) drmMsg("TEGRA IOCTL: " fmt, ##__VA_ARGS__)
# define TRACE_PUSH(fmt, ...) drmMsg("TEGRA PUSH: " fmt, ##__VA_ARGS__)
#else
# define TRACE_IOCTL(fmt, ...)
# define TRACE_PUSH(fmt, ...)
#endif

#endif
