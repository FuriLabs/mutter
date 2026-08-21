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

#include "backends/meta-furios-screen-cast-stream-src-native-buffer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <hybris/eglplatformcommon/hybris_nativebufferext.h>
#include <hardware/gralloc.h>

#include "backends/meta-stage-private.h"
#include "backends/meta-backend-private.h"

enum
{
  SIGNAL_FRAME_PUBLISHED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

struct _MetaFuriosScreenCastStreamSrcNativeBuffer
{
  GObject parent_instance;

  MetaBackend *backend;

  guint width;
  guint height;
  float fps;

  guint n_slots;

  EGLint usage;
  EGLint hal_format;
  EGLint stride_pixels;

  EGLClientBuffer *buffers;

  int *slot_num_ints;
  int *slot_num_fds;
  int **slot_ints;
  int **slot_fds;

  guint seq;
  guint last_published_slot;

  guint cur_slot;
  gboolean slots_initialized;

  MetaStageWatch *paint_watch;
  gboolean watch_ready;

  GObject *watch_self_ref;

  guint stage_kick_idle_id;

  guint pending_requests;

  EGLDisplay egl_display;
  gboolean egl_gl_ready;

  PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
  PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

  EGLImageKHR *slot_images;
  GLuint *slot_textures;
  GLuint *slot_fbos;

  CoglContext *cogl_context;
  CoglTexture **slot_cogl_textures;
  CoglOffscreen **slot_cogl_offscreens;
  CoglFramebuffer **slot_cogl_fbs;
  CoglPixelFormat *slot_cogl_formats;

  PFNEGLHYBRISCREATENATIVEBUFFERPROC eglHybrisCreateNativeBuffer;
  PFNEGLHYBRISRELEASENATIVEBUFFERPROC eglHybrisReleaseNativeBuffer;
  PFNEGLHYBRISGETNATIVEBUFFERINFOPROC eglHybrisGetNativeBufferInfo;
  PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC eglHybrisSerializeNativeBuffer;
};

G_DEFINE_TYPE (MetaFuriosScreenCastStreamSrcNativeBuffer,
               meta_furios_screen_cast_stream_src_native_buffer,
               G_TYPE_OBJECT)

static void
ensure_cogl_context (MetaFuriosScreenCastStreamSrcNativeBuffer *self)
{
  if (self->cogl_context)
    return;

  if (self->backend) {
    ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (self->backend);
    if (clutter_backend)
      self->cogl_context = clutter_backend_get_cogl_context (clutter_backend);
  }

  if (!self->cogl_context) {
    ClutterBackend *cb = clutter_get_default_backend ();
    if (cb)
      self->cogl_context = clutter_backend_get_cogl_context (cb);
  }
}

static gboolean
load_entrypoints (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                  GError                                   **error)
{
  self->eglHybrisCreateNativeBuffer = (PFNEGLHYBRISCREATENATIVEBUFFERPROC) eglGetProcAddress ("eglHybrisCreateNativeBuffer");
  self->eglHybrisReleaseNativeBuffer = (PFNEGLHYBRISRELEASENATIVEBUFFERPROC) eglGetProcAddress ("eglHybrisReleaseNativeBuffer");
  self->eglHybrisGetNativeBufferInfo = (PFNEGLHYBRISGETNATIVEBUFFERINFOPROC) eglGetProcAddress ("eglHybrisGetNativeBufferInfo");
  self->eglHybrisSerializeNativeBuffer = (PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC) eglGetProcAddress ("eglHybrisSerializeNativeBuffer");

  if (!self->eglHybrisCreateNativeBuffer ||
      !self->eglHybrisReleaseNativeBuffer ||
      !self->eglHybrisGetNativeBufferInfo ||
      !self->eglHybrisSerializeNativeBuffer) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_NOT_SUPPORTED,
                 "Missing hybris native-buffer entrypoints (need EGL_HYBRIS_native_buffer2): "
                 "eglHybrisCreateNativeBuffer=%p eglHybrisReleaseNativeBuffer=%p "
                 "eglHybrisGetNativeBufferInfo=%p eglHybrisSerializeNativeBuffer=%p",
                 self->eglHybrisCreateNativeBuffer,
                 self->eglHybrisReleaseNativeBuffer,
                 self->eglHybrisGetNativeBufferInfo,
                 self->eglHybrisSerializeNativeBuffer);
    return FALSE;
  }

  return TRUE;
}

static void
free_slot_data (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                guint                                      i)
{
  if (self->slot_ints && self->slot_ints[i]) {
    g_free (self->slot_ints[i]);
    self->slot_ints[i] = NULL;
  }

  if (self->slot_fds && self->slot_fds[i]) {
    for (int k = 0; k < self->slot_num_fds[i]; k++) {
      if (self->slot_fds[i][k] >= 0)
        close (self->slot_fds[i][k]);
    }
    g_free (self->slot_fds[i]);
    self->slot_fds[i] = NULL;
  }

  if (self->slot_num_ints)
    self->slot_num_ints[i] = 0;
  if (self->slot_num_fds)
    self->slot_num_fds[i] = 0;
}

static gboolean
alloc_and_serialize_slot (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                          guint                                      i,
                          GError                                   **error)
{
  EGLClientBuffer buf = (EGLClientBuffer) 0;
  EGLint stride = 0;

  if (!self->eglHybrisCreateNativeBuffer ((EGLint) self->width,
                                          (EGLint) self->height,
                                          self->usage,
                                          self->hal_format,
                                          &stride,
                                          &buf) || !buf) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_FAILED,
                 "eglHybrisCreateNativeBuffer failed for slot %u",
                 i);
    return FALSE;
  }

  if (self->stride_pixels == 0)
    self->stride_pixels = stride;

  self->buffers[i] = buf;

  int num_ints = 0;
  int num_fds = 0;
  if (!self->eglHybrisGetNativeBufferInfo (buf, &num_ints, &num_fds) || num_ints <= 0 || num_fds <= 0) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_FAILED,
                 "eglHybrisGetNativeBufferInfo failed for slot %u (ints=%d fds=%d)",
                 i, num_ints, num_fds);
    return FALSE;
  }

  self->slot_num_ints[i] = num_ints;
  self->slot_num_fds[i] = num_fds;

  self->slot_ints[i] = g_new0 (int, num_ints);
  self->slot_fds[i] = g_new0 (int, num_fds);

  if (!self->eglHybrisSerializeNativeBuffer (buf, self->slot_ints[i], self->slot_fds[i])) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_FAILED,
                 "eglHybrisSerializeNativeBuffer failed for slot %u",
                 i);
    return FALSE;
  }

  return TRUE;
}

static gboolean
init_ring (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
           GError                                   **error)
{
  self->buffers = g_new0 (EGLClientBuffer, self->n_slots);

  self->slot_num_ints = g_new0 (int, self->n_slots);
  self->slot_num_fds = g_new0 (int, self->n_slots);
  self->slot_ints = g_new0 (int*, self->n_slots);
  self->slot_fds = g_new0 (int*, self->n_slots);

  self->slot_images = g_new0 (EGLImageKHR, self->n_slots);
  self->slot_textures = g_new0 (GLuint, self->n_slots);
  self->slot_fbos = g_new0 (GLuint, self->n_slots);

  self->slot_cogl_textures = g_new0 (CoglTexture*, self->n_slots);
  self->slot_cogl_offscreens = g_new0 (CoglOffscreen*, self->n_slots);
  self->slot_cogl_fbs = g_new0 (CoglFramebuffer*, self->n_slots);
  self->slot_cogl_formats = g_new0 (CoglPixelFormat, self->n_slots);

  for (guint i = 0; i < self->n_slots; i++) {
    self->slot_images[i] = EGL_NO_IMAGE_KHR;
    self->slot_cogl_formats[i] = COGL_PIXEL_FORMAT_ANY;
    if (!alloc_and_serialize_slot (self, i, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
stage_is_ready_for_kick (ClutterActor *stage_actor)
{
  if (!stage_actor)
    return FALSE;
  if (!clutter_actor_is_realized (stage_actor))
    return FALSE;
  return TRUE;
}

static void
kick_stage_update (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                   ClutterActor                              *actor)
{
  if (!actor)
    return;

  if (!clutter_actor_has_allocation (actor))
    clutter_actor_queue_relayout (actor);

  clutter_actor_queue_redraw (actor);
  clutter_stage_schedule_update (CLUTTER_STAGE (actor));
}

static ClutterStageView *
get_any_stage_view (MetaFuriosScreenCastStreamSrcNativeBuffer *self)
{
  ClutterActor *actor;
  ClutterStage *stage;
  ClutterStageView *view;

  actor = meta_backend_get_stage (self->backend);
  if (!actor)
    return NULL;

  stage = CLUTTER_STAGE (actor);

  view = clutter_stage_get_view_at (stage, 0.f, 0.f);
  if (view)
    return view;

  view = clutter_stage_get_view_at (stage, 1.f, 1.f);
  if (view)
    return view;

  view = clutter_stage_get_view_at (stage, 16.f, 16.f);
  if (view)
    return view;

  return NULL;
}

static gboolean
ensure_egl_gl (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
               GError                                  **error)
{
  if (self->egl_gl_ready)
    return TRUE;

  self->egl_display = eglGetCurrentDisplay ();
  if (self->egl_display == EGL_NO_DISPLAY) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No current EGLDisplay (eglGetCurrentDisplay returned EGL_NO_DISPLAY)");
    return FALSE;
  }

  self->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress ("eglCreateImageKHR");
  self->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress ("eglDestroyImageKHR");
  self->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress ("glEGLImageTargetTexture2DOES");

  if (!self->eglCreateImageKHR || !self->eglDestroyImageKHR || !self->glEGLImageTargetTexture2DOES) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Missing required EGL/GL entrypoints: eglCreateImageKHR=%p eglDestroyImageKHR=%p glEGLImageTargetTexture2DOES=%p",
                 self->eglCreateImageKHR, self->eglDestroyImageKHR, self->glEGLImageTargetTexture2DOES);
    return FALSE;
  }

  self->egl_gl_ready = TRUE;
  return TRUE;
}

static void
destroy_slot_cogl_objects (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                           guint                                      slot)
{
  if (self->slot_cogl_offscreens && self->slot_cogl_offscreens[slot]) {
    g_object_unref (self->slot_cogl_offscreens[slot]);
    self->slot_cogl_offscreens[slot] = NULL;
    self->slot_cogl_fbs[slot] = NULL;
  }

  if (self->slot_cogl_textures && self->slot_cogl_textures[slot]) {
    g_object_unref (self->slot_cogl_textures[slot]);
    self->slot_cogl_textures[slot] = NULL;
  }

  if (self->slot_cogl_formats)
    self->slot_cogl_formats[slot] = COGL_PIXEL_FORMAT_ANY;
}

static gboolean
ensure_slot_gl_objects (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                        guint                                      slot,
                        GError                                   **error)
{
  if (slot >= self->n_slots) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid slot %u", slot);
    return FALSE;
  }

  if (!ensure_egl_gl (self, error))
    return FALSE;

  if (self->slot_images[slot] != EGL_NO_IMAGE_KHR &&
      self->slot_textures[slot] != 0 &&
      self->slot_fbos[slot] != 0)
    return TRUE;

  EGLImageKHR img = self->eglCreateImageKHR (self->egl_display,
                                             EGL_NO_CONTEXT,
                                             EGL_NATIVE_BUFFER_ANDROID,
                                             self->buffers[slot],
                                             (const EGLint[]) {
                                               EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                                               EGL_NONE
                                             });
  if (img == EGL_NO_IMAGE_KHR) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) failed for slot %u (eglGetError=0x%04x)",
                 slot, eglGetError ());
    return FALSE;
  }

  GLuint tex = 0;
  glGenTextures (1, &tex);
  glBindTexture (GL_TEXTURE_2D, tex);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  self->glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, (GLeglImageOES) img);

  GLuint fbo = 0;
  glGenFramebuffers (1, &fbo);
  glBindFramebuffer (GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

  GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
  glBindFramebuffer (GL_FRAMEBUFFER, 0);
  glBindTexture (GL_TEXTURE_2D, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &tex);
    self->eglDestroyImageKHR (self->egl_display, img);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Destination FBO incomplete for slot %u (0x%04x)",
                 slot, (unsigned) status);
    return FALSE;
  }

  self->slot_images[slot] = img;
  self->slot_textures[slot] = tex;
  self->slot_fbos[slot] = fbo;

  destroy_slot_cogl_objects (self, slot);

  return TRUE;
}

static gboolean
ensure_slot_cogl_framebuffer (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                              guint                                      slot,
                              CoglPixelFormat                            desired_format,
                              GError                                   **error)
{
  if (slot >= self->n_slots) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid slot %u", slot);
    return FALSE;
  }

  ensure_cogl_context (self);
  if (!self->cogl_context) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No CoglContext available");
    return FALSE;
  }

  if (!ensure_slot_gl_objects (self, slot, error))
    return FALSE;

  /* if we already have a dst fb with the right premult bit, reuse it */
  if (self->slot_cogl_fbs[slot] && self->slot_cogl_formats[slot] == desired_format)
    return TRUE;

  /* otherwise recreate Cogl wrappers for this slot */
  destroy_slot_cogl_objects (self, slot);

  g_autoptr (GError) local_error = NULL;

  CoglTexture *cogl_tex = cogl_texture_2d_new_from_egl_image (self->cogl_context,
                                                              (int) self->width,
                                                              (int) self->height,
                                                              desired_format,
                                                              self->slot_images[slot],
                                                              COGL_EGL_IMAGE_FLAG_NONE,
                                                              &local_error);

  if (!cogl_tex) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "cogl_texture_2d_new_from_egl_image failed for slot %u: %s",
                 slot, local_error ? local_error->message : "unknown");
    return FALSE;
  }

  CoglOffscreen *offscreen = cogl_offscreen_new_with_texture (cogl_tex);
  if (!offscreen) {
    g_object_unref (cogl_tex);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "cogl_offscreen_new_with_texture failed for slot %u",
                 slot);
    return FALSE;
  }

  CoglFramebuffer *dst_fb = COGL_FRAMEBUFFER (offscreen);

  if (!cogl_framebuffer_allocate (dst_fb, &local_error)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "cogl_framebuffer_allocate failed for slot %u: %s",
                 slot, local_error ? local_error->message : "unknown");
    g_object_unref (offscreen);
    g_object_unref (cogl_tex);
    return FALSE;
  }

  self->slot_cogl_textures[slot] = cogl_tex;
  self->slot_cogl_offscreens[slot] = offscreen;
  self->slot_cogl_fbs[slot] = dst_fb;
  self->slot_cogl_formats[slot] = desired_format;

  return TRUE;
}

static gboolean
blit_stage_view_into_slot (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                           ClutterStageView                          *view,
                           guint                                      slot,
                           GError                                   **error)
{
  if (!view) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No stage view");
    return FALSE;
  }

  CoglFramebuffer *src_fb = clutter_stage_view_get_framebuffer (view);
  if (!src_fb) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No stage view framebuffer");
    return FALSE;
  }

  const int width = (int) self->width;
  const int height = (int) self->height;

  if (width <= 0 || height <= 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid capture size");
    return FALSE;
  }

  if (!ensure_slot_cogl_framebuffer (self, slot, COGL_PIXEL_FORMAT_RGBX_8888, error))
    return FALSE;

  CoglFramebuffer *dst_fb = self->slot_cogl_fbs[slot];
  if (!dst_fb) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No destination framebuffer");
    return FALSE;
  }

  g_autoptr (GError) local_error = NULL;
  if (!cogl_framebuffer_blit (src_fb,
                              dst_fb,
                              0, 0,
                              0, 0,
                              width, height,
                              &local_error)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "cogl_framebuffer_blit failed: %s",
                 local_error ? local_error->message : "unknown");
    return FALSE;
  }

  return TRUE;
}

static gboolean
copy_stage_view_into_slot (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                           ClutterStageView                          *view,
                           guint                                      slot,
                           GError                                   **error)
{
  if (!blit_stage_view_into_slot (self, view, slot, error))
    return FALSE;

  glFinish ();

  return TRUE;
}

static void
publish_slot (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
              guint                                      slot)
{
  self->seq++;
  self->last_published_slot = slot;

  g_signal_emit (self, signals[SIGNAL_FRAME_PUBLISHED], 0, self->seq, slot);
}

static gboolean
initialize_slots_from_stage_view (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                                  ClutterStageView                          *view,
                                  GError                                   **error)
{
  if (!self || !view)
    return FALSE;

  for (guint slot = 0; slot < self->n_slots; slot++) {
    if (!blit_stage_view_into_slot (self, view, slot, error))
      return FALSE;
  }

  glFinish ();

  self->slots_initialized = TRUE;

  return TRUE;
}

static void
on_after_paint (MetaStage        *stage,
                ClutterStageView *view,
                const MtkRegion  *redraw_clip,
                ClutterFrame     *frame,
                gpointer          user_data)
{
  (void) stage;
  (void) redraw_clip;
  (void) frame;

  MetaFuriosScreenCastStreamSrcNativeBuffer *self = user_data;

  if (!self)
    return;

  if (!self->pending_requests)
    return;

  GError *local_error = NULL;

  if (!self->slots_initialized) {
    if (!initialize_slots_from_stage_view (self, view, &local_error)) {
      if (local_error) {
        g_warning ("native-buffer screencast: initial capture failed: %s", local_error->message);
        g_clear_error (&local_error);
      }

      /* keep pending_requests set so a future paint can satisfy it */
      return;
    }

    self->cur_slot = self->n_slots > 1 ? 1 : 0;

    publish_slot (self, 0);

    self->pending_requests = 0;

    return;
  }

  guint slot = self->cur_slot;
  self->cur_slot = (self->cur_slot + 1) % self->n_slots;

  if (!copy_stage_view_into_slot (self, view, slot, &local_error)) {
    if (local_error) {
      g_warning ("native-buffer screencast: capture failed: %s", local_error->message);
      g_clear_error (&local_error);
    }

    /* keep pending_requests set so a future paint can satisfy it */
    return;
  }

  publish_slot (self, slot);

  /* satisfy exactly one request. coalesce multiple requests into 1 capture */
  self->pending_requests = 0;
}

static void
ensure_stage_watch (MetaFuriosScreenCastStreamSrcNativeBuffer *self)
{
  MetaStage *meta_stage;
  ClutterStageView *view;

  if (self->watch_ready)
    return;
  if (!self->backend)
    return;

  ClutterActor *actor = meta_backend_get_stage (self->backend);
  if (!actor || !clutter_actor_is_realized (actor))
    return;

  meta_stage = META_STAGE (actor);
  if (!meta_stage)
    return;

  view = get_any_stage_view (self);
  if (!view)
    return;

  if (!self->watch_self_ref)
    self->watch_self_ref = G_OBJECT (g_object_ref (self));

  self->paint_watch = meta_stage_watch_view (meta_stage,
                                             view,
                                             META_STAGE_WATCH_AFTER_PAINT,
                                             on_after_paint,
                                             self);

  self->watch_ready = TRUE;

  ensure_cogl_context (self);
}

static gboolean
stage_kick_idle_cb (gpointer user_data)
{
  MetaFuriosScreenCastStreamSrcNativeBuffer *self = user_data;

  if (!self)
    return G_SOURCE_REMOVE;

  if (!self->backend) {
    self->stage_kick_idle_id = 0;
    return G_SOURCE_REMOVE;
  }

  if (self->slots_initialized || !self->pending_requests) {
    self->stage_kick_idle_id = 0;
    return G_SOURCE_REMOVE;
  }

  ClutterActor *actor = meta_backend_get_stage (self->backend);
  if (!stage_is_ready_for_kick (actor))
    return G_SOURCE_CONTINUE;

  ensure_stage_watch (self);

  kick_stage_update (self, actor);

  self->stage_kick_idle_id = 0;
  return G_SOURCE_REMOVE;
}

static void
ensure_stage_kick_scheduled (MetaFuriosScreenCastStreamSrcNativeBuffer *self)
{
  if (self->stage_kick_idle_id != 0)
    return;

  self->stage_kick_idle_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                              stage_kick_idle_cb,
                                              g_object_ref (self),
                                              (GDestroyNotify) g_object_unref);
}

static void
destroy_slot_gl_objects (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                         guint                                      i)
{
  destroy_slot_cogl_objects (self, i);

  if (self->slot_fbos && self->slot_fbos[i]) {
    glDeleteFramebuffers (1, &self->slot_fbos[i]);
    self->slot_fbos[i] = 0;
  }

  if (self->slot_textures && self->slot_textures[i]) {
    glDeleteTextures (1, &self->slot_textures[i]);
    self->slot_textures[i] = 0;
  }

  if (self->slot_images && self->slot_images[i] != EGL_NO_IMAGE_KHR) {
    if (self->egl_gl_ready && self->eglDestroyImageKHR && self->egl_display != EGL_NO_DISPLAY)
      self->eglDestroyImageKHR (self->egl_display, self->slot_images[i]);
    self->slot_images[i] = EGL_NO_IMAGE_KHR;
  }
}

static void
meta_furios_screen_cast_stream_src_native_buffer_dispose (GObject *object)
{
  MetaFuriosScreenCastStreamSrcNativeBuffer *self = META_FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER (object);

  if (self->paint_watch) {
    ClutterActor *actor = meta_backend_get_stage (self->backend);
    MetaStage *meta_stage = actor ? META_STAGE (actor) : NULL;
    if (meta_stage)
      meta_stage_remove_watch (meta_stage, self->paint_watch);

    self->paint_watch = NULL;
    self->watch_ready = FALSE;
  }

  g_clear_object (&self->watch_self_ref);

  if (self->stage_kick_idle_id) {
    g_source_remove (self->stage_kick_idle_id);
    self->stage_kick_idle_id = 0;
  }

  if (self->slot_images || self->slot_textures || self->slot_fbos) {
    for (guint i = 0; i < self->n_slots; i++)
      destroy_slot_gl_objects (self, i);
  }

  g_clear_pointer (&self->slot_images, g_free);
  g_clear_pointer (&self->slot_textures, g_free);
  g_clear_pointer (&self->slot_fbos, g_free);

  g_clear_pointer (&self->slot_cogl_textures, g_free);
  g_clear_pointer (&self->slot_cogl_offscreens, g_free);
  g_clear_pointer (&self->slot_cogl_fbs, g_free);
  g_clear_pointer (&self->slot_cogl_formats, g_free);

  if (self->buffers) {
    for (guint i = 0; i < self->n_slots; i++) {
      free_slot_data (self, i);

      if (self->buffers[i] && self->eglHybrisReleaseNativeBuffer)
        self->eglHybrisReleaseNativeBuffer (self->buffers[i]);
      self->buffers[i] = (EGLClientBuffer) 0;
    }
  }

  g_clear_pointer (&self->buffers, g_free);
  g_clear_pointer (&self->slot_num_ints, g_free);
  g_clear_pointer (&self->slot_num_fds, g_free);
  g_clear_pointer (&self->slot_ints, g_free);
  g_clear_pointer (&self->slot_fds, g_free);

  g_clear_object (&self->backend);

  G_OBJECT_CLASS (meta_furios_screen_cast_stream_src_native_buffer_parent_class)->dispose (object);
}

static void
meta_furios_screen_cast_stream_src_native_buffer_class_init (MetaFuriosScreenCastStreamSrcNativeBufferClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_stream_src_native_buffer_dispose;

  signals[SIGNAL_FRAME_PUBLISHED] = g_signal_new ("frame-published",
                                                  G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL, NULL,
                                                  NULL,
                                                  G_TYPE_NONE,
                                                  2,
                                                  G_TYPE_UINT,
                                                  G_TYPE_UINT);
}

static void
meta_furios_screen_cast_stream_src_native_buffer_init (MetaFuriosScreenCastStreamSrcNativeBuffer *self)
{
  self->backend = NULL;

  self->width = 0;
  self->height = 0;
  self->fps = 0.0f;

  self->n_slots = 3;

  self->usage = GRALLOC_USAGE_HW_TEXTURE |
                GRALLOC_USAGE_HW_RENDER  |
                GRALLOC_USAGE_HW_FB;

  self->hal_format = HAL_PIXEL_FORMAT_RGBX_8888;
  self->stride_pixels = 0;

  self->buffers = NULL;

  self->slot_num_ints = NULL;
  self->slot_num_fds = NULL;
  self->slot_ints = NULL;
  self->slot_fds = NULL;

  self->seq = 0;
  self->last_published_slot = 0;
  self->cur_slot = 0;
  self->slots_initialized = FALSE;

  self->paint_watch = NULL;
  self->watch_ready = FALSE;
  self->watch_self_ref = NULL;

  self->stage_kick_idle_id = 0;
  self->pending_requests = 0;

  self->egl_display = EGL_NO_DISPLAY;
  self->egl_gl_ready = FALSE;
  self->eglCreateImageKHR = NULL;
  self->eglDestroyImageKHR = NULL;
  self->glEGLImageTargetTexture2DOES = NULL;

  self->slot_images = NULL;
  self->slot_textures = NULL;
  self->slot_fbos = NULL;

  self->cogl_context = NULL;
  self->slot_cogl_textures = NULL;
  self->slot_cogl_offscreens = NULL;
  self->slot_cogl_fbs = NULL;
  self->slot_cogl_formats = NULL;

  self->eglHybrisCreateNativeBuffer = NULL;
  self->eglHybrisReleaseNativeBuffer = NULL;
  self->eglHybrisGetNativeBufferInfo = NULL;
  self->eglHybrisSerializeNativeBuffer = NULL;
}

MetaFuriosScreenCastStreamSrcNativeBuffer *
meta_furios_screen_cast_stream_src_native_buffer_new (MetaBackend  *backend,
                                                      guint         width,
                                                      guint         height,
                                                      float         fps,
                                                      GError      **error)
{
  MetaFuriosScreenCastStreamSrcNativeBuffer *self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER, NULL);

  self->backend = backend ? g_object_ref (backend) : NULL;
  self->width = width;
  self->height = height;
  self->fps = fps;

  if (!load_entrypoints (self, error)) {
    g_object_unref (self);
    return NULL;
  }

  if (!init_ring (self, error)) {
    g_object_unref (self);
    return NULL;
  }

  ensure_stage_watch (self);

  return self;
}

void
meta_furios_screen_cast_stream_src_native_buffer_request_frame_async (MetaFuriosScreenCastStreamSrcNativeBuffer *self)
{
  g_return_if_fail (META_IS_FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER (self));

  ensure_stage_watch (self);

  /* coalesce multiple requests. only need 1 pending capture */
  self->pending_requests = 1;

  if (self->slots_initialized)
    return;

  ClutterActor *actor = meta_backend_get_stage (self->backend);

  if (stage_is_ready_for_kick (actor)) {
    kick_stage_update (self, actor);
    return;
  }

  ensure_stage_kick_scheduled (self);
}

static GVariant *
variant_new_int_array (const int *data, int n)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("ai"));
  for (int i = 0; i < n; i++)
    g_variant_builder_add (&b, "i", data[i]);
  return g_variant_builder_end (&b);
}

static GVariant *
variant_new_handle_array (const int *fd_indices, int n)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("ah"));
  for (int i = 0; i < n; i++)
    g_variant_builder_add (&b, "h", fd_indices[i]);
  return g_variant_builder_end (&b);
}

gboolean
meta_furios_screen_cast_stream_src_native_buffer_get_handle_info (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                                                                  GUnixFDList                               *fd_list,
                                                                  GVariant                                 **out_info,
                                                                  GError                                   **error)
{
  g_return_val_if_fail (META_IS_FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER (self), FALSE);
  g_return_val_if_fail (G_IS_UNIX_FD_LIST (fd_list), FALSE);
  g_return_val_if_fail (out_info != NULL, FALSE);

  GVariantBuilder top;
  g_variant_builder_init (&top, G_VARIANT_TYPE_VARDICT);

  GVariantBuilder buf_arr;
  g_variant_builder_init (&buf_arr, G_VARIANT_TYPE ("aa{sv}"));

  for (guint s = 0; s < self->n_slots; s++) {
    const int num_ints = self->slot_num_ints[s];
    const int num_fds = self->slot_num_fds[s];

    if (num_ints <= 0 || num_fds <= 0 || !self->slot_ints[s] || !self->slot_fds[s]) {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "slot %u has no serialized data (ints=%d fds=%d)",
                   s, num_ints, num_fds);
      return FALSE;
    }

    int *fd_indices = g_new0 (int, num_fds);
    for (int i = 0; i < num_fds; i++) {
      g_autoptr (GError) local_error = NULL;
      int idx = g_unix_fd_list_append (fd_list, self->slot_fds[s][i], &local_error);
      if (idx < 0) {
        g_set_error (error,
                     G_IO_ERROR,
                     G_IO_ERROR_FAILED,
                     "Failed to append fd for slot %u: %s",
                     s, local_error ? local_error->message : "unknown");
        g_free (fd_indices);
        return FALSE;
      }
      fd_indices[i] = idx;
    }

    GVariantBuilder one;
    g_variant_builder_init (&one, G_VARIANT_TYPE_VARDICT);

    g_variant_builder_add (&one, "{sv}", "slot", g_variant_new_uint32 (s));
    g_variant_builder_add (&one, "{sv}", "width", g_variant_new_uint32 (self->width));
    g_variant_builder_add (&one, "{sv}", "height", g_variant_new_uint32 (self->height));
    g_variant_builder_add (&one, "{sv}", "stride_pixels", g_variant_new_uint32 ((guint) self->stride_pixels));
    g_variant_builder_add (&one, "{sv}", "hal_format", g_variant_new_int32 ((gint32) self->hal_format));
    g_variant_builder_add (&one, "{sv}", "usage", g_variant_new_uint32 ((guint32) self->usage));

    g_variant_builder_add (&one, "{sv}", "num_ints", g_variant_new_int32 (num_ints));
    g_variant_builder_add (&one, "{sv}", "num_fds", g_variant_new_int32 (num_fds));
    g_variant_builder_add (&one, "{sv}", "ints", variant_new_int_array (self->slot_ints[s], num_ints));
    g_variant_builder_add (&one, "{sv}", "fds", variant_new_handle_array (fd_indices, num_fds));

    g_free (fd_indices);

    g_variant_builder_add (&buf_arr, "@a{sv}", g_variant_builder_end (&one));
  }

  g_variant_builder_add (&top, "{sv}", "type", g_variant_new_string ("native-buffer"));
  g_variant_builder_add (&top, "{sv}", "buffer_count", g_variant_new_uint32 (self->n_slots));
  g_variant_builder_add (&top, "{sv}", "width", g_variant_new_uint32 (self->width));
  g_variant_builder_add (&top, "{sv}", "height", g_variant_new_uint32 (self->height));
  g_variant_builder_add (&top, "{sv}", "stride_pixels", g_variant_new_uint32 ((guint) self->stride_pixels));
  g_variant_builder_add (&top, "{sv}", "hal_format", g_variant_new_int32 ((gint32) self->hal_format));
  g_variant_builder_add (&top, "{sv}", "usage", g_variant_new_uint32 ((guint32) self->usage));
  g_variant_builder_add (&top, "{sv}", "buffers", g_variant_builder_end (&buf_arr));

  *out_info = g_variant_builder_end (&top);
  return TRUE;
}

void
meta_furios_screen_cast_stream_src_native_buffer_add_info (MetaFuriosScreenCastStreamSrcNativeBuffer *self,
                                                           GVariantBuilder                           *builder)
{
  g_return_if_fail (META_IS_FURIOS_SCREEN_CAST_STREAM_SRC_NATIVE_BUFFER (self));
  g_return_if_fail (builder != NULL);

  g_variant_builder_add (builder, "{sv}", "buffer_count", g_variant_new_uint32 (self->n_slots));
  g_variant_builder_add (builder, "{sv}", "stride_pixels", g_variant_new_uint32 ((guint) self->stride_pixels));
  g_variant_builder_add (builder, "{sv}", "hal_format", g_variant_new_int32 ((gint32) self->hal_format));
  g_variant_builder_add (builder, "{sv}", "usage", g_variant_new_uint32 ((guint32) self->usage));
}
