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

#include "backends/meta-backend-private.h"

#include "backends/wayland-nested/meta-seat-wayland-nested.h"
#include "backends/wayland-nested/meta-input-device-wayland-nested.h"
#include "backends/wayland-nested/meta-keymap-wayland-nested.h"
#include "backends/wayland-nested/meta-virtual-input-device-wayland-nested.h"

enum
{
  PROP_0,
  PROP_BACKEND,
  N_PROPS
};

static GParamSpec *props[N_PROPS];

struct _MetaSeatWaylandNested
{
  ClutterSeat parent_instance;

  MetaBackend *backend;

  ClutterInputDevice *pointer_device;
  ClutterInputDevice *keyboard_device;

  GList *devices;

  ClutterKeymap *keymap;
  xkb_layout_index_t layout_index;

  graphene_point_t pointer_pos;

  ClutterModifierType modifiers;

  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
};

G_DEFINE_TYPE (MetaSeatWaylandNested,
               meta_seat_wayland_nested,
               CLUTTER_TYPE_SEAT)

void
meta_seat_wayland_nested_start (MetaSeatWaylandNested *self)
{
  (void) self;
}

static void
meta_seat_wayland_nested_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (object);

  switch (prop_id) {
  case PROP_BACKEND:
    g_set_object (&self->backend, g_value_get_object (value));
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
meta_seat_wayland_nested_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (object);

  switch (prop_id) {
  case PROP_BACKEND:
    g_value_set_object (value, self->backend);
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static ClutterInputDevice *
meta_seat_wayland_nested_get_pointer (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return self->pointer_device;
}

static ClutterInputDevice *
meta_seat_wayland_nested_get_keyboard (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return self->keyboard_device;
}

static const GList *
meta_seat_wayland_nested_peek_devices (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return self->devices;
}

static void
meta_seat_wayland_nested_bell_notify (ClutterSeat *seat)
{
  (void) seat;
}

static ClutterKeymap *
meta_seat_wayland_nested_get_keymap (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return self->keymap;
}

static void
meta_seat_wayland_nested_init_pointer_position (ClutterSeat *seat,
                                                float        x,
                                                float        y)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  self->pointer_pos = GRAPHENE_POINT_INIT (x, y);
}

static void
meta_seat_wayland_nested_warp_pointer (ClutterSeat *seat,
                                       int          x,
                                       int          y)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  self->pointer_pos = GRAPHENE_POINT_INIT ((float) x, (float) y);
}

static gboolean
meta_seat_wayland_nested_query_state (ClutterSeat         *seat,
                                      ClutterSprite       *sprite,
                                      graphene_point_t    *coords,
                                      ClutterModifierType *modifiers)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);

  (void) sprite;

  if (coords)
    *coords = self->pointer_pos;

  if (modifiers)
    *modifiers = self->modifiers;

  return TRUE;
}

static ClutterVirtualInputDevice *
meta_seat_wayland_nested_create_virtual_device (ClutterSeat            *seat,
                                                ClutterInputDeviceType  device_type)
{
  return CLUTTER_VIRTUAL_INPUT_DEVICE (meta_virtual_input_device_wayland_nested_new (seat, device_type));
}

static ClutterVirtualDeviceType
meta_seat_wayland_nested_get_supported_virtual_device_types (ClutterSeat *seat)
{
  (void) seat;

  return (CLUTTER_VIRTUAL_DEVICE_TYPE_KEYBOARD |
          CLUTTER_VIRTUAL_DEVICE_TYPE_POINTER |
          CLUTTER_VIRTUAL_DEVICE_TYPE_TOUCHSCREEN);
}

static void
clear_xkb_state (MetaSeatWaylandNested *self)
{
  if (self->xkb_state) {
    xkb_state_unref (self->xkb_state);
    self->xkb_state = NULL;
  }

  if (self->xkb_keymap) {
    xkb_keymap_unref (self->xkb_keymap);
    self->xkb_keymap = NULL;
  }
}

static void
meta_seat_wayland_nested_dispose (GObject *object)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (object);

  g_clear_object (&self->pointer_device);
  g_clear_object (&self->keyboard_device);

  g_list_free_full (self->devices, g_object_unref);
  self->devices = NULL;

  g_clear_object (&self->keymap);

  clear_xkb_state (self);

  g_clear_object (&self->backend);

  G_OBJECT_CLASS (meta_seat_wayland_nested_parent_class)->dispose (object);
}

static void
meta_seat_wayland_nested_constructed (GObject *object)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (object);

  G_OBJECT_CLASS (meta_seat_wayland_nested_parent_class)->constructed (object);

  self->pointer_device = meta_input_device_wayland_nested_new (CLUTTER_SEAT (self),
                                                               CLUTTER_POINTER_DEVICE);

  self->keyboard_device = meta_input_device_wayland_nested_new (CLUTTER_SEAT (self),
                                                                CLUTTER_KEYBOARD_DEVICE);

  if (self->pointer_device)
    self->devices = g_list_append (self->devices, g_object_ref (self->pointer_device));
  if (self->keyboard_device)
    self->devices = g_list_append (self->devices, g_object_ref (self->keyboard_device));

  if (!self->keymap)
    self->keymap = g_object_new (META_TYPE_KEYMAP_WAYLAND_NESTED, NULL);
}

static void
meta_seat_wayland_nested_class_init (MetaSeatWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterSeatClass *seat_class = CLUTTER_SEAT_CLASS (klass);

  object_class->constructed = meta_seat_wayland_nested_constructed;
  object_class->dispose = meta_seat_wayland_nested_dispose;
  object_class->set_property = meta_seat_wayland_nested_set_property;
  object_class->get_property = meta_seat_wayland_nested_get_property;

  props[PROP_BACKEND] = g_param_spec_object ("backend",
                                             "Backend",
                                             "MetaBackend owning this seat",
                                             META_TYPE_BACKEND,
                                             G_PARAM_READWRITE |
                                             G_PARAM_CONSTRUCT_ONLY |
                                             G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  seat_class->get_pointer = meta_seat_wayland_nested_get_pointer;
  seat_class->get_keyboard = meta_seat_wayland_nested_get_keyboard;
  seat_class->peek_devices = meta_seat_wayland_nested_peek_devices;
  seat_class->bell_notify = meta_seat_wayland_nested_bell_notify;
  seat_class->get_keymap = meta_seat_wayland_nested_get_keymap;

  seat_class->create_virtual_device = meta_seat_wayland_nested_create_virtual_device;
  seat_class->get_supported_virtual_device_types = meta_seat_wayland_nested_get_supported_virtual_device_types;

  seat_class->init_pointer_position = meta_seat_wayland_nested_init_pointer_position;
  seat_class->warp_pointer = meta_seat_wayland_nested_warp_pointer;
  seat_class->query_state = meta_seat_wayland_nested_query_state;
}

static void
meta_seat_wayland_nested_init (MetaSeatWaylandNested *self)
{
  self->backend = NULL;
  self->pointer_device = NULL;
  self->keyboard_device = NULL;
  self->devices = NULL;
  self->keymap = NULL;
  self->layout_index = 0;

  self->pointer_pos = GRAPHENE_POINT_INIT (0.f, 0.f);

  self->modifiers = 0;

  self->xkb_keymap = NULL;
  self->xkb_state = NULL;
}

MetaBackend *
meta_seat_wayland_nested_get_backend (MetaSeatWaylandNested *self)
{
  g_return_val_if_fail (META_IS_SEAT_WAYLAND_NESTED (self), NULL);
  return self->backend;
}

void
meta_seat_wayland_nested_set_keymap (MetaSeatWaylandNested *self,
                                     struct xkb_keymap     *keymap,
                                     xkb_layout_index_t     layout_index)
{
  g_return_if_fail (META_IS_SEAT_WAYLAND_NESTED (self));

  self->layout_index = layout_index;

  if (self->keymap)
    meta_keymap_wayland_nested_set_keyboard_map (META_KEYMAP_WAYLAND_NESTED (self->keymap),
                                                 keymap);

  clear_xkb_state (self);

  if (keymap) {
    self->xkb_keymap = xkb_keymap_ref (keymap);
    self->xkb_state = xkb_state_new (self->xkb_keymap);

    if (self->xkb_state)
      xkb_state_update_mask (self->xkb_state,
                             0, 0, 0,
                             0, 0,
                             self->layout_index);
  }
}

struct xkb_state *
meta_seat_wayland_nested_peek_xkb_state (MetaSeatWaylandNested *self)
{
  g_return_val_if_fail (META_IS_SEAT_WAYLAND_NESTED (self), NULL);
  return self->xkb_state;
}

void
meta_seat_wayland_nested_update_pointer_position (MetaSeatWaylandNested *self,
                                                  float                  x,
                                                  float                  y)
{
  g_return_if_fail (META_IS_SEAT_WAYLAND_NESTED (self));
  self->pointer_pos = GRAPHENE_POINT_INIT (x, y);
}

ClutterModifierType
meta_seat_wayland_nested_get_modifiers (MetaSeatWaylandNested *self)
{
  g_return_val_if_fail (META_IS_SEAT_WAYLAND_NESTED (self), 0);
  return self->modifiers;
}

void
meta_seat_wayland_nested_set_modifiers (MetaSeatWaylandNested *self,
                                        ClutterModifierType    modifiers)
{
  g_return_if_fail (META_IS_SEAT_WAYLAND_NESTED (self));
  self->modifiers = modifiers;
}
