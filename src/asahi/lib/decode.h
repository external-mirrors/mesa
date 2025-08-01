/*
 * Copyright 2017-2019 Lyude Paul
 * Copyright 2017-2019 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include <sys/types.h>
#include "agx_bo.h"

#include "drm-uapi/asahi_drm.h"

struct agxdecode_ctx;

struct agxdecode_ctx *agxdecode_new_context(uint64_t shader_base);

void agxdecode_destroy_context(struct agxdecode_ctx *ctx);

void agxdecode_next_frame(void);

void agxdecode_close(void);

void agxdecode_cmdstream(struct agxdecode_ctx *ctx, unsigned cmdbuf_index,
                         unsigned map_index, bool verbose);

struct util_dynarray;

void agxdecode_drm_cmdbuf(struct agxdecode_ctx *ctx,
                          struct drm_asahi_params_global *params,
                          struct util_dynarray *cmdbuf, bool verbose);

void agxdecode_dump_file_open(void);

void agxdecode_track_alloc(struct agxdecode_ctx *ctx, struct agx_bo *alloc);

void agxdecode_track_free(struct agxdecode_ctx *ctx, struct agx_bo *bo);

struct libagxdecode_config {
   uint32_t chip_id;
   size_t (*read_gpu_mem)(uint64_t addr, size_t size, void *data);
   ssize_t (*stream_write)(const char *buffer, size_t size);
};

void libagxdecode_init(struct libagxdecode_config *config);
void libagxdecode_vdm(struct agxdecode_ctx *ctx, uint64_t addr,
                      const char *label, bool verbose);
void libagxdecode_cdm(struct agxdecode_ctx *ctx, uint64_t addr,
                      const char *label, bool verbose);
void libagxdecode_usc(struct agxdecode_ctx *ctx, uint64_t addr,
                      const char *label, bool verbose);
void libagxdecode_shutdown(void);
