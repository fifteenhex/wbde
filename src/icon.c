#define _GNU_SOURCE
/*
 * A .info file is struct DiskObject written out field by field, big endian,
 * followed by what its pointers pointed at, in this order:
 *
 *   DiskObject (78 bytes)
 *     0 UWORD do_Magic 0xe310, 2 UWORD do_Version 1
 *     4 struct Gadget (44): +4 Left +6 Top +8 Width +10 Height +12 Flags
 *        +14 Activation +16 GadgetType +18 GadgetRender +22 SelectRender
 *        +44 UserData (low byte: the icon's revision)
 *    48 UBYTE do_Type
 *    50 do_DefaultTool  54 do_ToolTypes  58 do_CurrentX  62 do_CurrentY
 *    66 do_DrawerData   70 do_ToolWindow 74 do_StackSize
 *   DrawerData (56: a NewWindow, then dd_CurrentX/Y), if do_DrawerData
 *   struct Image (20) + planes, for GadgetRender
 *   struct Image (20) + planes, if SelectRender
 *   DefaultTool: ULONG length, bytes (with the NUL), if do_DefaultTool
 *   ToolTypes: ULONG (n + 1) * 4, then n of (ULONG length, bytes)
 *   ToolWindow string, if do_ToolWindow
 *   DrawerData2 (dd_Flags ULONG, dd_ViewModes UWORD) if drawer + revision >= 1
 *   optionally a FORM ICON (OS 3.5 "glow" icons), which wins when present
 *
 * Only whether a pointer is non-zero matters: the values are stale addresses
 * from whatever machine wrote the file.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "icon.h"
#include "iff.h"

#define GFLG_GADGHIMAGE	0x0002
#define GFLG_GADGHMASK	0x0003

struct rd {
	const uint8_t *p;
	size_t len, at;
	int bad;
};

static const uint8_t *take(struct rd *r, size_t n)
{
	const uint8_t *p;

	if (r->bad || n > r->len - r->at) {
		r->bad = 1;
		return NULL;
	}
	p = r->p + r->at;
	r->at += n;
	return p;
}

static uint32_t take32(struct rd *r)
{
	const uint8_t *p = take(r, 4);
	return p ? be32(p) : 0;
}

static char *take_string(struct rd *r)
{
	uint32_t n = take32(r);
	const uint8_t *p;
	char *s;

	if (n > 4096) {
		r->bad = 1;
		return NULL;
	}
	p = take(r, n);
	if (!p)
		return NULL;
	s = malloc(n + 1);
	if (!s)
		return NULL;
	memcpy(s, p, n);
	s[n] = 0;
	return s;
}

static int take_image(struct rd *r, struct bm *bm, int *w, int *h)
{
	const uint8_t *hdr = take(r, 20), *data;
	int width, height, depth, pick, onoff, planes, p;
	size_t bytes;

	if (!hdr)
		return -EINVAL;
	width = (int16_t)be16(hdr + 4);
	height = (int16_t)be16(hdr + 6);
	depth = (int16_t)be16(hdr + 8);
	pick = hdr[14];
	onoff = hdr[15];
	if (width < 1 || height < 1 || width > 1024 || height > 1024 || depth < 0 || depth > 8)
		return -EINVAL;
	/* the planes stored are the ones picked, at most depth of them */
	planes = 0;
	for (p = 0; p < 8; p++)
		if (pick & (1 << p))
			planes++;
	if (planes > depth)
		planes = depth;
	if (!be32(hdr + 10))
		planes = 0;
	bytes = (size_t)((width + 15) / 16) * 2 * height * planes;
	data = take(r, bytes);
	if (!data && bytes)
		return -EINVAL;
	if (bm_alloc(bm, width, height))
		return -ENOMEM;
	planar_to_chunky(bm, data, width, height, planes, pick, onoff);
	if (w)
		*w = width;
	if (h)
		*h = height;
	return 0;
}

/* --- OS 3.5 FORM ICON --- */

struct bits {
	const uint8_t *p, *end;
	unsigned int acc, n;
};

static int getbits(struct bits *b, int n)
{
	while ((int)b->n < n) {
		if (b->p >= b->end)
			return -1;
		b->acc = (b->acc << 8) | *b->p++;
		b->n += 8;
	}
	b->n -= (unsigned int)n;
	return (int)((b->acc >> b->n) & ((1u << n) - 1));
}

/* the ByteRun1 relative that packs values of `depth` bits */
static int glow_unpack(const uint8_t *src, size_t srclen, int depth, int rle,
		       uint8_t *dst, size_t want)
{
	struct bits b = { src, src + srclen, 0, 0 };
	size_t at = 0;

	if (!rle) {
		while (at < want) {
			int v = getbits(&b, depth);

			if (v < 0)
				return -1;
			dst[at++] = (uint8_t)v;
		}
		return 0;
	}
	while (at < want) {
		int c = getbits(&b, 8), v, k;

		if (c < 0)
			return -1;
		if (c < 128) {
			for (k = 0; k <= c && at < want; k++) {
				v = getbits(&b, depth);
				if (v < 0)
					return -1;
				dst[at++] = (uint8_t)v;
			}
		} else if (c > 128) {
			v = getbits(&b, depth);
			if (v < 0)
				return -1;
			for (k = 0; k < 257 - c && at < want; k++)
				dst[at++] = (uint8_t)v;
		}
	}
	return 0;
}

static int glow_image(const uint8_t *imag, uint32_t len, int w, int h,
		      uint8_t pal[256][3], int *npal, struct bm *bm, int *key,
		      icon_penfn pen, void *penarg)
{
	int trans, ncol, flags, ifmt, pfmt, depth, isize, psize, i;
	uint8_t pens[256], *idx;

	if (len < 10)
		return -EINVAL;
	trans = imag[0];
	ncol = imag[1] + 1;
	flags = imag[2];
	ifmt = imag[3];
	pfmt = imag[4];
	depth = imag[5];
	isize = be16(imag + 6) + 1;
	psize = be16(imag + 8) + 1;
	if (depth < 1 || depth > 8 || 10 + (uint32_t)isize > len)
		return -EINVAL;

	if (flags & 2) {
		uint8_t raw[768];

		if (10 + (uint32_t)isize + (uint32_t)psize > len || ncol > 256)
			return -EINVAL;
		if (glow_unpack(imag + 10 + isize, (size_t)psize, 8, pfmt, raw, (size_t)ncol * 3))
			return -EINVAL;
		memcpy(pal, raw, (size_t)ncol * 3);
		*npal = ncol;
	}
	if (!*npal)
		return -EINVAL;

	idx = malloc((size_t)w * h);
	if (!idx)
		return -ENOMEM;
	if (glow_unpack(imag + 10, (size_t)isize, depth, ifmt, idx, (size_t)w * h)) {
		free(idx);
		return -EINVAL;
	}
	if (bm_alloc(bm, w, h)) {
		free(idx);
		return -ENOMEM;
	}
	for (i = 0; i < 256; i++)
		pens[i] = 0;
	for (i = 0; i < *npal; i++)
		pens[i] = (uint8_t)(pen ? pen(penarg, pal[i][0], pal[i][1], pal[i][2]) : i);
	/* see-through pixels become the key, which is a pen nothing else uses */
	*key = (flags & 1) ? 255 : -1;
	for (i = 0; i < w * h; i++) {
		uint8_t v = idx[i];

		bm->px[(size_t)(i / w) * bm->stride + i % w] =
			((flags & 1) && v == trans) ? 255 : pens[v];
	}
	free(idx);
	return 0;
}

static int parse_glow(const uint8_t *form, size_t len, struct icon *ic,
		      icon_penfn pen, void *penarg)
{
	uint8_t pal[256][3];
	const uint8_t *face, *imag;
	uint32_t facelen, imaglen, formlen, at;
	struct bm img[2] = { { 0 }, { 0 } };
	int w, h, n = 0, npal = 0, key = -1;

	face = iff_chunk(form, len, IFF_ID('I', 'C', 'O', 'N'), IFF_ID('F', 'A', 'C', 'E'), &facelen);
	if (!face || facelen < 6)
		return -EINVAL;
	w = face[0] + 1;
	h = face[1] + 1;

	/* IMAG chunks in order: normal, then selected */
	formlen = be32(form + 4);
	if (formlen + 8 > len)
		formlen = (uint32_t)len - 8;
	for (at = 12; at + 8 <= formlen + 8 && n < 2;) {
		uint32_t cid = be32(form + at), clen = be32(form + at + 4);

		if (clen > formlen + 8 - at - 8)
			break;
		imag = form + at + 8;
		imaglen = clen;
		if (cid == IFF_ID('I', 'M', 'A', 'G') &&
		    !glow_image(imag, imaglen, w, h, pal, &npal, &img[n], &key, pen, penarg))
			n++;
		at += 8 + clen + (clen & 1);
	}
	if (!n)
		return -EINVAL;

	bm_free(&ic->img[0]);
	if (ic->img[1].px)
		bm_free(&ic->img[1]);
	ic->img[0] = img[0];
	ic->img[1] = img[1];
	ic->w = w;
	ic->h = h;
	ic->key = key;
	ic->glow = 1;
	ic->frameless = face[2] & 1;
	return 0;
}

int icon_parse(const uint8_t *buf, size_t len, struct icon *ic,
	       icon_penfn pen, void *penarg)
{
	struct rd r = { buf, len, 0, 0 };
	const uint8_t *d = take(&r, 78);
	uint32_t has_dd, has_sel, has_tool, has_tt, has_tw, rev, flags;
	size_t i;

	memset(ic, 0, sizeof(*ic));
	ic->x = ic->y = ICON_NOPOS;
	ic->key = 0;
	if (!d || be16(d) != 0xe310 || be16(d + 2) != 1)
		return -EINVAL;

	flags = be16(d + 4 + 12);
	has_sel = be32(d + 4 + 22);
	rev = be32(d + 4 + 40) & 0xff;
	ic->type = d[48];
	has_tool = be32(d + 50);
	has_tt = be32(d + 54);
	if (be32(d + 58) != 0x80000000u)
		ic->x = (int32_t)be32(d + 58);
	if (be32(d + 62) != 0x80000000u)
		ic->y = (int32_t)be32(d + 62);
	if (ic->x == ICON_NOPOS || ic->y == ICON_NOPOS)
		ic->x = ic->y = ICON_NOPOS;
	has_dd = be32(d + 66);
	has_tw = be32(d + 70);
	ic->stack = (int32_t)be32(d + 74);

	if (has_dd) {
		const uint8_t *dd = take(&r, 56);

		if (!dd)
			return -EINVAL;
		ic->has_window = 1;
		ic->win = rect_make((int16_t)be16(dd), (int16_t)be16(dd + 2),
				    (int16_t)be16(dd + 4), (int16_t)be16(dd + 6));
		ic->dd_x = (int32_t)be32(dd + 48);
		ic->dd_y = (int32_t)be32(dd + 52);
	}
	if (take_image(&r, &ic->img[0], &ic->w, &ic->h))
		goto bad;
	if (has_sel && (flags & GFLG_GADGHMASK) == GFLG_GADGHIMAGE) {
		if (take_image(&r, &ic->img[1], NULL, NULL))
			goto bad;
	} else if (has_sel) {
		struct bm skip;

		/* an image that is not used for highlighting: read past it */
		if (take_image(&r, &skip, NULL, NULL))
			goto bad;
		bm_free(&skip);
	}
	if (has_tool)
		ic->deftool = take_string(&r);
	if (has_tt && !r.bad) {
		uint32_t n = take32(&r) / 4;

		if (n)
			n--;
		if (n > 256)
			r.bad = 1;
		if (!r.bad) {
			ic->tooltypes = calloc(n + 1, sizeof(char *));
			for (i = 0; ic->tooltypes && i < n && !r.bad; i++) {
				ic->tooltypes[i] = take_string(&r);
				if (ic->tooltypes[i])
					ic->ntooltypes++;
			}
		}
	}
	if (has_tw && !r.bad)
		free(take_string(&r));
	if (has_dd && rev >= 1 && !r.bad) {
		const uint8_t *dd2 = take(&r, 6);

		if (dd2) {
			ic->dd_flags = be32(dd2);
			ic->dd_viewmodes = be16(dd2 + 4);
		}
	}

	/* an OS 3.5 icon carries a FORM ICON after the old image, which wins */
	for (i = r.bad ? 78 : r.at; i + 12 <= len; i++) {
		if (be32(buf + i) == IFF_ID('F', 'O', 'R', 'M') &&
		    be32(buf + i + 8) == IFF_ID('I', 'C', 'O', 'N')) {
			parse_glow(buf + i, len - i, ic, pen, penarg);
			break;
		}
	}
	return 0;
bad:
	icon_free(ic);
	return -EINVAL;
}

int icon_load(const char *path, struct icon *ic, icon_penfn pen, void *penarg)
{
	size_t len;
	uint8_t *buf = file_slurp(path, &len, 1 << 20);
	int ret;

	if (!buf)
		return -errno ? -errno : -EIO;
	ret = icon_parse(buf, len, ic, pen, penarg);
	free(buf);
	return ret;
}

void icon_free(struct icon *ic)
{
	int i;

	bm_free(&ic->img[0]);
	if (ic->img[1].px)
		bm_free(&ic->img[1]);
	free(ic->deftool);
	for (i = 0; ic->tooltypes && ic->tooltypes[i]; i++)
		free(ic->tooltypes[i]);
	free(ic->tooltypes);
	memset(ic, 0, sizeof(*ic));
}

static int bm_dup(struct bm *dst, const struct bm *src)
{
	if (!src->px) {
		memset(dst, 0, sizeof(*dst));
		return 0;
	}
	if (bm_alloc(dst, src->w, src->h))
		return -ENOMEM;
	memcpy(dst->px, src->px, (size_t)src->stride * src->h);
	return 0;
}

int icon_dup(struct icon *dst, const struct icon *src)
{
	int i;

	*dst = *src;
	dst->deftool = src->deftool ? strdup(src->deftool) : NULL;
	dst->tooltypes = NULL;
	dst->ntooltypes = 0;
	if (src->tooltypes) {
		dst->tooltypes = calloc((size_t)src->ntooltypes + 1, sizeof(char *));
		for (i = 0; dst->tooltypes && i < src->ntooltypes; i++)
			dst->tooltypes[i] = strdup(src->tooltypes[i]);
		dst->ntooltypes = src->ntooltypes;
	}
	if (bm_dup(&dst->img[0], &src->img[0]) || bm_dup(&dst->img[1], &src->img[1]))
		return -ENOMEM;
	return 0;
}

const char *icon_tooltype(const struct icon *ic, const char *name)
{
	size_t n = strlen(name);
	int i;

	for (i = 0; i < ic->ntooltypes; i++) {
		const char *t = ic->tooltypes[i];

		if (!t || strncasecmp(t, name, n))
			continue;
		if (t[n] == '=')
			return t + n + 1;
		if (!t[n])
			return t + n;
	}
	return NULL;
}

/*
 * A DiskObject as it sits in memory (icon.library's built-in defaults):
 * the same 78 bytes, but the pointers are real and point at struct Images,
 * strings and the DrawerData.
 */
static int mem_image(const struct memimg *m, uint32_t a, struct bm *bm, int *w, int *h)
{
	int width, height, depth, pick, onoff, planes, p;
	uint32_t data;
	size_t bytes;

	if (!memimg_in(m, a, 20))
		return -EINVAL;
	width = (int16_t)memimg_w(m, a + 4);
	height = (int16_t)memimg_w(m, a + 6);
	depth = (int16_t)memimg_w(m, a + 8);
	data = memimg_l(m, a + 10);
	pick = m->p[a - m->base + 14];
	onoff = m->p[a - m->base + 15];
	if (width < 1 || height < 1 || width > 1024 || height > 1024 || depth < 0 || depth > 8)
		return -EINVAL;
	planes = 0;
	for (p = 0; p < 8; p++)
		if (pick & (1 << p))
			planes++;
	if (planes > depth)
		planes = depth;
	bytes = (size_t)((width + 15) / 16) * 2 * height * planes;
	if (planes && !memimg_in(m, data, (uint32_t)bytes))
		return -EINVAL;
	if (bm_alloc(bm, width, height))
		return -ENOMEM;
	planar_to_chunky(bm, planes ? m->p + (data - m->base) : NULL, width, height,
			 planes, pick, onoff);
	if (w)
		*w = width;
	if (h)
		*h = height;
	return 0;
}

int icon_from_memory(const struct memimg *m, uint32_t a, struct icon *ic)
{
	uint32_t render, select, tool, dd, flags;

	memset(ic, 0, sizeof(*ic));
	ic->x = ic->y = ICON_NOPOS;
	if (!memimg_in(m, a, 78) || memimg_w(m, a) != 0xe310 || memimg_w(m, a + 2) != 1)
		return -EINVAL;
	flags = memimg_w(m, a + 16);
	render = memimg_l(m, a + 22);
	select = memimg_l(m, a + 26);
	ic->type = m->p[a - m->base + 48];
	tool = memimg_l(m, a + 50);
	dd = memimg_l(m, a + 66);
	ic->stack = (int32_t)memimg_l(m, a + 74);
	if (mem_image(m, render, &ic->img[0], &ic->w, &ic->h))
		return -EINVAL;
	if (select && (flags & GFLG_GADGHMASK) == GFLG_GADGHIMAGE &&
	    mem_image(m, select, &ic->img[1], NULL, NULL))
		memset(&ic->img[1], 0, sizeof(ic->img[1]));
	if (tool && memimg_in(m, tool, 1)) {
		uint32_t e = tool;

		while (memimg_in(m, e, 1) && m->p[e - m->base])
			e++;
		ic->deftool = strndup((const char *)m->p + (tool - m->base), e - tool);
	}
	if (dd && memimg_in(m, dd, 56)) {
		ic->has_window = 1;
		ic->win = rect_make((int16_t)memimg_w(m, dd), (int16_t)memimg_w(m, dd + 2),
				    (int16_t)memimg_w(m, dd + 4), (int16_t)memimg_w(m, dd + 6));
	}
	return 0;
}
