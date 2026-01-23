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
#include "cogl/winsys/cogl-winsys-egl-private.h"
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
wayland_nested_display_setup (CoglDisplay *display,
                              GError     **error)
{
  CoglDisplayEGL *display_egl = display->winsys;
  CoglRendererEGL *renderer_egl = cogl_renderer_get_winsys (display->renderer);

  (void) error;

  display_egl->platform = renderer_egl->platform;

  return TRUE;
}

static void
wayland_nested_display_destroy (CoglDisplay *display)
{
  (void) display;
}

static int
wayland_nested_add_config_attributes (CoglDisplay *display,
                                      EGLint      *attributes)
{
  int i = 0;

  (void) display;

  attributes[i++] = EGL_SURFACE_TYPE;
  attributes[i++] = EGL_PBUFFER_BIT;

  return i;
}

static gboolean
wayland_nested_choose_config (CoglDisplay *display,
                              EGLint      *attributes,
                              EGLConfig   *out_config,
                              GError     **error)
{
  CoglRendererEGL *renderer_egl = cogl_renderer_get_winsys (display->renderer);
  EGLDisplay egl_display = renderer_egl->edpy;

  EGLConfig config = NULL;
  EGLint n_configs = 0;

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
wayland_nested_context_created (CoglDisplay *display,
                                GError     **error)
{
  CoglDisplayEGL *display_egl = display->winsys;
  CoglRendererEGL *renderer_egl = cogl_renderer_get_winsys (display->renderer);

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
wayland_nested_cleanup_context (CoglDisplay *display)
{
  CoglDisplayEGL *display_egl = display->winsys;
  CoglRendererEGL *renderer_egl = cogl_renderer_get_winsys (display->renderer);

  if (display_egl->dummy_surface != EGL_NO_SURFACE) {
    eglDestroySurface (renderer_egl->edpy, display_egl->dummy_surface);
    display_egl->dummy_surface = EGL_NO_SURFACE;
  }
}

static gboolean
wayland_nested_context_init (CoglContext *context,
                             GError     **error)
{
  (void) context;
  (void) error;
  return TRUE;
}

static void
wayland_nested_context_deinit (CoglContext *context)
{
  (void) context;
}

static const CoglWinsysEGLVtable wayland_nested_egl_platform_vtable = {
  .display_setup = wayland_nested_display_setup,
  .display_destroy = wayland_nested_display_destroy,
  .context_created = wayland_nested_context_created,
  .cleanup_context = wayland_nested_cleanup_context,
  .context_init = wayland_nested_context_init,
  .context_deinit = wayland_nested_context_deinit,
  .add_config_attributes = wayland_nested_add_config_attributes,
  .choose_config = wayland_nested_choose_config,
};

static gboolean
meta_wayland_nested_renderer_connect (CoglRenderer *cogl_renderer,
                                      GError      **error)
{
  MetaWaylandNestedEglPlatform *platform = NULL;
  CoglRendererEGL *cogl_renderer_egl = NULL;

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

  cogl_renderer_set_winsys (cogl_renderer, g_new0 (CoglRendererEGL, 1));
  cogl_renderer_egl = cogl_renderer_get_winsys (cogl_renderer);

  cogl_renderer_egl->platform_vtable = &wayland_nested_egl_platform_vtable;
  cogl_renderer_egl->platform = platform;
  cogl_renderer_egl->edpy = platform->egl_display;

  cogl_renderer_egl->needs_config = TRUE;

  if (!_cogl_winsys_egl_renderer_connect_common (cogl_renderer, error)) {
    wl_display_disconnect (platform->wl_display);
    g_free (platform);
    return FALSE;
  }

  return TRUE;
}

static const CoglWinsysVtable *
get_wayland_nested_cogl_winsys_vtable (CoglRenderer *cogl_renderer)
{
  static gboolean vtable_inited = FALSE;
  static CoglWinsysVtable vtable;

  (void) cogl_renderer;

  if (!vtable_inited) {
    vtable = *_cogl_winsys_egl_get_vtable ();

    vtable.id = COGL_WINSYS_ID_CUSTOM;
    vtable.name = "EGL_WAYLAND_NESTED";

    /* override connect so EGL winsys is initialized correctly. */
    vtable.renderer_connect = meta_wayland_nested_renderer_connect;
    vtable_inited = TRUE;
  }

  return &vtable;
}
#endif /* HAVE_EGL */

static CoglRenderer *
meta_renderer_wayland_nested_create_cogl_renderer (MetaRenderer *renderer)
{
  CoglRenderer *cogl_renderer;

  (void) renderer;

  cogl_renderer = cogl_renderer_new ();

#ifdef HAVE_EGL
  cogl_renderer_set_custom_winsys (cogl_renderer,
                                   get_wayland_nested_cogl_winsys_vtable,
                                   NULL);
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
