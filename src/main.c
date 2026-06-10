/* wlpick – minimal Wayland color picker.
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

#include <errno.h>
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
#include "build/viewporter-protocol.h"
#include "build/wlr-layer-shell-unstable-v1-protocol.h"

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

#define PREVIEW_SIZE 110   /* side length of the color preview square, px */
#define CURSOR_SIZE  11    /* side length of the hollow-square cursor, px  */
#define CURSOR_HOT   5     /* hotspot offset from top-left corner          */
#define MAX_OUTPUTS  8     /* maximum number of simultaneously tracked outputs */

/* -------------------------------------------------------------------------
 * Phase
 * ---------------------------------------------------------------------- */

typedef enum {
    PHASE_CAPTURE,    /* waiting for all output captures to complete       */
    PHASE_PICK,       /* overlays shown; tracking pointer                 */
    PHASE_CLIPBOARD,  /* color written to clipboard; waiting for cancel   */
    PHASE_DONE,       /* error or clipboard replaced; time to exit        */
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
    fd = memfd_create("wlpick-shm", MFD_CLOEXEC);
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
                               uint32_t format,
                               struct wl_buffer **buf_out)
{
    size_t stride = (size_t)width * 4U;
    size_t size   = stride * (size_t)height;

    /* wl_shm_create_pool and wl_shm_pool_create_buffer take int32_t sizes;
     * reject anything that would not survive the cast.  size is the largest
     * of size/stride/width, so this one check covers all three casts.       */
    if (size > (size_t)INT32_MAX) {
        fprintf(stderr, "create_shm_buffer: buffer too large (%zu bytes)\n", size);
        return NULL;
    }

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
                                          format);
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
    uint32_t  cap_format;           /* SHM pixel format from shm_format event */
    int       cap_format_rank;      /* >0 once a usable format is chosen      */
    size_t    r_off;                /* red byte offset within each 4-byte px  */
    size_t    b_off;                /* blue byte offset (green is always +1)  */
    uint8_t  *pixels;               /* captured screen pixels (cap_format)    */
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
    int               ov_back;    /* index of the buffer we write into (0|1) */
    int               ov_busy[2]; /* 1 = compositor still holds the buffer  */
    int32_t           prev_px[2]; /* preview x per buffer; INT32_MIN = clean */
    int32_t           prev_py[2]; /* preview y per buffer; INT32_MIN = clean */
    int32_t           damage_x;   /* dirty rect written by draw_overlay      */
    int32_t           damage_y;
    int32_t           damage_w;
    int32_t           damage_h;

    /* ── Viewport (fractional-scale fix) ────────────────────────────────── */
    struct wp_viewport *viewport;
    uint32_t  log_width;           /* logical surface size from configure    */
    uint32_t  log_height;
    int32_t   preview_px;          /* PREVIEW_SIZE scaled to physical pixels */
    int32_t   glyph_scale;         /* font block size in physical pixels     */
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
    struct wl_keyboard   *keyboard;
    struct wl_data_device_manager *ddm;
    struct wl_data_device         *data_device;
    struct wl_data_source         *data_source;

    /* ── Extension protocol globals ────────────────────────────────────── */
    struct ext_output_image_capture_source_manager_v1 *capture_src_mgr;
    struct ext_image_copy_capture_manager_v1           *capture_mgr;
    struct zwlr_layer_shell_v1                         *layer_shell;
    struct wp_viewporter                               *viewporter;

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

/* -------------------------------------------------------------------------
 * Minimal 5×7 bitmap font for '#', '0'–'9', 'A'–'F'.
 *
 * Each entry encodes 7 rows top-to-bottom; each row is 5 pixels wide
 * (bit 4 = leftmost, bit 0 = rightmost).
 * ---------------------------------------------------------------------- */

/* clang-format off */
static const uint8_t font5x7[17][7] = {
    /* '#' */ {0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00, 0x00},
    /* '0' */ {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* '1' */ {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* '2' */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    /* '3' */ {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    /* '4' */ {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* '5' */ {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    /* '6' */ {0x07, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    /* '7' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* '8' */ {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    /* '9' */ {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
    /* 'A' */ {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* 'B' */ {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    /* 'C' */ {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    /* 'D' */ {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    /* 'E' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    /* 'F' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
};
/* clang-format on */

/* Map a character to a font5x7 row index; returns -1 if unsupported. */
static int glyph_index(char ch)
{
    if (ch == '#') {
        return 0;
    }
    if (ch >= '0' && ch <= '9') {
        return 1 + (ch - '0');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 11 + (ch - 'A');
    }
    return -1;
}

/* Paint a gs×gs block of pixels for one font dot. */
static void draw_font_pixel(uint32_t *pixels, uint32_t cw, uint32_t ch,
                             int32_t rx, int32_t ry, int32_t gs, uint32_t color)
{
    for (int32_t bry = 0; bry < gs; bry++) {
        int32_t row = ry + bry;
        if (row < 0 || row >= (int32_t)ch) {
            continue;
        }
        for (int32_t brx = 0; brx < gs; brx++) {
            int32_t col = rx + brx;
            if (col < 0 || col >= (int32_t)cw) {
                continue;
            }
            pixels[((size_t)row * (size_t)cw) + (size_t)col] = color;
        }
    }
}

/* Draw the hex label centred at the bottom of the preview square.
 * preview_px: physical PREVIEW_SIZE. bar_h_px: physical bar height.
 * gs: font block size in physical pixels (≥1).                        */
static void draw_label(Output *out, uint32_t *pixels,
                       int32_t sq_x, int32_t sq_y,
                       int32_t preview_px, int32_t bar_h_px, int32_t gs,
                       uint32_t text_color)
{
    App     *app = out->app;
    uint32_t cw  = out->cap_width;
    uint32_t ch  = out->cap_height;

    /* label_w = 7 glyphs × (5+1) font-dots × gs, minus the trailing gap. */
    int32_t label_w = 41 * gs;
    int32_t x0      = sq_x + ((preview_px - label_w) / 2);
    /* Centre label vertically in the bar. */
    int32_t y0 = sq_y + (preview_px - ((bar_h_px + (7 * gs)) / 2));

    if (y0 < 0) {
        return;
    }

    for (int ci = 0; ci < 7; ci++) {
        int gi = glyph_index(app->color_str[ci]);
        if (gi < 0) {
            continue;
        }
        int32_t gx0 = x0 + (int32_t)(ci * 6 * gs);

        for (int gy = 0; gy < 7; gy++) {
            uint8_t rb = font5x7[gi][gy];
            for (int gx = 0; gx < 5; gx++) {
                if (((unsigned)rb & (1U << (unsigned)(4 - gx))) == 0U) {
                    continue;
                }
                draw_font_pixel(pixels, cw, ch,
                                gx0 + (int32_t)(gx * gs),
                                y0  + (int32_t)(gy * gs),
                                gs, text_color);
            }
        }
    }
}

/* Fill a solid rectangle of height bar_h at the bottom of the preview square. */
static void draw_color_bar(Output *out, uint32_t *pixels,
                           int32_t sq_x, int32_t sq_y,
                           int32_t bar_h, int32_t preview_px, uint32_t color)
{
    uint32_t cw = out->cap_width;
    uint32_t ch = out->cap_height;
    int32_t  y0 = sq_y + preview_px - bar_h;
    for (int32_t dy = 0; dy < bar_h; dy++) {
        int32_t row = y0 + dy;
        if (row < 0 || row >= (int32_t)ch) {
            continue;
        }
        for (int32_t dx = 0; dx < preview_px; dx++) {
            int32_t col = sq_x + dx;
            if (col >= 0 && col < (int32_t)cw) {
                pixels[((size_t)row * (size_t)cw) + (size_t)col] = color;
            }
        }
    }
}

/* Draw a 1-pixel hollow rectangle of the given size starting at (x0, y0). */
static void draw_hollow_rect(Output *out, uint32_t *pixels,
                              int32_t x0, int32_t y0, int32_t size,
                              uint32_t color)
{
    uint32_t cw = out->cap_width;
    uint32_t ch = out->cap_height;

    for (int32_t di = -1; di <= size; di++) {
        for (int32_t edge = -1; edge <= size; edge += (size + 1)) {
            int32_t row = y0 + edge;
            int32_t col = x0 + di;
            if (row >= 0 && row < (int32_t)ch && col >= 0 && col < (int32_t)cw) {
                pixels[((size_t)row * (size_t)cw) + (size_t)col] = color;
            }
        }
        for (int32_t edge = -1; edge <= size; edge += (size + 1)) {
            int32_t row = y0 + di;
            int32_t col = x0 + edge;
            if (row >= 0 && row < (int32_t)ch && col >= 0 && col < (int32_t)cw) {
                pixels[((size_t)row * (size_t)cw) + (size_t)col] = color;
            }
        }
    }
}

/* Draw a 1-pixel border around the hotspot block.
 * zoom: physical pixels per source pixel. hot_px: physical hotspot offset. */
static void draw_sample_marker(Output *out, uint32_t *pixels,
                               int32_t sq_x, int32_t sq_y,
                               int32_t zoom, int32_t hot_px, uint32_t color)
{
    draw_hollow_rect(out, pixels,
                     sq_x + (hot_px * zoom), sq_y + (hot_px * zoom),
                     zoom, color);
}

/* Draw a 1-pixel border around the preview square. preview_px: physical size. */
static void draw_border(Output *out, uint32_t *pixels,
                        int32_t sq_x, int32_t sq_y, int32_t preview_px,
                        uint32_t border_color)
{
    draw_hollow_rect(out, pixels, sq_x, sq_y, preview_px, border_color);
}

/* Copy a rectangle of captured screen pixels (in physical coordinates) into
 * the overlay buffer.  Both buffers are at physical resolution so no scaling
 * is needed — coordinates are clamped to the capture bounds internally.    */
static void blit_capture_rect(Output *out, uint32_t *pixels,
                               int32_t rx, int32_t ry,
                               int32_t rw, int32_t rh)
{
    uint32_t cw = out->cap_width;
    uint32_t ch = out->cap_height;
    int32_t  x0 = (rx < 0) ? 0 : rx;
    int32_t  y0 = (ry < 0) ? 0 : ry;
    int32_t  x1 = ((rx + rw) > (int32_t)cw) ? (int32_t)cw : (rx + rw);
    int32_t  y1 = ((ry + rh) > (int32_t)ch) ? (int32_t)ch : (ry + rh);

    for (int32_t row = y0; row < y1; row++) {
        for (int32_t col = x0; col < x1; col++) {
            size_t  off = (((size_t)row * (size_t)cw) + (size_t)col) * 4U;
            uint8_t cb  = out->pixels[off + out->b_off];
            uint8_t cg  = out->pixels[off + 1U];
            uint8_t cr  = out->pixels[off + out->r_off];
            pixels[((size_t)row * (size_t)cw) + (size_t)col] =
                (0xFFU << 24U) | ((uint32_t)cr << 16U) |
                ((uint32_t)cg <<  8U) | (uint32_t)cb;
        }
    }
}

/* Compute the union of the new and (if any) old preview dirty rects and store
 * it in out->damage_*.  Each rect is PREVIEW_SIZE+2 wide/tall, starting 1px
 * before the square corner to include the 1-pixel border.                  */
static void update_damage_rect(Output *out,
                                int32_t px, int32_t py,
                                int32_t old_px, int32_t old_py)
{
    int32_t pp  = out->preview_px;
    int32_t dx0 = px - 1;
    int32_t dy0 = py - 1;
    int32_t dx1 = px + pp + 1;
    int32_t dy1 = py + pp + 1;
    if (old_px != INT32_MIN) {
        if ((old_px - 1) < dx0) { dx0 = old_px - 1; }
        if ((old_py - 1) < dy0) { dy0 = old_py - 1; }
        if ((old_px + pp + 1) > dx1) { dx1 = old_px + pp + 1; }
        if ((old_py + pp + 1) > dy1) { dy1 = old_py + pp + 1; }
    }
    out->damage_x = dx0;
    out->damage_y = dy0;
    out->damage_w = dx1 - dx0;
    out->damage_h = dy1 - dy0;
}

/* Return black or white, whichever contrasts better with the given colour. */
static inline uint32_t contrast_color(uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t luma = (r * 299U) + (g * 587U) + (b * 114U);
    return (luma > 127500U) ? 0xFF000000U : 0xFFFFFFFFU;
}

static void draw_overlay(Output *out)
{
    int       bi     = out->ov_back;
    uint32_t *pixels = out->ov_pixels[bi];
    if (!pixels) {
        return;
    }

    App     *app  = out->app;
    uint32_t cw   = out->cap_width;
    uint32_t ch   = out->cap_height;
    int32_t  pp   = out->preview_px;
    int32_t  gs   = out->glyph_scale;

    /* Scale pointer logical coords to physical buffer coords. */
    int32_t ptr_x = (int32_t)(((size_t)app->pointer_x * (size_t)cw) / (size_t)out->log_width);
    int32_t ptr_y = (int32_t)(((size_t)app->pointer_y * (size_t)ch) / (size_t)out->log_height);

    /* Physical equivalents of the cursor size constants. */
    int32_t cursor_px = (int32_t)(((size_t)CURSOR_SIZE * (size_t)cw) / (size_t)out->log_width);
    int32_t hot_px    = (int32_t)(((size_t)CURSOR_HOT  * (size_t)cw) / (size_t)out->log_width);
    if (cursor_px < 1) { cursor_px = 1; }
    if (hot_px    < 1) { hot_px    = 1; }

    /* Repair this buffer's previous dirty region before drawing the new one. */
    int32_t old_px = out->prev_px[bi];
    int32_t old_py = out->prev_py[bi];
    if (old_px != INT32_MIN) {
        blit_capture_rect(out, pixels, old_px - 1, old_py - 1, pp + 2, pp + 2);
    }

    /* Position preview square (prefer right+below; clamp to physical edge). */
    int32_t gap = (int32_t)(((size_t)4U * (size_t)cw) / (size_t)out->log_width);
    int32_t px  = ptr_x + cursor_px + gap;
    int32_t py  = ptr_y + cursor_px + gap;
    if (px + pp > (int32_t)cw) { px = ptr_x - pp - gap; }
    if (py + pp > (int32_t)ch) { py = ptr_y - pp - gap; }

    /* Zoom cursor_px×cursor_px source region to pp×pp in the preview.
     * Source and destination are both in physical pixel coordinates.   */
    /* zoom must stay >= 1: on a downscaled output (physical < logical) pp can
     * fall below cursor_px, and zoom is a divisor below — never let it hit 0. */
    int32_t zoom         = pp / cursor_px;
    if (zoom < 1) { zoom = 1; }
    int32_t src_origin_x = ptr_x - hot_px;
    int32_t src_origin_y = ptr_y - hot_px;

    for (int32_t dy = 0; dy < pp; dy++) {
        int32_t row = py + dy;
        if (row < 0 || row >= (int32_t)ch) {
            continue;
        }
        int32_t src_py = src_origin_y + (dy / zoom);

        for (int32_t dx = 0; dx < pp; dx++) {
            int32_t col = px + dx;
            if (col < 0 || col >= (int32_t)cw) {
                continue;
            }
            int32_t src_px = src_origin_x + (dx / zoom);

            uint32_t color;
            if (!out->pixels ||
                src_px < 0 || src_py < 0 ||
                (uint32_t)src_px >= cw || (uint32_t)src_py >= ch) {
                color = 0xFF000000U;
            } else {
                size_t  off = (((size_t)src_py * (size_t)cw) + (size_t)src_px) * 4U;
                uint8_t cb  = out->pixels[off + out->b_off];
                uint8_t cg  = out->pixels[off + 1U];
                uint8_t cr  = out->pixels[off + out->r_off];
                color = (0xFFU << 24U) | ((uint32_t)cr << 16U) |
                        ((uint32_t)cg << 8U) | (uint32_t)cb;
            }
            pixels[((size_t)row * (size_t)cw) + (size_t)col] = color;
        }
    }

    uint32_t tr = (app->current_color >> 16U) & 0xFFU;
    uint32_t tg = (app->current_color >>  8U) & 0xFFU;
    uint32_t tb =  app->current_color          & 0xFFU;
    uint32_t bar_color = (0xFFU << 24U) | (tr << 16U) | (tg << 8U) | tb;
    int32_t  bar_h_px  = (int32_t)(((size_t)20U * (size_t)cw) / (size_t)out->log_width);
    if (bar_h_px < gs) { bar_h_px = gs; }
    draw_color_bar(out, pixels, px, py, bar_h_px, pp, bar_color);

    uint32_t text_color = contrast_color(tr, tg, tb);

    draw_border(out, pixels, px, py, pp, text_color);
    draw_sample_marker(out, pixels, px, py, zoom, hot_px, text_color);
    draw_label(out, pixels, px, py, pp, bar_h_px, gs, text_color);

    out->prev_px[bi] = px;
    out->prev_py[bi] = py;
    update_damage_rect(out, px, py, old_px, old_py);
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
                             out->damage_x, out->damage_y,
                             out->damage_w, out->damage_h);
    wl_surface_commit(out->overlay_surface);
    wl_display_flush(out->app->display);
}

static void draw_cursor(App *app, uint32_t color)
{
    for (int cy = 0; cy < CURSOR_SIZE; cy++) {
        for (int cx = 0; cx < CURSOR_SIZE; cx++) {
            int border = (cx == 0 || cx == CURSOR_SIZE - 1 ||
                          cy == 0 || cy == CURSOR_SIZE - 1);
            app->cursor_pixels[((size_t)cy * (size_t)CURSOR_SIZE) + (size_t)cx] =
                border ? color : 0x00000000U;
        }
    }
}

static void update_cursor_color(App *app)
{
    if (!app->cursor_pixels) {
        return;
    }
    uint32_t tr    = (app->current_color >> 16U) & 0xFFU;
    uint32_t tg    = (app->current_color >>  8U) & 0xFFU;
    uint32_t tb    =  app->current_color          & 0xFFU;
    uint32_t color = contrast_color(tr, tg, tb);
    draw_cursor(app, color);
    wl_surface_attach(app->cursor_surface, app->cursor_buf, 0, 0);
    wl_surface_damage_buffer(app->cursor_surface,
                             0, 0, CURSOR_SIZE, CURSOR_SIZE);
    wl_surface_commit(app->cursor_surface);
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

    /* Bounds check in logical coords; scale to physical for the capture lookup. */
    if (px < 0 || py < 0 ||
        (uint32_t)px >= out->log_width ||
        (uint32_t)py >= out->log_height) {
        return;
    }

    size_t phys_x = ((size_t)px * (size_t)out->cap_width)  / (size_t)out->log_width;
    size_t phys_y = ((size_t)py * (size_t)out->cap_height) / (size_t)out->log_height;

    size_t offset = ((phys_y * (size_t)out->cap_width) + phys_x) * 4U;
    uint8_t cb = out->pixels[offset + out->b_off];
    uint8_t cg = out->pixels[offset + 1U];
    uint8_t cr = out->pixels[offset + out->r_off];
    /* remaining byte is alpha/unused – ignored */

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
        update_cursor_color(app);
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
        if (!surface || out->overlay_surface != surface ||
            !out->ov_pixels[out->ov_back]) {
            continue;
        }
        int bi = out->ov_back;

        /* If both buffers are clean there is no preview to erase.          */
        if (out->prev_px[bi] == INT32_MIN && out->prev_px[bi ^ 1] == INT32_MIN) {
            break;
        }

        /* Pick a buffer we are allowed to write into.  Mirror commit_overlay's
         * busy-check: never modify a buffer the compositor still holds.    */
        if (out->ov_busy[bi]) {
            int alt = bi ^ 1;
            if (out->ov_busy[alt]) {
                break; /* both held by compositor; release will arrive soon */
            }
            bi = alt;
        }

        /* Repair the chosen buffer's dirty region if it has one.
         * If the buffer is clean but the front buffer is dirty, the
         * buffer already holds correct screenshot pixels everywhere —
         * commit it as-is to replace the stale front buffer.              */
        if (out->prev_px[bi] != INT32_MIN) {
            blit_capture_rect(out, out->ov_pixels[bi],
                              out->prev_px[bi] - 1, out->prev_py[bi] - 1,
                              out->preview_px + 2, out->preview_px + 2);
            out->prev_px[bi] = INT32_MIN;
            out->prev_py[bi] = INT32_MIN;
        }

        out->ov_busy[bi] = 1;
        out->ov_back     = bi ^ 1;
        wl_surface_attach(out->overlay_surface, out->ov_buf[bi], 0, 0);
        /* Full-surface damage: the front buffer may have had a preview at a
         * different position than the back buffer, and both must be cleared. */
        wl_surface_damage_buffer(out->overlay_surface, 0, 0,
                                 (int32_t)out->cap_width, (int32_t)out->cap_height);
        wl_surface_commit(out->overlay_surface);
        wl_display_flush(app->display);
        break;
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
    update_cursor_color(app);
    commit_overlay(app->active_output);
}

/* Forward declaration – defined after the capture session callbacks. */
static const struct ext_image_copy_capture_session_v1_listener
    capture_session_listener;


#define BTN_LEFT_EVDEV 0x110U

static void pointer_button(void *data, struct wl_pointer *pointer,
                            uint32_t serial, uint32_t time,
                            uint32_t button, uint32_t state)
{
    (void)pointer; (void)time;
    App *app = data;

    if (app->phase != PHASE_PICK) {
        return;
    }

    /* Only the left button copies; ignore every other button. */
    if (button != BTN_LEFT_EVDEV ||
        state != WL_POINTER_BUTTON_STATE_PRESSED) {
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

/* -------------------------------------------------------------------------
 * Keyboard listener – only used to detect Escape for cancellation.
 * ---------------------------------------------------------------------- */

#define KEY_ESC_EVDEV   1U

static void keyboard_keymap(void *data, struct wl_keyboard *kb,
                             uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)kb; (void)format; (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb,
                            uint32_t serial, struct wl_surface *surface,
                            struct wl_array *keys)
{
    (void)data; (void)kb; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *kb,
                            uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)kb; (void)serial; (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *kb,
                          uint32_t serial, uint32_t time,
                          uint32_t key, uint32_t state)
{
    (void)kb; (void)serial; (void)time;
    App *app = data;
    if (key == KEY_ESC_EVDEV && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        app->phase = PHASE_DONE;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                                uint32_t serial,
                                uint32_t mods_depressed,
                                uint32_t mods_latched,
                                uint32_t mods_locked,
                                uint32_t group)
{
    (void)data; (void)kb; (void)serial;
    (void)mods_depressed; (void)mods_latched; (void)mods_locked; (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                  int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                               uint32_t caps)
{
    App *app = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
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

    /* Store logical surface dimensions; create overlay buffers at physical size. */
    out->log_width  = width  ? width  : out->cap_width;
    out->log_height = height ? height : out->cap_height;
    out->ov_width   = out->cap_width;
    out->ov_height  = out->cap_height;

    /* Compute physical equivalents of the logical UI size constants. */
    out->preview_px  = (int32_t)(((size_t)PREVIEW_SIZE * (size_t)out->cap_width) /
                                  (size_t)out->log_width);
    out->glyph_scale = (int32_t)((2U * out->cap_width) / out->log_width);
    if (out->preview_px  < 1) { out->preview_px  = (int32_t)PREVIEW_SIZE; }
    if (out->glyph_scale < 1) { out->glyph_scale = 1; }

    /* Tell the compositor to map the physical buffer to the logical surface. */
    if (out->viewport) {
        wp_viewport_set_destination(out->viewport,
                                    (int32_t)out->log_width,
                                    (int32_t)out->log_height);
    }

    /* Allocate two overlay buffers at physical resolution for double-buffering. */
    out->ov_pixels_size = ((size_t)out->cap_width * (size_t)out->cap_height) * 4U;
    for (int bi = 0; bi < 2; bi++) {
        out->ov_pixels[bi] = create_shm_buffer(app->shm,
                                               out->cap_width, out->cap_height,
                                               WL_SHM_FORMAT_ARGB8888,
                                               &out->ov_buf[bi]);
        if (!out->ov_pixels[bi]) {
            fprintf(stderr, "Failed to allocate overlay buffer\n");
            app->phase = PHASE_DONE;
            return;
        }
        wl_buffer_add_listener(out->ov_buf[bi], &ov_buffer_listener, out);
    }
    out->ov_back = 0;

    /* Blit the captured screen into both buffers so neither ever needs a full
     * repaint — draw_overlay will repair only the small preview region.     */
    for (int bi = 0; bi < 2; bi++) {
        blit_capture_rect(out, out->ov_pixels[bi],
                          0, 0,
                          (int32_t)out->cap_width, (int32_t)out->cap_height);
        out->prev_px[bi] = INT32_MIN;
        out->prev_py[bi] = INT32_MIN;
    }

    /* Set up the shared cursor surface exactly once (on the first configure). */
    if (!app->cursor_pixels) {
        app->cursor_pixels_size =
            ((size_t)CURSOR_SIZE * (size_t)CURSOR_SIZE) * 4U;
        app->cursor_pixels = create_shm_buffer(app->shm,
                                               (uint32_t)CURSOR_SIZE,
                                               (uint32_t)CURSOR_SIZE,
                                               WL_SHM_FORMAT_ARGB8888,
                                               &app->cursor_buf);
        if (!app->cursor_pixels) {
            fprintf(stderr, "Failed to allocate cursor buffer\n");
            app->phase = PHASE_DONE;
            return;
        }
        draw_cursor(app, 0xFFFFFFFFU);
        wl_surface_attach(app->cursor_surface, app->cursor_buf, 0, 0);
        wl_surface_damage_buffer(app->cursor_surface,
                                 0, 0, CURSOR_SIZE, CURSOR_SIZE);
        wl_surface_commit(app->cursor_surface);
    }

    /* Initial commit: background is already painted above; full damage because
     * the compositor has never seen this buffer before.                     */
    int ini_b = out->ov_back;
    out->ov_busy[ini_b] = 1;
    out->ov_back = ini_b ^ 1;
    wl_surface_attach(out->overlay_surface, out->ov_buf[ini_b], 0, 0);
    wl_surface_damage_buffer(out->overlay_surface, 0, 0,
                             (int32_t)out->cap_width, (int32_t)out->cap_height);
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

        if (app->viewporter) {
            out->viewport = wp_viewporter_get_viewport(app->viewporter,
                                                       out->overlay_surface);
        }

        out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            app->layer_shell,
            out->overlay_surface,
            out->wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            "wlpick");

        zwlr_layer_surface_v1_set_size(out->layer_surface, 0, 0);
        zwlr_layer_surface_v1_set_anchor(out->layer_surface, all_anchors);
        zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            out->layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

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

    /* The compositor may have sent a new buffer_size (e.g. on an output mode
     * change) while the frame was in flight, updating cap_width/cap_height
     * after out->pixels was allocated.  Everything downstream indexes
     * out->pixels by cap_width × cap_height, so a mismatch would read past
     * the end of the mapping.  The session is destroyed above, so no further
     * buffer_size events can arrive — this check closes the race for good. */
    size_t expected = ((size_t)out->cap_width * (size_t)out->cap_height) * 4U;
    if (expected != out->pixels_size) {
        fprintf(stderr, "Capture: buffer size changed while frame was in flight\n");
        app->phase = PHASE_DONE;
        return;
    }

    app->captures_pending--;

    if (app->captures_pending == 0) {
        /* Initial capture complete; build the pick overlay. */
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

/* Classify an advertised shm format.  We only handle the four 8-bit-per-channel
 * packed formats whose green byte sits at offset +1 and whose red/blue bytes are
 * at the two ends; the alpha/unused byte is ignored.  Returns a preference rank
 * (>0 = usable, higher = preferred) and reports the red/blue byte offsets.   */
static int shm_format_rank(uint32_t format, size_t *r_off, size_t *b_off)
{
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888: *r_off = 2; *b_off = 0; return 4;
    case WL_SHM_FORMAT_XRGB8888: *r_off = 2; *b_off = 0; return 3;
    case WL_SHM_FORMAT_ABGR8888: *r_off = 0; *b_off = 2; return 2;
    case WL_SHM_FORMAT_XBGR8888: *r_off = 0; *b_off = 2; return 1;
    default:                                             return 0;
    }
}

static void capture_session_shm_format(void *data,
    struct ext_image_copy_capture_session_v1 *session,
    uint32_t format)
{
    (void)session;
    Output *out = data;

    /* Keep the highest-ranked format the compositor advertises.  Green is read
     * from offset +1 in every case; r_off/b_off pin down red and blue so the
     * BGR-order formats (X/ABGR8888) sample the same colours as the RGB ones. */
    size_t r_off = 0;
    size_t b_off = 0;
    int rank = shm_format_rank(format, &r_off, &b_off);
    if (rank > out->cap_format_rank) {
        out->cap_format      = format;
        out->cap_format_rank = rank;
        out->r_off           = r_off;
        out->b_off           = b_off;
    }
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

    /* The compositor may re-send buffer constraints — a second `done` — while
     * a frame is still in flight.  At most one frame may exist per session, so
     * calling create_frame again would raise the duplicate_frame protocol
     * error and disconnect us.  We only need a single capture per session, so
     * ignore constraint updates once a frame is already pending.            */
    if (out->capture_frame) {
        return;
    }

    /* Honour the shm_format constraint: the compositor requires us to attach a
     * buffer in one of the advertised formats.  If it advertised none we can
     * read (only ARGB8888/XRGB8888 share our byte order), guessing would mean
     * attaching an unsupported buffer — fail loudly instead.                 */
    if (!out->cap_format_rank) {
        fprintf(stderr, "Capture: no supported shm format advertised\n");
        app->phase = PHASE_DONE;
        return;
    }

    size_t needed = ((size_t)out->cap_width * (size_t)out->cap_height) * 4U;

    /* Allocate the pixel buffer on the first `done`.  If a buffer already
     * exists from an earlier capture its size must still match the advertised
     * dimensions, otherwise the compositor would write past the end of our
     * mapping — bail rather than attach a mismatched buffer.               */
    if (!out->pixels) {
        out->pixels_size = needed;
        out->pixels = create_shm_buffer(app->shm,
                                        out->cap_width, out->cap_height,
                                        out->cap_format,
                                        &out->capture_buf);
        if (!out->pixels) {
            fprintf(stderr, "Failed to allocate screen capture buffer\n");
            app->phase = PHASE_DONE;
            return;
        }
    } else if (out->pixels_size != needed) {
        fprintf(stderr, "Capture: buffer size changed between sessions\n");
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

    const char *buf = app->color_str;
    size_t remaining = strlen(buf);
    while (remaining > 0) {
        ssize_t n = write(fd, buf, remaining);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            break;
        }
        buf       += (size_t)n;
        remaining -= (size_t)n;
    }
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
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        app->viewporter = wl_registry_bind(registry, name,
                                            &wp_viewporter_interface, 1);
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
 * Cleanup – runs on every exit path that may have allocated buffers.
 * ---------------------------------------------------------------------- */

static void cleanup_app(App *app)
{
    for (int i = 0; i < app->output_count; i++) {
        Output *out = &app->outputs[i];
        if (out->viewport) {
            wp_viewport_destroy(out->viewport);
        }
        /* These buffers hold copies of the captured screen (the overlay
         * buffers have the screenshot blitted into them).  Scrub them before
         * unmapping so potentially sensitive screen content does not linger
         * in freed pages until the kernel happens to reuse them.            */
        for (int bi = 0; bi < 2; bi++) {
            if (out->ov_pixels[bi]) {
                explicit_bzero(out->ov_pixels[bi], out->ov_pixels_size);
                munmap(out->ov_pixels[bi], out->ov_pixels_size);
            }
        }
        if (out->pixels) {
            explicit_bzero(out->pixels, out->pixels_size);
            munmap(out->pixels, out->pixels_size);
        }
    }
    if (app->cursor_pixels) {
        munmap(app->cursor_pixels, app->cursor_pixels_size);
    }
    if (app->viewporter) {
        wp_viewporter_destroy(app->viewporter);
    }

    wl_display_disconnect(app->display);
}

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
    while ((app.phase == PHASE_CAPTURE ||
            app.phase == PHASE_PICK) &&
           wl_display_dispatch(app.display) >= 0) {
        /* nothing */
    }

    if (app.phase == PHASE_DONE) {
        /* User pressed Escape – clean cancellation. */
        cleanup_app(&app);
        return 0;
    }

    if (app.phase != PHASE_CLIPBOARD) {
        fprintf(stderr, "Capture or pick phase failed\n");
        cleanup_app(&app);
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
    cleanup_app(&app);
    return 0;
}

