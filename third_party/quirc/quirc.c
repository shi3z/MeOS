/* quirc -- QR-code recognition library
 * Copyright (C) 2010-2012 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
/* MeOS: the big buffers (a camera frame's worth) come from PSRAM, not newlib's malloc (see kernel/memory/kmalloc.c). */
#include <m5os/kmalloc.h>
static inline void *m5os_qr_malloc(size_t n) { return kmalloc_psram(n); }
static inline void *m5os_qr_calloc(size_t a, size_t b) { void *p = kmalloc_psram(a * b); if (p) { char *c = p; for (size_t i = 0; i < a * b; i++) c[i] = 0; } return p; }
#define malloc(n)    m5os_qr_malloc(n)
#define calloc(a, b) m5os_qr_calloc(a, b)
#define free(p)      kfree(p)

#include <string.h>
#include "quirc_internal.h"

const char *quirc_version(void)
{
	return "1.0";
}

struct quirc *quirc_new(void)
{
	struct quirc *q = malloc(sizeof(*q));

	if (!q)
		return NULL;

	memset(q, 0, sizeof(*q));
	return q;
}

void quirc_destroy(struct quirc *q)
{
	free(q->image);
	/* q->pixels may alias q->image when their type representation is of the
	   same size, so we need to be careful here to avoid a double free */
	if (!QUIRC_PIXEL_ALIAS_IMAGE)
		free(q->pixels);
	free(q->flood_fill_vars);
	free(q);
}

int quirc_resize(struct quirc *q, int w, int h)
{
	uint8_t		*image  = NULL;
	quirc_pixel_t	*pixels = NULL;
	size_t num_vars;
	size_t vars_byte_size;
	struct quirc_flood_fill_vars *vars = NULL;

	/*
	 * XXX: w and h should be size_t (or at least unsigned) as negatives
	 * values would not make much sense. The downside is that it would break
	 * both the API and ABI. Thus, at the moment, let's just do a sanity
	 * check.
	 */
	if (w < 0 || h < 0)
		goto fail;

	/*
	 * alloc a new buffer for q->image. We avoid realloc(3) because we want
	 * on failure to be leave `q` in a consistant, unmodified state.
	 */
	image = calloc(w, h);
	if (!image)
		goto fail;

	/* compute the "old" (i.e. currently allocated) and the "new"
	   (i.e. requested) image dimensions */
	size_t olddim = q->w * q->h;
	size_t newdim = w * h;
	size_t min = (olddim < newdim ? olddim : newdim);

	/*
	 * copy the data into the new buffer, avoiding (a) to read beyond the
	 * old buffer when the new size is greater and (b) to write beyond the
	 * new buffer when the new size is smaller, hence the min computation.
	 */
	(void)memcpy(image, q->image, min);

	/* alloc a new buffer for q->pixels if needed */
	if (!QUIRC_PIXEL_ALIAS_IMAGE) {
		pixels = calloc(newdim, sizeof(quirc_pixel_t));
		if (!pixels)
			goto fail;
	}

	/*
	 * alloc the work area for the flood filling logic.
	 *
	 * the size was chosen with the following assumptions and observations:
	 *
	 * - rings are the regions which requires the biggest work area.
	 * - they consumes the most when they are rotated by about 45 degree.
	 *   in that case, the necessary depth is about (2 * height_of_the_ring).
	 * - the maximum height of rings would be about 1/3 of the image height.
	 */

	if ((size_t)h * 2 / 2 != h) {
		goto fail; /* size_t overflow */
	}
	num_vars = (size_t)h * 2 / 3;
	if (num_vars == 0) {
		num_vars = 1;
	}

	vars_byte_size = sizeof(*vars) * num_vars;
	if (vars_byte_size / sizeof(*vars) != num_vars) {
		goto fail; /* size_t overflow */
	}
	vars = malloc(vars_byte_size);
	if (!vars)
		goto fail;

	/* alloc succeeded, update `q` with the new size and buffers */
	q->w = w;
	q->h = h;
	free(q->image);
	q->image = image;
	if (!QUIRC_PIXEL_ALIAS_IMAGE) {
		free(q->pixels);
		q->pixels = pixels;
	}
	free(q->flood_fill_vars);
	q->flood_fill_vars = vars;
	q->num_flood_fill_vars = num_vars;

	return 0;
	/* NOTREACHED */
fail:
	free(image);
	free(pixels);
	free(vars);

	return -1;
}

int quirc_count(const struct quirc *q)
{
	return q->num_grids;
}

static const char *const error_table[] = {
	[QUIRC_SUCCESS] = "Success",
	[QUIRC_ERROR_INVALID_GRID_SIZE] = "Invalid grid size",
	[QUIRC_ERROR_INVALID_VERSION] = "Invalid version",
	[QUIRC_ERROR_FORMAT_ECC] = "Format data ECC failure",
	[QUIRC_ERROR_DATA_ECC] = "ECC failure",
	[QUIRC_ERROR_UNKNOWN_DATA_TYPE] = "Unknown data type",
	[QUIRC_ERROR_DATA_OVERFLOW] = "Data overflow",
	[QUIRC_ERROR_DATA_UNDERFLOW] = "Data underflow"
};

const char *quirc_strerror(quirc_decode_error_t err)
{
	if (err >= 0 && err < sizeof(error_table) / sizeof(error_table[0]))
		return error_table[err];

	return "Unknown error";
}
