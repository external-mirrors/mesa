/*
 * Copyright © 2016 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#ifndef FD5_TEXTURE_H_
#define FD5_TEXTURE_H_

#include "pipe/p_context.h"

#include "freedreno_resource.h"
#include "freedreno_texture.h"

#include "fd5_context.h"
#include "fd5_format.h"

struct fd5_sampler_stateobj {
   struct pipe_sampler_state base;
   uint32_t texsamp0, texsamp1, texsamp2, texsamp3;
   bool needs_border;
};

static inline struct fd5_sampler_stateobj *
fd5_sampler_stateobj(struct pipe_sampler_state *samp)
{
   return (struct fd5_sampler_stateobj *)samp;
}

struct fd5_pipe_sampler_view {
   struct pipe_sampler_view base;
   uint32_t texconst0, texconst1, texconst2, texconst3, texconst5;
   uint32_t texconst6, texconst7, texconst8, texconst9, texconst10, texconst11;
   uint32_t offset;
};

static inline struct fd5_pipe_sampler_view *
fd5_pipe_sampler_view(struct pipe_sampler_view *pview)
{
   return (struct fd5_pipe_sampler_view *)pview;
}

void fd5_texture_init(struct pipe_context *pctx);

static inline enum a5xx_tex_type
fd5_tex_type(unsigned target)
{
   switch (target) {
   default:
      UNREACHABLE("Unsupported target");;
   case PIPE_BUFFER:
      return A5XX_TEX_BUFFER;
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
      return A5XX_TEX_1D;
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
      return A5XX_TEX_2D;
   case PIPE_TEXTURE_3D:
      return A5XX_TEX_3D;
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return A5XX_TEX_CUBE;
   }
}

#endif /* FD5_TEXTURE_H_ */
