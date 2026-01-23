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

#include "backends/meta-renderer.h"

#include "backends/wayland-nested/meta-stage-wayland-nested.h"

struct _MetaStageWaylandNested
{
  MetaStageImpl parent;
};

G_DEFINE_FINAL_TYPE (MetaStageWaylandNested,
                     meta_stage_wayland_nested,
                     META_TYPE_STAGE_IMPL)

static gboolean
meta_stage_wayland_nested_can_clip_redraws (ClutterStageWindow *stage_window)
{
  (void) stage_window;
  return TRUE;
}

static void
meta_stage_wayland_nested_get_geometry (ClutterStageWindow *stage_window,
                                        MtkRectangle       *geometry)
{
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);
  MetaBackend *backend = meta_stage_impl_get_backend (stage_impl);
  MetaMonitorManager *monitor_manager = meta_backend_get_monitor_manager (backend);

  if (monitor_manager) {
    int width, height;

    meta_monitor_manager_get_screen_size (monitor_manager, &width, &height);
    *geometry = (MtkRectangle) {
      .width = width,
      .height = height,
    };
  } else {
    *geometry = (MtkRectangle) {
      .width = 1,
      .height = 1,
    };
  }
}

static GList *
meta_stage_wayland_nested_get_views (ClutterStageWindow *stage_window)
{
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);
  MetaBackend *backend = meta_stage_impl_get_backend (stage_impl);
  MetaRenderer *renderer = meta_backend_get_renderer (backend);

  return meta_renderer_get_views (renderer);
}

static void
meta_stage_wayland_nested_prepare_frame (ClutterStageWindow *stage_window,
                                         ClutterStageView   *stage_view,
                                         ClutterFrame       *frame)
{
  (void) stage_window;
  (void) stage_view;
  (void) frame;
}

static void
meta_stage_wayland_nested_redraw_view (ClutterStageWindow *stage_window,
                                       ClutterStageView   *view,
                                       ClutterFrame       *frame)
{
  CLUTTER_STAGE_WINDOW_CLASS (meta_stage_wayland_nested_parent_class)->redraw_view (stage_window, view, frame);

  if (!clutter_frame_has_result (frame))
    clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_PENDING_PRESENTED);

  (void) view;
}

static void
meta_stage_wayland_nested_finish_frame (ClutterStageWindow *stage_window,
                                        ClutterStageView   *stage_view,
                                        ClutterFrame       *frame)
{
  (void) stage_window;
  (void) stage_view;

  if (!clutter_frame_has_result (frame))
    clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_IDLE);
}

static void
meta_stage_wayland_nested_init (MetaStageWaylandNested *self)
{
  (void) self;
}

static void
meta_stage_wayland_nested_class_init (MetaStageWaylandNestedClass *klass)
{
  ClutterStageWindowClass *window_class = CLUTTER_STAGE_WINDOW_CLASS (klass);

  window_class->can_clip_redraws = meta_stage_wayland_nested_can_clip_redraws;
  window_class->get_geometry = meta_stage_wayland_nested_get_geometry;
  window_class->get_views = meta_stage_wayland_nested_get_views;
  window_class->prepare_frame = meta_stage_wayland_nested_prepare_frame;
  window_class->redraw_view = meta_stage_wayland_nested_redraw_view;
  window_class->finish_frame = meta_stage_wayland_nested_finish_frame;
}
