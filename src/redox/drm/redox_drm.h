/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * libdrm for Redox: the functions of xf86drm.h that Mesa's asahi driver uses, carried to
 * the GPU daemon's scheme (apple-agxd, /scheme/gpu.agx) instead of a kernel DRM device.
 * Wire format: redox-bridge/gpu-ref/logs/CLIENT-SCHEME.md (version 1).
 *
 * This header is the part that is not libdrm's: the marshalling, exposed so it can be
 * tested without a daemon, and the way to open the device.
 */
#ifndef REDOX_DRM_H
#define REDOX_DRM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REDOX_DRM_DEFAULT_CARD "/scheme/gpu.agx/card"

/* Opens the daemon's device: $REDOX_DRM_CARD or REDOX_DRM_DEFAULT_CARD, O_RDWR|O_CLOEXEC.
 * One open is one client (Linux's drm_file). Returns an fd or -1 with errno. */
int redox_drm_open(void);

/* One array a request's structure points at. On the wire it follows the structure. */
struct redox_drm_array {
   size_t ptr_off;     /* offset in the structure of the 64-bit user pointer */
   size_t bytes;       /* length of the pointed-to area */
   size_t payload_off; /* where it sits in the payload (a multiple of 8) */
   bool in;            /* copied into the payload before the call */
   bool out;           /* copied back to the caller after a successful call */
};

struct redox_drm_layout {
   uint8_t nr;         /* _IOC_NR(request): metadata[0] of the call */
   size_t struct_size; /* _IOC_SIZE(request): what the caller passes */
   size_t wire_struct_size; /* what the daemon expects: the payload starts with this many
                             * bytes, the caller's structure zero-extended or cut */
   bool struct_out;    /* the structure is copied back (request has IOC_OUT) */
   unsigned num_arrays;
   struct redox_drm_array arrays[2];
   size_t total;       /* payload length */
};

/* Describes the payload of an ioctl request for the structure at arg.
 * Returns 0, or a negative errno (-ENOTTY: not a DRM request; -EINVAL: sizes that cannot be
 * right, such as counts that overflow). */
int redox_drm_layout(unsigned long request, const void *arg,
                     struct redox_drm_layout *layout);

/* Builds the payload (layout->total bytes, zero padded) from arg and the arrays. */
void redox_drm_pack(const struct redox_drm_layout *layout, const void *arg,
                    uint8_t *payload);

/* After a successful call: the structure (pointer fields kept as the caller's) and the
 * out arrays go back to the caller. */
void redox_drm_unpack(const struct redox_drm_layout *layout, void *arg,
                      const uint8_t *payload);

/* The transport: one call on the scheme. Returns 0 or more, or a negative errno.
 * The default is SYS_CALL on Redox (read-write payload, metadata[0] = nr) and -ENOSYS
 * elsewhere; tests replace it. Passing NULL restores the default. */
typedef ssize_t (*redox_drm_transport_fn)(int fd, uint64_t nr, void *payload,
                                          size_t len);
void redox_drm_set_transport(redox_drm_transport_fn fn);

/* DRM_IOCTL_VERSION's payload (CLIENT-SCHEME.md 2.1). */
struct redox_drm_agx_version {
   uint32_t major;
   uint32_t minor;
   uint32_t flags; /* bit 0: the daemon runs its simulated firmware */
   uint32_t pad;
   char name[16];
};
#define REDOX_DRM_AGX_VERSION_SIM 1u

/* Asks the daemon for its version record. Returns 0 or a negative errno. */
int redox_drm_agx_version(int fd, struct redox_drm_agx_version *out);

#ifdef __cplusplus
}
#endif

#endif
