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

#include "backends/meta-furios-screen-cast.h"
#include "backends/meta-furios-screen-cast-session.h"

#define FURIOS_SCREEN_CAST_OBJECT_PATH "/io/furios/Mutter/ScreenCast"
#define FURIOS_SCREEN_CAST_BUS_NAME "io.furios.Mutter.ScreenCast"

struct _MetaFuriosScreenCast
{
  GObject parent_instance;

  MetaBackend *backend;

  GDBusConnection *connection;
  MetaDBusMutterScreenCast *skeleton;

  guint name_owner_id;
};

G_DEFINE_TYPE (MetaFuriosScreenCast, meta_furios_screen_cast, G_TYPE_OBJECT)

static gboolean
handle_create_session (MetaDBusMutterScreenCast *object,
                       GDBusMethodInvocation    *invocation,
                       GVariant                 *arg_properties,
                       gpointer                  user_data)
{
  (void) arg_properties;

  MetaFuriosScreenCast *self = user_data;
  g_autoptr (GError) error = NULL;
  const char *peer_name;
  MetaFuriosScreenCastSession *session;
  char *session_path = NULL;

  peer_name = g_dbus_method_invocation_get_sender (invocation);
  if (!peer_name || !*peer_name) {
    g_dbus_method_invocation_return_error (invocation,
                                          G_IO_ERROR,
                                          G_IO_ERROR_FAILED,
                                          "Missing peer name");
    return TRUE;
  }

  session = meta_furios_screen_cast_session_new (self->backend,
                                                 self->connection,
                                                 peer_name,
                                                 &error);
  if (!session) {
    g_dbus_method_invocation_return_gerror (invocation, error);
    return TRUE;
  }

  session_path =
    g_strdup (g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (session)));

  if (!session_path || !*session_path) {
    g_clear_pointer (&session_path, g_free);
    g_object_unref (session);
    g_dbus_method_invocation_return_error (invocation,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "Session was not exported (no object path)");
    return TRUE;
  }

  meta_dbus_mutter_screen_cast_complete_create_session (object,
                                                        invocation,
                                                        session_path);
  g_free (session_path);

  return TRUE;
}

static void
export_root_skeleton (MetaFuriosScreenCast *self)
{
  g_autoptr (GError) error = NULL;

  if (!self->connection || !self->skeleton)
    return;

  if (g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (self->skeleton)))
    return;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         self->connection,
                                         FURIOS_SCREEN_CAST_OBJECT_PATH,
                                         &error)) {
    g_warning ("Failed to export %s: %s",
               FURIOS_SCREEN_CAST_OBJECT_PATH,
               error->message);
    return;
  }
}

static void
unexport_root_skeleton (MetaFuriosScreenCast *self)
{
  if (!self->skeleton)
    return;

  if (g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (self->skeleton)))
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  (void) name;

  MetaFuriosScreenCast *self = user_data;

  g_clear_object (&self->connection);
  self->connection = g_object_ref (connection);

  export_root_skeleton (self);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  (void) connection;
  (void) name;
  (void) user_data;
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  (void) connection;
  (void) name;

  MetaFuriosScreenCast *self = user_data;

  unexport_root_skeleton (self);
  g_clear_object (&self->connection);
}

static void
meta_furios_screen_cast_dispose (GObject *object)
{
  MetaFuriosScreenCast *self = META_FURIOS_SCREEN_CAST (object);

  if (self->name_owner_id) {
    g_bus_unown_name (self->name_owner_id);
    self->name_owner_id = 0;
  }

  unexport_root_skeleton (self);

  g_clear_object (&self->connection);
  g_clear_object (&self->skeleton);
  g_clear_object (&self->backend);

  G_OBJECT_CLASS (meta_furios_screen_cast_parent_class)->dispose (object);
}

static void
meta_furios_screen_cast_class_init (MetaFuriosScreenCastClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_furios_screen_cast_dispose;
}

static void
meta_furios_screen_cast_init (MetaFuriosScreenCast *self)
{
  self->skeleton = meta_dbus_mutter_screen_cast_skeleton_new ();

  meta_dbus_mutter_screen_cast_set_version (self->skeleton, 1);

  g_signal_connect (self->skeleton,
                    "handle-create-session",
                    G_CALLBACK (handle_create_session),
                    self);
}

MetaFuriosScreenCast *
meta_furios_screen_cast_new (MetaBackend *backend)
{
  MetaFuriosScreenCast *self;

  g_return_val_if_fail (META_IS_BACKEND (backend), NULL);

  self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST, NULL);
  self->backend = g_object_ref (backend);

  self->name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                        FURIOS_SCREEN_CAST_BUS_NAME,
                                        G_BUS_NAME_OWNER_FLAGS_NONE,
                                        on_bus_acquired,
                                        on_name_acquired,
                                        on_name_lost,
                                        self,
                                        NULL);

  return self;
}
