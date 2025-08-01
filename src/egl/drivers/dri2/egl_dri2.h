/*
 * Copyright © 2011 Intel Corporation
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
 */

#ifndef EGL_DRI2_INCLUDED
#define EGL_DRI2_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_X11_PLATFORM
#include <X11/Xlib-xcb.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include "loader_dri_helper.h"
#ifdef HAVE_LIBDRM
#include "loader_dri3_helper.h"
#endif
#endif

#ifdef HAVE_WAYLAND_PLATFORM
#include "loader_wayland_helper.h"

/* forward declarations to avoid pulling wayland headers everywhere */
struct wl_egl_window;
struct wl_event_queue;
struct wl_callback;
struct wl_display;
struct wl_drm;
struct wl_registry;
struct wl_shm;
struct wl_surface;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_dmabuf_feedback_v1;
#endif

#include <GL/gl.h>
#include "mesa_interface.h"
#include "kopper_interface.h"

#ifdef HAVE_DRM_PLATFORM
#include <gbm_driint.h>
#endif

#ifdef HAVE_ANDROID_PLATFORM
#define LOG_TAG "EGL-DRI2"

#include <hardware/gralloc.h>

#include "util/u_gralloc/u_gralloc.h"

#if ANDROID_API_LEVEL >= 26
#include <vndk/window.h>
#else
#include <system/window.h>
#endif

#endif /* HAVE_ANDROID_PLATFORM */

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldevice.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglimage.h"
#include "egllog.h"
#include "eglsurface.h"
#include "eglsync.h"

#include "util/bitset.h"
#include "util/u_dynarray.h"
#include "util/u_vector.h"
#include "util/format/u_format.h"

struct wl_buffer;

struct dri2_egl_display_vtbl {
   /* mandatory on Wayland, unused otherwise */
   int (*authenticate)(_EGLDisplay *disp, uint32_t id);

   /* mandatory */
   _EGLSurface *(*create_window_surface)(_EGLDisplay *disp, _EGLConfig *config,
                                         void *native_window,
                                         const EGLint *attrib_list);

   /* optional */
   _EGLSurface *(*create_pixmap_surface)(_EGLDisplay *disp, _EGLConfig *config,
                                         void *native_pixmap,
                                         const EGLint *attrib_list);

   /* optional */
   _EGLSurface *(*create_pbuffer_surface)(_EGLDisplay *disp, _EGLConfig *config,
                                          const EGLint *attrib_list);

   /* mandatory */
   EGLBoolean (*destroy_surface)(_EGLDisplay *disp, _EGLSurface *surface);

   /* optional */
   EGLBoolean (*swap_interval)(_EGLDisplay *disp, _EGLSurface *surf,
                               EGLint interval);

   /* mandatory */
   _EGLImage *(*create_image)(_EGLDisplay *disp, _EGLContext *ctx,
                              EGLenum target, EGLClientBuffer buffer,
                              const EGLint *attr_list);

   /* mandatory */
   EGLBoolean (*swap_buffers)(_EGLDisplay *disp, _EGLSurface *surf);

   /* optional - falls back to .swap_buffers */
   EGLBoolean (*swap_buffers_with_damage)(_EGLDisplay *disp,
                                          _EGLSurface *surface,
                                          const EGLint *rects, EGLint n_rects);

   /* optional */
   EGLBoolean (*copy_buffers)(_EGLDisplay *disp, _EGLSurface *surf,
                              void *native_pixmap_target);

   /* optional */
   EGLint (*query_buffer_age)(_EGLDisplay *disp, _EGLSurface *surf);

   /* optional */
   EGLBoolean (*query_surface)(_EGLDisplay *disp, _EGLSurface *surf,
                               EGLint attribute, EGLint *value);

   /* optional */
   struct wl_buffer *(*create_wayland_buffer_from_image)(_EGLDisplay *disp,
                                                         _EGLImage *img);

   /* optional */
   EGLBoolean (*get_sync_values)(_EGLDisplay *display, _EGLSurface *surface,
                                 EGLuint64KHR *ust, EGLuint64KHR *msc,
                                 EGLuint64KHR *sbc);

   /* optional */
   EGLBoolean (*get_msc_rate)(_EGLDisplay *display, _EGLSurface *surface,
                              EGLint *numerator, EGLint *denominator);

   /* mandatory */
   struct dri_drawable *(*get_dri_drawable)(_EGLSurface *surf);

   /* optional */
   void (*close_screen_notify)(_EGLDisplay *disp);

   /* Used in EGL_KHR_mutable_render_buffer to update the native window's
    * shared buffer mode.
    * optional
    */
   bool (*set_shared_buffer_mode)(_EGLDisplay *disp, _EGLSurface *surf,
                                  bool mode);
};

#ifdef HAVE_WAYLAND_PLATFORM
struct dri2_wl_formats {
   unsigned int num_formats;

   /* Bitmap referencing dri2_wl_visuals */
   unsigned int *formats_bitmap;

   /* Array of vectors. Contains one modifier vector per format */
   struct u_vector *modifiers;
};

struct dmabuf_feedback_format_table {
   unsigned int size;
   struct {
      uint32_t format;
      uint32_t padding; /* unused */
      uint64_t modifier;
   } *data;
};

struct dmabuf_feedback_tranche {
   dev_t target_device;
   uint32_t flags;
   struct dri2_wl_formats formats;
};

struct dmabuf_feedback {
   dev_t main_device;
   struct dmabuf_feedback_format_table format_table;
   struct util_dynarray tranches;
   struct dmabuf_feedback_tranche pending_tranche;
};
#endif

struct dri2_egl_display {
   const struct dri2_egl_display_vtbl *vtbl;

   mtx_t lock;

   struct dri_screen *dri_screen_render_gpu;
   /* dri_screen_display_gpu holds display GPU in case of prime gpu offloading
    * else dri_screen_render_gpu and dri_screen_display_gpu is same. In case of
    * prime gpu offloading, if display and render driver names are different
    * (potentially not compatible), dri_screen_display_gpu will be NULL but
    * fd_display_gpu will still hold fd for display driver.
    */
   struct dri_screen *dri_screen_display_gpu;
   bool own_dri_screen;
   const struct dri_config **driver_configs;
   /* fd of the GPU used for rendering. */
   int fd_render_gpu;
   /* fd of the GPU used for display. If the same GPU is used for display
    * and rendering, then fd_render_gpu == fd_display_gpu (no need to use
    * os_same_file_description).
    */
   int fd_display_gpu;

   /* dri2_initialize/dri2_terminate increment/decrement this count, so does
    * dri2_make_current (tracks if there are active contexts/surfaces). */
   int ref_count;

   bool has_compression_modifiers;
   bool own_device;
   bool kopper;
   bool swrast;
   bool swrast_not_kms;
   int min_swap_interval;
   int max_swap_interval;
   int default_swap_interval;
#ifdef HAVE_DRM_PLATFORM
   struct gbm_dri_device *gbm_dri;
#endif

   char *driver_name;

   const __DRIextension **loader_extensions;

   bool has_dmabuf_import;
   bool has_dmabuf_export;
   bool explicit_modifiers;
   bool multibuffers_available;
#ifdef HAVE_X11_PLATFORM
   xcb_connection_t *conn;
   xcb_screen_t *screen;
   bool swap_available;
#ifdef HAVE_LIBDRM
   struct loader_screen_resources screen_resources;
#endif
#endif

#ifdef HAVE_WAYLAND_PLATFORM
   struct wl_display *wl_dpy;
   struct wl_display *wl_dpy_wrapper;
   struct wl_registry *wl_registry;
#ifdef HAVE_BIND_WL_DISPLAY
   struct wl_drm *wl_server_drm;
   struct wl_drm *wl_drm;
   uint32_t wl_drm_version, wl_drm_name;
   bool authenticated;
   uint32_t capabilities;
#endif
   struct wl_shm *wl_shm;
   struct wl_event_queue *wl_queue;
   struct zwp_linux_dmabuf_v1 *wl_dmabuf;
   struct wp_presentation *wp_presentation;
   struct dri2_wl_formats formats;
   struct zwp_linux_dmabuf_feedback_v1 *wl_dmabuf_feedback;
   struct dmabuf_feedback_format_table format_table;
   char *device_name;
   bool is_render_node;
   clockid_t presentation_clock_id;
#endif

#ifdef HAVE_ANDROID_PLATFORM
   struct u_gralloc *gralloc;
   /* gralloc vendor usage bit for front rendering */
   uint32_t front_rendering_usage;
   bool has_native_fence_fd;
   bool pure_swrast;
#endif
};

struct dri2_egl_context {
   _EGLContext base;
   struct dri_context *dri_context;
};

struct dri2_egl_surface {
   _EGLSurface base;
   struct dri_drawable *dri_drawable;
   __DRIbuffer buffers[5];
   bool have_fake_front;

#ifdef HAVE_X11_PLATFORM
   xcb_drawable_t drawable;
   xcb_xfixes_region_t region;
   int depth;
   int bytes_per_pixel;
   xcb_gcontext_t gc;
   xcb_gcontext_t swapgc;
#endif

#ifdef HAVE_WAYLAND_PLATFORM
   struct wl_egl_window *wl_win;
   int dx;
   int dy;
   struct wl_event_queue *wl_queue;
   struct loader_wayland_surface wayland_surface;
   struct wl_display *wl_dpy_wrapper;
   struct wl_drm *wl_drm_wrapper;
   struct wl_callback *throttle_callback;
   struct zwp_linux_dmabuf_feedback_v1 *wl_dmabuf_feedback;
   struct dmabuf_feedback dmabuf_feedback, pending_dmabuf_feedback;
   struct loader_wayland_presentation wayland_presentation;
   bool compositor_using_another_device;
   int format;
   bool resized;
   bool received_dmabuf_feedback;
#endif

#ifdef HAVE_DRM_PLATFORM
   struct gbm_dri_surface *gbm_surf;
#endif

#if defined(HAVE_WAYLAND_PLATFORM) || defined(HAVE_DRM_PLATFORM)
   struct {
#ifdef HAVE_WAYLAND_PLATFORM
      struct loader_wayland_buffer wayland_buffer;
      bool wl_release;
      struct dri_image *dri_image;
      /* for is_different_gpu case. NULL else */
      struct dri_image *linear_copy;
      /* for swrast */
      void *data;
      int data_size;
#endif
#ifdef HAVE_DRM_PLATFORM
      struct gbm_bo *bo;
#endif
      bool locked;
      int age;
   } color_buffers[4], *back, *current;
#endif

#ifdef HAVE_ANDROID_PLATFORM
   struct ANativeWindow *window;
   struct ANativeWindowBuffer *buffer;

   /* in-fence associated with buffer, -1 once passed down to dri layer: */
   int in_fence_fd;

   struct dri_image *dri_image_back;
   struct dri_image *dri_image_front;

   /* Used to record all the buffers created by ANativeWindow and their ages.
    * Allocate number of color_buffers based on query to android bufferqueue
    * and save color_buffers_count.
    */
   int color_buffers_count;
   struct {
      struct ANativeWindowBuffer *buffer;
      int age;
   } *color_buffers, *back;
   uint32_t gralloc_usage;
#endif

   /* surfaceless and device */
   struct dri_image *front;
   enum pipe_format visual;

   int out_fence_fd;
   EGLBoolean enable_out_fence;

   /* swrast device */
   char *swrast_device_buffer;
};

struct dri2_egl_config {
   _EGLConfig base;
   const struct dri_config *dri_config[2][2];
};

struct dri2_egl_image {
   _EGLImage base;
   struct dri_image *dri_image;
};

struct dri2_egl_sync {
   _EGLSync base;
   mtx_t mutex;
   cnd_t cond;
   int refcount;
   void *fence;
};

/* standard typecasts */
_EGL_DRIVER_STANDARD_TYPECASTS(dri2_egl)
_EGL_DRIVER_TYPECAST(dri2_egl_image, _EGLImage, obj)
_EGL_DRIVER_TYPECAST(dri2_egl_sync, _EGLSync, obj)

static inline struct dri2_egl_display *
dri2_egl_display_lock(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (dri2_dpy)
      mtx_lock(&dri2_dpy->lock);

   return dri2_dpy;
}

static inline EGLBoolean
dri2_egl_error_unlock(struct dri2_egl_display *dri2_dpy, EGLint err,
                      const char *msg)
{
   mtx_unlock(&dri2_dpy->lock);
   return _eglError(err, msg);
}

extern const __DRIimageLookupExtension image_lookup_extension;
extern const __DRIswrastLoaderExtension swrast_pbuffer_loader_extension;
extern const __DRIkopperLoaderExtension kopper_pbuffer_loader_extension;

void
dri2_detect_swrast_kopper(_EGLDisplay *disp);

/* Helper for platforms not using dri2_create_screen */
void
dri2_setup_screen(_EGLDisplay *disp);

void
dri2_setup_swap_interval(_EGLDisplay *disp, int max_swap_interval);

EGLBoolean
dri2_create_screen(_EGLDisplay *disp);

EGLBoolean
dri2_setup_device(_EGLDisplay *disp, EGLBoolean software);

struct dri_drawable *
dri2_surface_get_dri_drawable(_EGLSurface *surf);

GLboolean
dri2_validate_egl_image(void *image, void *data);

struct dri_image *
dri2_lookup_egl_image_validated(void *image, void *data);

void
dri2_get_shifts_and_sizes(const struct dri_config *config, int *shifts,
                          unsigned int *sizes);

enum pipe_format
dri2_image_format_for_pbuffer_config(struct dri2_egl_display *dri2_dpy,
                                     const struct dri_config *config);

struct dri2_egl_config *
dri2_add_config(_EGLDisplay *disp, const struct dri_config *dri_config,
                EGLint surface_type, const EGLint *attr_list);

void
dri2_add_pbuffer_configs_for_visuals(_EGLDisplay *disp);

EGLint
dri2_from_dri_compression_rate(enum __DRIFixedRateCompression rate);

enum __DRIFixedRateCompression
dri2_to_dri_compression_rate(EGLint rate);

_EGLImage *
dri2_create_image_khr(_EGLDisplay *disp, _EGLContext *ctx, EGLenum target,
                      EGLClientBuffer buffer, const EGLint *attr_list);

_EGLImage *
dri2_create_image_dma_buf(_EGLDisplay *disp, _EGLContext *ctx,
                          EGLClientBuffer buffer, const EGLint *attr_list);

_EGLImage *
dri2_create_image_from_dri(_EGLDisplay *disp, struct dri_image *dri_image);

#ifdef HAVE_X11_PLATFORM
EGLBoolean
dri2_initialize_x11(_EGLDisplay *disp);
void
dri2_teardown_x11(struct dri2_egl_display *dri2_dpy);
unsigned int
dri2_x11_get_red_mask_for_depth(struct dri2_egl_display *dri2_dpy, int depth);
#else
static inline EGLBoolean
dri2_initialize_x11(_EGLDisplay *disp)
{
   return _eglError(EGL_NOT_INITIALIZED, "X11 platform not built");
}
static inline void
dri2_teardown_x11(struct dri2_egl_display *dri2_dpy)
{
}
static inline unsigned int
dri2_x11_get_red_mask_for_depth(struct dri2_egl_display *dri2_dpy, int depth)
{
   return 0;
}
#endif

#ifdef HAVE_DRM_PLATFORM
EGLBoolean
dri2_initialize_drm(_EGLDisplay *disp);
void
dri2_teardown_drm(struct dri2_egl_display *dri2_dpy);
#else
static inline EGLBoolean
dri2_initialize_drm(_EGLDisplay *disp)
{
   return _eglError(EGL_NOT_INITIALIZED, "GBM/DRM platform not built");
}
static inline void
dri2_teardown_drm(struct dri2_egl_display *dri2_dpy)
{
}
#endif

#ifdef HAVE_WAYLAND_PLATFORM
EGLBoolean
dri2_initialize_wayland(_EGLDisplay *disp);
void
dri2_teardown_wayland(struct dri2_egl_display *dri2_dpy);
bool
dri2_wl_is_format_supported(void *user_data, uint32_t format);
#else
static inline EGLBoolean
dri2_initialize_wayland(_EGLDisplay *disp)
{
   return _eglError(EGL_NOT_INITIALIZED, "Wayland platform not built");
}
static inline void
dri2_teardown_wayland(struct dri2_egl_display *dri2_dpy)
{
}
#endif

#ifdef HAVE_ANDROID_PLATFORM
EGLBoolean
dri2_initialize_android(_EGLDisplay *disp);
#else
static inline EGLBoolean
dri2_initialize_android(_EGLDisplay *disp)
{
   return _eglError(EGL_NOT_INITIALIZED, "Android platform not built");
}
#endif

EGLBoolean
dri2_initialize_surfaceless(_EGLDisplay *disp);

EGLBoolean
dri2_initialize_device(_EGLDisplay *disp);
static inline void
dri2_teardown_device(struct dri2_egl_display *dri2_dpy)
{ /* noop */
}

void
dri2_flush_drawable_for_swapbuffers_flags(
   _EGLDisplay *disp, _EGLSurface *draw,
   enum __DRI2throttleReason throttle_reason);
void
dri2_flush_drawable_for_swapbuffers(_EGLDisplay *disp, _EGLSurface *draw);

const struct dri_config *
dri2_get_dri_config(struct dri2_egl_config *conf, EGLint surface_type,
                    EGLenum colorspace);
#include "dri_util.h"
static inline void
dri2_set_WL_bind_wayland_display(_EGLDisplay *disp)
{
#ifdef HAVE_BIND_WL_DISPLAY
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   disp->Extensions.WL_bind_wayland_display =
      dri2_dpy->has_dmabuf_import && dri2_dpy->has_dmabuf_export;
#endif
}

void
dri2_display_destroy(_EGLDisplay *disp);

struct dri2_egl_display *
dri2_display_create(_EGLDisplay *disp);

EGLBoolean
dri2_init_surface(_EGLSurface *surf, _EGLDisplay *disp, EGLint type,
                  _EGLConfig *conf, const EGLint *attrib_list,
                  EGLBoolean enable_out_fence, void *native_surface);

void
dri2_fini_surface(_EGLSurface *surf);

EGLBoolean
dri2_create_drawable(struct dri2_egl_display *dri2_dpy,
                     const struct dri_config *config,
                     struct dri2_egl_surface *dri2_surf, void *loaderPrivate);

static inline uint64_t
combine_u32_into_u64(uint32_t hi, uint32_t lo)
{
   return (((uint64_t)hi) << 32) | (((uint64_t)lo) & 0xffffffff);
}

#endif /* EGL_DRI2_INCLUDED */
