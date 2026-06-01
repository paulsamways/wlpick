/* wayland-color-picker – minimal Wayland color picker.
 *
 * Three-phase flow:
 *   capture   – capture all connected outputs once before the overlay exists.
 *   pick      – show a fullscreen overlay on every output; track pointer.
 *   clipboard – write #RRGGBB to clipboard; wait for selection to be replaced.
 *
 * Multi-output design
 * -------------------
 * One Output struct is created for each wl_output the compositor advertises.
 * Each output gets its own capture session + pixel buffer, and its own
 * zwlr_layer_surface_v1.  wl_pointer.enter tells us which surface the cursor
 * entered, so we always know which output's pixels to sample – no global
 * coordinate math is required.
 */

/* _GNU_SOURCE is set via the compiler command line (-D_GNU_SOURCE). */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

/* Generated protocol headers (produced by wayland-scanner at build time). */
#include "build/ext-image-capture-source-v1-protocol.h"
#include "build/ext-image-copy-capture-v1-protocol.h"
#include "build/wlr-layer-shell-unstable-v1-protocol.h"

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define PREVIEW_SIZE 100   /* side length of the color preview square, px */
#define CURSOR_SIZE  10    /* side length of the hollow-square cursor, px  */
#define CURSOR_HOT   5     /* hotspot offset from top-left corner          */
#define MAX_OUTPUTS  8     /* maximum number of simultaneously tracked outputs */

/* -------------------------------------------------------------------------
 * Phase
 * ---------------------------------------------------------------------- */

typedef enum {
    PHASE_CAPTURE,   /* waiting for all output captures to complete        */
    PHASE_PICK,      /* overlays shown; tracking pointer                  */
    PHASE_CLIPBOARD, /* color written to clipboard; waiting for cancel     */
    PHASE_DONE,      /* error or clipboard replaced; time to exit          */
} Phase;

/* Forward declaration so Output can hold an App pointer. */
typedef struct App App;

/* -------------------------------------------------------------------------
 * Shared-memory helpers
 * ---------------------------------------------------------------------- */

/* Create an anonymous shared-memory file of the given size and return
 * its fd.  The file is already unlinked from the filesystem.            */
static int shm_alloc(size_t size)
{
    int fd = -1;

#ifdef __linux__
    fd = memfd_create("wayland-color-picker-shm", MFD_CLOEXEC);
#endif
    if (fd < 0) {
        /* Fall back to shm_open + unlink on other POSIX systems. */
        char name[64];
        snprintf(name, sizeof(name), "/wcp-%d", (int)getpid());
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            (void)shm_unlink(name);
        }
    }

    if (fd < 0) {
        perror("shm_alloc");
        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}

/* Map a wl_buffer backed by shared memory.
 * Returns the mmap'd pointer (must be munmap'd by the caller) or NULL.  */
static void *create_shm_buffer(struct wl_shm *shm,
                               uint32_t width, uint32_t height,
                               struct wl_buffer **buf_out)
{
    size_t stride = (size_t)width * 4U;
    size_t size   = stride * (size_t)height;

    int fd = shm_alloc(size);
    if (fd < 0) {
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    close(fd); /* pool holds the fd; we no longer need it */

    *buf_out = wl_shm_pool_create_buffer(pool, 0,
                                          (int32_t)width, (int32_t)height,
                                          (int32_t)stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    return data;
}

/* -------------------------------------------------------------------------
 * Per-output state
 * ---------------------------------------------------------------------- */

typedef struct {
    App *app;                       /* back-pointer (set at bind time)    */

    struct wl_output *wl_output;

    /* ── Capture ───────────────────────────────────────────────────────── */
    struct ext_image_capture_source_v1       *capture_source;
    struct ext_image_copy_capture_session_v1 *capture_session;
    struct ext_image_copy_capture_frame_v1   *capture_frame;
    uint32_t  cap_width;
    uint32_t  cap_height;
    uint8_t  *pixels;               /* ARGB8888 captured screen pixels    */
    size_t    pixels_size;
    struct wl_buffer *capture_buf;

    /* ── Overlay (double-buffered) ─────────────────────────────────────── */
    struct wl_surface            *overlay_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t  ov_width;
    uint32_t  ov_height;
    size_t    ov_pixels_size;   /* bytes per buffer (both same size)      */
    /* Two buffers: we write into [back] while compositor reads [front].  */
    uint32_t        *ov_pixels[2];
    struct wl_buffer *ov_buf[2];
    int               ov_back;  /* index of the buffer we write into (0|1) */
    int               ov_busy[2]; /* 1 = compositor still holds the buffer */
} Output;

/* -------------------------------------------------------------------------
 * App state
 * ---------------------------------------------------------------------- */

struct App {
    /* ── Core Wayland globals ──────────────────────────────────────────── */
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct wl_seat       *seat;
    struct wl_pointer    *pointer;
    struct wl_data_device_manager *ddm;
    struct wl_data_device         *data_device;
    struct wl_data_source         *data_source;

    /* ── Extension protocol globals ────────────────────────────────────── */
    struct ext_output_image_capture_source_manager_v1 *capture_src_mgr;
    struct ext_image_copy_capture_manager_v1           *capture_mgr;
    struct zwlr_layer_shell_v1                         *layer_shell;

    /* ── Outputs ───────────────────────────────────────────────────────── */
    Output outputs[MAX_OUTPUTS];
    int    output_count;
    int    captures_pending;    /* decremented per frame_ready; starts overlays at 0 */
    int    overlays_configured; /* incremented per configure; phase→PICK at output_count */

    /* Output the pointer is currently on (updated in pointer_enter/leave). */
    Output *active_output;

    /* ── Custom cursor (shared across all outputs) ─────────────────────── */
    struct wl_surface *cursor_surface;
    uint32_t *cursor_pixels;
    size_t    cursor_pixels_size;
    struct wl_buffer *cursor_buf;

    /* ── Pointer state ─────────────────────────────────────────────────── */
    uint32_t pointer_serial;
    int32_t  pointer_x;
    int32_t  pointer_y;

    /* ── Sampled color ─────────────────────────────────────────────────── */
    uint32_t current_color;
    char     color_str[8];          /* "#RRGGBB\0"                         */

    /* ── Control flow ──────────────────────────────────────────────────── */
    Phase phase;
};

/* -------------------------------------------------------------------------
 * Drawing helpers
 * ---------------------------------------------------------------------- */

/* Called by the compositor when it has finished reading a buffer. */
static void ov_buffer_release(void *data, struct wl_buffer *buf)
{
    Output *out = data;
    for (int i = 0; i < 2; i++) {
        if (out->ov_buf[i] == buf) {
            out->ov_busy[i] = 0;
            break;
        }
    }
}

static const struct wl_buffer_listener ov_buffer_listener = {
    .release = ov_buffer_release,
};

static void draw_overlay(Output *out)
{
    uint32_t *pixels = out->ov_pixels[out->ov_back];
    if (!pixels) {
        return;
    }

    App *app = out->app;
    uint32_t ow = out->ov_width;
    uint32_t oh = out->ov_height;

    /* Fully transparent background. */
    memset(pixels, 0, out->ov_pixels_size);

    /* Draw the color preview square next to the cursor.
     * Prefer right+below; clamp to the surface edge.     */
    int32_t px = app->pointer_x + CURSOR_SIZE + 4;
    int32_t py = app->pointer_y + CURSOR_SIZE + 4;

    if (px + PREVIEW_SIZE > (int32_t)ow) {
        px = app->pointer_x - PREVIEW_SIZE - 4;
    }
    if (py + PREVIEW_SIZE > (int32_t)oh) {
        py = app->pointer_y - PREVIEW_SIZE - 4;
    }

    uint32_t color = (0xFFU << 24U) |
                     (((app->current_color >> 16U) & 0xFFU) << 16U) |
                     (((app->current_color >>  8U) & 0xFFU) <<  8U) |
                      ((app->current_color        ) & 0xFFU);

    for (int32_t dy = 0; dy < PREVIEW_SIZE; dy++) {
        int32_t row = py + dy;
        if (row < 0 || row >= (int32_t)oh) {
            continue;
        }
        for (int32_t dx = 0; dx < PREVIEW_SIZE; dx++) {
            int32_t col = px + dx;
            if (col < 0 || col >= (int32_t)ow) {
                continue;
            }
            pixels[((size_t)row * (size_t)ow) + (size_t)col] = color;
        }
    }
}

static void commit_overlay(Output *out)
{
    /* Pick the back buffer.  If it's still held by the compositor, try the
     * other one.  If both are busy, skip this frame rather than tearing.  */
    if (out->ov_busy[out->ov_back]) {
        int alt = out->ov_back ^ 1;
        if (out->ov_busy[alt]) {
            return; /* both busy; compositor will release soon */
        }
        out->ov_back = alt;
    }

    draw_overlay(out);

    int b = out->ov_back;
    out->ov_busy[b] = 1;
    out->ov_back    = b ^ 1; /* next write goes to the other buffer */

    wl_surface_attach(out->overlay_surface, out->ov_buf[b], 0, 0);
    wl_surface_damage_buffer(out->overlay_surface,
                             0, 0,
                             (int32_t)out->ov_width,
                             (int32_t)out->ov_height);
    wl_surface_commit(out->overlay_surface);
    wl_display_flush(out->app->display);
}

static void draw_cursor(App *app)
{
    /* Draw a CURSOR_SIZE × CURSOR_SIZE hollow white square, 1px border. */
    for (int cy = 0; cy < CURSOR_SIZE; cy++) {
        for (int cx = 0; cx < CURSOR_SIZE; cx++) {
            int border = (cx == 0 || cx == CURSOR_SIZE - 1 ||
                          cy == 0 || cy == CURSOR_SIZE - 1);
            app->cursor_pixels[((size_t)cy * (size_t)CURSOR_SIZE) + (size_t)cx] =
                border ? 0xFFFFFFFFU : 0x00000000U;
        }
    }
}

/* -------------------------------------------------------------------------
 * Sample the color under the pointer on the active output.
 * ---------------------------------------------------------------------- */
static void sample_color(App *app)
{
    Output *out = app->active_output;
    if (!out || !out->pixels || !out->ov_width || !out->ov_height) {
        return;
    }

    int32_t px = app->pointer_x;
    int32_t py = app->pointer_y;

    /* Pointer coordinates are in logical (surface-local) pixels.
     * The capture buffer is at physical pixel resolution.
     * Scale using the cap/overlay ratio to handle HiDPI outputs.        */
    if (px < 0 || py < 0 ||
        (uint32_t)px >= out->ov_width ||
        (uint32_t)py >= out->ov_height) {
        return;
    }

    size_t phys_x = ((size_t)px * (size_t)out->cap_width)  / (size_t)out->ov_width;
    size_t phys_y = ((size_t)py * (size_t)out->cap_height) / (size_t)out->ov_height;

    size_t offset = ((phys_y * (size_t)out->cap_width) + phys_x) * 4U;
    uint8_t cb = out->pixels[offset + 0U];
    uint8_t cg = out->pixels[offset + 1U];
    uint8_t cr = out->pixels[offset + 2U];
    /* byte 3 is alpha – ignored */

    app->current_color = ((uint32_t)cr << 16U) |
                          ((uint32_t)cg <<  8U) |
                           (uint32_t)cb;

    (void)snprintf(app->color_str, sizeof(app->color_str),
                   "#%02X%02X%02X", cr, cg, cb);
}

/* -------------------------------------------------------------------------
 * Pointer listener
 * ---------------------------------------------------------------------- */

static void pointer_enter(void *data, struct wl_pointer *pointer,
                           uint32_t serial,
                           struct wl_surface *surface,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    App *app = data;
    app->pointer_serial = serial;

    /* Identify which output the pointer entered by matching the surface. */
    app->active_output = NULL;
    for (int i = 0; i < app->output_count; i++) {
        if (app->outputs[i].overlay_surface == surface) {
            app->active_output = &app->outputs[i];
            break;
        }
    }

    wl_pointer_set_cursor(pointer, serial,
                          app->cursor_surface,
                          CURSOR_HOT, CURSOR_HOT);

    /* Initialise the preview at the entry position so the overlay is correct
     * immediately, before the first pointer_motion event arrives.          */
    if (app->active_output && app->phase == PHASE_PICK) {
        app->pointer_x = wl_fixed_to_int(sx);
        app->pointer_y = wl_fixed_to_int(sy);
        sample_color(app);
        commit_overlay(app->active_output);
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)pointer; (void)serial;
    App *app = data;

    /* Clear the overlay on the output we are leaving so no stale preview
     * remains visible after the pointer has moved to another output.
     * Guard against surface==NULL: libwayland passes NULL when the server
     * sends a leave event for a proxy the client has already destroyed.  */
    for (int i = 0; i < app->output_count; i++) {
        Output *out = &app->outputs[i];
        if (surface && out->overlay_surface == surface && out->ov_pixels[out->ov_back]) {
            int b = out->ov_back;
            out->ov_busy[b] = 1;
            out->ov_back    = b ^ 1;
            memset(out->ov_pixels[b], 0, out->ov_pixels_size);
            wl_surface_attach(out->overlay_surface, out->ov_buf[b], 0, 0);
            wl_surface_damage_buffer(out->overlay_surface, 0, 0,
                                     (int32_t)out->ov_width,
                                     (int32_t)out->ov_height);
            wl_surface_commit(out->overlay_surface);
            wl_display_flush(app->display);
            break;
        }
    }

    app->active_output = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                            uint32_t time,
                            wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *app = data;

    if (app->phase != PHASE_PICK || !app->active_output) {
        return;
    }

    app->pointer_x = wl_fixed_to_int(sx);
    app->pointer_y = wl_fixed_to_int(sy);

    sample_color(app);
    commit_overlay(app->active_output);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                            uint32_t serial, uint32_t time,
                            uint32_t button, uint32_t state)
{
    (void)pointer; (void)time; (void)button;
    App *app = data;

    if (app->phase != PHASE_PICK) {
        return;
    }

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        return;
    }

    /* Transition to clipboard phase. */
    app->phase = PHASE_CLIPBOARD;

    printf("%s\n", app->color_str);

    /* Destroy all overlay surfaces. */
    for (int i = 0; i < app->output_count; i++) {
        Output *out = &app->outputs[i];
        if (out->layer_surface) {
            zwlr_layer_surface_v1_destroy(out->layer_surface);
            out->layer_surface = NULL;
        }
        if (out->overlay_surface) {
            wl_surface_destroy(out->overlay_surface);
            out->overlay_surface = NULL;
        }
    }

    /* Set clipboard selection. */
    app->data_source = wl_data_device_manager_create_data_source(app->ddm);
    wl_data_source_offer(app->data_source, "text/plain;charset=utf-8");
    wl_data_source_offer(app->data_source, "text/plain");
    wl_data_source_offer(app->data_source, "UTF8_STRING");
    wl_data_source_offer(app->data_source, "STRING");

    wl_data_device_set_selection(app->data_device,
                                 app->data_source,
                                 serial);

    wl_display_flush(app->display);
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                          uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                  uint32_t axis_source)
{
    (void)data; (void)pointer; (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                                uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                    uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter         = pointer_enter,
    .leave         = pointer_leave,
    .motion        = pointer_motion,
    .button        = pointer_button,
    .axis          = pointer_axis,
    .frame         = pointer_frame,
    .axis_source   = pointer_axis_source,
    .axis_stop     = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* -------------------------------------------------------------------------
 * Seat listener
 * ---------------------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *seat,
                               uint32_t caps)
{
    App *app = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* -------------------------------------------------------------------------
 * Layer-surface listener
 * ---------------------------------------------------------------------- */

static void layer_surface_configure(void *data,
                                     struct zwlr_layer_surface_v1 *ls,
                                     uint32_t serial,
                                     uint32_t width, uint32_t height)
{
    Output *out = data;
    App *app = out->app;

    zwlr_layer_surface_v1_ack_configure(ls, serial);

    if (out->ov_pixels[0]) {
        return; /* already configured */
    }

    out->ov_width  = width  ? width  : out->cap_width;
    out->ov_height = height ? height : out->cap_height;

    /* Allocate two overlay buffers for double-buffering. */
    out->ov_pixels_size = ((size_t)out->ov_width * (size_t)out->ov_height) * 4U;
    for (int bi = 0; bi < 2; bi++) {
        out->ov_pixels[bi] = create_shm_buffer(app->shm,
                                               out->ov_width, out->ov_height,
                                               &out->ov_buf[bi]);
        if (!out->ov_pixels[bi]) {
            fprintf(stderr, "Failed to allocate overlay buffer\n");
            app->phase = PHASE_DONE;
            return;
        }
        wl_buffer_add_listener(out->ov_buf[bi], &ov_buffer_listener, out);
    }
    out->ov_back = 0;

    /* Set up the shared cursor surface exactly once (on the first configure). */
    if (!app->cursor_pixels) {
        app->cursor_pixels_size =
            ((size_t)CURSOR_SIZE * (size_t)CURSOR_SIZE) * 4U;
        app->cursor_pixels = create_shm_buffer(app->shm,
                                               (uint32_t)CURSOR_SIZE,
                                               (uint32_t)CURSOR_SIZE,
                                               &app->cursor_buf);
        if (!app->cursor_pixels) {
            fprintf(stderr, "Failed to allocate cursor buffer\n");
            app->phase = PHASE_DONE;
            return;
        }
        draw_cursor(app);
        wl_surface_attach(app->cursor_surface, app->cursor_buf, 0, 0);
        wl_surface_damage_buffer(app->cursor_surface,
                                 0, 0, CURSOR_SIZE, CURSOR_SIZE);
        wl_surface_commit(app->cursor_surface);
    }

    /* Initial commit: fully transparent so no preview appears until the
     * pointer actually enters this output.                               */
    int ini_b = out->ov_back;
    out->ov_busy[ini_b] = 1;
    out->ov_back = ini_b ^ 1;
    memset(out->ov_pixels[ini_b], 0, out->ov_pixels_size);
    wl_surface_attach(out->overlay_surface, out->ov_buf[ini_b], 0, 0);
    wl_surface_damage_buffer(out->overlay_surface, 0, 0,
                             (int32_t)out->ov_width, (int32_t)out->ov_height);
    wl_surface_commit(out->overlay_surface);

    app->overlays_configured++;
    if (app->overlays_configured == app->output_count) {
        app->phase = PHASE_PICK;
    }
}

static void layer_surface_closed(void *data,
                                   struct zwlr_layer_surface_v1 *ls)
{
    (void)ls;
    Output *out = data;
    out->app->phase = PHASE_DONE;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* -------------------------------------------------------------------------
 * Start pick phase: create one fullscreen overlay per output.
 * Called once all captures are complete.
 * ---------------------------------------------------------------------- */

static void start_pick_phase(App *app)
{
    uint32_t all_anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

    app->cursor_surface = wl_compositor_create_surface(app->compositor);

    for (int i = 0; i < app->output_count; i++) {
        Output *out = &app->outputs[i];

        out->overlay_surface = wl_compositor_create_surface(app->compositor);

        out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            app->layer_shell,
            out->overlay_surface,
            out->wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            "color-picker");

        zwlr_layer_surface_v1_set_size(out->layer_surface, 0, 0);
        zwlr_layer_surface_v1_set_anchor(out->layer_surface, all_anchors);
        zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            out->layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        zwlr_layer_surface_v1_add_listener(out->layer_surface,
                                            &layer_surface_listener, out);

        /* Initial commit triggers the configure event. */
        wl_surface_commit(out->overlay_surface);
    }
}

/* -------------------------------------------------------------------------
 * Capture frame listener
 * ---------------------------------------------------------------------- */

static void capture_frame_transform(void *data,
    struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t transform)
{
    (void)data; (void)frame; (void)transform;
}

static void capture_frame_damage(void *data,
    struct ext_image_copy_capture_frame_v1 *frame,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void capture_frame_presentation_time(void *data,
    struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)data; (void)frame;
    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
}

static void capture_frame_ready(void *data,
    struct ext_image_copy_capture_frame_v1 *frame)
{
    (void)frame;
    Output *out = data;
    App *app = out->app;

    /* Pixels are now populated.  Tear down per-output capture objects. */
    ext_image_copy_capture_frame_v1_destroy(out->capture_frame);
    out->capture_frame = NULL;

    ext_image_copy_capture_session_v1_destroy(out->capture_session);
    out->capture_session = NULL;

    ext_image_capture_source_v1_destroy(out->capture_source);
    out->capture_source = NULL;

    app->captures_pending--;

    if (app->captures_pending == 0) {
        /* All outputs captured; build the pick overlay. */
        start_pick_phase(app);
    }
}

static void capture_frame_failed(void *data,
    struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t reason)
{
    (void)frame;
    Output *out = data;
    fprintf(stderr, "Capture frame failed (reason=%u)\n", reason);
    out->app->phase = PHASE_DONE;
}

static const struct ext_image_copy_capture_frame_v1_listener
    capture_frame_listener = {
    .transform         = capture_frame_transform,
    .damage            = capture_frame_damage,
    .presentation_time = capture_frame_presentation_time,
    .ready             = capture_frame_ready,
    .failed            = capture_frame_failed,
};

/* -------------------------------------------------------------------------
 * Capture session listener
 * ---------------------------------------------------------------------- */

static void capture_session_buffer_size(void *data,
    struct ext_image_copy_capture_session_v1 *session,
    uint32_t width, uint32_t height)
{
    (void)session;
    Output *out = data;
    out->cap_width  = width;
    out->cap_height = height;
}

static void capture_session_shm_format(void *data,
    struct ext_image_copy_capture_session_v1 *session,
    uint32_t format)
{
    (void)data; (void)session; (void)format;
}

static void capture_session_dmabuf_device(void *data,
    struct ext_image_copy_capture_session_v1 *session,
    struct wl_array *device)
{
    (void)data; (void)session; (void)device;
}

static void capture_session_dmabuf_format(void *data,
    struct ext_image_copy_capture_session_v1 *session,
    uint32_t format, struct wl_array *modifiers)
{
    (void)data; (void)session; (void)format; (void)modifiers;
}

static void capture_session_done(void *data,
    struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    Output *out = data;
    App *app = out->app;

    if (!out->cap_width || !out->cap_height) {
        fprintf(stderr, "Capture: zero dimensions on output\n");
        app->phase = PHASE_DONE;
        return;
    }

    out->pixels_size = ((size_t)out->cap_width * (size_t)out->cap_height) * 4U;
    out->pixels = create_shm_buffer(app->shm,
                                    out->cap_width, out->cap_height,
                                    &out->capture_buf);
    if (!out->pixels) {
        fprintf(stderr, "Failed to allocate screen capture buffer\n");
        app->phase = PHASE_DONE;
        return;
    }

    /* Create the frame, attach listener, and fire the capture – all client-
     * side writes so no roundtrip is required before the next dispatch.   */
    out->capture_frame =
        ext_image_copy_capture_session_v1_create_frame(out->capture_session);

    ext_image_copy_capture_frame_v1_add_listener(out->capture_frame,
                                                  &capture_frame_listener, out);
    ext_image_copy_capture_frame_v1_attach_buffer(out->capture_frame,
                                                   out->capture_buf);
    ext_image_copy_capture_frame_v1_damage_buffer(out->capture_frame,
                                                   0, 0,
                                                   (int32_t)out->cap_width,
                                                   (int32_t)out->cap_height);
    ext_image_copy_capture_frame_v1_capture(out->capture_frame);
}

static void capture_session_stopped(void *data,
    struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    Output *out = data;
    App *app = out->app;

    if (app->phase == PHASE_CAPTURE) {
        fprintf(stderr, "Capture session stopped before frame was ready\n");
        app->phase = PHASE_DONE;
    }
}

static const struct ext_image_copy_capture_session_v1_listener
    capture_session_listener = {
    .buffer_size   = capture_session_buffer_size,
    .shm_format    = capture_session_shm_format,
    .dmabuf_device = capture_session_dmabuf_device,
    .dmabuf_format = capture_session_dmabuf_format,
    .done          = capture_session_done,
    .stopped       = capture_session_stopped,
};

/* -------------------------------------------------------------------------
 * Data source listener (clipboard)
 * ---------------------------------------------------------------------- */

static void data_source_target(void *data, struct wl_data_source *source,
                                const char *mime_type)
{
    (void)data; (void)source; (void)mime_type;
}

static void data_source_send(void *data, struct wl_data_source *source,
                               const char *mime_type, int32_t fd)
{
    (void)source; (void)mime_type;
    App *app = data;

    size_t len = strlen(app->color_str);
    ssize_t written = write(fd, app->color_str, len);
    (void)written;
    close(fd);

    /* Data has been delivered to the requesting client; exit cleanly. */
    app->phase = PHASE_DONE;
}

static void data_source_cancelled(void *data, struct wl_data_source *source)
{
    (void)source;
    App *app = data;
    /* Selection replaced before anyone read it; exit anyway. */
    wl_data_source_destroy(app->data_source);
    app->data_source = NULL;
    app->phase = PHASE_DONE;
}

static void data_source_dnd_drop_performed(void *data,
                                             struct wl_data_source *source)
{
    (void)data; (void)source;
}

static void data_source_dnd_finished(void *data, struct wl_data_source *source)
{
    (void)data; (void)source;
}

static void data_source_action(void *data, struct wl_data_source *source,
                                 uint32_t dnd_action)
{
    (void)data; (void)source; (void)dnd_action;
}

static const struct wl_data_source_listener data_source_listener = {
    .target              = data_source_target,
    .send                = data_source_send,
    .cancelled           = data_source_cancelled,
    .dnd_drop_performed  = data_source_dnd_drop_performed,
    .dnd_finished        = data_source_dnd_finished,
    .action              = data_source_action,
};

/* -------------------------------------------------------------------------
 * Registry listener
 * ---------------------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *registry,
                              uint32_t name, const char *interface,
                              uint32_t version)
{
    (void)version;
    App *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name,
                                            &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (app->output_count < MAX_OUTPUTS) {
            Output *out = &app->outputs[app->output_count++];
            out->app = app;
            out->wl_output = wl_registry_bind(registry, name,
                                               &wl_output_interface, 3);
        }
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        app->ddm = wl_registry_bind(registry, name,
                                     &wl_data_device_manager_interface, 3);
    } else if (strcmp(interface,
                      ext_output_image_capture_source_manager_v1_interface.name)
               == 0) {
        app->capture_src_mgr = wl_registry_bind(
            registry, name,
            &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface,
                      ext_image_copy_capture_manager_v1_interface.name) == 0) {
        app->capture_mgr = wl_registry_bind(
            registry, name,
            &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(registry, name,
                                             &zwlr_layer_shell_v1_interface, 4);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                    uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    App app = {0};
    app.phase = PHASE_CAPTURE;

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);

    /* First roundtrip: collect all globals. */
    (void)wl_display_roundtrip(app.display);
    /* Second roundtrip: let seat/output events arrive. */
    (void)wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.output_count || !app.seat ||
        !app.ddm || !app.capture_src_mgr || !app.capture_mgr ||
        !app.layer_shell) {
        fprintf(stderr,
                "Missing required Wayland protocols.\n"
                "This app requires a wlroots-based compositor with:\n"
                "  zwlr_layer_shell_v1\n"
                "  ext_image_copy_capture_manager_v1\n"
                "  ext_output_image_capture_source_manager_v1\n");
        return 1;
    }

    /* Third roundtrip: seat capabilities so we have a pointer. */
    (void)wl_display_roundtrip(app.display);

    if (!app.pointer) {
        fprintf(stderr, "No pointer device found\n");
        return 1;
    }

    app.data_device = wl_data_device_manager_get_data_device(app.ddm,
                                                              app.seat);

    /* ── Phase: capture ──────────────────────────────────────────────────
     * Start one capture session per output.  capture_session_done allocates
     * the pixel buffer and fires the frame capture.  capture_frame_ready
     * calls start_pick_phase when captures_pending reaches 0.            */
    app.captures_pending = app.output_count;

    for (int i = 0; i < app.output_count; i++) {
        Output *out = &app.outputs[i];

        out->capture_source =
            ext_output_image_capture_source_manager_v1_create_source(
                app.capture_src_mgr, out->wl_output);

        out->capture_session =
            ext_image_copy_capture_manager_v1_create_session(
                app.capture_mgr, out->capture_source, 0);

        ext_image_copy_capture_session_v1_add_listener(out->capture_session,
                                                        &capture_session_listener,
                                                        out);
    }

    wl_display_flush(app.display);

    /* ── Phase: pick ─────────────────────────────────────────────────────
     * One event loop drives both phases:
     *   CAPTURE → all frames ready → start_pick_phase → configure events
     *   → PICK → button click → CLIPBOARD / DONE
     */
    while ((app.phase == PHASE_CAPTURE || app.phase == PHASE_PICK) &&
           wl_display_dispatch(app.display) >= 0) {
        /* nothing */
    }

    if (app.phase != PHASE_CLIPBOARD) {
        fprintf(stderr, "Capture or pick phase failed\n");
        return 1;
    }

    /* ── Phase: clipboard ────────────────────────────────────────────────
     * Serve clipboard read requests until another app takes the selection.*/
    wl_data_source_add_listener(app.data_source, &data_source_listener, &app);

    while (app.phase == PHASE_CLIPBOARD &&
           wl_display_dispatch(app.display) >= 0) {
        /* nothing */
    }

    /* ── Cleanup ─────────────────────────────────────────────────────────*/
    for (int i = 0; i < app.output_count; i++) {
        Output *out = &app.outputs[i];
        for (int bi = 0; bi < 2; bi++) {
            if (out->ov_pixels[bi]) {
                munmap(out->ov_pixels[bi], out->ov_pixels_size);
            }
        }
        if (out->pixels) {
            munmap(out->pixels, out->pixels_size);
        }
    }
    if (app.cursor_pixels) {
        munmap(app.cursor_pixels, app.cursor_pixels_size);
    }

    wl_display_disconnect(app.display);
    return 0;
}

