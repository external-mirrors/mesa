/*
 * Copyright © 2018 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "anv_private.h"
#include "vk_util.h"

#include "perf/intel_perf.h"
#include "perf/intel_perf_mdapi.h"

#include "util/mesa-sha1.h"

void
anv_physical_device_init_perf(struct anv_physical_device *device, int fd)
{
   struct intel_perf_config *perf = intel_perf_new(NULL);

   intel_perf_init_metrics(perf, &device->info, fd,
                           false /* pipeline statistics */,
                           true /* register snapshots */);

   if (!perf->n_queries)
      goto err;

   /* We need DRM_I915_PERF_PROP_HOLD_PREEMPTION support, only available in
    * perf revision 2.
    */
   if (!INTEL_DEBUG(DEBUG_NO_OACONFIG)) {
      if (!intel_perf_has_hold_preemption(perf))
         goto err;
   }

   device->perf = perf;

   /* Compute the number of commands we need to implement a performance
    * query.
    */
   const struct intel_perf_query_field_layout *layout = &perf->query_layout;
   device->n_perf_query_commands = 0;
   for (uint32_t f = 0; f < layout->n_fields; f++) {
      struct intel_perf_query_field *field = &layout->fields[f];

      switch (field->type) {
      case INTEL_PERF_QUERY_FIELD_TYPE_MI_RPC:
         device->n_perf_query_commands++;
         break;
      case INTEL_PERF_QUERY_FIELD_TYPE_SRM_PERFCNT:
      case INTEL_PERF_QUERY_FIELD_TYPE_SRM_RPSTAT:
      case INTEL_PERF_QUERY_FIELD_TYPE_SRM_OA_A:
      case INTEL_PERF_QUERY_FIELD_TYPE_SRM_OA_B:
      case INTEL_PERF_QUERY_FIELD_TYPE_SRM_OA_C:
      case INTEL_PERF_QUERY_FIELD_TYPE_SRM_OA_PEC:
         device->n_perf_query_commands += field->size / 4;
         break;
      default:
         UNREACHABLE("Unhandled register type");
      }
   }
   device->n_perf_query_commands *= 2; /* Begin & End */
   device->n_perf_query_commands += 1; /* availability */

   return;

err:
   intel_perf_free(perf);
}

void
anv_device_perf_init(struct anv_device *device)
{
   device->perf_fd = -1;
   device->perf_queue = NULL;
}

void
anv_device_perf_close(struct anv_device *device)
{
   if (device->perf_fd == -1)
      return;

   if (intel_bind_timeline_get_syncobj(&device->perf_timeline))
      intel_bind_timeline_finish(&device->perf_timeline, device->fd);
   close(device->perf_fd);
   device->perf_fd = -1;
}

static uint32_t
anv_device_perf_get_queue_context_or_exec_queue_id(struct anv_queue *queue)
{
   struct anv_device *device = queue->device;
   uint32_t context_or_exec_queue_id;

   switch (device->physical->info.kmd_type) {
   case INTEL_KMD_TYPE_I915:
      context_or_exec_queue_id = device->physical->has_vm_control ?
                                 queue->context_id : device->context_id;
      break;
   case INTEL_KMD_TYPE_XE:
      context_or_exec_queue_id = queue->exec_queue_id;
      break;
   default:
      UNREACHABLE("missing");
      context_or_exec_queue_id = 0;
   }

   return context_or_exec_queue_id;
}

static int
anv_device_perf_open(struct anv_device *device, struct anv_queue *queue, uint64_t metric_id)
{
   uint64_t period_exponent = 31; /* slowest sampling period */
   int ret;

   if (intel_perf_has_metric_sync(device->physical->perf)) {
      if (!intel_bind_timeline_init(&device->perf_timeline, device->fd))
         return -1;
   }

   ret = intel_perf_stream_open(device->physical->perf, device->fd,
                                anv_device_perf_get_queue_context_or_exec_queue_id(queue),
                                metric_id, period_exponent, true, true,
                                &device->perf_timeline);
   if (ret >= 0)
      device->perf_queue = queue;
   else
      intel_bind_timeline_finish(&device->perf_timeline, device->fd);

   return ret;
}

/* VK_INTEL_performance_query */
VkResult anv_InitializePerformanceApiINTEL(
    VkDevice                                    _device,
    const VkInitializePerformanceApiInfoINTEL*  pInitializeInfo)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   if (!device->physical->perf)
      return VK_ERROR_EXTENSION_NOT_PRESENT;

   /* Not much to do here */
   return VK_SUCCESS;
}

VkResult anv_GetPerformanceParameterINTEL(
    VkDevice                                    _device,
    VkPerformanceParameterTypeINTEL             parameter,
    VkPerformanceValueINTEL*                    pValue)
{
      ANV_FROM_HANDLE(anv_device, device, _device);

      if (!device->physical->perf)
         return VK_ERROR_EXTENSION_NOT_PRESENT;

      VkResult result = VK_SUCCESS;
      switch (parameter) {
      case VK_PERFORMANCE_PARAMETER_TYPE_HW_COUNTERS_SUPPORTED_INTEL:
         pValue->type = VK_PERFORMANCE_VALUE_TYPE_BOOL_INTEL;
         pValue->data.valueBool = VK_TRUE;
         break;

      case VK_PERFORMANCE_PARAMETER_TYPE_STREAM_MARKER_VALID_BITS_INTEL:
         pValue->type = VK_PERFORMANCE_VALUE_TYPE_UINT32_INTEL;
         pValue->data.value32 = 25;
         break;

      default:
         result = VK_ERROR_FEATURE_NOT_PRESENT;
         break;
      }

      return result;
}

VkResult anv_CmdSetPerformanceMarkerINTEL(
    VkCommandBuffer                             commandBuffer,
    const VkPerformanceMarkerInfoINTEL*         pMarkerInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->intel_perf_marker = pMarkerInfo->marker;

   return VK_SUCCESS;
}

VkResult anv_AcquirePerformanceConfigurationINTEL(
    VkDevice                                    _device,
    const VkPerformanceConfigurationAcquireInfoINTEL* pAcquireInfo,
    VkPerformanceConfigurationINTEL*            pConfiguration)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_performance_configuration_intel *config;

   config = vk_object_alloc(&device->vk, NULL, sizeof(*config),
                            VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL);
   if (!config)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (!INTEL_DEBUG(DEBUG_NO_OACONFIG)) {
      config->register_config =
         intel_perf_load_configuration(device->physical->perf, device->fd,
                                     INTEL_PERF_QUERY_GUID_MDAPI);
      if (!config->register_config) {
         vk_object_free(&device->vk, NULL, config);
         return VK_INCOMPLETE;
      }

      uint64_t ret =
         intel_perf_store_configuration(device->physical->perf, device->fd,
                                      config->register_config, NULL /* guid */);
      if (ret == 0) {
         ralloc_free(config->register_config);
         vk_object_free(&device->vk, NULL, config);
         return VK_INCOMPLETE;
      }

      config->config_id = ret;
   }

   *pConfiguration = anv_performance_configuration_intel_to_handle(config);

   return VK_SUCCESS;
}

VkResult anv_ReleasePerformanceConfigurationINTEL(
    VkDevice                                    _device,
    VkPerformanceConfigurationINTEL             _configuration)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_performance_configuration_intel, config, _configuration);

   if (!INTEL_DEBUG(DEBUG_NO_OACONFIG))
      intel_perf_remove_configuration(device->physical->perf, device->fd, config->config_id);

   ralloc_free(config->register_config);

   vk_object_free(&device->vk, NULL, config);

   return VK_SUCCESS;
}

static struct anv_queue *
anv_device_get_perf_queue(struct anv_device *device)
{
   for (uint32_t i = 0; i < device->queue_count; i++) {
      struct anv_queue *queue = &device->queues[i];
      const struct anv_queue_family *family = queue->family;

      if (family->supports_perf)
         return queue;
   }

   return NULL;
}

VkResult anv_QueueSetPerformanceConfigurationINTEL(
    VkQueue                                     _queue,
    VkPerformanceConfigurationINTEL             _configuration)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   ANV_FROM_HANDLE(anv_performance_configuration_intel, config, _configuration);
   struct anv_device *device = queue->device;

   if (queue != anv_device_get_perf_queue(device))
      return VK_ERROR_UNKNOWN;

   if (!INTEL_DEBUG(DEBUG_NO_OACONFIG)) {
      if (device->perf_fd < 0) {
         device->perf_fd = anv_device_perf_open(device, queue, config->config_id);
         if (device->perf_fd < 0)
            return VK_ERROR_INITIALIZATION_FAILED;
      } else {
         uint32_t context_or_exec_queue = anv_device_perf_get_queue_context_or_exec_queue_id(device->perf_queue);
         int ret = intel_perf_stream_set_metrics_id(device->physical->perf,
                                                    device->fd,
                                                    device->perf_fd,
                                                    context_or_exec_queue,
                                                    config->config_id,
                                                    &device->perf_timeline);
         if (ret < 0)
            return vk_device_set_lost(&device->vk, "i915-perf config failed: %m");
      }
   }

   return VK_SUCCESS;
}

void anv_UninitializePerformanceApiINTEL(
    VkDevice                                    _device)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   anv_device_perf_close(device);
}

/* VK_KHR_performance_query */
static const VkPerformanceCounterUnitKHR
intel_perf_counter_unit_to_vk_unit[] = {
   [INTEL_PERF_COUNTER_UNITS_BYTES]                                = VK_PERFORMANCE_COUNTER_UNIT_BYTES_KHR,
   [INTEL_PERF_COUNTER_UNITS_HZ]                                   = VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR,
   [INTEL_PERF_COUNTER_UNITS_NS]                                   = VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR,
   [INTEL_PERF_COUNTER_UNITS_US]                                   = VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR, /* todo */
   [INTEL_PERF_COUNTER_UNITS_PIXELS]                               = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_TEXELS]                               = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_THREADS]                              = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_PERCENT]                              = VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR,
   [INTEL_PERF_COUNTER_UNITS_MESSAGES]                             = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_NUMBER]                               = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_CYCLES]                               = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_EVENTS]                               = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_UTILIZATION]                          = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_EU_SENDS_TO_L3_CACHE_LINES]           = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_EU_ATOMIC_REQUESTS_TO_L3_CACHE_LINES] = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_EU_REQUESTS_TO_L3_CACHE_LINES]        = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
   [INTEL_PERF_COUNTER_UNITS_EU_BYTES_PER_L3_CACHE_LINE]           = VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR,
};

static const VkPerformanceCounterStorageKHR
intel_perf_counter_data_type_to_vk_storage[] = {
   [INTEL_PERF_COUNTER_DATA_TYPE_BOOL32] = VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR,
   [INTEL_PERF_COUNTER_DATA_TYPE_UINT32] = VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR,
   [INTEL_PERF_COUNTER_DATA_TYPE_UINT64] = VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR,
   [INTEL_PERF_COUNTER_DATA_TYPE_FLOAT]  = VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR,
   [INTEL_PERF_COUNTER_DATA_TYPE_DOUBLE] = VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR,
};

VkResult anv_EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    uint32_t*                                   pCounterCount,
    VkPerformanceCounterKHR*                    pCounters,
    VkPerformanceCounterDescriptionKHR*         pCounterDescriptions)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);
   struct intel_perf_config *perf = pdevice->perf;

   uint32_t desc_count = *pCounterCount;

   VK_OUTARRAY_MAKE_TYPED(VkPerformanceCounterKHR, out, pCounters, pCounterCount);
   VK_OUTARRAY_MAKE_TYPED(VkPerformanceCounterDescriptionKHR, out_desc,
                          pCounterDescriptions, &desc_count);

   /* We cannot support performance queries on anything other than RCS,
    * because the MI_REPORT_PERF_COUNT command is not available on other
    * engines.
    */
   struct anv_queue_family *queue_family =
      &pdevice->queue.families[queueFamilyIndex];
   if (queue_family->engine_class != INTEL_ENGINE_CLASS_RENDER)
      return vk_outarray_status(&out);

   for (int c = 0; c < (perf ? perf->n_counters : 0); c++) {
      const struct intel_perf_query_counter *intel_counter = perf->counter_infos[c].counter;

      vk_outarray_append_typed(VkPerformanceCounterKHR, &out, counter) {
         counter->unit = intel_perf_counter_unit_to_vk_unit[intel_counter->units];
         counter->scope = VK_PERFORMANCE_COUNTER_SCOPE_COMMAND_KHR;
         counter->storage = intel_perf_counter_data_type_to_vk_storage[intel_counter->data_type];

         unsigned char sha1_result[20];
         _mesa_sha1_compute(intel_counter->symbol_name,
                            strlen(intel_counter->symbol_name),
                            sha1_result);
         memcpy(counter->uuid, sha1_result, sizeof(counter->uuid));
      }

      vk_outarray_append_typed(VkPerformanceCounterDescriptionKHR, &out_desc, desc) {
         desc->flags = 0; /* None so far. */
         snprintf(desc->name, sizeof(desc->name), "%s",
                  INTEL_DEBUG(DEBUG_PERF_SYMBOL_NAMES) ?
                  intel_counter->symbol_name :
                  intel_counter->name);
         snprintf(desc->category, sizeof(desc->category), "%s", intel_counter->category);
         snprintf(desc->description, sizeof(desc->description), "%s", intel_counter->desc);
      }
   }

   return vk_outarray_status(&out);
}

void anv_GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(
    VkPhysicalDevice                            physicalDevice,
    const VkQueryPoolPerformanceCreateInfoKHR*  pPerformanceQueryCreateInfo,
    uint32_t*                                   pNumPasses)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);
   struct intel_perf_config *perf = pdevice->perf;

   if (!perf) {
      *pNumPasses = 0;
      return;
   }

   *pNumPasses = intel_perf_get_n_passes(perf,
                                       pPerformanceQueryCreateInfo->pCounterIndices,
                                       pPerformanceQueryCreateInfo->counterIndexCount,
                                       NULL);
}

VkResult anv_AcquireProfilingLockKHR(
    VkDevice                                    _device,
    const VkAcquireProfilingLockInfoKHR*        pInfo)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct intel_perf_config *perf = device->physical->perf;
   struct intel_perf_query_info *first_metric_set = &perf->queries[0];
   int fd = -1;

   assert(device->perf_fd == -1);

   if (!INTEL_DEBUG(DEBUG_NO_OACONFIG)) {
      struct anv_queue *queue = anv_device_get_perf_queue(device);

      if (queue == NULL)
         return VK_ERROR_UNKNOWN;
      fd = anv_device_perf_open(device, queue, first_metric_set->oa_metrics_set_id);
      if (fd < 0)
         return VK_TIMEOUT;
   }

   device->perf_fd = fd;
   return VK_SUCCESS;
}

void anv_ReleaseProfilingLockKHR(
    VkDevice                                    _device)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   anv_device_perf_close(device);
}

void
anv_perf_write_pass_results(struct intel_perf_config *perf,
                            struct anv_query_pool *pool, uint32_t pass,
                            const struct intel_perf_query_result *accumulated_results,
                            union VkPerformanceCounterResultKHR *results)
{
   const struct intel_perf_query_info *query = pool->pass_query[pass];

   for (uint32_t c = 0; c < pool->n_counters; c++) {
      const struct intel_perf_counter_pass *counter_pass = &pool->counter_pass[c];

      if (counter_pass->query != query)
         continue;

      switch (pool->pass_query[pass]->kind) {
      case INTEL_PERF_QUERY_TYPE_PIPELINE: {
         assert(counter_pass->counter->data_type == INTEL_PERF_COUNTER_DATA_TYPE_UINT64);
         uint32_t accu_offset = counter_pass->counter->offset / sizeof(uint64_t);
         results[c].uint64 = accumulated_results->accumulator[accu_offset];
         break;
      }

      case INTEL_PERF_QUERY_TYPE_OA:
      case INTEL_PERF_QUERY_TYPE_RAW:
         switch (counter_pass->counter->data_type) {
         case INTEL_PERF_COUNTER_DATA_TYPE_UINT64:
            results[c].uint64 =
               counter_pass->counter->oa_counter_read_uint64(perf,
                                                             counter_pass->query,
                                                             accumulated_results);
            break;
         case INTEL_PERF_COUNTER_DATA_TYPE_FLOAT:
            results[c].float32 =
               counter_pass->counter->oa_counter_read_float(perf,
                                                            counter_pass->query,
                                                            accumulated_results);
            break;
         default:
            /* So far we aren't using uint32, double or bool32... */
            UNREACHABLE("unexpected counter data type");
         }
         break;

      default:
         UNREACHABLE("invalid query type");
      }

      /* The Vulkan extension only has nanoseconds as a unit */
      if (counter_pass->counter->units == INTEL_PERF_COUNTER_UNITS_US) {
         assert(counter_pass->counter->data_type == INTEL_PERF_COUNTER_DATA_TYPE_UINT64);
         results[c].uint64 *= 1000;
      }
   }
}
