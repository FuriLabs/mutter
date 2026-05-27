#pragma once

#include "wayland/meta-wayland-private.h"

#include <glib.h>
#include <wayland-server.h>

void meta_wayland_android_wlegl_init (MetaWaylandCompositor *compositor);

gboolean meta_wayland_android_wlegl_buffer_is_android (struct wl_resource *resource);

gboolean meta_wayland_android_wlegl_buffer_get_size (struct wl_resource *resource,
                                                     int                *width,
                                                     int                *height);

int meta_wayland_android_wlegl_buffer_get_format (struct wl_resource *resource);
int meta_wayland_android_wlegl_buffer_get_stride (struct wl_resource *resource);
int meta_wayland_android_wlegl_buffer_get_usage (struct wl_resource *resource);

const struct wl_array *
meta_wayland_android_wlegl_buffer_get_ints (struct wl_resource *resource);

GArray *
meta_wayland_android_wlegl_buffer_get_fds (struct wl_resource *resource);
