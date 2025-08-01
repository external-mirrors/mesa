#ifndef _DRM_HELPER_PUBLIC_H
#define _DRM_HELPER_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

extern const struct drm_driver_descriptor i915_driver_descriptor;
extern const struct drm_driver_descriptor iris_driver_descriptor;
extern const struct drm_driver_descriptor crocus_driver_descriptor;
extern const struct drm_driver_descriptor nouveau_driver_descriptor;
extern const struct drm_driver_descriptor r300_driver_descriptor;
extern const struct drm_driver_descriptor r600_driver_descriptor;
extern const struct drm_driver_descriptor radeonsi_driver_descriptor;
extern const struct drm_driver_descriptor vmwgfx_driver_descriptor;
extern const struct drm_driver_descriptor kgsl_driver_descriptor;
extern const struct drm_driver_descriptor msm_driver_descriptor;
extern const struct drm_driver_descriptor virtio_gpu_driver_descriptor;
extern const struct drm_driver_descriptor v3d_driver_descriptor;
extern const struct drm_driver_descriptor vc4_driver_descriptor;
extern const struct drm_driver_descriptor panfrost_driver_descriptor;
extern const struct drm_driver_descriptor panthor_driver_descriptor;
extern const struct drm_driver_descriptor asahi_driver_descriptor;
extern const struct drm_driver_descriptor etnaviv_driver_descriptor;
extern const struct drm_driver_descriptor rknpu_driver_descriptor;
extern const struct drm_driver_descriptor rocket_driver_descriptor;
extern const struct drm_driver_descriptor tegra_driver_descriptor;
extern const struct drm_driver_descriptor lima_driver_descriptor;
extern const struct drm_driver_descriptor zink_driver_descriptor;
extern const struct drm_driver_descriptor kmsro_driver_descriptor;

#endif /* _DRM_HELPER_PUBLIC_H */
