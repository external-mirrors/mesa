/*
 * Copyright © 2019 Raspberry Pi Ltd
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "v3dv_private.h"

#include "drm-uapi/drm_fourcc.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "vk_util.h"
#include "vulkan/wsi/wsi_common.h"
#include "vk_android.h"

/**
 * Computes the HW's UIFblock padding for a given height/cpp.
 *
 * The goal of the padding is to keep pages of the same color (bank number) at
 * least half a page away from each other vertically when crossing between
 * columns of UIF blocks.
 */
static uint32_t
v3d_get_ub_pad(uint32_t cpp, uint32_t height)
{
   uint32_t utile_h = v3d_utile_height(cpp);
   uint32_t uif_block_h = utile_h * 2;
   uint32_t height_ub = height / uif_block_h;

   uint32_t height_offset_in_pc = height_ub % PAGE_CACHE_UB_ROWS;

   /* For the perfectly-aligned-for-UIF-XOR case, don't add any pad. */
   if (height_offset_in_pc == 0)
      return 0;

   /* Try padding up to where we're offset by at least half a page. */
   if (height_offset_in_pc < PAGE_UB_ROWS_TIMES_1_5) {
      /* If we fit entirely in the page cache, don't pad. */
      if (height_ub < PAGE_CACHE_UB_ROWS)
         return 0;
      else
         return PAGE_UB_ROWS_TIMES_1_5 - height_offset_in_pc;
   }

   /* If we're close to being aligned to page cache size, then round up
    * and rely on XOR.
    */
   if (height_offset_in_pc > PAGE_CACHE_MINUS_1_5_UB_ROWS)
      return PAGE_CACHE_UB_ROWS - height_offset_in_pc;

   /* Otherwise, we're far enough away (top and bottom) to not need any
    * padding.
    */
   return 0;
}

/**
 * Computes the dimension with required padding for mip levels.
 *
 * This padding is required for width and height dimensions when the mip
 * level is greater than 1, and for the depth dimension when the mip level
 * is greater than 0. This function expects to be passed a mip level >= 1.
 *
 * Note: Hardware documentation seems to suggest that the third argument
 * should be the utile dimensions, but through testing it was found that
 * the block dimension should be used instead.
 */
static uint32_t
v3d_get_dimension_mpad(uint32_t dimension, uint32_t level, uint32_t block_dimension)
{
   assert(level >= 1);
   uint32_t pot_dim = u_minify(dimension, 1);
   pot_dim = util_next_power_of_two(DIV_ROUND_UP(pot_dim, block_dimension));
   uint32_t padded_dim = block_dimension * pot_dim;
   return u_minify(padded_dim, level - 1);
}

static bool
v3d_setup_plane_slices(struct v3dv_image *image, uint8_t plane,
                       uint32_t plane_offset,
                       const VkSubresourceLayout *plane_layouts)
{
   assert(image->planes[plane].cpp > 0);

   uint32_t width = image->planes[plane].width;
   uint32_t height = image->planes[plane].height;
   uint32_t depth = image->vk.extent.depth;

   uint32_t utile_w = v3d_utile_width(image->planes[plane].cpp);
   uint32_t utile_h = v3d_utile_height(image->planes[plane].cpp);
   uint32_t uif_block_w = utile_w * 2;
   uint32_t uif_block_h = utile_h * 2;

   uint32_t block_width = vk_format_get_blockwidth(image->vk.format);
   uint32_t block_height = vk_format_get_blockheight(image->vk.format);

   /* Note that power-of-two padding is based on level 1.  These are not
    * equivalent to just util_next_power_of_two(dimension), because at a
    * level 0 dimension of 9, the level 1 power-of-two padded value is 4,
    * not 8. Additionally the pot padding is based on the block size.
    */
   uint32_t pot_width = 2 * v3d_get_dimension_mpad(width,
                                                   1,
                                                   block_width);
   uint32_t pot_height = 2 * v3d_get_dimension_mpad(height,
                                                    1,
                                                    block_height);
   uint32_t pot_depth = 2 * v3d_get_dimension_mpad(depth,
                                                   1,
                                                   1);

   assert(image->vk.samples == VK_SAMPLE_COUNT_1_BIT ||
          image->vk.samples == VK_SAMPLE_COUNT_4_BIT);
   bool msaa = image->vk.samples != VK_SAMPLE_COUNT_1_BIT;

   bool uif_top = msaa;

   assert(image->vk.array_layers > 0);
   assert(depth > 0);
   assert(image->vk.mip_levels >= 1);

   /* Texture Base Address needs to be 64-byte aligned. If we have an explicit
    * plane layout we will return false to fail image creation with appropriate
    * error code.
    */
   uint32_t offset;
   if (plane_layouts) {
      offset = plane_layouts[plane].offset;
      if (offset % 64 != 0)
         return false;
   } else {
      offset = plane_offset;
   }
   assert(plane_offset % 64 == 0);

   for (int32_t i = image->vk.mip_levels - 1; i >= 0; i--) {
      struct v3d_resource_slice *slice = &image->planes[plane].slices[i];

      slice->width = u_minify(width, i);
      slice->height = u_minify(height, i);

      uint32_t level_width, level_height, level_depth;
      if (i < 2) {
         level_width = slice->width;
         level_height = slice->height;
      } else {
         level_width = u_minify(pot_width, i);
         level_height = u_minify(pot_height, i);
      }

      if (i < 1)
         level_depth = u_minify(depth, i);
      else
         level_depth = u_minify(pot_depth, i);

      if (msaa) {
         level_width *= 2;
         level_height *= 2;
      }

      level_width = DIV_ROUND_UP(level_width, block_width);
      level_height = DIV_ROUND_UP(level_height, block_height);

      if (!image->tiled) {
         slice->tiling = V3D_TILING_RASTER;
         if (image->vk.image_type == VK_IMAGE_TYPE_1D)
            level_width = align(level_width, 64 / image->planes[plane].cpp);
      } else {
         if ((i != 0 || !uif_top) &&
             (level_width <= utile_w || level_height <= utile_h)) {
            slice->tiling = V3D_TILING_LINEARTILE;
            level_width = align(level_width, utile_w);
            level_height = align(level_height, utile_h);
         } else if ((i != 0 || !uif_top) && level_width <= uif_block_w) {
            slice->tiling = V3D_TILING_UBLINEAR_1_COLUMN;
            level_width = align(level_width, uif_block_w);
            level_height = align(level_height, uif_block_h);
         } else if ((i != 0 || !uif_top) && level_width <= 2 * uif_block_w) {
            slice->tiling = V3D_TILING_UBLINEAR_2_COLUMN;
            level_width = align(level_width, 2 * uif_block_w);
            level_height = align(level_height, uif_block_h);
         } else {
            /* We align the width to a 4-block column of UIF blocks, but we
             * only align height to UIF blocks.
             */
            level_width = align(level_width, 4 * uif_block_w);
            level_height = align(level_height, uif_block_h);

            slice->ub_pad = v3d_get_ub_pad(image->planes[plane].cpp,
                                           level_height);
            level_height += slice->ub_pad * uif_block_h;

            /* If the padding set us to to be aligned to the page cache size,
             * then the HW will use the XOR bit on odd columns to get us
             * perfectly misaligned.
             */
            if ((level_height / uif_block_h) %
                (V3D_PAGE_CACHE_SIZE / V3D_UIFBLOCK_ROW_SIZE) == 0) {
               slice->tiling = V3D_TILING_UIF_XOR;
            } else {
               slice->tiling = V3D_TILING_UIF_NO_XOR;
            }
         }
      }

      slice->offset = offset;
      slice->stride = level_width * image->planes[plane].cpp;

      /* We assume that rowPitch in the plane layout refers to level 0 */
      if (plane_layouts && i == 0) {
         if (plane_layouts[plane].rowPitch < slice->stride)
            return false;
         if (plane_layouts[plane].rowPitch % image->planes[plane].cpp)
            return false;
         if (image->tiled && (plane_layouts[plane].rowPitch % (4 * uif_block_w)))
            return false;
         slice->stride = plane_layouts[plane].rowPitch;
      }

      slice->padded_height = level_height;
      if (slice->tiling == V3D_TILING_UIF_NO_XOR ||
          slice->tiling == V3D_TILING_UIF_XOR) {
         slice->padded_height_of_output_image_in_uif_blocks =
            slice->padded_height /
               (2 * v3d_utile_height(image->planes[plane].cpp));
      }

      slice->size = level_height * slice->stride;
      uint32_t slice_total_size = slice->size * level_depth;

      /* The HW aligns level 1's base to a page if any of level 1 or
       * below could be UIF XOR.  The lower levels then inherit the
       * alignment for as long as necessary, thanks to being power of
       * two aligned.
       */
      if (i == 1 &&
          level_width > 4 * uif_block_w &&
          level_height > PAGE_CACHE_MINUS_1_5_UB_ROWS * uif_block_h) {
         slice_total_size = align(slice_total_size, V3D_UIFCFG_PAGE_SIZE);
      }

      offset += slice_total_size;
   }

   image->planes[plane].size = offset - plane_offset;

   /* UIF/UBLINEAR levels need to be aligned to UIF-blocks, and LT only
    * needs to be aligned to utile boundaries.  Since tiles are laid out
    * from small to big in memory, we need to align the later UIF slices
    * to UIF blocks, if they were preceded by non-UIF-block-aligned LT
    * slices.
    *
    * We additionally align to 4k, which improves UIF XOR performance.
    *
    * Finally, because the Texture Base Address field must be 64-byte aligned,
    * we also need to align linear images to 64 if the image is going to be
    * used for transfer.
    */
   if (image->tiled) {
      image->planes[plane].alignment = 4096;
   } else {
      image->planes[plane].alignment =
         (image->vk.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ?
            64 : image->planes[plane].cpp;
   }

   uint32_t align_offset =
      align(image->planes[plane].slices[0].offset,
            image->planes[plane].alignment) -
            image->planes[plane].slices[0].offset;
   if (align_offset) {
      image->planes[plane].size += align_offset;
      for (int i = 0; i < image->vk.mip_levels; i++)
         image->planes[plane].slices[i].offset += align_offset;
   }

   /* Arrays and cube textures have a stride which is the distance from
    * one full mipmap tree to the next (64b aligned).  For 3D textures,
    * we need to program the stride between slices of miplevel 0.
    */
   if (image->vk.image_type != VK_IMAGE_TYPE_3D) {
      image->planes[plane].cube_map_stride =
         align(image->planes[plane].slices[0].offset +
               image->planes[plane].slices[0].size, 64);

      if (plane_layouts && image->vk.array_layers > 1) {
         if (plane_layouts[plane].arrayPitch % 64 != 0)
            return false;
         if (plane_layouts[plane].arrayPitch <
             image->planes[plane].cube_map_stride) {
            return false;
         }
         image->planes[plane].cube_map_stride = plane_layouts[plane].arrayPitch;
      }

      image->planes[plane].size += image->planes[plane].cube_map_stride *
                                   (image->vk.array_layers - 1);
   } else {
      image->planes[plane].cube_map_stride = image->planes[plane].slices[0].size;
      if (plane_layouts) {
         /* We assume that depthPitch in the plane layout refers to level 0 */
         if (plane_layouts[plane].depthPitch !=
             image->planes[plane].slices[0].size) {
               return false;
         }
      }
   }

   return true;
}

static VkResult
v3d_setup_slices(struct v3dv_image *image, bool disjoint,
                 const VkSubresourceLayout *plane_layouts)
{
   if (disjoint && image->plane_count == 1)
      disjoint = false;

   uint64_t offset = 0;
   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      offset = disjoint ? 0 : offset;
      if (!v3d_setup_plane_slices(image, plane, offset, plane_layouts)) {
         assert(plane_layouts);
         return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
      }
      offset += align64(image->planes[plane].size, 64);
   }

   /* From the Vulkan spec:
    *
    *   "If the size of the resultant image would exceed maxResourceSize, then
    *    vkCreateImage must fail and return VK_ERROR_OUT_OF_DEVICE_MEMORY. This
    *    failure may occur even when all image creation parameters satisfy their
    *    valid usage requirements."
    */
   if (offset > 0xffffffff)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   image->non_disjoint_size = disjoint ? 0 : offset;
   return VK_SUCCESS;
}

uint32_t
v3dv_layer_offset(const struct v3dv_image *image, uint32_t level, uint32_t layer,
                  uint8_t plane)
{
   const struct v3d_resource_slice *slice = &image->planes[plane].slices[level];

   if (image->vk.image_type == VK_IMAGE_TYPE_3D)
      return image->planes[plane].mem_offset + slice->offset + layer * slice->size;
   else
      return image->planes[plane].mem_offset + slice->offset +
         layer * image->planes[plane].cube_map_stride;
}

VkResult
v3dv_update_image_layout(struct v3dv_device *device,
                         struct v3dv_image *image,
                         uint64_t modifier,
                         bool disjoint,
                         const VkImageDrmFormatModifierExplicitCreateInfoEXT *explicit_mod_info)
{
   assert(!explicit_mod_info ||
          image->plane_count == explicit_mod_info->drmFormatModifierPlaneCount);

   assert(!explicit_mod_info ||
          modifier == explicit_mod_info->drmFormatModifier);

   image->tiled = modifier != DRM_FORMAT_MOD_LINEAR;

   image->vk.drm_format_mod = modifier;

   return v3d_setup_slices(image, disjoint,
                           explicit_mod_info ? explicit_mod_info->pPlaneLayouts :
                                               NULL);
}

VkResult
v3dv_image_init(struct v3dv_device *device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                struct v3dv_image *image)
{
   /* When using the simulator the WSI common code will see that our
    * driver wsi device doesn't match the display device and because of that
    * it will not attempt to present directly from the swapchain images,
    * instead it will use the prime blit path (use_buffer_blit flag in
    * struct wsi_swapchain), where it copies the contents of the swapchain
    * images to a linear buffer with appropriate row stride for presentation.
    * As a result, on that path, swapchain images do not have any special
    * requirements and are not created with the pNext structs below.
    */
   VkImageTiling tiling = pCreateInfo->tiling;
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   const VkImageDrmFormatModifierListCreateInfoEXT *mod_info = NULL;
   const VkImageDrmFormatModifierExplicitCreateInfoEXT *explicit_mod_info = NULL;

   /* This section is removed by the optimizer for non-ANDROID builds */
   VkImageDrmFormatModifierExplicitCreateInfoEXT eci;
   VkSubresourceLayout a_plane_layouts[V3DV_MAX_PLANE_COUNT];
   if (vk_image_is_android_native_buffer(&image->vk)) {
      VkResult result = vk_android_get_anb_layout(
         pCreateInfo, &eci, a_plane_layouts, V3DV_MAX_PLANE_COUNT);
      if (result != VK_SUCCESS)
         return result;

      explicit_mod_info = &eci;
      modifier = eci.drmFormatModifier;
   }

   if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      mod_info =
         vk_find_struct_const(pCreateInfo->pNext,
                              IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
      explicit_mod_info =
         vk_find_struct_const(pCreateInfo->pNext,
                              IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
      assert(mod_info || explicit_mod_info);

      if (mod_info) {
         for (uint32_t i = 0; i < mod_info->drmFormatModifierCount; i++) {
            switch (mod_info->pDrmFormatModifiers[i]) {
            case DRM_FORMAT_MOD_LINEAR:
               if (modifier == DRM_FORMAT_MOD_INVALID)
                  modifier = DRM_FORMAT_MOD_LINEAR;
               break;
            case DRM_FORMAT_MOD_BROADCOM_UIF:
               modifier = DRM_FORMAT_MOD_BROADCOM_UIF;
               break;
            }
         }
      } else {
         modifier = explicit_mod_info->drmFormatModifier;
      }
      assert(modifier == DRM_FORMAT_MOD_LINEAR ||
             modifier == DRM_FORMAT_MOD_BROADCOM_UIF);
   } else if (pCreateInfo->imageType == VK_IMAGE_TYPE_1D ||
              image->vk.wsi_legacy_scanout) {
      tiling = VK_IMAGE_TILING_LINEAR;
   }

   if (modifier == DRM_FORMAT_MOD_INVALID)
      modifier = (tiling == VK_IMAGE_TILING_OPTIMAL) ? DRM_FORMAT_MOD_BROADCOM_UIF
                                                     : DRM_FORMAT_MOD_LINEAR;

   const struct v3dv_format *format =
      v3d_X((&device->devinfo), get_format)(image->vk.format);
   v3dv_assert(format != NULL && format->plane_count);

   assert(pCreateInfo->samples == VK_SAMPLE_COUNT_1_BIT ||
          pCreateInfo->samples == VK_SAMPLE_COUNT_4_BIT);

   image->format = format;

   image->plane_count = vk_format_get_plane_count(image->vk.format);

   const struct vk_format_ycbcr_info *ycbcr_info =
      vk_format_get_ycbcr_info(image->vk.format);

   for (uint8_t plane = 0; plane < image->plane_count; plane++) {
      VkFormat plane_format =
         vk_format_get_plane_format(image->vk.format, plane);
      image->planes[plane].cpp =
         vk_format_get_blocksize(plane_format);
      image->planes[plane].vk_format = plane_format;

      image->planes[plane].width = image->vk.extent.width;
      image->planes[plane].height = image->vk.extent.height;

      if (ycbcr_info) {
         image->planes[plane].width /=
            ycbcr_info->planes[plane].denominator_scales[0];

         image->planes[plane].height /=
            ycbcr_info->planes[plane].denominator_scales[1];
      }
   }

   /* Our meta paths can create image views with compatible formats for any
    * image, so always set this flag to keep the common Vulkan image code
    * happy.
    */
   image->vk.create_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

   /* At this time, an AHB handle is not yet provided.
    * Image layout will be filled up during vkBindImageMemory2
    * This section is removed by the optimizer for non-ANDROID builds
    */
   if (vk_image_is_android_hardware_buffer(&image->vk))
      return VK_SUCCESS;

   bool disjoint = image->vk.create_flags & VK_IMAGE_CREATE_DISJOINT_BIT;

   return v3dv_update_image_layout(device, image, modifier, disjoint,
                                   explicit_mod_info);
}

static VkResult
create_image(struct v3dv_device *device,
             const VkImageCreateInfo *pCreateInfo,
             const VkAllocationCallbacks *pAllocator,
             VkImage *pImage)
{
#if !DETECT_OS_ANDROID
   const VkImageSwapchainCreateInfoKHR *swapchain_info =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_SWAPCHAIN_CREATE_INFO_KHR);
   if (swapchain_info && swapchain_info->swapchain != VK_NULL_HANDLE) {
      return wsi_common_create_swapchain_image(&device->pdevice->wsi_device,
                                               pCreateInfo,
                                               swapchain_info->swapchain,
                                               pImage);
   }
#endif

   VkResult result;
   struct v3dv_image *image = NULL;

   image = vk_image_create(&device->vk, pCreateInfo, pAllocator, sizeof(*image));
   if (image == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = v3dv_image_init(device, pCreateInfo, pAllocator, image);
   if (result != VK_SUCCESS)
      goto fail;

   /* This section is removed by the optimizer for non-ANDROID builds */
   if (vk_image_is_android_native_buffer(&image->vk)) {
      result = vk_android_import_anb(&device->vk, pCreateInfo, pAllocator,
                                     &image->vk);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pImage = v3dv_image_to_handle(image);

   return VK_SUCCESS;

fail:
   vk_image_destroy(&device->vk, pAllocator, &image->vk);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_CreateImage(VkDevice _device,
                 const VkImageCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkImage *pImage)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   return create_image(device, pCreateInfo, pAllocator, pImage);
}

static void
get_image_subresource_layout(struct v3dv_device *device,
                             struct v3dv_image *image,
                             const VkImageSubresource2KHR *subresource2,
                             VkSubresourceLayout2KHR *layout2)
{
   const VkImageSubresource *subresource = &subresource2->imageSubresource;
   VkSubresourceLayout *layout = &layout2->subresourceLayout;

   uint8_t plane = v3dv_plane_from_aspect(subresource->aspectMask);
   const struct v3d_resource_slice *slice =
      &image->planes[plane].slices[subresource->mipLevel];

   /* About why the offset below works for both disjoint and non-disjoint
    * cases, from the Vulkan spec:
    *
    *   "If the image is disjoint, then the offset is relative to the base
    *    address of the plane."
    *
    *   "If the image is non-disjoint, then the offset is relative to the base
    *    address of the image."
    *
    * In our case, the per-plane mem_offset for non-disjoint images is the
    * same for all planes and matches the base address of the image.
    */
   layout->offset =
      v3dv_layer_offset(image, subresource->mipLevel, subresource->arrayLayer,
                        plane) - image->planes[plane].mem_offset;
   layout->rowPitch = slice->stride;
   layout->depthPitch = image->vk.image_type == VK_IMAGE_TYPE_3D ?
                        image->planes[plane].cube_map_stride : 0;
   layout->arrayPitch = image->vk.array_layers > 1 ?
                        image->planes[plane].cube_map_stride : 0;

   if (image->vk.image_type != VK_IMAGE_TYPE_3D) {
      layout->size = slice->size;
   } else {
      /* For 3D images, the size of the slice represents the size of a 2D slice
       * in the 3D image, so we have to multiply by the depth extent of the
       * miplevel. For levels other than the first, we just compute the size
       * as the distance between consecutive levels (notice that mip levels are
       * arranged in memory from last to first).
       */
      if (subresource->mipLevel == 0) {
         layout->size = slice->size * image->vk.extent.depth;
      } else {
            const struct v3d_resource_slice *prev_slice =
               &image->planes[plane].slices[subresource->mipLevel - 1];
            layout->size = prev_slice->offset - slice->offset;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
v3dv_GetImageSubresourceLayout2KHR(VkDevice _device,
                                   VkImage _image,
                                   const VkImageSubresource2KHR *subresource2,
                                   VkSubresourceLayout2KHR *layout2)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_image, image, _image);
   get_image_subresource_layout(device, image, subresource2, layout2);
}

VKAPI_ATTR void VKAPI_CALL
v3dv_GetDeviceImageSubresourceLayoutKHR(VkDevice vk_device,
                                        const VkDeviceImageSubresourceInfoKHR *pInfo,
                                        VkSubresourceLayout2KHR *pLayout)
{
   V3DV_FROM_HANDLE(v3dv_device, device, vk_device);

   memset(&pLayout->subresourceLayout, 0, sizeof(pLayout->subresourceLayout));

   VkImage vk_image = VK_NULL_HANDLE;
   VkResult result = create_image(device, pInfo->pCreateInfo, NULL, &vk_image);
   if (result != VK_SUCCESS)
      return;

   struct v3dv_image *image = v3dv_image_from_handle(vk_image);
   get_image_subresource_layout(device, image, pInfo->pSubresource, pLayout);

   v3dv_DestroyImage(vk_device, vk_image, NULL);
}

VKAPI_ATTR void VKAPI_CALL
v3dv_DestroyImage(VkDevice _device,
                  VkImage _image,
                  const VkAllocationCallbacks* pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   if (image == NULL)
      return;

   /* If we have created a shadow tiled image for this image we must also free
    * it (along with its memory allocation).
    */
   if (image->shadow) {
      bool disjoint = image->vk.create_flags & VK_IMAGE_CREATE_DISJOINT_BIT;
      for (int i = 0; i < (disjoint ? image->plane_count : 1); i++) {
         if (image->shadow->planes[i].mem) {
            v3dv_FreeMemory(_device,
                            v3dv_device_memory_to_handle(image->shadow->planes[i].mem),
                            pAllocator);
         }
      }
      v3dv_DestroyImage(_device, v3dv_image_to_handle(image->shadow),
                        pAllocator);
      image->shadow = NULL;
   }

   vk_image_destroy(&device->vk, pAllocator, &image->vk);
}

VkImageViewType
v3dv_image_type_to_view_type(VkImageType type)
{
   switch (type) {
   case VK_IMAGE_TYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
   case VK_IMAGE_TYPE_2D: return VK_IMAGE_VIEW_TYPE_2D;
   case VK_IMAGE_TYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
   default:
      UNREACHABLE("Invalid image type");
   }
}

static VkResult
create_image_view(struct v3dv_device *device,
                  bool driver_internal,
                  const VkImageViewCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkImageView *pView)
{
   V3DV_FROM_HANDLE(v3dv_image, image, pCreateInfo->image);
   struct v3dv_image_view *iview;

   iview = vk_image_view_create(&device->vk, driver_internal, pCreateInfo,
                                pAllocator, sizeof(*iview));
   if (iview == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImageAspectFlagBits any_plane_aspect =
      VK_IMAGE_ASPECT_PLANE_0_BIT |
      VK_IMAGE_ASPECT_PLANE_1_BIT |
      VK_IMAGE_ASPECT_PLANE_2_BIT;

   if (image->vk.aspects & any_plane_aspect) {
      assert((image->vk.aspects & ~any_plane_aspect) == 0);
      iview->plane_count = 0;
      static const VkImageAspectFlagBits plane_aspects[]= {
         VK_IMAGE_ASPECT_PLANE_0_BIT,
         VK_IMAGE_ASPECT_PLANE_1_BIT,
         VK_IMAGE_ASPECT_PLANE_2_BIT
      };
      for (uint8_t plane = 0; plane < V3DV_MAX_PLANE_COUNT; plane++) {
         if (iview->vk.aspects & plane_aspects[plane])
            iview->planes[iview->plane_count++].image_plane = plane;
      }
   } else {
      iview->plane_count = 1;
      iview->planes[0].image_plane = 0;
   }
   /* At this point we should have at least one plane */
   assert(iview->plane_count > 0);

   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;

   /* If we have D24S8 format but the view only selects the stencil aspect
    * we want to re-interpret the format as RGBA8_UINT, then map our stencil
    * data reads to the R component and ignore the GBA channels that contain
    * the depth aspect data.
    *
    * FIXME: thwe code belows calls vk_component_mapping_to_pipe_swizzle
    * only so it can then call util_format_compose_swizzles later. Maybe it
    * makes sense to implement swizzle composition using VkSwizzle directly.
    */
   VkFormat format;
   if (image->vk.format == VK_FORMAT_D24_UNORM_S8_UINT &&
       range->aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
      format = VK_FORMAT_R8G8B8A8_UINT;
      uint8_t stencil_aspect_swizzle[4] = {
         PIPE_SWIZZLE_X, PIPE_SWIZZLE_0, PIPE_SWIZZLE_0, PIPE_SWIZZLE_1,
      };
      uint8_t view_swizzle[4];
      vk_component_mapping_to_pipe_swizzle(iview->vk.swizzle, view_swizzle);

      util_format_compose_swizzles(stencil_aspect_swizzle, view_swizzle,
                                   iview->view_swizzle);
   } else {
      format = iview->vk.format;
      vk_component_mapping_to_pipe_swizzle(iview->vk.swizzle,
                                           iview->view_swizzle);
   }

   iview->vk.view_format = format;
   iview->format = v3d_X((&device->devinfo), get_format)(format);
   assert(iview->format && iview->format->plane_count);

   for (uint8_t plane = 0; plane < iview->plane_count; plane++) {
      iview->planes[plane].offset = v3dv_layer_offset(image,
                                                      iview->vk.base_mip_level,
                                                      iview->vk.base_array_layer,
                                                      plane);

      if (vk_format_is_depth_or_stencil(iview->vk.view_format)) {
         iview->planes[plane].internal_type =
            v3d_X((&device->devinfo), get_internal_depth_type)(iview->vk.view_format);
      } else {
         v3d_X((&device->devinfo), get_internal_type_bpp_for_output_format)
            (iview->format->planes[plane].rt_type,
             &iview->planes[plane].internal_type,
             &iview->planes[plane].internal_bpp);
      }

      const uint8_t *format_swizzle =
         v3dv_get_format_swizzle(device, format, plane);
      util_format_compose_swizzles(format_swizzle, iview->view_swizzle,
                                   iview->planes[plane].swizzle);

      iview->planes[plane].swap_rb = v3dv_format_swizzle_needs_rb_swap(format_swizzle);
      iview->planes[plane].channel_reverse = v3dv_format_swizzle_needs_reverse(format_swizzle);
   }

   v3d_X((&device->devinfo), pack_texture_shader_state)(device, iview);

   *pView = v3dv_image_view_to_handle(iview);

   return VK_SUCCESS;
}

VkResult
v3dv_create_image_view(struct v3dv_device *device,
                       const VkImageViewCreateInfo *pCreateInfo,
                       VkImageView *pView)
{
   return create_image_view(device, true, pCreateInfo, NULL, pView);
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_CreateImageView(VkDevice _device,
                     const VkImageViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkImageView *pView)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   return create_image_view(device, false, pCreateInfo, pAllocator, pView);
}

VKAPI_ATTR void VKAPI_CALL
v3dv_DestroyImageView(VkDevice _device,
                      VkImageView imageView,
                      const VkAllocationCallbacks* pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_image_view, image_view, imageView);

   if (image_view == NULL)
      return;

   if (image_view->shadow) {
      v3dv_DestroyImageView(_device,
                            v3dv_image_view_to_handle(image_view->shadow),
                            pAllocator);
      image_view->shadow = NULL;
   }

   vk_image_view_destroy(&device->vk, pAllocator, &image_view->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_CreateBufferView(VkDevice _device,
                      const VkBufferViewCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkBufferView *pView)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   struct v3dv_buffer *buffer =
      v3dv_buffer_from_handle(pCreateInfo->buffer);

   struct v3dv_buffer_view *view =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*view),
                       VK_OBJECT_TYPE_BUFFER_VIEW);
   if (!view)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t range;
   if (pCreateInfo->range == VK_WHOLE_SIZE)
      range = buffer->size - pCreateInfo->offset;
   else
      range = pCreateInfo->range;

   enum pipe_format pipe_format = vk_format_to_pipe_format(pCreateInfo->format);
   uint32_t num_elements = range / util_format_get_blocksize(pipe_format);

   view->buffer = buffer;
   view->offset = pCreateInfo->offset;
   view->size = view->offset + range;
   view->num_elements = num_elements;
   view->vk_format = pCreateInfo->format;
   view->format = v3d_X((&device->devinfo), get_format)(view->vk_format);

   /* We don't support multi-plane formats for buffer views */
   assert(view->format->plane_count == 1);
   v3d_X((&device->devinfo), get_internal_type_bpp_for_output_format)
      (view->format->planes[0].rt_type, &view->internal_type, &view->internal_bpp);

   const VkBufferUsageFlags2CreateInfoKHR *flags2 =
      vk_find_struct_const(pCreateInfo->pNext,
                           BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR);

   VkBufferUsageFlags2KHR usage;
   if (flags2)
      usage = flags2->usage;
   else
      usage = buffer->usage;

   if (usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT ||
       usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)
      v3d_X((&device->devinfo), pack_texture_shader_state_from_buffer_view)(device, view);

   *pView = v3dv_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
v3dv_DestroyBufferView(VkDevice _device,
                       VkBufferView bufferView,
                       const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_buffer_view, buffer_view, bufferView);

   if (buffer_view == NULL)
      return;

   vk_object_free(&device->vk, pAllocator, buffer_view);
}
