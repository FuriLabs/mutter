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
#include "backends/meta-color-manager.h"
#include "backends/meta-input-settings-dummy.h"
#include "backends/meta-stage-private.h"

#ifdef HAVE_LOGIND
#include "backends/meta-launcher.h"
#endif

#include "backends/wayland-nested/meta-backend-wayland-nested.h"
#include "backends/wayland-nested/meta-gpu-wayland-nested.h"
#include "backends/wayland-nested/meta-monitor-manager-wayland-nested.h"
#include "backends/wayland-nested/meta-renderer-wayland-nested.h"
#include "backends/wayland-nested/meta-clutter-backend-wayland-nested.h"
#include "backends/wayland-nested/meta-seat-wayland-nested.h"
#include "backends/wayland-nested/meta-cursor-renderer-wayland-nested.h"

struct _MetaBackendWaylandNested
{
  MetaBackend parent;
};

typedef struct _MetaBackendWaylandNestedPrivate
{
  MetaGpu *gpu;
  MetaInputSettings *input_settings;
  ClutterSeat *seat;

  MetaCursorRenderer *cursor_renderer;

  struct xkb_keymap *xkb_keymap;
  xkb_layout_index_t xkb_layout_index;
} MetaBackendWaylandNestedPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MetaBackendWaylandNested,
                            meta_backend_wayland_nested,
                            META_TYPE_BACKEND)

static void
init_gpus (MetaBackendWaylandNested *self)
{
  MetaBackendWaylandNestedPrivate *priv =
    meta_backend_wayland_nested_get_instance_private (self);

  priv->gpu = g_object_new (META_TYPE_GPU_WAYLAND_NESTED,
                            "backend", self,
                            NULL);
  meta_backend_add_gpu (META_BACKEND (self), priv->gpu);
}

static MetaSeatWaylandNested *
get_seat_wayland_nested_or_null (MetaBackend *backend)
{
  ClutterSeat *seat = meta_backend_get_default_seat (backend);
  if (seat && META_IS_SEAT_WAYLAND_NESTED (seat))
    return META_SEAT_WAYLAND_NESTED (seat);
  return NULL;
}

static void
on_context_started (MetaContext *context,
                    MetaBackend *backend)
{
  MetaSeatWaylandNested *seat_wl = get_seat_wayland_nested_or_null (backend);
  if (seat_wl)
    meta_seat_wayland_nested_start (seat_wl);
}

static struct xkb_keymap *
create_keymap (const char *layouts,
               const char *variants,
               const char *options,
               const char *model)
{
  struct xkb_rule_names names;
  struct xkb_context *context;
  struct xkb_keymap *keymap;

  memset (&names, 0, sizeof (names));
  names.rules = "evdev";
  names.model = model && *model ? model : "pc105";
  names.layout = layouts && *layouts ? layouts : "us";
  names.variant = variants && *variants ? variants : NULL;
  names.options = options && *options ? options : NULL;

  context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (!context)
    return NULL;

  keymap = xkb_keymap_new_from_names (context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref (context);

  return keymap;
}

static gboolean
meta_backend_wayland_nested_init_basic (MetaBackend  *backend,
                                        GError      **error)
{
  MetaBackendClass *parent_backend_class = META_BACKEND_CLASS (meta_backend_wayland_nested_parent_class);

  if (parent_backend_class->init_basic) {
    if (!parent_backend_class->init_basic (backend, error))
      return FALSE;
  }

  init_gpus (META_BACKEND_WAYLAND_NESTED (backend));

  g_signal_connect (meta_backend_get_context (backend),
                    "started",
                    G_CALLBACK (on_context_started),
                    backend);

  return TRUE;
}

#ifdef HAVE_LOGIND
static gboolean
meta_backend_wayland_nested_create_launcher (MetaBackend   *backend,
                                             MetaLauncher **launcher_out,
                                             GError       **error)
{
  *launcher_out = NULL;
  return TRUE;
}
#endif

static ClutterBackend *
meta_backend_wayland_nested_create_clutter_backend (MetaBackend    *backend,
                                                    ClutterContext *context)
{
  return CLUTTER_BACKEND (meta_clutter_backend_wayland_nested_new (backend, context));
}

static ClutterSeat *
meta_backend_wayland_nested_create_default_seat (MetaBackend  *backend,
                                                 GError      **error)
{
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));
  ClutterContext *clutter_context = meta_backend_get_clutter_context (backend);

  (void) error;

  if (!priv->seat)
    priv->seat = CLUTTER_SEAT (g_object_new (META_TYPE_SEAT_WAYLAND_NESTED,
                                             "backend", backend,
                                             "context", clutter_context,
                                             NULL));

  return g_object_ref (priv->seat);
}

static MetaRenderer *
meta_backend_wayland_nested_create_renderer (MetaBackend *backend,
                                             GError     **error)
{
  (void) error;

  return g_object_new (META_TYPE_RENDERER_WAYLAND_NESTED,
                       "backend", backend,
                       NULL);
}

static MetaMonitorManager *
meta_backend_wayland_nested_create_monitor_manager (MetaBackend *backend,
                                                    GError     **error)
{
  (void) error;

  return g_object_new (META_TYPE_MONITOR_MANAGER_WAYLAND_NESTED,
                       "backend", backend,
                       NULL);
}

static MetaColorManager *
meta_backend_wayland_nested_create_color_manager (MetaBackend *backend)
{
  return g_object_new (META_TYPE_COLOR_MANAGER,
                       "backend", backend,
                       NULL);
}

static MetaInputSettings *
meta_backend_wayland_nested_get_input_settings (MetaBackend *backend)
{
  MetaBackendWaylandNestedPrivate *priv =
    meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));

  if (!priv->input_settings)
    priv->input_settings = g_object_new (META_TYPE_INPUT_SETTINGS_DUMMY,
                                         "backend", backend,
                                         NULL);

  return priv->input_settings;
}

static MetaCursorRenderer *
meta_backend_wayland_nested_get_cursor_renderer (MetaBackend   *backend,
                                                 ClutterSprite *sprite)
{
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));

  if (!priv->cursor_renderer)
    priv->cursor_renderer = meta_cursor_renderer_wayland_nested_new (backend, sprite);

  return priv->cursor_renderer;
}

static void
meta_backend_wayland_nested_update_stage (MetaBackend *backend)
{
  ClutterActor *stage = meta_backend_get_stage (backend);
  meta_stage_rebuild_views (META_STAGE (stage));
}

static void
meta_backend_wayland_nested_select_stage_events (MetaBackend *backend)
{
  (void) backend;
}

static MetaLogicalMonitor *
meta_backend_wayland_nested_get_current_logical_monitor (MetaBackend *backend)
{
  MetaMonitorManager *monitor_manager;
  MetaLogicalMonitor *logical_monitor;

  monitor_manager = meta_backend_get_monitor_manager (backend);
  if (!monitor_manager)
    return NULL;

  logical_monitor = meta_monitor_manager_get_primary_logical_monitor (monitor_manager);
  if (logical_monitor)
    return logical_monitor;

  const GList *logical_monitors = meta_monitor_manager_get_logical_monitors (monitor_manager);

  if (logical_monitors)
    return logical_monitors->data;

  return NULL;
}

static void
meta_backend_wayland_nested_set_keymap_async (MetaBackend *backend,
                                              const char  *layouts,
                                              const char  *variants,
                                              const char  *options,
                                              const char  *model,
                                              GTask       *task)
{
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));

  struct xkb_keymap *new_keymap = create_keymap (layouts, variants, options, model);

  if (!new_keymap) {
    g_task_return_new_error (task,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Failed to create XKB keymap");
    g_object_unref (task);
    return;
  }

  if (priv->xkb_keymap)
    xkb_keymap_unref (priv->xkb_keymap);

  priv->xkb_keymap = new_keymap;
  priv->xkb_layout_index = 0;

  MetaSeatWaylandNested *seat_wl = get_seat_wayland_nested_or_null (backend);
  if (seat_wl)
    meta_seat_wayland_nested_set_keymap (seat_wl, priv->xkb_keymap, priv->xkb_layout_index);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
meta_backend_wayland_nested_set_keymap_layout_group_async (MetaBackend        *backend,
                                                           xkb_layout_index_t  idx,
                                                           GTask              *task)
{
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));
  MetaSeatWaylandNested *seat_wl;

  priv->xkb_layout_index = idx;

  seat_wl = get_seat_wayland_nested_or_null (backend);
  if (seat_wl && priv->xkb_keymap)
    meta_seat_wayland_nested_set_keymap (seat_wl,
                                         priv->xkb_keymap,
                                         priv->xkb_layout_index);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static struct xkb_keymap *
meta_backend_wayland_nested_get_keymap (MetaBackend *backend)
{
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));

  if (!priv->xkb_keymap) {
    priv->xkb_keymap = create_keymap ("us", "", "", "pc105");
    priv->xkb_layout_index = 0;

    MetaSeatWaylandNested *seat_wl = get_seat_wayland_nested_or_null (backend);
    if (seat_wl && priv->xkb_keymap)
      meta_seat_wayland_nested_set_keymap (seat_wl, priv->xkb_keymap, priv->xkb_layout_index);
  }

  return priv->xkb_keymap;
}

static xkb_layout_index_t
meta_backend_wayland_nested_get_keymap_layout_group (MetaBackend *backend)
{
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (META_BACKEND_WAYLAND_NESTED (backend));

  return priv->xkb_layout_index;
}

static gboolean
meta_backend_wayland_nested_is_lid_closed (MetaBackend *backend)
{
  (void) backend;
  return FALSE;
}

static void
meta_backend_wayland_nested_set_pointer_constraint (MetaBackend           *backend,
                                                    MetaPointerConstraint *constraint)
{
  (void) backend;
  (void) constraint;
}

static MetaBackendCapabilities
meta_backend_wayland_nested_get_capabilities (MetaBackend *backend)
{
  (void) backend;
  return META_BACKEND_CAPABILITY_NONE;
}

static void
meta_backend_wayland_nested_dispose (GObject *object)
{
  MetaBackendWaylandNested *self = META_BACKEND_WAYLAND_NESTED (object);
  MetaBackendWaylandNestedPrivate *priv = meta_backend_wayland_nested_get_instance_private (self);

  g_clear_object (&priv->cursor_renderer);
  g_clear_object (&priv->seat);
  g_clear_object (&priv->input_settings);
  g_clear_object (&priv->gpu);

  if (priv->xkb_keymap) {
    xkb_keymap_unref (priv->xkb_keymap);
    priv->xkb_keymap = NULL;
  }

  G_OBJECT_CLASS (meta_backend_wayland_nested_parent_class)->dispose (object);
}

static void
meta_backend_wayland_nested_init (MetaBackendWaylandNested *self)
{
  (void) self;
}

static void
meta_backend_wayland_nested_class_init (MetaBackendWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaBackendClass *backend_class = META_BACKEND_CLASS (klass);

  object_class->dispose = meta_backend_wayland_nested_dispose;

  backend_class->init_basic = meta_backend_wayland_nested_init_basic;
  backend_class->get_capabilities = meta_backend_wayland_nested_get_capabilities;

#ifdef HAVE_LOGIND
  backend_class->create_launcher = meta_backend_wayland_nested_create_launcher;
#endif

  backend_class->create_clutter_backend = meta_backend_wayland_nested_create_clutter_backend;
  backend_class->create_default_seat = meta_backend_wayland_nested_create_default_seat;

  backend_class->create_renderer = meta_backend_wayland_nested_create_renderer;
  backend_class->create_monitor_manager = meta_backend_wayland_nested_create_monitor_manager;
  backend_class->create_color_manager = meta_backend_wayland_nested_create_color_manager;

  backend_class->get_input_settings = meta_backend_wayland_nested_get_input_settings;

  backend_class->get_cursor_renderer = meta_backend_wayland_nested_get_cursor_renderer;

  backend_class->update_stage = meta_backend_wayland_nested_update_stage;
  backend_class->select_stage_events = meta_backend_wayland_nested_select_stage_events;

  backend_class->get_current_logical_monitor = meta_backend_wayland_nested_get_current_logical_monitor;

  backend_class->set_keymap_async = meta_backend_wayland_nested_set_keymap_async;
  backend_class->get_keymap = meta_backend_wayland_nested_get_keymap;
  backend_class->get_keymap_layout_group = meta_backend_wayland_nested_get_keymap_layout_group;
  backend_class->set_keymap_layout_group_async = meta_backend_wayland_nested_set_keymap_layout_group_async;

  backend_class->is_lid_closed = meta_backend_wayland_nested_is_lid_closed;
  backend_class->set_pointer_constraint = meta_backend_wayland_nested_set_pointer_constraint;
}
