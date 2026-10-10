/* M5Stack Tab5: the software driver draws the desktop example's load modes over the full grid in landscape, and the
 * PPA rotates that into the portrait MIPI-DSI panel's frame buffer: (A) composed in PSRAM and rotated once per frame,
 * (B) drawn band by band into one internal-RAM band, (C) into two; in B and C a band the PPA still reads is drawn into
 * a PSRAM spare instead. C alone unless CONFIG_SHIROKO_TAB5_ALL_MODES. A worker task on the main task's core starts the
 * queued DMA2D copies (keeps; in B and C also scrolled rows moved inside the frame buffer, once a boot self-test proved
 * the moves right) and PPA rotations in order, so the work of a frame runs on while the next one is built: a frame is
 * presented once the work of the frame before it is done; its tail runs until its own is.
 * Per scene: an R line with set_cell, build, draw (software), rotate (queueing the hardware work and waiting for it),
 * present (cache write-back), frame and busy (frame less the hardware waits and the cap's sleep) p50/p95/max after WARM,
 * the largest frame of all loop frames, max1 (loop frame 0 left out, as frames are judged), settle, transitions, flash
 * reads and traffic per frame; an M line (move waits, tail, PSRAM traffic); a frame buffer checksum every mode and rep
 * must agree on. A round ends with each scene's median and largest over its reps. Loop frames run back to back unless
 * CONFIG_SHIROKO_TAB5_FPS_CAP (or TAB5_FPS_CAP) caps them (0 = uncapped).
 * Build options: TAB5_LOADS=<mask> (bit n: load n), TAB5_MOVES=0 (draw moved rows again), TAB5_DMA_MBPS (DMA2D rate
 * cap), TAB5_MOVE_CHUNKS (runs of rows per move), TAB5_STEPS (scroll steps per frame), TAB5_REPLAY=1 (each scene's
 * batches and calls replayed; TAB5_REPLAY_PARTS bits driver-sw, driver-p4, compositor; TAB5_REPLAY_FRAMES=1 adds BF
 * lines), TAB5_COST=1 (the README's app cost table), TAB5_SOAK=1 (H lines with the heaps and the allocations by the
 * context, the driver and, with CONFIG_HEAP_USE_HOOKS, all; a Z line that must count none over the last TAB5_SOAK_K
 * loop frames, else a FAIL line). CONFIG_SHIROKO_TAB5_BOARD_HEADLESS runs the same on an ESP32-P4 without a panel,
 * scanning the frame buffer out with a Tab5 panel's timing unless CONFIG_SHIROKO_TAB5_SCANOUT_NONE. */
#include <shiroko/port_software.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if TAB5_COST /* whatever else the build sets */
#undef TAB5_REPLAY
#undef TAB5_REPLAY_PARTS
#undef TAB5_LOADS
#undef TAB5_FPS_CAP
#define TAB5_REPLAY 1
#define TAB5_REPLAY_PARTS 6
#define TAB5_LOADS T5_COST_LOADS
#define TAB5_FPS_CAP 0
#endif
#ifndef TAB5_REPLAY
#define TAB5_REPLAY 0
#endif
#include "rec.h"
#if TAB5_REPLAY
#include "rec_api.h"
#endif
#include "tab5_scenes.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define CW SHR_CELL_WIDTH
#define CH SHR_CELL_HEIGHT
#define SCROLL_FRAMES T5_SCROLL_FRAMES
#define WARM 10
#ifndef TAB5_REPLAY_FRAMES
#define TAB5_REPLAY_FRAMES 0
#endif
#ifndef TAB5_REPLAY_PARTS
#define TAB5_REPLAY_PARTS 7
#endif
#define REPS (TAB5_REPLAY ? 1 : 3)
#define SETTLE_MAX 64
#ifdef TAB5_FPS_CAP
#define FPS_CAP TAB5_FPS_CAP
#else
#define FPS_CAP CONFIG_SHIROKO_TAB5_FPS_CAP
#endif
#define CAP_NS (FPS_CAP ? 1000000000ull / (FPS_CAP + !FPS_CAP) : 0)
#define HW_TIMEOUT_MS 200
#define PPA_QUEUE 8
#define BAND_H 16
/* Quarter turns to output x offsets of 2 mod 4 can hang the PPA (esp-idf#19096): 16-pixel edges avoid them. */
#define PPA_ALIGN 16
#define DMA_QUEUE 16
#define DMA_MIN_BYTES 8192 /* smaller copies cost the CPU less than a DMA2D submission */
#define DMA_BURST 128      /* bytes per DMA2D burst (16..128): smaller ones hold PSRAM in shorter turns, slower */
#define CHAIN 64           /* queued copies, and rotations */
#define SPARES 3           /* PSRAM bands drawn into while an internal band still waits for the hardware */
#define SLOTS (2 + SPARES)

#define FONT_SUBTYPE 0x40

#if CONFIG_SHIROKO_TAB5_BOARD_TAB5
#define BOARD "Tab5"
#elif CONFIG_SHIROKO_TAB5_SCANOUT_NONE
#define BOARD "headless P4, no scan-out"
#elif CONFIG_SHIROKO_TAB5_SCANOUT_ILI9881C
#define BOARD "headless P4, ILI9881C scan-out"
#else
#define BOARD "headless P4, ST7121 scan-out"
#endif

#ifndef TAB5_LOADS
#define TAB5_LOADS ((1u << COST_CELL) - 1)
#endif
#ifndef TAB5_MOVES
#define TAB5_MOVES 1
#endif
#ifndef TAB5_DMA_MBPS
#define TAB5_DMA_MBPS 0
#endif
#ifndef TAB5_SOAK
#define TAB5_SOAK 0
#endif
#ifndef TAB5_SOAK_K
#define TAB5_SOAK_K 10
#endif
#ifndef TAB5_MOVE_CHUNKS
#define TAB5_MOVE_CHUNKS 1
#endif
enum { COMPOSE, BAND1, BAND2, MODES };
static const char *const mode_names[MODES] = {"A compose", "B 1 band", "C 2 bands"};
#ifdef CONFIG_SHIROKO_TAB5_ALL_MODES
#define MODE0 COMPOSE
#else
#define MODE0 BAND2
#endif

/* Bytes of keep copies by DMA and by the CPU, PPA rotations and moves (read and written). */
typedef struct traffic {
    uint64_t copied[2], rotated, moved;
} traffic;

typedef struct scene_stats {
    float p50, p95, max, max1, busy;
    int transitions, settle;
} scene_stats;

/* The numbers of each frame and scene, read once a scene: in PSRAM, leaving internal RAM to the application. */
static EXT_RAM_BSS_ATTR struct {
    float t[7][SCROLL_FRAMES], move_t[SCROLL_FRAMES], move_tail[SCROLL_FRAMES];
    uint32_t sums[REPS][LOADS][MODES];
    scene_stats stats[REPS][LOADS][MODES];
} log_ram;

static struct {
    esp_lcd_panel_handle_t panel;
    shr_surface fb, bands[2];
    shr_framebuffer_driver drv, sw; /* drv runs ROTATE on the PPA, the rest on sw */
    ppa_client_handle_t ppa;
    TaskHandle_t task;
    const void *shown; /* the composition the panel shows, except `dirty` */
    shr_rect dirty;
    t5_scene s;
    int mode, cur; /* cur: the loop frame running, -1 outside the loop */
    int64_t present_us, draw_us, rotate_us, wait_us, read_us, read_bytes;
    int64_t sleep_us, wall_us; /* until the frame-rate cap lets a frame start; the frames after WARM */
    uint32_t presents;
    traffic bytes, loop; /* since WARM; at the end of the loop frames */
    float (*t)[SCROLL_FRAMES];
    uint32_t (*sums)[LOADS][MODES];
    scene_stats (*stats)[LOADS][MODES];
} a = {.cur = -1, .t = log_ram.t, .sums = log_ram.sums, .stats = log_ram.stats};

static void check(shr_status st, const char *what) {
    if (st == SHR_OK) return;
    printf("%s: %s\n", what, shr_status_name(st));
    for (;;) vTaskDelay(portMAX_DELAY);
}

/* Writes the CPU's frame buffer writes back (C2M) or drops its cached lines (M2C); the first failure is printed. */
static void fb_sync(int dir) {
    static bool failed;
    esp_err_t e = esp_cache_msync(a.fb.pixels, a.fb.byte_length, dir);
    if (e != ESP_OK && !failed) failed = true, printf("frame buffer cache sync: %s\n", esp_err_to_name(e));
}

/* ===== DMA2D copies and PPA rotations: queued by the main task, started in order by a worker task on its core ===== */

/* Copies start in queue order, each once the copies up to dma_after and the rotations up to ppa_after are done;
 * rotations likewise after the copies up to dma_after, the last writing their slots or the frame buffer. Copy n and
 * rotation n are tickets n; rotations finish in order, copies in any (the DMA2D runs two at once), so a copy ticket
 * counts as done once it and all before it are. */
typedef struct dma_op {
    async_color_convert_request_t q;
    uint32_t dma_after, ppa_after;
    uint16_t lo, hi, left, right; /* a trimmed keep draw's fill (shr_software_trim, RGB565) */
} dma_op;

typedef struct ppa_op {
    ppa_srm_oper_config_t op;
    uint32_t dma_after;
} ppa_op;

/* A cap on the copy rate in MB/s (bytes per us), leaving PSRAM bandwidth to the app; 0 = none. */
static uint32_t dma_mbps = TAB5_DMA_MBPS;

enum { MOVE_AUTO, MOVE_ONE, MOVE_TWO, MOVE_COLUMNS };
static struct {
    uint16_t *tmp;
    int32_t up_max;
    bool ok;   /* B and C advertise moves */
    int force; /* the self-test's way */
    uint32_t last; /* ticket of the last run queued */
    int64_t wait_us;
    float *t, *tail; /* per frame: waits while a move was due; the tail */
} mv = {.t = log_ram.move_t, .tail = log_ram.move_tail};

/* Slots: the internal bands, then the PSRAM spares. last: the tickets of the last copy and rotation touching each;
 * dma_wr: of the last copy writing each, dma_fb: writing the frame buffer. The main task is notified once what it
 * waits for (want_*, or any completion) is done. */
static struct {
    TaskHandle_t worker;
    async_color_convert_handle_t conv;
    dma_op *dq;
    ppa_op *pq;
    uint32_t dq_head, dq_tail, pq_head, pq_tail; /* head: the next to start (worker), tail: the next free (main) */
    volatile uint32_t dma_done, ppa_done, dma_finished;
    volatile uint8_t finished[64]; /* copies finished past dma_done */
    uint32_t dma_started, ppa_started;
    uint64_t dma_queued, ppa_queued;
    uint32_t dma_last[SLOTS], ppa_last[SLOTS], dma_wr[SLOTS], dma_fb;
    volatile uint32_t want_dma, want_ppa;
    volatile bool waiting, any;
    uint8_t *spare[SPARES];
    int phys[2];  /* the slot each band draws into */
    bool cpu[2];  /* whether the CPU draws into it */
    bool held;    /* held_op: band 0's rotation, waiting to go with band 1's */
    ppa_srm_oper_config_t held_op;
    esp_timer_handle_t pace;
    int64_t next_us;
    volatile bool pace_armed;
} hw;

/* The frame's tail: from its present until the work queued by then is done. The completions read the tickets: set
 * before `end`. */
static struct {
    volatile uint32_t dma, ppa;
    volatile int64_t at, end;
    int frame;
} tail = {.frame = -1};

static bool done(uint32_t now, uint32_t ticket) { return (int32_t)(now - ticket) >= 0; }

static void tail_check(void) {
    if (!tail.end && done(hw.dma_done, tail.dma) && done(hw.ppa_done, tail.ppa)) tail.end = esp_timer_get_time();
}

static bool dma_may(const dma_op *o);
static bool ppa_ready(const ppa_op *o);

/* The worker is woken once an operation at the head of a queue may start. */
static bool notify_both(void) {
    BaseType_t woken = pdFALSE;
    uint32_t dh = hw.dq_head, ph = hw.pq_head;
    tail_check();
    if ((dh != __atomic_load_n(&hw.dq_tail, __ATOMIC_ACQUIRE) && dma_may(&hw.dq[dh % CHAIN])) ||
        (ph != __atomic_load_n(&hw.pq_tail, __ATOMIC_ACQUIRE) && ppa_ready(&hw.pq[ph % CHAIN])))
        vTaskNotifyGiveFromISR(hw.worker, &woken);
    if (hw.waiting && (hw.any || (done(hw.dma_done, hw.want_dma) && done(hw.ppa_done, hw.want_ppa))))
        vTaskNotifyGiveFromISR(a.task, &woken);
    return woken == pdTRUE;
}

static bool dma_finished(async_color_convert_handle_t conv, async_color_convert_event_data_t *e, void *ticket) {
    (void)conv, (void)e;
    hw.finished[(uintptr_t)ticket % N(hw.finished)] = 1, hw.dma_finished++;
    for (uint32_t t = hw.dma_done + 1; hw.finished[t % N(hw.finished)]; t++) hw.finished[t % N(hw.finished)] = 0, hw.dma_done = t;
    return notify_both();
}

static bool ppa_finished(ppa_client_handle_t client, ppa_event_data_t *e, void *user) {
    (void)client, (void)e, (void)user;
    hw.ppa_done++;
    return notify_both();
}

static void dma_paced(void *arg) {
    (void)arg;
    hw.pace_armed = false;
    xTaskNotifyGive(hw.worker);
}

/* The queue keeps a spare entry, as the drivers may return one only after the callback ran; tickets started stay
 * within `finished` past dma_done. */
static bool dma_may(const dma_op *o) {
    return done(hw.dma_done, o->dma_after) && done(hw.ppa_done, o->ppa_after) &&
           hw.dma_started - hw.dma_finished < DMA_QUEUE - 2 && hw.dma_started - hw.dma_done < N(hw.finished);
}

static bool dma_ready(const dma_op *o) {
    if (!dma_may(o)) return false;
    if (!dma_mbps) return true;
    int64_t now = esp_timer_get_time(); /* with a cap, a copy starts no earlier than the rate allows after the last */
    if (hw.next_us > now) {
        if (!hw.pace_armed) {
            hw.pace_armed = true;
            ESP_ERROR_CHECK(esp_timer_start_once(hw.pace, (uint64_t)(hw.next_us - now)));
        }
        return false;
    }
    hw.next_us = (hw.next_us > now ? hw.next_us : now) + (int64_t)((size_t)o->q.copy_width * o->q.copy_height * 2 / dma_mbps);
    return true;
}

static bool ppa_ready(const ppa_op *o) {
    return done(hw.dma_done, o->dma_after) && hw.ppa_started - hw.ppa_done < PPA_QUEUE - 1;
}

/* A trimmed keep draw's copy is started, then the CPU fills the band columns around it while it runs: other cache lines
 * than the copy writes (whole ones, the copy's being aligned to them), written back by the rotation's cache sync. The
 * rotation starts only after the copy is done, and the fill is finished before then. */
static void dma_go(dma_op *o) {
    o->q.src_color_format = o->q.dst_color_format = ESP_COLOR_FOURCC_RGB16;
    if (esp_async_color_convert(hw.conv, &o->q, dma_finished, (void *)(uintptr_t)++hw.dma_started) != ESP_OK)
        check(SHR_E_DEVICE, "DMA2D");
    if (o->lo | o->hi)
        shr_software_trim_fill(o->q.dst_buffer, o->q.dst_stride * 2, o->q.copy_width * 2, o->q.copy_height,
                               &(shr_software_trim){o->lo, o->hi, o->left * 0x10001u, o->right * 0x10001u});
}

static void ppa_go(ppa_op *o) {
    hw.ppa_started++;
    if (ppa_do_scale_rotate_mirror(a.ppa, &o->op) != ESP_OK) check(SHR_E_DEVICE, "PPA");
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        for (bool went = true; went;) {
            went = false;
            if (hw.dq_head != __atomic_load_n(&hw.dq_tail, __ATOMIC_ACQUIRE) && dma_ready(&hw.dq[hw.dq_head % CHAIN])) {
                dma_go(&hw.dq[hw.dq_head % CHAIN]);
                __atomic_store_n(&hw.dq_head, hw.dq_head + 1, __ATOMIC_RELEASE);
                went = true;
            }
            if (hw.pq_head != __atomic_load_n(&hw.pq_tail, __ATOMIC_ACQUIRE) && ppa_ready(&hw.pq[hw.pq_head % CHAIN])) {
                ppa_go(&hw.pq[hw.pq_head % CHAIN]);
                __atomic_store_n(&hw.pq_head, hw.pq_head + 1, __ATOMIC_RELEASE);
                went = true;
            }
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

/* Waits for a notification, counting the time as waiting (for moves while one is due). */
static void hw_block(void) {
    int64_t t0 = esp_timer_get_time();
    bool moving = !done(hw.dma_done, mv.last);
    uint32_t d = hw.dma_done + hw.ppa_done; /* a hang: nothing finished for HW_TIMEOUT_MS */
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HW_TIMEOUT_MS)) && d == hw.dma_done + hw.ppa_done)
        check(SHR_E_TIMEOUT, "DMA2D/PPA HANG");
    int64_t t = esp_timer_get_time() - t0;
    a.wait_us += t, mv.wait_us += moving ? t : 0;
}

/* Until the copies up to `dma` and the rotations up to `ppa` are done. */
static void hw_wait(uint32_t dma, uint32_t ppa) {
    if (done(hw.dma_done, dma) && done(hw.ppa_done, ppa)) return;
    hw.want_dma = dma, hw.want_ppa = ppa, hw.waiting = true;
    while (!done(hw.dma_done, dma) || !done(hw.ppa_done, ppa)) hw_block();
    hw.waiting = false;
}

/* Until a queue has room. */
static void hw_room(const uint32_t *tail, const uint32_t *head) {
    if (*tail - __atomic_load_n(head, __ATOMIC_ACQUIRE) < CHAIN) return;
    hw.any = hw.waiting = true;
    while (*tail - __atomic_load_n(head, __ATOMIC_ACQUIRE) >= CHAIN) hw_block();
    hw.any = hw.waiting = false;
}

/* An operation that may start now while its queue is empty starts here: the worker, asleep then, has nothing of that
 * kind to start. Otherwise it is queued; the worker is woken when it is at the head once queued (the worker may have
 * emptied the queue meanwhile), else by the completion the operation ahead of it waits for. */
static uint32_t dma_queue(async_color_convert_request_t q, uint32_t dma_after, uint32_t ppa_after,
                          const shr_software_trim *t) {
    hw_room(&hw.dq_tail, &hw.dq_head);
    uint32_t head = __atomic_load_n(&hw.dq_head, __ATOMIC_ACQUIRE);
    dma_op *o = &hw.dq[hw.dq_tail % CHAIN];
    *o = t ? (dma_op){q, dma_after, ppa_after, (uint16_t)t->lo, (uint16_t)t->hi, (uint16_t)t->left, (uint16_t)t->right}
           : (dma_op){q, dma_after, ppa_after, 0, 0, 0, 0};
    if (head == hw.dq_tail && dma_ready(o)) {
        dma_go(o);
    } else {
        __atomic_store_n(&hw.dq_tail, hw.dq_tail + 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&hw.dq_head, __ATOMIC_SEQ_CST) == hw.dq_tail - 1) xTaskNotifyGive(hw.worker);
    }
    return (uint32_t)++hw.dma_queued;
}

static uint32_t ppa_queue(ppa_srm_oper_config_t op, uint32_t dma_after) {
    hw_room(&hw.pq_tail, &hw.pq_head);
    uint32_t head = __atomic_load_n(&hw.pq_head, __ATOMIC_ACQUIRE);
    ppa_op *o = &hw.pq[hw.pq_tail % CHAIN];
    *o = (ppa_op){op, dma_after};
    if (head == hw.pq_tail && ppa_ready(o)) {
        ppa_go(o);
    } else {
        __atomic_store_n(&hw.pq_tail, hw.pq_tail + 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&hw.pq_head, __ATOMIC_SEQ_CST) == hw.pq_tail - 1) xTaskNotifyGive(hw.worker);
    }
    return (uint32_t)++hw.ppa_queued;
}

static uint32_t later(uint32_t t, uint32_t u) { return done(t, u) ? t : u; }

/* A rotation reading slot s waits for the copies writing it or the frame buffer, not for those reading it. */
static uint32_t rot_after(int s) { return later(hw.dma_wr[s], hw.dma_fb); }

static void held_flush(void) {
    if (hw.held) hw.held = false, hw.ppa_last[0] = ppa_queue(hw.held_op, rot_after(0));
}

static void hw_drain(void) {
    held_flush();
    hw_wait((uint32_t)hw.dma_queued, (uint32_t)hw.ppa_queued);
}

static uint8_t *slot_px(int s) { return s < 2 ? a.bands[s].pixels : hw.spare[s - 2]; }

static int slot_of(const void *p) {
    for (int s = 0; s < SLOTS; s++) {
        const uint8_t *b = slot_px(s);
        if (b && (const uint8_t *)p >= b && (const uint8_t *)p < b + a.bands[0].byte_length) return s;
    }
    return -1;
}

static bool slot_free(int s) { return done(hw.dma_done, hw.dma_last[s]) && done(hw.ppa_done, hw.ppa_last[s]); }

static bool slot_started(int s) { return done(hw.dma_started, hw.dma_last[s]) && done(hw.ppa_started, hw.ppa_last[s]); }

/* The slot band k the CPU draws goes to: its own once its work started, else a free spare, else its own, but while
 * moves run (its rotation may wait for all of them) the one whose last rotation comes first. */
static int slot_for(int k) {
    if (slot_started(k)) return k;
    int e = 2;
    for (int i = 2; i < SLOTS; i++) {
        if (slot_free(i)) return i;
        e = done(hw.ppa_last[i], hw.ppa_last[e]) ? e : i;
    }
    return done(hw.dma_done, mv.last) || done(hw.ppa_last[e], hw.ppa_last[k]) ? k : e;
}

static void slot_wait(int s) {
    if (s >= 0) hw_wait(hw.dma_last[s], hw.ppa_last[s]);
}

/* Starts copying w x h pixels (filling around them as `t` says); its ticket. */
static uint32_t dma_copy(void *dst, size_t dst_stride, const void *src, size_t src_stride, int32_t w, int32_t h,
                         uint32_t dma_after, uint32_t ppa_after, const shr_software_trim *t) {
    return dma_queue((async_color_convert_request_t){.src_buffer = src, .src_stride = src_stride / 2, .src_height = h,
                                                     .dst_buffer = dst, .dst_stride = dst_stride / 2, .dst_height = h,
                                                     .copy_width = w, .copy_height = h},
                     dma_after, ppa_after, t);
}

static const ppa_srm_rotation_angle_t ccw[] = {[SHR_ROTATE_NONE] = PPA_SRM_ROTATION_ANGLE_0,
                                               [SHR_ROTATE_90_CW] = PPA_SRM_ROTATION_ANGLE_270,
                                               [SHR_ROTATE_180] = PPA_SRM_ROTATION_ANGLE_180,
                                               [SHR_ROTATE_90_CCW] = PPA_SRM_ROTATION_ANGLE_90};

/* Rotating block `r` of the w x h picture at `in` into `d` at (ox, oy). */
static ppa_srm_oper_config_t ppa_config(const shr_surface *d, const void *in, int32_t w, int32_t h, shr_rect r, int32_t ox,
                                        int32_t oy, shr_rotation rot) {
    a.bytes.rotated += (uint64_t)(r.x1 - r.x0) * (uint64_t)(r.y1 - r.y0) * 2;
    return (ppa_srm_oper_config_t){
        .in = {.buffer = in, .pic_w = w, .pic_h = h, .block_w = r.x1 - r.x0, .block_h = r.y1 - r.y0,
               .block_offset_x = r.x0, .block_offset_y = r.y0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = d->pixels, .buffer_size = d->byte_length, .pic_w = d->width, .pic_h = d->height,
                .block_offset_x = ox, .block_offset_y = oy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = ccw[rot], .scale_x = 1, .scale_y = 1, .mode = PPA_TRANS_MODE_NON_BLOCKING};
}

static bool packed565(const shr_surface *d, const shr_image_ref *s) {
    return s->format == SHR_FORMAT_RGB565 && d->format == SHR_FORMAT_RGB565 && d->stride == (size_t)d->width * 2;
}

static int32_t up16(int32_t v, int32_t max) { return v + 15 > max ? max : (v + 15) & ~15; }

/* (A) Rotates what was drawn into the composition since the last rotation, all of it when unknown, and waits;
 * false when the PPA cannot take it. */
static bool ppa_rotate(const shr_surface *d, const shr_draw_cmd *c) {
    const shr_image_ref *s = &c->src;
    if (!packed565(d, s) || s->stride != (size_t)s->width * 2) return false;
    shr_rect r = a.shown == s->pixels && a.dirty.x0 < a.dirty.x1 ? a.dirty : (shr_rect){0, 0, s->width, s->height};
    r = (shr_rect){r.x0 & ~15, r.y0 & ~15, up16(r.x1, s->width), up16(r.y1, s->height)};
    shr_rotation rot = c->rotation;
    int32_t ox = rot == SHR_ROTATE_90_CW ? s->height - r.y1 : rot == SHR_ROTATE_90_CCW ? r.y0 : s->width - r.x1;
    int32_t oy = rot == SHR_ROTATE_90_CW ? r.x0 : rot == SHR_ROTATE_90_CCW ? s->width - r.x1 : s->height - r.y1;
    a.shown = NULL;
    hw_drain();
    hw_wait(0, ppa_queue(ppa_config(d, s->pixels, s->width, s->height, r, c->dst.x0 + ox, c->dst.y0 + oy, rot),
                         (uint32_t)hw.dma_queued));
    a.shown = s->pixels, a.dirty = (shr_rect){0};
    return true;
}

static int band_of(const void *p) {
    for (int k = 0; k < 2; k++) {
        const uint8_t *b = a.bands[k].pixels;
        if (b && (const uint8_t *)p >= b && (const uint8_t *)p < b + a.bands[k].byte_length) return k;
    }
    return -1;
}

/* The software driver's copier. The DMA writes whole cache lines from dst on: dst starts one, and its rows do too or
 * are contiguous (a keep). A copy into a slot starts once the copies and rotations before it touching the slot are
 * done (a keep-only batch does not wait for the batches before it); one left to the CPU waits for its slots. */
static uint64_t keep_copy(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride, size_t bytes,
                          int32_t rows) {
    (void)user;
    int sd = slot_of(dst), ss = slot_of(src), s = sd >= 0 ? sd : ss;
    a.bytes.copied[1] += bytes * rows;
    if (bytes * rows < DMA_MIN_BYTES || (uintptr_t)dst % CONFIG_CACHE_L2_CACHE_LINE_SIZE ||
        ((uintptr_t)src | bytes) % 32 || (dst_stride % CONFIG_CACHE_L2_CACHE_LINE_SIZE && dst_stride != bytes)) {
        slot_wait(sd), slot_wait(ss);
        return 0;
    }
    uint32_t t =
        dma_copy(dst, dst_stride, src, src_stride, (int32_t)(bytes / 2), rows, sd >= 0 ? hw.dma_last[sd] : 0,
                 s >= 0 ? hw.ppa_last[s] : 0, NULL);
    a.bytes.copied[0] += bytes * rows, a.bytes.copied[1] -= bytes * rows;
    if (sd >= 0) hw.dma_last[sd] = hw.dma_wr[sd] = t;
    if (ss >= 0) hw.dma_last[ss] = t;
    if ((uintptr_t)dst - (uintptr_t)a.fb.pixels < a.fb.byte_length) hw.dma_fb = t;
    return hw.dma_queued;
}

static void keep_wait(void *user, uint64_t ticket) {
    (void)user;
    hw_wait((uint32_t)ticket, 0);
}

/* A trimmed keep draw into an internal band: once the copies and rotations before it touching the slot are done, its
 * copy starts and the band columns around it are filled (dma_go), without the main task waiting. Elsewhere, or when
 * the whole rows would not go to the DMA2D, the whole rows are copied. */
static uint64_t keep_copy_trim(void *user, void *dst, size_t dst_stride, const void *src, size_t src_stride,
                               size_t bytes, int32_t rows, const shr_software_trim *t) {
    int s = slot_of(dst);
    size_t all = bytes + t->lo + t->hi;
    if (s < 0 || s >= 2 || all * rows < DMA_MIN_BYTES || (uintptr_t)dst % CONFIG_CACHE_L2_CACHE_LINE_SIZE ||
        ((uintptr_t)src | bytes) % 32 || dst_stride % CONFIG_CACHE_L2_CACHE_LINE_SIZE)
        return keep_copy(user, (uint8_t *)dst - t->lo, dst_stride, (const uint8_t *)src - t->lo, src_stride, all, rows);
    a.bytes.copied[0] += bytes * rows;
    hw.dma_last[s] = hw.dma_wr[s] =
        dma_copy(dst, dst_stride, src, src_stride, (int32_t)(bytes / 2), rows, hw.dma_last[s], hw.ppa_last[s], t);
    return hw.dma_queued;
}

static const shr_software_copier p4_copier = {.copy = keep_copy, .wait = keep_wait, .copy_trim = keep_copy_trim,
                                              .align = CONFIG_CACHE_L2_CACHE_LINE_SIZE};

/* ===== Moves: a COPY of the frame buffer onto itself, on the DMA2D ===== */

/* The DMA2D moves a window onto an overlapping one toward lower addresses right, toward higher ones only by less than
 * its burst along a row (measured, not documented; a burst or more can pass the self-test yet go wrong while the CPU
 * writes PSRAM): the boot self-test finds how far one copy goes below a burst (up_max pixels).
 * Farther along a row, the window moves in columns as wide as the move, from the far end on, each started once the
 * one before is done; other moves take two copies through `tmp`. A copy runs in TAB5_MOVE_CHUNKS runs of rows, which
 * dma_mbps spaces out. Moves start once the rotations queued before them are done. */

/* Queues `q` in runs of rows after copy `after`; the ticket of the last. */
static uint32_t move_queue(async_color_convert_request_t q, uint32_t after) {
    uint32_t y = 0, h = q.copy_height, t = after;
    for (int k = 1; k <= TAB5_MOVE_CHUNKS; k++) {
        uint32_t y1 = h * (uint32_t)k / TAB5_MOVE_CHUNKS;
        async_color_convert_request_t r = q;
        r.src_y += y, r.dst_y += y, r.copy_height = y1 - y;
        a.bytes.moved += 4ull * r.copy_width * r.copy_height; /* RGB565, read and written */
        if (y1 > y) t = hw.dma_fb = dma_queue(r, after, (uint32_t)hw.ppa_queued, NULL);
        y = y1;
    }
    return t;
}

static bool is_move(const shr_surface *d, const shr_draw_cmd *c) {
    return c->kind == SHR_CMD_COPY && c->src.pixels == d->pixels && mv.ok && packed565(d, &c->src);
}

/* Queues `c`, a COPY of `d` onto itself. */
static void move_start(const shr_surface *d, const shr_draw_cmd *c) {
    async_color_convert_request_t q = {
        .src_buffer = d->pixels, .src_stride = (uint32_t)d->width, .src_height = (uint32_t)d->height,
        .src_x = (uint32_t)c->src_origin.x, .src_y = (uint32_t)c->src_origin.y,
        .dst_buffer = d->pixels, .dst_stride = (uint32_t)d->width, .dst_height = (uint32_t)d->height,
        .dst_x = (uint32_t)c->dst.x0, .dst_y = (uint32_t)c->dst.y0,
        .copy_width = (uint32_t)(c->dst.x1 - c->dst.x0), .copy_height = (uint32_t)(c->dst.y1 - c->dst.y0)};
    int32_t up = c->src_origin.y == c->dst.y0 ? c->dst.x0 - c->src_origin.x : INT32_MAX;
    int way = mv.force ? mv.force : up <= mv.up_max ? MOVE_ONE : up < INT32_MAX ? MOVE_COLUMNS : MOVE_TWO;
    if (way == MOVE_COLUMNS) {
        for (int32_t x = (int32_t)q.copy_width; x > 0; x -= up) {
            async_color_convert_request_t s = q;
            int32_t x0 = x > up ? x - up : 0;
            s.src_x += (uint32_t)x0, s.dst_x += (uint32_t)x0, s.copy_width = (uint32_t)(x - x0);
            mv.last = move_queue(s, mv.last);
        }
        return;
    }
    if (way == MOVE_TWO) {
        async_color_convert_request_t to_tmp = q;
        to_tmp.dst_buffer = mv.tmp, to_tmp.dst_x = q.src_x, to_tmp.dst_y = q.src_y;
        mv.last = move_queue(to_tmp, mv.last);
        q.src_buffer = mv.tmp;
    }
    mv.last = move_queue(q, mv.last);
}

static uint16_t move_pattern(int32_t x, int32_t y) { return (uint16_t)(x * 31 + y * 7); }

/* Moves a pattern in the frame buffer from column x0 on `s` pixels along its rows the given way and checks every
 * pixel; the time in microseconds, or -1 when a pixel is wrong. */
static int64_t move_try(int32_t s, int way, int32_t x0) {
    uint16_t *px = a.fb.pixels;
    int32_t w = a.fb.width, h = a.fb.height, sx = x0 + (s < 0 ? -s : 0), dx = x0 + (s < 0 ? 0 : s);
    int32_t cw = w - x0 - (s < 0 ? -s : s);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) px[y * w + x] = move_pattern(x, y);
    fb_sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    mv.force = way;
    int64_t t0 = esp_timer_get_time();
    move_start(&a.fb, &(shr_draw_cmd){.kind = SHR_CMD_COPY, .dst = {dx, 0, dx + cw, h}, .src_origin = {sx, 0}});
    hw_drain();
    int64_t t = esp_timer_get_time() - t0;
    mv.force = MOVE_AUTO;
    fb_sync(ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    bool ok = true;
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++)
            ok &= px[y * w + x] == (x >= dx && x < dx + cw ? move_pattern(x - dx + sx, y) : move_pattern(x, y));
    return ok ? t : -1;
}

/* Moves are advertised once one copy moves a row (CH pixels) both ways, from column 0 and from column CH on (as below
 * a status line), and columns and two copies move any distance. */
static void move_setup(void) {
    mv.tmp = heap_caps_aligned_alloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, a.fb.byte_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    printf("move self-test (DMA2D burst %d, %d runs, %lu MB/s cap, temporary frame %s):", DMA_BURST, TAB5_MOVE_CHUNKS,
           (unsigned long)dma_mbps, mv.tmp ? "ok" : "none");
    if (!mv.tmp || !TAB5_MOVES) {
        printf(" moves off\n");
        return;
    }
    static const int32_t shifts[] = {CH, 2 * CH, 3 * CH, 4 * CH, 6 * CH, -CH, -5 * CH};
    bool down = true, gap = false, far = true;
    for (size_t i = 0; i < N(shifts); i++) {
        int32_t s = shifts[i];
        bool ok = true;
        int64_t t = 0;
        for (int rep = 0; rep < 3; rep++) ok &= (t = move_try(s, MOVE_ONE, 0)) >= 0 && move_try(s, MOVE_ONE, CH) >= 0;
        printf(" %+ld %s", (long)s, ok ? "ok" : "WRONG");
        if (ok) printf(" %.2f ms", (double)t / 1000);
        if (s > 0 && ok && !gap && s * 2 < DMA_BURST) mv.up_max = s;
        gap |= s > 0 && !ok;
        down &= s > 0 || ok;
    }
    static const struct {
        int way;
        int32_t s;
        const char *name;
    } fars[] = {{MOVE_COLUMNS, 5 * CH, "columns"}, {MOVE_TWO, 5 * CH, "two copies"}, {MOVE_TWO, -CH, "two copies"}};
    for (size_t i = 0; i < N(fars); i++) {
        int64_t t = move_try(fars[i].s, fars[i].way, 0);
        printf(" %s %+ld %s", fars[i].name, (long)fars[i].s, t >= 0 ? "ok" : "WRONG");
        if (t >= 0) printf(" %.2f ms", (double)t / 1000);
        far &= t >= 0;
    }
    mv.ok = mv.up_max >= CH && down && far;
    printf(" -> one copy up to +%ld px, moves %s\n", (long)mv.up_max, mv.ok ? "on in B and C" : "off");
}

/* (B, C) A batch rotating parts of one band from the slot it was drawn into: queued, not waited for. Two whole bands
 * of keeps next to each other, band 0's above, turn as one block of the two internal bands (adjacent in memory): band
 * 0's rotation is held until band 1's comes. */
static bool ppa_band(const shr_surface *d, const shr_draw_cmd *c, size_t n) {
    int k = band_of(c->src.pixels);
    for (size_t i = 0; i < n; i++)
        if (c[i].kind != SHR_CMD_ROTATE || band_of(c[i].src.pixels) != k || !packed565(d, &c[i].src) ||
            (c[i].dst.x0 | c[i].dst.y0) % PPA_ALIGN)
            return false;
    const shr_surface *b = &a.bands[k];
    int s = hw.phys[k];
    bool whole = n == 1 && s == k && !hw.cpu[k] && c->src.pixels == b->pixels && c->src.width == b->width &&
                 c->src.height == b->height && c->rotation == SHR_ROTATE_90_CW;
    hw.phys[k] = k, hw.cpu[k] = true; /* the band's next draw batch sets them again */
    shr_rect all = {0, 0, b->width, b->height};
    if (whole && k == 1 && hw.held && hw.held_op.out.block_offset_x == c->dst.x1 &&
        hw.held_op.out.block_offset_y == c->dst.y0) {
        hw.held = false, a.bytes.rotated -= b->byte_length; /* counted again below */
        hw.ppa_last[0] = hw.ppa_last[1] = ppa_queue(
            ppa_config(d, a.bands[0].pixels, b->width, 2 * b->height, (shr_rect){0, 0, b->width, 2 * b->height},
                       c->dst.x0, c->dst.y0, c->rotation),
            later(rot_after(0), hw.dma_wr[1]));
        return true;
    }
    held_flush();
    if (whole && k == 0) {
        hw.held = true, hw.held_op = ppa_config(d, b->pixels, b->width, b->height, all, c->dst.x0, c->dst.y0, c->rotation);
        return true;
    }
    for (size_t i = 0; i < n; i++) {
        size_t off = (size_t)((const uint8_t *)c[i].src.pixels - (const uint8_t *)b->pixels);
        int32_t x = (int32_t)(off % b->stride / 2), y = (int32_t)(off / b->stride);
        shr_rect r = {x, y, x + c[i].src.width, y + c[i].src.height};
        hw.ppa_last[s] = ppa_queue(
            ppa_config(d, slot_px(s), b->width, b->height, r, c[i].dst.x0, c[i].dst.y0, c[i].rotation), rot_after(s));
    }
    return true;
}

/* ROTATE goes to the PPA, moves to the DMA2D, everything else to the software driver, which tracks what it draws into the
 * composition. */
static shr_status p4_execute(void *user, const shr_surface *d, const shr_draw_cmd *c, size_t n, shr_fence f) {
    (void)user;
    int64_t t0 = esp_timer_get_time();
    if (n && c->kind == SHR_CMD_ROTATE && (band_of(c->src.pixels) >= 0 ? ppa_band(d, c, n) : n == 1 && ppa_rotate(d, c))) {
        a.rotate_us += esp_timer_get_time() - t0;
        return SHR_OK;
    }
    int k = band_of(d->pixels);
    if (k != 1) held_flush();
    size_t p = 0;
    while (p < n && c[p].kind >= SHR_CMD_BUFFER_REGISTER && c[p].kind <= SHR_CMD_KEEP_RELEASE) p++;
    if (p < n && is_move(d, &c[p])) { /* the leading buffer commands, the moves, then the rest as a batch of its own */
        shr_status st = p ? a.sw.execute(a.sw.user, d, c, p, f) : SHR_OK;
        while (st == SHR_OK && p < n && is_move(d, &c[p])) move_start(d, &c[p++]);
        a.rotate_us += esp_timer_get_time() - t0;
        return st != SHR_OK || p == n ? st : p4_execute(user, d, c + p, n - p, f);
    }
    /* A band drawing only keeps is queued at once. */
    shr_surface ds = *d;
    if (k >= 0) {
        bool cpu = false;
        for (size_t i = 0; i < n; i++) cpu |= c[i].kind < SHR_CMD_KEEP_DRAW || c[i].kind == SHR_CMD_LINE;
        hw.cpu[k] = cpu;
        int s = cpu ? slot_for(k) : k;
        if (cpu) slot_wait(s);
        hw.phys[k] = s, ds.pixels = slot_px(s);
    } else { /* the CPU may read the bands (ROTATE the PPA cannot take): their pixels back where they were drawn */
        hw_drain();
        for (int j = 0; j < 2; j++)
            if (hw.phys[j] != j) memcpy(a.bands[j].pixels, slot_px(hw.phys[j]), a.bands[j].byte_length), hw.phys[j] = j;
    }
    int64_t t1 = esp_timer_get_time();
    a.rotate_us += t1 - t0, t0 = t1;
    if (d->pixels != a.shown) a.shown = NULL;
    for (size_t i = 0; a.shown && i < n; i++) {
        shr_rect r = c[i].dst, *u = &a.dirty;
        if (c[i].kind > SHR_CMD_COPY && c[i].kind != SHR_CMD_KEEP_DRAW && c[i].kind != SHR_CMD_LINE) continue;
        *u = u->x0 >= u->x1 ? r
                            : (shr_rect){r.x0 < u->x0 ? r.x0 : u->x0, r.y0 < u->y0 ? r.y0 : u->y0,
                                         r.x1 > u->x1 ? r.x1 : u->x1, r.y1 > u->y1 ? r.y1 : u->y1};
    }
    shr_status st = a.sw.execute(a.sw.user, &ds, c, n, f);
    if (d->pixels == a.fb.pixels) fb_sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M); /* rotations only invalidate */
    a.draw_us += esp_timer_get_time() - t0;
    return st;
}

static shr_status out_acquire(void *user, shr_surface *s) {
    (void)user;
    *s = a.fb;
    return SHR_OK;
}

static void tail_close(void) {
    if (tail.frame >= 0) mv.tail[tail.frame] = (float)((tail.end ? tail.end : esp_timer_get_time()) - tail.at) / 1000;
    tail.frame = -1;
}

/* The frame buffer is the panel's: the work queued for the frame finishes while the next one is built, the work
 * of the frame before it first. */
static shr_status out_present(void *user, const shr_surface *s, uint64_t id) {
    (void)user, (void)id;
    int64_t t0 = esp_timer_get_time();
    held_flush();
    hw_wait(tail.dma, tail.ppa); /* the hardware runs at most one frame behind */
    tail_close();
    tail.dma = (uint32_t)hw.dma_queued, tail.ppa = (uint32_t)hw.ppa_queued;
    tail.at = esp_timer_get_time(), tail.end = 0;
    tail.frame = a.cur;
    tail_check();
    int64_t t1 = esp_timer_get_time();
    a.rotate_us += t1 - t0, t0 = t1;
    esp_err_t e = a.panel ? esp_lcd_panel_draw_bitmap(a.panel, 0, 0, s->width, s->height, s->pixels)
                          : esp_cache_msync(s->pixels, s->byte_length, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    a.present_us += esp_timer_get_time() - t0;
    a.presents++;
    return e == ESP_OK ? SHR_OK : SHR_E_DEVICE;
}

static void out_discard(void *user, const shr_surface *s) { (void)user, (void)s; }

static uint64_t clock_ns(void *user) { return (void)user, (uint64_t)esp_timer_get_time() * 1000; }

/* vTaskDelay blocks at most the ticks asked for; a spin covers the rest of the last tick. */
static void sleep_until(void *user, uint64_t at_ns) {
    (void)user;
    int64_t t0 = esp_timer_get_time(), at = (int64_t)((at_ns + 999) / 1000);
    if (at - t0 >= 1000) vTaskDelay(pdMS_TO_TICKS((at - t0) / 1000));
    while (esp_timer_get_time() < at) continue;
    a.sleep_us += esp_timer_get_time() - t0;
}

/* Font partitions stay mapped for good: packages are reopened by every scene. */
static struct {
    const esp_partition_t *part;
    const uint8_t *data;
} pkgs[8];

static void map_packages(void) {
    size_t n = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA, FONT_SUBTYPE, NULL);
    for (; it && n < N(pkgs); it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        const void *data;
        esp_partition_mmap_handle_t h;
        if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &data, &h) != ESP_OK) continue;
        pkgs[n].part = p, pkgs[n++].data = data;
        printf("%s: flash 0x%lx, %lu B mapped at %p\n", p->label, (unsigned long)p->address, (unsigned long)p->size, data);
    }
    esp_partition_iterator_release(it);
}

/* Packages are read from their mapped partitions rather than used in place: pages drawn from the page cache in
 * PSRAM beat pages drawn from flash. */
static shr_status read_mapped(void *user, uint64_t offset, uint32_t length, void *dst, uint64_t request) {
    (void)request;
    int64_t t0 = esp_timer_get_time();
    memcpy(dst, (const uint8_t *)user + offset, length);
    a.read_us += esp_timer_get_time() - t0, a.read_bytes += length;
    return SHR_OK;
}

static shr_status open_package(void *user, const char *name, shr_asset_source *out) {
    (void)user;
    size_t len = strcspn(name, ".");
    for (size_t i = 0; i < N(pkgs) && pkgs[i].part; i++) {
        const esp_partition_t *p = pkgs[i].part;
        if (strlen(p->label) != len || memcmp(p->label, name, len)) continue;
        uint64_t size; /* the header's file size; erased flash fails as a bad package */
        memcpy(&size, pkgs[i].data + 32, 8);
        shr_asset_source_init(out);
        out->size = size < p->size ? size : p->size;
        out->user = (void *)pkgs[i].data, out->read = read_mapped;
        return SHR_OK;
    }
    return SHR_E_NOT_FOUND;
}

/* Memory used every frame (SHR_ALLOC_HOT) goes to internal RAM while it fits in the budget, the rest of it and other
 * payload to PSRAM, so the rest of internal RAM stays with the application; DMA memory, hot or not, to DMA-capable
 * PSRAM, other descriptors to malloc. */
#define BUDGET ((size_t)CONFIG_SHIROKO_TAB5_INTERNAL_BUDGET_KB << 10)
static struct {
    size_t used, peak, spilled; /* budgeted bytes, the most since the scene opened; hot bytes in PSRAM */
} budget;

static void *place_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user;
    void *p = NULL;
    align = align < 8 ? 8 : align;
    if ((kind & ~SHR_ALLOC_HOT) == SHR_ALLOC_DMA) { /* whole cache lines */
        align = align < CONFIG_CACHE_L2_CACHE_LINE_SIZE ? CONFIG_CACHE_L2_CACHE_LINE_SIZE : align;
        return heap_caps_aligned_alloc(align, (size + align - 1) / align * align, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    }
    if (kind & SHR_ALLOC_HOT) {
        if (budget.used + size <= BUDGET && (p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))
            budget.used += size, budget.peak = budget.used > budget.peak ? budget.used : budget.peak;
        else if ((p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
            budget.spilled += size;
        return p;
    }
    if (kind == SHR_ALLOC_PAYLOAD) /* also the keep block DMA2D copies to and from: PSRAM, in 128-byte lines */
        return heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return align <= _Alignof(max_align_t) ? malloc(size) : aligned_alloc(align, (size + align - 1) / align * align);
}

static void place_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user, (void)align;
    if (p && (kind & SHR_ALLOC_HOT) && (kind & ~SHR_ALLOC_HOT) != SHR_ALLOC_DMA)
        *(esp_ptr_internal(p) ? &budget.used : &budget.spilled) -= size;
    free(p);
}

#if TAB5_SOAK
/* Allocations and frees by the context (0), the driver (1) and, with hooks, the heap (2), in internal RAM and PSRAM. */
static struct {
    uint32_t allocs[3][2], frees[3][2];
} soak;

static IRAM_ATTR void soak_count(uint32_t (*n)[2], int who, void *p) {
    __atomic_fetch_add(&n[who][!esp_ptr_internal(p)], 1, __ATOMIC_RELAXED);
}

/* user: 1 for the driver. */
static void *soak_alloc(void *user, size_t size, size_t align, shr_alloc_kind kind) {
    void *p = place_alloc(NULL, size, align, kind);
    if (p) soak_count(soak.allocs, (int)(uintptr_t)user, p);
    return p;
}

static void soak_free(void *user, void *p, size_t size, size_t align, shr_alloc_kind kind) {
    soak_count(soak.frees, (int)(uintptr_t)user, p);
    place_free(user, p, size, align, kind);
}

#if CONFIG_HEAP_USE_HOOKS /* IRAM, as esp_heap_caps.h declares them */
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps) {
    (void)size, (void)caps;
    soak_count(soak.allocs, 2, ptr);
}

void esp_heap_trace_free_hook(void *ptr) { soak_count(soak.frees, 2, ptr); }
#endif

static const shr_allocator soak_driver = {(void *)1, soak_alloc, soak_free, SHR_ALLOC_HOT};
#define DRIVER_ALLOCATOR (&soak_driver)
#define PLACE soak_alloc, soak_free

static uint32_t soak_sum(uint32_t (*n)[2], int who) { return n[who][0] + n[who][1]; }

/* `loop`: the counts when loop frame 1 started, `frames` loop frames since. */
static void soak_line(const char *at, const uint32_t (*loop)[3], int frames) {
    const uint32_t caps[2] = {MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM};
    printf("H t=%.1f load=%s at=%s", (double)esp_timer_get_time() / 1e6, load_names[a.s.load], at);
    for (int k = 0; k < 2; k++)
        printf(" %s free %u largest %u min %u", k ? "psram" : "int", (unsigned)heap_caps_get_free_size(caps[k]),
               (unsigned)heap_caps_get_largest_free_block(caps[k]), (unsigned)heap_caps_get_minimum_free_size(caps[k]));
    static const char *const who[3] = {"ctx", "drv", "heap"};
    for (int w = 0; w < 3 && loop; w++)
        printf(" loop-%s %.2f/%.2f", who[w], (double)(soak_sum(soak.allocs, w) - loop[0][w]) / frames,
               (double)(soak_sum(soak.frees, w) - loop[1][w]) / frames);
    for (int w = 0; w < 3; w++)
        printf(" %s %lu/%lu int %lu/%lu", who[w], (unsigned long)soak_sum(soak.allocs, w),
               (unsigned long)soak_sum(soak.frees, w), (unsigned long)soak.allocs[w][0], (unsigned long)soak.frees[w][0]);
    printf("\n");
}

/* `at`: the allocations by context, driver and heap when the last TAB5_SOAK_K loop frames started; `n`: at their end. */
static void soak_zero(const uint32_t *at, const uint32_t *n) {
    static const char *const who[3] = {"ctx", "drv", "heap"};
    bool zero = true;
    printf("Z load=%s last %d frames allocs", load_names[a.s.load], TAB5_SOAK_K);
    for (int w = 0; w < 3; w++) printf(" %s %lu", who[w], (unsigned long)(n[w] - at[w])), zero &= n[w] == at[w];
#if !CONFIG_HEAP_USE_HOOKS
    printf(" (heap: no CONFIG_HEAP_USE_HOOKS)");
#endif
    printf("\n");
    if (zero) return;
    printf("FAIL zero-alloc load=%s:", load_names[a.s.load]);
    for (int w = 0; w < 3; w++)
        if (n[w] != at[w]) printf(" %s %lu", who[w], (unsigned long)(n[w] - at[w]));
    printf("\n");
}
#else
#define DRIVER_ALLOCATOR (&placement)
#define PLACE place_alloc, place_free
#endif
static const shr_allocator placement = {NULL, PLACE, SHR_ALLOC_HOT};

/* ===== Scenes ===== */

static void frame(bool submit) {
    t5_frame(&a.s, submit);
    check(a.s.st, a.s.what);
}

static void heap_line(const char *when) {
    printf("  heap %s: free internal %u KiB (largest %u KiB), PSRAM %u KiB, budget %u/%u/%u KiB used/peak/cap, hot in"
           " PSRAM %u KiB\n", when,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10), (unsigned)(budget.used >> 10),
           (unsigned)(budget.peak >> 10), (unsigned)(BUDGET >> 10), (unsigned)(budget.spilled >> 10));
}

static long page_loads;

static void count_pages(void *user, const shr_trace_event *e) {
    (void)user, page_loads += e->kind == SHR_TRACE_PAGE_READY;
}

/* With `w`, the record pass of the scene into w (rec.h), and its calls into c unless NULL: the clock held, no
 * frame-rate cap, no lines. */
static void scene_open(int load, int mode, rec_writer *w, rec_profile *p, rec_calls *c) {
    a.mode = mode;
    t5_scene_reset(&a.s, load, a.fb.height, a.fb.width);
    a.s.now_ns = clock_ns, a.s.sleep = w ? NULL : sleep_until, a.s.frozen = w ? T5_REC_T0_NS : 0;
    budget.peak = budget.used;
    a.drv.caps.flags = (a.sw.caps.flags & (SHR_DRIVER_CHEAP_STORE | SHR_DRIVER_SCALE)) | /* store: DMA2D copier */
                       (mode != COMPOSE && mv.ok ? SHR_DRIVER_CHEAP_MOVE : 0);
    shr_output output;
    shr_output_init(&output);
    output.acquire = out_acquire, output.present = out_present, output.discard = out_discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    shr_context_desc cd;
    t5_context_desc(&a.s, &cd);
    cd.driver = &a.drv, cd.output = &output, cd.allocator = &placement;
    cd.trace = count_pages, cd.min_frame_interval_ns = w ? 0 : CAP_NS;
    shr_screen_desc sd;
    t5_screen_desc(&a.s, &sd);
    if (mode != COMPOSE) sd.bands = a.bands, sd.band_count = mode == BAND1 ? 1 : 2, sd.band_align = PPA_ALIGN;
    if (w) {
        p->context = &cd, p->screen = &sd;
        const shr_surface targets[3] = {a.fb, a.bands[0], a.bands[1]};
        check(rec_begin(w, p, &a.drv, targets, 3), "rec_begin");
        rec_frame(w, REC_OPEN, a.s.frozen);
        if (c) check(rec_calls_begin(c, p, &a.drv.caps, targets, 3), "rec_calls_begin"), rec_frame(&c->w, REC_OPEN, a.s.frozen);
        cd.driver = &w->drv;
    }
    check(shr_create(&cd, &a.s.ctx), "create");
    check(shr_screen_configure(a.s.ctx, &sd), "screen_configure");
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    check(shr_pl_res_bitmap_font_create(a.s.ctx, &fd, &a.s.font), "font_create");
    page_loads = 0;
    int64_t t0 = esp_timer_get_time();
    t5_preload(&a.s);
    check(a.s.st, a.s.what);
    frame(false);
    if (!w) printf("  preload %ld pages %.2f ms\n", page_loads, (double)(esp_timer_get_time() - t0) / 1000);
    page_loads = 0;
    t5_scene_fill(&a.s);
    check(a.s.st, a.s.what);
    a.read_us = a.read_bytes = 0;
    t0 = esp_timer_get_time();
    frame(true);
    if (!w)
        printf("[landscape %ldx%ld, %ldx%ld cells] %s, %s: first frame %.2f ms, %ld page loads, flash reads %.2f ms"
               " %ld KiB\n", (long)a.s.width, (long)a.s.height, (long)a.s.cols, (long)a.s.rows, load_names[load],
               mode_names[mode], (double)(esp_timer_get_time() - t0) / 1000, page_loads, (double)a.read_us / 1000,
               (long)(a.read_bytes >> 10));
    page_loads = 0, a.read_us = a.read_bytes = 0;
}

static void scene_close(void) {
    t5_scene_close(&a.s);
    check(a.s.st, a.s.what);
}

/* FNV-1a of the frame buffer as the panel shows it. */
static uint32_t fb_checksum(void) {
    hw_drain();
    fb_sync(ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const uint32_t *w = a.fb.pixels;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < a.fb.byte_length / 4; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

static void print_stats(int rep, uint32_t sum, int settle) {
    static const char *const names[7] = {"set_cell", "build", "draw", "rotate", "present", "frame", "busy"};
    const int n = a.s.frames - WARM;
    float warm_max = 0;
    int max1_f = 1, busy_f = 0;
    for (int f = 0; f < WARM; f++) warm_max = a.t[5][f] > warm_max ? a.t[5][f] : warm_max;
    for (int f = 1; f < a.s.frames; f++) {
        max1_f = a.t[5][f] > a.t[5][max1_f] ? f : max1_f;
        busy_f = a.t[6][f] > a.t[6][busy_f] ? f : busy_f;
    }
    const int max_f = a.t[5][0] >= a.t[5][max1_f] ? 0 : max1_f;
    const float max_all = a.t[5][max_f], max1 = a.t[5][max1_f], busy_all = a.t[6][busy_f];
    printf("  R rep=%d load=%s mode=%c frames=%d ms p50/p95/max:", rep, load_names[a.s.load], 'A' + a.mode, n);
    for (int k = 0; k < 7; k++) {
        float *v = a.t[k] + WARM;
        qsort(v, n, sizeof(float), rec_cmp_float);
        printf(" %s %.2f/%.2f/%.2f", names[k], (double)v[n / 2], (double)v[(n * 95 + 99) / 100 - 1], (double)v[n - 1]);
    }
    const float *fr = a.t[5] + WARM; /* sorted */
    int over = 0, transitions = 0;
    for (int f = 0; f < n; f++) over += fr[f] > 33.3f;
    for (int f = 0; f < a.s.frames; f++) transitions += a.t[5][f] > 1.4f * fr[n / 2];
    printf(" warm-max %.2f frame-p99 %.2f over-33.3 %d", (double)warm_max, (double)fr[(n * 99 + 99) / 100 - 1], over);
    printf(" fb %08lx max %.2f max-frame %d max1 %.2f max1-frame %d%s", (unsigned long)sum, (double)max_all, max_f,
           (double)max1, max1_f, a.s.resource_failed ? " (fallback glyphs: packages missing)" : "");
    printf(" busy-max-all %.2f busy-max-frame %d settle %d transitions %d flash-reads %.2f ms %ld KiB KB/frame dma %.0f cpu"
           " %.0f rot %.0f", (double)busy_all, busy_f, settle, transitions, (double)a.read_us / 1000,
           (long)(a.read_bytes >> 10), (double)a.loop.copied[0] / n / 1024, (double)a.loop.copied[1] / n / 1024,
           (double)a.loop.rotated / n / 1024);
    printf(" cap %d fps %.1f\n", FPS_CAP, n * 1e6 / (double)a.wall_us);
    a.stats[rep][a.s.load][a.mode] =
        (scene_stats){fr[n / 2], fr[(n * 95 + 99) / 100 - 1], max_all, max1, busy_all, transitions, settle};
}

/* Median and largest of each scene's numbers over its reps. */
static void print_summary(int load, int mode) {
    float v[5][REPS];
    int transitions = 0, settle = 0;
    for (int rep = 0; rep < REPS; rep++) {
        const scene_stats *st = &a.stats[rep][load][mode];
        v[0][rep] = st->p50, v[1][rep] = st->p95, v[2][rep] = st->max, v[3][rep] = st->max1, v[4][rep] = st->busy;
        transitions = st->transitions > transitions ? st->transitions : transitions;
        settle = st->settle > settle ? st->settle : settle;
    }
    printf("  S load=%s mode=%c reps=%d ms median/largest:", load_names[load], 'A' + mode, REPS);
    static const char *const names[5] = {"p50", "p95", "max", "max1", "busy-max-all"};
    for (int k = 0; k < 5; k++) {
        qsort(v[k], REPS, sizeof(float), rec_cmp_float);
        printf(" %s %.2f/%.2f", names[k], (double)v[k][REPS / 2], (double)v[k][REPS - 1]);
    }
    printf(" transitions-largest %d settle-largest %d\n", transitions, settle);
}

/* Over the measured frames: the waits for moves, and the PSRAM traffic of moves (read and write), keep copies and
 * rotations (CPU reads of glyph pages are not counted) at the rate frames ran and at 30 frames a second. */
static void print_moves(int rep) {
    const int n = a.s.frames - WARM;
    float m[SCROLL_FRAMES], tl[SCROLL_FRAMES];
    for (int f = WARM; f < a.s.frames; f++) m[f - WARM] = mv.t[f], tl[f - WARM] = mv.tail[f];
    const traffic *b = &a.loop;
    double fps = n * 1e6 / (double)a.wall_us, mb = (double)(b->moved + b->copied[0] + b->copied[1] + b->rotated) / n / 1e6;
    qsort(m, (size_t)n, sizeof(float), rec_cmp_float);
    qsort(tl, (size_t)n, sizeof(float), rec_cmp_float);
    printf("  M rep=%d load=%s mode=%c moves %s wait p50/p95/max %.2f/%.2f/%.2f tail p50/max %.2f/%.2f"
           " psram MB/frame move %.2f keep %.2f rot %.2f"
           " total %.2f, %.0f MB/s at %.1f fps, %.0f MB/s at 30 fps, steps %d\n",
           rep, load_names[a.s.load], 'A' + a.mode, a.drv.caps.flags & SHR_DRIVER_CHEAP_MOVE ? "on" : "off",
           (double)m[n / 2], (double)m[(n * 95 + 99) / 100 - 1], (double)m[n - 1], (double)tl[n / 2], (double)tl[n - 1],
           (double)b->moved / n / 1e6, (double)(b->copied[0] + b->copied[1]) / n / 1e6, (double)b->rotated / n / 1e6, mb,
           mb * fps, fps, mb * (fps < 30 ? fps : 30), TAB5_STEPS);
}

static void run_scene(int rep, int load, int mode) {
    scene_open(load, mode, NULL, NULL, NULL);
#if TAB5_SOAK
    uint32_t loop[2][3] = {{0}}, last[2][3] = {{0}};
#endif
    int64_t wall0 = 0;
    for (long f = 0; f < a.s.frames; f++) {
        int64_t t0 = esp_timer_get_time();
        if (f == WARM) a.bytes = (traffic){0}, wall0 = t0;
#if TAB5_SOAK
        if (f == 1)
            for (int w = 0; w < 3; w++) loop[0][w] = soak_sum(soak.allocs, w), loop[1][w] = soak_sum(soak.frees, w);
        for (int w = 0; w < 3 && f == a.s.frames - TAB5_SOAK_K; w++) last[0][w] = soak_sum(soak.allocs, w);
#endif
        bool submit = load_step(&a.s);
        check(a.s.st, a.s.what);
        int64_t t1 = esp_timer_get_time();
        a.present_us = a.draw_us = a.rotate_us = a.wait_us = a.sleep_us = mv.wait_us = 0;
        a.cur = (int)f, mv.tail[f] = 0;
        frame(submit);
        int64_t t2 = esp_timer_get_time();
        a.t[0][f] = (float)(t1 - t0) / 1000;
        a.t[1][f] = (float)(t2 - t1 - a.present_us - a.draw_us - a.rotate_us - a.sleep_us) / 1000;
        a.t[2][f] = (float)a.draw_us / 1000, a.t[3][f] = (float)a.rotate_us / 1000;
        a.t[4][f] = (float)a.present_us / 1000, a.t[5][f] = (float)(t2 - t0) / 1000;
        a.t[6][f] = (float)(t2 - t0 - a.wait_us - a.sleep_us) / 1000;
        mv.t[f] = (float)mv.wait_us / 1000;
    }
    a.wall_us = esp_timer_get_time() - wall0, a.loop = a.bytes;
#if TAB5_SOAK
    for (int w = 0; w < 3; w++) last[1][w] = soak_sum(soak.allocs, w);
#endif
    hw_drain();
    tail_close();
    a.cur = -1;
    int settle = 1; /* the blink scene changes with the clock alone: frame(false) always draws its next phase */
    for (uint32_t shown = a.presents; a.s.load != BLINK && settle < SETTLE_MAX; settle++, shown = a.presents) {
        frame(false);
        if (a.presents == shown) break;
    }
    t5_hold_last_phase(&a.s, CAP_NS);
    check(a.s.st, a.s.what);
    uint32_t sum = fb_checksum();
    a.sums[rep][load][mode] = sum;
    print_moves(rep);
    print_stats(rep, sum, settle);
    printf("  loop page loads %ld\n", page_loads);
    heap_line("at the last frame");
#if TAB5_SOAK
    soak_line("end", (const uint32_t (*)[3])loop, a.s.frames - 1);
    soak_zero(last[0], last[1]);
    scene_close();
    soak_line("closed", NULL, 0);
#else
    scene_close();
#endif
}

#if TAB5_REPLAY
/* ===== Replay: each scene recorded, then its batches replayed into the software driver alone (CPU rotation, memcpy
 * keeps) and into this driver, REPLAY_REPS times each in alternating order; then recorded again with its calls, which
 * are replayed into the compositor over a driver drawing nothing ===== */

#define REPLAY_REPS 5
#define ARENA_MAX (16u << 20)
#define HASH_BYTES (4u << 20) /* the largest frame of commands, hashed only (rec.h) */
#define SCRATCH_BYTES (16u << 10)

static struct {
    rec_writer w;
    uint8_t *arena;
    size_t cap;
    rec_package pk[T5_N(t5_packages)];
} rp;

static uint32_t rp_checksum(void *user) { return (void)user, fb_checksum(); }

static void rp_idle(void *user) { (void)user, vTaskDelay(1); }

static uint64_t rp_waited(void *user) { return (void)user, (uint64_t)a.wait_us * 1000; }

static void rp_finish(void *user) {
    (void)user;
    hw_drain();
}

/* As out_present: the CPU's frame buffer writes reach PSRAM before the next frame's DMA reads them. */
static void rp_settle(void *user) {
    fb_sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    rp_idle(user);
}

static void *rp_alloc(void *user, size_t bytes) { return (void)user, place_alloc(NULL, bytes, 128, SHR_ALLOC_PAYLOAD); }

static void rp_free(void *user, void *p) { (void)user, free(p); }

/* Hashes the packages for the recording header and takes the recording's PSRAM while most of it is free. */
static void replay_setup(void) {
    for (size_t i = 0; i < T5_N(t5_packages); i++) {
        shr_asset_source src;
        bool found = open_package(NULL, t5_packages[i], &src) == SHR_OK;
        rp.pk[i] = (rec_package){t5_packages[i], found ? src.size : 0,
                                 found ? rec_hash(REC_HASH_INIT, src.user, (size_t)src.size) : 0};
    }
    for (rp.cap = ARENA_MAX; rp.cap >= 1u << 20 && !(rp.arena = heap_caps_malloc(rp.cap, MALLOC_CAP_SPIRAM)); rp.cap -= 1u << 20)
        continue;
    printf("replay: recording %zu KiB in PSRAM, %d reps of parts 0x%x (driver-sw, driver-p4, compositor)\n",
           rp.arena ? rp.cap >> 10 : 0, REPLAY_REPS, TAB5_REPLAY_PARTS);
    heap_line("with the recording");
}

static void *internal_or_psram(size_t bytes, bool *internal) {
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *internal = p;
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

static void replay_part(int p4, bool verify, rec_times *t, shr_draw_cmd *cmds, uint32_t *sum, int32_t *bad) {
    check(shr_software_driver_set_copier(&a.sw, p4 ? &p4_copier : NULL), "copier");
    a.shown = NULL, a.dirty = (shr_rect){0};
    memset(a.fb.pixels, 0x5A, a.fb.byte_length);
    fb_sync(ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    for (int k = 0; k < 2; k++) memset(a.bands[k].pixels, 0x5A, a.bands[k].byte_length);
    const shr_surface targets[3] = {a.fb, a.bands[0], a.bands[1]};
    rec_host h = {p4 ? &a.drv : &a.sw, targets, 3, cmds, rp_alloc, rp_free, clock_ns, rp_waited,
                  p4 ? rp_finish : NULL, rp_settle, rp_checksum, NULL};
    check(rec_play(rp.arena, rp.w.len, &h, verify, t, sum, bad), "replay");
}

/* The command recording, replayed into the drivers TAB5_REPLAY_PARTS names; its hashes per frame go to ref. */
static void replay_drivers(int load, uint32_t *ref) {
    static const char *const parts[2] = {"driver-sw", "driver-p4"};
    rec_profile p = {load_names[load], NULL, NULL, T5_REC_STEP_NS, t5_rec_frames(load), rp.pk, T5_N(rp.pk)};
    rp.w = (rec_writer){.buf = rp.arena, .cap = rp.cap};
    scene_open(load, BAND2, &rp.w, &p, NULL);
    uint32_t pass = t5_record_loop(&a.s, &rp.w, NULL, rp_checksum, rp_idle, NULL);
    check(a.s.st, a.s.what);
    scene_close();
    check(rp.w.st, "recording");
    check(rec_end(&rp.w), "rec_end");
    rec_info in;
    check(rec_scan(rp.arena, rp.w.len, &in), "rec_scan");
    rec_print_pass(&rp.w, &in, pass);
    for (uint32_t k = 0; k < in.frames; k++) ref[k] = rec_frame_hash(&in, k);
    bool internal;
    shr_draw_cmd *cmds = internal_or_psram((in.max_batch + 1u) * sizeof(shr_draw_cmd), &internal);
    rec_times *t = heap_caps_malloc(in.frames * sizeof(*t), MALLOC_CAP_SPIRAM);
    if (!cmds || !t) check(SHR_E_NO_MEMORY, "replay");
    uint32_t record = rec_sum(&in, in.frames - 1), sums[2] = {0};
    int32_t bad[2] = {-2, -2};
    bool same = true;
    for (int k = 0; k < 2; k++) {
        if (!(TAB5_REPLAY_PARTS >> k & 1)) continue;
        replay_part(k, true, t, cmds, &sums[k], &bad[k]);
        rec_print_diff(&in, parts[k], t);
        same &= sums[k] == record && bad[k] == -2;
    }
    rec_stats st[2][REPLAY_REPS];
    for (int rep = 0; rep < REPLAY_REPS && (TAB5_REPLAY_PARTS & 3); rep++)
        for (int j = 0; j < 2; j++) {
            int k = rep & 1 ? 1 - j : j;
            int32_t b;
            uint32_t sum;
            if (!(TAB5_REPLAY_PARTS >> k & 1)) continue;
            replay_part(k, false, t, cmds, &sum, &b);
            same &= sum == record;
            st[k][rep] = rec_print(rep, in.scene, parts[k], rec_driver_cols, REC_COLS, t, in.frames - 1, sum, sum == record,
                                   in.hash, TAB5_REPLAY_FRAMES);
        }
    for (int k = 0; k < 2; k++)
        if (TAB5_REPLAY_PARTS >> k & 1) rec_print_summary(in.scene, parts[k], st[k], REPLAY_REPS);
    if (TAB5_REPLAY_PARTS & 3)
        printf("REPLAY CHECKSUMS rec=%s record %08lx sw %08lx p4 %08lx pass %08lx scene %08lx rec-hash %08lx first-bad %d"
               " %d commands %s -> %s\n", in.scene, (unsigned long)record, (unsigned long)sums[0], (unsigned long)sums[1],
               (unsigned long)pass, (unsigned long)a.sums[0][load][MODE0], (unsigned long)in.hash, (int)bad[0],
               (int)bad[1], internal ? "internal" : "PSRAM", same ? "EQUAL" : "DIFFER");
    check(shr_software_driver_set_copier(&a.sw, &p4_copier), "copier");
    free(cmds), free(t);
    rec_free(&rp.w);
}

/* Records the scene's calls (the commands hashed alongside) and replays them into the compositor, whose commands must
 * hash as `ref`. */
static void replay_calls(int load, const uint32_t *ref) {
    rec_profile p = {load_names[load], NULL, NULL, T5_REC_STEP_NS, t5_rec_frames(load), rp.pk, T5_N(rp.pk)};
    rec_calls c = {.w = {.buf = rp.arena, .cap = rp.cap - HASH_BYTES}};
    rec_writer v = {.buf = rp.arena + rp.cap - HASH_BYTES, .cap = HASH_BYTES, .hash_only = true};
    scene_open(load, BAND2, &v, &p, &c);
    uint32_t pass = t5_record_loop(&a.s, &v, &c.w, rp_checksum, rp_idle, NULL);
    check(a.s.st, a.s.what);
    check(c.w.st, "call recording");
    check(rec_calls_end(&c), "rec_calls_end");
    scene_close();
    check(v.st, "recording");
    uint32_t pass2 = v.hash;
    size_t len = c.w.len;
    rec_free(&v);
    rec_info in;
    check(rec_scan(rp.arena, len, &in), "rec_scan");
    printf("  RC rec=%s frames=%u/%u pass %08lx bytes %.1f MiB objects %u styles %u calls %llu rec %08lx cmd-hash"
           " %08lx%s\n", in.scene, (unsigned)in.frames - 1, (unsigned)c.w.limit, (unsigned long)pass, (double)len / 1048576,
           (unsigned)in.mems, (unsigned)in.max_batch, (unsigned long long)in.cmds, (unsigned long)in.hash,
           (unsigned long)pass2, c.w.full || v.full ? " FULL" : "");
    rec_calls_free(&c);
    bool internal;
    void *scratch = internal_or_psram(SCRATCH_BYTES, &internal);
    rec_times *t = heap_caps_malloc(2 * in.frames * sizeof(*t), MALLOC_CAP_SPIRAM);
    if (!scratch || !t) check(SHR_E_NO_MEMORY, "replay");
    const shr_surface targets[3] = {a.fb, a.bands[0], a.bands[1]};
    rec_calls_host h = {&a.drv, targets, 3, &placement, open_package, clock_ns, rp_idle, NULL, scratch, SCRATCH_BYTES};
    v = (rec_writer){.buf = rp.arena + rp.cap - HASH_BYTES, .cap = HASH_BYTES, .hash_only = true};
    check(rec_calls_play(rp.arena, len, &h, &v, t, t + in.frames), "replay calls");
    int32_t bad = -2;
    for (uint32_t k = 0; k < v.done && bad == -2; k++)
        if (v.hashes[k] != ref[k]) bad = (int32_t)k - 1;
    if (v.done != in.frames && bad == -2) bad = (int32_t)v.done - 1;
    uint32_t hash = v.hash;
    size_t peak = v.peak;
    bool same = hash == ref[in.frames - 1] && pass2 == hash && bad == -2 && !v.full && v.st == SHR_OK;
    rec_free(&v);
    rec_stats st[2][REPLAY_REPS];
    for (int rep = 0; rep < REPLAY_REPS; rep++) {
        check(rec_calls_play(rp.arena, len, &h, NULL, t, t + in.frames), "replay calls");
        st[0][rep] = rec_print(rep, in.scene, "compositor", rec_compositor_cols, REC_COMPOSITOR_COLS, t, in.frames - 1,
                               hash, same, in.hash, TAB5_REPLAY_FRAMES);
        st[1][rep] = rec_print(rep, in.scene, "api", rec_api_cols, REC_API_COLS, t + in.frames, in.frames - 1, hash, same,
                               in.hash, TAB5_REPLAY_FRAMES);
    }
    rec_print_summary(in.scene, "compositor", st[0], REPLAY_REPS);
    rec_print_summary(in.scene, "api", st[1], REPLAY_REPS);
    printf("REPLAY CALLS rec=%s calls-hash %08lx cmd-hash record %08lx pass %08lx replay %08lx first-bad %d peak %zu KiB"
           " scratch %s -> %s\n", in.scene, (unsigned long)in.hash, (unsigned long)ref[in.frames - 1], (unsigned long)pass2,
           (unsigned long)hash, (int)bad, peak >> 10, internal ? "internal" : "PSRAM", same ? "EQUAL" : "DIFFER");
    free(scratch), free(t);
}

static void replay_scene(int load) {
    uint32_t ref[T5_FRAMES + 1];
    if (!rp.arena) return;
    replay_drivers(load, ref);
    if (!(TAB5_REPLAY_PARTS & 4)) return;
    if (rp.cap > HASH_BYTES) replay_calls(load, ref);
    else printf("  calls of %s not replayed: %zu KiB of recording memory\n", load_names[load], rp.cap >> 10);
}
#endif

#if CONFIG_SHIROKO_TAB5_BOARD_HEADLESS
/* The frame buffer as the DPI driver allocates it; a scan-out streams it to the empty DSI connector, no panel set up. */
static void *headless_fb(void) {
#if CONFIG_SHIROKO_TAB5_SCANOUT_NONE
    size_t bytes = (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2;
    void *fb = heap_caps_aligned_calloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!fb) check(SHR_E_NO_MEMORY, "frame buffer");
    ESP_ERROR_CHECK(esp_cache_msync(fb, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M));
#else
#if CONFIG_SHIROKO_TAB5_SCANOUT_ILI9881C
    esp_lcd_dpi_panel_config_t pc = {.dpi_clock_freq_mhz = 60,
                                     .video_timing = {.hsync_back_porch = 140, .hsync_pulse_width = 40, .hsync_front_porch = 40,
                                                      .vsync_back_porch = 20, .vsync_pulse_width = 4, .vsync_front_porch = 20}};
#else
    esp_lcd_dpi_panel_config_t pc = {.dpi_clock_freq_mhz = 70,
                                     .video_timing = {.hsync_back_porch = 40, .hsync_pulse_width = 2, .hsync_front_porch = 40,
                                                      .vsync_back_porch = 24, .vsync_pulse_width = 20, .vsync_front_porch = 200}};
#endif
    pc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, pc.in_color_format = LCD_COLOR_FMT_RGB565, pc.num_fbs = 1;
    pc.video_timing.h_size = BSP_LCD_H_RES, pc.video_timing.v_size = BSP_LCD_V_RES;
    esp_ldo_channel_handle_t ldo;
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(
        &(esp_ldo_channel_config_t){.chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN, .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV}, &ldo));
    esp_lcd_dsi_bus_handle_t bus;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&(esp_lcd_dsi_bus_config_t){.bus_id = 0, .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
                                                                    .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS},
                                        &bus));
    ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(bus, &pc, &a.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(a.panel));
    void *fb;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(a.panel, 1, &fb));
#endif
    return fb;
}
#endif

void app_main(void) {
    a.task = xTaskGetCurrentTaskHandle();
#if CONFIG_ESP_TASK_WDT_INIT /* loop frames run back to back on this core: its idle task may not run for seconds */
    (void)esp_task_wdt_reconfigure(&(esp_task_wdt_config_t){
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = ((1u << portNUM_PROCESSORS) - 1) & ~(1u << xPortGetCoreID())});
#endif
    /* Mid-scene the largest free internal block is a few KiB: the bands are taken first. */
    size_t band_bytes = (size_t)BSP_LCD_V_RES * 2 * BAND_H;
    uint8_t *band_px =
        heap_caps_aligned_alloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, 2 * band_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!band_px) check(SHR_E_NO_MEMORY, "bands");
    for (int k = 0; k < 2; k++)
        a.bands[k] = (shr_surface){band_px + k * band_bytes, BSP_LCD_V_RES, BAND_H, BSP_LCD_V_RES * 2, band_bytes,
                                   SHR_FORMAT_RGB565, 1, SHR_MEMORY_CPU, 0};
#if CONFIG_SHIROKO_TAB5_BOARD_TAB5
    bsp_display_config_t dc = {.dsi_bus = {.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS}};
    bsp_lcd_handles_t lcd;
    ESP_ERROR_CHECK(bsp_display_new_with_handles(&dc, &lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd.panel, true));
    void *fb;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(lcd.panel, 1, &fb));
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    a.panel = lcd.panel;
#else
    void *fb = headless_fb();
#endif
    size_t row = BSP_LCD_H_RES * 2;
    a.fb = (shr_surface){fb, BSP_LCD_H_RES, BSP_LCD_V_RES, row, row * BSP_LCD_V_RES, SHR_FORMAT_RGB565, 0, SHR_MEMORY_CPU, 0};
    /* Keeps for the rows of CONFIG_SHIROKO_TAB5_KEEP_SCREENS screens, each in a slot of one logical row (40960 B);
     * PSRAM, as malloc places them. */
    uint32_t keeps = CONFIG_SHIROKO_TAB5_KEEP_SCREENS * (BSP_LCD_H_RES / CH);
    check(shr_software_driver_create(DRIVER_ALLOCATOR, keeps * (BSP_LCD_V_RES * CH * 2ull), keeps, 256, &a.sw), "driver");
    check(shr_software_driver_image_planes(&a.sw, 2u << 20), "planes"); /* twice the scenes' image_bytes */
    async_color_convert_config_t cc = {.backlog = DMA_QUEUE, .dma_burst_size = DMA_BURST};
    ESP_ERROR_CHECK(esp_async_color_convert_install_dma2d(&cc, &hw.conv));
    ESP_ERROR_CHECK(esp_timer_create(&(esp_timer_create_args_t){.callback = dma_paced, .name = "dma pace"}, &hw.pace));
    hw.dq = heap_caps_malloc(CHAIN * sizeof(dma_op), MALLOC_CAP_SPIRAM), hw.pq = heap_caps_malloc(CHAIN * sizeof(ppa_op), MALLOC_CAP_SPIRAM);
    if (!hw.dq || !hw.pq) check(SHR_E_NO_MEMORY, "queues");
    for (int i = 0; i < SPARES; i++)
        if (!(hw.spare[i] = heap_caps_aligned_alloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, band_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)))
            check(SHR_E_NO_MEMORY, "spare bands");
    if (xTaskCreatePinnedToCore(worker, "hw", 4096, NULL, 5, &hw.worker, xPortGetCoreID()) != pdPASS)
        check(SHR_E_NO_MEMORY, "worker");
    check(shr_software_driver_set_copier(&a.sw, &p4_copier), "copier");
    a.drv = a.sw, a.drv.execute = p4_execute;
    ESP_ERROR_CHECK(ppa_register_client(&(ppa_client_config_t){.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = PPA_QUEUE},
                                        &a.ppa));
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(a.ppa, &(ppa_event_callbacks_t){.on_trans_done = ppa_finished}));
    printf("shiroko on " BOARD ": %dx%d RGB565, cells %dx%d, bands 2 x %zu B internal at %p, frame-rate cap %d,"
           " keeps %d screens\n", BSP_LCD_H_RES, BSP_LCD_V_RES, CW, CH, band_bytes, (void *)band_px, FPS_CAP,
           CONFIG_SHIROKO_TAB5_KEEP_SCREENS);
    heap_line("at boot");
    move_setup();
    map_packages();
#if TAB5_REPLAY
    replay_setup();
#endif
    for (long round = 0;; round++) {
        for (int rep = 0; rep < REPS; rep++)
            for (int load = 0; load < LOADS; load++)
                for (int m = MODE0; (TAB5_LOADS >> load & 1) && m < MODES; m++)
                    run_scene(rep, load, rep & 1 ? MODES - 1 - m + MODE0 : m);
#if TAB5_REPLAY
        for (int load = 0; load < LOADS; load++)
            if (TAB5_LOADS >> load & 1) replay_scene(load);
#endif
        for (int load = 0; load < LOADS; load++)
            for (int m = MODE0; (TAB5_LOADS >> load & 1) && m < MODES; m++) print_summary(load, m);
        printf("CHECKSUMS round %ld:", round);
        bool same = true;
        for (int load = 0; load < LOADS; load++) {
            if (!(TAB5_LOADS >> load & 1)) continue;
            printf(" %s", load_names[load]);
            for (int rep = 0; rep < REPS; rep++)
                for (int m = MODE0; m < MODES; m++) {
                    printf(" %08lx", (unsigned long)a.sums[rep][load][m]);
                    same &= a.sums[rep][load][m] == a.sums[0][load][MODE0];
                }
        }
        printf(" -> %s\n", same ? "ALL EQUAL" : "DIFFER");
    }
}
