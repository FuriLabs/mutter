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

#pragma once

#include "backends/meta-backend-private.h"

G_BEGIN_DECLS

#define META_TYPE_SEAT_WAYLAND_NESTED (meta_seat_wayland_nested_get_type ())
G_DECLARE_FINAL_TYPE (MetaSeatWaylandNested,
                      meta_seat_wayland_nested,
                      META, SEAT_WAYLAND_NESTED,
                      ClutterSeat)

MetaBackend * meta_seat_wayland_nested_get_backend (MetaSeatWaylandNested *self);

void meta_seat_wayland_nested_start (MetaSeatWaylandNested *self);

void meta_seat_wayland_nested_set_keymap (MetaSeatWaylandNested *self,
                                          struct xkb_keymap     *keymap,
                                          xkb_layout_index_t     layout_index);

struct xkb_state * meta_seat_wayland_nested_peek_xkb_state (MetaSeatWaylandNested *self);

void meta_seat_wayland_nested_update_pointer_position (MetaSeatWaylandNested *self,
                                                       float                  x,
                                                       float                  y);

ClutterModifierType meta_seat_wayland_nested_get_modifiers (MetaSeatWaylandNested *self);

void meta_seat_wayland_nested_set_modifiers (MetaSeatWaylandNested *self,
                                             ClutterModifierType    modifiers);

void meta_seat_wayland_nested_notify_key (MetaSeatWaylandNested *self,
                                          uint32_t               key,
                                          ClutterKeyState        key_state);

G_END_DECLS
