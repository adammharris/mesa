/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * Drives Mesa's asahi device layer (agx_open_device, buffer objects, mappings, a command
 * queue, sync objects, the GPU clock) through src/redox/drm.
 *
 *   agxdev_test --fake    against a daemon played inside this process: the transport of
 *                         src/redox/drm is replaced by one that decodes each payload as
 *                         CLIENT-SCHEME.md lays it out and answers as a G15S (M3 Pro)
 *                         kernel would; buffer memory is a temporary file, so Mesa's
 *                         mmap of the "card" fd is a real mmap. Runs anywhere.
 *   agxdev_test           against the scheme: $REDOX_DRM_CARD or /scheme/gpu.agx/card
 *                         (the real daemon, or a stub scheme). Redox only.
 *   agxdev_test --raw     the scheme again, but only the calls a minimal stub answers:
 *                         VERSION, GEM_CREATE, GEM_MMAP_OFFSET + mmap, GET_TIME, GEM_CLOSE,
 *                         sync objects. No Mesa device.
 *
 * Exit status 0 when every check passed.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "xf86drm.h"
#include "drm-uapi/asahi_drm.h"
#include "redox_drm.h"

#include "util/ralloc.h"
#include "agx_bo.h"
#include "agx_device.h"

static int failures;
#define CHECK(cond)                                                                      \
   do {                                                                                  \
      if (!(cond)) {                                                                     \
         printf("FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno);        \
         failures++;                                                                     \
      }                                                                                  \
   } while (0)

/* ------------------------------------------------------------- the fake daemon */

#define FAKE_MAX 4096

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
now_ns(void)
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
      t.gpu_timestamp = now_ns();
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

/* ------------------------------------------------------------------ the checks */

static void
check_syncobjs(int fd)
{
   uint32_t a = 0, b = 0, first = 99;
   CHECK(drmSyncobjCreate(fd, 0, &a) == 0 && a != 0);
   CHECK(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &b) == 0 && b != 0 && b != a);

   uint32_t both[2] = {a, b};
   /* a has no fence: a poll (timeout 0) of it alone must not succeed */
   int r = drmSyncobjWait(fd, &a, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
   CHECK(r == -ETIME);
   r = drmSyncobjWait(fd, both, 2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &first);
   CHECK(r == 0 && first == 1);
   CHECK(drmSyncobjSignal(fd, &a, 1) == 0);
   r = drmSyncobjWait(fd, both, 2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
   CHECK(r == 0);
   CHECK(drmSyncobjReset(fd, &a, 1) == 0);
   r = drmSyncobjWait(fd, &a, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
   CHECK(r == -ETIME);
   CHECK(drmSyncobjDestroy(fd, a) == 0);
   CHECK(drmSyncobjDestroy(fd, b) == 0);
   errno = 0;
   CHECK(drmSyncobjDestroy(fd, b) == -1 && errno == ENOENT);
   printf("sync objects: create, wait (poll, any, all), signal, reset, destroy\n");
}

static int
run_raw(int fd)
{
   drmVersionPtr v = drmGetVersion(fd);
   CHECK(v != NULL);
   if (!v)
      return 1;
   printf("version: %s %d.%d (%s)\n", v->name, v->version_major, v->version_minor, v->desc);
   CHECK(!strcmp(v->name, "asahi"));
   drmFreeVersion(v);

   struct drm_asahi_get_time t0 = {0}, t1 = {0};
   CHECK(drmIoctl(fd, DRM_IOCTL_ASAHI_GET_TIME, &t0) == 0);
   CHECK(drmIoctl(fd, DRM_IOCTL_ASAHI_GET_TIME, &t1) == 0);
   CHECK(t0.gpu_timestamp != 0 && t1.gpu_timestamp >= t0.gpu_timestamp);
   printf("gpu time: %llu ns, then +%llu\n", (unsigned long long)t0.gpu_timestamp,
          (unsigned long long)(t1.gpu_timestamp - t0.gpu_timestamp));

   struct drm_asahi_gem_create g = {.size = 0x6000};
   CHECK(drmIoctl(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &g) == 0);
   CHECK(g.handle != 0 && g.size == 0x8000);
   struct drm_asahi_gem_mmap_offset m = {.handle = g.handle};
   CHECK(drmIoctl(fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &m) == 0);
   printf("gem: handle %u size %#llx mmap offset %#llx\n", g.handle,
          (unsigned long long)g.size, (unsigned long long)m.offset);

   /* two mappings of the same object must be the same memory */
   uint32_t *p = mmap(NULL, g.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
   uint32_t *q = mmap(NULL, g.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
   CHECK(p != MAP_FAILED && q != MAP_FAILED);
   if (p != MAP_FAILED && q != MAP_FAILED) {
      CHECK(p[0] == 0 && p[g.size / 4 - 1] == 0); /* new objects are zeroed */
      for (unsigned i = 0; i < g.size / 4; i++)
         p[i] = 0xa5000000u + i;
      bool same = true;
      for (unsigned i = 0; i < g.size / 4; i++)
         same &= q[i] == 0xa5000000u + i;
      CHECK(same);
      printf("mmap: two mappings of the object share %llu bytes: %s\n",
             (unsigned long long)g.size, same ? "yes" : "NO");
      munmap(p, g.size);
      munmap(q, g.size);
   }

   struct drm_gem_close c = {.handle = g.handle};
   CHECK(drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c) == 0);
   errno = 0;
   CHECK(drmIoctl(fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &m) == -1 && errno == ENOENT);

   /* a number the daemon does not have */
   struct drm_get_cap cap = {.capability = DRM_CAP_SYNCOBJ};
   errno = 0;
   CHECK(drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap) == -1 && errno == EOPNOTSUPP);

   check_syncobjs(fd);
   return 0;
}

static int
run_device(int fd, bool is_fake)
{
   void *memctx = ralloc_context(NULL);
   struct agx_device *dev = rzalloc(memctx, struct agx_device);
   dev->fd = fd;

   if (!agx_open_device(memctx, dev)) {
      printf("FAIL: agx_open_device\n");
      return 1;
   }
   printf("device: %s, chip id %#x, G%u%c, %u cores, features %#llx, g15 wire %d, vm %u\n",
          dev->name, dev->params.chip_id, dev->params.gpu_generation,
          (char)dev->params.gpu_variant, agx_get_num_cores(dev),
          (unsigned long long)dev->params.features, dev->g15_legacy_wire, dev->vm_id);
   CHECK(dev->chip == AGX_CHIP_G15S);
   CHECK(dev->g15_legacy_wire);
   CHECK(dev->vm_id != 0);

   /* buffer objects: allocate, map, write, read; several sizes */
   static const size_t sizes[] = {0x4000, 0x8000, 0x100000, 0x3000};
   struct agx_bo *bos[ARRAY_SIZE(sizes)];
   for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
      bos[i] = agx_bo_create(dev, sizes[i], 0, 0, "agxdev_test");
      CHECK(bos[i] != NULL);
      if (!bos[i])
         continue;
      uint32_t *map = agx_bo_map(bos[i]);
      CHECK(map != NULL && map != MAP_FAILED);
      CHECK(bos[i]->va && bos[i]->va->addr != 0);
      for (size_t w = 0; w < sizes[i] / 4; w++)
         map[w] = (uint32_t)(i << 28 | w);
   }
   bool intact = true;
   for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
      if (!bos[i])
         continue;
      uint32_t *map = agx_bo_map(bos[i]);
      for (size_t w = 0; w < sizes[i] / 4; w++)
         intact &= map[w] == (uint32_t)(i << 28 | w);
      printf("bo %u: size %#zx handle %u gpu va %#llx\n", i, bos[i]->size, bos[i]->handle,
             (unsigned long long)bos[i]->va->addr);
   }
   CHECK(intact);

   uint32_t q = agx_create_command_queue(dev, DRM_ASAHI_PRIORITY_MEDIUM);
   CHECK(q != UINT32_MAX && q != 0);
   printf("queue: %u\n", q);

   uint64_t t0 = agx_get_gpu_timestamp(dev), t1 = agx_get_gpu_timestamp(dev);
   CHECK(t0 != 0 && t1 >= t0);
   printf("gpu timestamp: %llu, then +%llu\n", (unsigned long long)t0,
          (unsigned long long)(t1 - t0));

   check_syncobjs(dev->fd);

   if (q != UINT32_MAX)
      CHECK(agx_destroy_command_queue(dev, q) == 0);
   for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
      if (bos[i])
         agx_bo_unreference(dev, bos[i]);
   }
   agx_close_device(dev);
   ralloc_free(memctx);

   if (is_fake) {
      static const struct {
         uint8_t nr;
         const char *name;
      } names[] = {
         {0x00, "VERSION"},      {0x40, "GET_PARAMS"},  {0x42, "VM_CREATE"},
         {0x45, "GEM_CREATE"},   {0x46, "GEM_MMAP_OFFSET"}, {0x44, "VM_BIND"},
         {0x47, "GEM_BIND_OBJECT"}, {0x48, "QUEUE_CREATE"}, {0x49, "QUEUE_DESTROY"},
         {0x41, "GET_TIME"},     {0x4b, "GEM_MADVISE"}, {0x09, "GEM_CLOSE"},
         {0x43, "VM_DESTROY"},   {0xbf, "SYNCOBJ_CREATE"}, {0xc3, "SYNCOBJ_WAIT"},
         {0x4a, "SUBMIT"},
      };
      printf("calls the fake daemon decoded:");
      for (unsigned i = 0; i < ARRAY_SIZE(names); i++)
         printf(" %s=%u", names[i].name, fake.calls[names[i].nr]);
      printf("; binds %u unbinds %u, objects %u, file %llu bytes\n", fake.binds, fake.unbinds,
             fake.num_gems, (unsigned long long)fake.file_size);
      CHECK(fake.bad == 0);
      CHECK(fake.calls[0x42] == 1 && fake.calls[0x45] >= 4 && fake.calls[0x46] >= 4);
      CHECK(fake.binds >= 4);
   }
   return 0;
}

int
main(int argc, char **argv)
{
   bool is_fake = argc > 1 && !strcmp(argv[1], "--fake");
   bool raw = argc > 1 && !strcmp(argv[1], "--raw");
   int fd = is_fake ? fake_open() : redox_drm_open();

   if (fd < 0) {
      printf("FAIL: cannot open the device: %s\n", strerror(errno));
      return 1;
   }

   /* The raw checks hold against the fake as well; they go first because closing the
    * device closes the fd. */
   int r = (raw || is_fake) ? run_raw(fd) : 0;
   if (!raw && !r)
      r = run_device(fd, is_fake);
   printf(failures || r ? "agxdev_test: FAILED (%d checks)\n" : "agxdev_test: ok\n", failures);
   return failures || r;
}
