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

#include <sys/mman.h>
#include <unistd.h>

#include <wayland-server.h>

#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-virtual-keyboard.h"

#include "virtual-keyboard-unstable-v1-server-protocol.h"

typedef struct _MetaWaylandVirtualKeyboard
{
  MetaWaylandCompositor *compositor;

  struct wl_resource *resource;

  ClutterVirtualInputDevice *virtual_keyboard;

  struct xkb_context *xkb_context;
  struct xkb_keymap *keymap;
  struct xkb_state *state;

  gboolean has_keymap;
} MetaWaylandVirtualKeyboard;

static ClutterVirtualInputDevice *
create_virtual_keyboard_device (MetaWaylandCompositor *compositor)
{
  MetaContext *context = meta_wayland_compositor_get_context (compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterBackend *clutter_backend = meta_backend_get_clutter_backend (backend);
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_backend);
  ClutterSeatClass *klass;

  if (!seat)
    return NULL;

  klass = CLUTTER_SEAT_GET_CLASS (seat);
  if (!klass || !klass->create_virtual_device)
    return NULL;

  return klass->create_virtual_device (seat, CLUTTER_KEYBOARD_DEVICE);
}

static void
zwp_virtual_keyboard_v1_keymap (struct wl_client   *client,
                                struct wl_resource *resource,
                                uint32_t            format,
                                int32_t             fd,
                                uint32_t            size)
{
  MetaWaylandVirtualKeyboard *vkbd = wl_resource_get_user_data (resource);
  void *map = NULL;
  struct xkb_keymap *keymap = NULL;

  if (!vkbd) {
    close (fd);
    return;
  }

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    wl_resource_post_error (resource,
                            WL_DISPLAY_ERROR_INVALID_METHOD,
                            "zwp_virtual_keyboard_v1: only XKB_V1 supported");
    close (fd);
    return;
  }

  if (size == 0) {
    wl_resource_post_error (resource,
                            ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
                            "zwp_virtual_keyboard_v1: keymap size is 0");
    close (fd);
    return;
  }

  map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) {
    wl_client_post_no_memory (client);
    close (fd);
    return;
  }

  if (!vkbd->xkb_context)
    vkbd->xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);

  if (!vkbd->xkb_context) {
    munmap (map, size);
    close (fd);
    wl_client_post_no_memory (client);
    return;
  }

  keymap = xkb_keymap_new_from_buffer (vkbd->xkb_context,
                                       (const char *) map,
                                       size,
                                       XKB_KEYMAP_FORMAT_TEXT_V1,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap (map, size);
  close (fd);

  if (!keymap) {
    wl_resource_post_error (resource,
                            WL_DISPLAY_ERROR_INVALID_OBJECT,
                            "zwp_virtual_keyboard_v1: keymap compilation failed");
    return;
  }

  if (vkbd->keymap)
    xkb_keymap_unref (vkbd->keymap);
  vkbd->keymap = keymap;

  if (vkbd->state)
    xkb_state_unref (vkbd->state);
  vkbd->state = xkb_state_new (vkbd->keymap);

  vkbd->has_keymap = TRUE;
}

static void
zwp_virtual_keyboard_v1_key (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            time,
                             uint32_t            key,
                             uint32_t            state)
{
  MetaWaylandVirtualKeyboard *vkbd = wl_resource_get_user_data (resource);
  (void) client;

  if (!vkbd || !vkbd->virtual_keyboard)
    return;

  if (!vkbd->has_keymap) {
    wl_resource_post_error (resource,
                            ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
                            "zwp_virtual_keyboard_v1: keymap must be set before key()");
    return;
  }

  uint64_t ts_us = (uint64_t) time * 1000;
  ClutterKeyState ks = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? CLUTTER_KEY_STATE_PRESSED
                                                                : CLUTTER_KEY_STATE_RELEASED;

  clutter_virtual_input_device_notify_key (vkbd->virtual_keyboard,
                                           ts_us,
                                           key,  /* evdev keycode */
                                           ks);
}

static void
zwp_virtual_keyboard_v1_modifiers (struct wl_client   *client,
                                   struct wl_resource *resource,
                                   uint32_t            mods_depressed,
                                   uint32_t            mods_latched,
                                   uint32_t            mods_locked,
                                   uint32_t            group)
{
  MetaWaylandVirtualKeyboard *vkbd = wl_resource_get_user_data (resource);
  (void) client;

  if (!vkbd)
    return;

  if (!vkbd->has_keymap || !vkbd->state) {
    wl_resource_post_error (resource,
                            ZWP_VIRTUAL_KEYBOARD_V1_ERROR_NO_KEYMAP,
                            "zwp_virtual_keyboard_v1: keymap must be set before modifiers()");
    return;
  }

  xkb_state_update_mask (vkbd->state,
                         mods_depressed,
                         mods_latched,
                         mods_locked,
                         0, 0,
                         group);
}

static void
zwp_virtual_keyboard_v1_destroy (struct wl_client   *client,
                                 struct wl_resource *resource)
{
  (void) client;
  wl_resource_destroy (resource);
}

static const struct zwp_virtual_keyboard_v1_interface virtual_keyboard_interface =
{
  zwp_virtual_keyboard_v1_keymap,
  zwp_virtual_keyboard_v1_key,
  zwp_virtual_keyboard_v1_modifiers,
  zwp_virtual_keyboard_v1_destroy,
};

static void
virtual_keyboard_resource_destroy (struct wl_resource *resource)
{
  MetaWaylandVirtualKeyboard *vkbd = wl_resource_get_user_data (resource);

  if (!vkbd)
    return;

  if (vkbd->virtual_keyboard)
    g_object_unref (vkbd->virtual_keyboard);

  if (vkbd->state)
    xkb_state_unref (vkbd->state);
  if (vkbd->keymap)
    xkb_keymap_unref (vkbd->keymap);
  if (vkbd->xkb_context)
    xkb_context_unref (vkbd->xkb_context);

  g_free (vkbd);
}

static gboolean
client_is_authorized (struct wl_client *client)
{
  pid_t pid = 0;
  uid_t uid = (uid_t) -1;
  gid_t gid = (gid_t) -1;

  if (!client)
    return FALSE;

  wl_client_get_credentials (client, &pid, &uid, &gid);

  if (uid == 0)
    return TRUE;

  if (uid == getuid ())
    return TRUE;

  return FALSE;
}

static void
zwp_virtual_keyboard_manager_v1_create_virtual_keyboard (struct wl_client   *client,
                                                         struct wl_resource *resource,
                                                         struct wl_resource *seat_resource,
                                                         uint32_t            id)
{
  MetaWaylandCompositor *compositor = wl_resource_get_user_data (resource);
  MetaWaylandVirtualKeyboard *vkbd;
  struct wl_resource *kbd_res;

  (void) seat_resource;

  if (!compositor) {
    wl_client_post_no_memory (client);
    return;
  }

  if (!client_is_authorized (client)) {
    wl_resource_post_error (resource,
                            ZWP_VIRTUAL_KEYBOARD_MANAGER_V1_ERROR_UNAUTHORIZED,
                            "zwp_virtual_keyboard_manager_v1: unauthorized");
    return;
  }

  vkbd = g_new0 (MetaWaylandVirtualKeyboard, 1);
  vkbd->compositor = compositor;

  vkbd->virtual_keyboard = create_virtual_keyboard_device (compositor);
  if (!vkbd->virtual_keyboard) {
    g_free (vkbd);
    wl_client_post_no_memory (client);
    return;
  }

  kbd_res = wl_resource_create (client,
                                &zwp_virtual_keyboard_v1_interface,
                                wl_resource_get_version (resource),
                                id);
  if (!kbd_res) {
    g_object_unref (vkbd->virtual_keyboard);
    g_free (vkbd);
    wl_client_post_no_memory (client);
    return;
  }

  vkbd->resource = kbd_res;

  wl_resource_set_implementation (kbd_res,
                                  &virtual_keyboard_interface,
                                  vkbd,
                                  virtual_keyboard_resource_destroy);
}

static const struct zwp_virtual_keyboard_manager_v1_interface virtual_keyboard_manager_interface =
{
  zwp_virtual_keyboard_manager_v1_create_virtual_keyboard,
};

static void
bind_virtual_keyboard_manager (struct wl_client *client,
                               void             *data,
                               uint32_t          version,
                               uint32_t          id)
{
  MetaWaylandCompositor *compositor = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client,
                                 &zwp_virtual_keyboard_manager_v1_interface,
                                 version,
                                 id);
  if (!resource) {
    wl_client_post_no_memory (client);
    return;
  }

  wl_resource_set_implementation (resource,
                                  &virtual_keyboard_manager_interface,
                                  compositor,
                                  NULL);
}

void
meta_wayland_virtual_keyboard_init (MetaWaylandCompositor *compositor)
{
  if (!wl_global_create (compositor->wayland_display,
                         &zwp_virtual_keyboard_manager_v1_interface,
                         1,
                         compositor,
                         bind_virtual_keyboard_manager))
    g_error ("Failed to create zwp_virtual_keyboard_manager_v1 global");
}
