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
#include <linux/memfd.h>
#include <sys/syscall.h>

#include "backends/meta-stage-private.h"

#include "backends/meta-furios-screen-cast-stream-src-memfd.h"
#include "backends/meta-furios-screen-cast-memfd-protocol.h"

#define MAX_DAMAGE_RECTS 64
/* merge rects that touch/are within 1px */
#define DAMAGE_MERGE_PAD 1

#define DAMAGE_SIGNAL_MAX_RECTS 32
/* if more than 70% of frame then use FrameReady
 * this is finnicky and not an exact representative */
#define DAMAGE_SIGNAL_AREA_PERCENT 70

struct _MetaFuriosScreenCastStreamSrcMemfd
{
  GObject parent;

  MetaBackend *backend;

  int memfd;
  void *map;
  size_t map_len;

  MetaFuriosMemfdHeader *shared_header;

  guint32 seq;

  guint tick_id;

  GDBusConnection *connection;
  char *object_path;
  char *peer_name;

  MetaStageWatch *paint_watch;
  gboolean watch_installed;

  gboolean stage_dirty;

  guint damage_n;
  guint last_damage_n;

  /* damage arrays so we can store last damage with swaps */
  MtkRectangle damage_rects_a[MAX_DAMAGE_RECTS];
  MtkRectangle damage_rects_b[MAX_DAMAGE_RECTS];

  MtkRectangle *damage_rects;
  MtkRectangle *last_damage_rects;

  gboolean last_should_emit_damage;
};

G_DEFINE_TYPE (MetaFuriosScreenCastStreamSrcMemfd,
               meta_furios_screen_cast_stream_src_memfd,
               G_TYPE_OBJECT)

static int
create_memfd (const char *name)
{
#ifdef SYS_memfd_create
  return (int) syscall (SYS_memfd_create, name, (unsigned int) MFD_CLOEXEC);
#else
  (void) name;
  errno = ENOSYS;
  return -1;
#endif
}

static inline gboolean
rect_is_empty (const MtkRectangle *r)
{
  return (r->width <= 0 || r->height <= 0);
}

static inline gboolean
rect_is_full (MetaFuriosScreenCastStreamSrcMemfd *self,
              const MtkRectangle                 *r)
{
  if (!self->shared_header)
    return FALSE;

  return r->x == 0 &&
         r->y == 0 &&
         r->width == (int) self->shared_header->width &&
         r->height == (int) self->shared_header->height;
}

static inline void
rect_union_inplace (MtkRectangle       *a,
                    const MtkRectangle *b)
{
  int x1 = MIN (a->x, b->x);
  int y1 = MIN (a->y, b->y);
  int x2 = MAX (a->x + a->width,  b->x + b->width);
  int y2 = MAX (a->y + a->height, b->y + b->height);

  a->x = x1;
  a->y = y1;
  a->width = x2 - x1;
  a->height = y2 - y1;
}

static inline gboolean
rect_intersects_or_touches (const MtkRectangle *a,
                            const MtkRectangle *b,
                            int                 pad)
{
  /* expand a by pad on all sides and test intersection with b */
  int ax1 = a->x - pad;
  int ay1 = a->y - pad;
  int ax2 = a->x + a->width + pad;
  int ay2 = a->y + a->height + pad;

  int bx1 = b->x;
  int by1 = b->y;
  int bx2 = b->x + b->width;
  int by2 = b->y + b->height;

  if (ax2 <= bx1 || bx2 <= ax1)
    return FALSE;
  if (ay2 <= by1 || by2 <= ay1)
    return FALSE;
  return TRUE;
}

static void
clamp_rect_to_buffer (MetaFuriosScreenCastStreamSrcMemfd *self,
                      MtkRectangle                       *r)
{
  if (!self->shared_header) {
    r->x = r->y = r->width = r->height = 0;
    return;
  }

  int width = (int) self->shared_header->width;
  int height = (int) self->shared_header->height;

  if (rect_is_empty (r)) {
    r->x = r->y = r->width = r->height = 0;
    return;
  }

  int x = r->x;
  int y = r->y;
  int w = r->width;
  int h = r->height;

  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }

  if (x + w > width)
    w = width - x;
  if (y + h > height)
    h = height - y;

  if (w <= 0 || h <= 0) {
    r->x = r->y = r->width = r->height = 0;
    return;
  }

  r->x = x;
  r->y = y;
  r->width = w;
  r->height = h;
}

static void
clear_damage (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  self->damage_n = 0;
}

static void
collapse_damage_to_bbox (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (self->damage_n == 0)
    return;

  MtkRectangle bbox = self->damage_rects[0];
  for (guint i = 1; i < self->damage_n; i++)
    rect_union_inplace (&bbox, &self->damage_rects[i]);

  self->damage_rects[0] = bbox;
  self->damage_n = 1;
}

static void
add_damage_rect (MetaFuriosScreenCastStreamSrcMemfd *self,
                 const MtkRectangle                 *in_r)
{
  MtkRectangle r = *in_r;

  if (rect_is_empty (&r))
    return;

  clamp_rect_to_buffer (self, &r);
  if (rect_is_empty (&r))
    return;

  /* if this is full frame damage, just replace everything */
  if (rect_is_full (self, &r)) {
    self->damage_rects[0] = r;
    self->damage_n = 1;
    return;
  }

  /* if we already have a full frame, no need to track anything else */
  if (self->damage_n == 1 && rect_is_full (self, &self->damage_rects[0]))
    return;

  /* try to merge into an existing rect */
  for (guint i = 0; i < self->damage_n; i++) {
    if (rect_intersects_or_touches (&self->damage_rects[i], &r, DAMAGE_MERGE_PAD)) {
      rect_union_inplace (&self->damage_rects[i], &r);

      /* the merged rect may now intersect others */
      for (guint j = 0; j < self->damage_n; ) {
        if (j != i &&
            rect_intersects_or_touches (&self->damage_rects[i], &self->damage_rects[j], DAMAGE_MERGE_PAD)) {
          rect_union_inplace (&self->damage_rects[i], &self->damage_rects[j]);

          /* remove j by swapping last */
          self->damage_rects[j] = self->damage_rects[self->damage_n - 1];
          self->damage_n--;
          continue;
        }
        j++;
      }
      return;
    }
  }

  if (self->damage_n < MAX_DAMAGE_RECTS) {
    self->damage_rects[self->damage_n++] = r;
    return;
  }

  /* collapse to bbox and union */
  collapse_damage_to_bbox (self);
  rect_union_inplace (&self->damage_rects[0], &r);
  clamp_rect_to_buffer (self, &self->damage_rects[0]);
  self->damage_n = 1;
}

static void
close_and_unmap (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (self->tick_id) {
    g_source_remove (self->tick_id);
    self->tick_id = 0;
  }

  if (self->paint_watch) {
    MetaStage *meta_stage = NULL;
    ClutterActor *actor = meta_backend_get_stage (self->backend);

    if (actor)
      meta_stage = META_STAGE (actor);

    if (meta_stage) {
      if (self->paint_watch) {
        meta_stage_remove_watch (meta_stage, self->paint_watch);
        self->paint_watch = NULL;
      }
    }

    self->watch_installed = FALSE;
  }

  if (self->map && self->map != MAP_FAILED) {
    munmap (self->map, self->map_len);
    self->map = NULL;
    self->map_len = 0;
  }

  self->shared_header = NULL;

  if (self->memfd >= 0) {
    close (self->memfd);
    self->memfd = -1;
  }

  g_clear_object (&self->connection);
  g_clear_pointer (&self->object_path, g_free);
  g_clear_pointer (&self->peer_name, g_free);
}

static void
meta_furios_screen_cast_stream_src_memfd_dispose (GObject *object)
{
  MetaFuriosScreenCastStreamSrcMemfd *self = META_FURIOS_SCREEN_CAST_STREAM_SRC_MEMFD (object);

  close_and_unmap (self);

  G_OBJECT_CLASS (meta_furios_screen_cast_stream_src_memfd_parent_class)->dispose (object);
}

static ClutterStageView *
get_any_stage_view (MetaFuriosScreenCastStreamSrcMemfd *self)
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

  /* during early startup (0,0) can be unmapped / invalid */
  view = clutter_stage_get_view_at (stage, 1.f, 1.f);
  if (view)
    return view;
  view = clutter_stage_get_view_at (stage, 16.f, 16.f);
  if (view)
    return view;
  view = clutter_stage_get_view_at (stage, 64.f, 64.f);
  if (view)
    return view;
  view = clutter_stage_get_view_at (stage, 256.f, 256.f);
  if (view)
    return view;
  view = clutter_stage_get_view_at (stage, 512.f, 512.f);
  if (view)
    return view;

  return NULL;
}

static void
add_region_damage (MetaFuriosScreenCastStreamSrcMemfd *self,
                   const MtkRegion                    *region)
{
  if (!self->shared_header)
    return;

  if (!region) {
    MtkRectangle full = { 0, 0, (int) self->shared_header->width, (int) self->shared_header->height };
    add_damage_rect (self, &full);
    return;
  }

  int n = mtk_region_num_rectangles (region);
  if (n <= 0)
    return;

  for (int i = 0; i < n; i++) {
    MtkRectangle r = mtk_region_get_rectangle (region, i);
    add_damage_rect (self, &r);
  }
}

static void
on_after_paint (MetaStage        *stage,
                ClutterStageView *view,
                const MtkRegion  *redraw_clip,
                ClutterFrame     *frame,
                gpointer          user_data)
{
  (void) stage;
  (void) view;
  (void) frame;

  MetaFuriosScreenCastStreamSrcMemfd *self = user_data;

  self->stage_dirty = TRUE;
  add_region_damage (self, redraw_clip);
}

static void
ensure_stage_watch (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  MetaStage *meta_stage;
  ClutterStageView *view;

  if (self->watch_installed)
    return;
  if (!self->backend)
    return;
  if (!self->shared_header)
    return;

  meta_stage = META_STAGE (meta_backend_get_stage (self->backend));
  if (!meta_stage)
    return;

  view = get_any_stage_view (self);
  if (!view)
    return;

  self->paint_watch = meta_stage_watch_view (meta_stage,
                                             view,
                                             META_STAGE_WATCH_AFTER_PAINT,
                                             on_after_paint,
                                             self);

  self->stage_dirty = TRUE;
  clear_damage (self);

  /* force a full frame on first capture */
  MtkRectangle full = { 0, 0, (int) self->shared_header->width, (int) self->shared_header->height };
  add_damage_rect (self, &full);

  self->watch_installed = TRUE;
}

static inline guint8 *
slot_ptr (MetaFuriosScreenCastStreamSrcMemfd *self,
          guint                               slot)
{
  guint8 *base = (guint8 *) self->map;
  return base + (size_t) self->shared_header->header_bytes + (size_t) slot * (size_t) self->shared_header->slot_bytes;
}

static gboolean
paint_rect_into_dst (MetaFuriosScreenCastStreamSrcMemfd *self,
                     guint8                             *dst_full,
                     const MtkRectangle                 *rect,
                     GError                            **error)
{
  ClutterStage *stage;
  ClutterPaintFlag paint_flags;
  float scale = 1.0f;

  int stride = (int) self->shared_header->stride;

  stage = CLUTTER_STAGE (meta_backend_get_stage (self->backend));
  if (!stage) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to get ClutterStage from backend");
    return FALSE;
  }

  paint_flags = CLUTTER_PAINT_FLAG_NO_CURSORS;
  if (rect_is_full (self, rect))
    paint_flags |= CLUTTER_PAINT_FLAG_CLEAR;

  guint8 *dst_rect = dst_full + (size_t) rect->y * (size_t) stride + (size_t) rect->x * 4;

  if (!clutter_stage_paint_to_buffer (stage,
                                      rect,
                                      scale,
                                      dst_rect,
                                      stride,
                                      COGL_PIXEL_FORMAT_CAIRO_ARGB32_COMPAT,
                                      paint_flags,
                                      error))
    return FALSE;

  return TRUE;
}

static gboolean
should_emit_damage_for_rects (MetaFuriosScreenCastStreamSrcMemfd *self,
                              guint                               n_rects,
                              const MtkRectangle                 *rects)
{
  if (!self->shared_header)
    return FALSE;

  if (n_rects == 0)
    return FALSE;

  /* if any rect is full frame use FrameReady */
  for (guint i = 0; i < n_rects; i++) {
    if (rect_is_full (self, &rects[i]))
      return FALSE;
  }

  /* too many rects treat as "big" update */
  if (n_rects > DAMAGE_SIGNAL_MAX_RECTS)
    return FALSE;

  uint64_t frame_area = (uint64_t) self->shared_header->width * (uint64_t) self->shared_header->height;

  if (frame_area == 0)
    return FALSE;

  uint64_t damage_area = 0;
  for (guint i = 0; i < n_rects; i++) {
    const MtkRectangle *r = &rects[i];
    if (rect_is_empty (r))
      continue;
    damage_area += (uint64_t) r->width * (uint64_t) r->height;
    if (damage_area >= frame_area)
      break;
  }

  uint64_t threshold = (frame_area * (uint64_t) DAMAGE_SIGNAL_AREA_PERCENT) / 100u;

  /* if too much area use FrameReady */
  if (damage_area >= threshold)
    return FALSE;

  return TRUE;
}

static void
store_last_damage (MetaFuriosScreenCastStreamSrcMemfd *self,
                   guint                               n_rects,
                   const MtkRectangle                 *rects)
{
  (void) rects;

  self->last_damage_n = 0;
  self->last_should_emit_damage = FALSE;

  /* swap buffers so last_damage_rects points at the just-captured damage list */
  MtkRectangle *tmp = self->last_damage_rects;
  self->last_damage_rects = self->damage_rects;
  self->damage_rects = tmp;

  self->last_damage_n = MIN (n_rects, (guint) MAX_DAMAGE_RECTS);

  self->last_should_emit_damage = should_emit_damage_for_rects (self, self->last_damage_n, self->last_damage_rects);

  /* start fresh for next capture */
  self->damage_n = 0;
}

static gboolean
capture_primary_into_slot (MetaFuriosScreenCastStreamSrcMemfd *self,
                           guint                               slot,
                           GError                            **error)
{
  if (!self->map || self->map == MAP_FAILED) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "memfd not mapped");
    return FALSE;
  }

  if (!self->shared_header) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "memfd header not available");
    return FALSE;
  }

  int width = (int) self->shared_header->width;
  int height = (int) self->shared_header->height;
  int stride = (int) self->shared_header->stride;

  if (width <= 0 || height <= 0 || stride <= 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "invalid header geometry");
    return FALSE;
  }

  if (self->damage_n == 0) {
    MtkRectangle full = { 0, 0, width, height };
    add_damage_rect (self, &full);
  }

  guint8 *dst = slot_ptr (self, slot);

  for (guint i = 0; i < self->damage_n; i++) {
    MtkRectangle r = self->damage_rects[i];
    clamp_rect_to_buffer (self, &r);
    if (rect_is_empty (&r))
      continue;

    if (!paint_rect_into_dst (self, dst, &r, error))
      return FALSE;
  }

  return TRUE;
}

gboolean
meta_furios_screen_cast_stream_src_memfd_request_frame (MetaFuriosScreenCastStreamSrcMemfd *self,
                                                        guint                              *out_seq,
                                                        guint                              *out_slot,
                                                        GError                            **error)
{
  ensure_stage_watch (self);

  if (!self->map || self->map == MAP_FAILED) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "memfd not mapped");
    return FALSE;
  }

  if (!self->shared_header) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "memfd header not available");
    return FALSE;
  }

  if (!self->stage_dirty) {
    if (out_seq)
      *out_seq = __atomic_load_n (&self->shared_header->seq, __ATOMIC_ACQUIRE);
    if (out_slot)
      *out_slot = self->shared_header->last_slot;
    return TRUE;
  }

  self->stage_dirty = FALSE;

  guint32 slot = 0;

  if (!capture_primary_into_slot (self, slot, error)) {
    self->stage_dirty = TRUE;
    return FALSE;
  }

  store_last_damage (self, self->damage_n, self->damage_rects);
  self->seq++;

  self->shared_header->last_slot = slot;
  self->shared_header->pts_ns = (uint64_t) g_get_monotonic_time () * 1000ULL;

  __atomic_store_n (&self->shared_header->seq, self->seq, __ATOMIC_RELEASE);

  if (out_seq)
    *out_seq = self->seq;
  if (out_slot)
    *out_slot = slot;

  return TRUE;
}

static gboolean
tick_fps (gpointer data)
{
  MetaFuriosScreenCastStreamSrcMemfd *self = data;

  if (!self->connection || !self->object_path)
    return G_SOURCE_CONTINUE;

  meta_furios_screen_cast_stream_src_memfd_request_frame (self, NULL, NULL, NULL);

  return G_SOURCE_CONTINUE;
}

MetaFuriosScreenCastStreamSrcMemfd *
meta_furios_screen_cast_stream_src_memfd_new (MetaBackend  *backend,
                                              guint         width,
                                              guint         height,
                                              float         fps,
                                              GError      **error)
{
  g_return_val_if_fail (META_IS_BACKEND (backend), NULL);

  MetaFuriosScreenCastStreamSrcMemfd *self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST_STREAM_SRC_MEMFD, NULL);

  self->backend = backend;
  self->memfd = -1;
  self->map = NULL;
  self->map_len = 0;
  self->shared_header = NULL;

  self->seq = 0;
  self->tick_id = 0;

  self->paint_watch = NULL;
  self->watch_installed = FALSE;

  self->stage_dirty = TRUE;

  self->damage_rects = self->damage_rects_a;
  self->last_damage_rects = self->damage_rects_b;
  self->damage_n = 0;
  self->last_damage_n = 0;
  self->last_should_emit_damage = FALSE;

  const guint format = META_FURIOS_MEMFD_FORMAT_BGRX8888;
  const guint stride = width * 4;

  const guint n_slots = 1;

  const guint header_bytes = (guint) sizeof (MetaFuriosMemfdHeader);
  const guint slot_bytes = stride * height;
  const size_t total = (size_t) header_bytes + (size_t) n_slots * (size_t) slot_bytes;

  self->memfd = create_memfd ("furios-screencast-memfd");
  if (self->memfd < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "create_memfd failed: %s", g_strerror (errno));
    g_object_unref (self);
    return NULL;
  }

  if (ftruncate (self->memfd, (off_t) total) < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "ftruncate failed: %s", g_strerror (errno));
    g_object_unref (self);
    return NULL;
  }

  self->map_len = total;
  self->map = mmap (NULL, self->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, self->memfd, 0);
  if (self->map == MAP_FAILED) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "mmap failed: %s", g_strerror (errno));
    self->map = NULL;
    g_object_unref (self);
    return NULL;
  }

  self->shared_header = (MetaFuriosMemfdHeader *) self->map;
  memset (self->shared_header, 0, sizeof (*self->shared_header));

  self->shared_header->magic = META_FURIOS_MEMFD_MAGIC;
  self->shared_header->version = META_FURIOS_MEMFD_VERSION;
  self->shared_header->width = width;
  self->shared_header->height = height;
  self->shared_header->stride = stride;
  self->shared_header->format = format;
  self->shared_header->n_slots = n_slots;
  self->shared_header->slot_bytes = slot_bytes;
  self->shared_header->header_bytes = header_bytes;
  self->shared_header->last_slot = 0;
  self->shared_header->pts_ns = 0;

  self->seq = 0;
  __atomic_store_n (&self->shared_header->seq, self->seq, __ATOMIC_RELEASE);

  ensure_stage_watch (self);

  if (fps > 0.0f) {
    guint interval_ms;

    if (fps >= 1000.0f) {
      interval_ms = 1;
    } else {
      interval_ms = (guint) (1000.0f / fps);
      if (interval_ms == 0)
        interval_ms = 1;
    }

    self->tick_id = g_timeout_add (interval_ms, tick_fps, self);
  }

  return self;
}

int
meta_furios_screen_cast_stream_src_memfd_dup_fd (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  g_return_val_if_fail (self->memfd >= 0, -1);
  return dup (self->memfd);
}

void
meta_furios_screen_cast_stream_src_memfd_set_dbus (MetaFuriosScreenCastStreamSrcMemfd *self,
                                                   GDBusConnection                    *connection,
                                                   const char                         *object_path,
                                                   const char                         *peer_name)
{
  g_clear_object (&self->connection);
  self->connection = connection ? g_object_ref (connection) : NULL;

  g_free (self->object_path);
  self->object_path = g_strdup (object_path);

  g_free (self->peer_name);
  self->peer_name = g_strdup (peer_name);
}

gboolean
meta_furios_screen_cast_stream_src_memfd_get_last_damage (MetaFuriosScreenCastStreamSrcMemfd *self,
                                                          gboolean                           *out_should_emit_damage,
                                                          GVariant                          **out_damage)
{
  g_return_val_if_fail (META_IS_FURIOS_SCREEN_CAST_STREAM_SRC_MEMFD (self), FALSE);

  if (out_should_emit_damage)
    *out_should_emit_damage = self->last_should_emit_damage;

  if (!out_damage)
    return TRUE;

  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(iiii)"));

  for (guint i = 0; i < self->last_damage_n; i++) {
    const MtkRectangle *r = &self->last_damage_rects[i];
    if (rect_is_empty (r))
      continue;

    g_variant_builder_add (&b, "(iiii)", r->x, r->y, r->width, r->height);
  }

  *out_damage = g_variant_ref_sink (g_variant_builder_end (&b));
  return TRUE;
}

static void
meta_furios_screen_cast_stream_src_memfd_class_init (MetaFuriosScreenCastStreamSrcMemfdClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_stream_src_memfd_dispose;
}

static void
meta_furios_screen_cast_stream_src_memfd_init (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  self->backend = NULL;

  self->memfd = -1;
  self->map = NULL;
  self->map_len = 0;
  self->shared_header = NULL;

  self->seq = 0;
  self->tick_id = 0;

  self->paint_watch = NULL;
  self->watch_installed = FALSE;

  self->stage_dirty = TRUE;

  self->damage_rects = self->damage_rects_a;
  self->last_damage_rects = self->damage_rects_b;
  self->damage_n = 0;
  self->last_damage_n = 0;
  self->last_should_emit_damage = FALSE;

  self->connection = NULL;
  self->object_path = NULL;
  self->peer_name = NULL;
}
