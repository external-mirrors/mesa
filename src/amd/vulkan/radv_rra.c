/*
 * Copyright © 2022 Friedrich Vock
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_rra.h"
#include "bvh/bvh.h"
#include "amd_family.h"
#include "radv_device.h"
#include "radv_entrypoints.h"
#include "radv_physical_device.h"
#include "vk_acceleration_structure.h"
#include "vk_common_entrypoints.h"

#define RRA_MAGIC 0x204644525F444D41

struct rra_file_header {
   uint64_t magic;
   uint32_t version;
   uint32_t unused;
   uint64_t chunk_descriptions_offset;
   uint64_t chunk_descriptions_size;
};

static_assert(sizeof(struct rra_file_header) == 32, "rra_file_header does not match RRA spec");

enum rra_chunk_version {
   RADV_RRA_ASIC_API_INFO_CHUNK_VERSION = 0x1,
   RADV_RRA_RAY_HISTORY_CHUNK_VERSION = 0x2,
   RADV_RRA_ACCEL_STRUCT_CHUNK_VERSION = 0xF0005,
};

enum rra_file_api {
   RADV_RRA_API_DX9,
   RADV_RRA_API_DX11,
   RADV_RRA_API_DX12,
   RADV_RRA_API_VULKAN,
   RADV_RRA_API_OPENGL,
   RADV_RRA_API_OPENCL,
   RADV_RRA_API_MANTLE,
   RADV_RRA_API_GENERIC,
};

struct rra_file_chunk_description {
   char name[16];
   uint32_t is_zstd_compressed;
   enum rra_chunk_version version;
   uint64_t header_offset;
   uint64_t header_size;
   uint64_t data_offset;
   uint64_t data_size;
   uint64_t unused;
};

static_assert(sizeof(struct rra_file_chunk_description) == 64, "rra_file_chunk_description does not match RRA spec");

static void
rra_dump_header(FILE *output, uint64_t chunk_descriptions_offset, uint64_t chunk_descriptions_size)
{
   struct rra_file_header header = {
      .magic = RRA_MAGIC,
      .version = 3,
      .chunk_descriptions_offset = chunk_descriptions_offset,
      .chunk_descriptions_size = chunk_descriptions_size,
   };
   fwrite(&header, sizeof(header), 1, output);
}

static void
rra_dump_chunk_description(uint64_t offset, uint64_t header_size, uint64_t data_size, const char *name,
                           enum rra_chunk_version version, FILE *output)
{
   struct rra_file_chunk_description chunk = {
      .version = version,
      .header_offset = offset,
      .header_size = header_size,
      .data_offset = offset + header_size,
      .data_size = data_size,
   };
   memcpy(chunk.name, name, strnlen(name, sizeof(chunk.name)));
   fwrite(&chunk, sizeof(struct rra_file_chunk_description), 1, output);
}

enum rra_memory_type {
   RRA_MEMORY_TYPE_UNKNOWN,
   RRA_MEMORY_TYPE_DDR,
   RRA_MEMORY_TYPE_DDR2,
   RRA_MEMORY_TYPE_DDR3,
   RRA_MEMORY_TYPE_DDR4,
   RRA_MEMORY_TYPE_DDR5,
   RRA_MEMORY_TYPE_GDDR3,
   RRA_MEMORY_TYPE_GDDR4,
   RRA_MEMORY_TYPE_GDDR5,
   RRA_MEMORY_TYPE_GDDR6,
   RRA_MEMORY_TYPE_HBM,
   RRA_MEMORY_TYPE_HBM2,
   RRA_MEMORY_TYPE_HBM3,
   RRA_MEMORY_TYPE_LPDDR4,
   RRA_MEMORY_TYPE_LPDDR5,
};

#define RRA_FILE_DEVICE_NAME_MAX_SIZE 256

struct rra_asic_info {
   uint64_t min_shader_clk_freq;
   uint64_t min_mem_clk_freq;
   char unused[8];
   uint64_t max_shader_clk_freq;
   uint64_t max_mem_clk_freq;
   uint32_t device_id;
   uint32_t rev_id;
   char unused2[80];
   uint64_t vram_size;
   uint32_t bus_width;
   char unused3[12];
   char device_name[RRA_FILE_DEVICE_NAME_MAX_SIZE];
   char unused4[16];
   uint32_t mem_ops_per_clk;
   uint32_t mem_type;
   char unused5[135];
   bool valid;
};

static_assert(sizeof(struct rra_asic_info) == 568, "rra_asic_info does not match RRA spec");

static uint32_t
amdgpu_vram_type_to_rra(uint32_t type)
{
   switch (type) {
   case AMD_VRAM_TYPE_UNKNOWN:
      return RRA_MEMORY_TYPE_UNKNOWN;
   case AMD_VRAM_TYPE_DDR2:
      return RRA_MEMORY_TYPE_DDR2;
   case AMD_VRAM_TYPE_DDR3:
      return RRA_MEMORY_TYPE_DDR3;
   case AMD_VRAM_TYPE_DDR4:
      return RRA_MEMORY_TYPE_DDR4;
   case AMD_VRAM_TYPE_DDR5:
      return RRA_MEMORY_TYPE_DDR5;
   case AMD_VRAM_TYPE_HBM:
      return RRA_MEMORY_TYPE_HBM;
   case AMD_VRAM_TYPE_GDDR3:
      return RRA_MEMORY_TYPE_GDDR3;
   case AMD_VRAM_TYPE_GDDR4:
      return RRA_MEMORY_TYPE_GDDR4;
   case AMD_VRAM_TYPE_GDDR5:
      return RRA_MEMORY_TYPE_GDDR5;
   case AMD_VRAM_TYPE_GDDR6:
      return RRA_MEMORY_TYPE_GDDR6;
   case AMD_VRAM_TYPE_LPDDR4:
      return RRA_MEMORY_TYPE_LPDDR4;
   case AMD_VRAM_TYPE_LPDDR5:
      return RRA_MEMORY_TYPE_LPDDR5;
   default:
      UNREACHABLE("invalid vram type");
   }
}

static void
rra_dump_asic_info(const struct radeon_info *gpu_info, FILE *output)
{
   struct rra_asic_info asic_info = {
      /* All frequencies are in Hz */
      .min_shader_clk_freq = 0,
      .max_shader_clk_freq = gpu_info->max_gpu_freq_mhz * 1000000,
      .min_mem_clk_freq = 0,
      .max_mem_clk_freq = gpu_info->memory_freq_mhz * 1000000,

      .vram_size = (uint64_t)gpu_info->vram_size_kb * 1024,

      .mem_type = amdgpu_vram_type_to_rra(gpu_info->vram_type),
      .mem_ops_per_clk = ac_memory_ops_per_clock(gpu_info->vram_type),
      .bus_width = gpu_info->memory_bus_width,

      .device_id = gpu_info->pci.dev,
      .rev_id = gpu_info->pci_rev_id,
   };

   strncpy(asic_info.device_name, gpu_info->marketing_name ? gpu_info->marketing_name : gpu_info->name,
           RRA_FILE_DEVICE_NAME_MAX_SIZE - 1);

   fwrite(&asic_info, sizeof(struct rra_asic_info), 1, output);
}

static struct rra_accel_struct_header
rra_fill_accel_struct_header_common(const struct radv_physical_device *pdev, struct radv_accel_struct_header *header,
                                    size_t parent_id_table_size, struct rra_bvh_info *bvh_info,
                                    uint64_t primitive_count)
{
   struct rra_accel_struct_header result = {
      .post_build_info =
         {
            .build_flags = header->build_flags,
            /* Seems to be no compression */
            .tri_compression_mode = 0,
         },
      .primitive_count = primitive_count,
      /* TODO: calculate active primitives */
      .active_primitive_count = primitive_count,
      .geometry_description_count = header->geometry_count,
      .interior_fp32_node_count = bvh_info->internal_nodes_size / sizeof(struct radv_bvh_box32_node),
      .leaf_node_count = primitive_count,
      .rt_driver_interface_version = 8 << 16,
      .rt_ip_version = pdev->info.rt_ip_version,
   };

   if (!radv_use_bvh8(pdev))
      result.rt_ip_version = MIN2(result.rt_ip_version, RT_1_1);

   result.metadata_size = sizeof(struct rra_accel_struct_metadata) + parent_id_table_size;
   result.file_size = result.metadata_size + sizeof(struct rra_accel_struct_header) + bvh_info->internal_nodes_size +
                      bvh_info->leaf_nodes_size;

   result.internal_nodes_offset = sizeof(struct rra_accel_struct_metadata);
   result.leaf_nodes_offset = result.internal_nodes_offset + bvh_info->internal_nodes_size;
   result.geometry_infos_offset = result.leaf_nodes_offset + bvh_info->leaf_nodes_size;
   result.leaf_ids_offset = result.geometry_infos_offset;
   if (header->instance_count) {
      if (radv_use_bvh8(pdev))
         result.leaf_ids_offset += bvh_info->instance_sideband_data_size;
   } else {
      result.leaf_ids_offset += header->geometry_count * sizeof(struct rra_geometry_info);
   }

   return result;
}

static void
rra_dump_tlas_header(const struct radv_physical_device *pdev, struct radv_accel_struct_header *header,
                     size_t parent_id_table_size, struct rra_bvh_info *bvh_info, uint64_t primitive_count, FILE *output)
{
   struct rra_accel_struct_header file_header =
      rra_fill_accel_struct_header_common(pdev, header, parent_id_table_size, bvh_info, primitive_count);
   file_header.post_build_info.bvh_type = RRA_BVH_TYPE_TLAS;
   file_header.geometry_type = VK_GEOMETRY_TYPE_INSTANCES_KHR;

   fwrite(&file_header, sizeof(struct rra_accel_struct_header), 1, output);
}

static void
rra_dump_blas_header(const struct radv_physical_device *pdev, struct radv_accel_struct_header *header,
                     size_t parent_id_table_size, struct radv_accel_struct_geometry_info *geometry_infos,
                     struct rra_bvh_info *bvh_info, uint64_t primitive_count, FILE *output)
{
   struct rra_accel_struct_header file_header =
      rra_fill_accel_struct_header_common(pdev, header, parent_id_table_size, bvh_info, primitive_count);
   file_header.post_build_info.bvh_type = RRA_BVH_TYPE_BLAS;
   file_header.geometry_type = header->geometry_count ? geometry_infos->type : VK_GEOMETRY_TYPE_TRIANGLES_KHR;

   fwrite(&file_header, sizeof(struct rra_accel_struct_header), 1, output);
}

void PRINTFLIKE(2, 3) rra_validation_fail(struct rra_validation_context *ctx, const char *message, ...)
{
   if (!ctx->failed) {
      fprintf(stderr, "radv: rra: Validation failed at %s:\n", ctx->location);
      ctx->failed = true;
   }

   fprintf(stderr, "   ");

   va_list list;
   va_start(list, message);
   vfprintf(stderr, message, list);
   va_end(list);

   fprintf(stderr, "\n");
}

static bool
rra_validate_header(struct radv_rra_accel_struct_data *accel_struct, const struct radv_accel_struct_header *header)
{
   struct rra_validation_context ctx = {
      .location = "header",
   };

   if (accel_struct->type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR && header->instance_count > 0)
      rra_validation_fail(&ctx, "BLAS contains instances");

   if (header->bvh_offset >= accel_struct->size)
      rra_validation_fail(&ctx, "Invalid BVH offset %u", header->bvh_offset);

   if (header->instance_count * sizeof(struct radv_bvh_instance_node) >= accel_struct->size)
      rra_validation_fail(&ctx, "Too many instances");

   return ctx.failed;
}

static VkResult
rra_dump_acceleration_structure(const struct radv_physical_device *pdev,
                                struct radv_rra_accel_struct_data *accel_struct, uint8_t *data,
                                struct hash_table_u64 *accel_struct_vas, struct set *used_blas, bool should_validate,
                                FILE *output)
{
   struct radv_accel_struct_header *header = (struct radv_accel_struct_header *)data;

   bool is_tlas = header->instance_count > 0;

   uint64_t geometry_infos_offset = sizeof(struct radv_accel_struct_header);

   /* convert root node id to offset */
   uint32_t src_root_offset = (RADV_BVH_ROOT_NODE & ~7) << 3;

   if (should_validate) {
      if (rra_validate_header(accel_struct, header)) {
         return VK_ERROR_VALIDATION_FAILED_EXT;
      }
      if (radv_use_bvh8(pdev)) {
         if (rra_validate_node_gfx12(accel_struct_vas, data + header->bvh_offset,
                                     data + header->bvh_offset + src_root_offset, header->geometry_count,
                                     accel_struct->size, !is_tlas, 0)) {
            return VK_ERROR_VALIDATION_FAILED_EXT;
         }
      } else {
         if (rra_validate_node_gfx10_3(accel_struct_vas, data + header->bvh_offset,
                                       data + header->bvh_offset + src_root_offset, header->geometry_count,
                                       accel_struct->size, !is_tlas, 0)) {
            return VK_ERROR_VALIDATION_FAILED_EXT;
         }
      }
   }

   VkResult result = VK_SUCCESS;

   struct rra_geometry_info *rra_geometry_infos = NULL;
   uint32_t *leaf_indices = NULL;
   uint32_t *node_parent_table = NULL;
   uint32_t *leaf_node_ids = NULL;
   uint8_t *dst_structure_data = NULL;

   rra_geometry_infos = calloc(header->geometry_count, sizeof(struct rra_geometry_info));
   if (!rra_geometry_infos) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto exit;
   }

   struct rra_bvh_info bvh_info = {
      .geometry_infos = rra_geometry_infos,
   };
   if (radv_use_bvh8(pdev))
      rra_gather_bvh_info_gfx12(data + header->bvh_offset, RADV_BVH_ROOT_NODE, &bvh_info);
   else
      rra_gather_bvh_info_gfx10_3(data + header->bvh_offset, RADV_BVH_ROOT_NODE, &bvh_info);

   leaf_indices = calloc(header->geometry_count, sizeof(struct rra_geometry_info));
   if (!leaf_indices) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto exit;
   }

   uint64_t primitive_count = 0;

   struct radv_accel_struct_geometry_info *geometry_infos =
      (struct radv_accel_struct_geometry_info *)(data + geometry_infos_offset);

   for (uint32_t i = 0; i < header->geometry_count; ++i) {
      rra_geometry_infos[i].flags = geometry_infos[i].flags;
      rra_geometry_infos[i].leaf_node_list_offset = primitive_count * sizeof(uint32_t);
      leaf_indices[i] = primitive_count;
      primitive_count += rra_geometry_infos[i].primitive_count;
   }

   uint32_t node_parent_table_size =
      ((bvh_info.leaf_nodes_size + bvh_info.internal_nodes_size) / 64) * sizeof(uint32_t);
   if (radv_use_bvh8(pdev))
      node_parent_table_size = 0;

   node_parent_table = calloc(node_parent_table_size, 1);
   if (!node_parent_table) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto exit;
   }

   leaf_node_ids = calloc(primitive_count, sizeof(uint32_t));
   if (!leaf_node_ids) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto exit;
   }
   dst_structure_data = calloc(RRA_ROOT_NODE_OFFSET + bvh_info.internal_nodes_size + bvh_info.leaf_nodes_size +
                                  bvh_info.instance_sideband_data_size,
                               1);
   if (!dst_structure_data) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto exit;
   }

   struct rra_transcoding_context ctx = {
      .used_blas = used_blas,
      .src = data + header->bvh_offset,
      .dst = dst_structure_data,
      .dst_leaf_offset = RRA_ROOT_NODE_OFFSET + bvh_info.internal_nodes_size,
      .dst_internal_offset = RRA_ROOT_NODE_OFFSET,
      .dst_instance_sideband_data_offset =
         RRA_ROOT_NODE_OFFSET + bvh_info.internal_nodes_size + bvh_info.leaf_nodes_size,
      .parent_id_table = node_parent_table,
      .parent_id_table_size = node_parent_table_size,
      .leaf_node_ids = leaf_node_ids,
      .leaf_indices = leaf_indices,
   };

   if (radv_use_bvh8(pdev)) {
      ctx.dst_internal_offset += sizeof(struct radv_gfx12_box_node);
      rra_transcode_node_gfx12(&ctx, 0xFFFFFFFF, RADV_BVH_ROOT_NODE, RRA_ROOT_NODE_OFFSET);
   } else {
      rra_transcode_node_gfx10_3(&ctx, 0xFFFFFFFF, RADV_BVH_ROOT_NODE, header->aabb);
   }

   struct rra_accel_struct_chunk_header chunk_header = {
      .metadata_offset = 0,
      /*
       * RRA loads the part of the metadata that is used into a struct.
       * If the size is larger than just the "used" part, the loading
       * operation overwrites internal pointers with data from the file,
       * likely causing a crash.
       */
      .metadata_size = offsetof(struct rra_accel_struct_metadata, unused),
      .header_offset = sizeof(struct rra_accel_struct_metadata) + node_parent_table_size,
      .header_size = sizeof(struct rra_accel_struct_header),
      .bvh_type = is_tlas ? RRA_BVH_TYPE_TLAS : RRA_BVH_TYPE_BLAS,
   };

   /*
    * When associating TLASes with BLASes, acceleration structure VAs are
    * looked up in a hashmap. But due to the way BLAS VAs are stored for
    * each instance in the RRA file format (divided by 8, and limited to 54 bits),
    * the top bits are masked away.
    * In order to make sure BLASes can be found in the hashmap, we have
    * to replicate that mask here.
    * On GFX12, we mask away the top 16 bits because the instance BLAS addresses
    * use pointer flags.
    */
   uint64_t va = (accel_struct->va & 0x1FFFFFFFFFFFFFF) - node_parent_table_size;
   if (radv_use_bvh8(pdev))
      va &= 0xFFFFFFFFFFFF;
   memcpy(chunk_header.virtual_address, &va, sizeof(uint64_t));

   struct rra_accel_struct_metadata rra_metadata = {
      .virtual_address = va,
      .byte_size = bvh_info.leaf_nodes_size + bvh_info.internal_nodes_size + sizeof(struct rra_accel_struct_header),
   };

   fwrite(&chunk_header, sizeof(struct rra_accel_struct_chunk_header), 1, output);
   fwrite(&rra_metadata, sizeof(struct rra_accel_struct_metadata), 1, output);

   /* Write node parent id data */
   fwrite(node_parent_table, 1, node_parent_table_size, output);

   if (is_tlas)
      rra_dump_tlas_header(pdev, header, node_parent_table_size, &bvh_info, primitive_count, output);
   else
      rra_dump_blas_header(pdev, header, node_parent_table_size, geometry_infos, &bvh_info, primitive_count, output);

   /* Write acceleration structure data  */
   fwrite(dst_structure_data + RRA_ROOT_NODE_OFFSET, 1,
          bvh_info.internal_nodes_size + bvh_info.leaf_nodes_size + bvh_info.instance_sideband_data_size, output);

   if (!is_tlas)
      fwrite(rra_geometry_infos, sizeof(struct rra_geometry_info), header->geometry_count, output);

   /* Write leaf node ids */
   uint32_t leaf_node_list_size = primitive_count * sizeof(uint32_t);
   fwrite(leaf_node_ids, 1, leaf_node_list_size, output);

exit:
   free(rra_geometry_infos);
   free(leaf_indices);
   free(dst_structure_data);
   free(node_parent_table);
   free(leaf_node_ids);

   return result;
}

VkResult
radv_rra_trace_init(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   device->rra_trace.validate_as = debug_get_bool_option("RADV_RRA_TRACE_VALIDATE", false);
   device->rra_trace.copy_after_build = debug_get_bool_option("RADV_RRA_TRACE_COPY_AFTER_BUILD", true);
   device->rra_trace.accel_structs = _mesa_pointer_hash_table_create(NULL);
   device->rra_trace.accel_struct_vas = _mesa_hash_table_u64_create(NULL);
   simple_mtx_init(&device->rra_trace.data_mtx, mtx_plain);

   device->rra_trace.copy_memory_index =
      radv_find_memory_index(pdev, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

   util_dynarray_init(&device->rra_trace.ray_history, NULL);

   device->rra_trace.ray_history_buffer_size = debug_get_num_option("RADV_RRA_TRACE_HISTORY_SIZE", 100 * 1024 * 1024);
   if (device->rra_trace.ray_history_buffer_size <
       sizeof(struct radv_ray_history_header) + sizeof(struct radv_packed_end_trace_token))
      return VK_SUCCESS;

   device->rra_trace.ray_history_resolution_scale = debug_get_num_option("RADV_RRA_TRACE_RESOLUTION_SCALE", 1);
   device->rra_trace.ray_history_resolution_scale = MAX2(device->rra_trace.ray_history_resolution_scale, 1);

   VkBufferCreateInfo buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext =
         &(VkBufferUsageFlags2CreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
            .usage = VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
         },
      .size = device->rra_trace.ray_history_buffer_size,
   };

   VkDevice _device = radv_device_to_handle(device);
   VkResult result = radv_CreateBuffer(_device, &buffer_create_info, NULL, &device->rra_trace.ray_history_buffer);
   if (result != VK_SUCCESS)
      return result;

   VkDeviceBufferMemoryRequirements buffer_mem_req_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &buffer_create_info,
   };
   VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };

   radv_GetDeviceBufferMemoryRequirements(_device, &buffer_mem_req_info, &requirements);

   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.memoryRequirements.size,
      .memoryTypeIndex =
         radv_find_memory_index(pdev, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };

   result = radv_AllocateMemory(_device, &alloc_info, NULL, &device->rra_trace.ray_history_memory);
   if (result != VK_SUCCESS)
      return result;

   VkMemoryMapInfo memory_map_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
      .memory = device->rra_trace.ray_history_memory,
      .size = VK_WHOLE_SIZE,
   };

   result = radv_MapMemory2(_device, &memory_map_info, (void **)&device->rra_trace.ray_history_data);
   if (result != VK_SUCCESS)
      return result;

   VkBindBufferMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = device->rra_trace.ray_history_buffer,
      .memory = device->rra_trace.ray_history_memory,
   };

   result = radv_BindBufferMemory2(_device, 1, &bind_info);

   VkBufferDeviceAddressInfo addr_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = device->rra_trace.ray_history_buffer,
   };
   device->rra_trace.ray_history_addr = vk_common_GetBufferDeviceAddress(_device, &addr_info);

   struct radv_ray_history_header *ray_history_header = device->rra_trace.ray_history_data;
   memset(ray_history_header, 0, sizeof(struct radv_ray_history_header));
   ray_history_header->offset = 1;

   return result;
}

void
radv_rra_trace_clear_ray_history(VkDevice _device, struct radv_rra_trace_data *data)
{
   util_dynarray_foreach (&data->ray_history, struct radv_rra_ray_history_data *, _entry) {
      struct radv_rra_ray_history_data *entry = *_entry;
      free(entry);
   }
   util_dynarray_clear(&data->ray_history);
}

void
radv_radv_rra_accel_struct_buffer_ref(struct radv_rra_accel_struct_buffer *buffer)
{
   assert(buffer->ref_cnt >= 1);
   p_atomic_inc(&buffer->ref_cnt);
}

void
radv_rra_accel_struct_buffer_unref(struct radv_device *device, struct radv_rra_accel_struct_buffer *buffer)
{
   if (p_atomic_dec_zero(&buffer->ref_cnt)) {
      VkDevice _device = radv_device_to_handle(device);
      radv_DestroyBuffer(_device, buffer->buffer, NULL);
      radv_FreeMemory(_device, buffer->memory, NULL);
   }
}

void
radv_rra_accel_struct_buffers_unref(struct radv_device *device, struct set *buffers)
{
   set_foreach_remove (buffers, entry)
      radv_rra_accel_struct_buffer_unref(device, (void *)entry->key);
}

void
radv_rra_trace_finish(VkDevice vk_device, struct radv_rra_trace_data *data)
{
   radv_DestroyBuffer(vk_device, data->ray_history_buffer, NULL);

   if (data->ray_history_memory) {
      VkMemoryUnmapInfo unmap_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
         .memory = data->ray_history_memory,
      };

      radv_UnmapMemory2(vk_device, &unmap_info);
   }

   radv_FreeMemory(vk_device, data->ray_history_memory, NULL);

   radv_rra_trace_clear_ray_history(vk_device, data);
   util_dynarray_fini(&data->ray_history);

   if (data->accel_structs)
      hash_table_foreach (data->accel_structs, entry)
         radv_destroy_rra_accel_struct_data(vk_device, entry->data);

   simple_mtx_destroy(&data->data_mtx);
   _mesa_hash_table_destroy(data->accel_structs, NULL);
   _mesa_hash_table_u64_destroy(data->accel_struct_vas);
}

void
radv_destroy_rra_accel_struct_data(VkDevice _device, struct radv_rra_accel_struct_data *data)
{
   VK_FROM_HANDLE(radv_device, device, _device);

   if (data->buffer)
      radv_rra_accel_struct_buffer_unref(device, data->buffer);

   radv_DestroyEvent(_device, data->build_event, NULL);
   free(data);
}

static int
accel_struct_entry_cmp(const void *a, const void *b)
{
   struct hash_entry *entry_a = *(struct hash_entry *const *)a;
   struct hash_entry *entry_b = *(struct hash_entry *const *)b;
   const struct radv_rra_accel_struct_data *s_a = entry_a->data;
   const struct radv_rra_accel_struct_data *s_b = entry_b->data;

   return s_a->va > s_b->va ? 1 : s_a->va < s_b->va ? -1 : 0;
}

struct rra_copy_context {
   VkDevice device;
   VkQueue queue;

   VkCommandPool pool;
   VkCommandBuffer cmd_buffer;
   uint32_t family_index;

   VkDeviceMemory memory;
   VkBuffer buffer;
   void *mapped_data;

   struct hash_entry **entries;

   uint32_t min_size;
};

static VkResult
rra_copy_context_init(struct rra_copy_context *ctx)
{
   VK_FROM_HANDLE(radv_device, device, ctx->device);
   if (device->rra_trace.copy_after_build)
      return VK_SUCCESS;

   uint32_t max_size = ctx->min_size;
   uint32_t accel_struct_count = _mesa_hash_table_num_entries(device->rra_trace.accel_structs);
   for (unsigned i = 0; i < accel_struct_count; i++) {
      struct radv_rra_accel_struct_data *data = ctx->entries[i]->data;
      max_size = MAX2(max_size, data->size);
   }

   VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->family_index,
   };

   VkResult result = vk_common_CreateCommandPool(ctx->device, &pool_info, NULL, &ctx->pool);
   if (result != VK_SUCCESS)
      return result;

   VkCommandBufferAllocateInfo cmdbuf_alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->pool,
      .commandBufferCount = 1,
   };

   result = vk_common_AllocateCommandBuffers(ctx->device, &cmdbuf_alloc_info, &ctx->cmd_buffer);
   if (result != VK_SUCCESS)
      goto fail_pool;

   VkBufferCreateInfo buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext =
         &(VkBufferUsageFlags2CreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
            .usage = VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
         },
      .size = max_size,
   };

   result = radv_CreateBuffer(ctx->device, &buffer_create_info, NULL, &ctx->buffer);
   if (result != VK_SUCCESS)
      goto fail_pool;

   VkDeviceBufferMemoryRequirements buffer_mem_req_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &buffer_create_info,
   };
   VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };

   radv_GetDeviceBufferMemoryRequirements(ctx->device, &buffer_mem_req_info, &requirements);

   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.memoryRequirements.size,
      .memoryTypeIndex = device->rra_trace.copy_memory_index,
   };

   result = radv_AllocateMemory(ctx->device, &alloc_info, NULL, &ctx->memory);
   if (result != VK_SUCCESS)
      goto fail_buffer;

   VkMemoryMapInfo memory_map_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
      .memory = ctx->memory,
      .size = VK_WHOLE_SIZE,
   };

   result = radv_MapMemory2(ctx->device, &memory_map_info, (void **)&ctx->mapped_data);
   if (result != VK_SUCCESS)
      goto fail_memory;

   VkBindBufferMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = ctx->buffer,
      .memory = ctx->memory,
   };

   result = radv_BindBufferMemory2(ctx->device, 1, &bind_info);
   if (result != VK_SUCCESS)
      goto fail_memory;

   return result;
fail_memory:
   radv_FreeMemory(ctx->device, ctx->memory, NULL);
fail_buffer:
   radv_DestroyBuffer(ctx->device, ctx->buffer, NULL);
fail_pool:
   vk_common_DestroyCommandPool(ctx->device, ctx->pool, NULL);
   return result;
}

static void
rra_copy_context_finish(struct rra_copy_context *ctx)
{
   VK_FROM_HANDLE(radv_device, device, ctx->device);
   if (device->rra_trace.copy_after_build)
      return;

   vk_common_DestroyCommandPool(ctx->device, ctx->pool, NULL);
   radv_DestroyBuffer(ctx->device, ctx->buffer, NULL);

   VkMemoryUnmapInfo unmap_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
      .memory = ctx->memory,
   };

   radv_UnmapMemory2(ctx->device, &unmap_info);
   radv_FreeMemory(ctx->device, ctx->memory, NULL);
}

static void *
rra_map_accel_struct_data(struct rra_copy_context *ctx, uint32_t i)
{
   struct radv_rra_accel_struct_data *data = ctx->entries[i]->data;
   if (radv_GetEventStatus(ctx->device, data->build_event) != VK_EVENT_SET)
      return NULL;

   if (data->buffer->memory) {
      VkMemoryMapInfo memory_map_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
         .memory = data->buffer->memory,
         .size = VK_WHOLE_SIZE,
      };
      void *mapped_data;

      radv_MapMemory2(ctx->device, &memory_map_info, &mapped_data);
      return mapped_data;
   }

   const struct vk_acceleration_structure *accel_struct = ctx->entries[i]->key;
   VkResult result;

   VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   result = radv_BeginCommandBuffer(ctx->cmd_buffer, &begin_info);
   if (result != VK_SUCCESS)
      return NULL;

   VkBufferCopy2 copy = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
      .srcOffset = accel_struct->offset,
      .size = accel_struct->size,
   };

   VkCopyBufferInfo2 copy_info = {
      .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
      .srcBuffer = vk_buffer_to_handle(accel_struct->buffer),
      .dstBuffer = ctx->buffer,
      .regionCount = 1,
      .pRegions = &copy,
   };

   radv_CmdCopyBuffer2(ctx->cmd_buffer, &copy_info);

   result = radv_EndCommandBuffer(ctx->cmd_buffer);
   if (result != VK_SUCCESS)
      return NULL;

   VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->cmd_buffer,
   };

   result = vk_common_QueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE);
   if (result != VK_SUCCESS)
      return NULL;

   result = vk_common_QueueWaitIdle(ctx->queue);
   if (result != VK_SUCCESS)
      return NULL;

   return ctx->mapped_data;
}

static void
rra_unmap_accel_struct_data(struct rra_copy_context *ctx, uint32_t i)
{
   struct radv_rra_accel_struct_data *data = ctx->entries[i]->data;

   if (data->buffer && data->buffer->memory) {
      VkMemoryUnmapInfo unmap_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
         .memory = data->buffer->memory,
      };

      radv_UnmapMemory2(ctx->device, &unmap_info);
   }
}

enum rra_ray_history_token_type {
   rra_ray_history_token_begin,
   rra_ray_history_token_tlas,
   rra_ray_history_token_blas,
   rra_ray_history_token_end,
   rra_ray_history_token_call,
   rra_ray_history_token_timestamp,
   rra_ray_history_token_ahit_status,
   rra_ray_history_token_call2,
   rra_ray_history_token_isec_status,
   rra_ray_history_token_end2,
   rra_ray_history_token_begin2,
   rra_ray_history_token_normal = 0xFFFF,
};

struct rra_ray_history_id_token {
   uint32_t id : 30;
   uint32_t reserved : 1;
   uint32_t has_control : 1;
};
static_assert(sizeof(struct rra_ray_history_id_token) == 4, "rra_ray_history_id_token does not match RRA expectations");

struct rra_ray_history_control_token {
   uint32_t type : 16;
   uint32_t length : 8;
   uint32_t data : 8;
};
static_assert(sizeof(struct rra_ray_history_control_token) == 4,
              "rra_ray_history_control_token does not match RRA expectations");

struct rra_ray_history_begin_token {
   uint32_t wave_id;
   uint32_t launch_ids[3];
   uint32_t accel_struct_lo;
   uint32_t accel_struct_hi;
   uint32_t ray_flags;
   uint32_t cull_mask : 8;
   uint32_t stb_offset : 4;
   uint32_t stb_stride : 4;
   uint32_t miss_index : 16;
   float origin[3];
   float tmin;
   float direction[3];
   float tmax;
};
static_assert(sizeof(struct rra_ray_history_begin_token) == 64,
              "rra_ray_history_begin_token does not match RRA expectations");

struct rra_ray_history_begin2_token {
   struct rra_ray_history_begin_token base;
   uint32_t call_instruction_id;
   uint32_t unique_wave_id;
   uint32_t parent_unique_wave_id;
};
static_assert(sizeof(struct rra_ray_history_begin2_token) == 76,
              "rra_ray_history_begin2_token does not match RRA expectations");

struct rra_ray_history_end_token {
   uint32_t primitive_index;
   uint32_t geometry_index;
};
static_assert(sizeof(struct rra_ray_history_end_token) == 8,
              "rra_ray_history_end_token does not match RRA expectations");

struct rra_ray_history_end2_token {
   struct rra_ray_history_end_token base;
   uint32_t instance_index : 24;
   uint32_t hit_kind : 8;
   uint32_t iteration_count;
   uint32_t candidate_instance_count;
   float t;
};
static_assert(sizeof(struct rra_ray_history_end2_token) == 24,
              "rra_ray_history_end2_token does not match RRA expectations");

struct rra_ray_history_tlas_token {
   uint64_t addr;
};
static_assert(sizeof(struct rra_ray_history_tlas_token) == 8,
              "rra_ray_history_tlas_token does not match RRA expectations");

struct rra_ray_history_blas_token {
   uint64_t addr;
};
static_assert(sizeof(struct rra_ray_history_blas_token) == 8,
              "rra_ray_history_blas_token does not match RRA expectations");

struct rra_ray_history_call_token {
   uint32_t addr[2];
};
static_assert(sizeof(struct rra_ray_history_call_token) == 8,
              "rra_ray_history_call_token does not match RRA expectations");

struct rra_ray_history_call2_token {
   struct rra_ray_history_call_token base;
   uint32_t sbt_index;
};
static_assert(sizeof(struct rra_ray_history_call2_token) == 12,
              "rra_ray_history_call2_token does not match RRA expectations");

struct rra_ray_history_isec_token {
   float t;
   uint32_t hit_kind;
};
static_assert(sizeof(struct rra_ray_history_isec_token) == 8,
              "rra_ray_history_isec_token does not match RRA expectations");

struct rra_ray_history_timestamp_token {
   uint64_t gpu_timestamp;
};
static_assert(sizeof(struct rra_ray_history_timestamp_token) == 8,
              "rra_ray_history_timestamp_token does not match RRA expectations");

VkResult
radv_rra_dump_trace(VkQueue vk_queue, char *filename)
{
   VK_FROM_HANDLE(radv_queue, queue, vk_queue);
   struct radv_device *device = radv_queue_device(queue);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkDevice vk_device = radv_device_to_handle(device);

   VkResult result = vk_common_DeviceWaitIdle(vk_device);
   if (result != VK_SUCCESS)
      return result;

   uint64_t *accel_struct_offsets = NULL;
   uint64_t *ray_history_offsets = NULL;
   uint64_t *ray_history_sizes = NULL;
   struct hash_entry **hash_entries = NULL;
   FILE *file = NULL;
   struct set *used_blas = NULL;

   uint32_t struct_count = _mesa_hash_table_num_entries(device->rra_trace.accel_structs);
   accel_struct_offsets = calloc(struct_count, sizeof(uint64_t));
   if (!accel_struct_offsets)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   uint32_t dispatch_count =
      util_dynarray_num_elements(&device->rra_trace.ray_history, struct radv_rra_ray_history_data *);
   ray_history_offsets = calloc(dispatch_count, sizeof(uint64_t));
   if (!ray_history_offsets) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto cleanup;
   }

   ray_history_sizes = calloc(dispatch_count, sizeof(uint64_t));
   if (!ray_history_sizes) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto cleanup;
   }

   hash_entries = malloc(sizeof(*hash_entries) * struct_count);
   if (!hash_entries) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto cleanup;
   }

   file = fopen(filename, "w");
   if (!file) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto cleanup;
   }

   /*
    * The header contents can only be determined after all acceleration
    * structures have been dumped. An empty struct is written instead
    * to keep offsets intact.
    */
   struct rra_file_header header = {0};
   fwrite(&header, sizeof(struct rra_file_header), 1, file);

   uint64_t api_info_offset = (uint64_t)ftell(file);
   uint64_t api = RADV_RRA_API_VULKAN;
   fwrite(&api, sizeof(uint64_t), 1, file);

   uint64_t asic_info_offset = (uint64_t)ftell(file);
   rra_dump_asic_info(&pdev->info, file);

   uint64_t written_accel_struct_count = 0;

   struct hash_entry *last_entry = NULL;
   for (unsigned i = 0; (last_entry = _mesa_hash_table_next_entry(device->rra_trace.accel_structs, last_entry)); ++i)
      hash_entries[i] = last_entry;

   qsort(hash_entries, struct_count, sizeof(*hash_entries), accel_struct_entry_cmp);

   struct rra_copy_context copy_ctx = {
      .device = vk_device,
      .queue = vk_queue,
      .entries = hash_entries,
      .family_index = queue->vk.queue_family_index,
      .min_size = device->rra_trace.ray_history_buffer_size,
   };

   result = rra_copy_context_init(&copy_ctx);
   if (result != VK_SUCCESS)
      goto cleanup;

   used_blas = _mesa_set_create(NULL, _mesa_hash_u64, _mesa_key_u64_equal);
   if (!used_blas)
      goto cleanup;

   for (unsigned i = 0; i < struct_count; i++) {
      struct radv_rra_accel_struct_data *data = hash_entries[i]->data;
      if (!data->can_be_tlas)
         continue;

      void *mapped_data = rra_map_accel_struct_data(&copy_ctx, i);
      if (!mapped_data)
         continue;

      accel_struct_offsets[written_accel_struct_count] = (uint64_t)ftell(file);
      result = rra_dump_acceleration_structure(pdev, data, mapped_data, device->rra_trace.accel_struct_vas, used_blas,
                                               device->rra_trace.validate_as, file);

      rra_unmap_accel_struct_data(&copy_ctx, i);

      if (result == VK_SUCCESS)
         written_accel_struct_count++;
   }

   for (unsigned i = 0; i < struct_count; i++) {
      struct radv_rra_accel_struct_data *data = hash_entries[i]->data;
      if (data->can_be_tlas)
         continue;

      if (!_mesa_set_search(used_blas, &data->va))
         continue;

      void *mapped_data = rra_map_accel_struct_data(&copy_ctx, i);
      if (!mapped_data)
         continue;

      accel_struct_offsets[written_accel_struct_count] = (uint64_t)ftell(file);
      result = rra_dump_acceleration_structure(pdev, data, mapped_data, device->rra_trace.accel_struct_vas, used_blas,
                                               device->rra_trace.validate_as, file);

      rra_unmap_accel_struct_data(&copy_ctx, i);

      if (result == VK_SUCCESS)
         written_accel_struct_count++;
   }

   uint64_t ray_history_offset = (uint64_t)ftell(file);

   if (dispatch_count) {
      uint32_t ray_history_index = 0xFFFFFFFF;
      struct radv_rra_ray_history_data *ray_history = NULL;

      uint8_t *history = device->rra_trace.ray_history_data;
      struct radv_ray_history_header *history_header = (void *)history;

      uint32_t history_buffer_size_mb = device->rra_trace.ray_history_buffer_size / 1024 / 1024;
      uint32_t history_size_mb = history_header->offset / 1024 / 1024;
      if (history_header->offset > device->rra_trace.ray_history_buffer_size) {
         fprintf(stderr, "radv: rra: The ray history buffer size (%u MB) is to small. %u MB is required.\n",
                 history_buffer_size_mb, history_size_mb);
      } else {
         fprintf(stderr, "radv: rra: Ray history buffer size = %u MB, ray history size = %u MB.\n",
                 history_buffer_size_mb, history_size_mb);
      }

      uint32_t history_size = MIN2(history_header->offset, device->rra_trace.ray_history_buffer_size);

      uint32_t token_size;
      for (uint32_t offset = sizeof(struct radv_ray_history_header);; offset += token_size) {
         if (offset + sizeof(struct radv_packed_end_trace_token) > history_size)
            break;

         struct radv_packed_end_trace_token *src = (void *)(history + offset);
         token_size = src->header.hit ? sizeof(struct radv_packed_end_trace_token)
                                      : offsetof(struct radv_packed_end_trace_token, primitive_id);

         if (src->dispatch_index != ray_history_index) {
            ray_history_index = src->dispatch_index;
            assert(ray_history_index < dispatch_count);
            ray_history = *util_dynarray_element(&device->rra_trace.ray_history, struct radv_rra_ray_history_data *,
                                                 ray_history_index);

            assert(!ray_history_offsets[ray_history_index]);
            ray_history_offsets[ray_history_index] = (uint64_t)ftell(file);
            fwrite(&ray_history->metadata, sizeof(struct radv_rra_ray_history_metadata), 1, file);
         }

         uint32_t *dispatch_size = ray_history->metadata.dispatch_size.size;

         uint32_t x = src->header.launch_index % dispatch_size[0];
         uint32_t y = (src->header.launch_index / dispatch_size[0]) % dispatch_size[1];
         uint32_t z = src->header.launch_index / (dispatch_size[0] * dispatch_size[1]);

         struct rra_ray_history_id_token begin_id = {
            .id = src->header.launch_index,
            .has_control = true,
         };
         struct rra_ray_history_control_token begin_control = {
            .type = rra_ray_history_token_begin,
            .length = sizeof(struct rra_ray_history_begin_token) / 4,
         };
         struct rra_ray_history_begin_token begin = {
            .wave_id = src->header.launch_index / 32,
            .launch_ids = {x, y, z},
            .accel_struct_lo = src->accel_struct_lo,
            .accel_struct_hi = src->accel_struct_hi & 0x1FFFFFF,
            .ray_flags = src->flags,
            .cull_mask = src->cull_mask,
            .stb_offset = src->sbt_offset,
            .stb_stride = src->sbt_stride,
            .miss_index = src->miss_index,
            .origin[0] = src->origin[0],
            .origin[1] = src->origin[1],
            .origin[2] = src->origin[2],
            .tmin = src->tmin,
            .direction[0] = src->direction[0],
            .direction[1] = src->direction[1],
            .direction[2] = src->direction[2],
            .tmax = src->tmax,
         };
         fwrite(&begin_id, sizeof(begin_id), 1, file);
         fwrite(&begin_control, sizeof(begin_control), 1, file);
         fwrite(&begin, sizeof(begin), 1, file);
         ray_history_sizes[ray_history_index] += sizeof(begin_id) + sizeof(begin_control) + sizeof(begin);

         for (uint32_t i = 0; i < src->ahit_count; i++) {
            struct rra_ray_history_id_token ahit_status_id = {
               .id = src->header.launch_index,
               .has_control = true,
            };
            struct rra_ray_history_control_token ahit_status_control = {
               .type = rra_ray_history_token_ahit_status,
               .data = i == src->ahit_count - 1 ? 2 : 0,
            };
            fwrite(&ahit_status_id, sizeof(ahit_status_id), 1, file);
            fwrite(&ahit_status_control, sizeof(ahit_status_control), 1, file);
            ray_history_sizes[ray_history_index] += sizeof(ahit_status_id) + sizeof(ahit_status_control);
         }

         for (uint32_t i = 0; i < src->isec_count; i++) {
            struct rra_ray_history_id_token isec_status_id = {
               .id = src->header.launch_index,
               .has_control = true,
            };
            struct rra_ray_history_control_token isec_status_control = {
               .type = rra_ray_history_token_isec_status,
               .data = i == src->isec_count - 1 ? 2 : 0,
            };
            fwrite(&isec_status_id, sizeof(isec_status_id), 1, file);
            fwrite(&isec_status_control, sizeof(isec_status_control), 1, file);
            ray_history_sizes[ray_history_index] += sizeof(isec_status_id) + sizeof(isec_status_control);
         }

         struct rra_ray_history_id_token end_id = {
            .id = src->header.launch_index,
            .has_control = true,
         };
         struct rra_ray_history_control_token end_control = {
            .type = rra_ray_history_token_end2,
            .length = sizeof(struct rra_ray_history_end2_token) / 4,
         };
         struct rra_ray_history_end2_token end = {
            .base.primitive_index = 0xFFFFFFFF,
            .base.geometry_index = 0xFFFFFFFF,
            .iteration_count = src->iteration_count,
            .candidate_instance_count = src->instance_count,
         };

         if (src->header.hit) {
            end.base.primitive_index = src->primitive_id;
            end.base.geometry_index = src->geometry_id;
            end.instance_index = src->instance_id;
            end.hit_kind = src->hit_kind;
            end.t = src->t;
         }

         fwrite(&end_id, sizeof(end_id), 1, file);
         fwrite(&end_control, sizeof(end_control), 1, file);
         fwrite(&end, sizeof(end), 1, file);
         ray_history_sizes[ray_history_index] += sizeof(end_id) + sizeof(end_control) + sizeof(end);
      }

      for (uint32_t i = 0; i < dispatch_count; i++) {
         if (ray_history_offsets[i])
            continue;

         ray_history = *util_dynarray_element(&device->rra_trace.ray_history, struct radv_rra_ray_history_data *, i);
         ray_history_offsets[i] = (uint64_t)ftell(file);
         fwrite(&ray_history->metadata, sizeof(struct radv_rra_ray_history_metadata), 1, file);
      }

      history_header->offset = 1;
   }

   rra_copy_context_finish(&copy_ctx);

   uint64_t chunk_info_offset = (uint64_t)ftell(file);
   rra_dump_chunk_description(api_info_offset, 0, 8, "ApiInfo", RADV_RRA_ASIC_API_INFO_CHUNK_VERSION, file);
   rra_dump_chunk_description(asic_info_offset, 0, sizeof(struct rra_asic_info), "AsicInfo",
                              RADV_RRA_ASIC_API_INFO_CHUNK_VERSION, file);

   for (uint32_t i = 0; i < dispatch_count; i++) {
      rra_dump_chunk_description(ray_history_offsets[i], 0, sizeof(struct radv_rra_ray_history_metadata),
                                 "HistoryMetadata", RADV_RRA_RAY_HISTORY_CHUNK_VERSION, file);
      rra_dump_chunk_description(ray_history_offsets[i] + sizeof(struct radv_rra_ray_history_metadata), 0,
                                 ray_history_sizes[i], "HistoryTokensRaw", RADV_RRA_RAY_HISTORY_CHUNK_VERSION, file);
   }

   for (uint32_t i = 0; i < written_accel_struct_count; ++i) {
      uint64_t accel_struct_size;
      if (i == written_accel_struct_count - 1)
         accel_struct_size = (uint64_t)(ray_history_offset - accel_struct_offsets[i]);
      else
         accel_struct_size = (uint64_t)(accel_struct_offsets[i + 1] - accel_struct_offsets[i]);

      rra_dump_chunk_description(accel_struct_offsets[i], sizeof(struct rra_accel_struct_chunk_header),
                                 accel_struct_size, "RawAccelStruct", RADV_RRA_ACCEL_STRUCT_CHUNK_VERSION, file);
   }

   uint64_t file_end = (uint64_t)ftell(file);

   /* All info is available, dump header now */
   fseek(file, 0, SEEK_SET);
   rra_dump_header(file, chunk_info_offset, file_end - chunk_info_offset);

   result = VK_SUCCESS;
cleanup:
   if (file)
      fclose(file);

   _mesa_set_destroy(used_blas, NULL);
   free(hash_entries);
   free(ray_history_sizes);
   free(ray_history_offsets);
   free(accel_struct_offsets);
   return result;
}
