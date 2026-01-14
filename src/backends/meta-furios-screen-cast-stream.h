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

#include "meta-dbus-furios-screen-cast.h"
#include "backends/meta-furios-screen-cast-stream-src-memfd.h"

G_BEGIN_DECLS

#define META_TYPE_FURIOS_SCREEN_CAST_STREAM (meta_furios_screen_cast_stream_get_type ())
G_DECLARE_FINAL_TYPE (MetaFuriosScreenCastStream,
                      meta_furios_screen_cast_stream,
                      META, FURIOS_SCREEN_CAST_STREAM,
                      MetaDBusMutterScreenCastStreamSkeleton)

typedef struct _MetaFuriosScreenCastSession MetaFuriosScreenCastSession;

MetaFuriosScreenCastStream *
meta_furios_screen_cast_stream_new (MetaFuriosScreenCastSession *session,
                                    GDBusConnection             *connection,
                                    const char                  *peer_name,
                                    GVariant                    *properties,
                                    GError                     **error);

void
meta_furios_screen_cast_stream_close (MetaFuriosScreenCastStream *stream);

const char *
meta_furios_screen_cast_stream_get_object_path (MetaFuriosScreenCastStream *stream);

G_END_DECLS
