/*
 * Copyright © 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <cairo.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "intel_bufops.h"

#define PAGE_SIZE 4096

#define WIDTH 64
#define HEIGHT 64
#define COLOR_00	0x00
#define COLOR_33	0x33
#define COLOR_77	0x77
#define COLOR_CC	0xcc

IGT_TEST_DESCRIPTION("intel_bb API check.");

enum reloc_objects {
	RELOC,
	NORELOC,
};

enum obj_cache_ops {
	PURGE_CACHE,
	KEEP_CACHE,
};

static bool debug_bb = false;
static bool write_png = false;
static bool buf_info = false;

static void *alloc_aligned(uint64_t size)
{
	void *p;

	igt_assert_eq(posix_memalign(&p, 16, size), 0);

	return p;
}

static void fill_buf(struct intel_buf *buf, uint8_t color)
{
	uint8_t *ptr;
	int i915 = buf_ops_get_fd(buf->bops);
	int i;

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					buf->surface[0].size, PROT_WRITE);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);
}

static void check_buf(struct intel_buf *buf, uint8_t color)
{
	uint8_t *ptr;
	int i915 = buf_ops_get_fd(buf->bops);
	int i;

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					buf->surface[0].size, PROT_READ);

	for (i = 0; i < buf->surface[0].size; i++)
		igt_assert(ptr[i] == color);

	munmap(ptr, buf->surface[0].size);
}


static struct intel_buf *
create_buf(struct buf_ops *bops, int width, int height, uint8_t color)
{
	struct intel_buf *buf;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	intel_buf_init(bops, buf, width/4, height, 32, 0, I915_TILING_NONE, 0);
	fill_buf(buf, color);

	return buf;
}

static void print_buf(struct intel_buf *buf, const char *name)
{
	uint8_t *ptr;
	int i915 = buf_ops_get_fd(buf->bops);

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					buf->surface[0].size, PROT_READ);
	igt_debug("[%s] Buf handle: %d, size: %d, v: 0x%02x, presumed_addr: %p\n",
		  name, buf->handle, buf->surface[0].size, ptr[0],
			from_user_pointer(buf->addr.offset));
	munmap(ptr, buf->surface[0].size);
}

static void simple_bb(struct buf_ops *bops, bool use_context)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	uint32_t ctx;

	if (use_context) {
		gem_require_contexts(i915);
		ctx = gem_context_create(i915);
	}

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	/* Check we're safe with reset and no double-free will occur */
	intel_bb_reset(ibb, true);
	intel_bb_reset(ibb, false);
	intel_bb_reset(ibb, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	if (use_context)
		intel_bb_exec_with_context(ibb, intel_bb_offset(ibb), ctx,
					   I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC,
					   true);

	intel_bb_destroy(ibb);
	if (use_context)
		gem_context_destroy(i915, ctx);
}

#define MI_FLUSH_DW (0x26<<23)
#define BCS_SWCTRL  0x22200
#define BCS_SRC_Y   (1 << 0)
#define BCS_DST_Y   (1 << 1)
static void __emit_blit(struct intel_bb *ibb,
			struct intel_buf *src, struct intel_buf *dst)
{
	uint32_t mask;
	bool has_64b_reloc;

	has_64b_reloc = ibb->gen >= 8;

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
		intel_bb_out(ibb, BCS_SWCTRL);

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst->tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		intel_bb_out(ibb, mask);
	}

	intel_bb_out(ibb,
		     XY_SRC_COPY_BLT_CMD |
		     XY_SRC_COPY_BLT_WRITE_ALPHA |
		     XY_SRC_COPY_BLT_WRITE_RGB |
		     (6 + 2 * has_64b_reloc));
	intel_bb_out(ibb, 3 << 24 | 0xcc << 16 | dst->surface[0].stride);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, intel_buf_height(dst) << 16 | intel_buf_width(dst));
	intel_bb_emit_reloc_fenced(ibb, dst->handle,
				   I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER,
				   0, dst->addr.offset);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, src->surface[0].stride);
	intel_bb_emit_reloc_fenced(ibb, src->handle,
				   I915_GEM_DOMAIN_RENDER, 0,
				   0, src->addr.offset);

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		igt_assert(ibb->gen >= 6);
		intel_bb_out(ibb, MI_FLUSH_DW | 2);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);

		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
		intel_bb_out(ibb, BCS_SWCTRL);
		intel_bb_out(ibb, (BCS_SRC_Y | BCS_DST_Y) << 16);
	}
}

static void blit(struct buf_ops *bops,
		 enum reloc_objects reloc_obj,
		 enum obj_cache_ops cache_op)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst;
	uint64_t poff_bb, poff_src, poff_dst;
	uint64_t poff2_bb, poff2_src, poff2_dst;
	uint64_t flags = 0;
	bool purge_cache = cache_op == PURGE_CACHE ? true : false;
	bool do_relocs = reloc_obj == RELOC ? true : false;

	src = create_buf(bops, WIDTH, HEIGHT, COLOR_CC);
	dst = create_buf(bops, WIDTH, HEIGHT, COLOR_00);

	if (buf_info) {
		print_buf(src, "src");
		print_buf(dst, "dst");
	}

	if (do_relocs) {
		ibb = intel_bb_create_with_relocs(i915, PAGE_SIZE);
	} else {
		ibb = intel_bb_create(i915, PAGE_SIZE);
		flags |= I915_EXEC_NO_RELOC;
	}

	if (ibb->gen >= 6)
		flags |= I915_EXEC_BLT;

	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	__emit_blit(ibb, src, dst);

	/* We expect initial addresses are zeroed for relocs */
	poff_bb = intel_bb_get_object_offset(ibb, ibb->handle);
	poff_src = intel_bb_get_object_offset(ibb, src->handle);
	poff_dst = intel_bb_get_object_offset(ibb, dst->handle);
	igt_debug("bb  presumed offset: 0x%"PRIx64"\n", poff_bb);
	igt_debug("src presumed offset: 0x%"PRIx64"\n", poff_src);
	igt_debug("dst presumed offset: 0x%"PRIx64"\n", poff_dst);
	if (reloc_obj == RELOC) {
		igt_assert(poff_bb == 0);
		igt_assert(poff_src == 0);
		igt_assert(poff_dst == 0);
	}

	intel_bb_emit_bbe(ibb);
	igt_debug("exec flags: %" PRIX64 "\n", flags);
	intel_bb_exec(ibb, intel_bb_offset(ibb), flags, true);
	check_buf(dst, COLOR_CC);

	poff_bb = intel_bb_get_object_offset(ibb, ibb->handle);
	poff_src = intel_bb_get_object_offset(ibb, src->handle);
	poff_dst = intel_bb_get_object_offset(ibb, dst->handle);
	intel_bb_reset(ibb, purge_cache);

	fill_buf(src, COLOR_77);
	fill_buf(dst, COLOR_00);

	__emit_blit(ibb, src, dst);

	poff2_bb = intel_bb_get_object_offset(ibb, ibb->handle);
	poff2_src = intel_bb_get_object_offset(ibb, src->handle);
	poff2_dst = intel_bb_get_object_offset(ibb, dst->handle);

	igt_debug("purge: %d, relocs: %d\n", purge_cache, do_relocs);
	igt_debug("bb  presumed offset: 0x%"PRIx64"\n", poff_bb);
	igt_debug("src presumed offset: 0x%"PRIx64"\n", poff_src);
	igt_debug("dst presumed offset: 0x%"PRIx64"\n", poff_dst);
	igt_debug("bb2  presumed offset: 0x%"PRIx64"\n", poff2_bb);
	igt_debug("src2 presumed offset: 0x%"PRIx64"\n", poff2_src);
	igt_debug("dst2 presumed offset: 0x%"PRIx64"\n", poff2_dst);
	if (purge_cache) {
		if (do_relocs) {
			igt_assert(poff2_bb == 0);
			igt_assert(poff2_src == 0);
			igt_assert(poff2_dst == 0);
		} else {
			igt_assert(poff_bb != poff2_bb);
			igt_assert(poff_src != poff2_src);
			igt_assert(poff_dst != poff2_dst);
		}
	} else {
		igt_assert(poff_bb == poff2_bb);
		igt_assert(poff_src == poff2_src);
		igt_assert(poff_dst == poff2_dst);
	}

	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb), flags, true);
	check_buf(dst, COLOR_77);

	poff2_src = intel_bb_get_object_offset(ibb, src->handle);
	poff2_dst = intel_bb_get_object_offset(ibb, dst->handle);
	igt_assert(poff_src == poff2_src);
	igt_assert(poff_dst == poff2_dst);

	intel_buf_close(bops, src);
	intel_buf_close(bops, dst);
	intel_bb_destroy(ibb);
}

static void scratch_buf_init(struct buf_ops *bops,
			     struct intel_buf *buf,
			     int width, int height,
			     uint32_t req_tiling,
			     enum i915_compression compression)
{
	int bpp = 32;

	intel_buf_init(bops, buf, width, height, bpp, 0,
		       req_tiling, compression);

	igt_assert(intel_buf_width(buf) == width);
	igt_assert(intel_buf_height(buf) == height);
}

static void scratch_buf_draw_pattern(struct buf_ops *bops,
				     struct intel_buf *buf,
				     int x, int y, int w, int h,
				     int cx, int cy, int cw, int ch,
				     bool use_alternate_colors)
{
	cairo_surface_t *surface;
	cairo_pattern_t *pat;
	cairo_t *cr;
	void *linear;

	linear = alloc_aligned(buf->surface[0].size);

	surface = cairo_image_surface_create_for_data(linear,
						      CAIRO_FORMAT_RGB24,
						      intel_buf_width(buf),
						      intel_buf_height(buf),
						      buf->surface[0].stride);

	cr = cairo_create(surface);

	cairo_rectangle(cr, cx, cy, cw, ch);
	cairo_clip(cr);

	pat = cairo_pattern_create_mesh();
	cairo_mesh_pattern_begin_patch(pat);
	cairo_mesh_pattern_move_to(pat, x,   y);
	cairo_mesh_pattern_line_to(pat, x+w, y);
	cairo_mesh_pattern_line_to(pat, x+w, y+h);
	cairo_mesh_pattern_line_to(pat, x,   y+h);
	if (use_alternate_colors) {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 0.0, 1.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 1.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 1.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 0.0, 0.0, 0.0);
	} else {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 1.0, 0.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 0.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 0.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 1.0, 1.0, 1.0);
	}
	cairo_mesh_pattern_end_patch(pat);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);

	linear_to_intel_buf(bops, buf, linear);

	free(linear);
}

#define GROUP_SIZE 4096
static int compare_detail(const uint32_t *ptr1, uint32_t *ptr2,
			  uint32_t size)
{
	int i, ok = 0, fail = 0;
	int groups = size / GROUP_SIZE;
	int *hist = calloc(GROUP_SIZE, groups);

	igt_debug("size: %d, group_size: %d, groups: %d\n",
		  size, GROUP_SIZE, groups);

	for (i = 0; i < size / sizeof(uint32_t); i++) {
		if (ptr1[i] == ptr2[i]) {
			ok++;
		} else {
			fail++;
			hist[i * sizeof(uint32_t) / GROUP_SIZE]++;
		}
	}

	for (i = 0; i < groups; i++) {
		if (hist[i]) {
			igt_debug("[group %4x]: %d\n", i, hist[i]);
		}
	}
	free(hist);

	igt_debug("ok: %d, fail: %d\n", ok, fail);

	return fail;
}

static int compare_bufs(struct intel_buf *buf1, struct intel_buf *buf2,
			 bool detail_compare)
{
	void *ptr1, *ptr2;
	int fd1, fd2, ret;

	igt_assert(buf1->surface[0].size == buf2->surface[0].size);

	fd1 = buf_ops_get_fd(buf1->bops);
	fd2 = buf_ops_get_fd(buf2->bops);

	ptr1 = gem_mmap__device_coherent(fd1, buf1->handle, 0,
					 buf1->surface[0].size, PROT_READ);
	ptr2 = gem_mmap__device_coherent(fd2, buf2->handle, 0,
					 buf2->surface[0].size, PROT_READ);
	ret = memcmp(ptr1, ptr2, buf1->surface[0].size);
	if (detail_compare)
		ret = compare_detail(ptr1, ptr2, buf1->surface[0].size);

	munmap(ptr1, buf1->surface[0].size);
	munmap(ptr2, buf2->surface[0].size);

	return ret;
}

static int __do_intel_bb_blit(struct buf_ops *bops, uint32_t tiling)
{
	struct intel_bb *ibb;
	const int width = 1024;
	const int height = 1024;
	struct intel_buf src, dst, final;
	char name[128];
	int i915 = buf_ops_get_fd(bops), fails;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	scratch_buf_init(bops, &src, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &dst, width, height, tiling,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &final, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);

	if (buf_info) {
		intel_buf_print(&src);
		intel_buf_print(&dst);
	}

	scratch_buf_draw_pattern(bops, &src,
				 0, 0, width, height,
				 0, 0, width, height, 0);

	intel_bb_blt_copy(ibb,
			  &src, 0, 0, src.surface[0].stride,
			  &dst, 0, 0, dst.surface[0].stride,
			  intel_buf_width(&dst),
			  intel_buf_height(&dst),
			  dst.bpp);

	intel_bb_blt_copy(ibb,
			  &dst, 0, 0, dst.surface[0].stride,
			  &final, 0, 0, final.surface[0].stride,
			  intel_buf_width(&dst),
			  intel_buf_height(&dst),
			  dst.bpp);

	igt_assert(intel_bb_sync(ibb) == 0);
	intel_bb_destroy(ibb);

	if (write_png) {
		snprintf(name, sizeof(name) - 1,
			 "bb_blit_dst_tiling_%d.png", tiling);
		intel_buf_write_to_png(&src, "bb_blit_src_tiling_none.png");
		intel_buf_write_to_png(&dst, name);
		intel_buf_write_to_png(&final, "bb_blit_final_tiling_none.png");
	}

	/* We'll fail on src <-> final compare so just warn */
	if (tiling == I915_TILING_NONE) {
		if (compare_bufs(&src, &dst, false) > 0)
			igt_warn("none->none blit failed!");
	} else {
		if (compare_bufs(&src, &dst, false) == 0)
			igt_warn("none->tiled blit failed!");
	}

	fails = compare_bufs(&src, &final, true);

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_buf_close(bops, &final);

	return fails;
}

static void do_intel_bb_blit(struct buf_ops *bops, int loops, uint32_t tiling)
{
	int i, fails = 0, i915 = buf_ops_get_fd(bops);

	gem_require_blitter(i915);

	/* We'll fix it for gen2/3 later. */
	igt_require(intel_gen(intel_get_drm_devid(i915)) > 3);

	for (i = 0; i < loops; i++) {
		fails += __do_intel_bb_blit(bops, tiling);
	}
	igt_assert_f(fails == 0, "intel-bb-blit (tiling: %d) fails: %d\n",
		     tiling, fails);
}

static void offset_control(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst1, *dst2, *dst3;
	uint64_t poff_src, poff_dst1, poff_dst2;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	src = create_buf(bops, WIDTH, HEIGHT, COLOR_CC);
	dst1 = create_buf(bops, WIDTH, HEIGHT, COLOR_00);
	dst2 = create_buf(bops, WIDTH, HEIGHT, COLOR_77);

	intel_bb_add_object(ibb, src->handle, src->addr.offset, false);
	intel_bb_add_object(ibb, dst1->handle, dst1->addr.offset, true);
	intel_bb_add_object(ibb, dst2->handle, dst2->addr.offset, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	if (buf_info) {
		print_buf(src, "src ");
		print_buf(dst1, "dst1");
		print_buf(dst2, "dst2");
	}

	igt_assert(intel_bb_object_offset_to_buf(ibb, src) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst1) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst2) == true);
	poff_src = src->addr.offset;
	poff_dst1 = dst1->addr.offset;
	poff_dst2 = dst2->addr.offset;
	intel_bb_reset(ibb, true);

	dst3 = create_buf(bops, WIDTH, HEIGHT, COLOR_33);
	intel_bb_add_object(ibb, dst3->handle, dst3->addr.offset, true);
	intel_bb_add_object(ibb, src->handle, src->addr.offset, false);
	intel_bb_add_object(ibb, dst1->handle, dst1->addr.offset, true);
	intel_bb_add_object(ibb, dst2->handle, dst2->addr.offset, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);
	intel_bb_sync(ibb);

	igt_assert(intel_bb_object_offset_to_buf(ibb, src) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst1) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst2) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst3) == true);
	igt_assert(poff_src == src->addr.offset);
	igt_assert(poff_dst1 == dst1->addr.offset);
	igt_assert(poff_dst2 == dst2->addr.offset);

	if (buf_info) {
		print_buf(src, "src ");
		print_buf(dst1, "dst1");
		print_buf(dst2, "dst2");
	}

	intel_bb_destroy(ibb);
}

static void full_batch(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	int i;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	for (i = 0; i < PAGE_SIZE / sizeof(uint32_t) - 1; i++)
		intel_bb_out(ibb, 0);
	intel_bb_emit_bbe(ibb);

	igt_assert(intel_bb_offset(ibb) == PAGE_SIZE);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	intel_bb_destroy(ibb);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'd':
		debug_bb = true;
		break;
	case 'p':
		write_png = true;
		break;
	case 'i':
		buf_info = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -d\tDebug bb\n"
	"  -p\tWrite surfaces to png\n"
	"  -i\tPrint buffer info\n"
	;

igt_main_args("dpi", NULL, help_str, opt_handler, NULL)
{
	int i915;
	struct buf_ops *bops;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		bops = buf_ops_create(i915);
	}

	igt_subtest("simple-bb")
		simple_bb(bops, false);

	igt_subtest("simple-bb-ctx")
		simple_bb(bops, true);

	igt_subtest("blit-noreloc-keep-cache")
		blit(bops, NORELOC, KEEP_CACHE);

	igt_subtest("blit-reloc-purge-cache")
		blit(bops, RELOC, PURGE_CACHE);

	igt_subtest("blit-noreloc-purge-cache")
		blit(bops, NORELOC, PURGE_CACHE);

	igt_subtest("blit-reloc-keep-cache")
		blit(bops, RELOC, KEEP_CACHE);

	igt_subtest("intel-bb-blit-none")
		do_intel_bb_blit(bops, 10, I915_TILING_NONE);

	igt_subtest("intel-bb-blit-x")
		do_intel_bb_blit(bops, 10, I915_TILING_X);

	igt_subtest("intel-bb-blit-y") {
		igt_require(intel_gen(intel_get_drm_devid(i915)) >= 6);
		do_intel_bb_blit(bops, 10, I915_TILING_Y);
	}

	igt_subtest("offset-control")
		offset_control(bops);

	igt_subtest("full-batch")
		full_batch(bops);

	igt_fixture {
		buf_ops_destroy(bops);
		close(i915);
	}
}
