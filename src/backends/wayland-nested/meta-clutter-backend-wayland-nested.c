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

#include "backends/meta-backend-private.h"

#include "backends/wayland-nested/meta-clutter-backend-wayland-nested.h"
#include "backends/wayland-nested/meta-stage-wayland-nested.h"
#include "backends/wayland-nested/meta-seat-wayland-nested.h"

struct _MetaClutterBackendWaylandNested
{
  ClutterBackend parent;

  MetaBackend *backend;
  ClutterContext *context;

  ClutterSeat *seat;

  ClutterSprite   *pointer_sprite;
  ClutterKeyFocus *key_focus;
};

G_DEFINE_TYPE (MetaClutterBackendWaylandNested,
               meta_clutter_backend_wayland_nested,
               CLUTTER_TYPE_BACKEND)

static CoglRenderer *
meta_clutter_backend_wayland_nested_get_renderer (ClutterBackend  *clutter_backend,
                                                  GError         **error)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);
  MetaRenderer *renderer = meta_backend_get_renderer (self->backend);

  (void) error;

  return meta_renderer_create_cogl_renderer (renderer);
}

static ClutterStageWindow *
meta_clutter_backend_wayland_nested_create_stage (ClutterBackend  *clutter_backend,
                                                  ClutterStage    *wrapper,
                                                  GError         **error)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  (void) error;

  return g_object_new (META_TYPE_STAGE_WAYLAND_NESTED,
                       "backend", self->backend,
                       "wrapper", wrapper,
                       NULL);
}

static void
ensure_seat (MetaClutterBackendWaylandNested *self)
{
  ClutterSeat *backend_seat;

  if (self->seat)
    return;

  if (!self->backend)
    return;

  backend_seat = meta_backend_get_default_seat (self->backend);
  if (!backend_seat)
    return;

  self->seat = g_object_ref (backend_seat);
}

static ClutterSeat *
meta_clutter_backend_wayland_nested_get_default_seat (ClutterBackend *clutter_backend)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  ensure_seat (self);

  return self->seat;
}

static gboolean
meta_clutter_backend_wayland_nested_is_display_server (ClutterBackend *clutter_backend)
{
  (void) clutter_backend;
  return TRUE;
}

static void
ensure_pointer_sprite (MetaClutterBackendWaylandNested *self,
                       ClutterStage                    *stage)
{
  if (self->pointer_sprite)
    return;

  ensure_seat (self);

  ClutterInputDevice *device = NULL;
  if (self->seat)
    device = clutter_seat_get_pointer (self->seat);

  self->pointer_sprite = g_object_new (CLUTTER_TYPE_SPRITE,
                                       "stage", stage,
                                       "device", device,
                                       NULL);
}

static ClutterSprite *
meta_clutter_backend_wayland_nested_get_pointer_sprite (ClutterBackend *clutter_backend,
                                                        ClutterStage   *stage)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  ensure_pointer_sprite (self, stage);
  return self->pointer_sprite;
}

static ClutterSprite *
meta_clutter_backend_wayland_nested_get_sprite (ClutterBackend     *clutter_backend,
                                                ClutterStage       *stage,
                                                const ClutterEvent *for_event)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  ClutterInputDevice *source_device = clutter_event_get_source_device (for_event);
  if (!source_device)
    return NULL;

  ClutterInputDeviceType t = clutter_input_device_get_device_type (source_device);
  if (t == CLUTTER_KEYBOARD_DEVICE || t == CLUTTER_PAD_DEVICE)
    return NULL;

  ensure_pointer_sprite (self, stage);
  return self->pointer_sprite;
}

static ClutterSprite *
meta_clutter_backend_wayland_nested_lookup_sprite (ClutterBackend       *clutter_backend,
                                                   ClutterStage         *stage,
                                                   ClutterInputDevice   *device,
                                                   ClutterEventSequence *sequence)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  (void) sequence;

  if (!device)
    return NULL;

  ClutterInputDeviceType t = clutter_input_device_get_device_type (device);
  if (t == CLUTTER_KEYBOARD_DEVICE || t == CLUTTER_PAD_DEVICE)
    return NULL;

  ensure_pointer_sprite (self, stage);
  return self->pointer_sprite;
}

static void
meta_clutter_backend_wayland_nested_destroy_sprite (ClutterBackend *clutter_backend,
                                                    ClutterSprite  *sprite)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  if (self->pointer_sprite == sprite)
    g_clear_object (&self->pointer_sprite);
}

static gboolean
meta_clutter_backend_wayland_nested_foreach_sprite (ClutterBackend               *clutter_backend,
                                                    ClutterStage                 *stage,
                                                    ClutterStageInputForeachFunc  func,
                                                    gpointer                      user_data)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  ensure_pointer_sprite (self, stage);

  if (self->pointer_sprite)
    return func (stage, self->pointer_sprite, user_data);

  return TRUE;
}

static ClutterKeyFocus *
meta_clutter_backend_wayland_nested_get_key_focus (ClutterBackend *clutter_backend,
                                                   ClutterStage   *stage)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (clutter_backend);

  if (!self->key_focus)
    self->key_focus = g_object_new (CLUTTER_TYPE_KEY_FOCUS,
                                    "stage", stage,
                                    NULL);

  return self->key_focus;
}

static void
meta_clutter_backend_wayland_nested_finalize (GObject *object)
{
  MetaClutterBackendWaylandNested *self = META_CLUTTER_BACKEND_WAYLAND_NESTED (object);

  g_clear_object (&self->pointer_sprite);
  g_clear_object (&self->key_focus);
  g_clear_object (&self->seat);

  self->backend = NULL;
  self->context = NULL;

  G_OBJECT_CLASS (meta_clutter_backend_wayland_nested_parent_class)->finalize (object);
}

static void
meta_clutter_backend_wayland_nested_class_init (MetaClutterBackendWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterBackendClass *clutter_backend_class = CLUTTER_BACKEND_CLASS (klass);

  object_class->finalize = meta_clutter_backend_wayland_nested_finalize;

  clutter_backend_class->get_renderer = meta_clutter_backend_wayland_nested_get_renderer;
  clutter_backend_class->create_stage = meta_clutter_backend_wayland_nested_create_stage;
  clutter_backend_class->get_default_seat = meta_clutter_backend_wayland_nested_get_default_seat;
  clutter_backend_class->is_display_server = meta_clutter_backend_wayland_nested_is_display_server;

  clutter_backend_class->get_pointer_sprite = meta_clutter_backend_wayland_nested_get_pointer_sprite;
  clutter_backend_class->get_sprite = meta_clutter_backend_wayland_nested_get_sprite;
  clutter_backend_class->lookup_sprite = meta_clutter_backend_wayland_nested_lookup_sprite;
  clutter_backend_class->destroy_sprite = meta_clutter_backend_wayland_nested_destroy_sprite;
  clutter_backend_class->foreach_sprite = meta_clutter_backend_wayland_nested_foreach_sprite;

  clutter_backend_class->get_key_focus = meta_clutter_backend_wayland_nested_get_key_focus;
}

static void
meta_clutter_backend_wayland_nested_init (MetaClutterBackendWaylandNested *self)
{
  self->backend = NULL;
  self->context = NULL;
  self->seat = NULL;
  self->pointer_sprite = NULL;
  self->key_focus = NULL;
}

MetaClutterBackendWaylandNested *
meta_clutter_backend_wayland_nested_new (MetaBackend    *backend,
                                         ClutterContext *context)
{
  MetaClutterBackendWaylandNested *self;

  self = g_object_new (META_TYPE_CLUTTER_BACKEND_WAYLAND_NESTED,
                       "context", context,
                       NULL);

  self->backend = backend;
  self->context = context;

  return self;
}
