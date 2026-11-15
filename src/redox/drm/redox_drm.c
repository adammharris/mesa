/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * libdrm for Redox (see redox_drm.h). Every DRM ioctl becomes one SYS_CALL on the GPU
 * daemon's scheme fd: metadata[0] is the ioctl number, the payload is the Linux structure
 * followed by the memory its user pointers name (CLIENT-SCHEME.md, "Pointers").
 *
 * relibc's ioctl() must not be used for these: it takes every 'd' request for its own
 * display-scheme translation (relibc src/header/sys_ioctl/redox/drm.rs).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xf86drm.h"
#include "asahi_drm.h"
#include "redox_drm.h"

#define ALIGN8(x) (((x) + 7) & ~(size_t)7)
#define MIN2(a, b) ((a) < (b) ? (a) : (b))

/* The pointer-carrying structures must be laid out as the daemon expects them. */
_Static_assert(sizeof(struct drm_asahi_get_params) == 24, "get_params");
_Static_assert(sizeof(struct drm_asahi_vm_bind) == 24, "vm_bind");
_Static_assert(sizeof(struct drm_asahi_submit) == 40, "submit");
_Static_assert(sizeof(struct drm_asahi_sync) == 16, "sync");
_Static_assert(sizeof(struct drm_syncobj_wait) == 40, "syncobj_wait");
_Static_assert(sizeof(struct drm_syncobj_timeline_wait) == 48, "timeline_wait");
_Static_assert(sizeof(struct drm_syncobj_array) == 16, "syncobj_array");
_Static_assert(sizeof(struct drm_syncobj_timeline_array) == 24, "timeline_array");
_Static_assert(sizeof(struct redox_drm_agx_version) == 32, "agx_version");

/* ------------------------------------------------------------------ transport */

#ifdef __redox__
/* relibc exports this (libredox ABI); no header declares it. */
extern uintptr_t redox_sys_call_v0(uintptr_t fd, uint8_t *payload, size_t payload_len,
                                   uintptr_t flags, const uint64_t *metadata,
                                   size_t metadata_len);
/* syscall::CallFlags. Without both, relibc sends a call with no payload at all. */
#define REDOX_CALL_WRITE (1u << 9)
#define REDOX_CALL_READ  (1u << 10)

static ssize_t
default_transport(int fd, uint64_t nr, void *payload, size_t len)
{
   uint64_t metadata[1] = {nr};
   /* Errors come back as -errno in the result (Error::mux). */
   return (ssize_t)redox_sys_call_v0((uintptr_t)fd, payload, len,
                                     REDOX_CALL_READ | REDOX_CALL_WRITE, metadata, 1);
}
#else
static ssize_t
default_transport(int fd, uint64_t nr, void *payload, size_t len)
{
   (void)fd; (void)nr; (void)payload; (void)len;
   return -ENOSYS;
}
#endif

static const char *card_path(void);

static redox_drm_transport_fn transport = default_transport;

void
redox_drm_set_transport(redox_drm_transport_fn fn)
{
   transport = fn ? fn : default_transport;
}

static ssize_t
call_retry(int fd, uint64_t nr, void *payload, size_t len)
{
   ssize_t r;
   do {
      r = transport(fd, nr, payload, len);
   } while (r == -EINTR || r == -EAGAIN);
   return r;
}

int
redox_drm_open(void)
{
   return open(card_path(), O_RDWR | O_CLOEXEC);
}

/* ---------------------------------------------------------------- marshalling */

/* The size of each request's structure as the daemon has it (CLIENT-SCHEME.md's table).
 * Linux's DRM core lets user space pass a shorter or longer structure than the kernel's
 * (it zero-extends or truncates), and Mesa relies on that: QUEUE_CREATE without ray-query
 * layouts is sent 24 bytes long. The payload always carries the daemon's size. */
static size_t
wire_struct_size(uint8_t nr, size_t caller_size)
{
   switch (nr) {
   case 0x09: return 8;  /* GEM_CLOSE */
   case 0x40: return 24; /* GET_PARAMS */
   case 0x41: return 16; /* GET_TIME */
   case 0x42: return 24; /* VM_CREATE */
   case 0x43: return 8;  /* VM_DESTROY */
   case 0x44: return 24; /* VM_BIND */
   case 0x45: return 24; /* GEM_CREATE */
   case 0x46: return 16; /* GEM_MMAP_OFFSET */
   case 0x47: return 40; /* GEM_BIND_OBJECT */
   case 0x48: return 40; /* QUEUE_CREATE */
   case 0x49: return 8;  /* QUEUE_DESTROY */
   case 0x4a: return 40; /* SUBMIT */
   case 0x4b: return 16; /* GEM_MADVISE */
   case 0xbf: return 8;  /* SYNCOBJ_CREATE */
   case 0xc0: return 8;  /* SYNCOBJ_DESTROY */
   case 0xc3: return 40; /* SYNCOBJ_WAIT */
   case 0xc4: return 16; /* SYNCOBJ_RESET */
   case 0xc5: return 16; /* SYNCOBJ_SIGNAL */
   case 0xca: return 48; /* SYNCOBJ_TIMELINE_WAIT */
   case 0xcb: return 24; /* SYNCOBJ_QUERY */
   case 0xcc: return 32; /* SYNCOBJ_TRANSFER */
   case 0xcd: return 24; /* SYNCOBJ_TIMELINE_SIGNAL */
   default:   return caller_size;
   }
}

static bool
add_array(struct redox_drm_layout *l, size_t ptr_off, uint64_t count, uint64_t elem,
          bool in, bool out)
{
   /* Bound every area: a request larger than this is a caller bug, not a job. */
   if (elem && count > (uint64_t)(64u << 20) / elem)
      return false;
   struct redox_drm_array *a = &l->arrays[l->num_arrays++];
   a->ptr_off = ptr_off;
   a->bytes = (size_t)(count * elem);
   a->payload_off = ALIGN8(l->total);
   a->in = in;
   a->out = out;
   l->total = a->payload_off + a->bytes;
   return true;
}

int
redox_drm_layout(unsigned long request, const void *arg, struct redox_drm_layout *l)
{
   memset(l, 0, sizeof(*l));
   if (_IOC_TYPE(request) != DRM_IOCTL_BASE)
      return -ENOTTY;

   l->nr = _IOC_NR(request);
   l->struct_size = _IOC_SIZE(request);
   l->struct_out = (_IOC_DIR(request) & _IOC_READ) != 0;
   l->wire_struct_size = wire_struct_size(l->nr, l->struct_size);
   l->total = l->wire_struct_size;
   if (l->struct_size && !arg)
      return -EFAULT;

   /* The caller's structure, zero-extended as the DRM core would. */
   union {
      uint8_t bytes[64];
      uint64_t align;
   } ext = {{0}};
   memcpy(ext.bytes, arg, MIN2(l->struct_size, sizeof(ext.bytes)));

   bool ok = true;
#define NEED(type)                                                                       \
   _Static_assert(sizeof(type) <= sizeof(ext), #type);                                   \
   const type *s = (const type *)ext.bytes
#define OFF(type, field) offsetof(type, field)

   switch (l->nr) {
   case DRM_COMMAND_BASE + DRM_ASAHI_GET_PARAMS: {
      NEED(struct drm_asahi_get_params);
      /* In for the groups that carry a request (VM status: vm_id), out for all. */
      ok = add_array(l, OFF(struct drm_asahi_get_params, pointer), s->size, 1, true, true);
      break;
   }
   case DRM_COMMAND_BASE + DRM_ASAHI_VM_BIND: {
      NEED(struct drm_asahi_vm_bind);
      ok = add_array(l, OFF(struct drm_asahi_vm_bind, userptr), s->num_binds, s->stride,
                     true, false);
      break;
   }
   case DRM_COMMAND_BASE + DRM_ASAHI_SUBMIT: {
      NEED(struct drm_asahi_submit);
      ok = add_array(l, OFF(struct drm_asahi_submit, syncs),
                     (uint64_t)s->in_sync_count + s->out_sync_count,
                     sizeof(struct drm_asahi_sync), true, false) &&
           add_array(l, OFF(struct drm_asahi_submit, cmdbuf), s->cmdbuf_size, 1, true,
                     false);
      break;
   }
   case 0xc3: { /* SYNCOBJ_WAIT */
      NEED(struct drm_syncobj_wait);
      ok = add_array(l, OFF(struct drm_syncobj_wait, handles), s->count_handles, 4, true,
                     false);
      break;
   }
   case 0xc4: /* SYNCOBJ_RESET */
   case 0xc5: { /* SYNCOBJ_SIGNAL */
      NEED(struct drm_syncobj_array);
      ok = add_array(l, OFF(struct drm_syncobj_array, handles), s->count_handles, 4, true,
                     false);
      break;
   }
   case 0xca: { /* SYNCOBJ_TIMELINE_WAIT */
      NEED(struct drm_syncobj_timeline_wait);
      ok = add_array(l, OFF(struct drm_syncobj_timeline_wait, handles), s->count_handles,
                     4, true, false) &&
           add_array(l, OFF(struct drm_syncobj_timeline_wait, points), s->count_handles,
                     8, true, false);
      break;
   }
   case 0xcb:   /* SYNCOBJ_QUERY: points out */
   case 0xcd: { /* SYNCOBJ_TIMELINE_SIGNAL: points in */
      NEED(struct drm_syncobj_timeline_array);
      bool query = l->nr == 0xcb;
      ok = add_array(l, OFF(struct drm_syncobj_timeline_array, handles), s->count_handles,
                     4, true, false) &&
           add_array(l, OFF(struct drm_syncobj_timeline_array, points), s->count_handles,
                     8, !query, query);
      break;
   }
   default:
      /* A plain structure (or a number the daemon will refuse with EOPNOTSUPP). */
      break;
   }
#undef NEED
#undef OFF
   return ok ? 0 : -EINVAL;
}

static void *
user_ptr(const void *arg, size_t off)
{
   uint64_t p;
   memcpy(&p, (const uint8_t *)arg + off, sizeof(p));
   return (void *)(uintptr_t)p;
}

void
redox_drm_pack(const struct redox_drm_layout *l, const void *arg, uint8_t *payload)
{
   memset(payload, 0, l->total);
   if (l->struct_size)
      memcpy(payload, arg, MIN2(l->struct_size, l->wire_struct_size));
   for (unsigned i = 0; i < l->num_arrays; i++) {
      const struct redox_drm_array *a = &l->arrays[i];
      if (a->in && a->bytes)
         memcpy(payload + a->payload_off, user_ptr(arg, a->ptr_off), a->bytes);
   }
}

void
redox_drm_unpack(const struct redox_drm_layout *l, void *arg, const uint8_t *payload)
{
   if (l->struct_out && l->struct_size) {
      uint64_t keep[2];
      for (unsigned i = 0; i < l->num_arrays; i++)
         memcpy(&keep[i], (uint8_t *)arg + l->arrays[i].ptr_off, 8);
      memcpy(arg, payload, MIN2(l->struct_size, l->wire_struct_size));
      for (unsigned i = 0; i < l->num_arrays; i++)
         memcpy((uint8_t *)arg + l->arrays[i].ptr_off, &keep[i], 8);
   }
   for (unsigned i = 0; i < l->num_arrays; i++) {
      const struct redox_drm_array *a = &l->arrays[i];
      if (a->out && a->bytes)
         memcpy(user_ptr(arg, a->ptr_off), payload + a->payload_off, a->bytes);
   }
}

/* -------------------------------------------------------------------- version */

int
redox_drm_agx_version(int fd, struct redox_drm_agx_version *out)
{
   struct redox_drm_agx_version v;
   memset(&v, 0, sizeof(v));
   ssize_t r = call_retry(fd, 0x00, &v, sizeof(v));
   if (r < 0)
      return (int)r;
   v.name[sizeof(v.name) - 1] = 0;
   *out = v;
   return 0;
}

static int
ioctl_version(int fd, struct drm_version *v)
{
   struct redox_drm_agx_version av;
   int r = redox_drm_agx_version(fd, &av);
   if (r < 0)
      return r;

   /* As the kernel: lengths in are buffer sizes, lengths out the real ones. */
   size_t n = strlen(av.name);
   if (v->name && v->name_len)
      memcpy(v->name, av.name, n < v->name_len ? n : v->name_len);
   v->version_major = (int)av.major;
   v->version_minor = (int)av.minor;
   v->version_patchlevel = 0;
   v->name_len = n;
   v->date_len = 0;
   v->desc_len = 0;
   return 0;
}

/* ---------------------------------------------------------------------- ioctl */

int
drmIoctl(int fd, unsigned long request, void *arg)
{
   struct redox_drm_layout l;
   int r = redox_drm_layout(request, arg, &l);
   if (r < 0) {
      errno = -r;
      return -1;
   }

   if (l.nr == 0x00) {
      if (l.struct_size != sizeof(struct drm_version)) {
         errno = EINVAL;
         return -1;
      }
      r = ioctl_version(fd, arg);
      if (r < 0) {
         errno = -r;
         return -1;
      }
      return 0;
   }

   uint8_t small[512];
   uint8_t *payload = l.total <= sizeof(small) ? small : malloc(l.total);
   if (!payload) {
      errno = ENOMEM;
      return -1;
   }

   redox_drm_pack(&l, arg, payload);
   ssize_t ret = call_retry(fd, l.nr, payload, l.total);
   if (ret >= 0)
      redox_drm_unpack(&l, arg, payload);
   if (payload != small)
      free(payload);

   if (ret < 0) {
      errno = (int)-ret;
      return -1;
   }
   return 0;
}

int
drmCommandNone(int fd, unsigned long index)
{
   return drmIoctl(fd, DRM_IO(DRM_COMMAND_BASE + index), NULL) ? -errno : 0;
}

int
drmCommandRead(int fd, unsigned long index, void *data, unsigned long size)
{
   unsigned long request = DRM_IOC(DRM_IOC_READ, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size);
   return drmIoctl(fd, request, data) ? -errno : 0;
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
   unsigned long request = DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size);
   return drmIoctl(fd, request, data) ? -errno : 0;
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
   unsigned long request =
      DRM_IOC(DRM_IOC_READ | DRM_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size);
   return drmIoctl(fd, request, data) ? -errno : 0;
}

/* ------------------------------------------------------------ device identity */

drmVersionPtr
drmGetVersion(int fd)
{
   struct redox_drm_agx_version av;
   int r = redox_drm_agx_version(fd, &av);
   if (r < 0) {
      errno = -r;
      return NULL;
   }

   drmVersionPtr v = calloc(1, sizeof(*v));
   if (!v)
      return NULL;
   v->version_major = (int)av.major;
   v->version_minor = (int)av.minor;
   v->name = strdup(av.name);
   v->date = strdup("");
   v->desc = strdup(av.flags & REDOX_DRM_AGX_VERSION_SIM ? "apple-agxd (simulated firmware)"
                                                         : "apple-agxd");
   if (!v->name || !v->date || !v->desc) {
      drmFreeVersion(v);
      return NULL;
   }
   v->name_len = (int)strlen(v->name);
   v->date_len = 0;
   v->desc_len = (int)strlen(v->desc);
   return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
   if (!v)
      return;
   free(v->name);
   free(v->date);
   free(v->desc);
   free(v);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   /* The daemon has no GET_CAP in version 1; these two are what its syncobj calls are. */
   switch (capability) {
   case DRM_CAP_SYNCOBJ:
   case DRM_CAP_SYNCOBJ_TIMELINE:
      *value = 1;
      return 0;
   default:
      errno = EOPNOTSUPP;
      return -1;
   }
}

int
drmGetMagic(int fd, drm_magic_t *magic)
{
   (void)fd; (void)magic;
   return -EOPNOTSUPP;
}

int
drmAuthMagic(int fd, drm_magic_t magic)
{
   (void)fd; (void)magic;
   return -EOPNOTSUPP;
}

int
drmClose(int fd)
{
   return close(fd);
}

/* There are no device nodes and no bus: one GPU, one scheme. */
int
drmGetNodeTypeFromFd(int fd)
{
   (void)fd;
   return DRM_NODE_RENDER;
}

char *
drmGetDeviceNameFromFd2(int fd)
{
   (void)fd;
   return strdup(card_path());
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
   (void)fd;
   return strdup(card_path());
}

char *
drmGetPrimaryDeviceNameFromFd(int fd)
{
   (void)fd;
   return strdup(card_path());
}

/* One device: the daemon's scheme, as a platform device with a render node only. */
static const char *
card_path(void)
{
   const char *path = getenv("REDOX_DRM_CARD");
   return path && path[0] ? path : REDOX_DRM_DEFAULT_CARD;
}

static drmDevicePtr
new_device(void)
{
   /* One allocation, as libdrm's: the device, its node table and strings, bus and
    * device records. drmFreeDevice() frees it whole. */
   struct block {
      drmDevice device;
      char *nodes[DRM_NODE_MAX];
      char node_names[DRM_NODE_MAX][256];
      drmPlatformBusInfo bus;
      drmPlatformDeviceInfo info;
      char *compatible[2];
      char compatible0[32];
   } *b = calloc(1, sizeof(*b));
   if (!b)
      return NULL;

   for (unsigned i = 0; i < DRM_NODE_MAX; i++)
      b->nodes[i] = b->node_names[i];
   snprintf(b->node_names[DRM_NODE_RENDER], sizeof(b->node_names[0]), "%s", card_path());
   snprintf(b->bus.fullname, sizeof(b->bus.fullname), "gpu.agx");
   snprintf(b->compatible0, sizeof(b->compatible0), "apple,agx");
   b->compatible[0] = b->compatible0;
   b->info.compatible = b->compatible;

   b->device.nodes = b->nodes;
   b->device.available_nodes = 1 << DRM_NODE_RENDER;
   b->device.bustype = DRM_BUS_PLATFORM;
   b->device.businfo.platform = &b->bus;
   b->device.deviceinfo.platform = &b->info;
   return &b->device;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd; (void)flags;
   *device = new_device();
   return *device ? 0 : -ENOMEM;
}

int
drmGetDevice(int fd, drmDevicePtr *device)
{
   return drmGetDevice2(fd, 0, device);
}

int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
   (void)dev_id; (void)flags;
   *device = NULL;
   return -ENODEV;
}

/* As libdrm: with no array, the number of devices; else how many were stored. */
int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   (void)flags;
   if (!devices)
      return 1;
   if (max_devices < 1)
      return 0;
   devices[0] = new_device();
   return devices[0] ? 1 : -ENOMEM;
}

int
drmGetDevices(drmDevicePtr devices[], int max_devices)
{
   return drmGetDevices2(0, devices, max_devices);
}

void
drmFreeDevice(drmDevicePtr *device)
{
   if (!device)
      return;
   free(*device);
   *device = NULL;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
   for (int i = 0; devices && i < count; i++)
      drmFreeDevice(&devices[i]);
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
   return a && b && !strcmp(a->nodes[DRM_NODE_RENDER], b->nodes[DRM_NODE_RENDER]);
}

/* ---------------------------------------------- PRIME, sync files: not in v1 */

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   errno = EOPNOTSUPP;
   return -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd; (void)prime_fd; (void)handle;
   errno = EOPNOTSUPP;
   return -1;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   struct drm_gem_close args = {.handle = handle};
   return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   (void)fd; (void)handle; (void)obj_fd;
   errno = EOPNOTSUPP;
   return -1;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   (void)fd; (void)obj_fd; (void)handle;
   errno = EOPNOTSUPP;
   return -1;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   errno = EOPNOTSUPP;
   return -1;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   errno = EOPNOTSUPP;
   return -1;
}

int
drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd, uint32_t flags)
{
   (void)fd; (void)handle; (void)point; (void)ev_fd; (void)flags;
   errno = EOPNOTSUPP;
   return -1;
}

/* ------------------------------------------------- sync objects (as libdrm's) */

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct drm_syncobj_create args = {.flags = flags};
   int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);
   if (ret)
      return ret;
   *handle = args.handle;
   return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct drm_syncobj_destroy args = {.handle = handle};
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
               unsigned flags, uint32_t *first_signaled)
{
   struct drm_syncobj_wait args = {
      .handles = (uintptr_t)handles,
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
   };
   int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
   if (ret < 0)
      return -errno;
   if (first_signaled)
      *first_signaled = args.first_signaled;
   return ret;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array args = {
      .handles = (uintptr_t)handles,
      .count_handles = handle_count,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array args = {
      .handles = (uintptr_t)handles,
      .count_handles = handle_count,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                         uint32_t handle_count)
{
   struct drm_syncobj_timeline_array args = {
      .handles = (uintptr_t)handles,
      .points = (uintptr_t)points,
      .count_handles = handle_count,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
                       int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   struct drm_syncobj_timeline_wait args = {
      .handles = (uintptr_t)handles,
      .points = (uintptr_t)points,
      .timeout_nsec = timeout_nsec,
      .count_handles = num_handles,
      .flags = flags,
   };
   int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
   if (ret < 0)
      return -errno;
   if (first_signaled)
      *first_signaled = args.first_signaled;
   return ret;
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
                 uint32_t flags)
{
   struct drm_syncobj_timeline_array args = {
      .handles = (uintptr_t)handles,
      .points = (uintptr_t)points,
      .count_handles = handle_count,
      .flags = flags,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
   return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                   uint64_t src_point, uint32_t flags)
{
   struct drm_syncobj_transfer args = {
      .src_handle = src_handle,
      .dst_handle = dst_handle,
      .src_point = src_point,
      .dst_point = dst_point,
      .flags = flags,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
}
