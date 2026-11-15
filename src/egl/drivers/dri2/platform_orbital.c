/*
 * Copyright 2026 The Redox OS port of Mesa
 * SPDX-License-Identifier: MIT
 *
 * EGL platform for Orbital, the window system of Redox OS.
 *
 * Native types:
 *   EGLNativeDisplayType  EGL_DEFAULT_DISPLAY (Orbital has no display object; the
 *                         display a window belongs to is in the path it was opened by).
 *   EGLNativeWindowType   the file descriptor of an open Orbital window
 *                         (open("/scheme/orbital/<flags>/<x>/<y>/<w>/<h>/<title>")),
 *                         the same handle orbclient's Window holds and winit hands out
 *                         as its raw window handle. The fd stays the caller's: EGL
 *                         never closes it and never reads events from it. Descriptor 0
 *                         cannot be used (EGL takes 0 for "no window").
 *
 * A window surface has one back buffer, a linear B8G8R8A8/B8G8R8X8 image of the GPU.
 * Mesa's window-system framebuffers have row 0 at the top, and Orbital's buffer is
 * rows of 0xAARRGGBB words going down, so eglSwapBuffers is: flush, wait for the
 * frame, one memcpy of the image's memory into the window's mapped buffer, fsync.
 * (Measured on an M3 Pro: reading the GPU buffer through its CPU mapping runs at
 * about 4 GB/s with plain memcpy; a 64-byte NEON loop was not faster.)
 *
 * The window's size is what Orbital says it is (fpath of the fd), looked at when the
 * surface is made, at every eglSwapBuffers before the copy, and by eglQuerySurface of
 * EGL_WIDTH/EGL_HEIGHT. When it changed (the program wrote "S,<w>,<h>" to the window,
 * as orbclient's set_size does after a resize event) the window is mapped again and
 * the next frame gets buffers of the new size.
 *
 * Sharing (EGL_ORBITAL_SHARE=1, off by default; needs a GPU daemon with scheme
 * version 1.1, "PRIME by token"): the back buffer is exported when it is made: a
 * buffer fd (/scheme/gpu.agx/buf/<token>) that names the same memory for any
 * process given the token. Orbital cannot take a buffer yet, so a swap still copies
 * into the window, but it reads the frame through the buffer fd's own mapping, the
 * way a compositor will: the producer waits for its job, then the consumer reads
 * (the scheme shares no fences). When the export fails (a version 1.0 daemon says
 * EOPNOTSUPP) the surface says so once and uses the ordinary path. What a
 * compositor will need on top: the token handed over through the window, and a
 * second buffer so the one being shown is not the one being drawn.
 *
 * Presenting without a copy (scheme 1.1 and an Orbital that composes with the GPU,
 * redox-bridge/gpu-ref/logs/ORBITAL-GPU-DESIGN.md): the surface has two exported back
 * buffers; a swap waits for the frame, writes "B,<token>,<w>,<h>,<stride>,0,0" to the
 * window and fsyncs, and the next frame is drawn into the other buffer. Orbital reads a
 * buffer only until the next "B" write returns. When Orbital refuses (EINVAL: it
 * composes on the CPU) or the export fails, the surface copies as above, for good.
 * Destroying the surface writes "B,0" (the window shows its own image again).
 *
 * Environment:
 *   EGL_ORBITAL_TIMING=<n>   print the average time of each part of eglSwapBuffers
 *                            to stderr every <n> frames (and when the surface goes).
 *   EGL_ORBITAL_PRESENT=0    never hand buffers to Orbital (always copy).
 *   EGL_ORBITAL_SHARE=1      export the back buffer and present from the buffer fd.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/redox.h>
#include <sys/stat.h>
#include "redox_drm.h"
#include "pipe/p_screen.h"
#include "util/libdrm.h"
#include "util/os_time.h"

#include "egl_dri2.h"
#include "eglglobals.h"
#include "loader.h"
#include "loader_dri_helper.h"
#include "dri_util.h"
#include "dri_screen.h"

enum { T_FLUSH, T_MAP, T_COPY, T_SYNC, T_PATH, T_N };

struct orbital_surface {
   int fd;
   uint32_t *map;         /* the window's buffer; NULL for a window without area */
   size_t map_size;
   int win_w, win_h;      /* size of what `map` is */
   struct dri_image *back;

   /* EGL_ORBITAL_SHARE: the back buffer as another process would have it */
   bool share, share_failed;
   int share_fd;          /* the buffer fd, -1 when not exported */
   const uint8_t *share_map;
   size_t share_size;
   uint64_t share_token;
   uint64_t share_frames;

   /* Presenting by handing the buffer to Orbital (EGL_ORBITAL_PRESENT, on unless 0): two
    * exported back buffers; `back` is drawn into, `spare` is the one Orbital shows. */
   bool present, present_failed;
   struct dri_image *spare;
   int spare_fd;
   uint64_t spare_token;
   uint64_t present_frames;

   /* EGL_ORBITAL_TIMING */
   unsigned timing_every, timing_frames;
   int64_t t[T_N], t_last_swap, t_between;
   uint64_t bytes;
};

/* The window's current size, from its path: "<flags>/<x>/<y>/<w>/<h>/<title>". */
static bool
orbital_window_size(int fd, int *w, int *h)
{
   char path[512];
   ssize_t n = redox_fpath(fd, path, sizeof(path) - 1);
   if (n <= 0)
      return false;
   path[n] = 0;

   /* The flags never hold a '/', the title may: the size is fields 3 and 4. A path
    * with the scheme in front ("orbital:..." or "/scheme/orbital/...") is cut first.
    */
   char *p = path;
   if (!strncmp(p, "/scheme/orbital/", 16))
      p += 16;
   else if (!strncmp(p, "orbital:", 8))
      p += 8;

   int x, y;
   char *s = strchr(p, '/');
   if (!s || sscanf(s, "/%d/%d/%d/%d", &x, &y, w, h) != 4)
      return false;
   return *w >= 0 && *h >= 0;
}

static void
orbital_unmap_window(struct orbital_surface *o)
{
   if (o->map)
      munmap(o->map, o->map_size);
   o->map = NULL;
   o->map_size = 0;
}

/* Looks at the window's size; maps its buffer again when it changed. Returns true
 * when the size changed (or false with the old state kept when Orbital did not
 * answer).
 */
static bool
orbital_update_window(struct orbital_surface *o)
{
   int w, h;
   if (!orbital_window_size(o->fd, &w, &h))
      return false;
   if (w == o->win_w && h == o->win_h)
      return false;

   orbital_unmap_window(o);
   o->win_w = w;
   o->win_h = h;
   if (w > 0 && h > 0) {
      size_t size = (size_t)w * h * 4;
      void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, o->fd, 0);
      if (map == MAP_FAILED) {
         _eglLog(_EGL_WARNING, "orbital: mapping the window (%dx%d): %s", w, h,
                 strerror(errno));
      } else {
         o->map = map;
         o->map_size = size;
      }
   }
   return true;
}

static void
orbital_timing_report(struct orbital_surface *o)
{
   unsigned n = o->timing_frames;
   if (!n)
      return;
   int64_t in_swap = 0;
   for (int i = 0; i < T_N; i++)
      in_swap += o->t[i];
   fprintf(stderr,
           "egl-orbital: %u frames %dx%d: per frame %.3f ms = outside swap %.3f + "
           "flush %.3f + wait+map %.3f + copy %.3f (%.0f MB/s) + fsync %.3f + "
           "fpath %.3f\n",
           n, o->win_w, o->win_h, (o->t_between + in_swap) / 1e6 / n,
           o->t_between / 1e6 / n, o->t[T_FLUSH] / 1e6 / n, o->t[T_MAP] / 1e6 / n,
           o->t[T_COPY] / 1e6 / n,
           o->t[T_COPY] ? o->bytes / 1048576.0 / (o->t[T_COPY] / 1e9) : 0.0,
           o->t[T_SYNC] / 1e6 / n, o->t[T_PATH] / 1e6 / n);
   memset(o->t, 0, sizeof(o->t));
   o->t_between = 0;
   o->bytes = 0;
   o->timing_frames = 0;
}

static void
orbital_drop_back(struct orbital_surface *o)
{
   if (o->spare)
      dri2_destroy_image(o->spare);
   o->spare = NULL;
   if (o->spare_fd >= 0)
      close(o->spare_fd);
   o->spare_fd = -1;
   o->spare_token = 0;
   if (o->share_map)
      munmap((void *)o->share_map, o->share_size);
   o->share_map = NULL;
   if (o->share_fd >= 0)
      close(o->share_fd);
   o->share_fd = -1;
   o->share_token = 0;
   if (o->back) {
      dri2_destroy_image(o->back);
      o->back = NULL;
   }
}

static void
orbital_free_images(struct dri2_egl_surface *dri2_surf)
{
   struct orbital_surface *o = dri2_surf->orbital;

   if (dri2_surf->front) {
      dri2_destroy_image(dri2_surf->front);
      dri2_surf->front = NULL;
   }
   if (o)
      orbital_drop_back(o);
}

/* EGL_ORBITAL_SHARE: exports the new back buffer and maps it through its buffer fd.
 * A failure leaves the surface on the ordinary path, for good.
 */
static void
orbital_share_back(struct orbital_surface *o)
{
   const char *what = "exporting the back buffer";
   struct stat st;
   int fd = -1;

   errno = 0;
   if (!dri2_query_image(o->back, __DRI_IMAGE_ATTRIB_FD, &fd) || fd < 0)
      goto fail;
   o->share_fd = fd;

   what = "the buffer fd's token";
   if (redox_drm_buffer_token(fd, &o->share_token))
      goto fail;
   what = "the buffer fd's size";
   if (fstat(fd, &st) || st.st_size <= 0)
      goto fail;
   what = "mapping the buffer fd";
   void *map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
   if (map == MAP_FAILED)
      goto fail;
   o->share_map = map;
   o->share_size = st.st_size;
   _eglLog(_EGL_DEBUG, "orbital: back buffer shared, %zu bytes", o->share_size);
   return;

fail:
   fprintf(stderr,
           "egl-orbital: EGL_ORBITAL_SHARE: %s failed (%s); presenting by the "
           "ordinary copy\n",
           what, errno ? strerror(errno) : "no handle");
   o->share_failed = true;
   if (o->share_fd >= 0)
      close(o->share_fd);
   o->share_fd = -1;
   o->share_token = 0;
}

/* EGL_ORBITAL_PRESENT: exports the new back buffer (its token goes to Orbital at a swap).
 * A failure leaves the surface on the copy path, for good.
 */
static void
orbital_export_back(struct orbital_surface *o)
{
   int fd = -1;
   errno = 0;
   if (dri2_query_image(o->back, __DRI_IMAGE_ATTRIB_FD, &fd) && fd >= 0 &&
       !redox_drm_buffer_token(fd, &o->share_token)) {
      o->share_fd = fd;
      return;
   }
   if (fd >= 0)
      close(fd);
   o->share_token = 0;
   o->present_failed = true;
   _eglLog(_EGL_DEBUG, "orbital: the back buffer cannot be exported (%s): presenting by copy",
           errno ? strerror(errno) : "no handle");
}

static int
orbital_image_get_buffers(struct dri_drawable *driDrawable, unsigned int format,
                          uint32_t *stamp, void *loaderPrivate,
                          uint32_t buffer_mask, struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   struct orbital_surface *o = dri2_surf->orbital;

   buffers->image_mask = 0;
   buffers->front = NULL;
   buffers->back = NULL;

   /* Pbuffers: a front buffer, as the surfaceless platform (see there for why). */
   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      if (!dri2_surf->front) {
         dri2_surf->front = dri_create_image(
            dri2_dpy->dri_screen_render_gpu, dri2_surf->base.Width,
            dri2_surf->base.Height, dri2_surf->visual, NULL, 0, 0, NULL);
         if (!dri2_surf->front)
            return 0;
      }
      buffers->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      buffers->front = dri2_surf->front;
   }

   if ((buffer_mask & __DRI_IMAGE_BUFFER_BACK) && o) {
      if (!o->back) {
         /* Linear, so that its memory is the picture as the window wants it, and
          * shareable from the start: the frontend calls flush_resource on a back
          * buffer at every swap, which makes the driver move a buffer that is not
          * shareable into a new one. A window without area still gets a buffer
          * to render into.
          */
         o->back = dri_create_image(
            dri2_dpy->dri_screen_render_gpu, MAX2(dri2_surf->base.Width, 1),
            MAX2(dri2_surf->base.Height, 1), dri2_surf->visual, NULL, 0,
            __DRI_IMAGE_USE_LINEAR | __DRI_IMAGE_USE_SHARE, NULL);
         if (!o->back)
            return 0;
         if (o->share && !o->share_failed)
            orbital_share_back(o);
         else if (o->present && !o->present_failed)
            orbital_export_back(o);
      }
      buffers->image_mask |= __DRI_IMAGE_BUFFER_BACK;
      buffers->back = o->back;
   }

   return 1;
}

/* Takes the window's size as the surface's; the drawable gets new buffers when it
 * differs from what the surface had.
 */
static void
orbital_surface_resized(struct dri2_egl_surface *dri2_surf)
{
   struct orbital_surface *o = dri2_surf->orbital;

   if (dri2_surf->base.Width == o->win_w && dri2_surf->base.Height == o->win_h)
      return;
   dri2_surf->base.Width = o->win_w;
   dri2_surf->base.Height = o->win_h;
   orbital_drop_back(o);
   if (dri2_surf->dri_drawable)
      dri_invalidate_drawable(dri2_surf->dri_drawable);
}

static _EGLSurface *
orbital_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                       void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSurface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list, false,
                          native_window))
      goto cleanup_surface;

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   dri2_surf->visual = dri2_image_format_for_pbuffer_config(dri2_dpy, config);
   if (dri2_surf->visual == PIPE_FORMAT_NONE)
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      struct orbital_surface *o = calloc(1, sizeof *o);
      if (!o) {
         _eglError(EGL_BAD_ALLOC, "eglCreateWindowSurface");
         goto cleanup_surface;
      }
      o->fd = (int)(intptr_t)native_window;
      o->share_fd = -1;
      o->share = debug_get_bool_option("EGL_ORBITAL_SHARE", false);
      o->spare_fd = -1;
      o->present = !o->share && debug_get_bool_option("EGL_ORBITAL_PRESENT", true);
      o->win_w = o->win_h = -1;
      if (!orbital_window_size(o->fd, &o->win_w, &o->win_h)) {
         free(o);
         _eglError(EGL_BAD_NATIVE_WINDOW,
                   "not the file descriptor of an Orbital window");
         goto cleanup_surface;
      }
      o->win_w = o->win_h = -1;
      orbital_update_window(o);
      o->timing_every = debug_get_num_option("EGL_ORBITAL_TIMING", 0);
      dri2_surf->orbital = o;
      dri2_surf->base.Width = o->win_w;
      dri2_surf->base.Height = o->win_h;
      dri2_surf->base.SwapInterval = dri2_dpy->default_swap_interval;
   }

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_window;

   return &dri2_surf->base;

cleanup_window:
   if (dri2_surf->orbital) {
      orbital_unmap_window(dri2_surf->orbital);
      free(dri2_surf->orbital);
   }
cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static _EGLSurface *
orbital_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_window, const EGLint *attrib_list)
{
   return orbital_create_surface(disp, EGL_WINDOW_BIT, conf, native_window,
                                 attrib_list);
}

static _EGLSurface *
orbital_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                               const EGLint *attrib_list)
{
   return orbital_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL, attrib_list);
}

static EGLBoolean
orbital_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   struct orbital_surface *o = dri2_surf->orbital;

   /* The window shows its own image again (Orbital lets the buffers go). */
   if (o && o->present_frames && !o->present_failed)
      (void)!write(o->fd, "B,0", 3);
   orbital_free_images(dri2_surf);
   driDestroyDrawable(dri2_surf->dri_drawable);

   if (o) {
      if (o->timing_every)
         orbital_timing_report(o);
      if (o->share)
         fprintf(stderr, "egl-orbital: %llu frames presented from the shared buffer\n",
                 (unsigned long long)o->share_frames);
      orbital_unmap_window(o); /* the fd is the caller's */
      free(o);
   }

   dri2_fini_surface(surf);
   free(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
orbital_swap_buffers(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   struct orbital_surface *o = dri2_surf->orbital;
   _EGLContext *ctx = _eglGetCurrentContext();

   /* Nothing to do for a pbuffer. */
   if (!o)
      return EGL_TRUE;

   int64_t t0 = os_time_get_nano(), t1, t2, t3, t4, t5;

   /* Everything drawn goes to the GPU. No throttling on the frame before: the
    * copy below waits for this frame, and a throttle fence costs four more calls
    * to the GPU daemon per frame.
    */
   dri2_flush_drawable_for_swapbuffers_flags(disp, surf,
                                             __DRI2_NOTHROTTLE_SWAPBUFFER);
   t1 = os_time_get_nano();

   /* The window as it is now: a resize since the last frame shows here. */
   orbital_update_window(o);
   t2 = os_time_get_nano();

   t3 = t4 = t2;
   if (o->back && o->map && ctx && o->present && !o->present_failed && o->share_token) {
      /* Hand the buffer to Orbital: wait for the frame (the scheme shares no fences),
       * name it in the window, sync. Orbital then reads it with the GPU; when the write
       * of the next "B" returns, it reads this one no more (it draws synchronously), so
       * with two buffers the next frame never waits. Refused (Orbital composes on the
       * CPU, or does not know "B"): the copy path, for good.
       */
      struct dri_context *dri_ctx = dri2_egl_context(ctx)->dri_context;
      int stride = 0;
      void *transfer = NULL;
      if (dri2_map_image(dri_ctx, o->back, 0, 0, 1, 1, __DRI_IMAGE_TRANSFER_READ, &stride, &transfer))
         dri2_unmap_image(dri_ctx, o->back, transfer);
      dri2_query_image(o->back, __DRI_IMAGE_ATTRIB_STRIDE, &stride);
      t3 = os_time_get_nano();
      char cmd[96];
      int n = snprintf(cmd, sizeof(cmd), "B,%016llx,%d,%d,%d,0,0", (unsigned long long)o->share_token,
                       dri2_surf->base.Width, dri2_surf->base.Height, stride);
      if (write(o->fd, cmd, n) == n) {
         t4 = os_time_get_nano();
         fsync(o->fd);
         o->present_frames++;
         /* The next frame goes into the other buffer. */
         struct dri_image *img = o->back;
         int fd = o->share_fd;
         uint64_t token = o->share_token;
         o->back = o->spare;
         o->share_fd = o->spare_fd;
         o->share_token = o->spare_token;
         o->spare = img;
         o->spare_fd = fd;
         o->spare_token = token;
         if (dri2_surf->dri_drawable)
            dri_invalidate_drawable(dri2_surf->dri_drawable);
         goto presented;
      }
      o->present_failed = true;
      fprintf(stderr, "egl-orbital: Orbital takes no buffers (%s): presenting by copy\n", strerror(errno));
   }
   if (o->back && o->map && ctx) {
      struct dri_context *dri_ctx = dri2_egl_context(ctx)->dri_context;
      int w = MIN2(dri2_surf->base.Width, o->win_w);
      int h = MIN2(dri2_surf->base.Height, o->win_h);
      int stride = 0;
      void *transfer = NULL;

      /* Waits for the frame; a linear image maps as its own memory. */
      const uint8_t *src = dri2_map_image(dri_ctx, o->back, 0, 0, w, h,
                                          __DRI_IMAGE_TRANSFER_READ, &stride,
                                          &transfer);
      t3 = os_time_get_nano();
      if (src && o->share_map && (size_t)stride * h <= o->share_size) {
         /* The frame is finished (the map waited): read it as the consumer of
          * the shared buffer would, through the buffer fd's mapping.
          */
         src = o->share_map;
         o->share_frames++;
      }
      if (src) {
         if (stride == o->win_w * 4 && w == o->win_w) {
            memcpy(o->map, src, (size_t)stride * h);
         } else {
            for (int y = 0; y < h; y++)
               memcpy(o->map + (size_t)y * o->win_w,
                            src + (size_t)y * stride, (size_t)w * 4);
         }
         dri2_unmap_image(dri_ctx, o->back, transfer);
         o->bytes += (uint64_t)w * h * 4;
      } else {
         _eglLog(_EGL_WARNING, "orbital: mapping the back buffer failed");
      }
      t4 = os_time_get_nano();
      fsync(o->fd);
   }
presented:
   t5 = os_time_get_nano();

   /* A new size takes effect with the next frame. */
   orbital_surface_resized(dri2_surf);

   if (o->timing_every) {
      if (o->t_last_swap)
         o->t_between += t0 - o->t_last_swap;
      o->t[T_FLUSH] += t1 - t0;
      o->t[T_PATH] += t2 - t1;
      o->t[T_MAP] += t3 - t2;
      o->t[T_COPY] += t4 - t3;
      o->t[T_SYNC] += t5 - t4;
      o->t_last_swap = os_time_get_nano();
      if (++o->timing_frames >= o->timing_every)
         orbital_timing_report(o);
   }

   return EGL_TRUE;
}

static EGLBoolean
orbital_swap_interval(_EGLDisplay *disp, _EGLSurface *surf, EGLint interval)
{
   /* Accepted and kept (eglapi clamps it to the config's range). Orbital gives
    * no signal to wait for: a swap returns when Orbital has taken the frame.
    */
   return EGL_TRUE;
}

static EGLBoolean
orbital_query_surface(_EGLDisplay *disp, _EGLSurface *surf, EGLint attribute,
                      EGLint *value)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   struct orbital_surface *o = dri2_surf->orbital;

   if (o && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)) {
      orbital_update_window(o);
      orbital_surface_resized(dri2_surf);
   }
   return _eglQuerySurface(disp, surf, attribute, value);
}

static const struct dri2_egl_display_vtbl dri2_orbital_display_vtbl = {
   .create_window_surface = orbital_create_window_surface,
   .create_pbuffer_surface = orbital_create_pbuffer_surface,
   .destroy_surface = orbital_destroy_surface,
   .swap_interval = orbital_swap_interval,
   .create_image = dri2_create_image_khr,
   .swap_buffers = orbital_swap_buffers,
   .query_surface = orbital_query_surface,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static void
orbital_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
}

static unsigned
orbital_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_FP16:
      return 1;
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static const __DRIimageLoaderExtension image_loader_extension = {
   .base = {__DRI_IMAGE_LOADER, 2},
   .getBuffers = orbital_image_get_buffers,
   .flushFrontBuffer = orbital_flush_front_buffer,
   .getCapability = orbital_get_capability,
};

static const __DRIextension *image_loader_extensions[] = {
   &image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

/* The GPU: the first DRM device whose driver loads (on Redox: the one device
 * src/redox/drm enumerates, the GPU daemon's scheme).
 */
static bool
orbital_probe_device(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLDevice *dev_list = _eglGlobal.DeviceList;

   for (; dev_list; dev_list = _eglDeviceNext(dev_list)) {
      if (!_eglDeviceSupports(dev_list, _EGL_DEVICE_DRM))
         continue;

      drmDevicePtr device = _eglDeviceDrm(dev_list);
      if (!(device->available_nodes & (1 << DRM_NODE_RENDER)))
         continue;

      dri2_dpy->fd_render_gpu =
         loader_open_device(device->nodes[DRM_NODE_RENDER]);
      if (dri2_dpy->fd_render_gpu < 0)
         continue;
      dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;

      disp->Device = dev_list;
      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd_render_gpu);
      if (dri2_dpy->driver_name) {
         dri2_detect_swrast_kopper(disp);
         dri2_dpy->loader_extensions = image_loader_extensions;
         if (dri2_create_screen(disp)) {
            if (dri2_dpy->dri_screen_render_gpu->base.screen->caps.graphics)
               return true;
            driDestroyScreen(dri2_dpy->dri_screen_render_gpu);
            dri2_dpy->dri_screen_render_gpu = NULL;
            dri2_dpy->own_dri_screen = false;
         } else {
            _eglLog(_EGL_WARNING, "orbital: failed to create the screen");
         }
      }

      free(dri2_dpy->driver_name);
      dri2_dpy->driver_name = NULL;
      close(dri2_dpy->fd_render_gpu);
      dri2_dpy->fd_render_gpu = -1;
      dri2_dpy->fd_display_gpu = -1;
   }

   return false;
}

/* Window (and pbuffer) configs: the driver's configs in the two formats that are
 * an Orbital pixel in memory, 0xAARRGGBB little-endian.
 */
static void
orbital_add_configs(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   static const enum pipe_format visuals[] = {
      PIPE_FORMAT_B8G8R8A8_UNORM,
      PIPE_FORMAT_B8G8R8X8_UNORM,
   };

   for (unsigned v = 0; v < ARRAY_SIZE(visuals); v++) {
      for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++) {
         const struct gl_config *gl_config =
            (struct gl_config *)dri2_dpy->driver_configs[i];

         if (util_format_linear(gl_config->color_format) != visuals[v])
            continue;

         dri2_add_config(disp, dri2_dpy->driver_configs[i],
                         EGL_WINDOW_BIT | EGL_PBUFFER_BIT, NULL);
      }
   }
}

EGLBoolean
dri2_initialize_orbital(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (!orbital_probe_device(disp))
      return _eglError(EGL_NOT_INITIALIZED, "orbital: failed to load a driver");

   dri2_setup_screen(disp);
   dri2_setup_swap_interval(disp, 1);

   orbital_add_configs(disp);
   /* The other formats (RGBA order, 10 bit, float): pbuffers only. */
   dri2_add_pbuffer_configs_for_visuals(disp);

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &dri2_orbital_display_vtbl;

   return EGL_TRUE;
}
