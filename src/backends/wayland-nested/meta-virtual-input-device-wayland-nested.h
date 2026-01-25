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

#include "clutter/clutter.h"

G_BEGIN_DECLS

#define META_TYPE_VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED (meta_virtual_input_device_wayland_nested_get_type ())

G_DECLARE_FINAL_TYPE (MetaVirtualInputDeviceWaylandNested,
                      meta_virtual_input_device_wayland_nested,
                      META, VIRTUAL_INPUT_DEVICE_WAYLAND_NESTED,
                      ClutterVirtualInputDevice)

MetaVirtualInputDeviceWaylandNested *
meta_virtual_input_device_wayland_nested_new (ClutterSeat            *seat,
                                              ClutterInputDeviceType  device_type);

G_END_DECLS
