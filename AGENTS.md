# AGENTS.md

This file provides guidance to agents when working with code in this repository.

## Build

```sh
meson setup build
meson compile -C build
./build/wayland-color-picker
```

Lint:
```sh
clang-tidy src/main.c -- -std=gnu11 -D_GNU_SOURCE -I build -I .
```

Format:
```sh
clang-format -i src/*.c
```

Requires `meson`, `ninja`, `gcc` or `clang`, `clang-tidy`, `clang-format`, `wayland-scanner`, and `libwayland-client`. No external package dependencies beyond Wayland itself.

## Toolchain constraints

- Language: **C11** (`-std=c11` in meson; clang-tidy uses `-std=gnu11 -D_GNU_SOURCE` because `memfd_create` requires GNU extensions)
- `_GNU_SOURCE` is passed via compiler flag, **never** defined in source
- Build flags: `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wundef -fstack-protector-strong`
- clang-tidy: `WarningsAsErrors: '*'`; minimum identifier length = 2 chars (single-char loop variables like `i`, `b` are rejected — use `bi`, etc.)
- Suppressed checks include: `readability-magic-numbers`, `misc-include-cleaner`, `cert-err33-c`, `bugprone-easily-swappable-parameters`, and others (see `.clang-tidy`)
- Common lint fixes needed: add parentheses for `readability-math-missing-parentheses`, use `(void)param` casts for unused callback params, add `U` suffix for `uint32_t` arithmetic, use `(size_t)` casts before array indexing
- `/* clang-format off/on */` is used to protect table-formatted data (e.g. the font bitmap table)

## Architecture

The app progresses through three phases driven by a single `wl_display_dispatch` event loop in `main()`:

1. **PHASE_CAPTURE** — before the overlay appears, capture each connected output once using `ext_image_copy_capture_manager_v1`. Raw ARGB8888 pixels are mmap'd into `Output.pixels`. Captures for all outputs run in parallel; `captures_pending` is decremented per `capture_frame_ready`; when it reaches 0 `start_pick_phase()` is called.
2. **PHASE_PICK** — a fullscreen transparent `zwlr_layer_surface_v1` overlay is shown on every output. The system cursor is replaced by a custom 10×10 hollow-square surface via `wl_pointer.set_cursor` (hotspot at centre, `CURSOR_HOT=5`). On every `wl_pointer.motion` event the hotspot pixel is read from the captured buffer of the active output and a 100×100 filled preview square is redrawn on that output's overlay. The preview includes a hex label (`#RRGGBB`) in a contrast-aware color (BT.601 luminance, black/white threshold) and a 1-pixel border. Overlay surfaces are **double-buffered** (`ov_buf[2]`, `ov_pixels[2]`, `ov_busy[2]`, `ov_back` index) with a `wl_buffer.release` listener tracking busy state. `wl_display_flush` is called inside `commit_overlay` to eliminate dispatch-cycle latency. Keyboard interactivity is set to `EXCLUSIVE` so Escape is received.
3. **PHASE_CLIPBOARD** — on button press, the hex string is written to a `wl_data_source` and offered via `wl_data_device.set_selection`. All overlay and cursor surfaces are destroyed. The event loop blocks until `wl_data_source.send` (paste) or `wl_data_source.cancelled` (selection replaced) fires, then exits 0.

Pressing Escape during PHASE_PICK sets `phase = PHASE_DONE`, which causes the main loop to exit; the app exits 0.

## Protocol bindings

`meson.build` runs `wayland-scanner` at build time over four vendored XMLs in `protocol/`. Each produces a `.h` and a `.c` in `build/`. The include path includes both `build/` and `.` (workspace root). Core Wayland types come from `libwayland-client` directly.

Protocols used:

| Protocol | Purpose |
|---|---|
| Core `wl_compositor`, `wl_shm`, `wl_output` | Surfaces, shared-memory pixel buffers, output enumeration |
| Core `wl_seat`, `wl_pointer`, `wl_keyboard` | Pointer motion/button, Escape key, custom cursor |
| Core `wl_data_device_manager` | Clipboard selection |
| `zwlr_layer_shell_v1` (vendored) | Fullscreen transparent overlay above all windows |
| `ext_output_image_capture_source_manager_v1` (vendored) | Capture source from an output |
| `ext_image_copy_capture_manager_v1` (vendored) | Copy output pixels into a shm buffer |

## Key types (`src/main.c`)

```c
typedef enum {
    PHASE_CAPTURE, PHASE_PICK, PHASE_CLIPBOARD, PHASE_DONE
} Phase;

typedef struct {
    App *app;
    struct wl_output *wl_output;
    /* Capture */
    struct ext_image_capture_source_v1       *capture_source;
    struct ext_image_copy_capture_session_v1 *capture_session;
    struct ext_image_copy_capture_frame_v1   *capture_frame;
    uint32_t  cap_width, cap_height;
    uint8_t  *pixels; size_t pixels_size;
    struct wl_buffer *capture_buf;
    /* Overlay – double buffered */
    struct wl_surface            *overlay_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t  ov_width, ov_height;
    size_t    ov_pixels_size;
    uint32_t        *ov_pixels[2];
    struct wl_buffer *ov_buf[2];
    int               ov_back;
    int               ov_busy[2];
} Output;

struct App {
    struct wl_display *display; struct wl_registry *registry;
    struct wl_compositor *compositor; struct wl_shm *shm;
    struct wl_seat *seat; struct wl_pointer *pointer; struct wl_keyboard *keyboard;
    struct wl_data_device_manager *ddm;
    struct wl_data_device *data_device; struct wl_data_source *data_source;
    struct ext_output_image_capture_source_manager_v1 *capture_src_mgr;
    struct ext_image_copy_capture_manager_v1 *capture_mgr;
    struct zwlr_layer_shell_v1 *layer_shell;
    Output outputs[MAX_OUTPUTS]; /* MAX_OUTPUTS = 8 */
    int output_count; int captures_pending; int overlays_configured;
    Output *active_output;
    struct wl_surface *cursor_surface;
    uint32_t *cursor_pixels; size_t cursor_pixels_size; struct wl_buffer *cursor_buf;
    uint32_t pointer_serial; int32_t pointer_x, pointer_y;
    uint32_t current_color; char color_str[8]; /* "#RRGGBB\0" */
    Phase phase;
};
```

## Key functions (definition order in `src/main.c`)

- `shm_alloc(size)` — anonymous shm fd; uses `memfd_create` on Linux, `shm_open` fallback
- `create_shm_buffer(shm, w, h, buf_out)` — wl_shm_pool + mmap; caller must `munmap`
- `ov_buffer_release` + `ov_buffer_listener` — clears `ov_busy[i]` when compositor releases buffer
- `font5x7[17][7]` — embedded 5×7 bitmap font (glyphs: `#`, `0`–`9`, `A`–`F`); wrapped in `/* clang-format off/on */`
- `glyph_index(char)` — maps character to font table index; returns -1 for unknown
- `draw_label(out, pixels, sq_x, sq_y, text_color)` — renders hex string at 2× scale (10×14 px per glyph, 2 px gap), centred at bottom of preview square with 4 px bottom margin
- `draw_border(out, pixels, sq_x, sq_y, border_color)` — 1-pixel border around the preview square
- `draw_overlay(Output*)` — clears to transparent, draws preview square, computes BT.601 luminance for text color, calls `draw_border` then `draw_label`
- `commit_overlay(Output*)` — picks free back buffer, marks busy, flips `ov_back`, attaches/damages/commits, calls `wl_display_flush`
- `draw_cursor(App*)` — 10×10 hollow white square (1 px border)
- `sample_color(App*)` — reads from `active_output->pixels`; scales logical→physical: `phys_x = px * cap_width / ov_width`
- `pointer_enter` — finds active output by surface match; uses entry `sx`/`sy` for immediate preview; calls `sample_color` + `commit_overlay`
- `pointer_leave` — clears leaving output's overlay with transparent commit + flush; sets `active_output = NULL`; guards `surface &&` to avoid NULL crash from libwayland
- `pointer_motion` — guards `phase == PICK && active_output`; calls `sample_color` + `commit_overlay`
- `pointer_button` — on press → destroy overlays/cursor, create data_source, set clipboard selection, set `PHASE_CLIPBOARD`; uses button-press `serial` (not stale enter serial)
- `keyboard_key` — Escape (evdev keycode 1) → `phase = PHASE_DONE`
- `layer_surface_configure` — allocates 2 overlay buffers + adds `ov_buffer_listener`; commits a blank frame (not `draw_overlay`) to avoid ghost preview at (0,0); increments `overlays_configured`; sets `phase = PICK` when all outputs configured
- `capture_frame_ready` — destroys frame/session/source; decrements `captures_pending`; when it reaches 0 calls `start_pick_phase`
- `capture_session_done` — allocates pixel buffer if not already allocated, creates frame, attaches + damages + fires capture in one step
- `data_source_send` — writes `color_str` to fd; closes fd; sets `phase = DONE`
- `data_source_cancelled` — destroys source; sets `phase = DONE`
- `registry_global` — binds all globals; binds every `wl_output` (no single-output guard)
- `main` — 3 roundtrips; validates globals; creates data_device; starts captures; runs merged CAPTURE+PICK event loop; checks `PHASE_DONE` (Escape → exit 0) before `PHASE_CLIPBOARD` check; runs clipboard loop; cleanup (munmap all buffers)

## Known patterns and pitfalls

- `(void)param;` casts for unused callback parameters (required by `-Wpedantic`)
- `uint32_t` arithmetic needs explicit `U` suffixes for `-Wconversion`
- Array indexing needs `(size_t)` casts: `pixels[(size_t)row * ow + (size_t)col]`
- `readability-math-missing-parentheses`: always parenthesise mixed `*` and `+`, e.g. `(a * b) + (c * d)`
- Identifier minimum length 2: use `bi` not `b`, `ci` not `i` in loops inside lint-checked functions
- `layer_surface_configure` must commit a **blank** frame on first configure — calling `draw_overlay` with zero pointer coordinates causes a ghost preview square at (0,0) on all outputs
- `pointer_leave` receives `surface == NULL` from libwayland when the proxy is already destroyed — must guard with `surface &&` before comparing
- Button serial for `wl_data_device_set_selection` must come from the `pointer_button` event, not from the stale `pointer_enter` serial
- `capture_session_done` guards pixel-buffer allocation with `!out->pixels` so a re-sent `done` (the compositor may emit buffer constraints more than once) does not allocate a second buffer
- Cognitive complexity limit is 25 per function (clang-tidy `readability-function-cognitive-complexity`); extract helpers rather than adding more nesting

