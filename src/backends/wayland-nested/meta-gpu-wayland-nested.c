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

#include "backends/meta-monitor-private.h"

#include "backends/wayland-nested/meta-gpu-wayland-nested.h"

#define MAX_MODES 8

typedef struct _MetaOutputWaylandNested
{
  MetaOutput parent;
  float scale;
} MetaOutputWaylandNested;

typedef struct _MetaCrtcWaylandNested
{
  MetaCrtc parent;
} MetaCrtcWaylandNested;

#define META_TYPE_OUTPUT_WAYLAND_NESTED (meta_output_wayland_nested_get_type ())
G_DECLARE_FINAL_TYPE (MetaOutputWaylandNested,
                      meta_output_wayland_nested,
                      META, OUTPUT_WAYLAND_NESTED,
                      MetaOutput)

#define META_TYPE_CRTC_WAYLAND_NESTED   (meta_crtc_wayland_nested_get_type ())
G_DECLARE_FINAL_TYPE (MetaCrtcWaylandNested,
                      meta_crtc_wayland_nested,
                      META, CRTC_WAYLAND_NESTED,
                      MetaCrtc)

G_DEFINE_TYPE (MetaOutputWaylandNested,
               meta_output_wayland_nested,
               META_TYPE_OUTPUT)

G_DEFINE_TYPE (MetaCrtcWaylandNested,
               meta_crtc_wayland_nested,
               META_TYPE_CRTC)

struct _MetaGpuWaylandNested
{
  MetaGpu parent;
};

typedef struct _CrtcModeSpec
{
  int width;
  int height;
  float refresh_rate;
} CrtcModeSpec;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CrtcModeSpec, g_free);

static size_t
meta_crtc_wayland_nested_get_gamma_lut_size (MetaCrtc *crtc)
{
  (void) crtc;
  return 0;
}

static MetaGammaLut *
meta_crtc_wayland_nested_get_gamma_lut (MetaCrtc *crtc)
{
  (void) crtc;
  return NULL;
}

static void
meta_crtc_wayland_nested_set_gamma_lut (MetaCrtc           *crtc,
                                        const MetaGammaLut *lut)
{
  (void) crtc;
  (void) lut;
}

static void
meta_crtc_wayland_nested_class_init (MetaCrtcWaylandNestedClass *klass)
{
  MetaCrtcClass *crtc_class = META_CRTC_CLASS (klass);

  crtc_class->get_gamma_lut_size = meta_crtc_wayland_nested_get_gamma_lut_size;
  crtc_class->get_gamma_lut = meta_crtc_wayland_nested_get_gamma_lut;
  crtc_class->set_gamma_lut = meta_crtc_wayland_nested_set_gamma_lut;
}

static void
meta_crtc_wayland_nested_init (MetaCrtcWaylandNested *self)
{
  (void) self;
}

static void
meta_output_wayland_nested_class_init (MetaOutputWaylandNestedClass *klass)
{
  (void) klass;
}

static void
meta_output_wayland_nested_init (MetaOutputWaylandNested *self)
{
  self->scale = 1.0f;
}

static MetaCrtcMode *
create_mode (CrtcModeSpec *spec,
             long          mode_id)
{
  g_autoptr (MetaCrtcModeInfo) crtc_mode_info = NULL;

  crtc_mode_info = meta_crtc_mode_info_new ();
  crtc_mode_info->width = spec->width;
  crtc_mode_info->height = spec->height;
  crtc_mode_info->refresh_rate = spec->refresh_rate;

  return g_object_new (META_TYPE_CRTC_MODE,
                       "id", (uint64_t) mode_id,
                       "info", crtc_mode_info,
                       NULL);
}

static gboolean
parse_resolution_env (const char *s,
                      int        *out_w,
                      int        *out_h,
                      float      *out_rr)
{
  int w = 0, h = 0;
  float rr = 60.0f;

  if (!s || *s == '\0')
    return FALSE;

  /* accepts strings like 1920x1080@60, 1920x1080, etc */
  if (!meta_parse_monitor_mode (s, &w, &h, &rr, 60.0f))
    return FALSE;

  if (w <= 0 || h <= 0 || rr <= 0.0f)
    return FALSE;

  *out_w = w;
  *out_h = h;
  *out_rr = rr;
  return TRUE;
}

/*
 * if env override is set, vary connector name so stored configs don't match,
 * allowing Mutter to generate a fresh config from preferred_mode.
 *
 * default:
 *  stable connector name: WLNEST1
 *  preferred mode: 1920x1080@60
 */
static char *
build_connector_name (int      width,
                      int      height,
                      gboolean env_override_active)
{
  if (env_override_active)
    return g_strdup_printf ("WLNEST-%dx%d", width, height);

  return g_strdup ("WLNEST1");
}

static void
meta_gpu_wayland_nested_build_outputs (MetaGpu *gpu)
{
  MetaBackend *backend = meta_gpu_get_backend (gpu);

  int pref_w = 1920;
  int pref_h = 1080;
  float pref_rr = 60.0f;

  const char *env = g_getenv ("MUTTER_WAYLAND_NESTED_DISPLAY_RESOLUTION");
  gboolean env_override = FALSE;

  if (parse_resolution_env (env, &pref_w, &pref_h, &pref_rr))
    env_override = TRUE;

  CrtcModeSpec default_specs[] = {
    { .width = pref_w, .height = pref_h, .refresh_rate = pref_rr }, /* preferred */
    { .width = 1920, .height = 1080, .refresh_rate = 60.0f },
    { .width = 1600, .height = 900,  .refresh_rate = 60.0f },
    { .width = 1280, .height = 720,  .refresh_rate = 60.0f },
    { .width = 1024, .height = 768,  .refresh_rate = 60.0f },
    { .width = 800,  .height = 600,  .refresh_rate = 60.0f },
  };

  g_autoptr (GPtrArray) unique_specs = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < G_N_ELEMENTS (default_specs) && unique_specs->len < MAX_MODES; i++) {
    gboolean seen = FALSE;
    for (guint j = 0; j < unique_specs->len; j++) {
      CrtcModeSpec *s = unique_specs->pdata[j];
      if (s->width == default_specs[i].width &&
          s->height == default_specs[i].height &&
          s->refresh_rate == default_specs[i].refresh_rate) {
        seen = TRUE;
        break;
      }
    }

    if (!seen) {
      CrtcModeSpec *copy = g_new0 (CrtcModeSpec, 1);
      *copy = default_specs[i];
      g_ptr_array_add (unique_specs, copy);
    }
  }

  GList *modes = NULL;
  GList *crtcs = NULL;
  GList *outputs = NULL;

  for (guint i = 0; i < unique_specs->len; i++) {
    CrtcModeSpec *spec = unique_specs->pdata[i];
    long mode_id = (long) i + 1;
    modes = g_list_append (modes, create_mode (spec, mode_id));
  }

  MetaCrtc *crtc = g_object_new (META_TYPE_CRTC_WAYLAND_NESTED,
                                 "id", (uint64_t) 1,
                                 "backend", backend,
                                 "gpu", gpu,
                                 NULL);
  crtcs = g_list_append (crtcs, crtc);

  g_autoptr (MetaOutputInfo) output_info = meta_output_info_new ();

  output_info->name = build_connector_name (pref_w, pref_h, env_override);
  output_info->vendor = g_strdup ("FuriLabs");
  output_info->product = g_strdup ("Convergence");
  output_info->serial = g_strdup ("0xFURILABS");

  /* arbitrary but plausible */
  output_info->width_mm = 344;
  output_info->height_mm = 194;
  output_info->subpixel_order = META_SUBPIXEL_ORDER_UNKNOWN;
  output_info->connector_type = META_CONNECTOR_TYPE_VIRTUAL;
  output_info->n_possible_clones = 0;

  /* preferred mode = first mode in list */
  output_info->preferred_mode = modes ? modes->data : NULL;

  output_info->n_modes = (unsigned int) g_list_length (modes);
  output_info->modes = g_new0 (MetaCrtcMode *, output_info->n_modes);

  GList *l = NULL;
  guint idx = 0;
  for (l = modes; l; l = l->next, idx++)
    output_info->modes[idx] = g_object_ref (META_CRTC_MODE (l->data));

  output_info->possible_crtcs = g_new0 (MetaCrtc *, 1);
  output_info->possible_crtcs[0] = crtc;
  output_info->n_possible_crtcs = 1;

  MetaOutput *output = g_object_new (META_TYPE_OUTPUT_WAYLAND_NESTED,
                                     "id", (uint64_t) 1,
                                     "gpu", gpu,
                                     "info", output_info,
                                     NULL);
  outputs = g_list_append (outputs, output);

  meta_gpu_take_modes (gpu, modes);
  meta_gpu_take_crtcs (gpu, crtcs);
  meta_gpu_take_outputs (gpu, outputs);
}

static gboolean
meta_gpu_wayland_nested_read_current (MetaGpu  *gpu,
                                      GError  **error)
{
  (void) error;
  meta_gpu_wayland_nested_build_outputs (gpu);
  return TRUE;
}

G_DEFINE_TYPE (MetaGpuWaylandNested, meta_gpu_wayland_nested, META_TYPE_GPU)

static void
meta_gpu_wayland_nested_class_init (MetaGpuWaylandNestedClass *klass)
{
  MetaGpuClass *gpu_class = META_GPU_CLASS (klass);
  gpu_class->read_current = meta_gpu_wayland_nested_read_current;
}

static void
meta_gpu_wayland_nested_init (MetaGpuWaylandNested *self)
{
  (void) self;
}
