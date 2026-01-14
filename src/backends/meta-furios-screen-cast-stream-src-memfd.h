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

G_BEGIN_DECLS

#define META_TYPE_FURIOS_SCREEN_CAST_STREAM_SRC_MEMFD (meta_furios_screen_cast_stream_src_memfd_get_type ())
G_DECLARE_FINAL_TYPE (MetaFuriosScreenCastStreamSrcMemfd,
                      meta_furios_screen_cast_stream_src_memfd,
                      META, FURIOS_SCREEN_CAST_STREAM_SRC_MEMFD,
                      GObject)

MetaFuriosScreenCastStreamSrcMemfd *
meta_furios_screen_cast_stream_src_memfd_new (MetaBackend  *backend,
                                              guint         width,
                                              guint         height,
                                              float         fps,
                                              GError      **error);

int meta_furios_screen_cast_stream_src_memfd_dup_fd (MetaFuriosScreenCastStreamSrcMemfd *self);

gboolean meta_furios_screen_cast_stream_src_memfd_request_frame (MetaFuriosScreenCastStreamSrcMemfd *self,
                                                                 guint                              *out_seq,
                                                                 guint                              *out_slot,
                                                                 GError                            **error);

void meta_furios_screen_cast_stream_src_memfd_set_dbus (MetaFuriosScreenCastStreamSrcMemfd *self,
                                                        GDBusConnection                    *connection,
                                                        const char                         *object_path,
                                                        const char                         *peer_name);

gboolean
meta_furios_screen_cast_stream_src_memfd_get_last_damage (MetaFuriosScreenCastStreamSrcMemfd *self,
                                                          gboolean                           *out_should_emit_damage,
                                                          GVariant                          **out_damage);

G_END_DECLS
