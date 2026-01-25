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

#include "backends/meta-keymap-utils.h"

#include "backends/wayland-nested/meta-keymap-wayland-nested.h"

#include "clutter/clutter-keymap-private.h"

static const char *option_xkb_layout = "us";
static const char *option_xkb_variant = "";
static const char *option_xkb_options = "";
static const char *option_xkb_model = "pc105";
static const char *option_xkb_rules = "evdev";

struct _MetaKeymapWaylandNested
{
  ClutterKeymap parent_instance;

  struct xkb_keymap *xkb_keymap;
};

G_DEFINE_TYPE (MetaKeymapWaylandNested, meta_keymap_wayland_nested, CLUTTER_TYPE_KEYMAP)

static void
meta_keymap_wayland_nested_finalize (GObject *object)
{
  MetaKeymapWaylandNested *self = META_KEYMAP_WAYLAND_NESTED (object);

  g_clear_pointer (&self->xkb_keymap, xkb_keymap_unref);

  G_OBJECT_CLASS (meta_keymap_wayland_nested_parent_class)->finalize (object);
}

static ClutterTextDirection
meta_keymap_wayland_nested_get_direction (ClutterKeymap *keymap)
{
  (void) keymap;
  return CLUTTER_TEXT_DIRECTION_DEFAULT;
}

static struct xkb_keymap *
create_default_keymap (void)
{
  struct xkb_rule_names names;
  struct xkb_context *ctx;
  struct xkb_keymap *keymap;

  names.rules = option_xkb_rules;
  names.model = option_xkb_model;
  names.layout = option_xkb_layout;
  names.variant = option_xkb_variant;
  names.options = option_xkb_options;

  ctx = meta_create_xkb_context ();
  g_assert (ctx);

  keymap = xkb_keymap_new_from_names (ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref (ctx);

  return keymap;
}

static void
meta_keymap_wayland_nested_init (MetaKeymapWaylandNested *self)
{
  self->xkb_keymap = create_default_keymap ();
}

static void
meta_keymap_wayland_nested_class_init (MetaKeymapWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterKeymapClass *keymap_class = CLUTTER_KEYMAP_CLASS (klass);

  object_class->finalize = meta_keymap_wayland_nested_finalize;
  keymap_class->get_direction = meta_keymap_wayland_nested_get_direction;
}

void
meta_keymap_wayland_nested_set_keyboard_map (MetaKeymapWaylandNested *self,
                                             struct xkb_keymap       *xkb_keymap)
{
  g_return_if_fail (META_IS_KEYMAP_WAYLAND_NESTED (self));
  g_return_if_fail (xkb_keymap != NULL);

  g_clear_pointer (&self->xkb_keymap, xkb_keymap_unref);
  self->xkb_keymap = xkb_keymap_ref (xkb_keymap);
}
