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

#include "backends/meta-eis.h"

G_BEGIN_DECLS

#define META_TYPE_FURIOS_SCREEN_CAST_EIS_VIEWPORT (meta_furios_screen_cast_eis_viewport_get_type ())

G_DECLARE_FINAL_TYPE (MetaFuriosScreenCastEisViewport,
                      meta_furios_screen_cast_eis_viewport,
                      META, FURIOS_SCREEN_CAST_EIS_VIEWPORT,
                      GObject)

MetaEisViewport *
meta_furios_screen_cast_eis_viewport_new (const char *mapping_id,
                                          int         width,
                                          int         height,
                                          double      physical_scale);


G_END_DECLS
