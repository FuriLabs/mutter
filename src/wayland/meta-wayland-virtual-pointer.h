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

#include "wayland/meta-wayland-types.h"

#define META_TYPE_WAYLAND_VIRTUAL_POINTER (meta_wayland_virtual_keyboard_resource_get_type ())
G_DECLARE_FINAL_TYPE (MetaWaylandVirtualPointer,
                      meta_wayland_virtual_pointer_resource,
                      META, WAYLAND_VIRTUAL_POINTER,
                      GObject);

void meta_wayland_virtual_pointer_init (MetaWaylandCompositor *compositor);
