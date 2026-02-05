/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright 2026 Furi Labs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Bardia Moshiri <bardia@furilabs.com>
 */

#include "config.h"

#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-virtual-pointer.h"

#include "wlr-virtual-pointer-unstable-v1-server-protocol.h"

typedef struct _MetaWaylandVirtualPointer
{
  MetaWaylandCompositor *compositor;

  struct wl_resource *resource;

  ClutterVirtualInputDevice *virtual_pointer;

  graphene_point_t last_coords;
  gboolean have_last_coords;

  enum wl_pointer_axis_source axis_source;
} MetaWaylandVirtualPointer;

#ifndef BTN_LEFT
#define BTN_LEFT   0x110
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT  0x111
#endif
#ifndef BTN_MIDDLE
#define BTN_MIDDLE 0x112
#endif

static uint32_t
evdev_button_to_logical (uint32_t evdev_button)
{
  switch (evdev_button) {
  case BTN_LEFT:
    return 1; /* primary */
  case BTN_MIDDLE:
    return 2; /* middle */
  case BTN_RIGHT:
    return 3; /* secondary */
  default:
    return 0;
  }
}

static ClutterVirtualInputDevice *
create_virtual_device_like (ClutterSeat        *seat,
                            ClutterInputDevice *like_device)
{
  ClutterSeatClass *klass;
  ClutterInputDeviceType device_type;

  if (!seat || !like_device)
    return NULL;

  klass = CLUTTER_SEAT_GET_CLASS (seat);
  if (!klass || !klass->create_virtual_device)
    return NULL;

  device_type = clutter_input_device_get_device_type (like_device);
  return klass->create_virtual_device (seat, device_type);
}

static ClutterVirtualInputDevice *
create_virtual_pointer_device (MetaWaylandCompositor *compositor)
{
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_backend);
  ClutterSeatClass *klass;
  ClutterInputDevice *real_pointer;

  if (!seat)
    return NULL;

  klass = CLUTTER_SEAT_GET_CLASS (seat);
  if (!klass || !klass->create_virtual_device)
    return NULL;

  real_pointer = clutter_seat_get_pointer (seat);
  if (real_pointer)
    return create_virtual_device_like (seat, real_pointer);

  return klass->create_virtual_device (seat, CLUTTER_POINTER_DEVICE);
}

static void
get_stage_size (MetaWaylandCompositor *compositor,
                float                 *w_out,
                float                 *h_out)
{
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  float w = 0.0f, h = 0.0f;

  clutter_actor_get_size (CLUTTER_ACTOR (stage), &w, &h);

  if (w <= 0.0f)
    w = 1.0f;
  if (h <= 0.0f)
    h = 1.0f;

  *w_out = w;
  *h_out = h;
}

static ClutterScrollSource
scroll_source_from_wl (enum wl_pointer_axis_source src)
{
  switch (src) {
  case WL_POINTER_AXIS_SOURCE_WHEEL:
    return CLUTTER_SCROLL_SOURCE_WHEEL;
  case WL_POINTER_AXIS_SOURCE_FINGER:
    return CLUTTER_SCROLL_SOURCE_FINGER;
  case WL_POINTER_AXIS_SOURCE_CONTINUOUS:
    return CLUTTER_SCROLL_SOURCE_CONTINUOUS;
  default:
    return CLUTTER_SCROLL_SOURCE_WHEEL;
  }
}

static void
ensure_pointer_initialized (MetaWaylandVirtualPointer *vp,
                            uint64_t                   ts_us)
{
  if (vp->have_last_coords)
    return;

  float w, h;
  get_stage_size (vp->compositor, &w, &h);

  vp->last_coords.x = w / 2.0f;
  vp->last_coords.y = h / 2.0f;
  vp->have_last_coords = TRUE;

  /* seed absolute pos once so downstream paths have a baseline. */
  clutter_virtual_input_device_notify_absolute_motion (vp->virtual_pointer,
                                                       ts_us,
                                                       (double) vp->last_coords.x,
                                                       (double) vp->last_coords.y);
}

static void
zwlr_virtual_pointer_v1_motion (struct wl_client   *client,
                                struct wl_resource *resource,
                                uint32_t            time,
                                wl_fixed_t          dx,
                                wl_fixed_t          dy)
{
  MetaWaylandVirtualPointer *vp = wl_resource_get_user_data (resource);
  (void) client;

  if (!vp || !vp->virtual_pointer)
    return;

  uint64_t ts_us = (uint64_t) time * 1000;
  float w, h;

  const double ddx = wl_fixed_to_double (dx);
  const double ddy = wl_fixed_to_double (dy);

  ensure_pointer_initialized (vp, ts_us);
  get_stage_size (vp->compositor, &w, &h);

  vp->last_coords.x += (float) ddx;
  vp->last_coords.y += (float) ddy;

  /* clamp to stage bounds */
  const float max_x = (w > 1.0f) ? (w - 1.0f) : 0.0f;
  const float max_y = (h > 1.0f) ? (h - 1.0f) : 0.0f;

  if (vp->last_coords.x < 0.0f)
    vp->last_coords.x = 0.0f;
  if (vp->last_coords.y < 0.0f)
    vp->last_coords.y = 0.0f;
  if (vp->last_coords.x > max_x)
    vp->last_coords.x = max_x;
  if (vp->last_coords.y > max_y)
    vp->last_coords.y = max_y;

  clutter_virtual_input_device_notify_absolute_motion (vp->virtual_pointer,
                                                       ts_us,
                                                       (double) vp->last_coords.x,
                                                       (double) vp->last_coords.y);
}

static void
zwlr_virtual_pointer_v1_motion_absolute (struct wl_client   *client,
                                         struct wl_resource *resource,
                                         uint32_t            time,
                                         uint32_t            x,
                                         uint32_t            y,
                                         uint32_t            x_extent,
                                         uint32_t            y_extent)
{
  MetaWaylandVirtualPointer *vp = wl_resource_get_user_data (resource);
  (void) client;

  if (!vp || !vp->virtual_pointer)
    return;

  uint64_t ts_us = (uint64_t) time * 1000;
  float stage_w, stage_h;

  if (x_extent == 0)
    x_extent = 1;
  if (y_extent == 0)
    y_extent = 1;

  get_stage_size (vp->compositor, &stage_w, &stage_h);

  const double abs_x = ((double) x * (double) stage_w) / (double) x_extent;
  const double abs_y = ((double) y * (double) stage_h) / (double) y_extent;

  vp->last_coords.x = (float) abs_x;
  vp->last_coords.y = (float) abs_y;
  vp->have_last_coords = TRUE;

  clutter_virtual_input_device_notify_absolute_motion (vp->virtual_pointer,
                                                       ts_us,
                                                       abs_x,
                                                       abs_y);
}

static void
zwlr_virtual_pointer_v1_button (struct wl_client   *client,
                                struct wl_resource *resource,
                                uint32_t            time,
                                uint32_t            button,
                                uint32_t            state)
{
  MetaWaylandVirtualPointer *vp = wl_resource_get_user_data (resource);
  (void) client;

  if (!vp || !vp->virtual_pointer)
    return;

  uint64_t ts_us = (uint64_t) time * 1000;

  uint32_t logical = evdev_button_to_logical (button);
  if (logical == 0)
    return;

  ClutterButtonState bs = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? CLUTTER_BUTTON_STATE_PRESSED
                                                                     : CLUTTER_BUTTON_STATE_RELEASED;

  clutter_virtual_input_device_notify_button (vp->virtual_pointer,
                                              ts_us,
                                              logical,
                                              bs);
}

static void
zwlr_virtual_pointer_v1_axis (struct wl_client   *client,
                              struct wl_resource *resource,
                              uint32_t            time,
                              uint32_t            axis,
                              wl_fixed_t          value)
{
  MetaWaylandVirtualPointer *vp = wl_resource_get_user_data (resource);
  (void) client;

  if (!vp || !vp->virtual_pointer)
    return;

  uint64_t ts_us = (uint64_t) time * 1000;

  double dx = 0.0, dy = 0.0;
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
    dy = wl_fixed_to_double (value);
  else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
    dx = wl_fixed_to_double (value);
  else
    return;

  clutter_virtual_input_device_notify_scroll_continuous (vp->virtual_pointer,
                                                         ts_us,
                                                         dx,
                                                         dy,
                                                         scroll_source_from_wl (vp->axis_source),
                                                         0);
}

static void
zwlr_virtual_pointer_v1_frame (struct wl_client   *client,
                               struct wl_resource *resource)
{
  (void) client;
  (void) resource;
}

static void
zwlr_virtual_pointer_v1_axis_source (struct wl_client   *client,
                                     struct wl_resource *resource,
                                     uint32_t            axis_source)
{
  MetaWaylandVirtualPointer *vp = wl_resource_get_user_data (resource);
  (void) client;

  if (!vp)
    return;

  vp->axis_source = axis_source;
}

static void
zwlr_virtual_pointer_v1_axis_stop (struct wl_client   *client,
                                   struct wl_resource *resource,
                                   uint32_t            time,
                                   uint32_t            axis)
{
  (void) client;
  (void) resource;
  (void) time;
  (void) axis;
}

static void
zwlr_virtual_pointer_v1_axis_discrete (struct wl_client   *client,
                                       struct wl_resource *resource,
                                       uint32_t            time,
                                       uint32_t            axis,
                                       wl_fixed_t          value,
                                       int32_t             discrete)
{
  (void) discrete;
  zwlr_virtual_pointer_v1_axis (client, resource, time, axis, value);
}

static void
zwlr_virtual_pointer_v1_destroy (struct wl_client   *client,
                                 struct wl_resource *resource)
{
  (void) client;
  wl_resource_destroy (resource);
}

static const struct zwlr_virtual_pointer_v1_interface virtual_pointer_interface =
{
  zwlr_virtual_pointer_v1_motion,
  zwlr_virtual_pointer_v1_motion_absolute,
  zwlr_virtual_pointer_v1_button,
  zwlr_virtual_pointer_v1_axis,
  zwlr_virtual_pointer_v1_frame,
  zwlr_virtual_pointer_v1_axis_source,
  zwlr_virtual_pointer_v1_axis_stop,
  zwlr_virtual_pointer_v1_axis_discrete,
  zwlr_virtual_pointer_v1_destroy,
};

static void
virtual_pointer_resource_destroy (struct wl_resource *resource)
{
  MetaWaylandVirtualPointer *vp = wl_resource_get_user_data (resource);
  if (!vp)
    return;

  if (vp->virtual_pointer)
    g_object_unref (vp->virtual_pointer);

  g_free (vp);
}

static void
zwlr_virtual_pointer_manager_v1_create_virtual_pointer (struct wl_client   *client,
                                                        struct wl_resource *resource,
                                                        struct wl_resource *seat_resource,
                                                        uint32_t            id)
{
  MetaWaylandCompositor *compositor = wl_resource_get_user_data (resource);
  MetaWaylandVirtualPointer *vp;
  struct wl_resource *vp_res;

  (void) seat_resource;

  if (!compositor) {
    wl_client_post_no_memory (client);
    return;
  }

  vp = g_new0 (MetaWaylandVirtualPointer, 1);
  vp->compositor = compositor;
  vp->axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;

  vp->virtual_pointer = create_virtual_pointer_device (compositor);
  if (!vp->virtual_pointer) {
    g_free (vp);
    wl_client_post_no_memory (client);
    return;
  }

  vp_res = wl_resource_create (client,
                               &zwlr_virtual_pointer_v1_interface,
                               wl_resource_get_version (resource),
                               id);
  if (!vp_res) {
    g_object_unref (vp->virtual_pointer);
    g_free (vp);
    wl_client_post_no_memory (client);
    return;
  }

  vp->resource = vp_res;

  wl_resource_set_implementation (vp_res,
                                  &virtual_pointer_interface,
                                  vp,
                                  virtual_pointer_resource_destroy);
}

static void
zwlr_virtual_pointer_manager_v1_destroy (struct wl_client   *client,
                                         struct wl_resource *resource)
{
  (void) client;
  wl_resource_destroy (resource);
}

static void
zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output (struct wl_client   *client,
                                                                    struct wl_resource *resource,
                                                                    struct wl_resource *seat_resource,
                                                                    struct wl_resource *output_resource,
                                                                    uint32_t            id)
{
  (void) output_resource;
  zwlr_virtual_pointer_manager_v1_create_virtual_pointer (client, resource, seat_resource, id);
}

static const struct zwlr_virtual_pointer_manager_v1_interface virtual_pointer_manager_interface =
{
  zwlr_virtual_pointer_manager_v1_create_virtual_pointer,
  zwlr_virtual_pointer_manager_v1_destroy,
  zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output,
};

static void
bind_virtual_pointer_manager (struct wl_client *client,
                              void             *data,
                              uint32_t          version,
                              uint32_t          id)
{
  MetaWaylandCompositor *compositor = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client,
                                 &zwlr_virtual_pointer_manager_v1_interface,
                                 version,
                                 id);
  if (!resource) {
    wl_client_post_no_memory (client);
    return;
  }

  wl_resource_set_implementation (resource,
                                  &virtual_pointer_manager_interface,
                                  compositor,
                                  NULL);
}

void
meta_wayland_virtual_pointer_init (MetaWaylandCompositor *compositor)
{
  if (!wl_global_create (compositor->wayland_display,
                         &zwlr_virtual_pointer_manager_v1_interface,
                         2,
                         compositor,
                         bind_virtual_pointer_manager))
    g_error ("Failed to create zwlr_virtual_pointer_manager_v1 global");
}
