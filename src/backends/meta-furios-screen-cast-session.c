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

#include "backends/meta-furios-screen-cast-session.h"
#include "backends/meta-furios-screen-cast-stream.h"

struct _MetaFuriosScreenCastSession
{
  MetaDBusMutterScreenCastSessionSkeleton parent_instance;

  MetaBackend *backend;
  GDBusConnection *connection;
  char *peer_name;
  char *object_path;

  GPtrArray *streams;
  gboolean started;
};

static void meta_furios_screen_cast_session_init_iface (MetaDBusMutterScreenCastSessionIface *iface);
void meta_furios_screen_cast_session_stop (MetaFuriosScreenCastSession *self);

G_DEFINE_TYPE_WITH_CODE (MetaFuriosScreenCastSession,
                         meta_furios_screen_cast_session,
                         META_DBUS_TYPE_MUTTER_SCREEN_CAST_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_MUTTER_SCREEN_CAST_SESSION,
                                                meta_furios_screen_cast_session_init_iface))

static gboolean
check_permission (MetaFuriosScreenCastSession *self,
                  GDBusMethodInvocation       *invocation)
{
  return g_strcmp0 (self->peer_name,
                    g_dbus_method_invocation_get_sender (invocation)) == 0;
}

static gboolean
handle_start (MetaDBusMutterScreenCastSession *skeleton,
              GDBusMethodInvocation           *invocation)
{
  MetaFuriosScreenCastSession *self = META_FURIOS_SCREEN_CAST_SESSION (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  self->started = TRUE;
  meta_dbus_mutter_screen_cast_session_complete_start (skeleton, invocation);
  return TRUE;
}

static gboolean
handle_stop (MetaDBusMutterScreenCastSession *skeleton,
             GDBusMethodInvocation           *invocation)
{
  MetaFuriosScreenCastSession *self = META_FURIOS_SCREEN_CAST_SESSION (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  meta_furios_screen_cast_session_stop (self);
  meta_dbus_mutter_screen_cast_session_complete_stop (skeleton, invocation);
  return TRUE;
}

static gboolean
handle_create_stream (MetaDBusMutterScreenCastSession *skeleton,
                      GDBusMethodInvocation           *invocation,
                      GVariant                        *arg_properties)
{
  MetaFuriosScreenCastSession *self = META_FURIOS_SCREEN_CAST_SESSION (skeleton);

  if (!check_permission (self, invocation)) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_ACCESS_DENIED,
                                           "Permission denied");
    return TRUE;
  }

  g_autoptr (GError) error = NULL;
  MetaFuriosScreenCastStream *stream = meta_furios_screen_cast_stream_new (self,
                                                                           self->connection,
                                                                           self->peer_name,
                                                                           arg_properties,
                                                                           &error);

  if (!stream) {
    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_FAILED,
                                           "%s", error->message);
    return TRUE;
  }

  g_ptr_array_add (self->streams, g_object_ref (stream));

  const char *stream_path = meta_furios_screen_cast_stream_get_object_path (stream);
  meta_dbus_mutter_screen_cast_session_complete_create_stream (skeleton, invocation, stream_path);

  g_object_unref (stream);
  return TRUE;
}

static void
meta_furios_screen_cast_session_init_iface (MetaDBusMutterScreenCastSessionIface *iface)
{
  iface->handle_start = handle_start;
  iface->handle_stop = handle_stop;
  iface->handle_create_stream = handle_create_stream;
}

static void
meta_furios_screen_cast_session_dispose (GObject *object)
{
  MetaFuriosScreenCastSession *self = META_FURIOS_SCREEN_CAST_SESSION (object);

  meta_furios_screen_cast_session_stop (self);

  g_clear_object (&self->connection);
  g_clear_pointer (&self->peer_name, g_free);
  g_clear_pointer (&self->object_path, g_free);

  G_OBJECT_CLASS (meta_furios_screen_cast_session_parent_class)->dispose (object);
}

static void
meta_furios_screen_cast_session_class_init (MetaFuriosScreenCastSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_session_dispose;
}

static void
meta_furios_screen_cast_session_init (MetaFuriosScreenCastSession *self)
{
  self->streams = g_ptr_array_new_with_free_func (g_object_unref);
  self->started = FALSE;
}

MetaBackend *
meta_furios_screen_cast_session_get_backend (MetaFuriosScreenCastSession *session)
{
  return session->backend;
}

void
meta_furios_screen_cast_session_stop (MetaFuriosScreenCastSession *self)
{
  if (!self)
    return;

  if (self->streams) {
    for (guint i = 0; i < self->streams->len; i++) {
      MetaFuriosScreenCastStream *stream = g_ptr_array_index (self->streams, i);
      meta_furios_screen_cast_stream_close (stream);
    }
    g_ptr_array_set_size (self->streams, 0);
  }

  if (g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (self))) {
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self));
    meta_dbus_mutter_screen_cast_session_emit_closed (META_DBUS_MUTTER_SCREEN_CAST_SESSION (self));
  }

  self->started = FALSE;
}

MetaFuriosScreenCastSession *
meta_furios_screen_cast_session_new (MetaBackend      *backend,
                                     GDBusConnection  *connection,
                                     const char       *peer_name,
                                     GError          **error)
{
  g_return_val_if_fail (backend != NULL, NULL);
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  g_return_val_if_fail (peer_name != NULL, NULL);

  MetaFuriosScreenCastSession *self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST_SESSION, NULL);

  self->backend = backend;
  self->connection = g_object_ref (connection);
  self->peer_name = g_strdup (peer_name);

  static guint session_counter = 0;
  self->object_path = g_strdup_printf ("/io/furios/Mutter/ScreenCast/Session/u%u", ++session_counter);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self),
                                         self->connection,
                                         self->object_path,
                                         error)) {
    g_object_unref (self);
    return NULL;
  }

  return self;
}
