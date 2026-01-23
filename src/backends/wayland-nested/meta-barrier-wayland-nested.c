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

#include "meta/barrier.h"

#include "backends/wayland-nested/meta-barrier-wayland-nested.h"

struct _MetaBarrierImplWaylandNested
{
  MetaBarrierImpl parent;
  MetaBarrier *barrier;
};

G_DEFINE_TYPE (MetaBarrierImplWaylandNested,
               meta_barrier_impl_wayland_nested,
               META_TYPE_BARRIER_IMPL)

static gboolean
meta_barrier_impl_wayland_nested_is_active (MetaBarrierImpl *impl)
{
  (void) impl;
  return FALSE;
}

static void
meta_barrier_impl_wayland_nested_release (MetaBarrierImpl  *impl,
                                          MetaBarrierEvent *event)
{
  (void) impl;
  (void) event;
}

static void
meta_barrier_impl_wayland_nested_destroy (MetaBarrierImpl *impl)
{
  (void) impl;
}

MetaBarrierImpl *
meta_barrier_impl_wayland_nested_new (MetaBarrier *barrier)
{
  MetaBarrierImplWaylandNested *self;

  self = g_object_new (META_TYPE_BARRIER_IMPL_WAYLAND_NESTED, NULL);
  self->barrier = barrier;

  return META_BARRIER_IMPL (self);
}

static void
meta_barrier_impl_wayland_nested_class_init (MetaBarrierImplWaylandNestedClass *klass)
{
  MetaBarrierImplClass *impl_class = META_BARRIER_IMPL_CLASS (klass);

  impl_class->is_active = meta_barrier_impl_wayland_nested_is_active;
  impl_class->release = meta_barrier_impl_wayland_nested_release;
  impl_class->destroy = meta_barrier_impl_wayland_nested_destroy;
}

static void
meta_barrier_impl_wayland_nested_init (MetaBarrierImplWaylandNested *self)
{
  (void) self;
}
