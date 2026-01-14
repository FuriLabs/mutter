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

#include "backends/meta-furios-screen-cast-stream-eis.h"

typedef struct _MetaFuriosScreenCastStreamEis
{
  GObject parent_instance;

  MetaBackend *backend;

  MetaEis *eis;
  MetaEisViewport *viewport;

  char *mapping_id;

  int width;
  int height;
  double physical_scale;
} MetaFuriosScreenCastStreamEis;

G_DEFINE_TYPE (MetaFuriosScreenCastStreamEis,
               meta_furios_screen_cast_stream_eis,
               G_TYPE_OBJECT)

static void
meta_furios_screen_cast_stream_eis_dispose (GObject *object)
{
  MetaFuriosScreenCastStreamEis *self = META_FURIOS_SCREEN_CAST_STREAM_EIS (object);

  if (self->eis && self->viewport)
    meta_eis_remove_viewport (self->eis, self->viewport);

  g_clear_object (&self->viewport);
  g_clear_object (&self->eis);
  g_clear_pointer (&self->mapping_id, g_free);

  self->backend = NULL;

  G_OBJECT_CLASS (meta_furios_screen_cast_stream_eis_parent_class)->dispose (object);
}

static void
meta_furios_screen_cast_stream_eis_class_init (MetaFuriosScreenCastStreamEisClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_stream_eis_dispose;
}

static void
meta_furios_screen_cast_stream_eis_init (MetaFuriosScreenCastStreamEis *self)
{
  self->backend = NULL;
  self->eis = NULL;
  self->viewport = NULL;
  self->mapping_id = NULL;

  self->width = 0;
  self->height = 0;
  self->physical_scale = 1.0;
}

MetaFuriosScreenCastStreamEis *
meta_furios_screen_cast_stream_eis_new (MetaBackend  *backend,
                                        const char   *mapping_id,
                                        int           width,
                                        int           height)
{
  g_return_val_if_fail (META_IS_BACKEND (backend), NULL);
  g_return_val_if_fail (mapping_id != NULL, NULL);
  g_return_val_if_fail (width > 0, NULL);
  g_return_val_if_fail (height > 0, NULL);

  MetaFuriosScreenCastStreamEis *self =
    g_object_new (META_TYPE_FURIOS_SCREEN_CAST_STREAM_EIS, NULL);

  self->backend = backend;
  self->mapping_id = g_strdup (mapping_id);
  self->width = width;
  self->height = height;
  self->physical_scale = 1.0;

  self->eis = meta_eis_new (backend,
                            META_EIS_DEVICE_TYPE_KEYBOARD |
                            META_EIS_DEVICE_TYPE_POINTER |
                            META_EIS_DEVICE_TYPE_TOUCHSCREEN);

  self->viewport = meta_furios_screen_cast_eis_viewport_new (mapping_id,
                                                             width,
                                                             height,
                                                             self->physical_scale);

  meta_eis_add_viewport (self->eis, self->viewport);

  return self;
}

int
meta_furios_screen_cast_stream_eis_add_client_get_fd (MetaFuriosScreenCastStreamEis *self)
{
  g_return_val_if_fail (META_IS_FURIOS_SCREEN_CAST_STREAM_EIS (self), -1);
  g_return_val_if_fail (self->eis != NULL, -1);

  return meta_eis_add_client_get_fd (self->eis);
}
