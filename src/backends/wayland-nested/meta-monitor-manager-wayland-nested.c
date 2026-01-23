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

#include "backends/meta-monitor-config-manager.h"

#include "backends/wayland-nested/meta-monitor-manager-wayland-nested.h"

struct _MetaMonitorManagerWaylandNested
{
  MetaMonitorManager parent_instance;
};

G_DEFINE_TYPE (MetaMonitorManagerWaylandNested,
               meta_monitor_manager_wayland_nested,
               META_TYPE_MONITOR_MANAGER)

static MetaGpu *
get_gpu (MetaMonitorManager *manager)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  return META_GPU (meta_backend_get_gpus (backend)->data);
}

static void
meta_monitor_manager_wayland_nested_read_current_state (MetaMonitorManager *manager)
{
  MetaMonitorManagerClass *parent_class = META_MONITOR_MANAGER_CLASS (meta_monitor_manager_wayland_nested_parent_class);

  if (parent_class->read_current_state)
    parent_class->read_current_state (manager);
}

static void
apply_crtc_assignments (MetaMonitorManager    *manager,
                        MetaCrtcAssignment   **crtcs,
                        unsigned int           n_crtcs,
                        MetaOutputAssignment **outputs,
                        unsigned int           n_outputs)
{
  g_autoptr (GList) to_configure_outputs = NULL;
  g_autoptr (GList) to_configure_crtcs = NULL;
  unsigned int i;

  to_configure_outputs = g_list_copy (meta_gpu_get_outputs (get_gpu (manager)));
  to_configure_crtcs = g_list_copy (meta_gpu_get_crtcs (get_gpu (manager)));

  for (i = 0; i < n_crtcs; i++) {
    MetaCrtcAssignment *crtc_assignment = crtcs[i];
    MetaCrtc *crtc = crtc_assignment->crtc;

    to_configure_crtcs = g_list_remove (to_configure_crtcs, crtc);

    if (crtc_assignment->mode == NULL) {
      meta_crtc_unset_config (crtc);
    } else {
      MetaCrtcConfig *crtc_config;
      unsigned int j;

      crtc_config = meta_crtc_config_new (&crtc_assignment->layout,
                                          crtc_assignment->mode,
                                          crtc_assignment->transform);
      meta_crtc_set_config (crtc, crtc_config,
                            crtc_assignment->backend_private);

      for (j = 0; j < crtc_assignment->outputs->len; j++) {
        MetaOutput *output;
        MetaOutputAssignment *output_assignment;

        output = ((MetaOutput **) crtc_assignment->outputs->pdata)[j];

        to_configure_outputs = g_list_remove (to_configure_outputs, output);

        output_assignment = meta_find_output_assignment (outputs,
                                                         n_outputs,
                                                         output);
        meta_output_assign_crtc (output, crtc, output_assignment);
      }
    }
  }

  g_list_foreach (to_configure_crtcs, (GFunc) meta_crtc_unset_config, NULL);
  g_list_foreach (to_configure_outputs, (GFunc) meta_output_unassign_crtc, NULL);
}

static void
update_screen_size (MetaMonitorManager *manager,
                    MetaMonitorsConfig *config)
{
  GList *l;
  int screen_width = 0;
  int screen_height = 0;

  for (l = config->logical_monitor_configs; l; l = l->next) {
    MetaLogicalMonitorConfig *logical_monitor_config = l->data;
    int right_edge = logical_monitor_config->layout.x + logical_monitor_config->layout.width;
    int bottom_edge = logical_monitor_config->layout.y + logical_monitor_config->layout.height;

    if (right_edge > screen_width)
      screen_width = right_edge;
    if (bottom_edge > screen_height)
      screen_height = bottom_edge;
  }

  manager->screen_width = screen_width;
  manager->screen_height = screen_height;
}

static gboolean
meta_monitor_manager_wayland_nested_apply_monitors_config (MetaMonitorManager      *manager,
                                                           MetaMonitorsConfig      *config,
                                                           MetaMonitorsConfigMethod method,
                                                           GError                 **error)
{
  GPtrArray *crtc_assignments;
  GPtrArray *output_assignments;

  if (!config) {
    manager->screen_width = META_MONITOR_MANAGER_MIN_SCREEN_WIDTH;
    manager->screen_height = META_MONITOR_MANAGER_MIN_SCREEN_HEIGHT;

    meta_monitor_manager_rebuild (manager, NULL);
    return TRUE;
  }

  if (!meta_monitor_config_manager_assign (manager,
                                           config,
                                           &crtc_assignments,
                                           &output_assignments,
                                           error))
      return FALSE;

  if (method == META_MONITORS_CONFIG_METHOD_VERIFY) {
    g_ptr_array_free (crtc_assignments, TRUE);
    g_ptr_array_free (output_assignments, TRUE);
    return TRUE;
  }

  apply_crtc_assignments (manager,
                          (MetaCrtcAssignment **) crtc_assignments->pdata,
                          crtc_assignments->len,
                          (MetaOutputAssignment **) output_assignments->pdata,
                          output_assignments->len);

  g_ptr_array_free (crtc_assignments, TRUE);
  g_ptr_array_free (output_assignments, TRUE);

  update_screen_size (manager, config);
  meta_monitor_manager_rebuild (manager, config);

  return TRUE;
}

static void
meta_monitor_manager_wayland_nested_ensure_initial_config (MetaMonitorManager *manager)
{
  MetaMonitorsConfig *config;

  config = meta_monitor_manager_ensure_configured (manager);
  meta_monitor_manager_update_logical_state (manager, config, NULL);
}

static float
meta_monitor_manager_wayland_nested_calculate_monitor_mode_scale (MetaMonitorManager           *manager,
                                                                  MetaLogicalMonitorLayoutMode  layout_mode,
                                                                  MetaMonitor                  *monitor,
                                                                  MetaMonitorMode              *monitor_mode)
{
  (void) manager;
  (void) layout_mode;
  (void) monitor;
  (void) monitor_mode;
  return 1.0f;
}

static float *
meta_monitor_manager_wayland_nested_calculate_supported_scales (MetaMonitorManager           *manager,
                                                                MetaLogicalMonitorLayoutMode  layout_mode,
                                                                MetaMonitor                  *monitor,
                                                                MetaMonitorMode              *monitor_mode,
                                                                int                          *n_supported_scales)
{
  MetaMonitorScalesConstraint constraints = META_MONITOR_SCALES_CONSTRAINT_NONE;

  (void) manager;

  if (layout_mode == META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL)
    constraints |= META_MONITOR_SCALES_CONSTRAINT_NO_FRAC;

  return meta_monitor_calculate_supported_scales (monitor,
                                                  monitor_mode,
                                                  constraints,
                                                  n_supported_scales);
}

static gboolean
is_monitor_framebuffers_scaled (MetaMonitorManager *manager)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaSettings *settings = meta_backend_get_settings (backend);

  return meta_settings_is_experimental_feature_enabled (settings, META_EXPERIMENTAL_FEATURE_SCALE_MONITOR_FRAMEBUFFER);
}

static MetaMonitorManagerCapability
meta_monitor_manager_wayland_nested_get_capabilities (MetaMonitorManager *manager)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaSettings *settings = meta_backend_get_settings (backend);
  MetaMonitorManagerCapability capabilities = META_MONITOR_MANAGER_CAPABILITY_NONE;

  if (meta_settings_is_experimental_feature_enabled (settings, META_EXPERIMENTAL_FEATURE_SCALE_MONITOR_FRAMEBUFFER))
    capabilities |= META_MONITOR_MANAGER_CAPABILITY_LAYOUT_MODE;

  return capabilities;
}

static gboolean
meta_monitor_manager_wayland_nested_get_max_screen_size (MetaMonitorManager *manager,
                                                         int                *max_width,
                                                         int                *max_height)
{
  (void) manager;
  (void) max_width;
  (void) max_height;
  return FALSE;
}

static MetaLogicalMonitorLayoutMode
meta_monitor_manager_wayland_nested_get_default_layout_mode (MetaMonitorManager *manager)
{
  if (is_monitor_framebuffers_scaled (manager))
    return META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL;

  return META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL;
}

static void
meta_monitor_manager_wayland_nested_constructed (GObject *object)
{
  G_OBJECT_CLASS (meta_monitor_manager_wayland_nested_parent_class)->constructed (object);
}

static void
meta_monitor_manager_wayland_nested_class_init (MetaMonitorManagerWaylandNestedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_CLASS (klass);

  object_class->constructed = meta_monitor_manager_wayland_nested_constructed;

  manager_class->read_current_state = meta_monitor_manager_wayland_nested_read_current_state;
  manager_class->ensure_initial_config = meta_monitor_manager_wayland_nested_ensure_initial_config;
  manager_class->apply_monitors_config = meta_monitor_manager_wayland_nested_apply_monitors_config;

  manager_class->calculate_monitor_mode_scale = meta_monitor_manager_wayland_nested_calculate_monitor_mode_scale;
  manager_class->calculate_supported_scales = meta_monitor_manager_wayland_nested_calculate_supported_scales;

  manager_class->get_capabilities = meta_monitor_manager_wayland_nested_get_capabilities;
  manager_class->get_max_screen_size = meta_monitor_manager_wayland_nested_get_max_screen_size;
  manager_class->get_default_layout_mode = meta_monitor_manager_wayland_nested_get_default_layout_mode;
}

static void
meta_monitor_manager_wayland_nested_init (MetaMonitorManagerWaylandNested *self)
{
  (void) self;
}
