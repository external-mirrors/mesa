/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "hk_buffer.h"

#include "agx_bo.h"
#include "agx_device.h"
#include "hk_device.h"
#include "hk_device_memory.h"
#include "hk_entrypoints.h"
#include "hk_physical_device.h"

static uint32_t
hk_get_buffer_alignment(const struct hk_physical_device *pdev,
                        VkBufferUsageFlags2KHR usage_flags,
                        VkBufferCreateFlags create_flags)
{
   uint32_t alignment = 16;

   if (usage_flags & VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT_KHR)
      alignment = MAX2(alignment, HK_MIN_UBO_ALIGNMENT);

   if (usage_flags & VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR)
      alignment = MAX2(alignment, HK_MIN_SSBO_ALIGNMENT);

   if (usage_flags & (VK_BUFFER_USAGE_2_UNIFORM_TEXEL_BUFFER_BIT_KHR |
                      VK_BUFFER_USAGE_2_STORAGE_TEXEL_BUFFER_BIT_KHR))
      alignment = MAX2(alignment, HK_MIN_TEXEL_BUFFER_ALIGNMENT);

   if (create_flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                       VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT))
      alignment = MAX2(alignment, 16384);

   return alignment;
}

static uint64_t
hk_get_bda_replay_addr(const VkBufferCreateInfo *pCreateInfo)
{
   uint64_t addr = 0;
   vk_foreach_struct_const(ext, pCreateInfo->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: {
         const VkBufferOpaqueCaptureAddressCreateInfo *bda = (void *)ext;
         if (bda->opaqueCaptureAddress != 0) {
#ifdef NDEBUG
            return bda->opaqueCaptureAddress;
#else
            assert(addr == 0 || bda->opaqueCaptureAddress == addr);
            addr = bda->opaqueCaptureAddress;
#endif
         }
         break;
      }

      case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: {
         const VkBufferDeviceAddressCreateInfoEXT *bda = (void *)ext;
         if (bda->deviceAddress != 0) {
#ifdef NDEBUG
            return bda->deviceAddress;
#else
            assert(addr == 0 || bda->deviceAddress == addr);
            addr = bda->deviceAddress;
#endif
         }
         break;
      }

      default:
         break;
      }
   }

   return addr;
}

VkResult
hk_bind_scratch(struct hk_device *dev, struct agx_va *va, unsigned offset_B,
                size_t size_B)
{
   uint64_t addr = va->addr + offset_B;
   uint32_t flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_SINGLE_PAGE;

   /* Map read-write scratch to the primary (bottom half) VA range */
   int ret = agx_bo_bind(&dev->dev, dev->dev.scratch_bo, addr, size_B, 0,
                         flags | DRM_ASAHI_BIND_WRITE);
   if (ret)
      return VK_ERROR_UNKNOWN;

   /* Map read-only scratch to the secondary (top half) VA range */
   ret = agx_bo_bind(&dev->dev, dev->dev.zero_bo,
                     addr + dev->dev.sparse_ro_offset, size_B, 0, flags);
   if (ret)
      return VK_ERROR_UNKNOWN;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_CreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   struct hk_buffer *buffer;

   if (pCreateInfo->size > HK_MAX_BUFFER_SIZE)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   buffer =
      vk_buffer_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*buffer));
   if (!buffer)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (buffer->vk.size > 0 &&
       (buffer->vk.create_flags &
        (VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
         VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT))) {

      const uint32_t alignment = hk_get_buffer_alignment(
         hk_device_physical(dev), buffer->vk.usage, buffer->vk.create_flags);
      assert(alignment >= 16384);
      uint64_t vma_size_B = align64(buffer->vk.size, alignment);

      const bool bda_capture_replay =
         buffer->vk.create_flags &
         VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;

      enum agx_va_flags flags = 0;
      uint64_t bda_fixed_addr = 0;
      if (bda_capture_replay) {
         bda_fixed_addr = hk_get_bda_replay_addr(pCreateInfo);
         if (bda_fixed_addr != 0)
            flags |= AGX_VA_FIXED;
      }

      buffer->va =
         agx_va_alloc(&dev->dev, vma_size_B, alignment, flags, bda_fixed_addr);

      if (!buffer->va) {
         vk_buffer_destroy(&dev->vk, pAllocator, &buffer->vk);
         return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Sparse VMA allocation failed");
      }
      buffer->vk.device_address = buffer->va->addr;

      /* Bind scratch pages to make read/write across the VA valid */
      hk_bind_scratch(dev, buffer->va, 0, vma_size_B);
   }

   *pBuffer = hk_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
hk_DestroyBuffer(VkDevice device, VkBuffer _buffer,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   VK_FROM_HANDLE(hk_buffer, buffer, _buffer);

   if (!buffer)
      return;

   if (buffer->va) {
      agx_va_free(&dev->dev, buffer->va, true);
   }

   vk_buffer_destroy(&dev->vk, pAllocator, &buffer->vk);
}

VKAPI_ATTR void VKAPI_CALL
hk_GetDeviceBufferMemoryRequirements(
   VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   struct hk_physical_device *pdev = hk_device_physical(dev);

   const uint32_t alignment = hk_get_buffer_alignment(
      hk_device_physical(dev), pInfo->pCreateInfo->usage,
      pInfo->pCreateInfo->flags);

   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size = align64(pInfo->pCreateInfo->size, alignment),
      .alignment = alignment,
      .memoryTypeBits = BITFIELD_MASK(pdev->mem_type_count),
   };

   vk_foreach_struct_const(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         dedicated->prefersDedicatedAllocation = false;
         dedicated->requiresDedicatedAllocation = false;
         break;
      }
      default:
         vk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
hk_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   /* The Vulkan 1.3.256 spec says:
    *
    *    VUID-VkPhysicalDeviceExternalBufferInfo-handleType-parameter
    *
    *    "handleType must be a valid VkExternalMemoryHandleTypeFlagBits value"
    *
    * This differs from VkPhysicalDeviceExternalImageFormatInfo, which
    * surprisingly permits handleType == 0.
    */
   assert(pExternalBufferInfo->handleType != 0);

   /* All of the current flags are for sparse which we don't support yet.
    * Even when we do support it, doing sparse on external memory sounds
    * sketchy.  Also, just disallowing flags is the safe option.
    */
   if (pExternalBufferInfo->flags)
      goto unsupported;

   switch (pExternalBufferInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      pExternalBufferProperties->externalMemoryProperties =
         hk_dma_buf_mem_props;
      return;
   default:
      goto unsupported;
   }

unsupported:
   /* From the Vulkan 1.3.256 spec:
    *
    *    compatibleHandleTypes must include at least handleType.
    */
   pExternalBufferProperties->externalMemoryProperties =
      (VkExternalMemoryProperties){
         .compatibleHandleTypes = pExternalBufferInfo->handleType,
      };
}

VKAPI_ATTR VkResult VKAPI_CALL
hk_BindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                     const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(hk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(hk_buffer, buffer, pBindInfos[i].buffer);

      if (buffer->va) {
         VK_FROM_HANDLE(hk_device, dev, device);
         size_t size = MIN2(mem->bo->size, buffer->va->size_B);

         /* Lower mapping: read-write */
         int ret = agx_bo_bind(&dev->dev, mem->bo, buffer->vk.device_address,
                               size, pBindInfos[i].memoryOffset,
                               DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE);
         if (ret)
            return VK_ERROR_UNKNOWN;

         /* Upper mapping: read-only */
         ret =
            agx_bo_bind(&dev->dev, mem->bo,
                        buffer->vk.device_address + dev->dev.sparse_ro_offset,
                        size, pBindInfos[i].memoryOffset, DRM_ASAHI_BIND_READ);
         if (ret)
            return VK_ERROR_UNKNOWN;
      } else {
         assert(buffer->vk.device_address == 0);
         buffer->vk.device_address =
            mem->bo->va->addr + pBindInfos[i].memoryOffset;
      }

      const VkBindMemoryStatusKHR *status =
         vk_find_struct_const(pBindInfos[i].pNext, BIND_MEMORY_STATUS_KHR);
      if (status != NULL && status->pResult != NULL)
         *status->pResult = VK_SUCCESS;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR uint64_t VKAPI_CALL
hk_GetBufferOpaqueCaptureAddress(UNUSED VkDevice device,
                                 const VkBufferDeviceAddressInfo *pInfo)
{
   VK_FROM_HANDLE(hk_buffer, buffer, pInfo->buffer);

   return hk_buffer_address_rw(buffer, 0);
}

uint64_t
hk_buffer_address(const struct hk_buffer *buffer, uint64_t offset,
                  bool read_only)
{
   struct hk_device *dev = (struct hk_device *)buffer->vk.base.device;
   uint64_t addr = vk_buffer_address(&buffer->vk, offset);

   /* If we are accessing the buffer read-only, we want to return the read-only
    * shadow mapping so non-resident pages return zeroes. That only applies to
    * sparse resident buffers, which will have buffer->va != NULL. If buffer->va
    * is NULL, the buffer is not sparse resident, so we don't need the fix up...
    * and indeed, there may not be a shadow map available.
    */
   if (read_only && buffer->va) {
      addr = agx_rw_addr_to_ro(&dev->dev, addr);
   }

   return addr;
}
