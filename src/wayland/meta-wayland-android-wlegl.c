#include "config.h"

#include "wayland/meta-wayland-android-wlegl.h"

#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#include "wayland-android-client-protocol.h"
#include "wayland-android-server-protocol.h"

typedef struct _MetaWaylandAndroidWlEgl
{
  MetaWaylandCompositor *compositor;
  struct wl_global *global;

  struct wl_display *host_display;
  struct wl_registry *host_registry;
  struct android_wlegl *host_android_wlegl;
} MetaWaylandAndroidWlEgl;

typedef struct _MetaWaylandAndroidBuffer
{
  struct wl_resource *client_buffer_resource;
  struct wl_buffer *host_buffer;

  struct wl_array ints;
  GArray *fds;

  int width;
  int height;
  int format;
  int stride;
  int usage;
} MetaWaylandAndroidBuffer;

typedef struct _MetaWaylandAndroidServerBufferHandle
{
  MetaWaylandAndroidWlEgl *android_wlegl;

  struct wl_resource *client_handle_resource;
  struct android_wlegl_server_buffer_handle *host_handle;

  struct wl_array ints;
  GArray *fds;

  int width;
  int height;
  int format;
  int usage;
  int stride;
} MetaWaylandAndroidServerBufferHandle;

static void
meta_android_buffer_destroy_resource (struct wl_resource *resource)
{
  MetaWaylandAndroidBuffer *buffer = wl_resource_get_user_data (resource);

  g_printerr ("ANDROID_WLEGL LOCAL WL_BUFFER DESTROY buffer=%p host_buffer=%p\n",
              buffer,
              buffer ? buffer->host_buffer : NULL);

  if (!buffer)
    return;

  for (guint i = 0; i < buffer->fds->len; i++) {
    int fd = g_array_index (buffer->fds, int, i);
    if (fd >= 0)
      close (fd);
  }

  g_array_unref (buffer->fds);
  wl_array_release (&buffer->ints);

  buffer->host_buffer = NULL;
  g_free (buffer);
}

static void
meta_android_buffer_destroy (struct wl_client   *client,
                             struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_buffer_interface meta_android_buffer_implementation = {
  meta_android_buffer_destroy,
};

gboolean
meta_wayland_android_wlegl_buffer_is_android (struct wl_resource *resource)
{
  if (!resource)
    return FALSE;

  return wl_resource_instance_of (resource,
                                  &wl_buffer_interface,
                                  &meta_android_buffer_implementation);
}

gboolean
meta_wayland_android_wlegl_buffer_get_size (struct wl_resource *resource,
                                            int                *width,
                                            int                *height)
{
  MetaWaylandAndroidBuffer *buffer;

  if (!meta_wayland_android_wlegl_buffer_is_android (resource))
    return FALSE;

  buffer = wl_resource_get_user_data (resource);
  if (!buffer)
    return FALSE;

  if (width)
    *width = buffer->width;
  if (height)
    *height = buffer->height;

  return TRUE;
}

int
meta_wayland_android_wlegl_buffer_get_format (struct wl_resource *resource)
{
  MetaWaylandAndroidBuffer *buffer;

  if (!meta_wayland_android_wlegl_buffer_is_android (resource))
    return 0;

  buffer = wl_resource_get_user_data (resource);
  if (!buffer)
    return 0;

  return buffer->format;
}

int
meta_wayland_android_wlegl_buffer_get_stride (struct wl_resource *resource)
{
  MetaWaylandAndroidBuffer *buffer;

  if (!meta_wayland_android_wlegl_buffer_is_android (resource))
    return 0;

  buffer = wl_resource_get_user_data (resource);
  if (!buffer)
    return 0;

  return buffer->stride;
}

int
meta_wayland_android_wlegl_buffer_get_usage (struct wl_resource *resource)
{
  MetaWaylandAndroidBuffer *buffer;

  if (!meta_wayland_android_wlegl_buffer_is_android (resource))
    return 0;

  buffer = wl_resource_get_user_data (resource);
  if (!buffer)
    return 0;

  return buffer->usage;
}

const struct wl_array *
meta_wayland_android_wlegl_buffer_get_ints (struct wl_resource *resource)
{
  MetaWaylandAndroidBuffer *buffer;

  if (!meta_wayland_android_wlegl_buffer_is_android (resource))
    return NULL;

  buffer = wl_resource_get_user_data (resource);
  if (!buffer)
    return NULL;

  return &buffer->ints;
}

GArray *
meta_wayland_android_wlegl_buffer_get_fds (struct wl_resource *resource)
{
  MetaWaylandAndroidBuffer *buffer;

  if (!meta_wayland_android_wlegl_buffer_is_android (resource))
    return NULL;

  buffer = wl_resource_get_user_data (resource);
  if (!buffer)
    return NULL;

  return buffer->fds;
}

static void
meta_server_buffer_handle_destroy_resource (struct wl_resource *resource)
{
  MetaWaylandAndroidServerBufferHandle *handle = wl_resource_get_user_data (resource);

  g_printerr ("ANDROID_WLEGL SERVER HANDLE DESTROY handle=%p\n", handle);

  if (!handle)
    return;

  if (handle->host_handle)
    android_wlegl_server_buffer_handle_destroy (handle->host_handle);

  for (guint i = 0; i < handle->fds->len; i++) {
    int fd = g_array_index (handle->fds, int, i);
    if (fd >= 0)
      close (fd);
  }

  g_array_unref (handle->fds);
  wl_array_release (&handle->ints);

  g_free (handle);
}

static void
host_server_buffer_handle_buffer_fd (void                                      *data,
                                     struct android_wlegl_server_buffer_handle *host_handle,
                                     int32_t                                    fd)
{
  MetaWaylandAndroidServerBufferHandle *handle = data;
  int dup_fd = dup (fd);

  g_printerr ("ANDROID_WLEGL HOST BUFFER_FD fd=%d dup=%d\n", fd, dup_fd);

  if (dup_fd >= 0)
    g_array_append_val (handle->fds, dup_fd);
}

static void
host_server_buffer_handle_buffer_ints (void                                      *data,
                                       struct android_wlegl_server_buffer_handle *host_handle,
                                       struct wl_array                           *ints)
{
  MetaWaylandAndroidServerBufferHandle *handle = data;
  void *dst;

  g_printerr ("ANDROID_WLEGL HOST BUFFER_INTS size=%zu\n", ints->size);

  dst = wl_array_add (&handle->ints, ints->size);
  memcpy (dst, ints->data, ints->size);
}

static void
host_server_buffer_handle_buffer (void                                      *data,
                                  struct android_wlegl_server_buffer_handle *host_handle,
                                  struct wl_buffer                          *host_buffer,
                                  int32_t                                    format,
                                  int32_t                                    stride)
{
  MetaWaylandAndroidServerBufferHandle *handle = data;
  MetaWaylandAndroidBuffer *buffer;
  struct wl_client *client;
  struct wl_resource *client_buffer_resource;
  void *ints_dst;

  g_printerr ("ANDROID_WLEGL HOST BUFFER host_buffer=%p format=%d stride=%d\n",
              host_buffer,
              format,
              stride);

  handle->stride = stride;
  client = wl_resource_get_client (handle->client_handle_resource);

  buffer = g_new0 (MetaWaylandAndroidBuffer, 1);
  buffer->host_buffer = host_buffer;
  buffer->width = handle->width;
  buffer->height = handle->height;
  buffer->format = format;
  buffer->stride = stride;
  buffer->usage = handle->usage;
  buffer->fds = g_array_new (FALSE, FALSE, sizeof (int));
  wl_array_init (&buffer->ints);

  ints_dst = wl_array_add (&buffer->ints, handle->ints.size);
  memcpy (ints_dst, handle->ints.data, handle->ints.size);

  for (guint i = 0; i < handle->fds->len; i++) {
    int fd = g_array_index (handle->fds, int, i);
    int dup_fd = dup (fd);

    if (dup_fd >= 0)
      g_array_append_val (buffer->fds, dup_fd);
  }

  client_buffer_resource = wl_resource_create (client,
                                               &wl_buffer_interface,
                                               1,
                                               0);

  if (!client_buffer_resource) {
    for (guint i = 0; i < buffer->fds->len; i++) {
      int fd = g_array_index (buffer->fds, int, i);
      if (fd >= 0)
        close (fd);
    }

    g_array_unref (buffer->fds);
    wl_array_release (&buffer->ints);
    g_free (buffer);
    wl_client_post_no_memory (client);
    return;
  }

  buffer->client_buffer_resource = client_buffer_resource;

  wl_resource_set_implementation (client_buffer_resource,
                                  &meta_android_buffer_implementation,
                                  buffer,
                                  meta_android_buffer_destroy_resource);

  android_wlegl_server_buffer_handle_send_buffer_ints (handle->client_handle_resource,
                                                       &handle->ints);

  for (guint i = 0; i < handle->fds->len; i++) {
    int fd = g_array_index (handle->fds, int, i);
    android_wlegl_server_buffer_handle_send_buffer_fd (handle->client_handle_resource,
                                                       fd);
  }

  android_wlegl_server_buffer_handle_send_buffer (handle->client_handle_resource,
                                                  client_buffer_resource,
                                                  format,
                                                  stride);

  wl_client_flush (client);

  g_printerr ("ANDROID_WLEGL SENT LOCAL BUFFER client_buffer=%p host_buffer=%p fds=%u ints=%zu\n",
              client_buffer_resource,
              host_buffer,
              buffer->fds->len,
              buffer->ints.size);
}

static const struct android_wlegl_server_buffer_handle_listener host_server_buffer_handle_listener = {
  host_server_buffer_handle_buffer_fd,
  host_server_buffer_handle_buffer_ints,
  host_server_buffer_handle_buffer,
};

static void
meta_android_wlegl_get_server_buffer_handle (struct wl_client   *client,
                                             struct wl_resource *resource,
                                             uint32_t            id,
                                             int32_t             width,
                                             int32_t             height,
                                             int32_t             format,
                                             int32_t             usage)
{
  MetaWaylandAndroidWlEgl *android_wlegl = wl_resource_get_user_data (resource);
  MetaWaylandAndroidServerBufferHandle *handle;

  g_printerr ("ANDROID_WLEGL GET_SERVER_BUFFER_HANDLE %dx%d format=%d usage=0x%x id=%u\n",
              width,
              height,
              format,
              usage,
              id);

  if (!android_wlegl->host_android_wlegl) {
    wl_resource_post_error (resource,
                            ANDROID_WLEGL_ERROR_BAD_VALUE,
                            "host android_wlegl is unavailable");
    return;
  }

  handle = g_new0 (MetaWaylandAndroidServerBufferHandle, 1);
  handle->android_wlegl = android_wlegl;
  handle->width = width;
  handle->height = height;
  handle->format = format;
  handle->usage = usage;
  handle->fds = g_array_new (FALSE, FALSE, sizeof (int));
  wl_array_init (&handle->ints);

  handle->client_handle_resource = wl_resource_create (client,
                                                       &android_wlegl_server_buffer_handle_interface,
                                                       1,
                                                       id);

  if (!handle->client_handle_resource) {
    g_array_unref (handle->fds);
    wl_array_release (&handle->ints);
    g_free (handle);
    wl_client_post_no_memory (client);
    return;
  }

  wl_resource_set_implementation (handle->client_handle_resource,
                                  NULL,
                                  handle,
                                  meta_server_buffer_handle_destroy_resource);

  handle->host_handle = android_wlegl_get_server_buffer_handle (android_wlegl->host_android_wlegl,
                                                                width,
                                                                height,
                                                                format,
                                                                usage);

  android_wlegl_server_buffer_handle_add_listener (handle->host_handle,
                                                   &host_server_buffer_handle_listener,
                                                   handle);

  wl_display_roundtrip (android_wlegl->host_display);
}

static void
meta_android_wlegl_destroy_resource (struct wl_resource *resource)
{
  g_printerr ("ANDROID_WLEGL RESOURCE DESTROY\n");
}

static void
meta_android_wlegl_handle_add_fd (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  int32_t             fd)
{
  g_printerr ("ANDROID_WLEGL HANDLE ADD_FD fd=%d\n", fd);
  close (fd);
}

static void
meta_android_wlegl_handle_destroy (struct wl_client   *client,
                                   struct wl_resource *resource)
{
  g_printerr ("ANDROID_WLEGL HANDLE DESTROY\n");
  wl_resource_destroy (resource);
}

static const struct android_wlegl_handle_interface meta_android_wlegl_handle_implementation = {
  meta_android_wlegl_handle_add_fd,
  meta_android_wlegl_handle_destroy,
};

static void
meta_android_wlegl_handle_resource_destroy (struct wl_resource *resource)
{
  g_printerr ("ANDROID_WLEGL HANDLE RESOURCE DESTROY\n");
}

static void
meta_android_wlegl_create_handle (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  uint32_t            id,
                                  int32_t             num_fds,
                                  struct wl_array    *ints)
{
  struct wl_resource *handle_resource;

  g_printerr ("ANDROID_WLEGL CREATE_HANDLE num_fds=%d ints_size=%zu\n",
              num_fds,
              ints ? ints->size : 0);

  handle_resource = wl_resource_create (client,
                                        &android_wlegl_handle_interface,
                                        1,
                                        id);

  if (!handle_resource) {
    wl_client_post_no_memory (client);
    return;
  }

  wl_resource_set_implementation (handle_resource,
                                  &meta_android_wlegl_handle_implementation,
                                  NULL,
                                  meta_android_wlegl_handle_resource_destroy);
}

static void
meta_android_wlegl_create_buffer (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  uint32_t            id,
                                  int32_t             width,
                                  int32_t             height,
                                  int32_t             stride,
                                  int32_t             format,
                                  int32_t             usage,
                                  struct wl_resource *native_handle)
{
  g_printerr ("ANDROID_WLEGL CREATE_BUFFER %dx%d stride=%d format=%d usage=0x%x\n",
              width,
              height,
              stride,
              format,
              usage);

  wl_resource_post_error (resource,
                          ANDROID_WLEGL_ERROR_BAD_VALUE,
                          "android_wlegl.create_buffer is not implemented yet");
}

static const struct android_wlegl_interface meta_android_wlegl_implementation = {
  meta_android_wlegl_create_handle,
  meta_android_wlegl_create_buffer,
  meta_android_wlegl_get_server_buffer_handle,
};

static void
bind_android_wlegl (struct wl_client *client,
                    void             *data,
                    uint32_t          version,
                    uint32_t          id)
{
  MetaWaylandAndroidWlEgl *android_wlegl = data;
  struct wl_resource *resource;
  uint32_t bind_version = MIN (version, 2);

  g_printerr ("ANDROID_WLEGL BIND version=%u bind_version=%u id=%u\n",
              version,
              bind_version,
              id);

  resource = wl_resource_create (client,
                                 &android_wlegl_interface,
                                 bind_version,
                                 id);

  if (!resource) {
    wl_client_post_no_memory (client);
    return;
  }

  wl_resource_set_implementation (resource,
                                  &meta_android_wlegl_implementation,
                                  android_wlegl,
                                  meta_android_wlegl_destroy_resource);
}

static void
host_registry_global (void               *data,
                      struct wl_registry *registry,
                      uint32_t            name,
                      const char         *interface,
                      uint32_t            version)
{
  MetaWaylandAndroidWlEgl *android_wlegl = data;

  if (strcmp (interface, "android_wlegl") == 0) {
    android_wlegl->host_android_wlegl = wl_registry_bind (registry,
                                                          name,
                                                          &android_wlegl_interface,
                                                          MIN (version, 2));

    g_printerr ("ANDROID_WLEGL HOST BOUND android_wlegl=%p\n",
                android_wlegl->host_android_wlegl);
  }
}

static void
host_registry_global_remove (void               *data,
                             struct wl_registry *registry,
                             uint32_t            name)
{
}

static const struct wl_registry_listener host_registry_listener = {
  host_registry_global,
  host_registry_global_remove,
};

void
meta_wayland_android_wlegl_init (MetaWaylandCompositor *compositor)
{
  MetaWaylandAndroidWlEgl *android_wlegl;

  g_printerr ("ANDROID_WLEGL INIT HIT\n");

  android_wlegl = g_new0 (MetaWaylandAndroidWlEgl, 1);
  android_wlegl->compositor = compositor;

  android_wlegl->host_display = wl_display_connect (NULL);
  if (!android_wlegl->host_display){
    g_printerr ("ANDROID_WLEGL failed to connect to host Wayland display\n");
    return;
  }

  android_wlegl->host_registry = wl_display_get_registry (android_wlegl->host_display);
  wl_registry_add_listener (android_wlegl->host_registry,
                            &host_registry_listener,
                            android_wlegl);
  wl_display_roundtrip (android_wlegl->host_display);

  android_wlegl->global = wl_global_create (compositor->wayland_display,
                                            &android_wlegl_interface,
                                            2,
                                            android_wlegl,
                                            bind_android_wlegl);

  if (!android_wlegl->global)
    g_error ("Failed to create android_wlegl global");

  g_printerr ("ANDROID_WLEGL GLOBAL CREATED host_android_wlegl=%p\n",
              android_wlegl->host_android_wlegl);
}
