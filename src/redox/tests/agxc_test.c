/*
 * Copyright 2026 The Mesa authors
 * SPDX-License-Identifier: MIT
 *
 * Compiles a few fixed NIR shaders with the AGX compiler for a few chips and prints the
 * machine code. The output depends on nothing but the compiler, so two builds of Mesa
 * (Linux/glibc or musl, Redox/relibc) must print the same text.
 *
 *   agxc_test            every shader for every chip
 *   agxc_test <shader>   one shader (cs_loop, cs_math, fs_clear, fs_grad)
 */

#include <stdio.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include "util/ralloc.h"
#include "agx_chip.h"
#include "agx_compile.h"
#include "agx_tilebuffer.h"

/* A loop with integer arithmetic over memory named by a 64-bit uniform. */
static nir_shader *
build_cs_loop(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &agx_nir_options,
                                                  "cs_loop");
   nir_def *addr = nir_load_preamble(&b, 1, 64, .base = 0);
   nir_def *v = nir_load_global(&b, 4, 32, addr, .align_mul = 4);

   nir_variable *acc = nir_local_variable_create(b.impl, glsl_uint_type(), "acc");
   nir_variable *i = nir_local_variable_create(b.impl, glsl_uint_type(), "i");
   nir_store_var(&b, acc, nir_channel(&b, v, 0), 1);
   nir_store_var(&b, i, nir_imm_int(&b, 0), 1);

   nir_push_loop(&b);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_break_if(&b, nir_uge(&b, iv, nir_channel(&b, v, 3)));
      nir_def *a = nir_load_var(&b, acc);
      a = nir_iadd(&b, nir_imul(&b, a, nir_channel(&b, v, 1)), iv);
      a = nir_ixor(&b, a, nir_ishl(&b, nir_channel(&b, v, 2), nir_iand_imm(&b, iv, 7)));
      nir_store_var(&b, acc, a, 1);
      nir_store_var(&b, i, nir_iadd_imm(&b, iv, 1), 1);
   }
   nir_pop_loop(&b, NULL);

   nir_def *res = nir_vec2(&b, nir_load_var(&b, acc), nir_load_var(&b, i));
   nir_store_global(&b, res, nir_iadd_imm(&b, addr, 16), .write_mask = 3, .align_mul = 4);
   return b.shader;
}

/* Floating point: transcendental lowering, selects, conversions. */
static nir_shader *
build_cs_math(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &agx_nir_options,
                                                  "cs_math");
   nir_def *addr = nir_load_preamble(&b, 1, 64, .base = 0);
   nir_def *v = nir_load_global(&b, 4, 32, addr, .align_mul = 4);

   nir_def *x = nir_channel(&b, v, 0), *y = nir_channel(&b, v, 1);
   nir_def *s = nir_fsin(&b, x);
   nir_def *c = nir_fcos(&b, y);
   nir_def *r = nir_fsqrt(&b, nir_fadd(&b, nir_fmul(&b, s, s), nir_fmul(&b, c, c)));
   nir_def *e = nir_fexp2(&b, nir_fmin(&b, x, y));
   nir_def *sel = nir_bcsel(&b, nir_flt(&b, x, y), r, e);
   nir_def *n = nir_f2i32(&b, nir_ffloor(&b, nir_fmul_imm(&b, sel, 255.0)));
   nir_def *p = nir_fpow(&b, nir_fabs(&b, nir_channel(&b, v, 2)), nir_channel(&b, v, 3));

   nir_def *res = nir_vec4(&b, sel, nir_i2f32(&b, n), p, nir_fdiv(&b, x, nir_fmax(&b, y, e)));
   nir_store_global(&b, res, nir_iadd_imm(&b, addr, 16), .write_mask = 0xf, .align_mul = 4);
   return b.shader;
}

/* What the driver's clear ("background") program is: one uniform colour to RT0. */
static nir_shader *
build_fs(bool grad)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &agx_nir_options,
                                                  grad ? "fs_grad" : "fs_clear");
   b.shader->info.fs.untyped_color_outputs = true;

   nir_def *color = nir_load_preamble(&b, 4, 32, .base = 4);
   if (grad) {
      /* a colour that depends on the pixel position */
      nir_def *xy = nir_u2f32(&b, nir_u2u32(&b, nir_load_pixel_coord(&b)));
      nir_def *t = nir_fmul_imm(&b, nir_channel(&b, xy, 0), 1.0 / 64.0);
      nir_def *u = nir_fmul_imm(&b, nir_channel(&b, xy, 1), 1.0 / 64.0);
      color = nir_fmul(&b, color, nir_vec4(&b, t, u, nir_fmul(&b, t, u), nir_imm_float(&b, 1.0)));
   }
   nir_store_output(&b, color, nir_imm_int(&b, 0), .write_mask = 0xf,
                    .src_type = grad ? nir_type_float32 : nir_type_uint32,
                    .io_semantics.location = FRAG_RESULT_DATA0, .io_semantics.num_slots = 1);
   b.shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DATA0);
   return b.shader;
}

struct chip {
   const char *name;
   enum agx_chip chip;
};

static const struct chip chips[] = {
   {"g13g", AGX_CHIP_G13G},
   {"g14g", AGX_CHIP_G14G},
   {"g15s", AGX_CHIP_G15S},
};

static const char *const shaders[] = {"cs_loop", "cs_math", "fs_clear", "fs_grad"};

static uint64_t
fnv1a(const uint8_t *p, size_t n)
{
   uint64_t h = 0xcbf29ce484222325ull;
   for (size_t i = 0; i < n; i++)
      h = (h ^ p[i]) * 0x100000001b3ull;
   return h;
}

static void
run(const char *name, const struct chip *chip)
{
   nir_shader *nir;
   bool fs = false;

   if (!strcmp(name, "cs_loop"))
      nir = build_cs_loop();
   else if (!strcmp(name, "cs_math"))
      nir = build_cs_math();
   else {
      nir = build_fs(!strcmp(name, "fs_grad"));
      fs = true;
   }

   struct agx_shader_key key = {
      .dev = {
         .chip = chip->chip,
         .encoding = agx_chip_isa_encoding(chip->chip),
         .needs_g13x_coherency = U_TRISTATE_NO,
         .soft_fault = false,
      },
      .reserved_preamble = 16,
      .no_preamble = agx_chip_native_agx3(chip->chip),
   };

   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   agx_preprocess_nir(nir);

   if (fs) {
      enum pipe_format formats[1] = {PIPE_FORMAT_R8G8B8A8_UNORM};
      struct agx_tilebuffer_layout tib = agx_build_tilebuffer_layout(formats, 1, 1, false);
      unsigned bindless_base = 0;

      key.fs.ignore_tib_dependencies = true;
      key.fs.background = true;
      key.fs.nr_samples = agx_chip_native_agx3(chip->chip) ? 1 : 0;
      NIR_PASS(_, nir, agx_nir_lower_tilebuffer, &tib, NULL, &bindless_base, NULL, NULL,
               false, agx_chip_native_agx3(chip->chip));
   }

   struct agx_shader_part bin = {0};
   bool ok = agx_compile_shader_nir(nir, &key, &bin);

   printf("shader %s chip %s: %s", name, chip->name, ok ? "compiled" : "refused");
   if (ok) {
      const uint8_t *p = bin.binary;
      printf(" size %u main_offset %u main_size %u preamble_size %u fnv1a %016llx\n",
             bin.info.binary_size, bin.info.main_offset, bin.info.main_size,
             bin.info.preamble_size, (unsigned long long)fnv1a(p, bin.info.binary_size));
      for (unsigned i = 0; i < bin.info.binary_size; i++)
         printf("%02x%s", p[i], (i % 32 == 31 || i + 1 == bin.info.binary_size) ? "\n" : "");
      free(bin.binary);
   } else {
      printf("\n");
   }
   ralloc_free(nir);
}

int
main(int argc, char **argv)
{
   glsl_type_singleton_init_or_ref();
   for (unsigned s = 0; s < ARRAY_SIZE(shaders); s++) {
      if (argc > 1 && strcmp(argv[1], shaders[s]))
         continue;
      for (unsigned c = 0; c < ARRAY_SIZE(chips); c++) {
         if (argc > 2 && strcmp(argv[2], chips[c].name))
            continue;
         run(shaders[s], &chips[c]);
         fflush(stdout);
      }
   }
   glsl_type_singleton_decref();
   return 0;
}
