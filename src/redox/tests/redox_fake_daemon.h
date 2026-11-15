/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * A daemon played inside the test process: a transport for src/redox/drm that decodes
 * each payload as CLIENT-SCHEME.md (version 1) lays it out and answers as a G15S (M3 Pro)
 * kernel would. Buffer memory is a temporary file that is also the "card" fd, so Mesa's
 * mmap of the device fd is a real mmap. Nothing renders: a submitted job "completes" at
 * once (its out syncs signal) and is recorded.
 */
#ifndef REDOX_FAKE_DAEMON_H
#define REDOX_FAKE_DAEMON_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "drm-uapi/asahi_drm.h"
#include "redox_drm.h"

#define FAKE_MAX 4096
#define FAKE_MAX_SUBMITS 64

static struct {
   int file;            /* backs every buffer object; also the "card" fd */
   uint64_t file_size;
   struct {
      uint64_t size, file_off;
      bool live;
   } gem[FAKE_MAX];
   uint32_t num_gems;
   struct {
      bool live, signaled;
      uint64_t point;
   } sync[FAKE_MAX];
   uint32_t num_syncs;
   uint32_t num_vms, num_queues, num_objects;
   unsigned calls[256];
   unsigned binds, unbinds, submits, bad;
   /* the submissions, as they arrived on the wire */
   struct {
      uint32_t queue, in_syncs, out_syncs, cmdbuf_size;
      uint64_t cmdbuf_fnv1a;
      unsigned renders, computes, others;
   } submit[FAKE_MAX_SUBMITS];
} fake;

/* drm-shim's params_g15s (T6030 as the M3 kernel reports it), with the feature word the
 * real G15 backend and apple-agxd give (0x3e, the private wire's numbering). */
static const struct drm_asahi_params_global fake_params = {
   .features = 0x3e,
   .gpu_generation = 15,
   .gpu_variant = 'S',
   .gpu_revision = 0x11,
   .chip_id = 0x6030,
   .num_dies = 1,
   .num_clusters_total = 2,
   .num_cores_per_cluster = 10,
   .core_masks = {0x1ff, 0x1ff},
   .vm_start = 0x4000,
   .vm_end = 0x2ffffff8000ull,
   .vm_kernel_min_size = 0x20000000,
   .max_commands_per_submission = 64,
   .max_attachments = 16,
   .max_frequency_khz = 1398000,
   .command_timestamp_frequency_hz = 1000000000,
   .usc_generation = 3,
   .gpu_hal_generation = DRM_ASAHI_GPU_HAL_LEGACY,
};

static uint64_t
fake_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define AT(type, name, off)                                                              \
   type name;                                                                            \
   if (len < (off) + sizeof(type))                                                       \
      return -EINVAL;                                                                    \
   memcpy(&name, p + (off), sizeof(type))
#define PUT(name, off) memcpy(p + (off), &name, sizeof(name))

static ssize_t
fake_transport(int fd, uint64_t nr, void *payload, size_t len)
{
   uint8_t *p = payload;

   if (fd != fake.file || nr > 0xff) {
      fake.bad++;
      return -EBADF;
   }
   fake.calls[nr]++;

   switch (nr) {
   case 0x00: {
      struct redox_drm_agx_version v = {.major = 1, .minor = 0, .flags = 1};
      strcpy(v.name, "asahi");
      if (len != sizeof(v))
         return -EINVAL;
      PUT(v, 0);
      return 0;
   }
   case 0x09: {
      AT(struct drm_gem_close, c, 0);
      if (!c.handle || c.handle > fake.num_gems || !fake.gem[c.handle].live)
         return -EINVAL;
      fake.gem[c.handle].live = false;
      return 0;
   }
   case 0x40: {
      AT(struct drm_asahi_get_params, g, 0);
      if (len != 24 + g.size)
         return -EINVAL;
      if (g.param_group == 0) {
         size_t n = g.size < sizeof(fake_params) ? g.size : sizeof(fake_params);
         memset(p + 24, 0, g.size);
         memcpy(p + 24, &fake_params, n);
         return 0;
      } else if (g.param_group == DRM_ASAHI_PARAM_GROUP_VM_STATUS) {
         AT(struct drm_asahi_vm_status, st, 24);
         if (!st.vm_id || st.vm_id > fake.num_vms)
            return -ENOENT;
         st.error = 0;
         PUT(st, 24);
         return 0;
      } else if (g.param_group == DRM_ASAHI_PARAM_GROUP_QUEUE_LIMITS) {
         struct drm_asahi_queue_limits lim = {.max_queues = 58, .max_in_flight_per_queue = 2};
         if (g.size < sizeof(lim))
            return -EINVAL;
         PUT(lim, 24);
         return 0;
      }
      return -EOPNOTSUPP;
   }
   case 0x41: {
      AT(struct drm_asahi_get_time, t, 0);
      t.gpu_timestamp = fake_now_ns();
      PUT(t, 0);
      return 0;
   }
   case 0x42: {
      AT(struct drm_asahi_vm_create, v, 0);
      if (v.kernel_end <= v.kernel_start ||
          v.kernel_end - v.kernel_start < fake_params.vm_kernel_min_size ||
          v.kernel_end > fake_params.vm_end || (v.kernel_start & 0x3fff))
         return -EINVAL;
      v.vm_id = ++fake.num_vms;
      PUT(v, 0);
      return 0;
   }
   case 0x43:
      return 0;
   case 0x44: {
      AT(struct drm_asahi_vm_bind, b, 0);
      if (b.stride < sizeof(struct drm_asahi_gem_bind_op) || (b.stride & 7) ||
          len != 24 + (size_t)b.num_binds * b.stride || !b.vm_id || b.vm_id > fake.num_vms)
         return -EINVAL;
      for (uint32_t i = 0; i < b.num_binds; i++) {
         AT(struct drm_asahi_gem_bind_op, op, 24 + (size_t)i * b.stride);
         if ((op.addr | op.range | op.offset) & 0x3fff)
            return -EINVAL;
         if (op.flags & DRM_ASAHI_BIND_UNBIND) {
            fake.unbinds++;
            continue;
         }
         if (!op.handle || op.handle > fake.num_gems || !fake.gem[op.handle].live ||
             op.offset + op.range > fake.gem[op.handle].size)
            return -EINVAL;
         if (op.addr < fake_params.vm_start || op.addr + op.range > fake_params.vm_end)
            return -EINVAL;
         fake.binds++;
      }
      return 0;
   }
   case 0x45: {
      AT(struct drm_asahi_gem_create, g, 0);
      if (!g.size || fake.num_gems + 1 >= FAKE_MAX)
         return -ENOMEM;
      uint64_t size = (g.size + 0x3fff) & ~0x3fffull;
      uint32_t h = ++fake.num_gems;
      fake.gem[h].size = size;
      fake.gem[h].file_off = fake.file_size;
      fake.gem[h].live = true;
      fake.file_size += size;
      if (ftruncate(fake.file, (off_t)fake.file_size))
         return -ENOMEM;
      g.size = size;
      g.handle = h;
      PUT(g, 0);
      return 0;
   }
   case 0x46: {
      AT(struct drm_asahi_gem_mmap_offset, m, 0);
      if (!m.handle || m.handle > fake.num_gems || !fake.gem[m.handle].live)
         return -ENOENT;
      /* The offset is the daemon's to choose; Mesa passes it to mmap unchanged. The real
       * daemon gives handle << 32, this one the object's place in its backing file. */
      m.offset = fake.gem[m.handle].file_off;
      PUT(m, 0);
      return 0;
   }
   case 0x47: {
      AT(struct drm_asahi_gem_bind_object, o, 0);
      if (o.op == DRM_ASAHI_BIND_OBJECT_OP_BIND) {
         o.object_handle = ++fake.num_objects;
         PUT(o, 0);
      }
      return 0;
   }
   case 0x48: {
      AT(struct drm_asahi_queue_create, q, 0);
      if (!q.vm_id || q.vm_id > fake.num_vms || (q.usc_exec_base & 0xffffffffull) ||
          q.render_scs_layout[0] || q.render_scs_layout[1])
         return -EINVAL;
      q.queue_id = ++fake.num_queues;
      PUT(q, 0);
      return 0;
   }
   case 0x49:
      return 0;
   case 0x4a: {
      AT(struct drm_asahi_submit, s, 0);
      size_t nsync = (size_t)s.in_sync_count + s.out_sync_count;
      size_t cmd_off = (40 + nsync * 16 + 7) & ~(size_t)7;
      if (len != cmd_off + s.cmdbuf_size || !s.queue_id || s.queue_id > fake.num_queues)
         return -EINVAL;
      /* the job "completes" at once: signal the out syncs */
      for (size_t i = s.in_sync_count; i < nsync; i++) {
         AT(struct drm_asahi_sync, y, 40 + i * 16);
         if (!y.handle || y.handle > fake.num_syncs || !fake.sync[y.handle].live)
            return -ENOENT;
         fake.sync[y.handle].signaled = true;
         if (y.sync_type == DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ)
            fake.sync[y.handle].point = y.timeline_value;
      }
      if (fake.submits < FAKE_MAX_SUBMITS) {
         const uint8_t *cmd = p + cmd_off;
         uint64_t h = 0xcbf29ce484222325ull;
         for (size_t i = 0; i < s.cmdbuf_size; i++)
            h = (h ^ cmd[i]) * 0x100000001b3ull;
         fake.submit[fake.submits].queue = s.queue_id;
         fake.submit[fake.submits].in_syncs = s.in_sync_count;
         fake.submit[fake.submits].out_syncs = s.out_sync_count;
         fake.submit[fake.submits].cmdbuf_size = s.cmdbuf_size;
         fake.submit[fake.submits].cmdbuf_fnv1a = h;
         /* walk the commands: header (type, size) + payload */
         for (size_t at = 0; at + sizeof(struct drm_asahi_cmd_header) <= s.cmdbuf_size;) {
            struct drm_asahi_cmd_header hd;
            memcpy(&hd, cmd + at, sizeof(hd));
            if (hd.cmd_type == DRM_ASAHI_CMD_RENDER)
               fake.submit[fake.submits].renders++;
            else if (hd.cmd_type == DRM_ASAHI_CMD_COMPUTE)
               fake.submit[fake.submits].computes++;
            else
               fake.submit[fake.submits].others++;
            at += sizeof(hd) + hd.size;
         }
      }
      fake.submits++;
      return 0;
   }
   case 0x4b: {
      AT(struct drm_asahi_gem_madvise, m, 0);
      m.retained = 1;
      PUT(m, 0);
      return 0;
   }
   case 0xbf: {
      AT(struct drm_syncobj_create, c, 0);
      if (fake.num_syncs + 1 >= FAKE_MAX)
         return -ENOMEM;
      c.handle = ++fake.num_syncs;
      fake.sync[c.handle].live = true;
      fake.sync[c.handle].signaled = c.flags & DRM_SYNCOBJ_CREATE_SIGNALED;
      PUT(c, 0);
      return 0;
   }
   case 0xc0: {
      AT(struct drm_syncobj_destroy, d, 0);
      if (!d.handle || d.handle > fake.num_syncs || !fake.sync[d.handle].live)
         return -ENOENT;
      fake.sync[d.handle].live = false;
      return 0;
   }
   case 0xc3: {
      AT(struct drm_syncobj_wait, w, 0);
      if (len != 40 + (size_t)w.count_handles * 4)
         return -EINVAL;
      bool all = w.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, any = false, every = true;
      for (uint32_t i = 0; i < w.count_handles; i++) {
         AT(uint32_t, h, 40 + (size_t)i * 4);
         if (!h || h > fake.num_syncs || !fake.sync[h].live)
            return -ENOENT;
         if (fake.sync[h].signaled) {
            if (!any)
               w.first_signaled = i;
            any = true;
         } else {
            every = false;
         }
      }
      if (all ? !every : !any)
         return -ETIME;
      PUT(w, 0);
      return 0;
   }
   case 0xc4:
   case 0xc5: {
      AT(struct drm_syncobj_array, a, 0);
      if (len != 16 + (size_t)a.count_handles * 4)
         return -EINVAL;
      for (uint32_t i = 0; i < a.count_handles; i++) {
         AT(uint32_t, h, 16 + (size_t)i * 4);
         if (!h || h > fake.num_syncs || !fake.sync[h].live)
            return -ENOENT;
         fake.sync[h].signaled = nr == 0xc5;
      }
      return 0;
   }
   case 0xcc: {
      AT(struct drm_syncobj_transfer, t, 0);
      if (!t.src_handle || t.src_handle > fake.num_syncs || !fake.sync[t.src_handle].live ||
          !t.dst_handle || t.dst_handle > fake.num_syncs || !fake.sync[t.dst_handle].live)
         return -ENOENT;
      /* jobs complete at once here, so "not signaled" means "no fence": Linux's EINVAL */
      if (!fake.sync[t.src_handle].signaled)
         return -EINVAL;
      fake.sync[t.dst_handle].signaled = true;
      return 0;
   }
   default:
      return -EOPNOTSUPP;
   }
}

static int
fake_open(void)
{
   char path[256];
   const char *dir = getenv("TMPDIR");
   snprintf(path, sizeof(path), "%s/agxdev_fake_XXXXXX", dir && dir[0] ? dir : "/tmp");
   memset(&fake, 0, sizeof(fake));
   fake.file = mkstemp(path);
   if (fake.file < 0)
      return -1;
   unlink(path);
   /* object 0 never exists: keep offset 0 unused too */
   fake.file_size = 0x4000;
   if (ftruncate(fake.file, (off_t)fake.file_size))
      return -1;
   redox_drm_set_transport(fake_transport);
   return fake.file;
}

#undef AT
#undef PUT

#endif
