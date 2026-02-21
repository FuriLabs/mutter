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

#include <gio/gio.h>

#include "backends/meta-backend-types.h"

G_BEGIN_DECLS

#define META_TYPE_FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER (meta_furios_screen_cast_stream_src_native_buffer_get_type ())

G_DECLARE_FINAL_TYPE (MetaFuriosScreenCastStreamSrcNativeBuffer,
                      meta_furios_screen_cast_stream_src_native_buffer,
                      META, FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER,
                      GObject)

MetaFuriosScreenCastStreamSrcNativeBuffer *
meta_furios_screen_cast_stream_src_native_buffer_new (MetaBackend  *backend,
                                                      guint         width,
                                                      guint         height,
                                                      float         fps,
                                                      GError      **error);

gboolean
meta_furios_screen_cast_stream_src_native_buffer_get_handle_info (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                                                                  GUnixFDList                               *fd_list,
                                                                  GVariant                                 **out_info,
                                                                  GError                                   **error);

void
meta_furios_screen_cast_stream_src_native_buffer_add_info (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                                                           GVariantBuilder                           *builder);

void
meta_furios_screen_cast_stream_src_native_buffer_request_frame_async (MetaFuriosScreenCastStreamSrcNativeBuffer *self);

int
meta_furios_screen_cast_stream_src_native_buffer_dup_fence_fd (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                                                               guint                                      slot);
G_END_DECLS
