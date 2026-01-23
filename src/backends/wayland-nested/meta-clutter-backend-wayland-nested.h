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

G_BEGIN_DECLS

#define META_TYPE_CLUTTER_BACKEND_WAYLAND_NESTED (meta_clutter_backend_wayland_nested_get_type ())
G_DECLARE_FINAL_TYPE (MetaClutterBackendWaylandNested,
                      meta_clutter_backend_wayland_nested,
                      META, CLUTTER_BACKEND_WAYLAND_NESTED,
                      ClutterBackend)

MetaClutterBackendWaylandNested * meta_clutter_backend_wayland_nested_new (MetaBackend    *backend,
                                                                           ClutterContext *context);

G_END_DECLS
