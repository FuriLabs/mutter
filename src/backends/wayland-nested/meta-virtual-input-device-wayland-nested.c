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

static uint32_t
normalize_button_code (uint32_t button)
{
  switch (button) {
  case 1:
    return 0x110; /* BTN_LEFT */
  case 2:
    return 0x112; /* BTN_MIDDLE */
  case 3:
    return 0x111; /* BTN_RIGHT */
  case 4:
    return 0x113; /* BTN_SIDE */
  case 5:
    return 0x114; /* BTN_EXTRA */
  default:
    break;
  }

  return button;
}

static int
evdev_to_clutter_button (uint32_t button)
{
  uint32_t evdev_button = normalize_button_code (button);

  switch (evdev_button) {
  case 0x110:
    return 1; /* BTN_LEFT */
  case 0x112:
    return 2; /* BTN_MIDDLE */
  case 0x111:
    return 3; /* BTN_RIGHT */
  case 0x113:
    return 4; /* BTN_SIDE */
  case 0x114:
    return 5; /* BTN_EXTRA */
  default:
    return (int) button;
  }
}

static ClutterModifierType
evdev_button_to_mask (uint32_t button_code)
{
  uint32_t evdev_button = normalize_button_code (button_code);

  switch (evdev_button) {
  case 0x110:
    return CLUTTER_BUTTON1_MASK; /* BTN_LEFT */
  case 0x112:
    return CLUTTER_BUTTON2_MASK; /* BTN_MIDDLE */
  case 0x111:
    return CLUTTER_BUTTON3_MASK; /* BTN_RIGHT */
  case 0x113:
    return CLUTTER_BUTTON4_MASK; /* BTN_SIDE */
  case 0x114:
    return CLUTTER_BUTTON5_MASK; /* BTN_EXTRA */
  default:
    return 0;
  }
}

static inline ClutterModifierType
button_mask_bits (void)
{
  return (CLUTTER_BUTTON1_MASK |
          CLUTTER_BUTTON2_MASK |
          CLUTTER_BUTTON3_MASK |
          CLUTTER_BUTTON4_MASK |
          CLUTTER_BUTTON5_MASK);
}

static gboolean
xkb_named_mod_active (const struct xkb_keymap *keymap,
                      struct xkb_state        *state,
                      const char              *name,
                      enum xkb_state_component comp)
{
  xkb_mod_index_t idx;

  if (!keymap || !state || !name)
    return FALSE;

  idx = xkb_keymap_mod_get_index ((struct xkb_keymap *) keymap, name);
  if (idx == XKB_MOD_INVALID)
    return FALSE;

  return xkb_state_mod_index_is_active (state, idx, comp);
}

static ClutterModifierType
clutter_modifiers_from_xkb_state (struct xkb_state *state)
{
  const struct xkb_keymap *keymap;
  enum xkb_state_component comp;
  ClutterModifierType mods = 0;

  if (!state)
    return 0;

  keymap = xkb_state_get_keymap (state);
  if (!keymap)
    return 0;

  comp = (enum xkb_state_component) (XKB_STATE_MODS_DEPRESSED |
                                     XKB_STATE_MODS_LATCHED |
                                     XKB_STATE_MODS_LOCKED);

  if (xkb_named_mod_active (keymap, state, XKB_MOD_NAME_SHIFT, comp))
    mods |= CLUTTER_SHIFT_MASK;

  if (xkb_named_mod_active (keymap, state, XKB_MOD_NAME_CTRL, comp))
    mods |= CLUTTER_CONTROL_MASK;

  if (xkb_named_mod_active (keymap, state, XKB_MOD_NAME_ALT, comp) ||
      xkb_named_mod_active (keymap, state, "Mod1", comp))
    mods |= CLUTTER_MOD1_MASK;

  if (xkb_named_mod_active (keymap, state, XKB_MOD_NAME_LOGO, comp) ||
      xkb_named_mod_active (keymap, state, "Mod4", comp))
    mods |= CLUTTER_SUPER_MASK;

  if (xkb_named_mod_active (keymap, state, XKB_MOD_NAME_CAPS, comp))
    mods |= CLUTTER_LOCK_MASK;

  if (xkb_named_mod_active (keymap, state, "Mod2", comp))
    mods |= CLUTTER_MOD2_MASK;

  return mods;
}

static inline ClutterModifierType
seat_get_current_modifiers (MetaVirtualInputDeviceWaylandNested *self)
{
  if (META_IS_SEAT_WAYLAND_NESTED (self->seat))
    return meta_seat_wayland_nested_get_modifiers (META_SEAT_WAYLAND_NESTED (self->seat));

  return 0;
}

static inline void
seat_set_current_modifiers (MetaVirtualInputDeviceWaylandNested *self,
                            ClutterModifierType                 modifiers)
{
  if (META_IS_SEAT_WAYLAND_NESTED (self->seat))
    meta_seat_wayland_nested_set_modifiers (META_SEAT_WAYLAND_NESTED (self->seat), modifiers);
}

static void
seat_update_keyboard_modifiers_from_xkb (MetaVirtualInputDeviceWaylandNested *self)
{
  MetaSeatWaylandNested *seat;
  struct xkb_state *state;
  ClutterModifierType keymods;
  ClutterModifierType cur;
  ClutterModifierType new_mods;

  if (!META_IS_SEAT_WAYLAND_NESTED (self->seat))
    return;

  seat = META_SEAT_WAYLAND_NESTED (self->seat);
  state = meta_seat_wayland_nested_peek_xkb_state (seat);

  keymods = clutter_modifiers_from_xkb_state (state);
  cur = meta_seat_wayland_nested_get_modifiers (seat);

  new_mods = (cur & button_mask_bits ()) | keymods;
  meta_seat_wayland_nested_set_modifiers (seat, new_mods);
}

static void
notify_relative_motion (ClutterVirtualInputDevice *vdev,
                        uint64_t                   time_us,
                        double                     dx,
                        double                     dy)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);
  ClutterInputDevice *source;
  graphene_point_t coords;
  graphene_point_t delta;
  ClutterModifierType mods;
  ClutterEvent *event;

  source = get_pointer_source (self);
  if (!source)
    return;

  self->x += (float) dx;
  self->y += (float) dy;

  if (META_IS_SEAT_WAYLAND_NESTED (self->seat))
    meta_seat_wayland_nested_update_pointer_position (META_SEAT_WAYLAND_NESTED (self->seat),
                                                      self->x, self->y);

  coords = GRAPHENE_POINT_INIT (self->x, self->y);
  delta = GRAPHENE_POINT_INIT ((float) dx, (float) dy);

  mods = seat_get_current_modifiers (self);

  event = clutter_event_motion_new (0,
                                    (int64_t) time_us,
                                    source,
                                    NULL,
                                    mods,
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
  ClutterInputDevice *source;
  graphene_point_t coords;
  graphene_point_t delta;
  ClutterModifierType mods;
  ClutterEvent *event;

  source = get_pointer_source (self);
  if (!source)
    return;

  self->x = (float) x;
  self->y = (float) y;

  if (META_IS_SEAT_WAYLAND_NESTED (self->seat))
    meta_seat_wayland_nested_update_pointer_position (META_SEAT_WAYLAND_NESTED (self->seat),
                                                      self->x, self->y);

  coords = GRAPHENE_POINT_INIT (self->x, self->y);
  delta = GRAPHENE_POINT_INIT (0.f, 0.f);

  mods = seat_get_current_modifiers (self);

  event = clutter_event_motion_new (0,
                                    (int64_t) time_us,
                                    source,
                                    NULL,
                                    mods,
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
  ClutterInputDevice *source;
  ClutterEventType type;
  uint32_t evdev_button;
  int clutter_button;
  ClutterModifierType mask;
  ClutterModifierType state;
  graphene_point_t coords;
  ClutterEvent *event;

  source = get_pointer_source (self);
  if (!source)
    return;

  type = (button_state == CLUTTER_BUTTON_STATE_PRESSED) ? CLUTTER_BUTTON_PRESS : CLUTTER_BUTTON_RELEASE;

  /* Wayland needs evdev codes. Clutter needs 1..5 */
  evdev_button = normalize_button_code (button);
  clutter_button = evdev_to_clutter_button (button);

  mask = evdev_button_to_mask (evdev_button);
  state = seat_get_current_modifiers (self);

  if (button_state == CLUTTER_BUTTON_STATE_PRESSED)
    state |= mask;
  else
    state &= ~mask;

  seat_set_current_modifiers (self, state);

  coords = GRAPHENE_POINT_INIT (self->x, self->y);

  event = clutter_event_button_new (type,
                                    0,
                                    (int64_t) time_us,
                                    source,
                                    NULL,
                                    state,
                                    coords,
                                    clutter_button,
                                    evdev_button,
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
  ClutterInputDevice *source;
  ClutterEventType type;
  xkb_keysym_t sym = XKB_KEY_NoSymbol;
  uint32_t unicode_value = 0;
  xkb_keycode_t xkb_code;
  ClutterModifierType mods;
  ClutterModifierSet raw;
  ClutterEvent *event;

  source = get_keyboard_source (self);
  if (!source)
    return;

  type = (key_state == CLUTTER_KEY_STATE_PRESSED) ? CLUTTER_KEY_PRESS : CLUTTER_KEY_RELEASE;

  mods = seat_get_current_modifiers (self);
  raw.pressed = mods;
  raw.latched = 0;
  raw.locked = 0;

  /* protocol sends evdev keycodes but xkbcommon expects evdev + 8. */
  xkb_code = (xkb_keycode_t) key + 8;

  if (META_IS_SEAT_WAYLAND_NESTED (self->seat)) {
    MetaSeatWaylandNested *seat = META_SEAT_WAYLAND_NESTED (self->seat);
    struct xkb_state *state = meta_seat_wayland_nested_peek_xkb_state (seat);

    if (state) {
      enum xkb_key_direction dir = (key_state == CLUTTER_KEY_STATE_PRESSED) ? XKB_KEY_DOWN : XKB_KEY_UP;

      xkb_state_update_key (state, xkb_code, dir);
      seat_update_keyboard_modifiers_from_xkb (self);

      sym = xkb_state_key_get_one_sym (state, xkb_code);
      if (sym != XKB_KEY_NoSymbol)
        unicode_value = xkb_keysym_to_utf32 (sym);
    }
  }

  event = clutter_event_key_new (type,
                                 0,
                                 (int64_t) time_us,
                                 source,
                                 raw,
                                 mods,
                                 (uint32_t) sym,
                                 key,
                                 (uint32_t) xkb_code,
                                 unicode_value);

  push_event (event);
}

static void
notify_keyval (ClutterVirtualInputDevice *vdev,
               uint64_t                   time_us,
               uint32_t                   keyval,
               ClutterKeyState            key_state)
{
  MetaVirtualInputDeviceWaylandNested *self = META_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (vdev);
  ClutterInputDevice *source;
  ClutterEventType type;
  uint32_t unicode_value;
  ClutterModifierType mods;
  ClutterModifierSet raw;
  ClutterEvent *event;

  source = get_keyboard_source (self);
  if (!source)
    return;

  type = (key_state == CLUTTER_KEY_STATE_PRESSED) ? CLUTTER_KEY_PRESS : CLUTTER_KEY_RELEASE;

  unicode_value = xkb_keysym_to_utf32 ((xkb_keysym_t) keyval);

  seat_update_keyboard_modifiers_from_xkb (self);
  mods = seat_get_current_modifiers (self);

  raw.pressed = mods;
  raw.latched = 0;
  raw.locked = 0;

  event = clutter_event_key_new (type,
                                 0,
                                 (int64_t) time_us,
                                 source,
                                 raw,
                                 mods,
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
  ClutterInputDevice *source;
  graphene_point_t coords;
  ClutterModifierType mods;
  ClutterEvent *event;

  source = get_pointer_source (self);
  if (!source)
    return;

  coords = GRAPHENE_POINT_INIT (self->x, self->y);
  mods = seat_get_current_modifiers (self);

  event = clutter_event_scroll_discrete_new (0,
                                             (int64_t) time_us,
                                             source,
                                             NULL,
                                             mods,
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
  ClutterInputDevice *source;
  graphene_point_t coords;
  graphene_point_t delta;
  ClutterModifierType mods;
  ClutterEvent *event;

  source = get_pointer_source (self);
  if (!source)
    return;

  coords = GRAPHENE_POINT_INIT (self->x, self->y);
  delta = GRAPHENE_POINT_INIT ((float) dx, (float) dy);
  mods = seat_get_current_modifiers (self);

  event = clutter_event_scroll_smooth_new (0,
                                           (int64_t) time_us,
                                           source,
                                           NULL,
                                           mods,
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
