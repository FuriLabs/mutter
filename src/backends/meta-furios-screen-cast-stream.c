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

#include "backends/meta-furios-screen-cast-stream.h"
#include "backends/meta-furios-screen-cast-session.h"
#include "backends/meta-furios-screen-cast-stream-eis.h"

#include "backends/meta-furios-screen-cast-stream-src-memfd.h"

#ifdef HAVE_FURIOS_NATIVE_BUFFER
#include "backends/meta-furios-screen-cast-stream-src-native-buffer.h"
#endif

typedef enum
{
  META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD = 0,
#ifdef HAVE_FURIOS_NATIVE_BUFFER
  META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER = 1,
#endif
} MetaFuriosScreenCastStreamBackendType;

typedef enum
{
  META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_UNSET = 0,
  META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_MEMFD,
  META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_NATIVE_BUFFER,
} MetaFuriosScreenCastStreamBackendForce;

struct _MetaFuriosScreenCastStream
{
  MetaDBusMutterScreenCastStreamSkeleton parent_instance;

  MetaFuriosScreenCastSession *session;
  GDBusConnection *connection;
  char *peer_name;
  char *object_path;
  char *mapping_id;

  MetaFuriosScreenCastStreamBackendType backend_type;

  MetaFuriosScreenCastStreamSrcMemfd *src_memfd;
#ifdef HAVE_FURIOS_NATIVE_BUFFER
  MetaFuriosScreenCastStreamSrcNativeBuffer *src_native_buffer;
#endif

  MetaFuriosScreenCastStreamEis *eis;

  guint32 last_emitted_seq;

  guint width;
  guint height;
  float fps;
};

static void
meta_furios_screen_cast_stream_init_iface (MetaDBusMutterScreenCastStreamIface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaFuriosScreenCastStream,
                         meta_furios_screen_cast_stream,
                         META_DBUS_TYPE_MUTTER_SCREEN_CAST_STREAM_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_MUTTER_SCREEN_CAST_STREAM,
                                                meta_furios_screen_cast_stream_init_iface))

static gboolean
check_permission (MetaFuriosScreenCastStream *self,
                  GDBusMethodInvocation      *invocation)
{
  return g_strcmp0 (self->peer_name,
                    g_dbus_method_invocation_get_sender (invocation)) == 0;
}

const char *
meta_furios_screen_cast_stream_get_object_path (MetaFuriosScreenCastStream *stream)
{
  return stream->object_path;
}

static gboolean
handle_get_memfd (MetaDBusMutterScreenCastStream *skeleton,
                  GDBusMethodInvocation          *invocation)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  if (self->backend_type != META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD || !self->src_memfd) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "GetMemfd not available for this stream type");
    return TRUE;
  }

  int fd = meta_furios_screen_cast_stream_src_memfd_dup_fd (self->src_memfd);
  if (fd < 0) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "No memfd");
    return TRUE;
  }

  g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();
  g_autoptr (GError) error = NULL;

  int idx = g_unix_fd_list_append (fd_list, fd, &error);
  close (fd);

  if (idx < 0) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "%s", error->message);
    return TRUE;
  }

  GVariant *out = g_variant_new ("(h)", idx);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           out,
                                                           fd_list);
  return TRUE;
}

static gboolean
handle_get_native_buffer_handle (MetaDBusMutterScreenCastStream *skeleton,
                                 GDBusMethodInvocation          *invocation)
{
#ifndef HAVE_FURIOS_NATIVE_BUFFER
  (void) skeleton;
  (void) invocation;
  return TRUE;
#else
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  if (self->backend_type != META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER ||
      !self->src_native_buffer) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "GetNativeBufferHandle not available for this stream type");
    return TRUE;
  }

  g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) info = NULL;

  if (!meta_furios_screen_cast_stream_src_native_buffer_get_handle_info (self->src_native_buffer,
                                                                         fd_list,
                                                                         &info,
                                                                         &error)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "%s", error ? error->message : "Failed to build native buffer handle");
    return TRUE;
  }

  GVariant *out = g_variant_new ("(@a{sv})", g_steal_pointer (&info));

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           out,
                                                           g_steal_pointer (&fd_list));
  return TRUE;
#endif
}

#ifdef HAVE_FURIOS_NATIVE_BUFFER
static gboolean
handle_get_fence (MetaDBusMutterScreenCastStream *skeleton,
                  GDBusMethodInvocation          *invocation,
                  guint                           slot)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  if (self->backend_type != META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER ||
      !self->src_native_buffer) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "GetFence not available for this stream type");
    return TRUE;
  }

  int fence_fd = meta_furios_screen_cast_stream_src_native_buffer_dup_fence_fd (self->src_native_buffer,
                                                                                (guint) slot);
  if (fence_fd < 0) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "No fence for slot %u",
                                           slot);
    return TRUE;
  }

  g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();
  g_autoptr (GError) error = NULL;

  int idx = g_unix_fd_list_append (fd_list, fence_fd, &error);
  close (fence_fd);

  if (idx < 0) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "%s", error ? error->message : "Failed to append fence fd");
    return TRUE;
  }

  GVariant *out = g_variant_new ("(h)", idx);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           out,
                                                           fd_list);
  return TRUE;
}
#endif

static gboolean
handle_get_eis_fd (MetaDBusMutterScreenCastStream *skeleton,
                   GDBusMethodInvocation          *invocation)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  if (!self->eis) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "No EIS");
    return TRUE;
  }

  int fd = meta_furios_screen_cast_stream_eis_add_client_get_fd (self->eis);
  if (fd < 0) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "No EIS fd");
    return TRUE;
  }

  g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();
  g_autoptr (GError) error = NULL;

  int idx = g_unix_fd_list_append (fd_list, fd, &error);
  close (fd);

  if (idx < 0) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "%s", error->message);
    return TRUE;
  }

  GVariant *out = g_variant_new ("(h)", idx);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
                                                           out,
                                                           fd_list);
  return TRUE;
}

#ifdef HAVE_FURIOS_NATIVE_BUFFER
static void
on_native_buffer_frame_published (MetaFuriosScreenCastStreamSrcNativeBuffer *src,
                                  guint                                      seq,
                                  guint                                      slot,
                                  gpointer                                   user_data)
{
  (void) src;

  MetaFuriosScreenCastStream *self = user_data;
  if (!self)
    return;

  if (self->backend_type != META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER)
    return;

  /* avoid duplicates */
  if ((guint32) seq == self->last_emitted_seq)
    return;

  self->last_emitted_seq = (guint32) seq;

  meta_dbus_mutter_screen_cast_stream_emit_frame_ready (META_DBUS_MUTTER_SCREEN_CAST_STREAM (self),
                                                        (guint32) seq,
                                                        (guint32) slot);
}
#endif

static gboolean
handle_request_frame (MetaDBusMutterScreenCastStream *skeleton,
                      GDBusMethodInvocation          *invocation)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  guint seq = 0;
  guint slot = 0;
  g_autoptr (GError) error = NULL;

  if (self->backend_type == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD) {
    if (!self->src_memfd) {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "No memfd backend");
      return TRUE;
    }

    if (!meta_furios_screen_cast_stream_src_memfd_request_frame (self->src_memfd,
                                                                 &seq,
                                                                 &slot,
                                                                 &error)) {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "%s", error->message);
      return TRUE;
    }

    /* only emit when we actually produced a new frame */
    if (seq != self->last_emitted_seq) {
      self->last_emitted_seq = seq;

      gboolean should_emit_damage = FALSE;
      g_autoptr (GVariant) damage = NULL;
      gboolean has_damage_info;

      has_damage_info = meta_furios_screen_cast_stream_src_memfd_get_last_damage (self->src_memfd,
                                                                                  &should_emit_damage,
                                                                                  &damage);
      if (has_damage_info && should_emit_damage && damage != NULL)
        meta_dbus_mutter_screen_cast_stream_emit_frame_ready_with_damage (META_DBUS_MUTTER_SCREEN_CAST_STREAM (self),
                                                                          seq,
                                                                          slot,
                                                                          damage);
      else
        meta_dbus_mutter_screen_cast_stream_emit_frame_ready (META_DBUS_MUTTER_SCREEN_CAST_STREAM (self),
                                                              seq,
                                                              slot);
    }

    meta_dbus_mutter_screen_cast_stream_complete_request_frame (skeleton,
                                                                invocation);
    return TRUE;
  }

#ifdef HAVE_FURIOS_NATIVE_BUFFER
  if (self->backend_type == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER) {
    if (!self->src_native_buffer) {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "No native-buffer backend");
      return TRUE;
    }

    meta_furios_screen_cast_stream_src_native_buffer_request_frame_async (self->src_native_buffer);

    meta_dbus_mutter_screen_cast_stream_complete_request_frame (skeleton,
                                                                invocation);
    return TRUE;
  }
#endif

  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR,
                                         G_DBUS_ERROR_FAILED,
                                         "Unknown stream backend type");
  return TRUE;
}

static gboolean
handle_get_info (MetaDBusMutterScreenCastStream *skeleton,
                 GDBusMethodInvocation          *invocation)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add (&b, "{sv}", "width", g_variant_new_uint32 (self->width));
  g_variant_builder_add (&b, "{sv}", "height", g_variant_new_uint32 (self->height));
  g_variant_builder_add (&b, "{sv}", "fps", g_variant_new_double ((double) self->fps));

  if (self->backend_type == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD) {
    g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("memfd"));
#ifdef HAVE_FURIOS_NATIVE_BUFFER
  } else if (self->backend_type == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER) {
    g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("native-buffer"));

    if (self->src_native_buffer)
      meta_furios_screen_cast_stream_src_native_buffer_add_info (self->src_native_buffer, &b);
#endif
  } else {
    g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("unknown"));
  }

  meta_dbus_mutter_screen_cast_stream_complete_get_info (skeleton,
                                                         invocation,
                                                         g_variant_builder_end (&b));
  return TRUE;
}

static gboolean
handle_stop (MetaDBusMutterScreenCastStream *skeleton,
             GDBusMethodInvocation          *invocation)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  meta_furios_screen_cast_stream_close (self);
  meta_dbus_mutter_screen_cast_stream_complete_stop (skeleton, invocation);
  return TRUE;
}

static void
meta_furios_screen_cast_stream_init_iface (MetaDBusMutterScreenCastStreamIface *iface)
{
  iface->handle_get_memfd = handle_get_memfd;
  iface->handle_request_frame = handle_request_frame;
  iface->handle_get_info = handle_get_info;
  iface->handle_stop = handle_stop;
  iface->handle_get_eis_fd = handle_get_eis_fd;
  iface->handle_get_native_buffer_handle = handle_get_native_buffer_handle;

#ifdef HAVE_FURIOS_NATIVE_BUFFER
  iface->handle_get_fence = handle_get_fence;
#endif
}

static void
meta_furios_screen_cast_stream_dispose (GObject *object)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (object);

  meta_furios_screen_cast_stream_close (self);

  g_clear_object (&self->eis);
  g_clear_object (&self->src_memfd);
#ifdef HAVE_FURIOS_NATIVE_BUFFER
  g_clear_object (&self->src_native_buffer);
#endif
  g_clear_object (&self->connection);
  g_clear_pointer (&self->peer_name, g_free);
  g_clear_pointer (&self->object_path, g_free);
  g_clear_pointer (&self->mapping_id, g_free);

  self->session = NULL;

  G_OBJECT_CLASS (meta_furios_screen_cast_stream_parent_class)->dispose (object);
}

static void
meta_furios_screen_cast_stream_class_init (MetaFuriosScreenCastStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_stream_dispose;
}

static void
meta_furios_screen_cast_stream_init (MetaFuriosScreenCastStream *self)
{
  self->last_emitted_seq = 0;
  self->mapping_id = NULL;

  self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD;

  self->src_memfd = NULL;
#ifdef HAVE_FURIOS_NATIVE_BUFFER
  self->src_native_buffer = NULL;
#endif

  self->width = 0;
  self->height = 0;
  self->fps = 0.0f;
}

void
meta_furios_screen_cast_stream_close (MetaFuriosScreenCastStream *self)
{
  if (!self)
    return;

  if (g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (self)))
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self));
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

static MetaFuriosScreenCastStreamBackendForce
parse_backend_force_env (const char *s)
{
  if (!s || *s == '\0')
    return META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_UNSET;
  if (g_ascii_strcasecmp (s, "memfd") == 0)
    return META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_MEMFD;
  if (g_ascii_strcasecmp (s, "native-buffer") == 0)
    return META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_NATIVE_BUFFER;
  return META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_UNSET;
}

#ifdef HAVE_FURIOS_NATIVE_BUFFER
static gboolean
try_init_native_buffer_backend (MetaFuriosScreenCastStream *self,
                                MetaBackend                *backend,
                                guint                       width,
                                guint                       height,
                                float                       fps,
                                GError                    **out_error)
{
  g_autoptr (GError) local_error = NULL;

  self->src_native_buffer = meta_furios_screen_cast_stream_src_native_buffer_new (backend,
                                                                                  width,
                                                                                  height,
                                                                                  fps,
                                                                                  &local_error);
  if (!self->src_native_buffer) {
    if (out_error)
      *out_error = g_steal_pointer (&local_error);
    return FALSE;
  }

  g_signal_connect_object (self->src_native_buffer,
                           "frame-published",
                           G_CALLBACK (on_native_buffer_frame_published),
                           self,
                           0);

  self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_NATIVE_BUFFER;
  return TRUE;
}
#endif

MetaFuriosScreenCastStream *
meta_furios_screen_cast_stream_new (MetaFuriosScreenCastSession *session,
                                    GDBusConnection             *connection,
                                    const char                  *peer_name,
                                    GVariant                    *properties,
                                    GError                     **error)
{
  g_return_val_if_fail (META_IS_FURIOS_SCREEN_CAST_SESSION (session), NULL);
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  g_return_val_if_fail (peer_name != NULL, NULL);

  MetaFuriosScreenCastStream *self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST_STREAM, NULL);

  self->session = session;
  self->connection = g_object_ref (connection);
  self->peer_name = g_strdup (peer_name);

  static guint stream_counter = 0;
  guint id = ++stream_counter;

  self->object_path = g_strdup_printf ("/io/furios/Mutter/ScreenCast/Stream/u%u", id);
  self->mapping_id = g_strdup_printf ("furios-screencast-stream-%u", id);

  guint width = 1920;
  guint height = 1080;
  float fps = 0.0f;

  int pref_w = 1920;
  int pref_h = 1080;
  float pref_rr = 0.0f;

  if (properties && g_variant_is_of_type (properties, G_VARIANT_TYPE_VARDICT)) {
    GVariant *v = NULL;

    if ((v = g_variant_lookup_value (properties, "width", G_VARIANT_TYPE_UINT32))) {
      width = g_variant_get_uint32 (v);
      g_variant_unref (v);
    }

    if ((v = g_variant_lookup_value (properties, "height", G_VARIANT_TYPE_UINT32))) {
      height = g_variant_get_uint32 (v);
      g_variant_unref (v);
    }

    if ((v = g_variant_lookup_value (properties, "fps", G_VARIANT_TYPE_DOUBLE))) {
      fps = (float) g_variant_get_double (v);
      g_variant_unref (v);
    } else if ((v = g_variant_lookup_value (properties, "fps", G_VARIANT_TYPE_UINT32))) {
      fps = (float) g_variant_get_uint32 (v);
      g_variant_unref (v);
    }
  }

  const char *env = g_getenv ("MUTTER_WAYLAND_NESTED_DISPLAY_RESOLUTION");
  if (parse_resolution_env (env, &pref_w, &pref_h, &pref_rr)) {
    width = (guint) pref_w;
    height = (guint) pref_h;
  }

  self->width = width;
  self->height = height;
  self->fps = fps;

  self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD;

  MetaBackend *backend = meta_furios_screen_cast_session_get_backend (session);

  const char *force_env = g_getenv ("MUTTER_FURIOS_SCREENCAST_BACKEND");
  MetaFuriosScreenCastStreamBackendForce force = parse_backend_force_env (force_env);

#ifdef HAVE_FURIOS_NATIVE_BUFFER
  if (force == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_NATIVE_BUFFER ||
      force == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_UNSET) {
    g_autoptr (GError) nb_error = NULL;

    if (try_init_native_buffer_backend (self, backend, width, height, fps, &nb_error)) {
      /* ok */
    } else if (force == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_NATIVE_BUFFER) {
      if (nb_error)
        g_debug ("native-buffer backend forced but unavailable, falling back to memfd: %s",
                 nb_error->message);
      else
        g_debug ("native-buffer backend forced but unavailable, falling back to memfd");
      self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD;
    } else {
      if (nb_error)
        g_debug ("native-buffer backend unavailable, falling back to memfd: %s", nb_error->message);
      else
        g_debug ("native-buffer backend unavailable, falling back to memfd");
      self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD;
    }
  } else if (force == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_MEMFD) {
    self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD;
  }
#else
  if (force == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_FORCE_NATIVE_BUFFER) {
    g_debug ("native-buffer backend forced but not built, falling back to memfd");
  }
  self->backend_type = META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD;
#endif

  if (self->backend_type == META_FURIOS_SCREEN_CAST_STREAM_BACKEND_MEMFD) {
    self->src_memfd = meta_furios_screen_cast_stream_src_memfd_new (backend,
                                                                    width,
                                                                    height,
                                                                    fps,
                                                                    error);
    if (!self->src_memfd) {
      g_object_unref (self);
      return NULL;
    }

    meta_furios_screen_cast_stream_src_memfd_set_dbus (self->src_memfd,
                                                       self->connection,
                                                       self->object_path,
                                                       self->peer_name);
  }

  self->eis = meta_furios_screen_cast_stream_eis_new (backend,
                                                      self->mapping_id,
                                                      (int) width,
                                                      (int) height);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self),
                                         self->connection,
                                         self->object_path,
                                         error)) {
    g_object_unref (self);
    return NULL;
  }

  return self;
}
