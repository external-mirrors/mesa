/*
 * Copyright © 2011-2012 Intel Corporation
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include "drm-uapi/drm_fourcc.h"
#include <sys/mman.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_wayland.h>

#include "util/anon_file.h"
#include "util/perf/cpu_trace.h"
#include "util/u_vector.h"
#include "util/format/u_formats.h"
#include "main/glconfig.h"
#include "pipe/p_screen.h"
#include "egl_dri2.h"
#include "eglglobals.h"
#include "kopper_interface.h"
#include "loader.h"
#include "loader_dri_helper.h"
#include "dri_screen.h"
#include "dri_util.h"
#include <loader_wayland_helper.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#ifdef HAVE_BIND_WL_DISPLAY
#include "wayland-drm-client-protocol.h"
#endif
#include <wayland-client.h>
#include <wayland-egl-backend.h>

/*
 * The index of entries in this table is used as a bitmask in
 * dri2_dpy->formats.formats_bitmap, which tracks the formats supported
 * by our server.
 */
static const struct dri2_wl_visual {
   uint32_t wl_drm_format;
   int pipe_format;
   /* alt_pipe_format is a substitute wl_buffer format to use for a
    * wl-server unsupported pipe_format, ie. some other pipe_format in
    * the table, of the same precision but with different channel ordering, or
    * PIPE_FORMAT_NONE if an alternate format is not needed or supported.
    * The code checks if alt_pipe_format can be used as a fallback for a
    * pipe_format for a given wl-server implementation.
    */
   int alt_pipe_format;
   int opaque_wl_drm_format;
} dri2_wl_visuals[] = {
   {
      DRM_FORMAT_ABGR16161616F,
      PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_XBGR16161616F,
   },
   {
      DRM_FORMAT_XBGR16161616F,
      PIPE_FORMAT_R16G16B16X16_FLOAT,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_XBGR16161616F,
   },
   {
      DRM_FORMAT_XRGB2101010,
      PIPE_FORMAT_B10G10R10X2_UNORM,
      PIPE_FORMAT_R10G10B10X2_UNORM,
      DRM_FORMAT_XRGB2101010,
   },
   {
      DRM_FORMAT_ARGB2101010,
      PIPE_FORMAT_B10G10R10A2_UNORM,
      PIPE_FORMAT_R10G10B10A2_UNORM,
      DRM_FORMAT_XRGB2101010,
   },
   {
      DRM_FORMAT_XBGR2101010,
      PIPE_FORMAT_R10G10B10X2_UNORM,
      PIPE_FORMAT_B10G10R10X2_UNORM,
      DRM_FORMAT_XBGR2101010,
   },
   {
      DRM_FORMAT_ABGR2101010,
      PIPE_FORMAT_R10G10B10A2_UNORM,
      PIPE_FORMAT_B10G10R10A2_UNORM,
      DRM_FORMAT_XBGR2101010,
   },
   {
      DRM_FORMAT_XRGB8888,
      PIPE_FORMAT_BGRX8888_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_XRGB8888,
   },
   {
      DRM_FORMAT_ARGB8888,
      PIPE_FORMAT_BGRA8888_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_XRGB8888,
   },
   {
      DRM_FORMAT_RGB888,
      PIPE_FORMAT_B8G8R8_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_RGB888,
   },
   {
      DRM_FORMAT_ABGR8888,
      PIPE_FORMAT_RGBA8888_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_XBGR8888,
   },
   {
      DRM_FORMAT_XBGR8888,
      PIPE_FORMAT_RGBX8888_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_XBGR8888,
   },
   {
      DRM_FORMAT_BGR888,
      PIPE_FORMAT_R8G8B8_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_BGR888,
   },
   {
      DRM_FORMAT_RGB565,
      PIPE_FORMAT_B5G6R5_UNORM,
      PIPE_FORMAT_NONE,
      DRM_FORMAT_RGB565,
   },
   {
      DRM_FORMAT_ARGB1555,
      PIPE_FORMAT_B5G5R5A1_UNORM,
      PIPE_FORMAT_R5G5B5A1_UNORM,
      DRM_FORMAT_XRGB1555,
   },
   {
      DRM_FORMAT_XRGB1555,
      PIPE_FORMAT_B5G5R5X1_UNORM,
      PIPE_FORMAT_R5G5B5X1_UNORM,
      DRM_FORMAT_XRGB1555,
   },
   {
      DRM_FORMAT_ARGB4444,
      PIPE_FORMAT_B4G4R4A4_UNORM,
      PIPE_FORMAT_R4G4B4A4_UNORM,
      DRM_FORMAT_XRGB4444,
   },
   {
      DRM_FORMAT_XRGB4444,
      PIPE_FORMAT_B4G4R4X4_UNORM,
      PIPE_FORMAT_R4G4B4X4_UNORM,
      DRM_FORMAT_XRGB4444,
   },
};

static int
dri2_wl_visual_idx_from_pipe_format(enum pipe_format pipe_format)
{
   if (util_format_is_srgb(pipe_format))
      pipe_format = util_format_linear(pipe_format);

   for (int i = 0; i < ARRAY_SIZE(dri2_wl_visuals); i++) {
      if (dri2_wl_visuals[i].pipe_format == pipe_format)
         return i;
   }

   return -1;
}

static int
dri2_wl_visual_idx_from_config(const struct dri_config *config)
{
   struct gl_config *gl_config = (struct gl_config *) config;

   return dri2_wl_visual_idx_from_pipe_format(gl_config->color_format);
}

static int
dri2_wl_visual_idx_from_fourcc(uint32_t fourcc)
{
   for (int i = 0; i < ARRAY_SIZE(dri2_wl_visuals); i++) {
      /* wl_drm format codes overlap with DRIImage FourCC codes for all formats
       * we support. */
      if (dri2_wl_visuals[i].wl_drm_format == fourcc)
         return i;
   }

   return -1;
}

static int
dri2_wl_shm_format_from_visual_idx(int idx)
{
   uint32_t fourcc = dri2_wl_visuals[idx].wl_drm_format;

   if (fourcc == DRM_FORMAT_ARGB8888)
      return WL_SHM_FORMAT_ARGB8888;
   else if (fourcc == DRM_FORMAT_XRGB8888)
      return WL_SHM_FORMAT_XRGB8888;
   else
      return fourcc;
}

static int
dri2_wl_visual_idx_from_shm_format(uint32_t shm_format)
{
   uint32_t fourcc;

   if (shm_format == WL_SHM_FORMAT_ARGB8888)
      fourcc = DRM_FORMAT_ARGB8888;
   else if (shm_format == WL_SHM_FORMAT_XRGB8888)
      fourcc = DRM_FORMAT_XRGB8888;
   else
      fourcc = shm_format;

   return dri2_wl_visual_idx_from_fourcc(fourcc);
}

bool
dri2_wl_is_format_supported(void *user_data, uint32_t format)
{
   _EGLDisplay *disp = (_EGLDisplay *)user_data;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   int j = dri2_wl_visual_idx_from_fourcc(format);

   if (j == -1)
      return false;

   for (int i = 0; dri2_dpy->driver_configs[i]; i++)
      if (j == dri2_wl_visual_idx_from_config(dri2_dpy->driver_configs[i]))
         return true;

   return false;
}

static bool
server_supports_format(struct dri2_wl_formats *formats, int idx)
{
   return idx >= 0 && BITSET_TEST(formats->formats_bitmap, idx);
}

static bool
server_supports_pipe_format(struct dri2_wl_formats *formats,
                            enum pipe_format format)
{
   return server_supports_format(formats,
                                 dri2_wl_visual_idx_from_pipe_format(format));
}

static bool
server_supports_fourcc(struct dri2_wl_formats *formats, uint32_t fourcc)
{
   return server_supports_format(formats, dri2_wl_visual_idx_from_fourcc(fourcc));
}

static int
roundtrip(struct dri2_egl_display *dri2_dpy)
{
   return wl_display_roundtrip_queue(dri2_dpy->wl_dpy, dri2_dpy->wl_queue);
}

static void
wl_buffer_release(void *data, struct wl_buffer *buffer)
{
   struct dri2_egl_surface *dri2_surf = data;
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); ++i)
      if (dri2_surf->color_buffers[i].wayland_buffer.buffer == buffer)
         break;

   assert(i < ARRAY_SIZE(dri2_surf->color_buffers));

   if (dri2_surf->color_buffers[i].wl_release) {
      loader_wayland_buffer_destroy(&dri2_surf->color_buffers[i].wayland_buffer);
      dri2_surf->color_buffers[i].wl_release = false;
      dri2_surf->color_buffers[i].wayland_buffer.buffer = NULL;
      dri2_surf->color_buffers[i].age = 0;
   }

   dri2_surf->color_buffers[i].locked = false;
}

static const struct wl_buffer_listener wl_buffer_listener = {
   .release = wl_buffer_release,
};

static void
dri2_wl_formats_fini(struct dri2_wl_formats *formats)
{
   unsigned int i;

   for (i = 0; i < formats->num_formats; i++)
      u_vector_finish(&formats->modifiers[i]);

   free(formats->modifiers);
   free(formats->formats_bitmap);
}

static int
dri2_wl_formats_init(struct dri2_wl_formats *formats)
{
   unsigned int i, j;

   /* formats->formats_bitmap tells us if a format in dri2_wl_visuals is present
    * or not. So we must compute the amount of unsigned int's needed to
    * represent all the formats of dri2_wl_visuals. We use BITSET_WORDS for
    * this task. */
   formats->num_formats = ARRAY_SIZE(dri2_wl_visuals);
   formats->formats_bitmap = calloc(BITSET_WORDS(formats->num_formats),
                                    sizeof(*formats->formats_bitmap));
   if (!formats->formats_bitmap)
      goto err;

   /* Here we have an array of u_vector's to store the modifiers supported by
    * each format in the bitmask. */
   formats->modifiers =
      calloc(formats->num_formats, sizeof(*formats->modifiers));
   if (!formats->modifiers)
      goto err_modifier;

   for (i = 0; i < formats->num_formats; i++)
      if (!u_vector_init_pow2(&formats->modifiers[i], 4, sizeof(uint64_t))) {
         j = i;
         goto err_vector_init;
      }

   return 0;

err_vector_init:
   for (i = 0; i < j; i++)
      u_vector_finish(&formats->modifiers[i]);
   free(formats->modifiers);
err_modifier:
   free(formats->formats_bitmap);
err:
   _eglError(EGL_BAD_ALLOC, "dri2_wl_formats_init");
   return -1;
}

static void
dmabuf_feedback_format_table_fini(
   struct dmabuf_feedback_format_table *format_table)
{
   if (format_table->data && format_table->data != MAP_FAILED)
      munmap(format_table->data, format_table->size);
}

static void
dmabuf_feedback_format_table_init(
   struct dmabuf_feedback_format_table *format_table)
{
   memset(format_table, 0, sizeof(*format_table));
}

static void
dmabuf_feedback_tranche_fini(struct dmabuf_feedback_tranche *tranche)
{
   dri2_wl_formats_fini(&tranche->formats);
}

static int
dmabuf_feedback_tranche_init(struct dmabuf_feedback_tranche *tranche)
{
   memset(tranche, 0, sizeof(*tranche));

   if (dri2_wl_formats_init(&tranche->formats) < 0)
      return -1;

   return 0;
}

static void
dmabuf_feedback_fini(struct dmabuf_feedback *dmabuf_feedback)
{
   dmabuf_feedback_tranche_fini(&dmabuf_feedback->pending_tranche);

   util_dynarray_foreach (&dmabuf_feedback->tranches,
                          struct dmabuf_feedback_tranche, tranche)
      dmabuf_feedback_tranche_fini(tranche);
   util_dynarray_fini(&dmabuf_feedback->tranches);

   dmabuf_feedback_format_table_fini(&dmabuf_feedback->format_table);
}

static int
dmabuf_feedback_init(struct dmabuf_feedback *dmabuf_feedback)
{
   memset(dmabuf_feedback, 0, sizeof(*dmabuf_feedback));

   if (dmabuf_feedback_tranche_init(&dmabuf_feedback->pending_tranche) < 0)
      return -1;

   util_dynarray_init(&dmabuf_feedback->tranches, NULL);

   dmabuf_feedback_format_table_init(&dmabuf_feedback->format_table);

   return 0;
}

static void
resize_callback(struct wl_egl_window *wl_win, void *data)
{
   struct dri2_egl_surface *dri2_surf = data;

   if (dri2_surf->base.Width == wl_win->width &&
       dri2_surf->base.Height == wl_win->height)
      return;

   dri2_surf->resized = true;

   /* Update the surface size as soon as native window is resized; from user
    * pov, this makes the effect that resize is done immediately after native
    * window resize, without requiring to wait until the first draw.
    *
    * A more detailed and lengthy explanation can be found at
    * https://lists.freedesktop.org/archives/mesa-dev/2018-June/196474.html
    */
   if (!dri2_surf->back) {
      dri2_surf->base.Width = wl_win->width;
      dri2_surf->base.Height = wl_win->height;
   }
   dri_invalidate_drawable(dri2_surf->dri_drawable);
}

static void
destroy_window_callback(void *data)
{
   struct dri2_egl_surface *dri2_surf = data;
   dri2_surf->wl_win = NULL;
}

static bool
get_wayland_surface(struct dri2_egl_surface *dri2_surf,
                    struct wl_egl_window *window)
{
   struct wl_surface *base_surface;

   /* Version 3 of wl_egl_window introduced a version field at the same
    * location where a pointer to wl_surface was stored. Thus, if
    * window->version is dereferenceable, we've been given an older version of
    * wl_egl_window, and window->version points to wl_surface */
   if (_eglPointerIsDereferenceable((void *)(window->version)))
      base_surface = (struct wl_surface *)window->version;
   else
      base_surface = window->surface;

   return loader_wayland_wrap_surface(&dri2_surf->wayland_surface,
                                      base_surface,
                                      dri2_surf->wl_queue);
}

static void
surface_dmabuf_feedback_format_table(
   void *data,
   struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
   int32_t fd, uint32_t size)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dmabuf_feedback *feedback = &dri2_surf->pending_dmabuf_feedback;

   feedback->format_table.size = size;
   feedback->format_table.data =
      mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

   close(fd);
}

static void
surface_dmabuf_feedback_main_device(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   struct wl_array *device)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dmabuf_feedback *feedback = &dri2_surf->pending_dmabuf_feedback;

   memcpy(&feedback->main_device, device->data, sizeof(feedback->main_device));

   /* Compositors may support switching render devices and change the main
    * device of the dma-buf feedback. In this case, when we reallocate the
    * buffers of the surface we must ensure that it is not allocated in memory
    * that is only visible to the GPU that EGL is using, as the compositor will
    * have to import them to the render device it is using.
    *
    * TODO: we still don't know how to allocate such buffers.
    */
   if (dri2_surf->dmabuf_feedback.main_device != 0 &&
       (feedback->main_device != dri2_surf->dmabuf_feedback.main_device))
      dri2_surf->compositor_using_another_device = true;
   else
      dri2_surf->compositor_using_another_device = false;
}

static void
surface_dmabuf_feedback_tranche_target_device(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   struct wl_array *device)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dmabuf_feedback *feedback = &dri2_surf->pending_dmabuf_feedback;

   memcpy(&feedback->pending_tranche.target_device, device->data,
          sizeof(feedback->pending_tranche.target_device));
}

static void
surface_dmabuf_feedback_tranche_flags(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   uint32_t flags)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dmabuf_feedback *feedback = &dri2_surf->pending_dmabuf_feedback;

   feedback->pending_tranche.flags = flags;
}

static void
surface_dmabuf_feedback_tranche_formats(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   struct wl_array *indices)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dmabuf_feedback *feedback = &dri2_surf->pending_dmabuf_feedback;
   uint32_t present_format = dri2_surf->format;
   uint64_t *modifier_ptr, modifier;
   uint32_t format;
   uint16_t *index;
   int visual_idx;

   if (dri2_surf->base.PresentOpaque) {
      visual_idx = dri2_wl_visual_idx_from_fourcc(present_format);
      if (visual_idx != -1)
         present_format = dri2_wl_visuals[visual_idx].opaque_wl_drm_format;
   }

   /* Compositor may advertise or not a format table. If it does, we use it.
    * Otherwise, we steal the most recent advertised format table. If we don't
    * have a most recent advertised format table, compositor did something
    * wrong. */
   if (feedback->format_table.data == NULL) {
      feedback->format_table = dri2_surf->dmabuf_feedback.format_table;
      dmabuf_feedback_format_table_init(
         &dri2_surf->dmabuf_feedback.format_table);
   }
   if (feedback->format_table.data == MAP_FAILED) {
      _eglLog(_EGL_WARNING, "wayland-egl: we could not map the format table "
                            "so we won't be able to use this batch of dma-buf "
                            "feedback events.");
      return;
   }
   if (feedback->format_table.data == NULL) {
      _eglLog(_EGL_WARNING,
              "wayland-egl: compositor didn't advertise a format "
              "table, so we won't be able to use this batch of dma-buf "
              "feedback events.");
      return;
   }

   wl_array_for_each (index, indices) {
      format = feedback->format_table.data[*index].format;
      modifier = feedback->format_table.data[*index].modifier;

      /* Skip formats that are not the one the surface is already using. We
       * can't switch to another format. */
      if (format != present_format)
         continue;

      /* We are sure that the format is supported because of the check above. */
      visual_idx = dri2_wl_visual_idx_from_fourcc(format);
      assert(visual_idx != -1);

      BITSET_SET(feedback->pending_tranche.formats.formats_bitmap, visual_idx);
      modifier_ptr =
         u_vector_add(&feedback->pending_tranche.formats.modifiers[visual_idx]);
      if (modifier_ptr)
         *modifier_ptr = modifier;
   }
}

static void
surface_dmabuf_feedback_tranche_done(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dmabuf_feedback *feedback = &dri2_surf->pending_dmabuf_feedback;

   /* Add tranche to array of tranches. */
   util_dynarray_append(&feedback->tranches, struct dmabuf_feedback_tranche,
                        feedback->pending_tranche);

   dmabuf_feedback_tranche_init(&feedback->pending_tranche);
}

static void
surface_dmabuf_feedback_done(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   struct dri2_egl_surface *dri2_surf = data;

   /* The dma-buf feedback protocol states that surface dma-buf feedback should
    * be sent by the compositor only if its buffers are using a suboptimal pair
    * of format and modifier. We can't change the buffer format, but we can
    * reallocate with another modifier. So we raise this flag in order to force
    * buffer reallocation based on the dma-buf feedback sent. */
   dri2_surf->received_dmabuf_feedback = true;

   dmabuf_feedback_fini(&dri2_surf->dmabuf_feedback);
   dri2_surf->dmabuf_feedback = dri2_surf->pending_dmabuf_feedback;
   dmabuf_feedback_init(&dri2_surf->pending_dmabuf_feedback);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener
   surface_dmabuf_feedback_listener = {
      .format_table = surface_dmabuf_feedback_format_table,
      .main_device = surface_dmabuf_feedback_main_device,
      .tranche_target_device = surface_dmabuf_feedback_tranche_target_device,
      .tranche_flags = surface_dmabuf_feedback_tranche_flags,
      .tranche_formats = surface_dmabuf_feedback_tranche_formats,
      .tranche_done = surface_dmabuf_feedback_tranche_done,
      .done = surface_dmabuf_feedback_done,
};

static bool
dri2_wl_modifiers_have_common(struct u_vector *modifiers1,
                              struct u_vector *modifiers2)
{
   uint64_t *mod1, *mod2;

   /* If both modifier vectors are empty, assume there is a compatible
    * implicit modifier. */
   if (u_vector_length(modifiers1) == 0 && u_vector_length(modifiers2) == 0)
       return true;

   u_vector_foreach(mod1, modifiers1)
   {
      u_vector_foreach(mod2, modifiers2)
      {
         if (*mod1 == *mod2)
            return true;
      }
   }

   return false;
}

/**
 * Called via eglCreateWindowSurface(), drv->CreateWindowSurface().
 */
static _EGLSurface *
dri2_wl_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct wl_egl_window *window = native_window;
   struct dri2_egl_surface *dri2_surf;
   struct zwp_linux_dmabuf_v1 *dmabuf_wrapper;
   int visual_idx;
   const struct dri_config *config;

   if (!window) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_create_surface");
      return NULL;
   }

   if (window->driver_private) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf,
                          attrib_list, false, native_window))
      goto cleanup_surf;

   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT,
                                dri2_surf->base.GLColorspace);

   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surf;
   }

   dri2_surf->base.Width = window->width;
   dri2_surf->base.Height = window->height;

   visual_idx = dri2_wl_visual_idx_from_config(config);
   assert(visual_idx != -1);
   assert(dri2_wl_visuals[visual_idx].pipe_format != PIPE_FORMAT_NONE);

   if (dri2_dpy->wl_dmabuf
#ifdef HAVE_BIND_WL_DISPLAY
   || dri2_dpy->wl_drm
#endif
   ) {
      dri2_surf->format = dri2_wl_visuals[visual_idx].wl_drm_format;
   } else {
      assert(dri2_dpy->wl_shm);
      dri2_surf->format = dri2_wl_shm_format_from_visual_idx(visual_idx);
   }

   if (dri2_surf->base.PresentOpaque) {
      uint32_t opaque_fourcc =
         dri2_wl_visuals[visual_idx].opaque_wl_drm_format;
      int opaque_visual_idx = dri2_wl_visual_idx_from_fourcc(opaque_fourcc);

      if (!server_supports_format(&dri2_dpy->formats, opaque_visual_idx) ||
          !dri2_wl_modifiers_have_common(
               &dri2_dpy->formats.modifiers[visual_idx],
               &dri2_dpy->formats.modifiers[opaque_visual_idx])) {
         _eglError(EGL_BAD_MATCH, "Unsupported opaque format");
         goto cleanup_surf;
      }
   }

   dri2_surf->wl_queue = wl_display_create_queue_with_name(dri2_dpy->wl_dpy,
                                                           "mesa egl surface queue");
   if (!dri2_surf->wl_queue) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      goto cleanup_surf;
   }

#ifdef HAVE_BIND_WL_DISPLAY
   if (dri2_dpy->wl_drm) {
      dri2_surf->wl_drm_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_drm);
      if (!dri2_surf->wl_drm_wrapper) {
         _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
         goto cleanup_queue;
      }
      wl_proxy_set_queue((struct wl_proxy *)dri2_surf->wl_drm_wrapper,
                         dri2_surf->wl_queue);
   }
#endif

   dri2_surf->wl_dpy_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_dpy);
   if (!dri2_surf->wl_dpy_wrapper) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      goto cleanup_drm;
   }
   wl_proxy_set_queue((struct wl_proxy *)dri2_surf->wl_dpy_wrapper,
                      dri2_surf->wl_queue);

   if (!get_wayland_surface(dri2_surf, window)) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      goto cleanup_dpy_wrapper;
   }

   if (dri2_dpy->wp_presentation) {
      loader_wayland_wrap_presentation(&dri2_surf->wayland_presentation,
                                       dri2_dpy->wp_presentation,
                                       dri2_surf->wl_queue,
                                       dri2_dpy->presentation_clock_id,
                                       &dri2_surf->wayland_surface,
                                       NULL, NULL, NULL);
   }

   if (dri2_dpy->wl_dmabuf &&
       zwp_linux_dmabuf_v1_get_version(dri2_dpy->wl_dmabuf) >=
          ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION) {
      dmabuf_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_dmabuf);
      if (!dmabuf_wrapper) {
         _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
         goto cleanup_surf_wrapper;
      }
      wl_proxy_set_queue((struct wl_proxy *)dmabuf_wrapper,
                         dri2_surf->wl_queue);
      dri2_surf->wl_dmabuf_feedback = zwp_linux_dmabuf_v1_get_surface_feedback(
         dmabuf_wrapper, dri2_surf->wayland_surface.wrapper);
      wl_proxy_wrapper_destroy(dmabuf_wrapper);

      zwp_linux_dmabuf_feedback_v1_add_listener(
         dri2_surf->wl_dmabuf_feedback, &surface_dmabuf_feedback_listener,
         dri2_surf);

      if (dmabuf_feedback_init(&dri2_surf->pending_dmabuf_feedback) < 0) {
         zwp_linux_dmabuf_feedback_v1_destroy(dri2_surf->wl_dmabuf_feedback);
         goto cleanup_surf_wrapper;
      }
      if (dmabuf_feedback_init(&dri2_surf->dmabuf_feedback) < 0) {
         dmabuf_feedback_fini(&dri2_surf->pending_dmabuf_feedback);
         zwp_linux_dmabuf_feedback_v1_destroy(dri2_surf->wl_dmabuf_feedback);
         goto cleanup_surf_wrapper;
      }

      if (roundtrip(dri2_dpy) < 0)
         goto cleanup_dmabuf_feedback;
   }

   dri2_surf->wl_win = window;
   dri2_surf->wl_win->driver_private = dri2_surf;
   dri2_surf->wl_win->destroy_window_callback = destroy_window_callback;
   if (!dri2_dpy->swrast_not_kms)
      dri2_surf->wl_win->resize_callback = resize_callback;

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_dmabuf_feedback;

   dri2_surf->base.SwapInterval = dri2_dpy->default_swap_interval;

   return &dri2_surf->base;

cleanup_dmabuf_feedback:
   if (dri2_surf->wl_dmabuf_feedback) {
      zwp_linux_dmabuf_feedback_v1_destroy(dri2_surf->wl_dmabuf_feedback);
      dmabuf_feedback_fini(&dri2_surf->dmabuf_feedback);
      dmabuf_feedback_fini(&dri2_surf->pending_dmabuf_feedback);
   }
cleanup_surf_wrapper:
   loader_wayland_surface_destroy(&dri2_surf->wayland_surface);
cleanup_dpy_wrapper:
   wl_proxy_wrapper_destroy(dri2_surf->wl_dpy_wrapper);
cleanup_drm:
#ifdef HAVE_BIND_WL_DISPLAY
   if (dri2_surf->wl_drm_wrapper)
      wl_proxy_wrapper_destroy(dri2_surf->wl_drm_wrapper);
cleanup_queue:
#endif
   wl_event_queue_destroy(dri2_surf->wl_queue);
cleanup_surf:
   free(dri2_surf);

   return NULL;
}

static _EGLSurface *
dri2_wl_create_pixmap_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_window, const EGLint *attrib_list)
{
   /* From the EGL_EXT_platform_wayland spec, version 3:
    *
    *   It is not valid to call eglCreatePlatformPixmapSurfaceEXT with a <dpy>
    *   that belongs to Wayland. Any such call fails and generates
    *   EGL_BAD_PARAMETER.
    */
   _eglError(EGL_BAD_PARAMETER, "cannot create EGL pixmap surfaces on "
                                "Wayland");
   return NULL;
}

/**
 * Called via eglDestroySurface(), drv->DestroySurface().
 */
static EGLBoolean
dri2_wl_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   driDestroyDrawable(dri2_surf->dri_drawable);

   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].wayland_buffer.buffer)
         loader_wayland_buffer_destroy(&dri2_surf->color_buffers[i].wayland_buffer);
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_destroy_image(dri2_surf->color_buffers[i].dri_image);
      if (dri2_surf->color_buffers[i].linear_copy)
         dri2_destroy_image(dri2_surf->color_buffers[i].linear_copy);
      if (dri2_surf->color_buffers[i].data)
         munmap(dri2_surf->color_buffers[i].data,
                dri2_surf->color_buffers[i].data_size);
   }

   if (dri2_surf->throttle_callback)
      wl_callback_destroy(dri2_surf->throttle_callback);

   if (dri2_surf->wl_win) {
      dri2_surf->wl_win->driver_private = NULL;
      dri2_surf->wl_win->resize_callback = NULL;
      dri2_surf->wl_win->destroy_window_callback = NULL;
   }

   loader_wayland_presentation_destroy(&dri2_surf->wayland_presentation);

   loader_wayland_surface_destroy(&dri2_surf->wayland_surface);
   wl_proxy_wrapper_destroy(dri2_surf->wl_dpy_wrapper);
#ifdef HAVE_BIND_WL_DISPLAY
   if (dri2_surf->wl_drm_wrapper)
      wl_proxy_wrapper_destroy(dri2_surf->wl_drm_wrapper);
#endif
   if (dri2_surf->wl_dmabuf_feedback) {
      zwp_linux_dmabuf_feedback_v1_destroy(dri2_surf->wl_dmabuf_feedback);
      dmabuf_feedback_fini(&dri2_surf->dmabuf_feedback);
      dmabuf_feedback_fini(&dri2_surf->pending_dmabuf_feedback);
   }
   wl_event_queue_destroy(dri2_surf->wl_queue);

   dri2_fini_surface(surf);
   free(surf);

   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_swap_interval(_EGLDisplay *disp, _EGLSurface *surf, EGLint interval)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   kopperSetSwapInterval(dri2_surf->dri_drawable, interval);

   return EGL_TRUE;
}

static void
dri2_wl_release_buffers(struct dri2_egl_surface *dri2_surf)
{
   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].wayland_buffer.buffer) {
         if (dri2_surf->color_buffers[i].locked) {
            dri2_surf->color_buffers[i].wl_release = true;
         } else {
            loader_wayland_buffer_destroy(&dri2_surf->color_buffers[i].wayland_buffer);
         }
      }
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_destroy_image(dri2_surf->color_buffers[i].dri_image);
      if (dri2_surf->color_buffers[i].linear_copy)
         dri2_destroy_image(dri2_surf->color_buffers[i].linear_copy);
      if (dri2_surf->color_buffers[i].data)
         munmap(dri2_surf->color_buffers[i].data,
                dri2_surf->color_buffers[i].data_size);

      dri2_surf->color_buffers[i].dri_image = NULL;
      dri2_surf->color_buffers[i].linear_copy = NULL;
      dri2_surf->color_buffers[i].data = NULL;
      dri2_surf->color_buffers[i].age = 0;
   }
}

/* Return list of modifiers that should be used to restrict the list of
 * modifiers actually supported by the surface. As of now, it is only used
 * to get the set of modifiers used for fixed-rate compression. */
static uint64_t *
get_surface_specific_modifiers(struct dri2_egl_surface *dri2_surf,
                               int *modifiers_count)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int rate = dri2_surf->base.CompressionRate;
   uint64_t *modifiers;

   if (rate == EGL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT ||
       !dri2_surf->wl_win)
      return NULL;

   if (!dri2_query_compression_modifiers(
          dri2_dpy->dri_screen_render_gpu, dri2_surf->format, rate,
          0, NULL, modifiers_count))
      return NULL;

   modifiers = malloc(*modifiers_count * sizeof(uint64_t));
   if (!modifiers)
      return NULL;

   if (!dri2_query_compression_modifiers(
          dri2_dpy->dri_screen_render_gpu, dri2_surf->format, rate,
          *modifiers_count, modifiers, modifiers_count)) {
      free(modifiers);
      return NULL;
   }

   return modifiers;
}

static void
update_surface(struct dri2_egl_surface *dri2_surf, struct dri_image *dri_img)
{
   int compression_rate;

   if (!dri_img)
      return;

   /* Update the surface with the actual compression rate */
   dri2_query_image(dri_img, __DRI_IMAGE_ATTRIB_COMPRESSION_RATE,
                               &compression_rate);
   dri2_surf->base.CompressionRate = compression_rate;
}

static bool
intersect_modifiers(struct u_vector *subset, struct u_vector *set,
                    uint64_t *other_modifiers, int other_modifiers_count)
{
   if (!u_vector_init_pow2(subset, 4, sizeof(uint64_t)))
      return false;

   uint64_t *modifier_ptr, *mod;
   u_vector_foreach(mod, set) {
      for (int i = 0; i < other_modifiers_count; ++i) {
         if (other_modifiers[i] != *mod)
            continue;
         modifier_ptr = u_vector_add(subset);
         if (modifier_ptr)
            *modifier_ptr = *mod;
      }
   }

   return true;
}

static void
create_dri_image(struct dri2_egl_surface *dri2_surf,
                 enum pipe_format pipe_format, uint32_t use_flags,
                 uint64_t *surf_modifiers, int surf_modifiers_count,
                 struct dri2_wl_formats *formats)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int visual_idx = dri2_wl_visual_idx_from_fourcc(dri2_surf->format);
   struct u_vector modifiers_subset;
   struct u_vector modifiers_subset_opaque;
   uint64_t *modifiers;
   unsigned int num_modifiers;
   struct u_vector *modifiers_present;
   bool implicit_mod_supported;

   assert(visual_idx != -1);

   if (dri2_surf->base.PresentOpaque) {
      uint32_t opaque_fourcc =
            dri2_wl_visuals[visual_idx].opaque_wl_drm_format;
      int opaque_visual_idx = dri2_wl_visual_idx_from_fourcc(opaque_fourcc);
      struct u_vector *modifiers_dpy = &dri2_dpy->formats.modifiers[visual_idx];
      /* Surface creation would have failed if we didn't support the matching
       * opaque format. */
      assert(opaque_visual_idx != -1);

      if (!BITSET_TEST(formats->formats_bitmap, opaque_visual_idx))
         return;

      if (!intersect_modifiers(&modifiers_subset_opaque,
                               &formats->modifiers[opaque_visual_idx],
                               u_vector_tail(modifiers_dpy),
                               u_vector_length(modifiers_dpy)))
         return;

      modifiers_present = &modifiers_subset_opaque;
   } else {
      if (!BITSET_TEST(formats->formats_bitmap, visual_idx))
         return;
      modifiers_present = &formats->modifiers[visual_idx];
   }

   if (surf_modifiers_count > 0) {
      if (!intersect_modifiers(&modifiers_subset, modifiers_present,
                               surf_modifiers, surf_modifiers_count))
         goto cleanup_present;
      modifiers = u_vector_tail(&modifiers_subset);
      num_modifiers = u_vector_length(&modifiers_subset);
   } else {
      modifiers = u_vector_tail(modifiers_present);
      num_modifiers = u_vector_length(modifiers_present);
   }

   if (!dri2_dpy->dri_screen_render_gpu->base.screen->resource_create_with_modifiers &&
       dri2_dpy->wl_dmabuf) {
      /* We don't support explicit modifiers, check if the compositor supports
       * implicit modifiers. */
      implicit_mod_supported = false;
      for (unsigned int i = 0; i < num_modifiers; i++) {
         if (modifiers[i] == DRM_FORMAT_MOD_INVALID) {
            implicit_mod_supported = true;
            break;
         }
      }

      if (!implicit_mod_supported) {
         if (surf_modifiers_count > 0) {
            u_vector_finish(&modifiers_subset);
         }

         goto cleanup_present;
      }

      num_modifiers = 0;
      modifiers = NULL;
   }

   /* For the purposes of this function, an INVALID modifier on
    * its own means the modifiers aren't supported. */
   if (num_modifiers == 0 ||
       (num_modifiers == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID)) {
      num_modifiers = 0;
      modifiers = NULL;
   }

   if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu) {
      use_flags = 0;
      modifiers = NULL;
      num_modifiers = 0;
   }

   dri2_surf->back->dri_image = dri_create_image_with_modifiers(
      dri2_dpy->dri_screen_render_gpu, dri2_surf->base.Width,
      dri2_surf->base.Height, pipe_format, use_flags,
      modifiers, num_modifiers, NULL);

   if (surf_modifiers_count > 0) {
      u_vector_finish(&modifiers_subset);
      update_surface(dri2_surf, dri2_surf->back->dri_image);
   }

cleanup_present:
   if (modifiers_present == &modifiers_subset_opaque)
      u_vector_finish(&modifiers_subset_opaque);
}

static void
create_dri_image_from_dmabuf_feedback(struct dri2_egl_surface *dri2_surf,
                                      enum pipe_format pipe_format,
                                      uint32_t use_flags,
                                      uint64_t *surf_modifiers,
                                      int surf_modifiers_count)
{
   uint32_t flags;

   /* We don't have valid dma-buf feedback, so return */
   if (dri2_surf->dmabuf_feedback.main_device == 0)
      return;

   /* Iterates through the dma-buf feedback to pick a new set of modifiers. The
    * tranches are sent in descending order of preference by the compositor, so
    * the first set that we can pick is the best one. For now we still can't
    * specify the target device in order to make the render device try its best
    * to allocate memory that can be directly scanned out by the KMS device. But
    * in the future this may change (newer versions of
    * createImageWithModifiers). Also, we are safe to pick modifiers from
    * tranches whose target device differs from the main device, as compositors
    * do not expose (in dma-buf feedback tranches) formats/modifiers that are
    * incompatible with the main device. */
   util_dynarray_foreach (&dri2_surf->dmabuf_feedback.tranches,
                          struct dmabuf_feedback_tranche, tranche) {
      flags = use_flags;
      if (tranche->flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT)
         flags |= __DRI_IMAGE_USE_SCANOUT;

      create_dri_image(dri2_surf, pipe_format, flags, surf_modifiers,
                       surf_modifiers_count, &tranche->formats);

      if (dri2_surf->back->dri_image)
         return;
   }
}

static void
create_dri_image_from_formats(struct dri2_egl_surface *dri2_surf,
                              enum pipe_format pipe_format, uint32_t use_flags,
                              uint64_t *surf_modifiers,
                              int surf_modifiers_count)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   create_dri_image(dri2_surf, pipe_format, use_flags, surf_modifiers,
                    surf_modifiers_count, &dri2_dpy->formats);
}

static void
wait_for_free_buffer(struct dri2_egl_display *dri2_dpy,
                     struct dri2_egl_surface *dri2_surf)
{
   MESA_TRACE_FUNC();

   /* There might be a buffer release already queued that wasn't processed */
   wl_display_dispatch_queue_pending(dri2_dpy->wl_dpy, dri2_surf->wl_queue);

   while (dri2_surf->back == NULL) {
      for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         /* Get an unlocked buffer, preferably one with a dri_buffer
          * already allocated and with minimum age.
          */
         if (dri2_surf->color_buffers[i].locked)
            continue;

         if (!dri2_surf->back || !dri2_surf->back->dri_image ||
             (dri2_surf->color_buffers[i].age > 0 &&
              dri2_surf->color_buffers[i].age < dri2_surf->back->age))
            dri2_surf->back = &dri2_surf->color_buffers[i];
      }

      if (dri2_surf->back)
         break;

      /* If we don't have a buffer, then block on the server to release one for
       * us, and try again. wl_display_dispatch_queue will process any pending
       * events, however not all servers flush on issuing a buffer release
       * event. So, we spam the server with roundtrips as they always cause a
       * client flush.
       */
      if (wl_display_roundtrip_queue(dri2_dpy->wl_dpy, dri2_surf->wl_queue) < 0)
         return;
   }
}

static int
get_back_bo(struct dri2_egl_surface *dri2_surf,
            struct mesa_trace_flow *flow)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int use_flags;
   int visual_idx;
   unsigned int pipe_format;
   unsigned int linear_pipe_format;

   MESA_TRACE_FUNC_FLOW(flow);

   visual_idx = dri2_wl_visual_idx_from_fourcc(dri2_surf->format);
   assert(visual_idx != -1);
   pipe_format = dri2_wl_visuals[visual_idx].pipe_format;
   linear_pipe_format = pipe_format;

   /* Substitute dri image format if server does not support original format */
   if (!BITSET_TEST(dri2_dpy->formats.formats_bitmap, visual_idx))
      linear_pipe_format = dri2_wl_visuals[visual_idx].alt_pipe_format;

   /* These asserts hold, as long as dri2_wl_visuals[] is self-consistent and
    * the PRIME substitution logic in dri2_wl_add_configs_for_visuals() is free
    * of bugs.
    */
   assert(linear_pipe_format != PIPE_FORMAT_NONE);
   assert(BITSET_TEST(
      dri2_dpy->formats.formats_bitmap,
      dri2_wl_visual_idx_from_pipe_format(linear_pipe_format)));

   wait_for_free_buffer(dri2_dpy, dri2_surf);
   if (dri2_surf->back == NULL)
      return -1;

   use_flags = __DRI_IMAGE_USE_SHARE | __DRI_IMAGE_USE_BACKBUFFER;

   if (dri2_surf->base.ProtectedContent) {
      /* Protected buffers can't be read from another GPU */
      if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu)
         return -1;
      use_flags |= __DRI_IMAGE_USE_PROTECTED;
   }

   if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu &&
       dri2_surf->back->linear_copy == NULL) {
      uint64_t linear_mod = DRM_FORMAT_MOD_LINEAR;
      const uint64_t *render_modifiers = NULL, *display_modifiers = NULL;
      unsigned int render_num_modifiers = 0, display_num_modifiers = 0;
      struct dri_image *linear_copy_display_gpu_image = NULL;

      if (dri2_dpy->dri_screen_render_gpu->base.screen->resource_create_with_modifiers) {
         render_modifiers = &linear_mod;
         render_num_modifiers = 1;
      }

      if (dri2_dpy->dri_screen_display_gpu) {
         if (dri2_dpy->dri_screen_display_gpu->base.screen->resource_create_with_modifiers) {
            display_modifiers = &linear_mod;
            display_num_modifiers = 1;
         }

         linear_copy_display_gpu_image = dri_create_image_with_modifiers(
            dri2_dpy->dri_screen_display_gpu,
            dri2_surf->base.Width, dri2_surf->base.Height,
            linear_pipe_format, use_flags | __DRI_IMAGE_USE_LINEAR,
            display_modifiers, display_num_modifiers, NULL);

         if (linear_copy_display_gpu_image) {
            int i, ret = 1;
            int fourcc;
            int num_planes = 0;
            int buffer_fds[4];
            int strides[4];
            int offsets[4];
            unsigned error;

            if (!dri2_query_image(linear_copy_display_gpu_image,
                                             __DRI_IMAGE_ATTRIB_NUM_PLANES,
                                             &num_planes))
               num_planes = 1;

            for (i = 0; i < num_planes; i++) {
               struct dri_image *image = dri2_from_planar(
                  linear_copy_display_gpu_image, i, NULL);

               if (!image) {
                  assert(i == 0);
                  image = linear_copy_display_gpu_image;
               }

               buffer_fds[i] = -1;
               ret &= dri2_query_image(image, __DRI_IMAGE_ATTRIB_FD,
                                                  &buffer_fds[i]);
               ret &= dri2_query_image(
                  image, __DRI_IMAGE_ATTRIB_STRIDE, &strides[i]);
               ret &= dri2_query_image(
                  image, __DRI_IMAGE_ATTRIB_OFFSET, &offsets[i]);

               if (image != linear_copy_display_gpu_image)
                  dri2_destroy_image(image);

               if (!ret) {
                  do {
                     if (buffer_fds[i] != -1)
                        close(buffer_fds[i]);
                  } while (--i >= 0);
                  dri2_destroy_image(linear_copy_display_gpu_image);
                  return -1;
               }
            }

            ret &= dri2_query_image(linear_copy_display_gpu_image,
                                               __DRI_IMAGE_ATTRIB_FOURCC,
                                               &fourcc);
            if (!ret) {
               do {
                  if (buffer_fds[i] != -1)
                     close(buffer_fds[i]);
               } while (--i >= 0);
               dri2_destroy_image(linear_copy_display_gpu_image);
               return -1;
            }

            /* The linear buffer was created in the display GPU's vram, so we
             * need to make it visible to render GPU
             */
            dri2_surf->back->linear_copy =
               dri2_from_dma_bufs(
                  dri2_dpy->dri_screen_render_gpu,
                  dri2_surf->base.Width, dri2_surf->base.Height,
                  fourcc, linear_mod,
                  &buffer_fds[0], num_planes, &strides[0], &offsets[0],
                  __DRI_YUV_COLOR_SPACE_UNDEFINED,
                  __DRI_YUV_RANGE_UNDEFINED, __DRI_YUV_CHROMA_SITING_UNDEFINED,
                  __DRI_YUV_CHROMA_SITING_UNDEFINED, __DRI_IMAGE_PRIME_LINEAR_BUFFER,
                  &error, dri2_surf->back);

            for (i = 0; i < num_planes; ++i) {
               if (buffer_fds[i] != -1)
                  close(buffer_fds[i]);
            }
            dri2_destroy_image(linear_copy_display_gpu_image);
         }
      }

      if (!dri2_surf->back->linear_copy) {
         dri2_surf->back->linear_copy = dri_create_image_with_modifiers(
            dri2_dpy->dri_screen_render_gpu,
            dri2_surf->base.Width, dri2_surf->base.Height,
            linear_pipe_format, use_flags | __DRI_IMAGE_USE_LINEAR,
            render_modifiers, render_num_modifiers, NULL);
      }

      if (dri2_surf->back->linear_copy == NULL)
         return -1;
   }

   if (dri2_surf->back->dri_image == NULL) {
      int modifiers_count = 0;
      uint64_t *modifiers =
         get_surface_specific_modifiers(dri2_surf, &modifiers_count);

      if (dri2_surf->wl_dmabuf_feedback)
         create_dri_image_from_dmabuf_feedback(
            dri2_surf, pipe_format, use_flags, modifiers, modifiers_count);
      if (dri2_surf->back->dri_image == NULL)
         create_dri_image_from_formats(dri2_surf, pipe_format, use_flags,
                                       modifiers, modifiers_count);

      free(modifiers);
      dri2_surf->back->age = 0;
   }

   if (dri2_surf->back->dri_image == NULL)
      return -1;

   loader_wayland_buffer_set_flow(&dri2_surf->back->wayland_buffer, flow);
   dri2_surf->back->locked = true;

   return 0;
}

static void
back_bo_to_dri_buffer(struct dri2_egl_surface *dri2_surf, __DRIbuffer *buffer)
{
   struct dri_image *image;
   int name, pitch;

   image = dri2_surf->back->dri_image;

   dri2_query_image(image, __DRI_IMAGE_ATTRIB_NAME, &name);
   dri2_query_image(image, __DRI_IMAGE_ATTRIB_STRIDE, &pitch);

   buffer->attachment = __DRI_BUFFER_BACK_LEFT;
   buffer->name = name;
   buffer->pitch = pitch;
   buffer->cpp = 4;
   buffer->flags = 0;
}

/* Value chosen empirically as a compromise between avoiding frequent
 * reallocations and extended time of increased memory consumption due to
 * unused buffers being kept.
 */
#define BUFFER_TRIM_AGE_HYSTERESIS 20

static int
update_buffers(struct dri2_egl_surface *dri2_surf,
               struct mesa_trace_flow *flow)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   MESA_TRACE_FUNC_FLOW(flow);

   if (dri2_surf->wl_win &&
       (dri2_surf->base.Width != dri2_surf->wl_win->width ||
        dri2_surf->base.Height != dri2_surf->wl_win->height)) {

      dri2_surf->base.Width = dri2_surf->wl_win->width;
      dri2_surf->base.Height = dri2_surf->wl_win->height;
      dri2_surf->dx = dri2_surf->wl_win->dx;
      dri2_surf->dy = dri2_surf->wl_win->dy;
   }

   if (dri2_surf->resized || dri2_surf->received_dmabuf_feedback) {
      dri2_wl_release_buffers(dri2_surf);
      dri2_surf->resized = false;
      dri2_surf->received_dmabuf_feedback = false;
   }

   if (get_back_bo(dri2_surf, flow) < 0) {
      _eglError(EGL_BAD_ALLOC, "failed to allocate color buffer");
      return -1;
   }

   /* If we have an extra unlocked buffer at this point, we had to do triple
    * buffering for a while, but now can go back to just double buffering.
    * That means we can free any unlocked buffer now. To avoid toggling between
    * going back to double buffering and needing to allocate another buffer too
    * fast we let the unneeded buffer sit around for a short while. */
   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (!dri2_surf->color_buffers[i].locked &&
          dri2_surf->color_buffers[i].wayland_buffer.buffer &&
          dri2_surf->color_buffers[i].age > BUFFER_TRIM_AGE_HYSTERESIS) {
         loader_wayland_buffer_destroy(&dri2_surf->color_buffers[i].wayland_buffer);
         dri2_destroy_image(dri2_surf->color_buffers[i].dri_image);
         if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu)
            dri2_destroy_image(
               dri2_surf->color_buffers[i].linear_copy);
         dri2_surf->color_buffers[i].wayland_buffer.buffer = NULL;
         dri2_surf->color_buffers[i].dri_image = NULL;
         dri2_surf->color_buffers[i].linear_copy = NULL;
         dri2_surf->color_buffers[i].age = 0;
      }
   }

   return 0;
}

static int
update_buffers_if_needed(struct dri2_egl_surface *dri2_surf,
                         struct mesa_trace_flow *flow)
{
   MESA_TRACE_FUNC_FLOW(flow);

   if (dri2_surf->back != NULL)
      return 0;

   return update_buffers(dri2_surf, flow);
}

static int
image_get_buffers(struct dri_drawable *driDrawable, unsigned int format,
                  uint32_t *stamp, void *loaderPrivate, uint32_t buffer_mask,
                  struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct mesa_trace_flow flow = {0};

   MESA_TRACE_FUNC_FLOW(&flow);

   if (update_buffers_if_needed(dri2_surf, &flow) < 0)
      return 0;

   buffers->image_mask = __DRI_IMAGE_BUFFER_BACK;
   buffers->back = dri2_surf->back->dri_image;

   return 1;
}

static void
dri2_wl_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
   (void)driDrawable;
   (void)loaderPrivate;
}

static unsigned
dri2_wl_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_FP16:
      return 1;
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static const __DRIimageLoaderExtension image_loader_extension = {
   .base = {__DRI_IMAGE_LOADER, 2},

   .getBuffers = image_get_buffers,
   .flushFrontBuffer = dri2_wl_flush_front_buffer,
   .getCapability = dri2_wl_get_capability,
};

static void
wayland_throttle_callback(void *data, struct wl_callback *callback,
                          uint32_t time)
{
   struct dri2_egl_surface *dri2_surf = data;

   dri2_surf->throttle_callback = NULL;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener throttle_listener = {
   .done = wayland_throttle_callback,
};

static struct wl_buffer *
create_wl_buffer(struct dri2_egl_display *dri2_dpy,
                 struct dri2_egl_surface *dri2_surf, struct dri_image *image)
{
   struct wl_buffer *ret = NULL;
   EGLBoolean query;
   int width, height, fourcc, num_planes;
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   int mod_hi, mod_lo;

   MESA_TRACE_FUNC();

   query = dri2_query_image(image, __DRI_IMAGE_ATTRIB_WIDTH, &width);
   query &=
      dri2_query_image(image, __DRI_IMAGE_ATTRIB_HEIGHT, &height);
   query &=
      dri2_query_image(image, __DRI_IMAGE_ATTRIB_FOURCC, &fourcc);
   if (!query)
      return NULL;

   query = dri2_query_image(image, __DRI_IMAGE_ATTRIB_NUM_PLANES,
                                       &num_planes);
   if (!query)
      num_planes = 1;

   query = dri2_query_image(image, __DRI_IMAGE_ATTRIB_MODIFIER_UPPER,
                                       &mod_hi);
   query &= dri2_query_image(
      image, __DRI_IMAGE_ATTRIB_MODIFIER_LOWER, &mod_lo);
   if (query) {
      modifier = combine_u32_into_u64(mod_hi, mod_lo);
   }

   bool supported_modifier = false;
   bool mod_invalid_supported = false;
   int visual_idx = dri2_wl_visual_idx_from_fourcc(fourcc);
   assert(visual_idx != -1);

   uint64_t *mod;
   u_vector_foreach(mod, &dri2_dpy->formats.modifiers[visual_idx])
   {
      if (*mod == DRM_FORMAT_MOD_INVALID) {
         mod_invalid_supported = true;
      }
      if (*mod == modifier) {
         supported_modifier = true;
         break;
      }
   }
   if (!supported_modifier && mod_invalid_supported) {
      /* If the server has advertised DRM_FORMAT_MOD_INVALID then we trust
       * that the client has allocated the buffer with the right implicit
       * modifier for the format, even though it's allocated a buffer the
       * server hasn't explicitly claimed to support. */
      modifier = DRM_FORMAT_MOD_INVALID;
      supported_modifier = true;
   }

   if (dri2_dpy->wl_dmabuf && supported_modifier) {
      struct zwp_linux_buffer_params_v1 *params;
      int i;

      /* We don't need a wrapper for wl_dmabuf objects, because we have to
       * create the intermediate params object; we can set the queue on this,
       * and the wl_buffer inherits it race-free. */
      params = zwp_linux_dmabuf_v1_create_params(dri2_dpy->wl_dmabuf);
      if (dri2_surf)
         wl_proxy_set_queue((struct wl_proxy *)params, dri2_surf->wl_queue);

      for (i = 0; i < num_planes; i++) {
         struct dri_image *p_image;
         int stride, offset;
         int fd = -1;

         p_image = dri2_from_planar(image, i, NULL);
         if (!p_image) {
            assert(i == 0);
            p_image = image;
         }

         query =
            dri2_query_image(p_image, __DRI_IMAGE_ATTRIB_FD, &fd);
         query &= dri2_query_image(
            p_image, __DRI_IMAGE_ATTRIB_STRIDE, &stride);
         query &= dri2_query_image(
            p_image, __DRI_IMAGE_ATTRIB_OFFSET, &offset);
         if (image != p_image)
            dri2_destroy_image(p_image);

         if (!query) {
            if (fd >= 0)
               close(fd);
            zwp_linux_buffer_params_v1_destroy(params);
            return NULL;
         }

         zwp_linux_buffer_params_v1_add(params, fd, i, offset, stride,
                                        modifier >> 32, modifier & 0xffffffff);
         close(fd);
      }

      if (dri2_surf && dri2_surf->base.PresentOpaque)
         fourcc = dri2_wl_visuals[visual_idx].opaque_wl_drm_format;

      ret = zwp_linux_buffer_params_v1_create_immed(params, width, height,
                                                    fourcc, 0);
      zwp_linux_buffer_params_v1_destroy(params);
   }
#ifdef HAVE_BIND_WL_DISPLAY
   else if (dri2_dpy->wl_drm) {
      struct wl_drm *wl_drm =
         dri2_surf ? dri2_surf->wl_drm_wrapper : dri2_dpy->wl_drm;
      int fd = -1, stride;

      /* wl_drm doesn't support explicit modifiers, so ideally we should bail
       * out if modifier != DRM_FORMAT_MOD_INVALID. However many drivers will
       * return a valid modifier when querying the DRIImage even if a buffer
       * was allocated without explicit modifiers.
       * XXX: bail out if the buffer was allocated without explicit modifiers
       */
      if (num_planes > 1)
         return NULL;

      query = dri2_query_image(image, __DRI_IMAGE_ATTRIB_FD, &fd);
      query &=
         dri2_query_image(image, __DRI_IMAGE_ATTRIB_STRIDE, &stride);
      if (!query) {
         if (fd >= 0)
            close(fd);
         return NULL;
      }

      ret = wl_drm_create_prime_buffer(wl_drm, fd, width, height, fourcc, 0,
                                       stride, 0, 0, 0, 0);
      close(fd);
   }
#endif

   return ret;
}

static EGLBoolean
try_damage_buffer(struct dri2_egl_surface *dri2_surf, const EGLint *rects,
                  EGLint n_rects)
{
   if (wl_proxy_get_version((struct wl_proxy *)dri2_surf->wayland_surface.wrapper) <
       WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
      return EGL_FALSE;

   if (n_rects == 0) {
      wl_surface_damage_buffer(dri2_surf->wayland_surface.wrapper, 0, 0,
                               INT32_MAX, INT32_MAX);
   } else {
      for (int i = 0; i < n_rects; i++) {
         const int *rect = &rects[i * 4];

         wl_surface_damage_buffer(dri2_surf->wayland_surface.wrapper, rect[0],
                                  dri2_surf->base.Height - rect[1] - rect[3],
                                  rect[2], rect[3]);
      }
   }

   return EGL_TRUE;
}

static int
throttle(struct dri2_egl_display *dri2_dpy,
         struct dri2_egl_surface *dri2_surf)
{
   MESA_TRACE_FUNC();

   while (dri2_surf->throttle_callback != NULL)
      if (loader_wayland_dispatch(dri2_dpy->wl_dpy, dri2_surf->wl_queue, NULL) ==
          -1)
         return -1;

   return 0;
}

/**
 * Called via eglSwapBuffers(), drv->SwapBuffers().
 */
static EGLBoolean
dri2_wl_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw,
                                 const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   struct mesa_trace_flow flow = { 0 };

   if (!dri2_surf->wl_win)
      return _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_swap_buffers");

   if (dri2_surf->back)
      flow = dri2_surf->back->wayland_buffer.flow;

   MESA_TRACE_FUNC_FLOW(&flow);

   /* Flush (and finish glthread) before:
    *   - update_buffers_if_needed because the unmarshalling thread
    *     may be running currently, and we would concurrently alloc/free
    *     the back bo.
    *   - swapping current/back because flushing may free the buffer and
    *     dri_image and reallocate them using get_back_bo (which causes a
    *     a crash because 'current' becomes NULL).
    *   - using any wl_* function because accessing them from this thread
    *     and glthread causes troubles (see #7624 and #8136)
    */
   dri2_flush_drawable_for_swapbuffers(disp, draw);
   dri_invalidate_drawable(dri2_surf->dri_drawable);

   if (dri2_surf->throttle_callback && throttle(dri2_dpy, dri2_surf) == -1)
      return -1;

   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++)
      if (dri2_surf->color_buffers[i].age > 0)
         dri2_surf->color_buffers[i].age++;

   /* Make sure we have a back buffer in case we're swapping without ever
    * rendering. */
   if (update_buffers_if_needed(dri2_surf, &flow) < 0)
      return _eglError(EGL_BAD_ALLOC, "dri2_swap_buffers");

   if (draw->SwapInterval > 0) {
      dri2_surf->throttle_callback =
         wl_surface_frame(dri2_surf->wayland_surface.wrapper);
      wl_callback_add_listener(dri2_surf->throttle_callback, &throttle_listener,
                               dri2_surf);
   }

   dri2_surf->back->age = 1;
   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   if (!dri2_surf->current->wayland_buffer.buffer) {
      struct dri_image *image;
      struct wl_buffer *buffer;

      if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu)
         image = dri2_surf->current->linear_copy;
      else
         image = dri2_surf->current->dri_image;

      buffer = create_wl_buffer(dri2_dpy, dri2_surf, image);
      if (buffer == NULL)
         return _eglError(EGL_BAD_ALLOC, "dri2_swap_buffers");
      loader_wayland_wrap_buffer(&dri2_surf->current->wayland_buffer, buffer);

      dri2_surf->current->wl_release = false;

      wl_buffer_add_listener(dri2_surf->current->wayland_buffer.buffer,
                             &wl_buffer_listener,
                             dri2_surf);

      loader_wayland_buffer_set_flow(&dri2_surf->current->wayland_buffer, &flow);
   }

   wl_surface_attach(dri2_surf->wayland_surface.wrapper,
                     dri2_surf->current->wayland_buffer.buffer,
                     dri2_surf->dx,
                     dri2_surf->dy);

   dri2_surf->wl_win->attached_width = dri2_surf->base.Width;
   dri2_surf->wl_win->attached_height = dri2_surf->base.Height;
   /* reset resize growing parameters */
   dri2_surf->dx = 0;
   dri2_surf->dy = 0;

   /* If the compositor doesn't support damage_buffer, we deliberately
    * ignore the damage region and post maximum damage, due to
    * https://bugs.freedesktop.org/78190 */
   if (!try_damage_buffer(dri2_surf, rects, n_rects))
      wl_surface_damage(dri2_surf->wayland_surface.wrapper, 0, 0, INT32_MAX,
                        INT32_MAX);

   if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu) {
      _EGLContext *ctx = _eglGetCurrentContext();
      struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
      struct dri_drawable *dri_drawable = dri2_dpy->vtbl->get_dri_drawable(draw);
      dri2_blit_image(
         dri2_ctx->dri_context, dri2_surf->current->linear_copy,
         dri2_surf->current->dri_image, 0, 0, dri2_surf->base.Width,
         dri2_surf->base.Height, 0, 0, dri2_surf->base.Width,
         dri2_surf->base.Height, 0);
      dri_flush_drawable(dri_drawable);
   }

   loader_wayland_presentation_feedback(&dri2_surf->wayland_presentation,
                                        &dri2_surf->current->wayland_buffer,
                                        NULL);

   wl_surface_commit(dri2_surf->wayland_surface.wrapper);

   /* If we're not waiting for a frame callback then we'll at least throttle
    * to a sync callback so that we always give a chance for the compositor to
    * handle the commit and send a release event before checking for a free
    * buffer */
   if (dri2_surf->throttle_callback == NULL) {
      dri2_surf->throttle_callback = wl_display_sync(dri2_surf->wl_dpy_wrapper);
      wl_callback_add_listener(dri2_surf->throttle_callback, &throttle_listener,
                               dri2_surf);
   }

   wl_display_flush(dri2_dpy->wl_dpy);

   return EGL_TRUE;
}

static EGLint
dri2_wl_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);
   struct mesa_trace_flow flow = { 0 };

   MESA_TRACE_FUNC_FLOW(&flow);

   if (update_buffers_if_needed(dri2_surf, &flow) < 0) {
      _eglError(EGL_BAD_ALLOC, "dri2_query_buffer_age");
      return -1;
   }

   return dri2_surf->back->age;
}

static EGLBoolean
dri2_wl_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   return dri2_wl_swap_buffers_with_damage(disp, draw, NULL, 0);
}

#ifdef HAVE_BIND_WL_DISPLAY
static struct wl_buffer *
dri2_wl_create_wayland_buffer_from_image(_EGLDisplay *disp, _EGLImage *img)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img = dri2_egl_image(img);
   struct dri_image *image = dri2_img->dri_image;
   struct wl_buffer *buffer;
   int fourcc;

   /* Check the upstream display supports this buffer's format. */
   dri2_query_image(image, __DRI_IMAGE_ATTRIB_FOURCC, &fourcc);
   if (!server_supports_fourcc(&dri2_dpy->formats, fourcc))
      goto bad_format;

   buffer = create_wl_buffer(dri2_dpy, NULL, image);

   /* The buffer object will have been created with our internal event queue
    * because it is using wl_dmabuf/wl_drm as a proxy factory. We want the
    * buffer to be used by the application so we'll reset it to the display's
    * default event queue. This isn't actually racy, as the only event the
    * buffer can get is a buffer release, which doesn't happen with an explicit
    * attach. */
   if (buffer)
      wl_proxy_set_queue((struct wl_proxy *)buffer, NULL);

   return buffer;

bad_format:
   _eglError(EGL_BAD_MATCH, "unsupported image format");
   return NULL;
}

static int
dri2_wl_authenticate(_EGLDisplay *disp, uint32_t id)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   int ret = 0;

   if (dri2_dpy->is_render_node) {
      _eglLog(_EGL_WARNING, "wayland-egl: client asks server to "
                            "authenticate for render-nodes");
      return 0;
   }
   dri2_dpy->authenticated = false;

   wl_drm_authenticate(dri2_dpy->wl_drm, id);
   if (roundtrip(dri2_dpy) < 0)
      ret = -1;

   if (!dri2_dpy->authenticated)
      ret = -1;

   /* reset authenticated */
   dri2_dpy->authenticated = true;

   return ret;
}

static void
drm_handle_device(void *data, struct wl_drm *drm, const char *device)
{
   struct dri2_egl_display *dri2_dpy = data;
   drm_magic_t magic;

   dri2_dpy->device_name = strdup(device);
   if (!dri2_dpy->device_name)
      return;

   dri2_dpy->fd_render_gpu = loader_open_device(dri2_dpy->device_name);
   if (dri2_dpy->fd_render_gpu == -1) {
      _eglLog(_EGL_WARNING, "wayland-egl: could not open %s (%s)",
              dri2_dpy->device_name, strerror(errno));
      free(dri2_dpy->device_name);
      dri2_dpy->device_name = NULL;
      return;
   }

   if (drmGetNodeTypeFromFd(dri2_dpy->fd_render_gpu) == DRM_NODE_RENDER) {
      dri2_dpy->authenticated = true;
   } else {
      if (drmGetMagic(dri2_dpy->fd_render_gpu, &magic)) {
         close(dri2_dpy->fd_render_gpu);
         dri2_dpy->fd_render_gpu = -1;
         free(dri2_dpy->device_name);
         dri2_dpy->device_name = NULL;
         _eglLog(_EGL_WARNING, "wayland-egl: drmGetMagic failed");
         return;
      }
      wl_drm_authenticate(dri2_dpy->wl_drm, magic);
   }
}

static void
drm_handle_format(void *data, struct wl_drm *drm, uint32_t format)
{
   struct dri2_egl_display *dri2_dpy = data;
   int visual_idx = dri2_wl_visual_idx_from_fourcc(format);

   if (visual_idx == -1)
      return;

   BITSET_SET(dri2_dpy->formats.formats_bitmap, visual_idx);
}

static void
drm_handle_capabilities(void *data, struct wl_drm *drm, uint32_t value)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->capabilities = value;
}

static void
drm_handle_authenticated(void *data, struct wl_drm *drm)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->authenticated = true;
}

static const struct wl_drm_listener drm_listener = {
   .device = drm_handle_device,
   .format = drm_handle_format,
   .authenticated = drm_handle_authenticated,
   .capabilities = drm_handle_capabilities,
};
#endif

static void
dmabuf_ignore_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
                     uint32_t format)
{
   /* formats are implicitly advertised by the 'modifier' event, so ignore */
}

static void
dmabuf_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
                       uint32_t format, uint32_t modifier_hi,
                       uint32_t modifier_lo)
{
   struct dri2_egl_display *dri2_dpy = data;
   int visual_idx = dri2_wl_visual_idx_from_fourcc(format);
   uint64_t *mod;

   /* Ignore this if the compositor advertised dma-buf feedback. From version 4
    * onwards (when dma-buf feedback was introduced), the compositor should not
    * advertise this event anymore, but let's keep this for safety. */
   if (dri2_dpy->wl_dmabuf_feedback)
      return;

   if (visual_idx == -1)
      return;

   BITSET_SET(dri2_dpy->formats.formats_bitmap, visual_idx);

   mod = u_vector_add(&dri2_dpy->formats.modifiers[visual_idx]);
   if (mod)
      *mod = combine_u32_into_u64(modifier_hi, modifier_lo);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
   .format = dmabuf_ignore_format,
   .modifier = dmabuf_handle_modifier,
};

#ifdef HAVE_BIND_WL_DISPLAY
static void
wl_drm_bind(struct dri2_egl_display *dri2_dpy)
{
   dri2_dpy->wl_drm =
      wl_registry_bind(dri2_dpy->wl_registry, dri2_dpy->wl_drm_name,
                       &wl_drm_interface, dri2_dpy->wl_drm_version);
   wl_drm_add_listener(dri2_dpy->wl_drm, &drm_listener, dri2_dpy);
}
#endif

static void
default_dmabuf_feedback_format_table(
   void *data,
   struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
   int32_t fd, uint32_t size)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->format_table.size = size;
   dri2_dpy->format_table.data =
      mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

   close(fd);
}

static void
default_dmabuf_feedback_main_device(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   struct wl_array *device)
{
   struct dri2_egl_display *dri2_dpy = data;
   char *node;
   int fd;
   dev_t dev;

   /* Given the device, look for a render node and try to open it. */
   memcpy(&dev, device->data, sizeof(dev));
   node = loader_get_render_node(dev);
   if (!node)
      return;
   fd = loader_open_device(node);
   if (fd == -1) {
      free(node);
      return;
   }

   dri2_dpy->device_name = node;
   dri2_dpy->fd_render_gpu = fd;
#ifdef HAVE_BIND_WL_DISPLAY
   dri2_dpy->authenticated = true;
#endif
}

static void
default_dmabuf_feedback_tranche_target_device(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   struct wl_array *device)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_tranche_flags(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   uint32_t flags)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_tranche_formats(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
   struct wl_array *indices)
{
   struct dri2_egl_display *dri2_dpy = data;
   uint64_t *modifier_ptr, modifier;
   uint32_t format;
   uint16_t *index;
   int visual_idx;

   if (dri2_dpy->format_table.data == MAP_FAILED) {
      _eglLog(_EGL_WARNING, "wayland-egl: we could not map the format table "
                            "so we won't be able to use this batch of dma-buf "
                            "feedback events.");
      return;
   }
   if (dri2_dpy->format_table.data == NULL) {
      _eglLog(_EGL_WARNING,
              "wayland-egl: compositor didn't advertise a format "
              "table, so we won't be able to use this batch of dma-buf "
              "feedback events.");
      return;
   }

   wl_array_for_each (index, indices) {
      format = dri2_dpy->format_table.data[*index].format;
      modifier = dri2_dpy->format_table.data[*index].modifier;

      /* skip formats that we don't support */
      visual_idx = dri2_wl_visual_idx_from_fourcc(format);
      if (visual_idx == -1)
         continue;

      BITSET_SET(dri2_dpy->formats.formats_bitmap, visual_idx);
      modifier_ptr = u_vector_add(&dri2_dpy->formats.modifiers[visual_idx]);
      if (modifier_ptr)
         *modifier_ptr = modifier;
   }
}

static void
default_dmabuf_feedback_tranche_done(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   /* ignore this event */
}

static void
default_dmabuf_feedback_done(
   void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback)
{
   /* ignore this event */
}

static const struct zwp_linux_dmabuf_feedback_v1_listener
   dmabuf_feedback_listener = {
      .format_table = default_dmabuf_feedback_format_table,
      .main_device = default_dmabuf_feedback_main_device,
      .tranche_target_device = default_dmabuf_feedback_tranche_target_device,
      .tranche_flags = default_dmabuf_feedback_tranche_flags,
      .tranche_formats = default_dmabuf_feedback_tranche_formats,
      .tranche_done = default_dmabuf_feedback_tranche_done,
      .done = default_dmabuf_feedback_done,
};

static void
presentation_handle_clock_id(void* data, struct wp_presentation *wp_presentation, uint32_t clk_id)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->presentation_clock_id = clk_id;
}

static const struct wp_presentation_listener presentation_listener = {
   presentation_handle_clock_id,
};

static void
registry_handle_global_drm(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface,
                           uint32_t version)
{
   struct dri2_egl_display *dri2_dpy = data;

#ifdef HAVE_BIND_WL_DISPLAY
   if (strcmp(interface, wl_drm_interface.name) == 0) {
      dri2_dpy->wl_drm_version = MIN2(version, 2);
      dri2_dpy->wl_drm_name = name;
   } else
#endif
   if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 &&
              version >= 3) {
      dri2_dpy->wl_dmabuf = wl_registry_bind(
         registry, name, &zwp_linux_dmabuf_v1_interface,
         MIN2(version, ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION));
      zwp_linux_dmabuf_v1_add_listener(dri2_dpy->wl_dmabuf, &dmabuf_listener,
                                       dri2_dpy);
   } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
      dri2_dpy->wp_presentation =
         wl_registry_bind(registry, name, &wp_presentation_interface, 1);
      wp_presentation_add_listener(dri2_dpy->wp_presentation,
                                   &presentation_listener, dri2_dpy);
   }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
}

static const struct wl_registry_listener registry_listener_drm = {
   .global = registry_handle_global_drm,
   .global_remove = registry_handle_global_remove,
};

static void
dri2_wl_setup_swap_interval(_EGLDisplay *disp)
{
   /* We can't use values greater than 1 on Wayland because we are using the
    * frame callback to synchronise the frame and the only way we be sure to
    * get a frame callback is to attach a new buffer. Therefore we can't just
    * sit drawing nothing to wait until the next ‘n’ frame callbacks */

   dri2_setup_swap_interval(disp, 1);
}

static const struct dri2_egl_display_vtbl dri2_wl_display_vtbl = {
#ifdef HAVE_BIND_WL_DISPLAY
   .authenticate = dri2_wl_authenticate,
   .create_wayland_buffer_from_image = dri2_wl_create_wayland_buffer_from_image,
#endif
   .create_window_surface = dri2_wl_create_window_surface,
   .create_pixmap_surface = dri2_wl_create_pixmap_surface,
   .destroy_surface = dri2_wl_destroy_surface,
   .swap_interval = dri2_wl_swap_interval,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_wl_swap_buffers,
   .swap_buffers_with_damage = dri2_wl_swap_buffers_with_damage,
   .query_buffer_age = dri2_wl_query_buffer_age,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static const __DRIextension *dri2_loader_extensions[] = {
   &image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

static EGLBoolean
dri2_wl_surface_throttle(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   while (dri2_surf->throttle_callback != NULL)
      if (loader_wayland_dispatch(dri2_dpy->wl_dpy, dri2_surf->wl_queue, NULL) ==
          -1)
         return EGL_FALSE;

   if (dri2_surf->base.SwapInterval > 0) {
      dri2_surf->throttle_callback =
         wl_surface_frame(dri2_surf->wayland_surface.wrapper);
      wl_callback_add_listener(dri2_surf->throttle_callback, &throttle_listener,
                               dri2_surf);
   }

   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_kopper_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw,
                                        const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (!dri2_surf->wl_win)
      return _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_swap_buffers");

   if (!dri2_wl_surface_throttle(dri2_surf))
      return EGL_FALSE;

   if (n_rects) {
      kopperSwapBuffersWithDamage(dri2_surf->dri_drawable, __DRI2_FLUSH_INVALIDATE_ANCILLARY, n_rects, rects);
   } else {
      kopperSwapBuffers(dri2_surf->dri_drawable, __DRI2_FLUSH_INVALIDATE_ANCILLARY);
   }

   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_kopper_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   dri2_wl_kopper_swap_buffers_with_damage(disp, draw, NULL, 0);
   return EGL_TRUE;
}

static EGLint
dri2_wl_kopper_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   return kopperQueryBufferAge(dri2_surf->dri_drawable);
}

static const struct dri2_egl_display_vtbl dri2_wl_kopper_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = dri2_wl_create_window_surface,
   .create_pixmap_surface = dri2_wl_create_pixmap_surface,
   .destroy_surface = dri2_wl_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_wl_kopper_swap_buffers,
   .swap_buffers_with_damage = dri2_wl_kopper_swap_buffers_with_damage,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
   .query_buffer_age = dri2_wl_kopper_query_buffer_age,
};

static_assert(sizeof(struct kopper_vk_surface_create_storage) >=
                 sizeof(VkWaylandSurfaceCreateInfoKHR),
              "");

static void
kopperSetSurfaceCreateInfo(void *_draw, struct kopper_loader_info *out)
{
   struct dri2_egl_surface *dri2_surf = _draw;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   VkWaylandSurfaceCreateInfoKHR *wlsci =
      (VkWaylandSurfaceCreateInfoKHR *)&out->bos;

   wlsci->sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
   wlsci->pNext = NULL;
   wlsci->flags = 0;
   wlsci->display = dri2_dpy->wl_dpy;
   /* Pass the original wl_surface through to Vulkan WSI.  If we pass the
    * proxy wrapper, kopper won't be able to properly de-duplicate surfaces
    * and we may end up creating two VkSurfaceKHRs for the same underlying
    * wl_surface.  Vulkan WSI (which kopper calls into) will make its own
    * queues and proxy wrappers.
    */
   wlsci->surface = dri2_surf->wayland_surface.surface;
   out->present_opaque = dri2_surf->base.PresentOpaque;
   /* convert to vulkan constants */
   switch (dri2_surf->base.CompressionRate) {
   case EGL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT:
      out->compression = 0;
      break;
   case EGL_SURFACE_COMPRESSION_FIXED_RATE_DEFAULT_EXT:
      out->compression = UINT32_MAX;
      break;
#define EGL_VK_COMP(NUM) \
   case EGL_SURFACE_COMPRESSION_FIXED_RATE_##NUM##BPC_EXT: \
      out->compression = VK_IMAGE_COMPRESSION_FIXED_RATE_##NUM##BPC_BIT_EXT; \
      break
   EGL_VK_COMP(1);
   EGL_VK_COMP(2);
   EGL_VK_COMP(3);
   EGL_VK_COMP(4);
   EGL_VK_COMP(5);
   EGL_VK_COMP(6);
   EGL_VK_COMP(7);
   EGL_VK_COMP(8);
   EGL_VK_COMP(9);
   EGL_VK_COMP(10);
   EGL_VK_COMP(11);
   EGL_VK_COMP(12);
#undef EGL_VK_COMP
   default:
      UNREACHABLE("unknown compression rate");
   }
}

static void
kopper_update_buffers(struct dri2_egl_surface *dri2_surf)
{
   /* we need to do the following operations only once per frame */
   if (dri2_surf->back)
      return;

   if (dri2_surf->wl_win &&
       (dri2_surf->base.Width != dri2_surf->wl_win->width ||
        dri2_surf->base.Height != dri2_surf->wl_win->height)) {

      dri2_surf->base.Width = dri2_surf->wl_win->width;
      dri2_surf->base.Height = dri2_surf->wl_win->height;
      dri2_surf->dx = dri2_surf->wl_win->dx;
      dri2_surf->dy = dri2_surf->wl_win->dy;
      dri2_surf->current = NULL;
   }
}

static void
dri2_wl_kopper_get_drawable_info(struct dri_drawable *draw, int *w,
                                 int *h, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   kopper_update_buffers(dri2_surf);
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static const __DRIkopperLoaderExtension kopper_loader_extension = {
   .base = {__DRI_KOPPER_LOADER, 1},

   .SetSurfaceCreateInfo = kopperSetSurfaceCreateInfo,
   .GetDrawableInfo = dri2_wl_kopper_get_drawable_info,
};

static const __DRIextension *kopper_loader_extensions[] = {
   &kopper_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

static void
dri2_wl_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned int format_count[ARRAY_SIZE(dri2_wl_visuals)] = {0};

   /* Try to create an EGLConfig for every config the driver declares */
   for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++) {
      struct dri2_egl_config *dri2_conf;
      bool conversion = false;
      int idx = dri2_wl_visual_idx_from_config(dri2_dpy->driver_configs[i]);

      if (idx < 0)
         continue;

      /* Check if the server natively supports the colour buffer format */
      if (!server_supports_format(&dri2_dpy->formats, idx)) {
         /* In multi-GPU scenarios, we usually have a different buffer, so a
          * format conversion is easy compared to the overhead of the copy */
         if (dri2_dpy->fd_render_gpu == dri2_dpy->fd_display_gpu)
            continue;

         /* Check if the server supports the alternate format */
         if (!server_supports_pipe_format(&dri2_dpy->formats,
                                          dri2_wl_visuals[idx].alt_pipe_format)) {
            continue;
         }

         conversion = true;
      }

      EGLint attr_list[] = {
         EGL_NATIVE_VISUAL_ID, dri2_wl_visuals[idx].wl_drm_format,
         EGL_NONE,
      };

      /* The format is supported one way or another; add the EGLConfig */
      dri2_conf = dri2_add_config(disp, dri2_dpy->driver_configs[i],
                                  EGL_WINDOW_BIT, attr_list);
      if (!dri2_conf)
         continue;

      format_count[idx]++;

      if (conversion && format_count[idx] == 1) {
         _eglLog(_EGL_DEBUG, "Client format %s converted via PRIME blitImage.",
                 util_format_name(dri2_wl_visuals[idx].pipe_format));
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(format_count); i++) {
      if (!format_count[i]) {
         _eglLog(_EGL_DEBUG, "No DRI config supports native format %s",
                 util_format_name(dri2_wl_visuals[i].pipe_format));
      }
   }
}

static bool
dri2_initialize_wayland_drm_extensions(struct dri2_egl_display *dri2_dpy)
{
   /* Get default dma-buf feedback */
   if (dri2_dpy->wl_dmabuf &&
       zwp_linux_dmabuf_v1_get_version(dri2_dpy->wl_dmabuf) >=
          ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
      dmabuf_feedback_format_table_init(&dri2_dpy->format_table);
      dri2_dpy->wl_dmabuf_feedback =
         zwp_linux_dmabuf_v1_get_default_feedback(dri2_dpy->wl_dmabuf);
      zwp_linux_dmabuf_feedback_v1_add_listener(
         dri2_dpy->wl_dmabuf_feedback, &dmabuf_feedback_listener, dri2_dpy);
   }

   if (roundtrip(dri2_dpy) < 0)
      return false;

   /* Destroy the default dma-buf feedback and the format table. */
   if (dri2_dpy->wl_dmabuf_feedback) {
      zwp_linux_dmabuf_feedback_v1_destroy(dri2_dpy->wl_dmabuf_feedback);
      dri2_dpy->wl_dmabuf_feedback = NULL;
      dmabuf_feedback_format_table_fini(&dri2_dpy->format_table);
   }

#ifdef HAVE_BIND_WL_DISPLAY
   /* We couldn't retrieve a render node from the dma-buf feedback (or the
    * feedback was not advertised at all), so we must fallback to wl_drm. */
   if (dri2_dpy->fd_render_gpu == -1) {
      /* wl_drm not advertised by compositor, so can't continue */
      if (dri2_dpy->wl_drm_name == 0)
         return false;
      wl_drm_bind(dri2_dpy);

      if (dri2_dpy->wl_drm == NULL)
         return false;
      if (roundtrip(dri2_dpy) < 0 || dri2_dpy->fd_render_gpu == -1)
         return false;

      if (!dri2_dpy->authenticated &&
          (roundtrip(dri2_dpy) < 0 || !dri2_dpy->authenticated))
         return false;
   }
#endif
   return true;
}

static EGLBoolean
dri2_initialize_wayland_drm(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (dri2_wl_formats_init(&dri2_dpy->formats) < 0)
      goto cleanup;

   if (disp->PlatformDisplay == NULL) {
      dri2_dpy->wl_dpy = wl_display_connect(NULL);
      if (dri2_dpy->wl_dpy == NULL)
         goto cleanup;
      dri2_dpy->own_device = true;
   } else {
      dri2_dpy->wl_dpy = disp->PlatformDisplay;
   }

   dri2_dpy->wl_queue = wl_display_create_queue_with_name(dri2_dpy->wl_dpy,
                                                          "mesa egl display queue");

   dri2_dpy->wl_dpy_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_dpy);
   if (dri2_dpy->wl_dpy_wrapper == NULL)
      goto cleanup;

   wl_proxy_set_queue((struct wl_proxy *)dri2_dpy->wl_dpy_wrapper,
                      dri2_dpy->wl_queue);

   if (dri2_dpy->own_device)
      wl_display_dispatch_pending(dri2_dpy->wl_dpy);

   dri2_dpy->wl_registry = wl_display_get_registry(dri2_dpy->wl_dpy_wrapper);
   wl_registry_add_listener(dri2_dpy->wl_registry, &registry_listener_drm,
                            dri2_dpy);

   if (roundtrip(dri2_dpy) < 0)
      goto cleanup;

   if (!dri2_initialize_wayland_drm_extensions(dri2_dpy))
      goto cleanup;

   loader_get_user_preferred_fd(&dri2_dpy->fd_render_gpu,
                                &dri2_dpy->fd_display_gpu);

   if (dri2_dpy->fd_render_gpu != dri2_dpy->fd_display_gpu) {
      free(dri2_dpy->device_name);
      dri2_dpy->device_name =
         loader_get_device_name_for_fd(dri2_dpy->fd_render_gpu);
      if (!dri2_dpy->device_name) {
         _eglError(EGL_BAD_ALLOC, "wayland-egl: failed to get device name "
                                  "for requested GPU");
         goto cleanup;
      }
   }

   /* we have to do the check now, because loader_get_user_preferred_fd
    * will return a render-node when the requested gpu is different
    * to the server, but also if the client asks for the same gpu than
    * the server by requesting its pci-id */
   dri2_dpy->is_render_node =
      drmGetNodeTypeFromFd(dri2_dpy->fd_render_gpu) == DRM_NODE_RENDER;

   if (disp->Options.Zink)
      dri2_dpy->driver_name = strdup("zink");
   else
      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd_render_gpu);
   if (dri2_dpy->driver_name == NULL) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to get driver name");
      goto cleanup;
   }

   dri2_detect_swrast_kopper(disp);

   dri2_dpy->loader_extensions = dri2_dpy->kopper ? kopper_loader_extensions
                                                  : dri2_loader_extensions;

   if (!dri2_create_screen(disp))
      goto cleanup;

   if (!dri2_setup_device(disp, false)) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to setup EGLDevice");
      goto cleanup;
   }

   dri2_setup_screen(disp);

   dri2_wl_setup_swap_interval(disp);

#ifdef HAVE_BIND_WL_DISPLAY
   if (dri2_dpy->wl_drm) {
      /* To use Prime, we must have _DRI_IMAGE v7 at least.
       * createImageFromDmaBufs support indicates that Prime export/import is
       * supported by the driver. We deprecated the support to GEM names API, so
       * we bail out if the driver does not support Prime. */
      if (!(dri2_dpy->capabilities & WL_DRM_CAPABILITY_PRIME) ||
          !dri2_dpy->has_dmabuf_import) {
         _eglLog(_EGL_WARNING, "wayland-egl: display does not support prime");
         goto cleanup;
      }
   }

   dri2_set_WL_bind_wayland_display(disp);
   /* When cannot convert EGLImage to wl_buffer when on a different gpu,
    * because the buffer of the EGLImage has likely a tiling mode the server
    * gpu won't support. These is no way to check for now. Thus do not support
    * the extension */
   if (dri2_dpy->fd_render_gpu == dri2_dpy->fd_display_gpu)
      disp->Extensions.WL_create_wayland_buffer_from_image = EGL_TRUE;
#endif

   dri2_wl_add_configs_for_visuals(disp);

   disp->Extensions.EXT_buffer_age = EGL_TRUE;
   disp->Extensions.EXT_swap_buffers_with_damage = EGL_TRUE;
   disp->Extensions.EXT_present_opaque = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = dri2_dpy->kopper ? &dri2_wl_kopper_display_vtbl
                                     : &dri2_wl_display_vtbl;

   return EGL_TRUE;

cleanup:
   return EGL_FALSE;
}

static int
dri2_wl_swrast_get_stride_for_format(int format, int w)
{
   int visual_idx = dri2_wl_visual_idx_from_shm_format(format);

   assume(visual_idx != -1);

   return w * util_format_get_blocksize(dri2_wl_visuals[visual_idx].pipe_format);
}

static EGLBoolean
dri2_wl_swrast_allocate_buffer(struct dri2_egl_surface *dri2_surf, int format,
                               int w, int h, void **data, int *size,
                               struct wl_buffer **buffer)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   struct wl_shm_pool *pool;
   int fd, stride, size_map;
   void *data_map;

   assert(!*buffer);

   stride = dri2_wl_swrast_get_stride_for_format(format, w);
   size_map = h * stride;

   /* Create a shareable buffer */
   fd = os_create_anonymous_file(size_map, NULL);
   if (fd < 0)
      return EGL_FALSE;

   data_map = mmap(NULL, size_map, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (data_map == MAP_FAILED) {
      close(fd);
      return EGL_FALSE;
   }

   /* Share it in a wl_buffer */
   pool = wl_shm_create_pool(dri2_dpy->wl_shm, fd, size_map);
   wl_proxy_set_queue((struct wl_proxy *)pool, dri2_surf->wl_queue);
   *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
   wl_shm_pool_destroy(pool);
   close(fd);

   *data = data_map;
   *size = size_map;
   return EGL_TRUE;
}

static int
swrast_update_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   /* we need to do the following operations only once per frame */
   if (dri2_surf->back)
      return 0;

   if (dri2_surf->wl_win &&
       (dri2_surf->base.Width != dri2_surf->wl_win->width ||
        dri2_surf->base.Height != dri2_surf->wl_win->height)) {

      dri2_wl_release_buffers(dri2_surf);

      dri2_surf->base.Width = dri2_surf->wl_win->width;
      dri2_surf->base.Height = dri2_surf->wl_win->height;
      dri2_surf->dx = dri2_surf->wl_win->dx;
      dri2_surf->dy = dri2_surf->wl_win->dy;
      dri2_surf->current = NULL;
   }

   /* find back buffer */
   /* There might be a buffer release already queued that wasn't processed */
   wl_display_dispatch_queue_pending(dri2_dpy->wl_dpy, dri2_surf->wl_queue);

   /* else choose any another free location */
   while (!dri2_surf->back) {
      for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         if (!dri2_surf->color_buffers[i].locked) {
            dri2_surf->back = &dri2_surf->color_buffers[i];
            if (dri2_surf->back->wayland_buffer.buffer)
               break;

            if (!dri2_wl_swrast_allocate_buffer(
                   dri2_surf, dri2_surf->format, dri2_surf->base.Width,
                   dri2_surf->base.Height, &dri2_surf->back->data,
                   &dri2_surf->back->data_size,
                   &dri2_surf->back->wayland_buffer.buffer)) {
               _eglError(EGL_BAD_ALLOC, "failed to allocate color buffer");
               return -1;
            }
            wl_buffer_add_listener(dri2_surf->back->wayland_buffer.buffer,
                                   &wl_buffer_listener, dri2_surf);
            break;
         }
      }

      /* wait for the compositor to release a buffer */
      if (!dri2_surf->back) {
         if (loader_wayland_dispatch(dri2_dpy->wl_dpy, dri2_surf->wl_queue, NULL) ==
             -1) {
            _eglError(EGL_BAD_ALLOC, "waiting for a free buffer failed");
            return -1;
         }
      }
   }

   dri2_surf->back->locked = true;

   /* If we have an extra unlocked buffer at this point, we had to do triple
    * buffering for a while, but now can go back to just double buffering.
    * That means we can free any unlocked buffer now. To avoid toggling between
    * going back to double buffering and needing to allocate another buffer too
    * fast we let the unneeded buffer sit around for a short while. */
   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (!dri2_surf->color_buffers[i].locked &&
          dri2_surf->color_buffers[i].wayland_buffer.buffer &&
          dri2_surf->color_buffers[i].age > BUFFER_TRIM_AGE_HYSTERESIS) {
         loader_wayland_buffer_destroy(&dri2_surf->color_buffers[i].wayland_buffer);
         munmap(dri2_surf->color_buffers[i].data,
                dri2_surf->color_buffers[i].data_size);
         dri2_surf->color_buffers[i].data = NULL;
         dri2_surf->color_buffers[i].age = 0;
      }
   }

   return 0;
}

static void *
dri2_wl_swrast_get_frontbuffer_data(struct dri2_egl_surface *dri2_surf)
{
   /* if there has been a resize: */
   if (!dri2_surf->current)
      return NULL;

   return dri2_surf->current->data;
}

static void *
dri2_wl_swrast_get_backbuffer_data(struct dri2_egl_surface *dri2_surf)
{
   assert(dri2_surf->back);
   return dri2_surf->back->data;
}

static void
dri2_wl_swrast_commit_backbuffer(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   dri2_surf->wl_win->attached_width = dri2_surf->base.Width;
   dri2_surf->wl_win->attached_height = dri2_surf->base.Height;
   /* reset resize growing parameters */
   dri2_surf->dx = 0;
   dri2_surf->dy = 0;

   wl_surface_commit(dri2_surf->wayland_surface.wrapper);

   /* If we're not waiting for a frame callback then we'll at least throttle
    * to a sync callback so that we always give a chance for the compositor to
    * handle the commit and send a release event before checking for a free
    * buffer */
   if (dri2_surf->throttle_callback == NULL) {
      dri2_surf->throttle_callback = wl_display_sync(dri2_surf->wl_dpy_wrapper);
      wl_callback_add_listener(dri2_surf->throttle_callback, &throttle_listener,
                               dri2_surf);
   }

   wl_display_flush(dri2_dpy->wl_dpy);
}

static void
dri2_wl_swrast_get_drawable_info(struct dri_drawable *draw, int *x, int *y, int *w,
                                 int *h, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   (void)swrast_update_buffers(dri2_surf);
   *x = 0;
   *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static void
dri2_wl_swrast_get_image(struct dri_drawable *read, int x, int y, int w, int h,
                         char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int copy_width = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   int x_offset = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, x);
   int src_stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format,
                                                         dri2_surf->base.Width);
   int dst_stride = copy_width;
   char *src, *dst;

   src = dri2_wl_swrast_get_frontbuffer_data(dri2_surf);
   /* this is already the most up-to-date buffer */
   if (src == data)
      return;
   if (!src) {
      memset(data, 0, copy_width * h);
      return;
   }

   assert(copy_width <= src_stride);

   src += x_offset;
   src += y * src_stride;
   dst = data;

   if (copy_width > src_stride - x_offset)
      copy_width = src_stride - x_offset;
   if (h > dri2_surf->base.Height - y)
      h = dri2_surf->base.Height - y;

   for (; h > 0; h--) {
      memcpy(dst, src, copy_width);
      src += src_stride;
      dst += dst_stride;
   }
}

static void
dri2_wl_swrast_put_image2(struct dri_drawable *draw, int op, int x, int y, int w,
                          int h, int stride, char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   /* clamp to surface size */
   w = MIN2(w, dri2_surf->base.Width);
   h = MIN2(h, dri2_surf->base.Height);
   int copy_width = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   int dst_stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format,
                                                         dri2_surf->base.Width);
   int x_offset = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, x);
   char *src, *dst;

   assert(copy_width <= stride);

   dst = dri2_wl_swrast_get_backbuffer_data(dri2_surf);

   dst += x_offset;
   dst += y * dst_stride;

   src = data;

   /* drivers expect we do these checks (and some rely on it) */
   if (copy_width > dst_stride - x_offset)
      copy_width = dst_stride - x_offset;
   if (h > dri2_surf->base.Height - y)
      h = dri2_surf->base.Height - y;

   for (; h > 0; h--) {
      memcpy(dst, src, copy_width);
      src += stride;
      dst += dst_stride;
   }
}

static void
dri2_wl_swrast_put_image(struct dri_drawable *draw, int op, int x, int y, int w,
                         int h, char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int stride;

   stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   dri2_wl_swrast_put_image2(draw, op, x, y, w, h, stride, data, loaderPrivate);
}

static EGLBoolean
dri2_wl_swrast_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw,
                                        const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (!dri2_surf->wl_win)
      return _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_swap_buffers");

   (void)swrast_update_buffers(dri2_surf);

   if (dri2_wl_surface_throttle(dri2_surf))
      wl_surface_attach(dri2_surf->wayland_surface.wrapper,
         /* 'back' here will be promoted to 'current' */
         dri2_surf->back->wayland_buffer.buffer, dri2_surf->dx,
         dri2_surf->dy);

   /* If the compositor doesn't support damage_buffer, we deliberately
    * ignore the damage region and post maximum damage, due to
    * https://bugs.freedesktop.org/78190 */
   if (!try_damage_buffer(dri2_surf, rects, n_rects))
      wl_surface_damage(dri2_surf->wayland_surface.wrapper, 0, 0, INT32_MAX,
                        INT32_MAX);

   /* guarantee full copy for partial update */
   int w = n_rects == 1 ? (rects[2] - rects[0]) : 0;
   int copy_width = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   int dst_stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format,
                                                         dri2_surf->base.Width);
   char *dst = dri2_wl_swrast_get_backbuffer_data(dri2_surf);

   /* partial copy, copy old content */
   if (copy_width < dst_stride)
      dri2_wl_swrast_get_image(NULL, 0, 0, dri2_surf->base.Width,
                                 dri2_surf->base.Height, dst, dri2_surf);

   if (n_rects)
      driSwapBuffersWithDamage(dri2_surf->dri_drawable, n_rects, rects);
   else
      driSwapBuffers(dri2_surf->dri_drawable);

   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   dri2_wl_swrast_commit_backbuffer(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_swrast_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   dri2_wl_swrast_swap_buffers_with_damage(disp, draw, NULL, 0);
   return EGL_TRUE;
}

static EGLint
dri2_wl_swrast_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   assert(dri2_dpy->swrast);
   return driSWRastQueryBufferAge(dri2_surf->dri_drawable);
}

static void
shm_handle_format(void *data, struct wl_shm *shm, uint32_t format)
{
   struct dri2_egl_display *dri2_dpy = data;
   int visual_idx = dri2_wl_visual_idx_from_shm_format(format);

   if (visual_idx == -1)
      return;

   BITSET_SET(dri2_dpy->formats.formats_bitmap, visual_idx);
}

static const struct wl_shm_listener shm_listener = {
   .format = shm_handle_format,
};

static void
registry_handle_global_swrast(void *data, struct wl_registry *registry,
                              uint32_t name, const char *interface,
                              uint32_t version)
{
   struct dri2_egl_display *dri2_dpy = data;

   if (strcmp(interface, wl_shm_interface.name) == 0) {
      dri2_dpy->wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
      wl_shm_add_listener(dri2_dpy->wl_shm, &shm_listener, dri2_dpy);
   } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
      dri2_dpy->wp_presentation =
         wl_registry_bind(registry, name, &wp_presentation_interface, 1);
      wp_presentation_add_listener(dri2_dpy->wp_presentation,
                                   &presentation_listener, dri2_dpy);
   }

}

static const struct wl_registry_listener registry_listener_swrast = {
   .global = registry_handle_global_swrast,
   .global_remove = registry_handle_global_remove,
};

static const struct dri2_egl_display_vtbl dri2_wl_swrast_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = dri2_wl_create_window_surface,
   .create_pixmap_surface = dri2_wl_create_pixmap_surface,
   .destroy_surface = dri2_wl_destroy_surface,
   .swap_interval = dri2_wl_swap_interval,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_wl_swrast_swap_buffers,
   .swap_buffers_with_damage = dri2_wl_swrast_swap_buffers_with_damage,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
   .query_buffer_age = dri2_wl_swrast_query_buffer_age,
};

static const __DRIswrastLoaderExtension swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 2},

   .getDrawableInfo = dri2_wl_swrast_get_drawable_info,
   .putImage = dri2_wl_swrast_put_image,
   .getImage = dri2_wl_swrast_get_image,
   .putImage2 = dri2_wl_swrast_put_image2,
};

static const __DRIextension *swrast_loader_extensions[] = {
   &swrast_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

static EGLBoolean
dri2_initialize_wayland_swrast(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (dri2_wl_formats_init(&dri2_dpy->formats) < 0)
      goto cleanup;

   if (disp->PlatformDisplay == NULL) {
      dri2_dpy->wl_dpy = wl_display_connect(NULL);
      if (dri2_dpy->wl_dpy == NULL)
         goto cleanup;
      dri2_dpy->own_device = true;
   } else {
      dri2_dpy->wl_dpy = disp->PlatformDisplay;
   }

   dri2_dpy->wl_queue = wl_display_create_queue_with_name(dri2_dpy->wl_dpy,
                                                          "mesa egl swrast display queue");

   dri2_dpy->wl_dpy_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_dpy);
   if (dri2_dpy->wl_dpy_wrapper == NULL)
      goto cleanup;

   wl_proxy_set_queue((struct wl_proxy *)dri2_dpy->wl_dpy_wrapper,
                      dri2_dpy->wl_queue);

   if (dri2_dpy->own_device)
      wl_display_dispatch_pending(dri2_dpy->wl_dpy);

   dri2_dpy->wl_registry = wl_display_get_registry(dri2_dpy->wl_dpy_wrapper);
   wl_registry_add_listener(dri2_dpy->wl_registry, &registry_listener_swrast,
                            dri2_dpy);

   if (roundtrip(dri2_dpy) < 0 || dri2_dpy->wl_shm == NULL)
      goto cleanup;

   if (roundtrip(dri2_dpy) < 0 ||
       !BITSET_TEST_RANGE(dri2_dpy->formats.formats_bitmap, 0,
                          dri2_dpy->formats.num_formats))
      goto cleanup;

   dri2_dpy->driver_name = strdup(disp->Options.Zink ? "zink" : "swrast");
   dri2_detect_swrast_kopper(disp);

   dri2_dpy->loader_extensions = dri2_dpy->kopper ? kopper_loader_extensions
                                                  : swrast_loader_extensions;

   if (!dri2_create_screen(disp))
      goto cleanup;

   if (!dri2_setup_device(disp, disp->Options.ForceSoftware)) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to setup EGLDevice");
      goto cleanup;
   }

   dri2_setup_screen(disp);

   dri2_wl_setup_swap_interval(disp);

   dri2_wl_add_configs_for_visuals(disp);

#ifdef HAVE_BIND_WL_DISPLAY
   if (disp->Options.Zink && dri2_dpy->fd_render_gpu >= 0 &&
       (dri2_dpy->wl_dmabuf || dri2_dpy->wl_drm))
      dri2_set_WL_bind_wayland_display(disp);
#endif
   disp->Extensions.EXT_buffer_age = EGL_TRUE;
   disp->Extensions.EXT_swap_buffers_with_damage = EGL_TRUE;
   disp->Extensions.EXT_present_opaque = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = dri2_dpy->kopper ? &dri2_wl_kopper_display_vtbl
                                     : &dri2_wl_swrast_display_vtbl;

   return EGL_TRUE;

cleanup:
   return EGL_FALSE;
}

EGLBoolean
dri2_initialize_wayland(_EGLDisplay *disp)
{
   if (disp->Options.ForceSoftware)
      return dri2_initialize_wayland_swrast(disp);
   else
      return dri2_initialize_wayland_drm(disp);
}

void
dri2_teardown_wayland(struct dri2_egl_display *dri2_dpy)
{
   dri2_wl_formats_fini(&dri2_dpy->formats);
   if (dri2_dpy->wp_presentation)
      wp_presentation_destroy(dri2_dpy->wp_presentation);
#ifdef HAVE_BIND_WL_DISPLAY
   if (dri2_dpy->wl_drm)
      wl_drm_destroy(dri2_dpy->wl_drm);
#endif
   if (dri2_dpy->wl_dmabuf)
      zwp_linux_dmabuf_v1_destroy(dri2_dpy->wl_dmabuf);
   if (dri2_dpy->wl_shm)
      wl_shm_destroy(dri2_dpy->wl_shm);
   if (dri2_dpy->wl_registry)
      wl_registry_destroy(dri2_dpy->wl_registry);
   if (dri2_dpy->wl_dpy_wrapper)
      wl_proxy_wrapper_destroy(dri2_dpy->wl_dpy_wrapper);
   if (dri2_dpy->wl_queue)
      wl_event_queue_destroy(dri2_dpy->wl_queue);

   if (dri2_dpy->own_device)
      wl_display_disconnect(dri2_dpy->wl_dpy);
}
