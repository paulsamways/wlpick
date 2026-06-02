# wayland-color-picker

A minimal, dependency-free color picker for wlroots-based Wayland compositors (Sway, Hyprland, and others). It captures the screen, lets you hover over any pixel to preview its color in real time, and copies the hex code to your clipboard on click — all without pulling in any GUI toolkit or image processing library. The only runtime dependency is `libwayland-client`.

## What it does

Launch the app and your cursor is replaced with a 10×10 hollow-square cursor. A 100×100 pixel preview square appears next to the cursor showing the color under the hotspot, updated live as you move the mouse. The hex code (`#RRGGBB`) is rendered inside the preview square in a contrast-aware color (black on light backgrounds, white on dark), with a matching 1-pixel border around the square.

- **Left-click** anywhere to copy the color to the clipboard and exit.
- **Right-click** to re-capture the screen and refresh the sampled image (useful when the initial capture missed hardware-accelerated content).
- **Escape** to cancel and exit with a success code.

The selected color is placed on the clipboard in `#RRGGBB` format. The process stays alive briefly to serve paste requests (standard Wayland clipboard behavior), then exits once another copy operation replaces the selection.

## Compatibility

Requires a **wlroots-based compositor** (e.g. Sway, Hyprland). The protocols used (`zwlr_layer_shell_v1`, `ext_image_copy_capture_v1`) are not available on GNOME (Mutter) or KDE (KWin).

The binding constraint is `ext_image_copy_capture_v1` and `ext_image_capture_source_v1`, which were added to wlroots in **0.19.0**:

| Compositor | Minimum version | wlroots version |
|---|---|---|
| Sway | **1.11** | 0.19.0 |
| Hyprland | Recent builds (late 2024+) — check your version supports `ext-image-copy-capture-v1` |

`zwlr_layer_shell_v1` has been available since wlroots 0.6 and is not a constraint.

Multi-monitor setups are fully supported — up to 8 outputs are tracked simultaneously, each with its own screen capture and overlay surface.

> **Note on hardware-accelerated content:** The screen is captured once at startup using the compositor's framebuffer. Content rendered via direct KMS/DRM scanout (fullscreen games, hardware-decoded video) may not be captured accurately — this is a compositor-level limitation, not a bug in the app. Right-click to trigger a fresh capture if you need to sample a region that was not captured correctly.

## How it works

The app runs through the following phases driven by a single Wayland event loop:

### 1. Capture phase

Before the overlay appears, `ext_image_copy_capture_v1` is used to capture each connected output into a shared-memory buffer (ARGB8888, mmap'd). Captures for all outputs run in parallel. Because the overlay does not yet exist, it cannot appear in the screenshot.

### 2. Pick phase

A fullscreen transparent `zwlr_layer_surface_v1` overlay is created on every output. The system cursor is replaced with a custom 10×10 hollow-square surface (hotspot at centre, 5,5). Keyboard interactivity is set to `EXCLUSIVE` so Escape is received immediately.

On every pointer motion event:
- The pixel at the cursor hotspot is read from the captured buffer for the active output.
- Logical pointer coordinates are scaled to physical pixel coordinates to handle HiDPI outputs correctly (`phys_x = logical_x × capture_width / overlay_width`).
- The 100×100 preview square is redrawn with the sampled color, a contrast-aware hex label (embedded 5×7 bitmap font, rendered at 2× scale), and a 1-pixel border.
- Overlay surfaces are double-buffered to prevent tearing or lag on non-primary outputs. `wl_display_flush` is called inside each commit to avoid dispatch-cycle latency.

On left-click: transitions to the clipboard phase (see below).

On right-click: transitions to the recapture phase (see below).

On Escape: exits with code 0.

### 3. Recapture phase

Triggered by right-click when the initial capture may not have included hardware-accelerated content. All overlay surfaces are blanked and the cursor is hidden, then a fresh capture session is started on every output. Because Wayland requests are processed in wire order, the compositor applies the blank commits before rendering the captured frame, ensuring the preview square does not appear in the new screenshot. Once all captures complete, the cursor and overlay are restored and the app returns to the pick phase.

### 4. Clipboard phase

On left-click, the color string is offered via `wl_data_source` / `wl_data_device`. The overlay and cursor surfaces are destroyed immediately. The event loop then blocks until another app pastes (triggering `wl_data_source.send`) or replaces the selection (`wl_data_source.cancelled`), at which point the process exits cleanly.

## Wayland protocols

| Protocol | Vendor | Purpose |
|---|---|---|
| Core Wayland (`wl_compositor`, `wl_shm`, `wl_output`) | Standard | Surfaces, shared-memory pixel buffers, output enumeration |
| Core Wayland (`wl_seat`, `wl_pointer`, `wl_keyboard`) | Standard | Pointer motion/button events, keyboard (Escape), custom cursor |
| Core Wayland (`wl_data_device_manager`) | Standard | Clipboard selection |
| `zwlr_layer_shell_v1` | wlr-protocols (vendored) | Fullscreen transparent overlay above all windows |
| `ext_output_image_capture_source_manager_v1` | wayland-protocols staging (vendored) | Creates a capture source from an output |
| `ext_image_copy_capture_manager_v1` | wayland-protocols staging (vendored) | Copies output pixels into a shared-memory buffer |

Protocol XML files are vendored under `protocol/`. C bindings are generated at build time by `wayland-scanner`.

"Vendored" means the XML files are checked into this repository rather than depending on them being installed on the build machine. This avoids a build-time dependency on `wayland-protocols` and `wlr-protocols` packages, which may be absent, outdated, or carry different versions of the protocol than the app was written against.

The files and their upstream sources:

| File | Upstream repository | Notes |
|---|---|---|
| `protocol/ext-image-capture-source-v1.xml` | [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) — `staging/ext-image-capture-source/` | Staging protocol; not yet in most distro packages |
| `protocol/ext-image-copy-capture-v1.xml` | [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) — `staging/ext-image-copy-capture/` | Staging protocol; not yet in most distro packages |
| `protocol/wlr-layer-shell-unstable-v1.xml` | [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) — `unstable/` | wlroots-specific; not part of the Wayland standard |
| `protocol/xdg-shell.xml` | [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) — `stable/xdg-shell/` | Not used directly by the app, but `wlr-layer-shell` references `xdg_popup_interface` internally, so the generated code requires it at link time |

If you need to update a protocol, replace the relevant file in `protocol/` with the new version from upstream and rebuild.

### `src/stubs.c`

`wayland-scanner` generates interface type definitions for every interface mentioned in a protocol XML, including interfaces that are only referenced as argument types and never actually used by the app. If those referenced interfaces live in a *different* protocol, the linker ends up with an undefined symbol.

`src/stubs.c` provides minimal stub definitions for these dangling references:

- `ext_foreign_toplevel_handle_v1_interface` — referenced by `ext-image-capture-source-v1` (the capture source protocol can optionally target a foreign toplevel window rather than a whole output; the app only uses the output path, but the scanner still emits the type reference)

If a future protocol update introduces new such references, add a corresponding stub definition here.

## Dependencies

### Runtime

- **libwayland-client** — present on any Wayland system

### Build

| Tool | Purpose |
|---|---|
| `meson` (≥ 0.60) | Build system |
| `ninja` | Build backend |
| `gcc` or `clang` | C11 compiler |
| `wayland-scanner` | Generates C protocol bindings from XML at build time |
| `libwayland-client` (dev headers) | e.g. `libwayland-dev` on Debian/Ubuntu, `wayland` on Arch |
| `clang-tidy` | Linting (optional, for development) |
| `clang-format` | Formatting (optional, for development) |

On Arch Linux: `sudo pacman -S meson ninja clang wayland wayland-protocols`  
On Debian/Ubuntu: `sudo apt install meson ninja-build gcc libwayland-dev wayland-scanner`

## Building

```sh
meson setup build
meson compile -C build
```

The compiled binary is at `./build/wayland-color-picker`.

### Running

```sh
./build/wayland-color-picker
```

### Linting

```sh
clang-tidy src/main.c -- -std=gnu11 -D_GNU_SOURCE -I build -I .
```

### Formatting

```sh
clang-format -i src/*.c
```

### VS Code IntelliSense

After running `meson setup build`, IntelliSense is configured via `.vscode/c_cpp_properties.json` to read `build/compile_commands.json`. This gives VS Code the correct include paths for the generated protocol headers without any manual configuration.

