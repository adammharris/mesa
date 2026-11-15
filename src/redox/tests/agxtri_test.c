/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * The asahi gallium driver, without a GL frontend or a window system: a pipe_screen on
 * the GPU daemon's device, a context, a 64x64 RGBA8 render target, a clear and one
 * triangle, a flush, a wait on the fence, and the target read back.
 *
 *   agxtri_test --fake   against the daemon played in this process (redox_fake_daemon.h):
 *                        the whole driver runs (shaders compiled, control streams built,
 *                        jobs packed to the G15 wire and "submitted") but nothing renders;
 *                        prints what was submitted. Runs anywhere.
 *   agxtri_test [out.rgba]
 *                        against the scheme ($REDOX_DRM_CARD or /scheme/gpu.agx/card): with
 *                        the real daemon the readback is the picture; it is checked
 *                        against the expected clear colour and triangle and optionally
 *                        written as raw RGBA8. A daemon that says it is simulated (VERSION
 *                        flags bit 0; the stub scheme too) is not expected to render.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/driconf.h"
#include "util/u_inlines.h"
#include "util/xmlconfig.h"
#include "asahi/compiler/agx_compile.h"
#include "agx_device.h"
#include "agx_public.h"
#include "agx_state.h"

#include "redox_fake_daemon.h"

#define W 64
#define H 64

/* Clip-space positions of the three corners, chosen by gl_VertexID: pixel corners
 * (8,8), (56,8), (8,56) of the 64x64 target, as gpu-ref/render-harness/vkjob.c draws. */
static void *
create_vs(struct pipe_context *pctx)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &agx_nir_options,
                                                  "agxtri vs");
   nir_def *id = nir_load_vertex_id(&b);
   nir_def *x = nir_bcsel(&b, nir_ieq_imm(&b, id, 1), nir_imm_float(&b, 0.75),
                          nir_imm_float(&b, -0.75));
   nir_def *y = nir_bcsel(&b, nir_ieq_imm(&b, id, 2), nir_imm_float(&b, 0.75),
                          nir_imm_float(&b, -0.75));
   nir_def *pos = nir_vec4(&b, x, y, nir_imm_float(&b, 0), nir_imm_float(&b, 1));
   nir_store_output(&b, pos, nir_imm_int(&b, 0), .base = 0, .write_mask = 0xf,
                    .io_semantics.location = VARYING_SLOT_POS);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   struct pipe_shader_state state = {.type = PIPE_SHADER_IR_NIR, .ir.nir = b.shader};
   return pctx->create_vs_state(pctx, &state);
}

static void *
create_fs(struct pipe_context *pctx)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &agx_nir_options,
                                                  "agxtri fs");
   /* ff 33 66 ff */
   nir_store_output(&b, nir_imm_vec4(&b, 1.0, 0.2, 0.4, 1.0), nir_imm_int(&b, 0), .base = 0,
                    .write_mask = 0xf, .io_semantics.location = FRAG_RESULT_DATA0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   struct pipe_shader_state state = {.type = PIPE_SHADER_IR_NIR, .ir.nir = b.shader};
   return pctx->create_fs_state(pctx, &state);
}

static bool
inside_triangle(unsigned px, unsigned py)
{
   /* pixel centre against (8,8) (56,8) (8,56) */
   float x = px + 0.5f, y = py + 0.5f;
   return x > 8 && y > 8 && (x - 8) + (y - 8) < 48;
}

int
main(int argc, char **argv)
{
   bool is_fake = argc > 1 && !strcmp(argv[1], "--fake");
   const char *out = (!is_fake && argc > 1) ? argv[1] : NULL;
   int fd = is_fake ? fake_open() : redox_drm_open();

   if (fd < 0) {
      printf("FAIL: cannot open the device: %s\n", strerror(errno));
      return 1;
   }

   /* A daemon on its simulated firmware (and the fake) completes jobs without rendering. */
   struct redox_drm_agx_version version = {0};
   bool renders = redox_drm_agx_version(fd, &version) == 0 &&
                  !(version.flags & REDOX_DRM_AGX_VERSION_SIM);
   printf("daemon: %s %u.%u, %s\n", version.name, version.major, version.minor,
          renders ? "renders" : "simulated: jobs complete, nothing renders");

   static const driOptionDescription options[] = {
#include "driinfo_asahi.h"
   };
   driOptionCache info, cache;
   driParseOptionInfo(&info, options, ARRAY_SIZE(options));
   const driConfigFileParseParams parse = {.driverName = "asahi"};
   driParseConfigFiles(&cache, &info, &parse);
   struct pipe_screen_config config = {.options = &cache, .options_info = &info};

   struct pipe_screen *screen = agx_screen_create(fd, NULL, &config);
   if (!screen) {
      printf("FAIL: agx_screen_create\n");
      return 1;
   }
   printf("screen: %s / %s\n", screen->get_vendor(screen), screen->get_name(screen));

   struct pipe_context *pctx = screen->context_create(screen, NULL, 0);
   if (!pctx) {
      printf("FAIL: context_create\n");
      return 1;
   }

   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .width0 = W,
      .height0 = H,
      .depth0 = 1,
      .array_size = 1,
      .nr_samples = 1,
      .nr_storage_samples = 1,
      .bind = PIPE_BIND_RENDER_TARGET,
   };
   struct pipe_resource *rt = screen->resource_create(screen, &templ);
   if (!rt) {
      printf("FAIL: resource_create\n");
      return 1;
   }

   void *vs = create_vs(pctx), *fs = create_fs(pctx);
   struct pipe_rasterizer_state rast_templ = {
      .depth_clip_near = true,
      .depth_clip_far = true,
      .half_pixel_center = true,
      .bottom_edge_rule = false,
      .point_size = 1,
      .line_width = 1,
   };
   struct pipe_blend_state blend_templ = {.rt[0].colormask = 0xf};
   struct pipe_depth_stencil_alpha_state depth_templ = {0};
   void *rast = pctx->create_rasterizer_state(pctx, &rast_templ);
   void *blend = pctx->create_blend_state(pctx, &blend_templ);
   void *depth = pctx->create_depth_stencil_alpha_state(pctx, &depth_templ);
   void *ve = pctx->create_vertex_elements_state(pctx, 0, NULL);
   if (!vs || !fs || !rast || !blend || !depth || !ve) {
      printf("FAIL: state objects (vs %p fs %p)\n", vs, fs);
      return 1;
   }
   pctx->bind_vs_state(pctx, vs);
   pctx->bind_fs_state(pctx, fs);
   pctx->bind_rasterizer_state(pctx, rast);
   pctx->bind_blend_state(pctx, blend);
   pctx->bind_depth_stencil_alpha_state(pctx, depth);
   pctx->bind_vertex_elements_state(pctx, ve);

   struct pipe_framebuffer_state fb = {
      .width = W,
      .height = H,
      .layers = 1,
      .nr_cbufs = 1,
      .cbufs[0] = {.texture = rt, .format = templ.format},
   };
   struct pipe_viewport_state vp = {
      .scale = {W / 2.0f, H / 2.0f, 0.5f},
      .translate = {W / 2.0f, H / 2.0f, 0.5f},
   };
   pctx->set_framebuffer_state(pctx, &fb);
   pctx->set_viewport_states(pctx, 0, 1, &vp);
   pctx->set_sample_mask(pctx, ~0u);

   /* 33 66 99 cc */
   union pipe_color_union clear = {.f = {0.2f, 0.4f, 0.6f, 0.8f}};
   pctx->clear(pctx, PIPE_CLEAR_COLOR0, ~0u, 0, NULL, &clear, 0.0, 0);

   struct pipe_draw_info draw = {.mode = MESA_PRIM_TRIANGLES, .instance_count = 1};
   struct pipe_draw_start_count_bias sc = {.count = 3};
   pctx->draw_vbo(pctx, &draw, 0, NULL, &sc, 1);

   struct pipe_fence_handle *fence = NULL;
   pctx->flush(pctx, &fence, PIPE_FLUSH_END_OF_FRAME);
   bool done = fence ? screen->fence_finish(screen, pctx, fence, 5000000000ull) : true;
   printf("flush: fence %s, finished %s\n", fence ? "yes" : "none", done ? "yes" : "NO");
   if (fence)
      screen->fence_reference(screen, &fence, NULL);

   /* read the target back */
   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe_texture_map(pctx, rt, 0, 0, PIPE_MAP_READ, 0, 0, W, H, &xfer);
   int rc = 0;
   if (!map) {
      printf("FAIL: cannot map the target\n");
      rc = 1;
   } else {
      static const uint8_t clear_px[4] = {0x33, 0x66, 0x99, 0xcc};
      static const uint8_t tri_px[4] = {0xff, 0x33, 0x66, 0xff};
      unsigned n_clear = 0, n_tri = 0, n_zero = 0, n_other = 0, wrong = 0;
      for (unsigned y = 0; y < H; y++) {
         for (unsigned x = 0; x < W; x++) {
            const uint8_t *px = map + (size_t)y * xfer->stride + x * 4;
            bool is_tri = !memcmp(px, tri_px, 4), is_clear = !memcmp(px, clear_px, 4);
            n_tri += is_tri;
            n_clear += is_clear;
            n_zero += !px[0] && !px[1] && !px[2] && !px[3];
            n_other += !is_tri && !is_clear && (px[0] || px[1] || px[2] || px[3]);
            wrong += inside_triangle(x, y) ? !is_tri : !is_clear;
         }
      }
      const uint8_t *c = map + (size_t)20 * xfer->stride + 20 * 4;
      printf("readback: %u clear, %u triangle, %u zero, %u other; pixel (20,20) = %02x %02x "
             "%02x %02x; %u of %u differ from the expected picture\n",
             n_clear, n_tri, n_zero, n_other, c[0], c[1], c[2], c[3], wrong, W * H);
      if (out) {
         FILE *f = fopen(out, "wb");
         if (f) {
            for (unsigned y = 0; y < H; y++)
               fwrite(map + (size_t)y * xfer->stride, 1, W * 4, f);
            fclose(f);
         }
      }
      if (renders && wrong) {
         printf("FAIL: the picture is not the expected one\n");
         rc = 1;
      }
      pipe_texture_unmap(pctx, xfer);
   }

   if (is_fake) {
      printf("submissions: %u\n", fake.submits);
      for (unsigned i = 0; i < fake.submits && i < FAKE_MAX_SUBMITS; i++)
         printf("  submit %u: queue %u, %u in syncs, %u out syncs, cmdbuf %u bytes (%u render, "
                "%u compute, %u other), fnv1a %016llx\n",
                i, fake.submit[i].queue, fake.submit[i].in_syncs, fake.submit[i].out_syncs,
                fake.submit[i].cmdbuf_size, fake.submit[i].renders, fake.submit[i].computes,
                fake.submit[i].others, (unsigned long long)fake.submit[i].cmdbuf_fnv1a);
      printf("objects %u, binds %u, bad calls %u\n", fake.num_gems, fake.binds, fake.bad);
      if (!fake.submits || fake.bad || !done) {
         printf("FAIL: nothing was submitted, or a call was malformed\n");
         rc = 1;
      }
   }

   pctx->bind_vs_state(pctx, NULL);
   pctx->bind_fs_state(pctx, NULL);
   pctx->bind_rasterizer_state(pctx, NULL);
   pctx->bind_blend_state(pctx, NULL);
   pctx->bind_depth_stencil_alpha_state(pctx, NULL);
   pctx->bind_vertex_elements_state(pctx, NULL);
   pctx->delete_vs_state(pctx, vs);
   pctx->delete_fs_state(pctx, fs);
   pctx->delete_rasterizer_state(pctx, rast);
   pctx->delete_blend_state(pctx, blend);
   pctx->delete_depth_stencil_alpha_state(pctx, depth);
   pctx->delete_vertex_elements_state(pctx, ve);
   struct pipe_framebuffer_state none = {0};
   pctx->set_framebuffer_state(pctx, &none);
   pipe_resource_reference(&rt, NULL);
   pctx->destroy(pctx);
   screen->destroy(screen);
   driDestroyOptionCache(&cache);
   driDestroyOptionInfo(&info);

   printf(rc ? "agxtri_test: FAILED\n" : "agxtri_test: ok\n");
   return rc;
}
