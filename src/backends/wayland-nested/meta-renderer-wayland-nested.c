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

#include "backends/meta-color-manager.h"
#include "backends/meta-logical-monitor-private.h"

#ifdef HAVE_EGL
#include "cogl/cogl-display-private.h"
#include "cogl/cogl.h"
#include "cogl/winsys/cogl-winsys-egl.h"
#endif

#include "backends/wayland-nested/meta-renderer-wayland-nested.h"

#include <wayland-client.h>

struct _MetaRendererWaylandNested
{
  MetaRenderer parent;
};

G_DEFINE_TYPE (MetaRendererWaylandNested,
               meta_renderer_wayland_nested,
               META_TYPE_RENDERER)

#ifdef HAVE_EGL
typedef struct _MetaWaylandNestedEglPlatform
{
  struct wl_display *wl_display;
  EGLDisplay egl_display;
} MetaWaylandNestedEglPlatform;

#define META_TYPE_WAYLAND_NESTED_WINSYS_EGL (meta_wayland_nested_winsys_egl_get_type ())
G_DECLARE_FINAL_TYPE (MetaWaylandNestedWinsysEgl,
                      meta_wayland_nested_winsys_egl,
                      META,
                      WAYLAND_NESTED_WINSYS_EGL,
                      CoglWinsysEGL)

struct _MetaWaylandNestedWinsysEgl
{
  CoglWinsysEGL parent;

  MetaWaylandNestedEglPlatform *platform;
};

G_DEFINE_FINAL_TYPE (MetaWaylandNestedWinsysEgl,
                     meta_wayland_nested_winsys_egl,
                     COGL_TYPE_WINSYS_EGL)

static EGLDisplay
get_egl_display_for_wl_display (struct wl_display *wl_display)
{
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext = NULL;

  get_platform_display_ext = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");

#ifdef EGL_KHR_platform_wayland
  if (get_platform_display_ext) {
    EGLDisplay dpy =
      get_platform_display_ext (EGL_PLATFORM_WAYLAND_KHR,
                                (void *) wl_display,
                                NULL);
    if (dpy != EGL_NO_DISPLAY)
      return dpy;
  }
#endif

  return eglGetDisplay ((EGLNativeDisplayType) wl_display);
}

static gboolean
wayland_nested_display_setup (CoglWinsys  *winsys,
                              CoglDisplay *display,
                              GError     **error)
{
  CoglWinsysClass *parent_winsys_class;
  CoglDisplayEGL *display_egl;
  CoglRenderer *renderer;
  CoglRendererEGL *renderer_egl;

  parent_winsys_class = g_type_class_peek_parent (COGL_WINSYS_EGL_GET_CLASS (winsys));
  if (!parent_winsys_class->display_setup (winsys, display, error))
    return FALSE;

  display_egl = display->winsys;
  renderer = cogl_display_get_renderer (display);
  renderer_egl = cogl_renderer_get_winsys_data (renderer);

  display_egl->platform = renderer_egl->platform;

  return TRUE;
}

static void
wayland_nested_display_destroy (CoglWinsys  *winsys,
                                CoglDisplay *display)
{
  CoglWinsysClass *parent_winsys_class;

  parent_winsys_class = g_type_class_peek_parent (COGL_WINSYS_EGL_GET_CLASS (winsys));
  parent_winsys_class->display_destroy (winsys, display);
}

static int
wayland_nested_add_config_attributes (CoglWinsysEGL *winsys,
                                      CoglDisplay   *display,
                                      EGLint        *attributes)
{
  int i = 0;

  (void) winsys;
  (void) display;

  attributes[i++] = EGL_SURFACE_TYPE;
  attributes[i++] = EGL_PBUFFER_BIT;

  return i;
}

static gboolean
wayland_nested_choose_config (CoglWinsysEGL  *winsys,
                              CoglDisplay    *display,
                              EGLint         *attributes,
                              EGLConfig      *out_config,
                              GError        **error)
{
  CoglRenderer *renderer;
  CoglRendererEGL *renderer_egl;
  EGLDisplay egl_display;
  EGLConfig config = NULL;
  EGLint n_configs = 0;

  (void) winsys;

  renderer = cogl_display_get_renderer (display);
  renderer_egl = cogl_renderer_get_winsys_data (renderer);
  egl_display = renderer_egl->edpy;

  if (!eglChooseConfig (egl_display, attributes, &config, 1, &n_configs) || n_configs < 1) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "eglChooseConfig failed (no matching EGLConfig)");
    return FALSE;
  }

  *out_config = config;
  return TRUE;
}

static EGLSurface
create_dummy_pbuffer (EGLDisplay egl_display,
                      EGLConfig  egl_config)
{
  static const EGLint pbuf_attribs[] = {
    EGL_WIDTH, 16,
    EGL_HEIGHT, 16,
    EGL_NONE
  };

  return eglCreatePbufferSurface (egl_display, egl_config, pbuf_attribs);
}

static gboolean
wayland_nested_context_created (CoglWinsysEGL  *winsys,
                                CoglDisplay    *display,
                                GError        **error)
{
  CoglDisplayEGL *display_egl;
  CoglRenderer *renderer;
  CoglRendererEGL *renderer_egl;

  (void) winsys;

  display_egl = display->winsys;
  renderer = cogl_display_get_renderer (display);
  renderer_egl = cogl_renderer_get_winsys_data (renderer);

  /*
   * if EGL winsys doesn’t support surfaceless contexts, we must provide a dummy
   * surface so _cogl_winsys_egl_make_current can succeed.
   */
  if ((renderer_egl->private_features & COGL_EGL_WINSYS_FEATURE_SURFACELESS_CONTEXT) == 0) {
    if (display_egl->dummy_surface == EGL_NO_SURFACE) {
      display_egl->dummy_surface = create_dummy_pbuffer (renderer_egl->edpy, display_egl->egl_config);

      if (display_egl->dummy_surface == EGL_NO_SURFACE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to create dummy EGL pbuffer surface");
        return FALSE;
      }
    }
  } else {
    /* surfaceless: keep dummy_surface as EGL_NO_SURFACE */
    display_egl->dummy_surface = EGL_NO_SURFACE;
  }

  if (!_cogl_winsys_egl_make_current (display,
                                      display_egl->dummy_surface,
                                      display_egl->dummy_surface,
                                      display_egl->egl_context)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to make EGL context current");
    return FALSE;
  }

  return TRUE;
}

static void
wayland_nested_cleanup_context (CoglWinsysEGL *winsys,
                                CoglDisplay   *display)
{
  CoglDisplayEGL *display_egl;
  CoglRenderer *renderer;
  CoglRendererEGL *renderer_egl;

  (void) winsys;

  display_egl = display->winsys;
  renderer = cogl_display_get_renderer (display);
  renderer_egl = cogl_renderer_get_winsys_data (renderer);

  if (display_egl->dummy_surface != EGL_NO_SURFACE) {
    eglDestroySurface (renderer_egl->edpy, display_egl->dummy_surface);
    display_egl->dummy_surface = EGL_NO_SURFACE;
  }
}

static gboolean
meta_wayland_nested_renderer_connect (CoglWinsys   *winsys,
                                      CoglRenderer *cogl_renderer,
                                      GError      **error)
{
  MetaWaylandNestedWinsysEgl *winsys_egl = META_WAYLAND_NESTED_WINSYS_EGL (winsys);
  MetaWaylandNestedEglPlatform *platform = NULL;
  CoglRendererEGL *cogl_renderer_egl = NULL;
  CoglWinsysClass *parent_winsys_class;

  platform = g_new0 (MetaWaylandNestedEglPlatform, 1);

  platform->wl_display = wl_display_connect (NULL);
  if (!platform->wl_display) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to connect to host Wayland display (WAYLAND_DISPLAY)");
    g_free (platform);
    return FALSE;
  }

  platform->egl_display = get_egl_display_for_wl_display (platform->wl_display);
  if (platform->egl_display == EGL_NO_DISPLAY) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to get EGLDisplay for host Wayland display");
    wl_display_disconnect (platform->wl_display);
    g_free (platform);
    return FALSE;
  }

  cogl_renderer_set_winsys_data (cogl_renderer,
                                 g_new0 (CoglRendererEGL, 1),
                                 g_free);
  cogl_renderer_egl = cogl_renderer_get_winsys_data (cogl_renderer);

  winsys_egl->platform = platform;
  cogl_renderer_egl->platform = platform;
  cogl_renderer_egl->edpy = platform->egl_display;
  cogl_renderer_egl->needs_config = TRUE;

  parent_winsys_class = g_type_class_peek_parent (COGL_WINSYS_EGL_GET_CLASS (winsys));
  if (!parent_winsys_class->renderer_connect (winsys, cogl_renderer, error))
    return FALSE;

  return TRUE;
}

static void
meta_wayland_nested_winsys_egl_finalize (GObject *object)
{
  MetaWaylandNestedWinsysEgl *winsys_egl = META_WAYLAND_NESTED_WINSYS_EGL (object);

  if (winsys_egl->platform) {
    if (winsys_egl->platform->wl_display)
      wl_display_disconnect (winsys_egl->platform->wl_display);
    g_free (winsys_egl->platform);
    winsys_egl->platform = NULL;
  }

  G_OBJECT_CLASS (meta_wayland_nested_winsys_egl_parent_class)->finalize (object);
}

static void
meta_wayland_nested_winsys_egl_class_init (MetaWaylandNestedWinsysEglClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CoglWinsysClass *winsys_class = COGL_WINSYS_CLASS (klass);
  CoglWinsysEGLClass *winsys_egl_class = COGL_WINSYS_EGL_CLASS (klass);

  object_class->finalize = meta_wayland_nested_winsys_egl_finalize;

  winsys_class->renderer_connect = meta_wayland_nested_renderer_connect;
  winsys_class->display_setup = wayland_nested_display_setup;
  winsys_class->display_destroy = wayland_nested_display_destroy;

  winsys_egl_class->add_config_attributes = wayland_nested_add_config_attributes;
  winsys_egl_class->choose_config = wayland_nested_choose_config;
  winsys_egl_class->context_created = wayland_nested_context_created;
  winsys_egl_class->cleanup_context = wayland_nested_cleanup_context;
}

static void
meta_wayland_nested_winsys_egl_init (MetaWaylandNestedWinsysEgl *self)
{
}
#endif /* HAVE_EGL */

static CoglRenderer *
meta_renderer_wayland_nested_create_cogl_renderer (MetaRenderer *renderer)
{
  CoglRenderer *cogl_renderer;

  (void) renderer;

  cogl_renderer = cogl_renderer_new ();

#ifdef HAVE_EGL
  CoglWinsys *winsys;

  winsys = g_object_new (META_TYPE_WAYLAND_NESTED_WINSYS_EGL,
                         "name", "EGL_WAYLAND_NESTED",
                         NULL);

  cogl_renderer_set_custom_winsys (cogl_renderer, winsys);
#else
  meta_fatal ("Wayland nested backend requires EGL support in this build");
#endif

  return cogl_renderer;
}

static CoglOffscreen *
create_offscreen (CoglContext *cogl_context,
                  int          width,
                  int          height)
{
  CoglTexture *texture_2d;
  CoglOffscreen *offscreen;
  g_autoptr (GError) local_error = NULL;

  texture_2d = cogl_texture_2d_new_with_size (cogl_context, width, height);
  offscreen = cogl_offscreen_new_with_texture (texture_2d);

  if (!cogl_framebuffer_allocate (COGL_FRAMEBUFFER (offscreen), &local_error))
    meta_fatal ("Couldn't allocate framebuffer: %s", local_error->message);

  return offscreen;
}

static MetaRendererView *
meta_renderer_wayland_nested_create_view (MetaRenderer        *renderer,
                                          MetaLogicalMonitor  *logical_monitor,
                                          MetaMonitor         *monitor,
                                          MetaOutput          *output,
                                          MetaCrtc            *crtc,
                                          GError             **error)
{
  MetaBackend *backend = meta_renderer_get_backend (renderer);
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  CoglContext *cogl_context = clutter_backend_get_cogl_context (clutter_backend);

  MetaColorManager *color_manager = meta_backend_get_color_manager (backend);
  MetaColorDevice *color_device = meta_color_manager_get_color_device (color_manager, monitor);

  float view_scale;
  const MetaCrtcConfig *crtc_config;
  int width, height;
  CoglOffscreen *fake_onscreen;
  MtkRectangle view_layout;
  const MetaCrtcModeInfo *mode_info;
  MetaRendererView *view;

  (void) error;

  if (meta_backend_is_stage_views_scaled (backend))
    view_scale = logical_monitor->scale;
  else
    view_scale = 1.0f;

  crtc_config = meta_crtc_get_config (crtc);

  width = (int) roundf (crtc_config->layout.size.width * view_scale);
  height = (int) roundf (crtc_config->layout.size.height * view_scale);

  fake_onscreen = create_offscreen (cogl_context, width, height);

  mtk_rectangle_from_graphene_rect (&crtc_config->layout,
                                    MTK_ROUNDING_STRATEGY_ROUND,
                                    &view_layout);

  mode_info = meta_crtc_mode_get_info (crtc_config->mode);

  view = g_object_new (META_TYPE_RENDERER_VIEW,
                       "name", meta_output_get_name (output),
                       "backend", backend,
                       "color-device", color_device,
                       "stage", meta_backend_get_stage (backend),
                       "layout", &view_layout,
                       "crtc", crtc,
                       "refresh-rate", mode_info->refresh_rate,
                       "framebuffer", COGL_FRAMEBUFFER (fake_onscreen),
                       "transform", MTK_MONITOR_TRANSFORM_NORMAL,
                       "scale", view_scale,
                       NULL);

  g_object_set_data (G_OBJECT (view), "crtc", crtc);

  return view;
}

static void
meta_renderer_wayland_nested_init (MetaRendererWaylandNested *self)
{
}

static void
meta_renderer_wayland_nested_class_init (MetaRendererWaylandNestedClass *klass)
{
  MetaRendererClass *renderer_class = META_RENDERER_CLASS (klass);

  renderer_class->create_cogl_renderer = meta_renderer_wayland_nested_create_cogl_renderer;
  renderer_class->create_view = meta_renderer_wayland_nested_create_view;
}
