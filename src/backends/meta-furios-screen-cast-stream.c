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

struct _MetaFuriosScreenCastStream
{
  MetaDBusMutterScreenCastStreamSkeleton parent_instance;

  MetaFuriosScreenCastSession *session;
  GDBusConnection *connection;
  char *peer_name;
  char *object_path;
  char *mapping_id;

  MetaFuriosScreenCastStreamSrcMemfd *src;

  MetaFuriosScreenCastStreamEis *eis;

  guint32 last_emitted_seq;
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

  int fd = meta_furios_screen_cast_stream_src_memfd_dup_fd (self->src);
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

  if (!meta_furios_screen_cast_stream_src_memfd_request_frame (self->src,
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

    has_damage_info = meta_furios_screen_cast_stream_src_memfd_get_last_damage (self->src,
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
  g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("memfd"));

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
}

static void
meta_furios_screen_cast_stream_dispose (GObject *object)
{
  MetaFuriosScreenCastStream *self = META_FURIOS_SCREEN_CAST_STREAM (object);

  meta_furios_screen_cast_stream_close (self);

  g_clear_object (&self->eis);
  g_clear_object (&self->src);
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

  if (properties && g_variant_is_of_type (properties,
                                         G_VARIANT_TYPE_VARDICT)) {
    GVariant *v = NULL;

    if ((v = g_variant_lookup_value (properties,
                                     "width",
                                     G_VARIANT_TYPE_UINT32))) {
      width = g_variant_get_uint32 (v);
      g_variant_unref (v);
    }

    if ((v = g_variant_lookup_value (properties,
                                     "height",
                                     G_VARIANT_TYPE_UINT32))) {
      height = g_variant_get_uint32 (v);
      g_variant_unref (v);
    }

    if ((v = g_variant_lookup_value (properties,
                                     "fps",
                                     G_VARIANT_TYPE_DOUBLE))) {
      fps = (float) g_variant_get_double (v);
      g_variant_unref (v);
    } else if ((v = g_variant_lookup_value (properties,
                                            "fps",
                                            G_VARIANT_TYPE_UINT32))) {
      fps = (float) g_variant_get_uint32 (v);
      g_variant_unref (v);
    }
  }

  const char *env = g_getenv ("MUTTER_WAYLAND_NESTED_DISPLAY_RESOLUTION");
  if (parse_resolution_env (env, &pref_w, &pref_h, &pref_rr)) {
    width = (guint) pref_w;
    height = (guint) pref_h;
  }

  MetaBackend *backend = meta_furios_screen_cast_session_get_backend (session);

  self->src = meta_furios_screen_cast_stream_src_memfd_new (backend,
                                                            width,
                                                            height,
                                                            fps,
                                                            error);
  if (!self->src) {
    g_object_unref (self);
    return NULL;
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

  meta_furios_screen_cast_stream_src_memfd_set_dbus (self->src,
                                                     self->connection,
                                                     self->object_path,
                                                     self->peer_name);

  return self;
}
