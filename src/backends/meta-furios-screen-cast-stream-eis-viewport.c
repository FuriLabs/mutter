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

#include "backends/meta-furios-screen-cast-stream-eis-viewport.h"

struct _MetaFuriosScreenCastEisViewport
{
  GObject parent_instance;

  char *mapping_id;

  int width;
  int height;
  double physical_scale;
};

static void meta_furios_screen_cast_eis_viewport_init_iface (MetaEisViewportInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaFuriosScreenCastEisViewport,
                         meta_furios_screen_cast_eis_viewport,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (META_TYPE_EIS_VIEWPORT,
                                                meta_furios_screen_cast_eis_viewport_init_iface))

static gboolean
eis_viewport_is_standalone (MetaEisViewport *viewport)
{
  (void) viewport;
  return TRUE;
}

static const char *
eis_viewport_get_mapping_id (MetaEisViewport *viewport)
{
  MetaFuriosScreenCastEisViewport *self = (MetaFuriosScreenCastEisViewport *) viewport;
  return self->mapping_id;
}

static gboolean
eis_viewport_get_position (MetaEisViewport *viewport,
                           int             *out_x,
                           int             *out_y)
{
  (void) viewport;

  if (out_x)
    *out_x = 0;
  if (out_y)
    *out_y = 0;

  return TRUE;
}

static void
eis_viewport_get_size (MetaEisViewport *viewport,
                       int             *out_width,
                       int             *out_height)
{
  MetaFuriosScreenCastEisViewport *self = (MetaFuriosScreenCastEisViewport *) viewport;

  if (out_width)
    *out_width = self->width;
  if (out_height)
    *out_height = self->height;
}

static double
eis_viewport_get_physical_scale (MetaEisViewport *viewport)
{
  MetaFuriosScreenCastEisViewport *self = (MetaFuriosScreenCastEisViewport *) viewport;
  return self->physical_scale;
}

static gboolean
eis_viewport_transform_coordinate (MetaEisViewport *viewport,
                                   double           x,
                                   double           y,
                                   double          *out_x,
                                   double          *out_y)
{
  MetaFuriosScreenCastEisViewport *self = (MetaFuriosScreenCastEisViewport *) viewport;
  double scale = self->physical_scale > 0.0 ? self->physical_scale : 1.0;

  if (out_x)
    *out_x = x * scale;
  if (out_y)
    *out_y = y * scale;

  return TRUE;
}

static void
meta_furios_screen_cast_eis_viewport_init_iface (MetaEisViewportInterface *iface)
{
  iface->is_standalone = eis_viewport_is_standalone;
  iface->get_mapping_id = eis_viewport_get_mapping_id;
  iface->get_position = eis_viewport_get_position;
  iface->get_size = eis_viewport_get_size;
  iface->get_physical_scale = eis_viewport_get_physical_scale;
  iface->transform_coordinate = eis_viewport_transform_coordinate;
}

static void
meta_furios_screen_cast_eis_viewport_dispose (GObject *object)
{
  MetaFuriosScreenCastEisViewport *self = (MetaFuriosScreenCastEisViewport *) object;

  g_clear_pointer (&self->mapping_id, g_free);

  G_OBJECT_CLASS (meta_furios_screen_cast_eis_viewport_parent_class)->dispose (object);
}

static void
meta_furios_screen_cast_eis_viewport_class_init (MetaFuriosScreenCastEisViewportClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_eis_viewport_dispose;
}

static void
meta_furios_screen_cast_eis_viewport_init (MetaFuriosScreenCastEisViewport *self)
{
  self->mapping_id = NULL;
  self->width = 0;
  self->height = 0;
  self->physical_scale = 1.0;
}

MetaEisViewport *
meta_furios_screen_cast_eis_viewport_new (const char *mapping_id,
                                          int         width,
                                          int         height,
                                          double      physical_scale)
{
  MetaFuriosScreenCastEisViewport *self;

  g_return_val_if_fail (mapping_id != NULL, NULL);
  g_return_val_if_fail (width > 0, NULL);
  g_return_val_if_fail (height > 0, NULL);

  self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST_EIS_VIEWPORT, NULL);

  self->mapping_id = g_strdup (mapping_id);
  self->width = width;
  self->height = height;
  self->physical_scale = physical_scale > 0.0 ? physical_scale : 1.0;

  return META_EIS_VIEWPORT (self);
}
