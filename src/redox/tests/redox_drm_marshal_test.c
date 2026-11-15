/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * Checks the payloads src/redox/drm builds against CLIENT-SCHEME.md (version 1), with a
 * transport that records the call and plays the daemon. No scheme, no daemon: runs
 * anywhere. On Linux:
 *   cc -I src/redox/drm -I include/drm-uapi -I include \
 *      src/redox/tests/redox_drm_marshal_test.c src/redox/drm/redox_drm.c -o t && ./t
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "asahi_drm.h"
#include "redox_drm.h"

static int failures;
#define CHECK(cond)                                                                      \
   do {                                                                                  \
      if (!(cond)) {                                                                     \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
         failures++;                                                                     \
      }                                                                                  \
   } while (0)

/* What the fake daemon saw, and what it answers. */
static struct {
   int fd;
   uint64_t nr;
   size_t len;
   uint8_t payload[4096];
   unsigned calls;
   ssize_t result;                          /* returned to the caller */
   void (*reply)(uint8_t *payload, size_t len); /* edits the payload in place */
   int fail_first;                          /* that many calls return -EINTR first */
} rec;

static ssize_t
fake_transport(int fd, uint64_t nr, void *payload, size_t len)
{
   rec.calls++;
   if (rec.fail_first > 0) {
      rec.fail_first--;
      return -EINTR;
   }
   rec.fd = fd;
   rec.nr = nr;
   rec.len = len;
   CHECK(len <= sizeof(rec.payload));
   memcpy(rec.payload, payload, len);
   if (rec.result >= 0 && rec.reply)
      rec.reply(payload, len);
   return rec.result;
}

static void
reset(void)
{
   memset(&rec, 0, sizeof(rec));
}

static uint32_t
rd32(size_t off)
{
   uint32_t v;
   memcpy(&v, rec.payload + off, 4);
   return v;
}

static uint64_t
rd64(size_t off)
{
   uint64_t v;
   memcpy(&v, rec.payload + off, 8);
   return v;
}

/* ---- replies ---- */

static void
reply_version(uint8_t *p, size_t len)
{
   struct redox_drm_agx_version v = {.major = 1, .minor = 7, .flags = 1};
   strcpy(v.name, "asahi");
   CHECK(len == sizeof(v));
   memcpy(p, &v, sizeof(v));
}

static void
reply_params(uint8_t *p, size_t len)
{
   /* the area starts at 24; fill it with a pattern */
   for (size_t i = 24; i < len; i++)
      p[i] = (uint8_t)(i - 24 + 1);
}

static void
reply_gem_create(uint8_t *p, size_t len)
{
   struct drm_asahi_gem_create g;
   CHECK(len == sizeof(g));
   memcpy(&g, p, sizeof(g));
   g.handle = 42;
   memcpy(p, &g, sizeof(g));
}

static void
reply_wait(uint8_t *p, size_t len)
{
   struct drm_syncobj_wait w;
   memcpy(&w, p, sizeof(w));
   w.first_signaled = 2;
   w.handles = 0; /* a daemon may scribble on pointer fields: the caller's must survive */
   memcpy(p, &w, sizeof(w));
   (void)len;
}

static void
reply_query(uint8_t *p, size_t len)
{
   /* 24 + 3 handles (12) -> points at 40 */
   CHECK(len == 40 + 3 * 8);
   for (unsigned i = 0; i < 3; i++) {
      uint64_t v = 100 + i;
      memcpy(p + 40 + 8 * i, &v, 8);
   }
}

static void
reply_scribble_bind(uint8_t *p, size_t len)
{
   memset(p, 0xee, len);
}

int
main(void)
{
   redox_drm_set_transport(fake_transport);

   /* Request numbers are Linux's. */
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_GET_PARAMS) == 0x40);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_GET_TIME) == 0x41);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_VM_CREATE) == 0x42);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_VM_DESTROY) == 0x43);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_VM_BIND) == 0x44);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_GEM_CREATE) == 0x45);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET) == 0x46);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_GEM_BIND_OBJECT) == 0x47);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_QUEUE_CREATE) == 0x48);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_QUEUE_DESTROY) == 0x49);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_SUBMIT) == 0x4a);
   CHECK(_IOC_NR(DRM_IOCTL_ASAHI_GEM_MADVISE) == 0x4b);
   CHECK(_IOC_NR(DRM_IOCTL_GEM_CLOSE) == 0x09);
   CHECK(_IOC_NR(DRM_IOCTL_SYNCOBJ_CREATE) == 0xbf);
   CHECK(_IOC_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL) == 0xcd);

   /* Structure sizes the scheme document states. */
   CHECK(sizeof(struct drm_gem_close) == 8);
   CHECK(sizeof(struct drm_asahi_get_time) == 16);
   CHECK(sizeof(struct drm_asahi_vm_create) == 24);
   CHECK(sizeof(struct drm_asahi_vm_destroy) == 8);
   CHECK(sizeof(struct drm_asahi_gem_create) == 24);
   CHECK(sizeof(struct drm_asahi_gem_mmap_offset) == 16);
   CHECK(sizeof(struct drm_asahi_gem_bind_object) == 40);
   CHECK(sizeof(struct drm_asahi_queue_create) == 40);
   CHECK(sizeof(struct drm_asahi_queue_destroy) == 8);
   CHECK(sizeof(struct drm_asahi_gem_madvise) == 16);
   CHECK(sizeof(struct drm_syncobj_create) == 8);
   CHECK(sizeof(struct drm_syncobj_destroy) == 8);
   CHECK(sizeof(struct drm_syncobj_transfer) == 32);
   CHECK(sizeof(struct drm_asahi_gem_bind_op) == 32);

   /* VERSION: 32 bytes, nr 0; drmGetVersion names the driver. */
   reset();
   rec.reply = reply_version;
   drmVersionPtr ver = drmGetVersion(7);
   CHECK(ver != NULL);
   if (ver) {
      CHECK(rec.fd == 7 && rec.nr == 0 && rec.len == 32);
      CHECK(!strcmp(ver->name, "asahi") && ver->name_len == 5);
      CHECK(ver->version_major == 1 && ver->version_minor == 7);
      drmFreeVersion(ver);
   }
   {
      /* and through the ioctl, as Linux's two-step protocol */
      char name[8] = {0};
      struct drm_version v = {0};
      CHECK(drmIoctl(7, DRM_IOCTL_VERSION, &v) == 0 && v.name_len == 5);
      v.name = name;
      v.name_len = sizeof(name) - 1;
      CHECK(drmIoctl(7, DRM_IOCTL_VERSION, &v) == 0);
      CHECK(!strcmp(name, "asahi") && v.version_major == 1);
   }

   /* A plain structure, written back: GEM_CREATE. */
   reset();
   rec.reply = reply_gem_create;
   {
      struct drm_asahi_gem_create g = {.size = 0x8000, .flags = 1, .vm_id = 3};
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_GEM_CREATE, &g) == 0);
      CHECK(rec.nr == 0x45 && rec.len == 24);
      CHECK(rd64(0) == 0x8000 && rd32(8) == 1 && rd32(12) == 3);
      CHECK(g.handle == 42);
   }

   /* A write-only structure is not written back: GEM_CLOSE. */
   reset();
   rec.reply = reply_scribble_bind;
   {
      struct drm_gem_close c = {.handle = 9};
      CHECK(drmIoctl(5, DRM_IOCTL_GEM_CLOSE, &c) == 0);
      CHECK(rec.nr == 0x09 && rec.len == 8 && rd32(0) == 9);
      CHECK(c.handle == 9);
   }

   /* GET_PARAMS: 24 + size; the area goes in and comes back. */
   reset();
   rec.reply = reply_params;
   {
      uint8_t area[20];
      memset(area, 0x5a, sizeof(area));
      struct drm_asahi_get_params p = {
         .param_group = 1,
         .pointer = (uintptr_t)area,
         .size = sizeof(area),
      };
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_GET_PARAMS, &p) == 0);
      CHECK(rec.nr == 0x40 && rec.len == 24 + 20);
      CHECK(rd32(0) == 1 && rd64(16) == 20);
      CHECK(rec.payload[24] == 0x5a && rec.payload[43] == 0x5a); /* in */
      CHECK(area[0] == 1 && area[19] == 20);                     /* out */
      CHECK(p.pointer == (uintptr_t)area && p.size == 20);
   }

   /* VM_BIND: 24 + num_binds * stride, in only (the caller's records may be const). */
   reset();
   rec.reply = reply_scribble_bind;
   {
      struct drm_asahi_gem_bind_op ops[3];
      memset(ops, 0, sizeof(ops));
      for (unsigned i = 0; i < 3; i++) {
         ops[i].flags = DRM_ASAHI_BIND_READ;
         ops[i].handle = 10 + i;
         ops[i].offset = 0;
         ops[i].range = 0x4000;
         ops[i].addr = 0x100000000ull + 0x4000 * i;
      }
      struct drm_asahi_gem_bind_op copy[3];
      memcpy(copy, ops, sizeof(ops));
      struct drm_asahi_vm_bind b = {
         .vm_id = 2,
         .num_binds = 3,
         .stride = sizeof(ops[0]),
         .userptr = (uintptr_t)ops,
      };
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_VM_BIND, &b) == 0);
      CHECK(rec.nr == 0x44 && rec.len == 24 + 3 * 32);
      CHECK(rd32(0) == 2 && rd32(4) == 3 && rd32(8) == 32);
      CHECK(!memcmp(rec.payload + 24, copy, sizeof(copy)));
      CHECK(!memcmp(ops, copy, sizeof(copy)));       /* not written back */
      CHECK(b.vm_id == 2 && b.userptr == (uintptr_t)ops);
   }

   /* SUBMIT: 40 + syncs, then the command buffer at the next multiple of 8. */
   reset();
   {
      struct drm_asahi_sync syncs[3] = {
         {.sync_type = DRM_ASAHI_SYNC_SYNCOBJ, .handle = 1},
         {.sync_type = DRM_ASAHI_SYNC_SYNCOBJ, .handle = 2},
         {.sync_type = DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ, .handle = 3, .timeline_value = 77},
      };
      uint8_t cmd[13];
      for (unsigned i = 0; i < sizeof(cmd); i++)
         cmd[i] = 0xc0 + i;
      struct drm_asahi_submit s = {
         .syncs = (uintptr_t)syncs,
         .cmdbuf = (uintptr_t)cmd,
         .queue_id = 4,
         .in_sync_count = 2,
         .out_sync_count = 1,
         .cmdbuf_size = sizeof(cmd),
      };
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_SUBMIT, &s) == 0);
      CHECK(rec.nr == 0x4a && rec.len == 40 + 48 + 13);
      CHECK(rd32(20) == 4 && rd32(24) == 2 && rd32(28) == 1 && rd32(32) == 13);
      CHECK(!memcmp(rec.payload + 40, syncs, sizeof(syncs)));
      CHECK(!memcmp(rec.payload + 88, cmd, sizeof(cmd)));

      /* no syncs: the command buffer directly at 40 */
      reset();
      s.in_sync_count = s.out_sync_count = 0;
      s.syncs = 0;
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_SUBMIT, &s) == 0);
      CHECK(rec.len == 40 + 13 && !memcmp(rec.payload + 40, cmd, sizeof(cmd)));
   }

   /* SYNCOBJ_WAIT: 40 + count * 4; first_signaled comes back, pointers survive. */
   reset();
   rec.reply = reply_wait;
   {
      uint32_t handles[3] = {11, 12, 13}, first = 99;
      CHECK(drmSyncobjWait(5, handles, 3, 123456789, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                           &first) == 0);
      CHECK(rec.nr == 0xc3 && rec.len == 40 + 12);
      CHECK(rd64(8) == 123456789 && rd32(16) == 3 && rd32(20) == 2);
      CHECK(rd32(40) == 11 && rd32(44) == 12 && rd32(48) == 13);
      CHECK(first == 2);
   }

   /* A timeout is ETIME, returned negative by the wait wrappers. */
   reset();
   rec.result = -ETIME;
   {
      uint32_t h = 1;
      CHECK(drmSyncobjWait(5, &h, 1, 0, 0, NULL) == -ETIME);
      uint64_t pt = 5;
      CHECK(drmSyncobjTimelineWait(5, &h, &pt, 1, 0, 0, NULL) == -ETIME);
   }

   /* TIMELINE_WAIT: 48 + handles, points at the next multiple of 8. */
   reset();
   {
      uint32_t handles[3] = {21, 22, 23};
      uint64_t points[3] = {5, 6, 7};
      CHECK(drmSyncobjTimelineWait(5, handles, points, 3, 1000, 0, NULL) == 0);
      CHECK(rec.nr == 0xca && rec.len == 64 + 24);
      CHECK(rd32(48) == 21 && rd32(56) == 23);
      CHECK(rd64(64) == 5 && rd64(80) == 7);
   }

   /* QUERY: points out. TIMELINE_SIGNAL: points in. */
   reset();
   rec.reply = reply_query;
   {
      uint32_t handles[3] = {1, 2, 3};
      uint64_t points[3] = {0, 0, 0};
      CHECK(drmSyncobjQuery(5, handles, points, 3) == 0);
      CHECK(rec.nr == 0xcb && rec.len == 64);
      CHECK(points[0] == 100 && points[2] == 102);
      reset();
      points[1] = 55;
      CHECK(drmSyncobjTimelineSignal(5, handles, points, 3) == 0);
      CHECK(rec.nr == 0xcd && rec.len == 64 && rd64(48) == 55);
   }

   /* RESET / SIGNAL / TRANSFER / CREATE / DESTROY. */
   reset();
   {
      uint32_t handles[2] = {8, 9};
      CHECK(drmSyncobjReset(5, handles, 2) == 0 && rec.nr == 0xc4 && rec.len == 24);
      CHECK(drmSyncobjSignal(5, handles, 2) == 0 && rec.nr == 0xc5 && rd32(20) == 9);
      CHECK(drmSyncobjTransfer(5, 2, 20, 1, 10, 0) == 0);
      CHECK(rec.nr == 0xcc && rec.len == 32);
      CHECK(rd32(0) == 1 && rd32(4) == 2 && rd64(8) == 10 && rd64(16) == 20);
      uint32_t h = 0;
      CHECK(drmSyncobjCreate(5, DRM_SYNCOBJ_CREATE_SIGNALED, &h) == 0);
      CHECK(rec.nr == 0xbf && rec.len == 8 && rd32(4) == 1);
      CHECK(drmSyncobjDestroy(5, 6) == 0 && rec.nr == 0xc0 && rd32(0) == 6);
   }

   /* A structure shorter than the daemon's is zero-extended, as Linux's DRM core does:
    * Mesa sends QUEUE_CREATE without its last 16 bytes when no ray-query layout is asked. */
   reset();
   rec.reply = reply_scribble_bind;
   {
      struct drm_asahi_queue_create q = {.vm_id = 1, .priority = 1, .usc_exec_base = 1ull << 32};
      q.render_scs_layout[0] = 0x1111; /* beyond the short request: must not be sent or touched */
      unsigned long request = _IOC(_IOC_DIR(DRM_IOCTL_ASAHI_QUEUE_CREATE), DRM_IOCTL_BASE, 0x48,
                                   offsetof(struct drm_asahi_queue_create, render_scs_layout));
      CHECK(drmIoctl(5, request, &q) == 0);
      CHECK(rec.nr == 0x48 && rec.len == 40);
      CHECK(rd32(4) == 1 && rd64(16) == 1ull << 32 && rd64(24) == 0 && rd64(32) == 0);
      CHECK(q.queue_id == 0xeeeeeeee && q.render_scs_layout[0] == 0x1111);
   }

   /* Errors: errno is the daemon's, the result -1; EINTR is retried. */
   reset();
   rec.result = -ENOMEM;
   {
      struct drm_asahi_gem_create g = {.size = 1};
      errno = 0;
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_GEM_CREATE, &g) == -1 && errno == ENOMEM);
      CHECK(g.handle == 0);
   }
   reset();
   rec.fail_first = 2;
   {
      struct drm_asahi_get_time t = {0};
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_GET_TIME, &t) == 0 && rec.calls == 3);
   }

   /* Not DRM (dma-buf ioctls reach drmIoctl in Mesa): refused here, nothing sent. */
   reset();
   {
      int dummy = 0;
      errno = 0;
      CHECK(drmIoctl(5, _IOWR('b', 2, int), &dummy) == -1 && errno == ENOTTY);
      CHECK(rec.calls == 0);
   }

   /* Sync files: not in the scheme, refused without a call. */
   reset();
   {
      int fd = -1;
      errno = 0;
      CHECK(drmSyncobjExportSyncFile(5, 1, &fd) == -1);
      CHECK(drmSyncobjImportSyncFile(5, 1, 3) == -1);
      CHECK(rec.calls == 0);
   }

   /* PRIME by token (scheme version 1.1, section 7): export is call 0x2d with the
    * 32-byte record, flags as Linux's numbers; a version 1.0 daemon's EOPNOTSUPP
    * comes back as errno and no buffer fd is opened. Import of a descriptor that
    * is not a buffer fd fails before any call. */
   reset();
   rec.result = -EOPNOTSUPP;
   {
      int fd = -1;
      uint32_t h;
      errno = 0;
      CHECK(drmPrimeHandleToFD(5, 7, DRM_CLOEXEC | DRM_RDWR, &fd) == -1 && errno == EOPNOTSUPP);
      CHECK(fd == -1);
      CHECK(rec.calls == 1 && rec.fd == 5 && rec.nr == 0x2d && rec.len == 32);
      CHECK(rd32(0) == 7 && rd32(4) == (0x80000 | 0x2) && rd32(8) == 0xffffffff);
      CHECK(rd32(12) == 0 && rd64(16) == 0 && rd64(24) == 0);
      CHECK(drmPrimeHandleToFD(5, 7, 0x4, &fd) == -1 && errno == EINVAL);
      CHECK(drmPrimeFDToHandle(5, 0, &h) == -1);
      CHECK(rec.calls == 1);
   }

   /* GET_CAP: the two syncobj answers are local, anything else asks (0x0c). */
   reset();
   {
      uint64_t v = 0;
      CHECK(drmGetCap(5, DRM_CAP_SYNCOBJ, &v) == 0 && v == 1 && rec.calls == 0);
      rec.result = -EOPNOTSUPP;
      errno = 0;
      CHECK(drmGetCap(5, DRM_CAP_PRIME, &v) == -1 && errno == EOPNOTSUPP);
      CHECK(rec.calls == 1 && rec.nr == 0x0c && rec.len == 16 && rd64(0) == DRM_CAP_PRIME);
   }

   /* Absurd counts never become an allocation. */
   reset();
   {
      struct drm_asahi_vm_bind b = {.num_binds = 0xffffffff, .stride = 0xffffffff};
      errno = 0;
      CHECK(drmIoctl(5, DRM_IOCTL_ASAHI_VM_BIND, &b) == -1 && errno == EINVAL);
      CHECK(rec.calls == 0);
   }

   printf(failures ? "redox_drm_marshal_test: %d FAILED\n" : "redox_drm_marshal_test: ok\n",
          failures);
   return failures != 0;
}
