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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "backends/meta-stage-private.h"
#include "backends/meta-backend-private.h"
#include "backends/meta-cursor-renderer.h"
#include "backends/meta-cursor-tracker-private.h"

#include "backends/meta-furios-screen-cast-stream-src-memfd.h"
#include "backends/meta-furios-screen-cast-memfd-protocol.h"

#include "cogl/cogl/cogl-context-private.h"

#define MAX_DAMAGE_RECTS 64
/* merge rects that touch/are within 1px */
#define DAMAGE_MERGE_PAD 1

#define DAMAGE_SIGNAL_MAX_RECTS 32
/* if more than 70% of frame then use FrameReady
 * this is finnicky and not an exact representative */
#define DAMAGE_SIGNAL_AREA_PERCENT 70

/* if damage list grows beyond this, collapse to bbox early */
#define DAMAGE_EARLY_BBOX_THRESHOLD 20

/* throttle update kicks (microseconds) */
#define STAGE_KICK_MIN_INTERVAL_US 16000  /* ~60Hz */

/* keep capturing briefly after a publish to avoid missing paints between requests */
#define ACTIVE_WINDOW_US 500000 /* 500ms */

enum
{
  FRAME_PUBLISHED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct _MetaFuriosGlFuncs
{
  PFNGLGENBUFFERSPROC glGenBuffers;
  PFNGLDELETEBUFFERSPROC glDeleteBuffers;
  PFNGLBINDBUFFERPROC glBindBuffer;
  PFNGLBUFFERDATAPROC glBufferData;
  PFNGLMAPBUFFERRANGEPROC glMapBufferRange;
  PFNGLMAPBUFFEROESPROC glMapBuffer;
  PFNGLUNMAPBUFFEROESPROC glUnmapBuffer;
  PFNGLPIXELSTOREIPROC glPixelStorei;
  PFNGLREADPIXELSPROC glReadPixels;
} MetaFuriosGlFuncs;

struct _MetaFuriosScreenCastStreamSrcMemfd
{
  GObject parent;

  MetaBackend *backend;

  int memfd;
  void *map;
  size_t map_len;

  MetaFuriosMemfdHeader *shared_header;

  guint32 seq;

  GDBusConnection *connection;
  char *object_path;
  char *peer_name;

  MetaStageWatch *paint_watch;
  gboolean watch_ready;

  guint stage_kick_idle_id;
  gint64 last_stage_kick_us;

  gboolean stage_dirty;

  guint damage_n;
  guint last_damage_n;

  /* damage arrays so we can store last damage with swaps */
  MtkRectangle damage_rects_a[MAX_DAMAGE_RECTS];
  MtkRectangle damage_rects_b[MAX_DAMAGE_RECTS];

  MtkRectangle *damage_rects;
  MtkRectangle *last_damage_rects;

  gboolean last_should_emit_damage;

  MetaCursorTracker *cursor_tracker;
  gulong cursor_changed_handler_id;
  gulong cursor_pos_invalidated_handler_id;

  gboolean last_cursor_rect_valid;
  graphene_rect_t last_cursor_rect;

  CoglContext *cogl_context;
  MetaFuriosGlFuncs gl;
  gboolean gl_funcs_inited;
  GLuint pbo_ids[2];
  size_t pbo_size;
  int pbo_write_index;
  gboolean pbo_have_previous;

  /* armed when RequestFrame is waiting */
  guint pending_requests;

  gint64 active_until_us;

  guint next_slot;
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

  /* if too many rects, prefer fewer stage paint calls */
  if (self->damage_n >= DAMAGE_EARLY_BBOX_THRESHOLD) {
    collapse_damage_to_bbox (self);
    rect_union_inplace (&self->damage_rects[0], &r);
    clamp_rect_to_buffer (self, &self->damage_rects[0]);
    self->damage_n = 1;
    return;
  }

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
ensure_cogl_context (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (self->cogl_context)
    return;

  ClutterBackend *cb = clutter_get_default_backend ();
  if (!cb)
    return;

  self->cogl_context = clutter_backend_get_cogl_context (cb);
}

static gboolean
ensure_gl_funcs (MetaFuriosScreenCastStreamSrcMemfd *self,
                 GError                            **error)
{
  if (self->gl_funcs_inited)
    return TRUE;

  self->gl.glGenBuffers = (PFNGLGENBUFFERSPROC) eglGetProcAddress ("glGenBuffers");
  self->gl.glDeleteBuffers = (PFNGLDELETEBUFFERSPROC) eglGetProcAddress ("glDeleteBuffers");
  self->gl.glBindBuffer = (PFNGLBINDBUFFERPROC) eglGetProcAddress ("glBindBuffer");
  self->gl.glBufferData = (PFNGLBUFFERDATAPROC) eglGetProcAddress ("glBufferData");
  self->gl.glMapBufferRange = (PFNGLMAPBUFFERRANGEPROC) eglGetProcAddress ("glMapBufferRange");
  self->gl.glMapBuffer = (PFNGLMAPBUFFEROESPROC) eglGetProcAddress ("glMapBufferOES");
  self->gl.glUnmapBuffer = (PFNGLUNMAPBUFFEROESPROC) eglGetProcAddress ("glUnmapBufferOES");
  self->gl.glPixelStorei = (PFNGLPIXELSTOREIPROC) eglGetProcAddress ("glPixelStorei");
  self->gl.glReadPixels = (PFNGLREADPIXELSPROC) eglGetProcAddress ("glReadPixels");

  if (!self->gl.glGenBuffers ||
      !self->gl.glDeleteBuffers ||
      !self->gl.glBindBuffer ||
      !self->gl.glBufferData ||
      !self->gl.glPixelStorei ||
      !self->gl.glReadPixels) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Required GL functions are not available");
    return FALSE;
  }

  self->gl_funcs_inited = TRUE;
  return TRUE;
}

static void
destroy_pbos (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (self->gl_funcs_inited &&
      self->gl.glDeleteBuffers &&
      (self->pbo_ids[0] || self->pbo_ids[1]))
    self->gl.glDeleteBuffers (2, self->pbo_ids);

  self->pbo_ids[0] = 0;
  self->pbo_ids[1] = 0;

  self->pbo_size = 0;
  self->pbo_write_index = 0;
  self->pbo_have_previous = FALSE;
}

static void
close_and_unmap (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (self->stage_kick_idle_id) {
    g_source_remove (self->stage_kick_idle_id);
    self->stage_kick_idle_id = 0;
  }

  if (self->cursor_tracker) {
    g_clear_signal_handler (&self->cursor_changed_handler_id, self->cursor_tracker);
    g_clear_signal_handler (&self->cursor_pos_invalidated_handler_id, self->cursor_tracker);
    self->cursor_tracker = NULL;
  }

  if (self->paint_watch) {
    MetaStage *meta_stage = NULL;
    ClutterActor *actor = meta_backend_get_stage (self->backend);

    if (actor)
      meta_stage = META_STAGE (actor);

    if (meta_stage) {
      meta_stage_remove_watch (meta_stage, self->paint_watch);
      self->paint_watch = NULL;
    }

    self->watch_ready = FALSE;
  }

  destroy_pbos (self);

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

static gboolean
stage_is_ready_for_kick (MetaFuriosScreenCastStreamSrcMemfd *self,
                         ClutterActor                       *stage_actor)
{
  (void) self;

  if (!stage_actor)
    return FALSE;
  if (!clutter_actor_is_realized (stage_actor))
    return FALSE;

  return TRUE;
}

static void
kick_stage_update (MetaFuriosScreenCastStreamSrcMemfd *self,
                   ClutterActor                      *actor)
{
  if (!actor)
    return;

  const gint64 now = g_get_monotonic_time ();
  if (self->last_stage_kick_us != 0 &&
      (now - self->last_stage_kick_us) < STAGE_KICK_MIN_INTERVAL_US)
    return;

  self->last_stage_kick_us = now;

  if (!clutter_actor_has_allocation (actor))
    clutter_actor_queue_relayout (actor);

  clutter_actor_queue_redraw (actor);
  clutter_stage_schedule_update (CLUTTER_STAGE (actor));
}

static gboolean
cursor_rect_from_renderer (MetaFuriosScreenCastStreamSrcMemfd *self,
                           graphene_rect_t                    *out_rect)
{
  MetaBackend *backend = self->backend;
  if (!backend || !self->shared_header)
    return FALSE;

  MetaCursorRenderer *cursor_renderer = meta_backend_get_cursor_renderer (backend);
  if (!cursor_renderer)
    return FALSE;

  ClutterCursor *cursor = meta_cursor_renderer_get_cursor (cursor_renderer);
  if (!cursor)
    return FALSE;

  graphene_rect_t rect =
    meta_cursor_renderer_calculate_rect (cursor_renderer, cursor);

  /* clamp to our stream bounds */
  graphene_rect_t bounds;
  bounds.origin.x = 0.f;
  bounds.origin.y = 0.f;
  bounds.size.width = (float) self->shared_header->width;
  bounds.size.height = (float) self->shared_header->height;

  graphene_rect_t clipped;
  if (!graphene_rect_intersection (&rect, &bounds, &clipped))
    return FALSE;

  if (clipped.size.width <= 0.f || clipped.size.height <= 0.f)
    return FALSE;

  *out_rect = clipped;
  return TRUE;
}

static void
add_damage_for_cursor_rect (MetaFuriosScreenCastStreamSrcMemfd *self,
                            const graphene_rect_t              *r)
{
  if (!self->shared_header || !r)
    return;

  /* avoid rounding artifacts */
  int x = (int) floorf (r->origin.x) - 1;
  int y = (int) floorf (r->origin.y) - 1;
  int w = (int) ceilf (r->origin.x + r->size.width) - x + 1;
  int h = (int) ceilf (r->origin.y + r->size.height) - y + 1;

  MtkRectangle mr = { x, y, w, h };
  clamp_rect_to_buffer (self, &mr);
  if (!rect_is_empty (&mr))
    add_damage_rect (self, &mr);
}

static void
sync_cursor_state_and_damage (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (!self->backend || !self->shared_header)
    return;

  if (!self->cursor_tracker)
    return;

  /* if compositor says cursor invisible, damage the old rect to erase it */
  if (!meta_cursor_tracker_get_pointer_visible (self->cursor_tracker)) {
    if (self->last_cursor_rect_valid) {
      add_damage_for_cursor_rect (self, &self->last_cursor_rect);
      self->last_cursor_rect_valid = FALSE;
      self->stage_dirty = TRUE;
    }
    return;
  }

  graphene_rect_t new_rect;
  gboolean have_new = cursor_rect_from_renderer (self, &new_rect);

  if (!have_new) {
    /* cursor may be outside bounds or missing texture. still erase old */
    if (self->last_cursor_rect_valid) {
      add_damage_for_cursor_rect (self, &self->last_cursor_rect);
      self->last_cursor_rect_valid = FALSE;
      self->stage_dirty = TRUE;
    }
    return;
  }

  /* damage old and new only if moved/changed rect */
  gboolean moved = FALSE;
  if (!self->last_cursor_rect_valid) {
    moved = TRUE;
  } else {
    const float eps = 0.01f;
    if (fabsf (self->last_cursor_rect.origin.x - new_rect.origin.x) > eps ||
        fabsf (self->last_cursor_rect.origin.y - new_rect.origin.y) > eps ||
        fabsf (self->last_cursor_rect.size.width - new_rect.size.width) > eps ||
        fabsf (self->last_cursor_rect.size.height - new_rect.size.height) > eps)
      moved = TRUE;
  }

  if (moved) {
    if (self->last_cursor_rect_valid)
      add_damage_for_cursor_rect (self, &self->last_cursor_rect);
    add_damage_for_cursor_rect (self, &new_rect);

    self->last_cursor_rect = new_rect;
    self->last_cursor_rect_valid = TRUE;
    self->stage_dirty = TRUE;
  } else {
    /* keep rect for future move detection */
    self->last_cursor_rect = new_rect;
    self->last_cursor_rect_valid = TRUE;
  }
}

static void
cursor_changed_cb (MetaCursorTracker *tracker,
                   gpointer           user_data)
{
  (void) tracker;
  MetaFuriosScreenCastStreamSrcMemfd *self = user_data;

  sync_cursor_state_and_damage (self);
}

static void
pointer_position_invalidated_cb (MetaCursorTracker *tracker,
                                 gpointer           user_data)
{
  (void) tracker;
  MetaFuriosScreenCastStreamSrcMemfd *self = user_data;

  sync_cursor_state_and_damage (self);
}

static void
ensure_cursor_tracking (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (!self->backend || self->cursor_tracker)
    return;

  self->cursor_tracker = meta_backend_get_cursor_tracker (self->backend);
  if (!self->cursor_tracker)
    return;

  self->cursor_pos_invalidated_handler_id = g_signal_connect_after (self->cursor_tracker,
                                                                    "position-invalidated",
                                                                    G_CALLBACK (pointer_position_invalidated_cb),
                                                                    self);

  self->cursor_changed_handler_id = g_signal_connect_after (self->cursor_tracker,
                                                            "cursor-changed",
                                                            G_CALLBACK (cursor_changed_cb),
                                                            self);

  self->last_cursor_rect_valid = FALSE;
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

  /* if too much area use FrameReady */
  uint64_t threshold = (frame_area * (uint64_t) DAMAGE_SIGNAL_AREA_PERCENT) / 100u;
  if (damage_area >= threshold)
    return FALSE;

  return TRUE;
}

static void
store_last_damage (MetaFuriosScreenCastStreamSrcMemfd *self,
                   guint                               n_rects)
{
  self->last_damage_n = 0;
  self->last_should_emit_damage = FALSE;

  MtkRectangle *tmp = self->last_damage_rects;
  self->last_damage_rects = self->damage_rects;
  self->damage_rects = tmp;

  self->last_damage_n = MIN (n_rects, (guint) MAX_DAMAGE_RECTS);
  self->last_should_emit_damage = should_emit_damage_for_rects (self, self->last_damage_n, self->last_damage_rects);

  self->damage_n = 0;
}

static void
publish_slot (MetaFuriosScreenCastStreamSrcMemfd *self,
              guint                               slot)
{
  self->seq++;

  self->shared_header->last_slot = (guint32) slot;
  self->shared_header->pts_ns = (uint64_t) g_get_monotonic_time () * 1000ULL;

  __atomic_store_n (&self->shared_header->seq, self->seq, __ATOMIC_RELEASE);

  /* keep hot briefly to avoid missing paints between requests */
  self->active_until_us = g_get_monotonic_time () + ACTIVE_WINDOW_US;

  g_signal_emit (self, signals[FRAME_PUBLISHED], 0, (guint) self->seq, (guint) slot);
}

static inline guint8 *
slot_ptr (MetaFuriosScreenCastStreamSrcMemfd *self,
          guint                               slot)
{
  guint8 *base = (guint8 *) self->map;
  return base + (size_t) self->shared_header->header_bytes +
         (size_t) slot * (size_t) self->shared_header->slot_bytes;
}

static gboolean
ensure_pbos (MetaFuriosScreenCastStreamSrcMemfd *self,
             size_t                              required_size,
             GError                            **error)
{
  ensure_cogl_context (self);

  if (!self->cogl_context) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No CoglContext available for PBO readback");
    return FALSE;
  }

  if (required_size == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid PBO size");
    return FALSE;
  }

  if (!ensure_gl_funcs (self, error))
    return FALSE;

  if (self->pbo_ids[0] == 0 && self->pbo_ids[1] == 0) {
    self->gl.glGenBuffers (2, self->pbo_ids);
    self->pbo_size = 0;
    self->pbo_write_index = 0;
    self->pbo_have_previous = FALSE;
  }

  if (self->pbo_size != required_size) {
    self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo_ids[0]);
    self->gl.glBufferData (GL_PIXEL_PACK_BUFFER,
                           (GLsizeiptr) required_size,
                           NULL,
                           GL_STREAM_READ);

    self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo_ids[1]);
    self->gl.glBufferData (GL_PIXEL_PACK_BUFFER,
                           (GLsizeiptr) required_size,
                           NULL,
                           GL_STREAM_READ);

    self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

    self->pbo_size = required_size;
    self->pbo_write_index = 0;
    self->pbo_have_previous = FALSE;
  }

  return TRUE;
}

static void
copy_pbo_to_memfd_bgra (MetaFuriosScreenCastStreamSrcMemfd *self,
                        const guint8                       *src,
                        guint8                             *dst_slot)
{
  const int width = (int) self->shared_header->width;
  const int height = (int) self->shared_header->height;
  const int dst_stride = (int) self->shared_header->stride;
  const int src_stride = width * 4;

  for (int y = 0; y < height; y++) {
    const guint8 *srow = src + (size_t) y * (size_t) src_stride;
    guint8 *drow = dst_slot + (size_t) y * (size_t) dst_stride;
    memcpy (drow, srow, (size_t) src_stride);
  }
}

static gboolean
pbo_readback_full_frame_into_slot (MetaFuriosScreenCastStreamSrcMemfd *self,
                                   guint                               slot,
                                   GError                            **error)
{
  const guint8 *src = NULL;

  if (!self->shared_header || !self->map || self->map == MAP_FAILED) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "memfd not mapped/header missing");
    return FALSE;
  }

  ensure_cogl_context (self);

  if (!self->cogl_context) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No CoglContext for readback");
    return FALSE;
  }

  ClutterStageView *view = get_any_stage_view (self);
  if (!view) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No stage view for readback");
    return FALSE;
  }

  CoglFramebuffer *fb = clutter_stage_view_get_framebuffer (view);
  if (!fb) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No stage view framebuffer for readback");
    return FALSE;
  }

  const int width = (int) self->shared_header->width;
  const int height = (int) self->shared_header->height;

  if (width <= 0 || height <= 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid capture size");
    return FALSE;
  }

  const size_t required = (size_t) width * (size_t) height * 4u;
  if (!ensure_pbos (self, required, error))
    return FALSE;

  self->gl.glPixelStorei (GL_PACK_ALIGNMENT, 1);

  const int write_i = self->pbo_write_index;
  const int read_i = 1 - write_i;

  self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo_ids[write_i]);

  cogl_framebuffer_flush (fb);

  self->gl.glReadPixels (0, 0, width, height,
                         GL_BGRA, GL_UNSIGNED_BYTE, (void *) 0);

  const int map_i = self->pbo_have_previous ? read_i : write_i;
  self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, self->pbo_ids[map_i]);

  if (self->gl.glMapBufferRange)
    src = (const guint8 *) self->gl.glMapBufferRange (GL_PIXEL_PACK_BUFFER,
                                                      0,
                                                      (GLsizeiptr) required,
                                                      GL_MAP_READ_BIT);
  else if (self->gl.glMapBuffer)
    src = (const guint8 *) self->gl.glMapBuffer (GL_PIXEL_PACK_BUFFER,
                                                 GL_READ_ONLY);

  if (!src) {
    self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to map PBO for readback");
    return FALSE;
  }

  guint8 *dst = slot_ptr (self, slot);

  copy_pbo_to_memfd_bgra (self, src, dst);

  if (self->gl.glUnmapBuffer)
    self->gl.glUnmapBuffer (GL_PIXEL_PACK_BUFFER);

  self->gl.glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

  self->pbo_have_previous = TRUE;
  self->pbo_write_index = read_i;

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
  (void) view;
  (void) frame;

  MetaFuriosScreenCastStreamSrcMemfd *self = user_data;

  add_region_damage (self, redraw_clip);

  ensure_cursor_tracking (self);
  sync_cursor_state_and_damage (self);

  const gint64 now = g_get_monotonic_time ();
  const gboolean hot = (self->active_until_us != 0 && now < self->active_until_us);
  const gboolean want_capture = (self->pending_requests != 0) || hot;

  /* no pending RequestFrame and not hot */
  if (!want_capture) {
    clear_damage (self);
    self->stage_dirty = FALSE;
    return;
  }

  if (!self->shared_header || !self->map || self->map == MAP_FAILED)
    return;

  /* do not publish if nothing changed */
  if (self->damage_n == 0) {
    self->stage_dirty = FALSE;
    return;
  }

  guint slot = self->next_slot;
  self->next_slot = (self->shared_header->n_slots > 1)
                  ? ((slot + 1) % self->shared_header->n_slots)
                  : 0;

  GError *local_error = NULL;
  const guint captured_damage_n = self->damage_n;

  if (!pbo_readback_full_frame_into_slot (self, slot, &local_error)) {
    if (local_error) {
      g_warning ("memfd screencast: PBO readback failed: %s", local_error->message);
      g_clear_error (&local_error);
    }
    self->stage_dirty = TRUE;
    return;
  }

  store_last_damage (self, captured_damage_n);

  self->pending_requests = 0;

  publish_slot (self, slot);

  self->stage_dirty = FALSE;
}

static void
ensure_stage_watch (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  MetaStage *meta_stage;
  ClutterStageView *view;

  if (self->watch_ready)
    return;
  if (!self->backend)
    return;
  if (!self->shared_header)
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

  self->paint_watch = meta_stage_watch_view (meta_stage,
                                             view,
                                             META_STAGE_WATCH_AFTER_PAINT,
                                             on_after_paint,
                                             self);

  self->stage_dirty = TRUE;
  clear_damage (self);

  MtkRectangle full = { 0, 0,
                        (int) self->shared_header->width,
                        (int) self->shared_header->height };
  add_damage_rect (self, &full);

  self->watch_ready = TRUE;

  ensure_cursor_tracking (self);
  sync_cursor_state_and_damage (self);

  ensure_cogl_context (self);
}

static gboolean
stage_kick_idle_cb (gpointer user_data)
{
  MetaFuriosScreenCastStreamSrcMemfd *self = user_data;

  if (!self || !self->backend) {
    if (self)
      self->stage_kick_idle_id = 0;
    return G_SOURCE_REMOVE;
  }

  ClutterActor *actor = meta_backend_get_stage (self->backend);
  if (!stage_is_ready_for_kick (self, actor))
    return G_SOURCE_CONTINUE;

  ensure_stage_watch (self);

  const guint cur_seq = __atomic_load_n (&self->shared_header->seq, __ATOMIC_ACQUIRE);

  /* only kick when bootstrapping or when we think stage is dirty */
  if (cur_seq == 0 || self->stage_dirty)
    kick_stage_update (self, actor);

  /* keep running until we actually publish at least one frame */
  if (__atomic_load_n (&self->shared_header->seq, __ATOMIC_ACQUIRE) == 0)
    return G_SOURCE_CONTINUE;

  self->stage_kick_idle_id = 0;
  return G_SOURCE_REMOVE;
}

static void
ensure_stage_kick_scheduled (MetaFuriosScreenCastStreamSrcMemfd *self)
{
  if (self->stage_kick_idle_id != 0)
    return;

  self->stage_kick_idle_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, stage_kick_idle_cb, self, NULL);
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

  ensure_cursor_tracking (self);
  sync_cursor_state_and_damage (self);

  self->pending_requests = 1;

  /*
   * if we never published yet, force full-frame damage and keep poking until
   * we get an after-paint to publish.
   */
  const guint cur_seq = __atomic_load_n (&self->shared_header->seq, __ATOMIC_ACQUIRE);
  if (cur_seq == 0) {
    clear_damage (self);
    MtkRectangle full = { 0, 0,
                          (int) self->shared_header->width,
                          (int) self->shared_header->height };
    add_damage_rect (self, &full);
    self->stage_dirty = TRUE;
  }

  ClutterActor *actor = meta_backend_get_stage (self->backend);

  /*
   * always schedule an idle kicker so we don't deadlock if the stage isn't
   * allocated/mapped yet at the moment of the request.
   */
  ensure_stage_kick_scheduled (self);

  /* attempt immediate kick only if realized and we are dirty/bootstrapping */
  if (stage_is_ready_for_kick (self, actor) && (self->stage_dirty || cur_seq == 0))
    kick_stage_update (self, actor);

  if (out_seq)
    *out_seq = __atomic_load_n (&self->shared_header->seq, __ATOMIC_ACQUIRE);
  if (out_slot)
    *out_slot = self->shared_header->last_slot;

  return TRUE;
}

MetaFuriosScreenCastStreamSrcMemfd *
meta_furios_screen_cast_stream_src_memfd_new (MetaBackend  *backend,
                                              guint         width,
                                              guint         height,
                                              float         fps,
                                              GError      **error)
{
  (void) fps;

  g_return_val_if_fail (META_IS_BACKEND (backend), NULL);

  MetaFuriosScreenCastStreamSrcMemfd *self = g_object_new (META_TYPE_FURIOS_SCREEN_CAST_STREAM_SRC_MEMFD, NULL);

  self->backend = backend;
  self->memfd = -1;
  self->map = NULL;
  self->map_len = 0;
  self->shared_header = NULL;

  self->seq = 0;

  self->paint_watch = NULL;
  self->watch_ready = FALSE;
  self->stage_kick_idle_id = 0;
  self->last_stage_kick_us = 0;

  self->stage_dirty = TRUE;

  self->damage_rects = self->damage_rects_a;
  self->last_damage_rects = self->damage_rects_b;
  self->damage_n = 0;
  self->last_damage_n = 0;
  self->last_should_emit_damage = FALSE;

  self->cursor_tracker = NULL;
  self->cursor_changed_handler_id = 0;
  self->cursor_pos_invalidated_handler_id = 0;
  self->last_cursor_rect_valid = FALSE;

  self->cogl_context = NULL;
  memset (&self->gl, 0, sizeof (self->gl));
  self->gl_funcs_inited = FALSE;
  self->pbo_ids[0] = 0;
  self->pbo_ids[1] = 0;
  self->pbo_size = 0;
  self->pbo_write_index = 0;
  self->pbo_have_previous = FALSE;

  self->pending_requests = 0;
  self->active_until_us = 0;
  self->next_slot = 0;

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

  const int H = self->shared_header ? (int) self->shared_header->height : 0;

  for (guint i = 0; i < self->last_damage_n; i++) {
    const MtkRectangle *r = &self->last_damage_rects[i];
    if (rect_is_empty (r))
      continue;

    int x = r->x;
    int y = r->y;
    int w = r->width;
    int h = r->height;

    if (H > 0)
      y = H - (y + h);

    g_variant_builder_add (&b, "(iiii)", x, y, w, h);
  }

  *out_damage = g_variant_ref_sink (g_variant_builder_end (&b));
  return TRUE;
}

static void
meta_furios_screen_cast_stream_src_memfd_class_init (MetaFuriosScreenCastStreamSrcMemfdClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = meta_furios_screen_cast_stream_src_memfd_dispose;

  signals[FRAME_PUBLISHED] = g_signal_new ("frame-published",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL, NULL, NULL,
                                           G_TYPE_NONE,
                                           2,
                                           G_TYPE_UINT,
                                           G_TYPE_UINT);
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

  self->paint_watch = NULL;
  self->watch_ready = FALSE;
  self->stage_kick_idle_id = 0;
  self->last_stage_kick_us = 0;

  self->stage_dirty = TRUE;

  self->damage_rects = self->damage_rects_a;
  self->last_damage_rects = self->damage_rects_b;
  self->damage_n = 0;
  self->last_damage_n = 0;
  self->last_should_emit_damage = FALSE;

  self->connection = NULL;
  self->object_path = NULL;
  self->peer_name = NULL;

  self->cursor_tracker = NULL;
  self->cursor_changed_handler_id = 0;
  self->cursor_pos_invalidated_handler_id = 0;
  self->last_cursor_rect_valid = FALSE;

  self->cogl_context = NULL;
  memset (&self->gl, 0, sizeof (self->gl));
  self->gl_funcs_inited = FALSE;
  self->pbo_ids[0] = 0;
  self->pbo_ids[1] = 0;
  self->pbo_size = 0;
  self->pbo_write_index = 0;
  self->pbo_have_previous = FALSE;

  self->pending_requests = 0;
  self->active_until_us = 0;
  self->next_slot = 0;
}
