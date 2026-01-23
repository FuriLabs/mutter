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

#include "backends/wayland-nested/meta-seat-wayland-nested.h"
#include "backends/wayland-nested/meta-input-device-wayland-nested.h"

struct _MetaInputDeviceWaylandNested
{
  MetaInputDevice parent_instance;
};

G_DEFINE_TYPE (MetaInputDeviceWaylandNested,
               meta_input_device_wayland_nested,
               META_TYPE_INPUT_DEVICE)

static void
meta_input_device_wayland_nested_class_init (MetaInputDeviceWaylandNestedClass *klass)
{
  (void) klass;
}

static void
meta_input_device_wayland_nested_init (MetaInputDeviceWaylandNested *self)
{
  (void) self;
}

static const char *
device_type_to_name (ClutterInputDeviceType device_type)
{
  switch (device_type) {
  case CLUTTER_POINTER_DEVICE:
    return "Wayland Nested Pointer";
  case CLUTTER_KEYBOARD_DEVICE:
    return "Wayland Nested Keyboard";
  case CLUTTER_TOUCHSCREEN_DEVICE:
    return "Wayland Nested Touchscreen";
  case CLUTTER_TABLET_DEVICE:
    return "Wayland Nested Tablet";
  case CLUTTER_PEN_DEVICE:
    return "Wayland Nested Pen";
  case CLUTTER_ERASER_DEVICE:
    return "Wayland Nested Eraser";
  case CLUTTER_PAD_DEVICE:
    return "Wayland Nested Pad";
  default:
    return "Wayland Nested Input Device";
  }
}

static ClutterInputCapabilities
device_type_to_capabilities (ClutterInputDeviceType device_type)
{
  switch (device_type) {
  case CLUTTER_POINTER_DEVICE:
    return (CLUTTER_INPUT_CAPABILITY_POINTER |
            CLUTTER_INPUT_CAPABILITY_TOUCHPAD |
            CLUTTER_INPUT_CAPABILITY_TRACKBALL |
            CLUTTER_INPUT_CAPABILITY_TRACKPOINT);
  case CLUTTER_KEYBOARD_DEVICE:
    return CLUTTER_INPUT_CAPABILITY_KEYBOARD;
  case CLUTTER_TOUCHSCREEN_DEVICE:
    return CLUTTER_INPUT_CAPABILITY_TOUCH;
  default:
    return CLUTTER_INPUT_CAPABILITY_NONE;
  }
}

ClutterInputDevice *
meta_input_device_wayland_nested_new (ClutterSeat            *seat,
                                      ClutterInputDeviceType  device_type)
{
  MetaBackend *backend = NULL;
  MetaInputDeviceWaylandNested *self;
  const char *name;
  ClutterInputCapabilities caps;

  g_return_val_if_fail (CLUTTER_IS_SEAT (seat), NULL);

  if (META_IS_SEAT_WAYLAND_NESTED (seat))
    backend = meta_seat_wayland_nested_get_backend (META_SEAT_WAYLAND_NESTED (seat));

  name = device_type_to_name (device_type);
  caps = device_type_to_capabilities (device_type);

  if (backend)
    self = g_object_new (META_TYPE_INPUT_DEVICE_WAYLAND_NESTED,
                         "seat", seat,
                         "backend", backend,
                         "device-type", device_type,
                         "name", name,
                         "device-mode", CLUTTER_INPUT_MODE_PHYSICAL,
                         "capabilities", caps,
                         NULL);
  else
    self = g_object_new (META_TYPE_INPUT_DEVICE_WAYLAND_NESTED,
                         "seat", seat,
                         "device-type", device_type,
                         "name", name,
                         "device-mode", CLUTTER_INPUT_MODE_PHYSICAL,
                         "capabilities", caps,
                         NULL);

  return CLUTTER_INPUT_DEVICE (self);
}
