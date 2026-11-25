#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "iff.h"

uint8_t *file_slurp(const char *path, size_t *len, size_t max)
{
	struct stat st;
	uint8_t *buf;
	size_t got = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) {
		close(fd);
		return NULL;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		return NULL;
	}
	while (got < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);

		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	if (got != (size_t)st.st_size) {
		free(buf);
		return NULL;
	}
	buf[got] = 0;
	*len = got;
	return buf;
}

const uint8_t *iff_chunk(const uint8_t *buf, size_t len, uint32_t type,
			 uint32_t id, uint32_t *chunklen)
{
	size_t formlen, at;

	if (len < 12 || be32(buf) != IFF_ID('F', 'O', 'R', 'M'))
		return NULL;
	formlen = be32(buf + 4);
	if (formlen + 8 > len)
		formlen = len - 8;
	if (type && be32(buf + 8) != type)
		return NULL;
	for (at = 12; at + 8 <= formlen + 8;) {
		uint32_t cid = be32(buf + at), clen = be32(buf + at + 4);

		if (clen > formlen + 8 - at - 8)
			return NULL;
		if (cid == id) {
			*chunklen = clen;
			return buf + at + 8;
		}
		at += 8 + clen + (clen & 1);
	}
	return NULL;
}

/* ByteRun1: n >= 0 copy n+1, n < 0 repeat -n+1, -128 nop */
static int unpack_row(const uint8_t **src, const uint8_t *end, uint8_t *dst, int n)
{
	const uint8_t *s = *src;

	while (n > 0) {
		int8_t c;

		if (s >= end)
			return -1;
		c = (int8_t)*s++;
		if (c >= 0) {
			int k = c + 1;

			if (k > n || s + k > end)
				return -1;
			memcpy(dst, s, (size_t)k);
			s += k;
			dst += k;
			n -= k;
		} else if (c != -128) {
			int k = -c + 1;

			if (k > n || s >= end)
				return -1;
			memset(dst, *s++, (size_t)k);
			dst += k;
			n -= k;
		}
	}
	*src = s;
	return 0;
}

int ilbm_decode(const uint8_t *buf, size_t len, struct ilbm *out)
{
	const uint8_t *bmhd, *cmap, *body, *grab, *end;
	uint32_t bmhdlen, cmaplen, bodylen, grablen;
	int w, h, depth, masking, comp, rowbytes, planes, x, y, p;
	uint8_t *row;

	memset(out, 0, sizeof(*out));
	out->transparent = -1;
	bmhd = iff_chunk(buf, len, IFF_ID('I', 'L', 'B', 'M'), IFF_ID('B', 'M', 'H', 'D'), &bmhdlen);
	body = iff_chunk(buf, len, IFF_ID('I', 'L', 'B', 'M'), IFF_ID('B', 'O', 'D', 'Y'), &bodylen);
	if (!bmhd || bmhdlen < 20 || !body) {
		size_t at;

		/* some tools wrap the picture: an outer FORM with an ANNO, then a FORM ILBM */
		for (at = 12; at + 12 <= len; at += 2)
			if (be32(buf + at) == IFF_ID('F', 'O', 'R', 'M') &&
			    be32(buf + at + 8) == IFF_ID('I', 'L', 'B', 'M'))
				return ilbm_decode(buf + at, len - at, out);
		return -EINVAL;
	}
	w = be16(bmhd);
	h = be16(bmhd + 2);
	depth = bmhd[8];
	masking = bmhd[9];
	comp = bmhd[10];
	if (masking == 2)
		out->transparent = be16(bmhd + 12);
	if (w < 1 || h < 1 || w > 4096 || h > 4096 || depth < 1 || depth > 8 || comp > 1)
		return -EINVAL;
	out->depth = depth;

	cmap = iff_chunk(buf, len, IFF_ID('I', 'L', 'B', 'M'), IFF_ID('C', 'M', 'A', 'P'), &cmaplen);
	if (cmap) {
		out->ncolours = (int)(cmaplen / 3) > 256 ? 256 : (int)(cmaplen / 3);
		memcpy(out->cmap, cmap, (size_t)out->ncolours * 3);
	}
	grab = iff_chunk(buf, len, IFF_ID('I', 'L', 'B', 'M'), IFF_ID('G', 'R', 'A', 'B'), &grablen);
	if (grab && grablen >= 4) {
		out->grabx = (int16_t)be16(grab);
		out->graby = (int16_t)be16(grab + 2);
	}

	if (bm_alloc(&out->bm, w, h))
		return -ENOMEM;
	if (masking == 1 && bm_alloc(&out->mask, w, h)) {
		bm_free(&out->bm);
		return -ENOMEM;
	}
	rowbytes = ((w + 15) / 16) * 2;
	planes = depth + (masking == 1 ? 1 : 0);
	row = malloc((size_t)rowbytes);
	if (!row) {
		ilbm_free(out);
		return -ENOMEM;
	}
	end = body + bodylen;
	for (y = 0; y < h; y++) {
		for (p = 0; p < planes; p++) {
			if (comp) {
				if (unpack_row(&body, end, row, rowbytes))
					goto done;
			} else {
				if (body + rowbytes > end)
					goto done;
				memcpy(row, body, (size_t)rowbytes);
				body += rowbytes;
			}
			for (x = 0; x < w; x++) {
				if (!(row[x >> 3] & (0x80 >> (x & 7))))
					continue;
				if (p < depth)
					out->bm.px[(size_t)y * out->bm.stride + x] |= (uint8_t)(1 << p);
				else
					out->mask.px[(size_t)y * out->mask.stride + x] = 1;
			}
		}
	}
done:
	free(row);
	return 0;
}

void ilbm_free(struct ilbm *ilbm)
{
	bm_free(&ilbm->bm);
	if (ilbm->mask.px)
		bm_free(&ilbm->mask);
}
