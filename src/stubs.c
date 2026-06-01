/* Stub definitions for protocol interfaces that are referenced by generated
 * scanner output but are not used by this application.                      */

#include <wayland-client.h>

/* ext-image-capture-source-v1.xml references ext_foreign_toplevel_handle_v1
 * from an interface we do not use.  Provide a minimal definition so the
 * linker is satisfied.                                                       */
const struct wl_interface ext_foreign_toplevel_handle_v1_interface = {
    "ext_foreign_toplevel_handle_v1", 1, 0, NULL, 0, NULL,
};
