/*
 * Copyright © 2021 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <perfetto.h>

#include "tu_perfetto.h"
#include "tu_buffer.h"
#include "tu_device.h"
#include "tu_queue.h"
#include "tu_image.h"

#include "util/hash_table.h"
#include "util/perf/u_perfetto.h"
#include "util/perf/u_perfetto_renderpass.h"

#include "tu_cmd_buffer.h"
#include "tu_tracepoints.h"
#include "tu_tracepoints_perfetto.h"
#include "vk_object.h"
#include "vk_util.h"

/* we can't include tu_knl.h and tu_device.h */

int
tu_device_get_gpu_timestamp(struct tu_device *dev,
                            uint64_t *ts);
int
tu_device_get_suspend_count(struct tu_device *dev,
                            uint64_t *suspend_count);
uint64_t
tu_device_ticks_to_ns(struct tu_device *dev, uint64_t ts);

struct u_trace_context *
tu_device_get_u_trace(struct tu_device *device);

/**
 * Queue-id's
 */
enum {
   DEFAULT_HW_QUEUE_ID,
};

/**
 * Render-stage id's
 */
enum tu_stage_id {
   CMD_BUFFER_STAGE_ID,
   CMD_BUFFER_ANNOTATION_STAGE_ID,
   RENDER_PASS_STAGE_ID,
   SECONDARY_CMD_BUFFER_STAGE_ID,
   CMD_BUFFER_ANNOTATION_RENDER_PASS_STAGE_ID,
   BINNING_STAGE_ID,
   GMEM_STAGE_ID,
   BYPASS_STAGE_ID,
   BLIT_STAGE_ID,
   DRAW_STAGE_ID,
   COMPUTE_STAGE_ID,
   CLEAR_SYSMEM_STAGE_ID,
   CLEAR_GMEM_STAGE_ID,
   GENERIC_CLEAR_STAGE_ID,
   GMEM_LOAD_STAGE_ID,
   GMEM_STORE_STAGE_ID,
   SYSMEM_RESOLVE_STAGE_ID,
   // TODO add the rest from fd_stage_id
};

static const struct {
   const char *name;
   const char *desc;
} queues[] = {
   [DEFAULT_HW_QUEUE_ID] = {"GPU Queue 0", "Default Adreno Hardware Queue"},
};

static const struct {
   const char *name;
   const char *desc;
} stages[] = {
   [CMD_BUFFER_STAGE_ID]     = { "Command Buffer" },
   [CMD_BUFFER_ANNOTATION_STAGE_ID]     = { "Annotation", "Command Buffer Annotation" },
   [RENDER_PASS_STAGE_ID]    = { "Render Pass" },
   [SECONDARY_CMD_BUFFER_STAGE_ID] = { "Secondary Command Buffer" },
   [CMD_BUFFER_ANNOTATION_RENDER_PASS_STAGE_ID]    = { "Annotation", "Render Pass Command Buffer Annotation" },
   [BINNING_STAGE_ID]        = { "Binning", "Perform Visibility pass and determine target bins" },
   [GMEM_STAGE_ID]           = { "GMEM", "Rendering to GMEM" },
   [BYPASS_STAGE_ID]         = { "Bypass", "Rendering to system memory" },
   [BLIT_STAGE_ID]           = { "Blit", "Performing a Blit operation" },
   [DRAW_STAGE_ID]           = { "Draw", "Performing a graphics-pipeline draw" },
   [COMPUTE_STAGE_ID]        = { "Compute", "Compute job" },
   [CLEAR_SYSMEM_STAGE_ID]   = { "Clear Sysmem", "" },
   [CLEAR_GMEM_STAGE_ID]     = { "Clear GMEM", "Per-tile (GMEM) clear" },
   [GENERIC_CLEAR_STAGE_ID]  = { "Clear Sysmem/Gmem", ""},
   [GMEM_LOAD_STAGE_ID]      = { "GMEM Load", "Per tile system memory to GMEM load" },
   [GMEM_STORE_STAGE_ID]     = { "GMEM Store", "Per tile GMEM to system memory store" },
   [SYSMEM_RESOLVE_STAGE_ID] = { "SysMem Resolve", "System memory MSAA resolve" },
   // TODO add the rest
};

static uint32_t gpu_clock_id;
static uint64_t next_clock_sync_ns; /* cpu time of next clk sync */

/**
 * The timestamp at the point where we first emitted the clock_sync..
 * this  will be a *later* timestamp that the first GPU traces (since
 * we capture the first clock_sync from the CPU *after* the first GPU
 * tracepoints happen).  To avoid confusing perfetto we need to drop
 * the GPU traces with timestamps before this.
 */
static uint64_t sync_gpu_ts;

static uint64_t last_suspend_count;

static uint64_t gpu_max_timestamp;
static uint64_t gpu_timestamp_offset;

struct TuRenderpassTraits : public perfetto::DefaultDataSourceTraits {
   using IncrementalStateType = MesaRenderpassIncrementalState;
};

class TuRenderpassDataSource : public MesaRenderpassDataSource<TuRenderpassDataSource,
                                                               TuRenderpassTraits> {
   void OnStart(const StartArgs &args) override
   {
      MesaRenderpassDataSource<TuRenderpassDataSource, TuRenderpassTraits>::OnStart(args);

      /* Note: clock_id's below 128 are reserved.. for custom clock sources,
       * using the hash of a namespaced string is the recommended approach.
       * See: https://perfetto.dev/docs/concepts/clock-sync
       */
      gpu_clock_id =
         _mesa_hash_string("org.freedesktop.mesa.freedreno") | 0x80000000;

      gpu_timestamp_offset = 0;
      gpu_max_timestamp = 0;
      last_suspend_count = 0;
   }
};

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(TuRenderpassDataSource);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(TuRenderpassDataSource);

static void
setup_incremental_state(TuRenderpassDataSource::TraceContext &ctx)
{
   auto state = ctx.GetIncrementalState();
   if (!state->was_cleared)
      return;

   state->was_cleared = false;

   PERFETTO_LOG("Sending renderstage descriptors");

   auto packet = ctx.NewTracePacket();

   /* This must be set before interned data is sent. */
   packet->set_sequence_flags(perfetto::protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED);

   packet->set_timestamp(0);

   auto event = packet->set_gpu_render_stage_event();
   event->set_gpu_id(0);

   auto spec = event->set_specifications();

   for (unsigned i = 0; i < ARRAY_SIZE(queues); i++) {
      auto desc = spec->add_hw_queue();

      desc->set_name(queues[i].name);
      desc->set_description(queues[i].desc);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(stages); i++) {
      auto desc = spec->add_stage();

      desc->set_name(stages[i].name);
      if (stages[i].desc)
         desc->set_description(stages[i].desc);
   }
}

static struct tu_perfetto_stage *
stage_push(struct tu_device *dev)
{
   struct tu_perfetto_state *p = &dev->perfetto;

   if (p->stage_depth >= ARRAY_SIZE(p->stages)) {
      p->skipped_depth++;
      return NULL;
   }

   return &p->stages[p->stage_depth++];
}

static struct tu_perfetto_stage *
stage_pop(struct tu_device *dev)
{
   struct tu_perfetto_state *p = &dev->perfetto;

   if (!p->stage_depth)
      return NULL;

   if (p->skipped_depth) {
      p->skipped_depth--;
      return NULL;
   }

   return &p->stages[--p->stage_depth];
}

static void
stage_start(struct tu_device *dev,
            uint64_t ts_ns,
            enum tu_stage_id stage_id,
            const char *app_event,
            const void *payload = nullptr,
            size_t payload_size = 0,
            const void *indirect = nullptr,
            trace_payload_as_extra_func payload_as_extra = nullptr)
{
   struct tu_perfetto_stage *stage = stage_push(dev);

   if (!stage) {
      PERFETTO_ELOG("stage %d is nested too deep", stage_id);
      return;
   }

   if (payload) {
      void* new_payload = malloc(payload_size);
      if (new_payload)
         memcpy(new_payload, payload, payload_size);
      else
         PERFETTO_ELOG("Failed to allocate payload for stage %d", stage_id);
      payload = new_payload;
   }

   *stage = (struct tu_perfetto_stage) {
      .stage_id = stage_id,
      .stage_iid = 0,
      .start_ts = ts_ns,
      .payload = payload,
      .start_payload_function = (void *) payload_as_extra,
   };

   if (app_event) {
      TuRenderpassDataSource::Trace([=](auto tctx) {
         stage->stage_iid =
            tctx.GetDataSourceLocked()->debug_marker_stage(tctx, app_event);
      });
   }
}

static void
stage_end(struct tu_device *dev, uint64_t ts_ns, enum tu_stage_id stage_id,
          const void *flush_data,
          const void* payload = nullptr,
          const void *indirect = nullptr,
          trace_payload_as_extra_func payload_as_extra = nullptr)
{
   struct tu_perfetto_stage *stage = stage_pop(dev);
   auto trace_flush_data =
      (const struct tu_u_trace_submission_data *) flush_data;
   uint32_t submission_id = trace_flush_data->submission_id;
   uint64_t gpu_ts_offset = trace_flush_data->gpu_ts_offset;

   if (!stage)
      return;

   uint64_t duration = ts_ns - stage->start_ts;
   /* Zero duration can only happen when tracepoints did not happen on GPU. */
   if (duration == 0)
      return;

   if (stage->stage_id != stage_id) {
      PERFETTO_ELOG("stage %d ended while stage %d is expected",
            stage_id, stage->stage_id);
      return;
   }

   /* If we haven't managed to calibrate the alignment between GPU and CPU
    * timestamps yet, then skip this trace, otherwise perfetto won't know
    * what to do with it.
    */
   if (!sync_gpu_ts)
      return;

   TuRenderpassDataSource::Trace([=](TuRenderpassDataSource::TraceContext tctx) {
      setup_incremental_state(tctx);

      auto packet = tctx.NewTracePacket();

      gpu_max_timestamp = MAX2(gpu_max_timestamp, ts_ns + gpu_ts_offset);

      packet->set_timestamp(stage->start_ts + gpu_ts_offset);
      packet->set_timestamp_clock_id(gpu_clock_id);

      auto event = packet->set_gpu_render_stage_event();
      event->set_event_id(0); // ???
      event->set_hw_queue_id(DEFAULT_HW_QUEUE_ID);
      event->set_duration(ts_ns - stage->start_ts);
      if (stage->stage_iid)
         event->set_stage_iid(stage->stage_iid);
      else
         event->set_stage_id(stage->stage_id);
      event->set_context((uintptr_t) dev);
      event->set_submission_id(submission_id);

      if (stage->payload) {
         if (stage->start_payload_function)
            ((trace_payload_as_extra_func) stage->start_payload_function)(
               event, stage->payload, nullptr);
         free((void *)stage->payload);
      }

      if (payload && payload_as_extra)
         payload_as_extra(event, payload, indirect);
   });
}

class TuMemoryDataSource : public perfetto::DataSource<TuMemoryDataSource> {
 public:
   void OnSetup(const SetupArgs &) override
   {
   }

   void OnStart(const StartArgs &) override
   {
      PERFETTO_LOG("Memory tracing started");
   }

   void OnStop(const StopArgs &) override
   {
      PERFETTO_LOG("Memory tracing stopped");
   }
};

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(TuMemoryDataSource);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(TuMemoryDataSource);


#ifdef __cplusplus
extern "C" {
#endif

void
tu_perfetto_init(void)
{
   util_perfetto_init();

   {
   perfetto::DataSourceDescriptor dsd;
#if DETECT_OS_ANDROID
     // Android tooling expects this data source name
     dsd.set_name("gpu.renderstages");
#else
      dsd.set_name("gpu.renderstages.msm");
#endif
      TuRenderpassDataSource::Register(dsd);
   }

   {
     perfetto::DataSourceDescriptor dsd;
     dsd.set_name("gpu.memory.msm");
     TuMemoryDataSource::Register(dsd);
   }
}

static void
emit_sync_timestamp(uint64_t cpu_ts, uint64_t gpu_ts)
{
   TuRenderpassDataSource::Trace([=](auto tctx) {
      MesaRenderpassDataSource<TuRenderpassDataSource,
                               TuRenderpassTraits>::EmitClockSync(tctx, cpu_ts,
                                                                  gpu_ts, gpu_clock_id);
   });
}

uint64_t
tu_perfetto_begin_submit()
{
   return perfetto::base::GetBootTimeNs().count();
}

static struct tu_perfetto_clocks
sync_clocks(struct tu_device *dev,
            const struct tu_perfetto_clocks *gpu_clocks)
{
   struct tu_perfetto_clocks clocks {};
   if (gpu_clocks) {
      clocks = *gpu_clocks;
   }

   clocks.cpu = perfetto::base::GetBootTimeNs().count();

   if (gpu_clocks) {
      /* TODO: It would be better to use CPU time that comes
       * together with GPU time from the KGSL, but it's not
       * equal to GetBootTimeNs.
       */

      clocks.gpu_ts_offset = MAX2(gpu_timestamp_offset, clocks.gpu_ts_offset);
      gpu_timestamp_offset = clocks.gpu_ts_offset;
      sync_gpu_ts = clocks.gpu_ts + clocks.gpu_ts_offset;
   } else {
      clocks.gpu_ts = 0;
      clocks.gpu_ts_offset = gpu_timestamp_offset;

      if (clocks.cpu < next_clock_sync_ns)
         return clocks;

      if (tu_device_get_gpu_timestamp(dev, &clocks.gpu_ts)) {
         PERFETTO_ELOG("Could not sync CPU and GPU clocks");
         return {};
      }

      clocks.gpu_ts = tu_device_ticks_to_ns(dev, clocks.gpu_ts);

      /* get cpu timestamp again because tu_device_get_gpu_timestamp can take
       * >100us
       */
      clocks.cpu = perfetto::base::GetBootTimeNs().count();

      uint64_t current_suspend_count = 0;
      /* If we fail to get it we will use a fallback */
      tu_device_get_suspend_count(dev, &current_suspend_count);

      /* GPU timestamp is being reset after suspend-resume cycle.
       * Perfetto requires clock snapshots to be monotonic,
       * so we have to fix-up the time.
       */
      if (current_suspend_count != last_suspend_count) {
         gpu_timestamp_offset = gpu_max_timestamp;
         last_suspend_count = current_suspend_count;
      }
      clocks.gpu_ts_offset = gpu_timestamp_offset;

      uint64_t gpu_absolute_ts = clocks.gpu_ts + clocks.gpu_ts_offset;

      /* Fallback check, detect non-monotonic cases which would happen
       * if we cannot retrieve suspend count.
       */
      if (sync_gpu_ts > gpu_absolute_ts) {
         gpu_absolute_ts += (gpu_max_timestamp - gpu_timestamp_offset);
         gpu_timestamp_offset = gpu_max_timestamp;
         clocks.gpu_ts = gpu_absolute_ts - gpu_timestamp_offset;
      }

      if (sync_gpu_ts > gpu_absolute_ts) {
         PERFETTO_ELOG("Non-monotonic gpu timestamp detected, bailing out");
         return {};
      }

      gpu_max_timestamp = clocks.gpu_ts;
      sync_gpu_ts = clocks.gpu_ts;
      next_clock_sync_ns = clocks.cpu + 30000000;
   }

   return clocks;
}

struct tu_perfetto_clocks
tu_perfetto_end_submit(struct tu_queue *queue,
                       uint32_t submission_id,
                       uint64_t start_ts,
                       struct tu_perfetto_clocks *gpu_clocks)
{
   struct tu_device *dev = queue->device;
   if (!u_trace_perfetto_active(tu_device_get_u_trace(dev)))
      return {};

   struct tu_perfetto_clocks clocks = sync_clocks(dev, gpu_clocks);
   if (clocks.gpu_ts > 0)
      emit_sync_timestamp(clocks.cpu, clocks.gpu_ts + clocks.gpu_ts_offset);

   TuRenderpassDataSource::Trace([=](TuRenderpassDataSource::TraceContext tctx) {
      auto packet = tctx.NewTracePacket();

      packet->set_timestamp(start_ts);

      auto event = packet->set_vulkan_api_event();
      auto submit = event->set_vk_queue_submit();

      submit->set_duration_ns(clocks.cpu - start_ts);
      submit->set_vk_queue((uintptr_t) queue);
      submit->set_submission_id(submission_id);
   });

   return clocks;
}

/*
 * Trace callbacks, called from u_trace once the timestamps from GPU have been
 * collected.
 *
 * The default "extra" funcs are code-generated into tu_tracepoints_perfetto.h
 * and just take the tracepoint's args and add them as name/value pairs in the
 * perfetto events.  This file can usually just map a tu_perfetto_* to
 * stage_start/end with a call to that codegenned "extra" func.  But you can
 * also provide your own entrypoint and extra funcs if you want to change that
 * mapping.
 */

#define CREATE_EVENT_CALLBACK(event_name, stage_id)                                 \
   void tu_perfetto_start_##event_name(                                             \
      struct tu_device *dev, uint64_t ts_ns, uint16_t tp_idx,                       \
      const void *flush_data, const struct trace_start_##event_name *payload,       \
      const void *indirect_data)                                                    \
   {                                                                                \
      stage_start(                                                                  \
         dev, ts_ns, stage_id, NULL, payload, sizeof(*payload), indirect_data,      \
         (trace_payload_as_extra_func) &trace_payload_as_extra_start_##event_name); \
   }                                                                                \
                                                                                    \
   void tu_perfetto_end_##event_name(                                               \
      struct tu_device *dev, uint64_t ts_ns, uint16_t tp_idx,                       \
      const void *flush_data, const struct trace_end_##event_name *payload,         \
      const void *indirect_data)                                                    \
   {                                                                                \
      stage_end(                                                                    \
         dev, ts_ns, stage_id, flush_data, payload, indirect_data,                  \
         (trace_payload_as_extra_func) &trace_payload_as_extra_end_##event_name);   \
   }

CREATE_EVENT_CALLBACK(cmd_buffer, CMD_BUFFER_STAGE_ID)
CREATE_EVENT_CALLBACK(secondary_cmd_buffer, SECONDARY_CMD_BUFFER_STAGE_ID)
CREATE_EVENT_CALLBACK(render_pass, RENDER_PASS_STAGE_ID)
CREATE_EVENT_CALLBACK(binning_ib, BINNING_STAGE_ID)
CREATE_EVENT_CALLBACK(draw_ib_gmem, GMEM_STAGE_ID)
CREATE_EVENT_CALLBACK(draw_ib_sysmem, BYPASS_STAGE_ID)
CREATE_EVENT_CALLBACK(blit, BLIT_STAGE_ID)
CREATE_EVENT_CALLBACK(draw, DRAW_STAGE_ID)
CREATE_EVENT_CALLBACK(compute, COMPUTE_STAGE_ID)
CREATE_EVENT_CALLBACK(compute_indirect, COMPUTE_STAGE_ID)
CREATE_EVENT_CALLBACK(generic_clear, GENERIC_CLEAR_STAGE_ID)
CREATE_EVENT_CALLBACK(gmem_clear, CLEAR_GMEM_STAGE_ID)
CREATE_EVENT_CALLBACK(sysmem_clear, CLEAR_SYSMEM_STAGE_ID)
CREATE_EVENT_CALLBACK(sysmem_clear_all, CLEAR_SYSMEM_STAGE_ID)
CREATE_EVENT_CALLBACK(gmem_load, GMEM_LOAD_STAGE_ID)
CREATE_EVENT_CALLBACK(gmem_store, GMEM_STORE_STAGE_ID)
CREATE_EVENT_CALLBACK(sysmem_resolve, SYSMEM_RESOLVE_STAGE_ID)

void
tu_perfetto_start_cmd_buffer_annotation(
   struct tu_device *dev,
   uint64_t ts_ns,
   uint16_t tp_idx,
   const void *flush_data,
   const struct trace_start_cmd_buffer_annotation *payload,
   const void *indirect_data)
{
   /* No extra func necessary, the only arg is in the end payload.*/
   stage_start(dev, ts_ns, CMD_BUFFER_ANNOTATION_STAGE_ID, payload->str, payload,
               sizeof(*payload), NULL);
}

void
tu_perfetto_end_cmd_buffer_annotation(
   struct tu_device *dev,
   uint64_t ts_ns,
   uint16_t tp_idx,
   const void *flush_data,
   const struct trace_end_cmd_buffer_annotation *payload,
   const void *indirect_data)
{
   /* Pass the payload string as the app_event, which will appear right on the
    * event block, rather than as metadata inside.
    */
   stage_end(dev, ts_ns, CMD_BUFFER_ANNOTATION_STAGE_ID, flush_data,
             payload, NULL);
}

void
tu_perfetto_start_cmd_buffer_annotation_rp(
   struct tu_device *dev,
   uint64_t ts_ns,
   uint16_t tp_idx,
   const void *flush_data,
   const struct trace_start_cmd_buffer_annotation_rp *payload,
   const void *indirect_data)
{
   /* No extra func necessary, the only arg is in the end payload.*/
   stage_start(dev, ts_ns, CMD_BUFFER_ANNOTATION_RENDER_PASS_STAGE_ID,
               payload->str, payload, sizeof(*payload), NULL);
}

void
tu_perfetto_end_cmd_buffer_annotation_rp(
   struct tu_device *dev,
   uint64_t ts_ns,
   uint16_t tp_idx,
   const void *flush_data,
   const struct trace_end_cmd_buffer_annotation_rp *payload,
   const void *indirect_data)
{
   /* Pass the payload string as the app_event, which will appear right on the
    * event block, rather than as metadata inside.
    */
   stage_end(dev, ts_ns, CMD_BUFFER_ANNOTATION_RENDER_PASS_STAGE_ID,
             flush_data, payload, NULL);
}


static void
log_mem(struct tu_device *dev, struct tu_buffer *buffer, struct tu_image *image,
        perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::Operation op)
{
   TuMemoryDataSource::Trace([=](TuMemoryDataSource::TraceContext tctx) {
      auto packet = tctx.NewTracePacket();

      packet->set_timestamp(perfetto::base::GetBootTimeNs().count());

      auto event = packet->set_vulkan_memory_event();

      event->set_timestamp(perfetto::base::GetBootTimeNs().count());
      event->set_operation(op);
      event->set_pid(getpid());

      if (buffer) {
         event->set_source(perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::SOURCE_BUFFER);
         event->set_memory_size(buffer->vk.size);
         if (buffer->bo)
            event->set_memory_address(buffer->vk.device_address);
      } else {
         assert(image);
         event->set_source(perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::SOURCE_IMAGE);
         event->set_memory_size(image->layout[0].size);
         if (image->bo)
            event->set_memory_address(image->iova);
      }

   });
}

void
tu_perfetto_log_create_buffer(struct tu_device *dev, struct tu_buffer *buffer)
{
   log_mem(dev, buffer, NULL, perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_CREATE);
}

void
tu_perfetto_log_bind_buffer(struct tu_device *dev, struct tu_buffer *buffer)
{
   log_mem(dev, buffer, NULL, perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_BIND);
}

void
tu_perfetto_log_destroy_buffer(struct tu_device *dev, struct tu_buffer *buffer)
{
   log_mem(dev, buffer, NULL, buffer->bo ?
      perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_DESTROY_BOUND :
      perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_DESTROY);
}

void
tu_perfetto_log_create_image(struct tu_device *dev, struct tu_image *image)
{
   log_mem(dev, NULL, image, perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_CREATE);
}

void
tu_perfetto_log_bind_image(struct tu_device *dev, struct tu_image *image)
{
   log_mem(dev, NULL, image, perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_BIND);
}

void
tu_perfetto_log_destroy_image(struct tu_device *dev, struct tu_image *image)
{
   log_mem(dev, NULL, image, image->bo ?
      perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_DESTROY_BOUND :
      perfetto::protos::pbzero::perfetto_pbzero_enum_VulkanMemoryEvent::OP_DESTROY);
}



void
tu_perfetto_set_debug_utils_object_name(const VkDebugUtilsObjectNameInfoEXT *pNameInfo)
{
   TuRenderpassDataSource::Trace([=](auto tctx) {
      /* Do we need this for SEQ_INCREMENTAL_STATE_CLEARED for the object name to stick? */
      setup_incremental_state(tctx);

      tctx.GetDataSourceLocked()->SetDebugUtilsObjectNameEXT(tctx, pNameInfo);
   });
}

void
tu_perfetto_refresh_debug_utils_object_name(const struct vk_object_base *object)
{
   TuRenderpassDataSource::Trace([=](auto tctx) {
      /* Do we need this for SEQ_INCREMENTAL_STATE_CLEARED for the object name to stick? */
      setup_incremental_state(tctx);

      tctx.GetDataSourceLocked()->RefreshSetDebugUtilsObjectNameEXT(tctx, object);
   });
}

#ifdef __cplusplus
}
#endif
