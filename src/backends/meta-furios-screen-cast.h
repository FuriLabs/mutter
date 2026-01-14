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

#include "meta/meta-backend.h"
#include "meta-dbus-furios-screen-cast.h"

G_BEGIN_DECLS

#define META_TYPE_FURIOS_SCREEN_CAST (meta_furios_screen_cast_get_type ())
G_DECLARE_FINAL_TYPE (MetaFuriosScreenCast,
                      meta_furios_screen_cast,
                      META, FURIOS_SCREEN_CAST,
                      MetaDBusMutterScreenCastSkeleton)

MetaFuriosScreenCast * meta_furios_screen_cast_new (MetaBackend *backend);

G_END_DECLS
