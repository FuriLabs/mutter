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

#include "backends/wayland-nested/meta-virtual-input-device-wayland-nested.h"

#include "clutter/clutter-event-private.h"
#include "clutter/clutter-seat-private.h"
#include "clutter/clutter-virtual-input-device.h"

#include "backends/wayland-nested/meta-seat-wayland-nested.h"
#include "backends/wayland-nested/meta-virtual-input-device-wayland-nested.h"

struct _MetaVirtualInputDeviceWaylandNested
{
  ClutterVirtualInputDevice parent_instance;

  ClutterSeat *seat;

  float x;
  float y;
};

G_DEFINE_FINAL_TYPE (MetaVirtualInputDeviceWaylandNested,
                     meta_virtual_input_device_wayland_nested,
                     CLUTTER_TYPE_VIRTUAL_INPUT_DEVICE)

static inline void
push_event (ClutterEvent *event)
{
  if (!event)
    return;

  clutter_event_put (event);
  clutter_event_free (event);
}

static inline ClutterInputDevice *
get_pointer_source (MetaVirtualInputDeviceWaylandNested *self)
{
  if (!self->seat)
    return NULL;
  return clutter_seat_get_pointer (self->seat);
}

static inline ClutterInputDevice *
get_keyboard_source (MetaVirtualInputDeviceWaylandNested *self)
{
  if (!self->seat)
    return NULL;
  return clutter_seat_get_keyboard (self->seat);
}

static void
notify_relative_motion (ClutterVirtualInputDevice *vdev,
                        uint64_t                   time_us,
                        double                     dx,
                        double                     dy)
{
  MetaVirtualInputDeviceWaylandNested *self =
    META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_pointer_source (self);
  if (!source)
    return;

  self->x += (float) dx;
  self->y += (float) dy;

  if (META_IS_SEAT_WAYLAND_NESTED (self->seat))
    meta_seat_wayland_nested_update_pointer_position (META_SEAT_WAYLAND_NESTED (self->seat),
                                                      self->x, self->y);

  graphene_point_t coords = GRAPHENE_POINT_INIT (self->x, self->y);
  graphene_point_t delta  = GRAPHENE_POINT_INIT ((float) dx, (float) dy);

  ClutterEvent *event = clutter_event_motion_new (0,
                                                  (int64_t) time_us,
                                                  source,
                                                  NULL,
                                                  0,
                                                  coords,
                                                  delta,
                                                  delta,
                                                  delta,
                                                  NULL);

  push_event (event);
}

static void
notify_absolute_motion (ClutterVirtualInputDevice *vdev,
                        uint64_t                   time_us,
                        double                     x,
                        double                     y)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_pointer_source (self);
  if (!source)
    return;

  self->x = (float) x;
  self->y = (float) y;

  if (META_IS_SEAT_WAYLAND_NESTED (self->seat))
    meta_seat_wayland_nested_update_pointer_position (META_SEAT_WAYLAND_NESTED (self->seat),
                                                      self->x, self->y);

  graphene_point_t coords = GRAPHENE_POINT_INIT (self->x, self->y);
  graphene_point_t delta  = GRAPHENE_POINT_INIT (0.f, 0.f);

  ClutterEvent *event = clutter_event_motion_new (0,
                                                  (int64_t) time_us,
                                                  source,
                                                  NULL,
                                                  0,
                                                  coords,
                                                  delta,
                                                  delta,
                                                  delta,
                                                  NULL);

  push_event (event);
}

static void
notify_button (ClutterVirtualInputDevice *vdev,
               uint64_t                   time_us,
               uint32_t                   button,
               ClutterButtonState         button_state)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_pointer_source (self);
  if (!source)
    return;

  ClutterEventType type = (button_state == CLUTTER_BUTTON_STATE_PRESSED) ? CLUTTER_BUTTON_PRESS : CLUTTER_BUTTON_RELEASE;

  graphene_point_t coords = GRAPHENE_POINT_INIT (self->x, self->y);

  ClutterEvent *event = clutter_event_button_new (type,
                                                  0,
                                                  (int64_t) time_us,
                                                  source,
                                                  NULL,
                                                  0,
                                                  coords,
                                                  (int) button,
                                                  0,
                                                  NULL);

  push_event (event);
}

static void
notify_key (ClutterVirtualInputDevice *vdev,
            uint64_t                   time_us,
            uint32_t                   key,
            ClutterKeyState            key_state)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_keyboard_source (self);
  if (!source)
    return;

  ClutterEventType type = (key_state == CLUTTER_KEY_STATE_PRESSED) ? CLUTTER_KEY_PRESS : CLUTTER_KEY_RELEASE;

  xkb_keysym_t sym = XKB_KEY_NoSymbol;
  uint32_t unicode_value = 0;

  if (META_IS_SEAT_WAYLAND_NESTED (self->seat)) {
    MetaSeatWaylandNested *seat = META_SEAT_WAYLAND_NESTED (self->seat);
    struct xkb_state *state = meta_seat_wayland_nested_peek_xkb_state (seat);

    if (state) {
//      xkb_keycode_t xkb_code = (xkb_keycode_t) key + 8;

      enum xkb_key_direction dir = key_state == (CLUTTER_KEY_STATE_PRESSED) ? XKB_KEY_DOWN : XKB_KEY_UP;

      /* update state first so modifiers and level selection are correct */
      xkb_state_update_key (state, /* xkb_code */ key, dir);

      sym = xkb_state_key_get_one_sym (state, /* xkb_code*/ key);
      if (sym != XKB_KEY_NoSymbol)
        unicode_value = xkb_keysym_to_utf32 (sym);
    }
  }

  ClutterModifierSet raw = { 0, 0, 0 };

  ClutterEvent *event = clutter_event_key_new (type,
                                               0,
                                               (int64_t) time_us,
                                               source,
                                               raw,
                                               0,
                                               (uint32_t) sym, /* keyval (keysym) */
                                               key,            /* evcode */
                                               key,            /* keycode */
                                               unicode_value);

  push_event (event);
}

static void
notify_keyval (ClutterVirtualInputDevice *vdev,
               uint64_t                   time_us,
               uint32_t                   keyval,
               ClutterKeyState            key_state)
{
  MetaVirtualInputDeviceWaylandNested *self =
    META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_keyboard_source (self);
  if (!source)
    return;

  ClutterEventType type = (key_state == CLUTTER_KEY_STATE_PRESSED) ? CLUTTER_KEY_PRESS : CLUTTER_KEY_RELEASE;

  uint32_t unicode_value = 0;
  unicode_value = xkb_keysym_to_utf32 ((xkb_keysym_t) keyval);

  ClutterModifierSet raw = { 0, 0, 0 };

  ClutterEvent *event = clutter_event_key_new (type,
                                               0,
                                               (int64_t) time_us,
                                               source,
                                               raw,
                                               0,
                                               keyval,
                                               0,
                                               0,
                                               unicode_value);

  push_event (event);
}

static void
notify_discrete_scroll (ClutterVirtualInputDevice *vdev,
                        uint64_t                   time_us,
                        ClutterScrollDirection     direction,
                        ClutterScrollSource        scroll_source)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_pointer_source (self);
  if (!source)
    return;

  graphene_point_t coords = GRAPHENE_POINT_INIT (self->x, self->y);

  ClutterEvent *event = clutter_event_scroll_discrete_new (0,
                                                           (int64_t) time_us,
                                                           source,
                                                           NULL,
                                                           0,
                                                           coords,
                                                           0,
                                                           scroll_source,
                                                           direction);

  push_event (event);
}

static void
notify_scroll_continuous (ClutterVirtualInputDevice *vdev,
                          uint64_t                   time_us,
                          double                     dx,
                          double                     dy,
                          ClutterScrollSource        scroll_source,
                          ClutterScrollFinishFlags   finish_flags)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);

  ClutterInputDevice *source = get_pointer_source (self);
  if (!source)
    return;

  graphene_point_t coords = GRAPHENE_POINT_INIT (self->x, self->y);
  graphene_point_t delta  = GRAPHENE_POINT_INIT ((float) dx, (float) dy);

  ClutterEvent *event = clutter_event_scroll_smooth_new (0,
                                                         (int64_t) time_us,
                                                         source,
                                                         NULL,
                                                         0,
                                                         coords,
                                                         delta,
                                                         0,
                                                         scroll_source,
                                                         finish_flags);

  push_event (event);
}

static void
meta_virtual_input_device_wayland_nested_init (MetaVirtualInputDeviceWaylandNested *self)
{
  self->seat = NULL;
  self->x = 0.f;
  self->y = 0.f;
}

static void
meta_virtual_input_device_wayland_nested_constructed (GObject *object)
{
  G_OBJECT_CLASS (meta_virtual_input_device_wayland_nested_parent_class)->constructed (object);

  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (object);

  self->x = 0.f;
  self->y = 0.f;
}

static void
meta_virtual_input_device_wayland_nested_class_init (MetaVirtualInputDeviceWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->constructed = meta_virtual_input_device_wayland_nested_constructed;

  ClutterVirtualInputDeviceClass *vclass = CLUTTER_VIRTUAL_INPUT_DEVICE_CLASS (klass);

  vclass->notify_relative_motion = notify_relative_motion;
  vclass->notify_absolute_motion = notify_absolute_motion;
  vclass->notify_button = notify_button;

  vclass->notify_key = notify_key;
  vclass->notify_keyval = notify_keyval;

  vclass->notify_discrete_scroll = notify_discrete_scroll;
  vclass->notify_scroll_continuous = notify_scroll_continuous;
}

MetaVirtualInputDeviceWaylandNested *
meta_virtual_input_device_wayland_nested_new (ClutterSeat            *seat,
                                              ClutterInputDeviceType  device_type)
{
  MetaVirtualInputDeviceWaylandNested *vdev = g_object_new (META_TYPE_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED,
                                                            "seat", seat,
                                                            "device-type", device_type,
                                                            NULL);

  vdev->seat = seat;

  return vdev;
}
