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
 *   agxdev_test --quit-stub   asks the stub scheme (fake-agxd) to exit: call number 0xfe.
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

#include "redox_fake_daemon.h"

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
   if (argc > 1 && !strcmp(argv[1], "--quit-stub")) {
      int q = drmIoctl(fd, DRM_IO(0xfe), NULL);
      printf("quit-stub: %s\n", q ? strerror(errno) : "ok");
      return q != 0;
   }

   int r = (raw || is_fake) ? run_raw(fd) : 0;
   if (!raw && !r)
      r = run_device(fd, is_fake);
   printf(failures || r ? "agxdev_test: FAILED (%d checks)\n" : "agxdev_test: ok\n", failures);
   return failures || r;
}
