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

#include "backends/wayland-nested/meta-cursor-renderer-wayland-nested.h"

struct _MetaCursorRendererWaylandNested
{
  MetaCursorRenderer parent;
};

typedef struct _MetaCursorRendererWaylandNestedPrivate
{
  MetaBackend *backend;
  ClutterSprite *sprite;

  MetaCursorSprite *current_cursor;
  gulong texture_changed_handler_id;

  guint animation_timeout_id;
} MetaCursorRendererWaylandNestedPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MetaCursorRendererWaylandNested,
                            meta_cursor_renderer_wayland_nested,
                            META_TYPE_CURSOR_RENDERER)

static gboolean
meta_cursor_renderer_wayland_nested_update_animation (MetaCursorRendererWaylandNested *self)
{
  MetaCursorRendererWaylandNestedPrivate *priv = meta_cursor_renderer_wayland_nested_get_instance_private (self);
  MetaCursorRenderer *renderer = META_CURSOR_RENDERER (self);
  MetaCursorSprite *sprite = meta_cursor_renderer_get_cursor (renderer);

  priv->animation_timeout_id = 0;

  if (!sprite)
    return G_SOURCE_REMOVE;

  meta_cursor_sprite_tick_frame (sprite);
  meta_cursor_renderer_force_update (renderer);

  return G_SOURCE_REMOVE;
}

static void
maybe_schedule_cursor_sprite_animation_frame (MetaCursorRendererWaylandNested *self,
                                              MetaCursorSprite               *sprite,
                                              gboolean                        cursor_changed)
{
  MetaCursorRendererWaylandNestedPrivate *priv = meta_cursor_renderer_wayland_nested_get_instance_private (self);

  if (!cursor_changed && priv->animation_timeout_id)
    return;

  g_clear_handle_id (&priv->animation_timeout_id, g_source_remove);

  if (sprite && meta_cursor_sprite_is_animated (sprite)) {
    guint delay = meta_cursor_sprite_get_current_frame_time (sprite);

    if (delay == 0)
      return;

    priv->animation_timeout_id =
      g_timeout_add (delay,
                     (GSourceFunc) meta_cursor_renderer_wayland_nested_update_animation,
                     self);
    g_source_set_name_by_id (priv->animation_timeout_id,
                             "[mutter] meta_cursor_renderer_wayland_nested_update_animation");
  }
}

static void
on_cursor_sprite_texture_changed (MetaCursorSprite   *sprite,
                                  MetaCursorRenderer *renderer)
{
  (void) sprite;

  /* force cursor redraw */
  meta_cursor_renderer_force_update (renderer);
}

static gboolean
meta_cursor_renderer_wayland_nested_update_cursor (MetaCursorRenderer *renderer,
                                                   MetaCursorSprite   *cursor_sprite)
{
  MetaCursorRendererWaylandNested *self = META_CURSOR_RENDERER_WAYLAND_NESTED (renderer);
  MetaCursorRendererWaylandNestedPrivate *priv = meta_cursor_renderer_wayland_nested_get_instance_private (self);

  gboolean cursor_changed = (priv->current_cursor != cursor_sprite);

  if (cursor_changed) {
    if (priv->current_cursor)
      g_clear_signal_handler (&priv->texture_changed_handler_id,
                              priv->current_cursor);

    g_set_object (&priv->current_cursor, cursor_sprite);

    if (priv->current_cursor)
      priv->texture_changed_handler_id = g_signal_connect (priv->current_cursor,
                                                           "texture-changed",
                                                           G_CALLBACK (on_cursor_sprite_texture_changed),
                                                           renderer);
  }

  if (!cursor_sprite) {
    maybe_schedule_cursor_sprite_animation_frame (self, NULL, cursor_changed);
    return FALSE;
  }

  meta_cursor_sprite_realize_texture (cursor_sprite);

  maybe_schedule_cursor_sprite_animation_frame (self, cursor_sprite, cursor_changed);

  return meta_cursor_sprite_get_cogl_texture (cursor_sprite) != NULL;
}

static void
meta_cursor_renderer_wayland_nested_finalize (GObject *object)
{
  MetaCursorRendererWaylandNested *self = META_CURSOR_RENDERER_WAYLAND_NESTED (object);
  MetaCursorRendererWaylandNestedPrivate *priv = meta_cursor_renderer_wayland_nested_get_instance_private (self);

  g_clear_signal_handler (&priv->texture_changed_handler_id, priv->current_cursor);
  g_clear_object (&priv->current_cursor);
  g_clear_handle_id (&priv->animation_timeout_id, g_source_remove);

  G_OBJECT_CLASS (meta_cursor_renderer_wayland_nested_parent_class)->finalize (object);
}

static void
meta_cursor_renderer_wayland_nested_class_init (MetaCursorRendererWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaCursorRendererClass *renderer_class = META_CURSOR_RENDERER_CLASS (klass);

  object_class->finalize = meta_cursor_renderer_wayland_nested_finalize;
  renderer_class->update_cursor = meta_cursor_renderer_wayland_nested_update_cursor;
}

static void
meta_cursor_renderer_wayland_nested_init (MetaCursorRendererWaylandNested *self)
{
  (void) self;
}

MetaCursorRenderer *
meta_cursor_renderer_wayland_nested_new (MetaBackend   *backend,
                                         ClutterSprite *sprite)
{
  MetaCursorRendererWaylandNested *self;
  MetaCursorRendererWaylandNestedPrivate *priv;

  self = g_object_new (META_TYPE_CURSOR_RENDERER_WAYLAND_NESTED,
                       "backend", backend,
                       NULL);

  priv = meta_cursor_renderer_wayland_nested_get_instance_private (self);
  priv->backend = backend;
  priv->sprite = sprite;

  return META_CURSOR_RENDERER (self);
}
