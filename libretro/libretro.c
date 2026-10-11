

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <streams/file_stream.h>
#include <compat/strl.h>
#include <libretro.h>

#include "../common.h"
#include "../memmap.h"
#include "libretro_core_options.h"

#include "../gba_memory.h"
#include "../gba_cc_lut.h"

#if defined(GPSP_BENCHMARK)
#include "gpsp_benchmark.h"
#include "../gpsp_benchmark_timing.h"

static struct gpsp_benchmark_frame s_benchmark_frame;

void gpsp_benchmark_get_last_frame(struct gpsp_benchmark_frame *frame)
{
   *frame = s_benchmark_frame;
}
#endif

#if defined(GPSP_RV32_JIT)
#include "../riscv/rv32_block_runtime.h"
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
#include "../riscv/rv32_arm_memory.h"
#include "../riscv/rv32_bios_probe.h"
#endif
#endif

#if defined(GPSP_BENCHMARK)
void gpsp_benchmark_get_engine(struct gpsp_benchmark_engine *engine)
{
   memset(engine, 0, sizeof(*engine));
#if defined(GPSP_RV32_JIT)
   gpsp_rv32_block_get_stats(
         &engine->blocks, &engine->scalar_steps, &engine->cache_flushes,
         &engine->code_bytes, &engine->ready);
   engine->scalar_runs = gpsp_rv32_block_get_interpreter_runs();
   engine->ram_flushes = gpsp_rv32_block_get_ram_flushes();
   engine->ram_code_bytes = gpsp_rv32_block_get_ram_code_bytes();
   struct gpsp_rv32_cache_stats cache;
   gpsp_rv32_block_get_cache_stats(&cache);
   engine->rom_code_capacity_bytes = cache.rom_capacity_bytes;
   engine->ram_code_capacity_bytes = cache.ram_capacity_bytes;
   engine->rom_discarded_bytes = cache.rom_discarded_bytes;
   engine->ram_discarded_bytes = cache.ram_discarded_bytes;
   engine->chunk_retries = cache.chunk_retries;
   engine->chunk_tail_bytes = cache.chunk_tail_bytes;
   struct gpsp_rv32_phase_cycles phases;
   gpsp_rv32_block_get_phase_cycles(&phases);
   engine->total_cycles = phases.total;
   engine->lookup_cycles = phases.lookup;
   engine->block_cycles = phases.block;
   engine->scalar_cycles = phases.scalar;
   struct gpsp_rv32_compile_stats compile;
   gpsp_rv32_block_get_compile_stats(&compile);
   engine->translated_blocks = compile.blocks;
   engine->translated_guest_ops = compile.guest_ops;
   engine->translated_code_bytes = compile.code_bytes;
   engine->flow_blocks = compile.flow_blocks;
   engine->flow_arm_blocks = compile.flow_arm_blocks;
   engine->flow_thumb_blocks = compile.flow_thumb_blocks;
   engine->flow_ram_blocks = compile.flow_ram_blocks;
   engine->flow_legacy_blocks = compile.flow_legacy_blocks;
   engine->flow_guest_ops = compile.flow_guest_ops;
   engine->flow_helper_ops = compile.flow_helper_ops;
   engine->flow_regions = compile.flow_regions;
   engine->flow_entries = compile.flow_entries;
   engine->flow_code_bytes = compile.flow_code_bytes;
   engine->bundled_guest_ops = compile.bundled_guest_ops;
   engine->flushed_code_bytes = compile.flushed_bytes;
   engine->max_guest_ops = compile.max_guest_ops;
   engine->max_thumb_guest_ops = compile.max_thumb_guest_ops;
   engine->max_block_bytes = compile.max_block_bytes;
   engine->max_flush_bytes = compile.max_flush_bytes;
   memcpy(engine->thumb_span_count, compile.span_count,
         sizeof(engine->thumb_span_count));
   struct gpsp_rv32_site_stats sites;
   gpsp_rv32_block_get_site_stats(&sites);
   engine->jit_sites.arm_ops = sites.arm_ops;
   engine->jit_sites.thumb_ops = sites.thumb_ops;
   engine->jit_sites.thumb_loads = sites.thumb_loads;
   engine->jit_sites.thumb_stores = sites.thumb_stores;
   engine->jit_sites.thumb_literals = sites.thumb_literals;
   engine->jit_sites.thumb_nonmem_fallbacks = sites.thumb_nonmem_fallbacks;
   engine->jit_sites.thumb_push = sites.thumb_push;
   engine->jit_sites.thumb_pop = sites.thumb_pop;
   engine->jit_sites.thumb_stmia = sites.thumb_stmia;
   engine->jit_sites.thumb_ldmia = sites.thumb_ldmia;
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
   struct gpsp_rv32_rom_byte_stats code_bytes;
   gpsp_rv32_block_get_rom_byte_stats(&code_bytes);
   struct gpsp_rv32_overlap_byte_stats overlap;
   gpsp_rv32_block_get_overlap_byte_stats(&overlap);
   _Static_assert(sizeof(engine->jit_code_bytes) == sizeof(code_bytes),
         "code-byte benchmark layout differs");
   _Static_assert(sizeof(engine->jit_overlap_bytes) == sizeof(overlap),
         "overlap benchmark layout differs");
   memcpy(&engine->jit_code_bytes, &code_bytes, sizeof(code_bytes));
   memcpy(&engine->jit_overlap_bytes, &overlap, sizeof(overlap));
#endif
   struct gpsp_rv32_interpreter_stats interpreter;
   gpsp_rv32_block_get_interpreter_stats(&interpreter);
   engine->generated_steps = interpreter.generated_steps;
   engine->generated_arm = interpreter.generated_arm;
   engine->generated_thumb = interpreter.generated_thumb;
   engine->direct_steps = interpreter.direct_steps;
   engine->bulk_runs = interpreter.bulk_runs;
   engine->bulk_steps = interpreter.bulk_steps;
   engine->generated_region.bios = interpreter.generated_bios;
   engine->generated_region.ewram = interpreter.generated_ewram;
   engine->generated_region.iwram = interpreter.generated_iwram;
   engine->generated_region.rom = interpreter.generated_rom;
   engine->generated_region.other = interpreter.generated_other;
   engine->bulk_region.bios = interpreter.bulk_bios;
   engine->bulk_region.ewram = interpreter.bulk_ewram;
   engine->bulk_region.iwram = interpreter.bulk_iwram;
   engine->bulk_region.rom = interpreter.bulk_rom;
   engine->bulk_region.other = interpreter.bulk_other;
   struct gpsp_rv32_block_region_stats blocks;
   gpsp_rv32_block_get_region_stats(&blocks);
   engine->block_region.bios = blocks.bios;
   engine->block_region.ewram = blocks.ewram;
   engine->block_region.iwram = blocks.iwram;
   engine->block_region.rom = blocks.rom;
   engine->block_region.other = blocks.other;
#if defined(GPSP_BENCHMARK_JIT_LOOKUP)
   struct gpsp_rv32_lookup_stats lookup;
   gpsp_rv32_block_get_lookup_stats(&lookup);
   _Static_assert(sizeof(engine->jit_lookup) == sizeof(lookup),
         "lookup benchmark layout differs");
   memcpy(&engine->jit_lookup, &lookup, sizeof(lookup));
#endif
#if defined(GPSP_BENCHMARK_JIT_DETAIL)
   struct gpsp_rv32_jit_detail_stats detail;
   gpsp_rv32_block_get_jit_detail(&detail);
   engine->jit_detail.code_cycles = detail.code_cycles;
   engine->jit_detail.helper_cycles = detail.helper_cycles;
   engine->jit_detail.helper_calls = detail.helper_calls;
   engine->jit_detail.ram_load_fast = detail.ram_load_fast;
   engine->jit_detail.ram_load_slow = detail.ram_load_slow;
   engine->jit_detail.ram_store_fast = detail.ram_store_fast;
   engine->jit_detail.ram_store_slow = detail.ram_store_slow;
   engine->jit_detail.multi_fast = detail.multi_fast;
   engine->jit_detail.multi_slow = detail.multi_slow;
   engine->jit_detail.memory_cycles = detail.memory_cycles;
   engine->jit_detail.memory_calls = detail.memory_calls;
   engine->jit_detail.memory_rom_loads = detail.memory_rom_loads;
   engine->jit_detail.memory_ram_loads = detail.memory_ram_loads;
   engine->jit_detail.memory_io_loads = detail.memory_io_loads;
   engine->jit_detail.memory_other_loads = detail.memory_other_loads;
   engine->jit_detail.memory_stores = detail.memory_stores;
   engine->jit_detail.arm_memory_calls = detail.arm_memory_calls;
   engine->jit_detail.arm_block_calls = detail.arm_block_calls;
   engine->jit_detail.arm_memory_cycles = detail.arm_memory_cycles;
   engine->jit_detail.arm_block_cycles = detail.arm_block_cycles;
   engine->jit_detail.arm_memory_ram = detail.arm_memory_ram;
   engine->jit_detail.arm_memory_io = detail.arm_memory_io;
   engine->jit_detail.arm_memory_video = detail.arm_memory_video;
   engine->jit_detail.arm_memory_rom = detail.arm_memory_rom;
   engine->jit_detail.arm_memory_save = detail.arm_memory_save;
   engine->jit_detail.arm_memory_other = detail.arm_memory_other;
   engine->jit_detail.arm_memory_stores = detail.arm_memory_stores;
   engine->jit_detail.system_calls = detail.system_calls;
   engine->jit_detail.rom_read_body_cycles = detail.rom_read_body_cycles;
   engine->jit_detail.rom_read_body_calls = detail.rom_read_body_calls;
   engine->jit_detail.rom_read_page_misses = detail.rom_read_page_misses;
   engine->jit_detail.rom_read_bridge_cycles = detail.rom_read_bridge_cycles;
   engine->jit_detail.rom_read_bridge_calls = detail.rom_read_bridge_calls;
   engine->jit_detail.block_cycles.bios = detail.block_cycles.bios;
   engine->jit_detail.block_cycles.ewram = detail.block_cycles.ewram;
   engine->jit_detail.block_cycles.iwram = detail.block_cycles.iwram;
   engine->jit_detail.block_cycles.rom = detail.block_cycles.rom;
   engine->jit_detail.block_cycles.other = detail.block_cycles.other;
   engine->jit_detail.block_ready_exits = detail.block_ready_exits;
   engine->jit_detail.block_budget_exits = detail.block_budget_exits;
   engine->jit_detail.block_halt_exits = detail.block_halt_exits;
   engine->jit_detail.block_irq_alerts = detail.block_irq_alerts;
   engine->jit_detail.block_smc_alerts = detail.block_smc_alerts;
   engine->jit_detail.resume_candidates = detail.resume_candidates;
   engine->jit_detail.resume_attempts = detail.resume_attempts;
   engine->jit_detail.resume_successes = detail.resume_successes;
   engine->jit_detail.resume_rejections = detail.resume_rejections;
   struct gpsp_rv32_arm_video_memory_stats video_memory;
   gpsp_rv32_arm_video_memory_get_stats(&video_memory);
   memcpy(engine->arm_video_memory.calls, video_memory.calls,
         sizeof(video_memory.calls));
   memcpy(engine->arm_video_memory.cycles, video_memory.cycles,
         sizeof(video_memory.cycles));
   struct gpsp_rv32_arm_video_source_stats video_source;
   gpsp_rv32_arm_video_source_get_stats(&video_source);
   _Static_assert(sizeof(engine->arm_video_source) == sizeof(video_source),
         "video source benchmark layout differs");
   memcpy(&engine->arm_video_source, &video_source, sizeof(video_source));
#endif
   struct gpsp_rv32_fallback_histogram fallback;
   gpsp_rv32_block_get_fallback_histogram(&fallback);
   for (unsigned i = 0; i < 8; ++i)
   {
      engine->fallback_thumb_top[i].opcode_high_byte =
            fallback.thumb[i].opcode_high_byte;
      engine->fallback_thumb_top[i].steps = fallback.thumb[i].steps;
      engine->fallback_arm_top[i].opcode_high_byte =
            fallback.arm[i].opcode_high_byte;
      engine->fallback_arm_top[i].steps = fallback.arm[i].steps;
   }
#endif
}

void gpsp_benchmark_take_hot_sites(
      struct gpsp_benchmark_jit_hot_sites *sites)
{
   memset(sites, 0, sizeof(*sites));
#if defined(GPSP_BENCHMARK_JIT_DETAIL) && defined(GPSP_RV32_JIT)
   struct gpsp_rv32_hot_sites native;
   gpsp_rv32_block_take_hot_sites(&native);
   _Static_assert(sizeof(*sites) == sizeof(native),
         "hot-site benchmark layout differs");
   memcpy(sites, &native, sizeof(native));
#endif
}

void gpsp_benchmark_get_lookup_totals(
      struct gpsp_benchmark_jit_lookup_frame *totals)
{
   memset(totals, 0, sizeof(*totals));
#if defined(GPSP_BENCHMARK_JIT_LOOKUP) && defined(GPSP_RV32_JIT)
   struct gpsp_rv32_lookup_stats lookup;
   gpsp_rv32_block_get_lookup_stats(&lookup);
   totals->hit_calls = lookup.top_hit_calls;
   totals->miss_calls = lookup.top_miss_calls;
   totals->hit_cycles = lookup.top_hit_cycles;
   totals->miss_cycles = lookup.top_miss_cycles;
#endif
}

void gpsp_benchmark_reset_bios_probe(void)
{
#if defined(GPSP_BENCHMARK_JIT_DETAIL) && defined(GPSP_RV32_JIT)
   gpsp_rv32_bios_probe_reset();
#endif
}

void gpsp_benchmark_get_bios_probe(struct gpsp_benchmark_bios_probe *probe)
{
   memset(probe, 0, sizeof(*probe));
#if defined(GPSP_BENCHMARK_JIT_DETAIL) && defined(GPSP_RV32_JIT)
   struct gpsp_rv32_bios_probe_stats stats;
   gpsp_rv32_bios_probe_get_stats(&stats);
   _Static_assert(sizeof(*probe) == sizeof(stats),
         "BIOS benchmark layout differs");
   memcpy(probe, &stats, sizeof(stats));
#endif
}
#endif

#if defined(VITA) && defined(HAVE_DYNAREC)
#include <psp2/kernel/sysmem.h>
static int translation_caches_inited = 0;
static inline int align(int x, int n) {
  return (((x >> n) + 1) << n );
}

#define FOUR_KB_ALIGN(x) align(x, 12)
#define MB_ALIGN(x) align(x, 20)

int _newlib_vm_size_user = ROM_TRANSLATION_CACHE_SIZE +
                           RAM_TRANSLATION_CACHE_SIZE; 

int getVMBlock();

#endif

#if defined(_3DS)
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#if defined(HAVE_DYNAREC)
#include <malloc.h>
#include "3ds/3ds_utils.h"
#define MEMOP_PROT   6
#define MEMOP_MAP 4
#define MEMOP_UNMAP 5
int32_t svcDuplicateHandle(uint32_t* out, uint32_t original);
int32_t svcCloseHandle(uint32_t handle);
int32_t svcControlProcessMemory(uint32_t process, void* addr0, void* addr1, uint32_t size, uint32_t type, uint32_t perm);
static int translation_caches_inited = 0;
#endif
#endif

#ifndef MAX_PATH
#define MAX_PATH (512)
#endif

// Usually 59.72750057 Hz, unless GBC_RATE is overclocked (for 60FPS)
#define GBA_FPS ((float) GBC_BASE_RATE) / (308 * 228 * 4)

static s16 *audio_sample_buffer        = NULL;
static float audio_samples_per_frame   = 0.0f;
static float audio_samples_accumulator = 0.0f;

/* Apply the gpsp_sound_rate core option. reinit_env is non-NULL only for
 * mid-session changes (retro_run variable check), where issuing
 * RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO is legal; at load time the new rate
 * is picked up by the normal av_info and buffer init paths. */
static void gpsp_apply_sound_rate(const char *value, retro_environment_t reinit_env)
{
   u32 new_bits = (value && !strcmp(value, "32768")) ? 15 : 16;

   if (new_bits == sound_freq_bits)
      return;

   sound_freq_bits = new_bits;
   sound_frequency = 1u << new_bits;
   /* init_sound() ran at the old rate before the game's options load. */
   sound_update_tick_step();
   audio_samples_per_frame   = (float)sound_frequency / (float)(GBA_FPS);
   audio_samples_accumulator = 0.0f;

   if (reinit_env)
   {
      struct retro_system_av_info av_info;
      render_gbc_sound();          /* drain pending ticks at the old rate */
      sound_frequency_changed();   /* recompute all derived steps */
      sound_flush_ring();
      retro_get_system_av_info(&av_info);
      reinit_env(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
   }
}

/* Workaround for a RetroArch audio driver
 * limitation: a maximum of 1024 frames
 * can be written per call of audio_batch_cb() */
#define AUDIO_BATCH_FRAMES_MAX 1024

/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 30

u32 skip_next_frame                          = 0;
static frameskip_type current_frameskip_type = no_frameskip;
static u32 frameskip_threshold               = 0;
static u32 frameskip_interval                = 0;
static u32 frameskip_counter                 = 0;
static bool audio_buff_active                = false;
static unsigned audio_buff_occupancy         = 0;
static bool audio_buff_underrun              = false;
static unsigned audio_latency                = 0;
static bool update_audio_latency             = false;
static bios_type selected_bios               = auto_detect;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_environment_t environ_cb;
static retro_set_rumble_state_t rumble_cb;

struct retro_perf_callback perf_cb;

int dynarec_enable;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;

static int rtc_mode = FEAT_AUTODETECT;
static int rumble_mode = FEAT_AUTODETECT;
static int serial_setting = SERIAL_MODE_AUTO;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static u16 *gba_screen_pixels_prev = NULL;
static u16 *gba_processed_pixels   = NULL;

static void (*video_post_process)(void) = NULL;
static bool post_process_cc  = false;
static bool post_process_mix = false;

static void error_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[gpSP]: %s\n", text);
}

static void info_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[gpSP]: %s\n", text);
}

static void show_warning_message(const char* text, unsigned durationms) {
  unsigned ifversion = 0;
  if (!environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &ifversion) || ifversion >= 1) {
    /* Use the new API to display messages */
    struct retro_message_ext msg = {
      .msg = text, .duration = durationms,
      .priority = 2, .level = RETRO_LOG_WARN,
      .target = RETRO_MESSAGE_TARGET_ALL,
      .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
      .progress = -1,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
  }
  else {
    struct retro_message msg = {.msg = text, .frames = durationms / 17};
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
  }
}

/* Frameskip START */

static void audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   audio_buff_active    = active;
   audio_buff_occupancy = occupancy;
   audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (current_frameskip_type == no_frameskip)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }
   else
   {
      bool calculate_audio_latency = true;

      if (current_frameskip_type == fixed_interval_frameskip)
         environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      else
      {
         struct retro_audio_buffer_status_callback buff_status_cb;
         buff_status_cb.callback = audio_buff_status_cb;

         if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &buff_status_cb))
         {
            error_msg("Frameskip disabled - frontend does not support audio buffer status monitoring");

            audio_buff_active       = false;
            audio_buff_occupancy    = 0;
            audio_buff_underrun     = false;
            audio_latency           = 0;
            calculate_audio_latency = false;
         }
      }

      if (calculate_audio_latency)
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / ((float) GBA_FPS);

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }

   }

   update_audio_latency = true;
   frameskip_counter    = 0;
}

/* Frameskip END */

/* Video post processing START */

/* Note: This code is intentionally W.E.T.
 * (Write Everything Twice). These functions
 * are performance critical, and we cannot
 * afford to do unnecessary comparisons/switches
 * inside the inner for loops */

static void video_post_process_cc(void)
{
   const uint16_t *src = gba_screen_pixels;
   uint16_t       *dst = gba_processed_pixels;
   const size_t    npx = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t src_color = src[i];

      /* Convert colour to RGB555 and perform lookup */
      dst[i] = gba_cc_lut[((src_color & 0xFFC0) >> 1) | (src_color & 0x1F)];
   }
}

static void video_post_process_mix(void)
{
   /* Flat loop: pitch equals width so the row-stride bookkeeping in
    * the original two-level form was bookkeeping for no actual stride
    * mismatch.  Compilers can vectorize the flat loop more aggressively;
    * on ARM Cortex-A it makes a measurable difference at -O2.  After the
    * mix we used to memcpy(prev, curr, 75 KB) so the next frame's mix
    * could read the previous frame; that copy is now elided by swapping
    * the two buffer pointers - after the swap, gba_screen_pixels points
    * to what was the prev buffer (about to be overwritten by the next
    * scanline-render pass), and gba_screen_pixels_prev points at the
    * just-rendered frame.  Saves ~4.5 MB/s of memory bandwidth at 60 fps
    * with mix enabled. */
   const uint16_t *src_curr = gba_screen_pixels;
   const uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t       *dst      = gba_processed_pixels;
   const size_t    npx      = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t rgb_curr = src_curr[i];
      uint16_t rgb_prev = src_prev[i];

      /* Mix colours:
       *   http://blargg.8bitalley.com/info/rgb_mixing.html */
      dst[i] = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;
   }

   /* Swap curr/prev: the just-rendered frame becomes prev for next time,
    * and the previous prev (now stale and about to be overwritten by the
    * next render pass) becomes the new render target.  Equivalent to the
    * old 'memcpy(prev, curr, GBA_SCREEN_BUFFER_SIZE)' but without the
    * copy.  Safe because the scanline renderer fully overwrites every
    * pixel of gba_screen_pixels before any read (fill_line_background
    * runs first per scanline) - no read-before-write hazard on the
    * post-swap buffer. */
   {
      u16 *t = gba_screen_pixels;
      gba_screen_pixels = gba_screen_pixels_prev;
      gba_screen_pixels_prev = t;
   }
}

static void video_post_process_cc_mix(void)
{
   /* See video_post_process_mix for the loop-flatten + buffer-swap
    * rationale.  Same shape, plus the gba_cc_lut colour-correction
    * lookup on the mixed value. */
   const uint16_t *src_curr = gba_screen_pixels;
   const uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t       *dst      = gba_processed_pixels;
   const size_t    npx      = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t rgb_curr = src_curr[i];
      uint16_t rgb_prev = src_prev[i];

      uint16_t rgb_mix  = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;

      /* Convert colour to RGB555 and perform lookup */
      dst[i] = gba_cc_lut[((rgb_mix & 0xFFC0) >> 1) | (rgb_mix & 0x1F)];
   }

   {
      u16 *t = gba_screen_pixels;
      gba_screen_pixels = gba_screen_pixels_prev;
      gba_screen_pixels_prev = t;
   }
}

static void init_post_processing(void)
{
   video_post_process = NULL;

   /* If post processing is disabled, return
    * immediately */
   if (!post_process_cc && !post_process_mix)
      return;

   /* Initialise output buffer, if required */
   if (!gba_processed_pixels &&
       (post_process_cc || post_process_mix))
   {
#ifdef _3DS
      gba_processed_pixels = (u16*)linearMemAlign(GBA_SCREEN_BUFFER_SIZE, 128);
#else
      gba_processed_pixels = (u16*)malloc(GBA_SCREEN_BUFFER_SIZE);
#endif

      if (!gba_processed_pixels)
         return;

      memset(gba_processed_pixels, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);
   }

   /* Initialise 'history' buffer, if required.  If this alloc fails we
    * must NOT fall through to the mix-variant assignment below: those
    * functions dereference gba_screen_pixels_prev unconditionally.
    * Leave the user setting (post_process_mix) intact - the failure
    * may be transient - but skip the function-pointer assignment for
    * the mix path so video_run keeps working. */
   if (!gba_screen_pixels_prev &&
       post_process_mix)
   {
      gba_screen_pixels_prev = (u16*)malloc(GBA_SCREEN_BUFFER_SIZE);

      if (!gba_screen_pixels_prev && log_cb)
         log_cb(RETRO_LOG_WARN,
                "Failed to allocate frame history buffer; disabling mix component of post-processing this run\n");
      else if (gba_screen_pixels_prev)
         memset(gba_screen_pixels_prev, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);
   }

   /* Assign post processing function.  Treat post_process_mix as
    * effectively false if the history buffer is missing. */
   {
      bool mix_active = post_process_mix && (gba_screen_pixels_prev != NULL);

      if (post_process_cc && mix_active)
         video_post_process = video_post_process_cc_mix;
      else if (post_process_cc)
         video_post_process = video_post_process_cc;
      else if (mix_active)
         video_post_process = video_post_process_mix;
   }
}

/* Video post processing END */

/* Fast forward override */
void set_fastforward_override(bool fastforward)
{
   struct retro_fastforwarding_override ff_override;

   if (!libretro_supports_ff_override)
      return;

   ff_override.ratio        = -1.0f;
   ff_override.notification = true;

   if (fastforward)
   {
      ff_override.fastforward    = true;
      ff_override.inhibit_toggle = true;
   }
   else
   {
      ff_override.fastforward    = false;
      ff_override.inhibit_toggle = false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);
}

static void audio_run(void)
{
   s16 *audio_buffer_ptr;
   u32 samples_to_read;
   u32 samples_produced;

   /* If retro_init's malloc failed we still get called every frame;
    * skip the work in that case rather than write through NULL. */
   if (!audio_sample_buffer)
      return;

   /* audio_samples_per_frame is decimal;
    * get integer component */
   samples_to_read = (u32)audio_samples_per_frame;

   /* Account for fractional component */
   audio_samples_accumulator += audio_samples_per_frame -
         (float)samples_to_read;

   if (audio_samples_accumulator >= 1.0f)
   {
      samples_to_read++;
      audio_samples_accumulator -= 1.0f;
   }

   samples_produced = sound_read_samples(audio_sample_buffer, samples_to_read);

   /* Workaround for a RetroArch audio driver
    * limitation: a maximum of 1024 frames
    * can be written per call of audio_batch_cb(),
    * so we have to send samples in chunks */
   audio_buffer_ptr = audio_sample_buffer;
   while (samples_produced > 0)
   {
      u32 samples_to_write = (samples_produced > AUDIO_BATCH_FRAMES_MAX) ?
            AUDIO_BATCH_FRAMES_MAX : samples_produced;

      audio_batch_cb(audio_buffer_ptr, samples_to_write);

      samples_produced -= samples_to_write;
      audio_buffer_ptr += samples_to_write << 1;
   }
}

static void video_run(void)
{
   u16 *gba_screen_pixels_buf = gba_screen_pixels;

   if (skip_next_frame)
   {
      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
      return;
   }

   if (video_post_process)
   {
      video_post_process();
      gba_screen_pixels_buf = gba_processed_pixels;
   }

   video_cb(gba_screen_pixels_buf, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
}

// Netplay (Netpacket) interface

u32 netplay_num_clients = 0, netplay_client_id = 0;
static retro_netpacket_send_t netpacket_send_fn_ptr = NULL;
static retro_netpacket_poll_receive_t netpacket_pollrcv_fn_ptr = NULL;

void netpacket_poll_receive() {
  if (netpacket_pollrcv_fn_ptr)
    netpacket_pollrcv_fn_ptr();
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len) {
  // Force all packets to be flushed ASAP, to minimize latency.
  if (netpacket_send_fn_ptr)
    netpacket_send_fn_ptr(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len, client_id);
}

static void netpacket_start(uint16_t client_id, retro_netpacket_send_t send_fn, retro_netpacket_poll_receive_t poll_receive_fn) {
  netpacket_send_fn_ptr = send_fn;
  netpacket_pollrcv_fn_ptr = poll_receive_fn;
  netplay_num_clients = 0;
  netplay_client_id = client_id;
}

// Netplay session ends.
static void netpacket_stop() {
  netpacket_send_fn_ptr = NULL;
  netpacket_pollrcv_fn_ptr = NULL;
}

static void netpacket_receive(const void* buf, size_t len, uint16_t client_id) {
  switch (serial_mode) {
  case SERIAL_MODE_RFU:
    rfu_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_POKE:
    serialpoke_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_AW1:
  case SERIAL_MODE_SERIAL_AW2:
    serialaw_net_receive(buf, len, client_id);
    break;
  };
}

// Ensure we do not have too many clients for the type of connection used.
static bool netpacket_connected(uint16_t client_id) {
  const uint8_t maxpl[] = {
     0,                        // SERIAL_MODE_DISABLED
     0,                        // SERIAL_MODE_GBP
     MAX_RFU_NETPLAYERS,       // SERIAL_MODE_RFU
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_POKE
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_AW1
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_AW2
     0,                        // SERIAL_MODE_AUTO
  };
  const u32 max_clients = maxpl[serial_mode] - 1U;

  if (netplay_num_clients >= max_clients)
    return false;

  netplay_num_clients++;
  return true;
}

static void netpacket_disconnected(uint16_t client_id) {
  netplay_num_clients--;
}

const struct retro_netpacket_callback netpacket_iface = {
  netpacket_start,          /* start */
  netpacket_receive,        /* receive */
  netpacket_stop,           /* stop */
  NULL,                     /* poll */
  netpacket_connected,      /* connected */
  netpacket_disconnected,   /* disconnected */
  GPSP_NETPACKET_VERSION,   /* core version char* */
};


#ifdef PERF_TEST

extern struct retro_perf_callback perf_cb;

#define RETRO_PERFORMANCE_INIT(X) \
   static struct retro_perf_counter X = {#X}; \
   do { \
      if (!(X).registered) \
         perf_cb.perf_register(&(X)); \
   } while(0)

#define RETRO_PERFORMANCE_START(X) perf_cb.perf_start(&(X))
#define RETRO_PERFORMANCE_STOP(X) perf_cb.perf_stop(&(X))
#else
#define RETRO_PERFORMANCE_INIT(X)
#define RETRO_PERFORMANCE_START(X)
#define RETRO_PERFORMANCE_STOP(X)

#endif

void retro_get_system_info(struct retro_system_info* info)
{
   info->library_name = GPSP_NAME;
 #ifdef GIT_VERSION
   info->library_version = GPSP_VERSION "-" GIT_VERSION;
#else
   info->library_version = GPSP_VERSION;
#endif
   info->need_fullpath = true;
   info->block_extract = false;
   info->valid_extensions = "gba|bin|agb|gbz|u1" ;
}


void retro_get_system_av_info(struct retro_system_av_info* info)
{
   info->geometry.base_width = GBA_SCREEN_WIDTH;
   info->geometry.base_height = GBA_SCREEN_HEIGHT;
   info->geometry.max_width = GBA_SCREEN_WIDTH;
   info->geometry.max_height = GBA_SCREEN_HEIGHT;
   info->geometry.aspect_ratio = 3.0f / 2.0f;
   info->timing.fps = ((float) GBA_FPS);
   info->timing.sample_rate = sound_frequency;
}

void retro_init(void)
{
   u32 audio_sample_buffer_size;
   audio_samples_per_frame   = (float)sound_frequency / (float)(GBA_FPS);
   audio_samples_accumulator = 0.0f;
   audio_sample_buffer_size  = (((u32)((float)(GBA_SOUND_FREQUENCY) / (float)(GBA_FPS))) + 1) * 2;
   audio_sample_buffer       = (s16*)malloc(audio_sample_buffer_size * sizeof(s16));
   if (!audio_sample_buffer)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR,
                "Failed to allocate audio sample buffer (%u bytes)\n",
                (unsigned)(audio_sample_buffer_size * sizeof(s16)));
      /* Continue: audio_run() now guards against this NULL and the
       * frontend just runs silent.  Better than failing retro_init
       * outright on memory-constrained targets where a small audio
       * allocation may fail but the rest of the core still works. */
   }

#if defined(HAVE_DYNAREC)
  #if defined(MMAP_JIT_CACHE)
   rom_translation_cache = map_jit_block(ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
   if (rom_translation_cache)
      ram_translation_cache = &rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
   else if (log_cb)
      /* No usable block within branch range of the text segment (or
       * RWX denied, e.g. hardened runtimes without a JIT entitlement).
       * check_variables pins dynarec_enable to 0 in this case and the
       * core runs on the interpreter. */
      log_cb(RETRO_LOG_ERROR,
             "Failed to map the JIT translation cache; "
             "falling back to the interpreter\n");
#elif defined(_3DS)
   if (__ctr_svchax && !translation_caches_inited)
   {
      uint32_t currentHandle;
      check_rosalina();
      
      rom_translation_cache_ptr  = memalign(0x1000, ROM_TRANSLATION_CACHE_SIZE);
      ram_translation_cache_ptr  = memalign(0x1000, RAM_TRANSLATION_CACHE_SIZE);

      svcDuplicateHandle(&currentHandle, 0xFFFF8001);
      svcControlProcessMemory(currentHandle,
                              rom_translation_cache, rom_translation_cache_ptr,
                              ROM_TRANSLATION_CACHE_SIZE, MEMOP_MAP, 0b111);
      svcControlProcessMemory(currentHandle,
                              ram_translation_cache, ram_translation_cache_ptr,
                              RAM_TRANSLATION_CACHE_SIZE, MEMOP_MAP, 0b111);
      svcCloseHandle(currentHandle);
      rom_translation_ptr = rom_translation_cache;
      ram_translation_ptr = ram_translation_cache;
      ctr_flush_invalidate_cache();
      translation_caches_inited = 1;
   }
#elif defined(VITA)
   if(!translation_caches_inited){
      void* currentHandle;

      sceBlock = getVMBlock();
      
      if (sceBlock < 0)
      {
        return;
      }

      // get base address
      int ret = sceKernelGetMemBlockBase(sceBlock, &currentHandle);
      if (ret < 0)
      {
        return;
      }

      rom_translation_cache  = (u8*)currentHandle;
      ram_translation_cache  = rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE;
      rom_translation_ptr = rom_translation_cache;
      ram_translation_ptr = ram_translation_cache;
      sceKernelOpenVMDomain();
      translation_caches_inited = 1;
    }
#endif
#endif

   init_gamepak_buffer();
   init_sound();

   if(!gba_screen_pixels)
#ifdef _3DS
      gba_screen_pixels = (uint16_t*)linearMemAlign(GBA_SCREEN_BUFFER_SIZE, 128);
#else
      gba_screen_pixels = (uint16_t*)malloc(GBA_SCREEN_BUFFER_SIZE);
#endif

   /* gba_screen_pixels is dereferenced unconditionally by both the
    * renderer (writes from update_scanline) and the post-process /
    * video_cb hand-off in video_run.  A NULL here means we cannot
    * safely run a single frame.  Log loudly; load_game will return
    * the failure to the frontend so the core unloads cleanly. */
   if (!gba_screen_pixels && log_cb)
      log_cb(RETRO_LOG_ERROR,
             "Failed to allocate frame buffer (%u bytes); core will refuse to load content\n",
             (unsigned)GBA_SCREEN_BUFFER_SIZE);

   libretro_supports_bitmasks = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   libretro_supports_ff_override = false;
   if (environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, NULL))
      libretro_supports_ff_override = true;

   // This interface is actually optional
   environ_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&netpacket_iface);

   current_frameskip_type = no_frameskip;
   frameskip_threshold    = 0;
   frameskip_interval     = 0;
   frameskip_counter      = 0;
   audio_buff_active      = false;
   audio_buff_occupancy   = 0;
   audio_buff_underrun    = false;
   audio_latency          = 0;
   update_audio_latency   = false;
   selected_bios          = auto_detect;
   selected_boot_mode     = boot_game;
}

void retro_deinit(void)
{
#if defined(GPSP_RV32_JIT)
   gpsp_rv32_block_deinit();
#endif
   perf_cb.perf_log();
   memory_term();

#if defined(MMAP_JIT_CACHE) && defined(HAVE_DYNAREC)
   if (rom_translation_cache)
      unmap_jit_block(rom_translation_cache, ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
#endif
#if defined(_3DS) && defined(HAVE_DYNAREC)

   if (__ctr_svchax && translation_caches_inited)
   {
      uint32_t currentHandle;
      svcDuplicateHandle(&currentHandle, 0xFFFF8001);
      svcControlProcessMemory(currentHandle,
                              rom_translation_cache, rom_translation_cache_ptr,
                              ROM_TRANSLATION_CACHE_SIZE, MEMOP_UNMAP, 0b111);
      svcControlProcessMemory(currentHandle,
                              ram_translation_cache, ram_translation_cache_ptr,
                              RAM_TRANSLATION_CACHE_SIZE, MEMOP_UNMAP, 0b111);
      svcCloseHandle(currentHandle);
      free(rom_translation_cache_ptr);
      free(ram_translation_cache_ptr);
      translation_caches_inited = 0;
   }
#endif

#if defined(VITA) && defined(HAVE_DYNAREC)
    if(translation_caches_inited){
        translation_caches_inited = 0;
    }
#endif

#ifdef _3DS
   linearFree(gba_screen_pixels);
   if (gba_processed_pixels)
      linearFree(gba_processed_pixels);
#else
   free(gba_screen_pixels);
   if (gba_processed_pixels)
      free(gba_processed_pixels);
#endif
   if (gba_screen_pixels_prev)
      free(gba_screen_pixels_prev);

   gba_screen_pixels      = NULL;
   gba_processed_pixels   = NULL;
   gba_screen_pixels_prev = NULL;
   video_post_process     = NULL;
   post_process_cc        = false;
   post_process_mix       = false;

   if (audio_sample_buffer)
      free(audio_sample_buffer);

   audio_sample_buffer       = NULL;
   audio_samples_per_frame   = 0.0f;
   audio_samples_accumulator = 0.0f;
}

static retro_time_t retro_perf_dummy_get_time_usec() { return 0; }
static retro_perf_tick_t retro_perf_dummy_get_counter() { return 0; }
static uint64_t retro_perf_dummy_get_cpu_features() { return 0; }
static void retro_perf_dummy_log() {}
static void retro_perf_dummy_counter(struct retro_perf_counter *counter) {};

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   struct retro_vfs_interface_info vfs_iface_info;

   environ_cb = cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   perf_cb = (struct retro_perf_callback){
      retro_perf_dummy_get_time_usec,
      retro_perf_dummy_get_counter,
      retro_perf_dummy_get_cpu_features,
      retro_perf_dummy_counter,
      retro_perf_dummy_counter,
      retro_perf_dummy_counter,
      retro_perf_dummy_log,
   };
   environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);

   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   libretro_set_core_options(environ_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) { }

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset(void)
{
   reset_gba();
}

size_t retro_serialize_size(void)
{
   return GBA_STATE_MEM_SIZE;
}

bool retro_serialize(void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   memset (data,0, GBA_STATE_MEM_SIZE);
   gba_save_state(data);

   return true;
}

bool retro_unserialize(const void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   return gba_load_state(data);
}

void retro_cheat_reset(void)
{
   cheat_clear();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
   if (!enabled)
   {
      /* Per the libretro contract, retro_cheat_set with enabled=false
       * must deactivate the cheat at this index.  Without this, a user
       * toggling a cheat off in the frontend would have no effect:
       * the cheat would stay active until the next retro_cheat_reset()
       * or content load.  Note RetroArch typically calls
       * retro_cheat_reset followed by a full reapply on every change,
       * so the visible bug surface is small, but other frontends and
       * runtime API callers can hit this directly. */
      cheat_disable(index);
      return;
   }

   switch (cheat_parse(index, code))
   {
   case CheatErrorTooMany:
      show_warning_message("Too many active cheats!", 2500);
      break;
   case CheatErrorTooBig:
      show_warning_message("Cheats are too big!", 2500);
      break;
   case CheatErrorEncrypted:
      show_warning_message("Encrypted cheats are not supported!", 2500);
      break;
   case CheatErrorNotSupported:
      show_warning_message("Cheat type is not supported!", 2500);
      break;
   case CheatNoError:
      break;
   };
}

static void extract_directory(char* buf, const char* path, size_t size)
{
   char* base;

   strlcpy(buf, path, size);

   base = strrchr(buf, '/');

   if (base)
      *base = '\0';
   else
      strlcpy(buf, ".", size);
}

static void check_variables(bool started_from_load)
{
   struct retro_variable var;
   frameskip_type frameskip_type_prev;
   bool post_process_cc_prev;
   bool post_process_mix_prev;

#ifdef HAVE_DYNAREC
   var.key = "gpsp_drc";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int prevvalue = dynarec_enable;
      if (strcmp(var.value, "disabled") == 0)
         dynarec_enable = 0;
      else if (strcmp(var.value, "enabled") == 0)
         dynarec_enable = 1;

      /* Flush dynarec cache to ensure we do not execute old code */
      if (dynarec_enable != prevvalue)
         flush_dynarec_caches();
   }
   else
      dynarec_enable = 1;

#if defined(MMAP_JIT_CACHE)
   /* Without a translation cache the dynarec cannot run; pin the
    * interpreter regardless of the core option. */
   if (!rom_translation_cache)
      dynarec_enable = 0;
#endif
#else
   dynarec_enable = 0;
#endif

   var.key = "gpsp_rtc_time_source";
   var.value = NULL;
   {
      rtc_time_source_type source = RTC_TIME_SOURCE_DETERMINISTIC;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
          !strcmp(var.value, "system"))
         source = RTC_TIME_SOURCE_SYSTEM;

      rtc_set_time_source(source);
   }

   if (started_from_load) {
     var.key                = "gpsp_bios";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "auto"))
           selected_bios = auto_detect;
        else if (!strcmp(var.value, "builtin"))
           selected_bios = builtin_bios;
        else if (!strcmp(var.value, "official"))
           selected_bios = official_bios;
     }

     var.key                = "gpsp_boot_mode";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "game"))
           selected_boot_mode = boot_game;
        else if (!strcmp(var.value, "bios"))
           selected_boot_mode = boot_bios;
     }

     var.key                = "gpsp_rtc";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           rtc_mode = FEAT_DISABLE;
        else if (!strcmp(var.value, "enabled"))
           rtc_mode = FEAT_ENABLE;
        else
           rtc_mode = FEAT_AUTODETECT;
     }

     var.key                = "gpsp_serial";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           serial_setting = SERIAL_MODE_DISABLED;
        else if (!strcmp(var.value, "rfu"))
           serial_setting = SERIAL_MODE_RFU;
        else if (!strcmp(var.value, "mul_poke"))
           serial_setting = SERIAL_MODE_SERIAL_POKE;
        else if (!strcmp(var.value, "mul_aw1"))
           serial_setting = SERIAL_MODE_SERIAL_AW1;
        else if (!strcmp(var.value, "mul_aw2"))
           serial_setting = SERIAL_MODE_SERIAL_AW2;
        else if (!strcmp(var.value, "gbp"))
           serial_setting = SERIAL_MODE_GBP;
        else
           serial_setting = SERIAL_MODE_AUTO;
     }

     var.key                = "gpsp_rumble";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           rumble_mode = FEAT_DISABLE;
        else if (!strcmp(var.value, "enabled"))
           rumble_mode = FEAT_ENABLE;
        else
           rumble_mode = FEAT_AUTODETECT;
     }
   }

   var.key                = "gpsp_sprlim";
   var.value              = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         sprite_limit = 1;
      else if (strcmp(var.value, "enabled") == 0)
         sprite_limit = 0;
   }
   {
      static int sound_rate_configured = 0;
      struct retro_variable svar = { "gpsp_sound_rate", NULL };
      environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &svar);
      gpsp_apply_sound_rate(svar.value, sound_rate_configured ? environ_cb : NULL);
      sound_rate_configured = 1;
   }

   var.key                = "gpsp_sound_headroom";
   var.value              = 0;
   sound_headroom_shift   = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
       !strcmp(var.value, "6"))
      sound_headroom_shift = 1;


   var.key                = "gpsp_frameskip";
   var.value              = 0;
   frameskip_type_prev    = current_frameskip_type;
   current_frameskip_type = no_frameskip;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
         current_frameskip_type = auto_frameskip;
      else if (!strcmp(var.value, "auto_threshold"))
         current_frameskip_type = auto_threshold_frameskip;
      else if (!strcmp(var.value, "fixed_interval"))
         current_frameskip_type = fixed_interval_frameskip;
   }

   var.key             = "gpsp_frameskip_threshold";
   var.value           = 0;
   frameskip_threshold = 33;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   var.key   = "gpsp_frameskip_interval";
   var.value = 0;

   frameskip_interval = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_interval = strtol(var.value, NULL, 10);

   /* (Re)Initialise frame skipping, if required */
   if (started_from_load ||
       (current_frameskip_type != frameskip_type_prev))
      init_frameskip();

   var.key              = "gpsp_color_correction";
   var.value            = NULL;
   post_process_cc_prev = post_process_cc;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         post_process_cc = false;
      else if (strcmp(var.value, "enabled") == 0)
         post_process_cc = true;
   }

   var.key               = "gpsp_frame_mixing";
   var.value             = NULL;
   post_process_mix_prev = post_process_mix;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         post_process_mix = false;
      else if (strcmp(var.value, "enabled") == 0)
         post_process_mix = true;
   }

   /* Check whether post processing options
    * have changed */
   if ((post_process_cc != post_process_cc_prev) ||
       (post_process_mix != post_process_mix_prev))
      init_post_processing();

   var.key           = "gpsp_turbo_period";
   var.value         = NULL;
   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      turbo_period = atoi(var.value);
      turbo_period = (turbo_period < TURBO_PERIOD_MIN) ?
            TURBO_PERIOD_MIN : turbo_period;
      turbo_period = (turbo_period > TURBO_PERIOD_MAX) ?
            TURBO_PERIOD_MAX : turbo_period;

      turbo_pulse_width = turbo_period >> 1;
      turbo_pulse_width = (turbo_pulse_width < TURBO_PULSE_WIDTH_MIN) ?
            TURBO_PULSE_WIDTH_MIN : turbo_pulse_width;
      turbo_pulse_width = (turbo_pulse_width > TURBO_PULSE_WIDTH_MAX) ?
            TURBO_PULSE_WIDTH_MAX : turbo_pulse_width;

      turbo_a_counter = 0;
      turbo_b_counter = 0;
   }
}

static void set_input_descriptors()
{
   struct retro_input_descriptor descriptors[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0 },
   };

   struct retro_input_descriptor descriptors_ff[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },
      { 0 },
   };

   if (libretro_supports_ff_override)
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors_ff);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors);
}

static void set_memory_descriptors(void)
{
   const uint64_t mem = RETRO_MEMORY_SYSTEM_RAM;
   struct retro_memory_descriptor desc[2] = {
      { mem, iwram, 0x00000 + 0x8000, 0x3000000, 0, 0,  0x8000, NULL },
      { mem, ewram, 0x00000,          0x2000000, 0, 0, 0x40000, NULL },
   };
   struct retro_memory_map retromap = {
      desc,
      sizeof(desc) / sizeof(desc[0])
   };
   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retromap);
}

bool retro_load_game(const struct retro_game_info* info)
{
   if (!info || !info->path)
      return false;

   /* If retro_init failed to allocate the frame buffer, refuse to load
    * rather than crash on the first video_run.  retro_init is allowed
    * to fail allocations silently; this is the point at which we have
    * to decide whether the core can usefully run. */
   if (!gba_screen_pixels)
      return false;

   /* init_gamepak_buffer() allocates the ROM buffers in 1MB blocks and
    * stops at the first failed malloc, so a memory-starved device can
    * leave none of them allocated.  load_gamepak() reads the cartridge
    * header straight out of gamepak_buffers[0], so there is no way to
    * proceed from here without dereferencing NULL. */
   if (!gamepak_buffer_count)
   {
      error_msg("Not enough memory to allocate the ROM buffer.");
      return false;
   }

   check_variables(true);
   set_input_descriptors();

   char filename_bios[MAX_PATH];
   const char* dir = NULL;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      info_msg("RGB565 is not supported.");

   extract_directory(main_path, info->path, sizeof(main_path));

   bool bios_loaded = false;
   if (selected_bios == auto_detect || selected_bios == official_bios)
   {
     /* Build '<dir>/gba_bios.bin'.  strlcpy returns the source length,
      * so we can reuse it as the offset for the suffix append - one
      * less strlen call than a strlcat-style pattern, and bounded
      * against MAX_PATH overflow either way.  If the directory path
      * was already truncated (len >= sizeof(filename_bios)), the
      * subsequent load_bios will fail and we fall back to the
      * built-in BIOS - the right outcome for a pathological frontend
      * path. */
     const char *base_dir = (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
                            ? dir : main_path;
     size_t len = strlcpy(filename_bios, base_dir, sizeof(filename_bios));
     if (len < sizeof(filename_bios))
        strlcpy(filename_bios + len, "/gba_bios.bin", sizeof(filename_bios) - len);

     bios_loaded = true;

     if (load_bios(filename_bios) != 0)
     {
        if (selected_bios == official_bios)
          show_warning_message("Could not load BIOS image file, using built-in BIOS", 2500);
        bios_loaded = false;
     }

     if (bios_loaded && bios_rom[0] != 0x18)
     {
        if (selected_bios == official_bios)
          show_warning_message("BIOS image seems incorrect, using built-in BIOS", 2500);
        bios_loaded = false;
     }
   }

   if (!bios_loaded) {
     /* Load the built-in BIOS */
     memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

   memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
   if (load_gamepak(info, info->path, rtc_mode, rumble_mode, serial_setting) != 0)
   {
      error_msg("Could not load the game file.");
      return false;
   }

   if (selected_bios != builtin_bios && gamepak_header_nonstandard)
   {
      show_warning_message("Non-standard ROM header detected, using built-in BIOS", 2500);
      memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

   extern bool require_m1_hle_bios;

   if (selected_bios != builtin_bios && require_m1_hle_bios)
   {
      show_warning_message("Pokemon ROM Hack (M1) detected, forcing built-in BIOS", 2500);
      memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

#if defined(GPSP_RV32_JIT)
   if (!gamepak_mini_materialized)
      gpsp_rv32_block_init();
#endif

#ifdef HAVE_DYNAREC
   if (gamepak_mini_materialized && dynarec_enable)
   {
      dynarec_enable = 0;
      flush_dynarec_caches();
      show_warning_message("Mini ROM detected, dynamic recompiler disabled", 2500);
   }
#endif

   struct retro_rumble_interface rumbleif;
   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumbleif))
      rumble_cb = rumbleif.set_rumble_state;
   else
      rumble_cb = NULL;

   reset_gba();

   set_memory_descriptors();

   return true;
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info* info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
#if defined(GPSP_RV32_JIT)
   gpsp_rv32_block_deinit();
#endif
   if (libretro_ff_enabled)
      set_fastforward_override(false);

   libretro_supports_bitmasks    = false;
   libretro_supports_ff_override = false;
   libretro_ff_enabled           = false;
   libretro_ff_enabled_prev      = false;

   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   turbo_a_counter   = 0;
   turbo_b_counter   = 0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return gamepak_backup;
   else if (id == RETRO_MEMORY_SYSTEM_RAM)
      return ewram;

   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return 0x20000;  /* Assume 128KiB, biggest possible save */
   else if (id == RETRO_MEMORY_SYSTEM_RAM)
      return 0x40000;

   return 0;
}

void retro_run(void)
{
   bool updated = false;
#if defined(GPSP_BENCHMARK)
   retro_time_t phase_start_us;
   retro_time_t phase_end_us;
#endif

   input_poll_cb();
   update_input();

   rumble_frame_reset();

   /* Check whether current frame should
    * be skipped */
   skip_next_frame = 0;

   if (current_frameskip_type != no_frameskip)
   {
      switch (current_frameskip_type)
      {
         case auto_frameskip:

            skip_next_frame =
                  (audio_buff_active && audio_buff_underrun) ?
                        1 : 0;

            if (!skip_next_frame ||
                (frameskip_counter >= FRAMESKIP_MAX))
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }
            else
               frameskip_counter++;

            break;
         case auto_threshold_frameskip:

            skip_next_frame =
                  (audio_buff_active &&
                        (audio_buff_occupancy < frameskip_threshold)) ?
                              1 : 0;

            if (!skip_next_frame ||
                (frameskip_counter >= FRAMESKIP_MAX))
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }
            else
               frameskip_counter++;

            break;
         case fixed_interval_frameskip:

            if (frameskip_counter < frameskip_interval)
            {
               skip_next_frame   = 1;
               frameskip_counter++;
            }
            else
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }

            break;
         default:
            skip_next_frame = 0;
            break;
      }
   }

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

   /* This runs just a frame */
#if defined(GPSP_BENCHMARK)
   phase_start_us = perf_cb.get_time_usec();
   gpsp_benchmark_timing_start(perf_cb.get_perf_counter);
#if defined(GPSP_BENCHMARK_JIT_LOOKUP) && defined(GPSP_RV32_JIT)
   gpsp_rv32_block_set_lookup_clock(perf_cb.get_perf_counter);
#endif
#endif
#if defined(GPSP_RV32_JIT)
   execute_arm_translate(frame_start_cycles());
#else
   #ifdef HAVE_DYNAREC
   if (dynarec_enable)
      execute_arm_translate(frame_start_cycles());
   else
#endif
   {
      /* Sticky bits only used in interpreter */
      clear_gamepak_stickybits();
      execute_arm(frame_start_cycles());
   }
#endif

#if defined(GPSP_BENCHMARK)
#if defined(GPSP_BENCHMARK_JIT_LOOKUP) && defined(GPSP_RV32_JIT)
   gpsp_rv32_block_set_lookup_clock(NULL);
#endif
   gpsp_benchmark_timing_stop(&s_benchmark_frame.cpu);
#if defined(GPSP_BENCHMARK_VIDEO_DETAIL)
   gpsp_benchmark_video_detail_get(&s_benchmark_frame.video_detail);
#endif
   s_benchmark_frame.execute_us = (uint32_t)(perf_cb.get_time_usec() - phase_start_us);
#endif

   if (rumble_cb) {
     // TODO: Add some user-option to select a rumble policy
     u32 strength = 0xffff * rumble_active_pct();
     rumble_cb(0, RETRO_RUMBLE_WEAK,   MIN(strength, 0xffff));
     rumble_cb(0, RETRO_RUMBLE_STRONG, MIN(strength, 0xffff) / 2);
   }

#if defined(GPSP_BENCHMARK)
   phase_start_us = perf_cb.get_time_usec();
#endif
   audio_run();
#if defined(GPSP_BENCHMARK)
   phase_end_us = perf_cb.get_time_usec();
   s_benchmark_frame.audio_us = (uint32_t)(phase_end_us - phase_start_us);
   phase_start_us = phase_end_us;
#endif
   video_run();
#if defined(GPSP_BENCHMARK)
   s_benchmark_frame.video_us = (uint32_t)(perf_cb.get_time_usec() - phase_start_us);
#endif

   switch (serial_mode) {
   case SERIAL_MODE_RFU:
     rfu_frame_update();
     break;
   case SERIAL_MODE_SERIAL_POKE:
     serialpoke_frame_update();
     break;
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}
