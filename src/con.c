/*
 * CON: windows. A program runs on a pseudo terminal; what it prints is
 * drawn in the screen font with the Workbench pens, and keys typed while
 * the window is active go to it. Escape sequences: enough VT100 for busybox
 * (line editing, clear, colours), mapped onto Workbench pens.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "con.h"
#include "font.h"

#define MAXCON 32

struct con {
	struct con *next;
	struct screen *s;
	struct win *w;
	char title[96];
	struct rect r;
	int fd;
	pid_t pid;
	int exited, status;
	int auto_open, interactive;
	int cols, rows;
	uint8_t *ch, *pen;	/* rows * cols */
	uint8_t *dirty;		/* per row */
	int cx, cy;
	int fg, inverse, bold;
	int esc, inter;
	char escbuf[40];
	int esclen;
	int scrolled;
};

static struct con *cons;

static int cellw(const struct con *c)
{
	return c->s->font->xsize;
}

static int cellh(const struct con *c)
{
	return c->s->font->ysize;
}

static void grid_size(struct con *c, int *cols, int *rows)
{
	struct rect in;

	if (!c->w) {
		*cols = (c->r.w - 22 - 4) / cellw(c);
		*rows = (c->r.h - 13 - 4) / cellh(c);
	} else {
		win_inner(c->w, &in);
		*cols = (in.w - 4) / cellw(c);
		*rows = (in.h - 2) / cellh(c);
	}
	if (*cols < 8)
		*cols = 8;
	if (*rows < 2)
		*rows = 2;
}

static int grid_alloc(struct con *c)
{
	int cols, rows, y, x;
	uint8_t *ch, *pen, *dirty;

	grid_size(c, &cols, &rows);
	ch = malloc((size_t)cols * rows);
	pen = malloc((size_t)cols * rows);
	dirty = calloc((size_t)rows, 1);
	if (!ch || !pen || !dirty) {
		free(ch);
		free(pen);
		free(dirty);
		return -1;
	}
	memset(ch, ' ', (size_t)cols * rows);
	memset(pen, c->s->pens[P_TEXT], (size_t)cols * rows);
	/* keep what fits, anchored at the bottom where the cursor is */
	if (c->ch) {
		int shift = c->cy - (rows - 1);

		if (shift < 0)
			shift = 0;
		for (y = 0; y < rows && y + shift < c->rows; y++)
			for (x = 0; x < cols && x < c->cols; x++) {
				ch[y * cols + x] = c->ch[(y + shift) * c->cols + x];
				pen[y * cols + x] = c->pen[(y + shift) * c->cols + x];
			}
		c->cy -= shift;
	}
	free(c->ch);
	free(c->pen);
	free(c->dirty);
	c->ch = ch;
	c->pen = pen;
	c->dirty = dirty;
	c->cols = cols;
	c->rows = rows;
	if (c->cx >= cols)
		c->cx = cols - 1;
	if (c->cy >= rows)
		c->cy = rows - 1;
	if (c->fd >= 0) {
		struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };

		ioctl(c->fd, TIOCSWINSZ, &ws);
	}
	return 0;
}

static void draw_row(struct con *c, struct rp *rp, int y)
{
	int x, cw = cellw(c), chh = cellh(c);
	int bg = c->s->pens[P_BACKGROUND];

	rp_fill(rp, 0, 2 + y * chh, c->cols * cw + 4, chh, bg);
	for (x = 0; x < c->cols; x++) {
		uint8_t p = c->pen[y * c->cols + x];
		int inv = p & 0x80;
		int fg = p & 0x7f, b = bg;
		char ch = (char)c->ch[y * c->cols + x];

		if (inv) {
			b = fg;
			fg = bg;
		}
		if (y == c->cy && x == c->cx && c->w && c->s->active == c->w && !c->exited) {
			b = c->s->pens[P_FILL];
			fg = c->s->pens[P_SHINE];
		}
		if (b != bg)
			rp_fill(rp, 2 + x * cw, 2 + y * chh, cw, chh, b);
		if (ch != ' ')
			rp_text(rp, c->s->font, 2 + x * cw, 2 + y * chh, &ch, 1, fg, -1);
	}
}

static void con_draw(struct win *w, struct rp *rp)
{
	struct con *c = w->priv;
	struct rect in;
	int y;

	win_inner(w, &in);
	rp_fill(rp, 0, 0, in.w, in.h, c->s->pens[P_BACKGROUND]);
	for (y = 0; y < c->rows; y++)
		draw_row(c, rp, y);
	memset(c->dirty, 0, (size_t)c->rows);
	c->scrolled = 0;
}

/* draw what changed straight into the backing store */
static void flush_rows(struct con *c)
{
	struct rect in, d;
	struct rp rp;
	int y, top = -1, bot = -1;

	if (!c->w)
		return;
	if (c->scrolled) {
		win_refresh(c->s, c->w);
		return;
	}
	rp_init(&rp, &c->w->bm);
	win_inner(c->w, &in);
	rp_clip(&rp, &in);
	rp.ox = in.x;
	rp.oy = in.y;
	for (y = 0; y < c->rows; y++) {
		if (!c->dirty[y])
			continue;
		draw_row(c, &rp, y);
		c->dirty[y] = 0;
		if (top < 0)
			top = y;
		bot = y;
	}
	if (top < 0)
		return;
	d = rect_make(c->w->r.x + in.x, c->w->r.y + in.y + 2 + top * cellh(c), in.w,
		      (bot - top + 1) * cellh(c));
	screen_damage(c->s, &d);
}

static void con_resized(struct win *w)
{
	struct con *c = w->priv;

	grid_alloc(c);
}

static void con_free(struct con *c)
{
	struct con **p;

	for (p = &cons; *p; p = &(*p)->next)
		if (*p == c) {
			*p = c->next;
			break;
		}
	if (c->fd >= 0)
		close(c->fd);
	if (c->pid > 0 && !c->exited)
		kill(c->pid, SIGHUP);
	free(c->ch);
	free(c->pen);
	free(c->dirty);
	free(c);
}

static void con_close(struct win *w)
{
	struct con *c = w->priv;
	struct screen *s = c->s;

	c->w = NULL;
	win_close(s, w);
	con_free(c);
}

static void send(struct con *c, const char *s, size_t n)
{
	if (c->fd >= 0 && !c->exited)
		if (write(c->fd, s, n) < 0)
			return;
}

static void con_key(struct win *w, const struct keyev *k)
{
	struct con *c = w->priv;
	char b[8];

	if (!k->down)
		return;
	switch (k->code) {
	case 28: case 96: send(c, "\r", 1); return;		/* enter */
	case 14: send(c, "\x7f", 1); return;			/* backspace */
	case 103: send(c, "\033[A", 3); return;			/* up */
	case 108: send(c, "\033[B", 3); return;			/* down */
	case 106: send(c, "\033[C", 3); return;			/* right */
	case 105: send(c, "\033[D", 3); return;			/* left */
	case 111: send(c, "\033[3~", 4); return;		/* delete */
	case 102: send(c, "\033[H", 3); return;			/* home */
	case 107: send(c, "\033[F", 3); return;			/* end */
	case 1: send(c, "\033", 1); return;			/* esc */
	case 15: send(c, "\t", 1); return;			/* tab */
	}
	if (!k->ch || (k->qual & (QUAL_LAMIGA | QUAL_RAMIGA)))
		return;
	b[0] = (char)k->ch;
	if ((k->qual & QUAL_CTRL) && ((k->ch >= '@' && k->ch <= '_') || (k->ch >= 'a' && k->ch <= 'z')))
		b[0] = (char)(k->ch & 0x1f);
	send(c, b, 1);
}

static void con_activate(struct win *w, int active)
{
	struct con *c = w->priv;

	(void)active;
	c->dirty[c->cy] = 1;
	flush_rows(c);
}

static void con_button(struct win *w, int x, int y, int b, int d, unsigned int q)
{
	(void)w; (void)x; (void)y; (void)b; (void)d; (void)q;
}

static const struct win_ops con_ops = {
	.draw = con_draw,
	.key = con_key,
	.button = con_button,
	.close = con_close,
	.resized = con_resized,
	.activate = con_activate,
};

static int open_window(struct con *c)
{
	c->w = win_open(c->s, &c->r, WF_STD, c->title, &con_ops, c);
	if (!c->w)
		return -1;
	grid_alloc(c);
	win_activate(c->s, c->w);
	win_refresh(c->s, c->w);
	return 0;
}

/* --- the terminal --- */

static void scroll_up(struct con *c)
{
	memmove(c->ch, c->ch + c->cols, (size_t)c->cols * (c->rows - 1));
	memmove(c->pen, c->pen + c->cols, (size_t)c->cols * (c->rows - 1));
	memset(c->ch + c->cols * (c->rows - 1), ' ', (size_t)c->cols);
	memset(c->pen + c->cols * (c->rows - 1), c->s->pens[P_TEXT], (size_t)c->cols);
	c->scrolled = 1;
}

static void newline(struct con *c)
{
	if (++c->cy >= c->rows) {
		c->cy = c->rows - 1;
		scroll_up(c);
	}
}

static void erase(struct con *c, int from, int to)	/* cell indices, inclusive */
{
	int i;

	for (i = from; i <= to && i < c->cols * c->rows; i++) {
		c->ch[i] = ' ';
		c->pen[i] = (uint8_t)c->s->pens[P_TEXT];
		c->dirty[i / c->cols] = 1;
	}
}

static int cur_pen(struct con *c)
{
	return (c->bold && c->fg == c->s->pens[P_TEXT] ? c->s->pens[P_SHINE] : c->fg) |
	       (c->inverse ? 0x80 : 0);
}

static void sgr(struct con *c, int *p, int n)
{
	/*
	 * As the Amiga's console.device does it: ESC[3nm is pen n. So a Linux
	 * "green" executable comes out white and a "blue" directory in pen 4;
	 * readable on Workbench grey, which ANSI's own colours would not be.
	 * 30 is pen 0 on the Amiga, invisible text; a Linux program means black.
	 */
	static const int map[8] = { 1, 1, 2, 3, 4, 5, 6, 7 };
	int i;

	if (!n) {
		p[0] = 0;
		n = 1;
	}
	for (i = 0; i < n; i++) {
		if (p[i] == 0) {
			c->fg = c->s->pens[P_TEXT];
			c->inverse = c->bold = 0;
		} else if (p[i] == 1) {
			c->bold = 1;
		} else if (p[i] == 7) {
			c->inverse = 1;
		} else if (p[i] == 27) {
			c->inverse = 0;
		} else if (p[i] == 22) {
			c->bold = 0;
		} else if (p[i] >= 30 && p[i] <= 37) {
			c->fg = map[p[i] - 30] < (1 << c->s->depth) ? map[p[i] - 30] : 1;
		} else if (p[i] == 39) {
			c->fg = c->s->pens[P_TEXT];
		}
	}
}

static void csi(struct con *c, char final)
{
	int p[8] = { 0 }, n = 0, priv = 0, i;
	char *s = c->escbuf;

	if (*s == '?') {
		priv = 1;
		s++;
	}
	while (*s && n < 8) {
		p[n++] = atoi(s);
		while (*s && *s != ';')
			s++;
		if (*s == ';')
			s++;
	}
	if (priv)
		return;
	c->dirty[c->cy] = 1;
	switch (final) {
	case 'm': sgr(c, p, n); break;
	case 'K':
		if (p[0] == 0)
			erase(c, c->cy * c->cols + c->cx, c->cy * c->cols + c->cols - 1);
		else if (p[0] == 1)
			erase(c, c->cy * c->cols, c->cy * c->cols + c->cx);
		else
			erase(c, c->cy * c->cols, c->cy * c->cols + c->cols - 1);
		break;
	case 'J':
		if (p[0] == 0)
			erase(c, c->cy * c->cols + c->cx, c->cols * c->rows - 1);
		else if (p[0] == 1)
			erase(c, 0, c->cy * c->cols + c->cx);
		else
			erase(c, 0, c->cols * c->rows - 1);
		break;
	case 'H': case 'f':
		c->cy = (n > 0 && p[0] ? p[0] : 1) - 1;
		c->cx = (n > 1 && p[1] ? p[1] : 1) - 1;
		break;
	case 'A': c->cy -= p[0] ? p[0] : 1; break;
	case 'B': c->cy += p[0] ? p[0] : 1; break;
	case 'C': c->cx += p[0] ? p[0] : 1; break;
	case 'D': c->cx -= p[0] ? p[0] : 1; break;
	case 'G': c->cx = (p[0] ? p[0] : 1) - 1; break;
	case 'd': c->cy = (p[0] ? p[0] : 1) - 1; break;
	case 'P':
		for (i = 0; i < (p[0] ? p[0] : 1); i++) {
			int at = c->cy * c->cols;

			memmove(c->ch + at + c->cx, c->ch + at + c->cx + 1, (size_t)(c->cols - c->cx - 1));
			memmove(c->pen + at + c->cx, c->pen + at + c->cx + 1, (size_t)(c->cols - c->cx - 1));
			c->ch[at + c->cols - 1] = ' ';
		}
		break;
	case '@':
		for (i = 0; i < (p[0] ? p[0] : 1); i++) {
			int at = c->cy * c->cols;

			memmove(c->ch + at + c->cx + 1, c->ch + at + c->cx, (size_t)(c->cols - c->cx - 1));
			memmove(c->pen + at + c->cx + 1, c->pen + at + c->cx, (size_t)(c->cols - c->cx - 1));
			c->ch[at + c->cx] = ' ';
		}
		break;
	case 'n':
		if (p[0] == 6) {
			char b[32];
			int len = snprintf(b, sizeof(b), "\033[%d;%dR", c->cy + 1, c->cx + 1);

			send(c, b, (size_t)len);
		}
		break;
	}
	if (c->cx < 0)
		c->cx = 0;
	if (c->cy < 0)
		c->cy = 0;
	if (c->cx >= c->cols)
		c->cx = c->cols - 1;
	if (c->cy >= c->rows)
		c->cy = c->rows - 1;
	c->dirty[c->cy] = 1;
}

static void output(struct con *c, const char *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)buf[i];

		if (c->esc == 1) {
			if (ch == '[') {
				c->esc = 2;
				c->esclen = 0;
				c->inter = 0;
			} else if (ch == ']') {
				c->esc = 3;
			} else {
				c->esc = 0;
			}
			continue;
		}
		if (c->esc == 2) {
			/* ECMA-48: parameters 0x30-0x3f, intermediates 0x20-0x2f, final 0x40-0x7e */
			if (ch >= 0x30 && ch <= 0x3f) {
				if (c->esclen < (int)sizeof(c->escbuf) - 1)
					c->escbuf[c->esclen++] = (char)ch;
				continue;
			}
			if (ch >= 0x20 && ch <= 0x2f) {
				c->inter = 1;
				continue;
			}
			c->escbuf[c->esclen] = 0;
			c->esc = 0;
			if (ch >= 0x40 && ch <= 0x7e && !c->inter)
				csi(c, (char)ch);
			c->inter = 0;
			if (ch >= 0x40)
				continue;
			/* a control character in the middle: act on it */
		}
		if (c->esc == 3) {
			if (ch == 7 || ch == '\\')
				c->esc = 0;
			continue;
		}
		c->dirty[c->cy] = 1;
		switch (ch) {
		case 27: c->esc = 1; break;
		case 0x9b: c->esc = 2; c->esclen = 0; c->inter = 0; break;	/* Amiga's CSI */
		case '\r': c->cx = 0; break;
		case '\n': newline(c); break;
		case '\b': if (c->cx > 0) c->cx--; break;
		case '\t': c->cx = (c->cx + 8) & ~7; if (c->cx >= c->cols) c->cx = c->cols - 1; break;
		case 7: break;
		default:
			if (ch < 32 || (ch >= 0x80 && ch < 0xa0))
				break;
			if (c->cx >= c->cols) {
				c->cx = 0;
				newline(c);
			}
			c->ch[c->cy * c->cols + c->cx] = ch;
			c->pen[c->cy * c->cols + c->cx] = (uint8_t)cur_pen(c);
			c->cx++;
			if (c->cx >= c->cols) {
				/* wrap lazily, as a VT100 does */
				c->cx = c->cols;
			}
			break;
		}
		c->dirty[c->cy < c->rows ? c->cy : c->rows - 1] = 1;
	}
	if (c->cx >= c->cols)
		c->cx = c->cols - 1;
}

struct con *con_spawn(struct screen *s, const struct rect *r, const char *title,
		      const char *dir, const char *const argv[], int auto_open, int *err)
{
	static int shells;
	struct con *c = calloc(1, sizeof(*c));
	int errpipe[2], master, e = 0, shellno = 0;
	char *slave;
	ssize_t n;
	pid_t pid;

	if (err)
		*err = 0;
	if (!c)
		return NULL;
	c->s = s;
	c->r = *r;
	c->fd = -1;
	c->fg = s->pens[P_TEXT];
	c->auto_open = auto_open;
	c->interactive = !argv;
	snprintf(c->title, sizeof(c->title), "%s", title);
	if (grid_alloc(c)) {
		free(c);
		return NULL;
	}

	if (!argv)
		shellno = ++shells;
	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) || unlockpt(master) || !(slave = ptsname(master)) ||
	    pipe2(errpipe, O_CLOEXEC)) {
		if (master >= 0)
			close(master);
		free(c->ch);
		free(c->pen);
		free(c->dirty);
		free(c);
		return NULL;
	}
	{
		struct winsize ws = { (unsigned short)c->rows, (unsigned short)c->cols, 0, 0 };

		ioctl(master, TIOCSWINSZ, &ws);
	}
	pid = fork();
	if (pid == 0) {
		int fd;
		char cols[16], rows[16];

		setsid();
		fd = open(slave, O_RDWR);
		if (fd < 0)
			_exit(127);
		ioctl(fd, TIOCSCTTY, 0);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		if (dir && chdir(dir))
			if (chdir("/"))
				_exit(127);
		snprintf(cols, sizeof(cols), "%d", c->cols);
		snprintf(rows, sizeof(rows), "%d", c->rows);
		setenv("TERM", "vt102", 1);
		if (!argv) {
			/* AmigaShell's prompt is "%N.%S> ": the CLI number and the directory */
			static int cli;
			char ps1[32];

			snprintf(ps1, sizeof(ps1), "%d.\\w> ", shellno);
			setenv("PS1", ps1, 1);
			(void)cli;
		}
		setenv("COLUMNS", cols, 1);
		setenv("LINES", rows, 1);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		if (argv) {
			execv(argv[0], (char *const *)argv);
		} else {
			const char *sh = "/bin/sh";

			execl(sh, "sh", (char *)NULL);	/* not a login shell: /etc/profile would set PS1 */
		}
		e = errno;
		if (write(errpipe[1], &e, sizeof(e)) < 0)
			_exit(127);
		_exit(127);
	}
	close(errpipe[1]);
	if (pid < 0) {
		close(errpipe[0]);
		close(master);
		free(c->ch);
		free(c->pen);
		free(c->dirty);
		free(c);
		return NULL;
	}
	n = read(errpipe[0], &e, sizeof(e));
	close(errpipe[0]);
	c->fd = master;
	c->pid = pid;
	c->next = cons;
	cons = c;
	if (n == (ssize_t)sizeof(e)) {
		/* exec failed: nothing to show, the caller says so */
		if (err)
			*err = e;
		c->exited = 1;
		waitpid(pid, NULL, 0);
		c->pid = 0;
		con_free(c);
		return NULL;
	}
	fcntl(master, F_SETFL, O_NONBLOCK | fcntl(master, F_GETFL));
	if (!auto_open && open_window(c)) {
		con_free(c);
		return NULL;
	}
	return c;
}

int con_pollfds(struct pollfd *fds, int max)
{
	struct con *c;
	int n = 0;

	for (c = cons; c && n < max; c = c->next) {
		if (c->fd < 0)
			continue;
		fds[n].fd = c->fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
	}
	return n;
}

static void finished(struct con *c)
{
	char t[128];

	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	if (!c->w) {
		con_free(c);
		return;
	}
	if (c->interactive) {
		con_close(c->w);
		return;
	}
	snprintf(t, sizeof(t), "%s [done]", c->title);
	win_set_title(c->s, c->w, t);
	c->dirty[c->cy] = 1;
	flush_rows(c);
}

void con_service(struct screen *s, const struct pollfd *fds, int n)
{
	struct con *c, *next;
	int i;

	(void)s;
	for (i = 0; i < n; i++) {
		if (!fds[i].revents)
			continue;
		for (c = cons; c; c = next) {
			char buf[2048];
			ssize_t got;

			next = c->next;
			if (c->fd != fds[i].fd)
				continue;
			got = read(c->fd, buf, sizeof(buf));
			if (got <= 0) {
				if (got < 0 && errno == EAGAIN)
					break;
				finished(c);
				break;
			}
			if (!c->w && open_window(c)) {
				finished(c);
				break;
			}
			output(c, buf, (size_t)got);
			flush_rows(c);
			break;
		}
	}
}

void con_reap(struct screen *s)
{
	struct con *c;
	int status;
	pid_t pid;

	(void)s;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		for (c = cons; c; c = c->next)
			if (c->pid == pid) {
				c->exited = 1;
				c->status = status;
			}
}
