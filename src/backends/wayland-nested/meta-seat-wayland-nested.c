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

#include "core/bell.h"

#include "backends/wayland-nested/meta-seat-wayland-nested.h"

enum
{
  PROP_0,
  PROP_BACKEND,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL };

struct _MetaSeatWaylandNested
{
  ClutterSeat parent_instance;

  MetaBackend *backend;

  /* devices owned by this seat */
  GList *devices;

  /* core devices */
  ClutterInputDevice *core_pointer;
  ClutterInputDevice *core_keyboard;

  /* keymap object */
  ClutterKeymap *keymap;

  /* cached XKB state */
  struct xkb_keymap *xkb_keymap;
  xkb_layout_index_t xkb_layout_index;

  /* virtual slot tracking */
  guint virtual_touch_slot_base;
  GHashTable *reserved_virtual_slots;
};

G_DEFINE_TYPE (MetaSeatWaylandNested, meta_seat_wayland_nested, CLUTTER_TYPE_SEAT)

void
meta_seat_wayland_nested_start (MetaSeatWaylandNested *self)
{
  (void) self;
}

void
meta_seat_wayland_nested_set_keymap (MetaSeatWaylandNested *self,
                                     struct xkb_keymap     *keymap,
                                     xkb_layout_index_t     layout_index)
{
  self->xkb_keymap = keymap;
  self->xkb_layout_index = layout_index;
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
    self->backend = g_value_get_object (value);
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

static void
meta_seat_wayland_nested_dispose (GObject *object)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (object);

  g_clear_object (&self->core_pointer);
  g_clear_object (&self->core_keyboard);
  g_clear_object (&self->keymap);

  g_list_free_full (g_steal_pointer (&self->devices), g_object_unref);

  g_clear_pointer (&self->reserved_virtual_slots, g_hash_table_unref);

  G_OBJECT_CLASS (meta_seat_wayland_nested_parent_class)->dispose (object);
}

static ClutterInputDevice *
meta_seat_wayland_nested_get_pointer (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return self->core_pointer;
}

static ClutterInputDevice *
meta_seat_wayland_nested_get_keyboard (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return self->core_keyboard;
}

static const GList *
meta_seat_wayland_nested_peek_devices (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  return (const GList *) self->devices;
}

static void
meta_seat_wayland_nested_bell_notify (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  MetaContext *context;
  MetaDisplay *display;

  if (!self->backend)
    return;

  context = meta_backend_get_context (self->backend);
  if (!context)
    return;

  display = meta_context_get_display (context);
  if (!display)
    return;

  meta_bell_notify (display, NULL);
}

static ClutterKeymap *
meta_seat_wayland_nested_get_keymap (ClutterSeat *seat)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);

  if (self->keymap)
    return self->keymap;

  return NULL;
}

static guint
bump_virtual_touch_slot_base (MetaSeatWaylandNested *self)
{
  if (!self->reserved_virtual_slots)
    self->reserved_virtual_slots = g_hash_table_new (g_direct_hash, g_direct_equal);

  while (TRUE) {
    if (self->virtual_touch_slot_base < 0x100)
      self->virtual_touch_slot_base = 0x100;

    self->virtual_touch_slot_base += CLUTTER_VIRTUAL_INPUT_DEVICE_MAX_TOUCH_SLOTS;

    if (!g_hash_table_contains (self->reserved_virtual_slots,
                                GUINT_TO_POINTER (self->virtual_touch_slot_base)))
      break;
  }

  return self->virtual_touch_slot_base;
}

static gboolean
device_type_needs_touch_slots (ClutterInputDeviceType device_type)
{
  return (device_type == CLUTTER_TOUCHSCREEN_DEVICE ||
          device_type == CLUTTER_TABLET_DEVICE ||
          device_type == CLUTTER_PEN_DEVICE ||
          device_type == CLUTTER_ERASER_DEVICE);
}

static ClutterVirtualInputDevice *
meta_seat_wayland_nested_create_virtual_device (ClutterSeat            *seat,
                                                ClutterInputDeviceType  device_type)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);

  if (device_type_needs_touch_slots (device_type)) {
    guint slot_base = bump_virtual_touch_slot_base (self);
    g_hash_table_add (self->reserved_virtual_slots, GUINT_TO_POINTER (slot_base));
  }

  return NULL;
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
meta_seat_wayland_nested_warp_pointer (ClutterSeat *seat,
                                       int          x,
                                       int          y)
{
  (void) seat;
  (void) x;
  (void) y;
}

static void
meta_seat_wayland_nested_init_pointer_position (ClutterSeat *seat,
                                                float        x,
                                                float        y)
{
  (void) seat;
  (void) x;
  (void) y;
}

static gboolean
meta_seat_wayland_nested_query_state (ClutterSeat         *seat,
                                      ClutterSprite       *sprite,
                                      graphene_point_t    *coords,
                                      ClutterModifierType *modifiers)
{
  (void) seat;
  (void) sprite;
  (void) coords;
  (void) modifiers;

  return FALSE;
}

static gboolean
meta_seat_wayland_nested_handle_event_post (ClutterSeat        *seat,
                                            const ClutterEvent *event)
{
  MetaSeatWaylandNested *self = META_SEAT_WAYLAND_NESTED (seat);
  ClutterInputDevice *device = clutter_event_get_source_device (event);
  ClutterEventType event_type = clutter_event_type (event);

  if (event_type == CLUTTER_DEVICE_ADDED && device) {
    self->devices = g_list_prepend (self->devices, g_object_ref (device));
  } else if (event_type == CLUTTER_DEVICE_REMOVED && device) {
    GList *l = g_list_find (self->devices, device);
    if (l) {
      self->devices = g_list_delete_link (self->devices, l);
      g_object_unref (device);
    }
  }

  return FALSE;
}

static void
meta_seat_wayland_nested_class_init (MetaSeatWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterSeatClass *seat_class = CLUTTER_SEAT_CLASS (klass);

  object_class->set_property = meta_seat_wayland_nested_set_property;
  object_class->get_property = meta_seat_wayland_nested_get_property;
  object_class->dispose = meta_seat_wayland_nested_dispose;

  seat_class->get_pointer = meta_seat_wayland_nested_get_pointer;
  seat_class->get_keyboard = meta_seat_wayland_nested_get_keyboard;
  seat_class->peek_devices = meta_seat_wayland_nested_peek_devices;
  seat_class->bell_notify = meta_seat_wayland_nested_bell_notify;
  seat_class->get_keymap = meta_seat_wayland_nested_get_keymap;
  seat_class->create_virtual_device = meta_seat_wayland_nested_create_virtual_device;
  seat_class->get_supported_virtual_device_types = meta_seat_wayland_nested_get_supported_virtual_device_types;
  seat_class->warp_pointer = meta_seat_wayland_nested_warp_pointer;
  seat_class->init_pointer_position = meta_seat_wayland_nested_init_pointer_position;
  seat_class->query_state = meta_seat_wayland_nested_query_state;
  seat_class->handle_event_post = meta_seat_wayland_nested_handle_event_post;

  props[PROP_BACKEND] = g_param_spec_object ("backend",
                                             NULL,
                                             NULL,
                                             META_TYPE_BACKEND,
                                             G_PARAM_READWRITE |
                                             G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
meta_seat_wayland_nested_init (MetaSeatWaylandNested *self)
{
  self->reserved_virtual_slots = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->virtual_touch_slot_base = 0;

  self->xkb_keymap = NULL;
  self->xkb_layout_index = 0;
}
