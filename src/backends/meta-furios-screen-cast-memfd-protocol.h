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

#pragma once

#define META_FURIOS_MEMFD_MAGIC 0x46555249u
#define META_FURIOS_MEMFD_VERSION 1u

typedef enum
{
  META_FURIOS_MEMFD_FORMAT_RGBA8888 = 1,
  META_FURIOS_MEMFD_FORMAT_BGRA8888 = 2,
  META_FURIOS_MEMFD_FORMAT_BGRX8888 = 3,
} MetaFuriosMemfdFormat;

typedef struct __attribute__((packed))
{
  uint32_t magic;        /* META_FURIOS_MEMFD_MAGIC */
  uint32_t version;      /* META_FURIOS_MEMFD_VERSION */

  uint32_t width;
  uint32_t height;
  uint32_t stride;       /* bytes per row */
  uint32_t format;       /* MetaFuriosMemfdFormat */

  uint32_t n_slots;      /* ring size */
  uint32_t slot_bytes;   /* stride * height */
  uint32_t header_bytes; /* sizeof(MetaFuriosMemfdHeader) */
  uint32_t reserved0;

  /* updated per frame */
  uint32_t last_slot;    /* slot index last written */
  uint32_t seq;          /* increments each frame */
  uint64_t pts_ns;       /* monotonic pts */
  uint64_t reserved1;
} MetaFuriosMemfdHeader;
