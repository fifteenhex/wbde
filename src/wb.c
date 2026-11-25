/*
 * Workbench itself. The disks sit on a backdrop window under everything; a
 * double click opens a drawer window holding the directory's icons: the
 * .info files where there are any, the default icons (from the System
 * disk's icon.library) otherwise. Tools are started by a double click,
 * projects through their default tool. Nothing here ever writes to a disk:
 * the menu items that would are ghosted.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "con.h"
#include "font.h"
#include "iff.h"
#include "wb.h"

#define DCLICK_MS	700
#define PEN(_wb, _p)	((_wb)->s->pens[_p])

enum {
	MN_BACKDROP = 1, MN_EXECUTE, MN_REDRAW, MN_UPDATEALL, MN_LASTMSG, MN_ABOUT, MN_QUIT,
	MN_NEWDRAWER, MN_PARENT, MN_CLOSE, MN_UPDATE, MN_SELECTALL, MN_CLEANUP,
	MN_SNAP_WIN, MN_SNAP_ALL, MN_SHOW_ICONS, MN_SHOW_ALL,
	MN_VIEW_ICON, MN_VIEW_NAME, MN_VIEW_DATE, MN_VIEW_SIZE,
	MN_OPEN, MN_COPY, MN_RENAME, MN_INFO, MN_SNAPSHOT, MN_UNSNAPSHOT, MN_LEAVEOUT,
	MN_PUTAWAY, MN_DELETE, MN_FORMAT, MN_EMPTYTRASH,
	MN_RESETWB, MN_SHELL,
};

static const struct menuitem snap_sub[] = {
	{ "Window", 0, MI_DISABLED, MN_SNAP_WIN, NULL, 0 },
	{ "All", 0, MI_DISABLED, MN_SNAP_ALL, NULL, 0 },
};
static const struct menuitem show_sub[] = {
	{ "Only Icons", 0, MI_CHECKIT, MN_SHOW_ICONS, NULL, 0 },
	{ "All Files", 0, MI_CHECKIT, MN_SHOW_ALL, NULL, 0 },
};
static const struct menuitem view_sub[] = {
	{ "Icon", 0, MI_CHECKIT, MN_VIEW_ICON, NULL, 0 },
	{ "Name", 0, MI_CHECKIT, MN_VIEW_NAME, NULL, 0 },
	{ "Date", 0, MI_CHECKIT | MI_DISABLED, MN_VIEW_DATE, NULL, 0 },
	{ "Size", 0, MI_CHECKIT | MI_DISABLED, MN_VIEW_SIZE, NULL, 0 },
};
static const struct menuitem wb_items[] = {
	{ "Backdrop", 'B', MI_CHECKIT | MI_CHECKED | MI_DISABLED, MN_BACKDROP, NULL, 0 },
	{ "Execute Command...", 'E', 0, MN_EXECUTE, NULL, 0 },
	{ "Redraw All", 0, 0, MN_REDRAW, NULL, 0 },
	{ "Update All", 0, 0, MN_UPDATEALL, NULL, 0 },
	{ "Last Message", 0, 0, MN_LASTMSG, NULL, 0 },
	{ "About...", '?', 0, MN_ABOUT, NULL, 0 },
	{ "Quit...", 'Q', 0, MN_QUIT, NULL, 0 },
};
static const struct menuitem win_items[] = {
	{ "New Drawer", 'N', MI_DISABLED, MN_NEWDRAWER, NULL, 0 },
	{ "Open Parent", 'K', 0, MN_PARENT, NULL, 0 },
	{ "Close", 'Q' - 'Q' + 0, 0, MN_CLOSE, NULL, 0 },
	{ "Update", 0, 0, MN_UPDATE, NULL, 0 },
	{ "Select Contents", 'A', 0, MN_SELECTALL, NULL, 0 },
	{ "Clean Up", '.', 0, MN_CLEANUP, NULL, 0 },
	{ "Snapshot", 0, 0, 0, snap_sub, 2 },
	{ "Show", 0, 0, 0, show_sub, 2 },
	{ "View By", 0, 0, 0, view_sub, 4 },
};
static const struct menuitem icon_items[] = {
	{ "Open", 'O', 0, MN_OPEN, NULL, 0 },
	{ "Copy", 'C', MI_DISABLED, MN_COPY, NULL, 0 },
	{ "Rename...", 'R', MI_DISABLED, MN_RENAME, NULL, 0 },
	{ "Information...", 'I', 0, MN_INFO, NULL, 0 },
	{ "Snapshot", 'S', MI_DISABLED, MN_SNAPSHOT, NULL, 0 },
	{ "UnSnapshot", 'U', MI_DISABLED, MN_UNSNAPSHOT, NULL, 0 },
	{ "Leave Out", 'L', MI_DISABLED, MN_LEAVEOUT, NULL, 0 },
	{ "Put Away", 'P', MI_DISABLED, MN_PUTAWAY, NULL, 0 },
	{ NULL, 0, MI_BAR, 0, NULL, 0 },
	{ "Delete...", 0, MI_DISABLED, MN_DELETE, NULL, 0 },
	{ "Format Disk...", 0, MI_DISABLED, MN_FORMAT, NULL, 0 },
	{ "Empty Trash", 0, MI_DISABLED, MN_EMPTYTRASH, NULL, 0 },
};
static const struct menuitem tools_items[] = {
	{ "ResetWB", 0, 0, MN_RESETWB, NULL, 0 },
	{ "Shell", 'Z', 0, MN_SHELL, NULL, 0 },
};
static const struct menu wb_menus[] = {
	{ "Workbench", wb_items, 7 },
	{ "Window", win_items, 9 },
	{ "Icons", icon_items, 12 },
	{ "Tools", tools_items, 2 },
};

static struct wb *WB;

/* --- small helpers --- */

static char *path_join(const char *dir, const char *name)
{
	size_t a = strlen(dir), b = strlen(name);
	char *p = malloc(a + b + 2);

	if (!p)
		return NULL;
	memcpy(p, dir, a);
	if (a && dir[a - 1] != '/')
		p[a++] = '/';
	memcpy(p + a, name, b + 1);
	return p;
}

static int ends_with_info(const char *name)
{
	size_t n = strlen(name);

	return n > 5 && !strcasecmp(name + n - 5, ".info");
}

/* "Foo.info" for "Foo", if it exists (the case on AFFS is not reliable) */
static char *find_info(const char *dir, const char *name)
{
	char *want = malloc(strlen(name) + 6), *p;

	if (!want)
		return NULL;
	sprintf(want, "%s.info", name);
	p = path_join(dir, want);
	free(want);
	if (p && access(p, R_OK) == 0)
		return p;
	free(p);
	return NULL;
}

static void view_free(struct iconview *v)
{
	int i;

	if (WB)
		WB->lasticon = NULL;
	for (i = 0; i < v->n; i++) {
		icon_free(&v->icons[i].ic);
		free(v->icons[i].label);
		free(v->icons[i].path);
	}
	free(v->icons);
	memset(v, 0, sizeof(*v));
}

static struct wbicon *view_add(struct iconview *v)
{
	if (v->n == v->cap) {
		int cap = v->cap ? v->cap * 2 : 32;
		struct wbicon *n = realloc(v->icons, sizeof(*n) * (size_t)cap);

		if (!n)
			return NULL;
		v->icons = n;
		v->cap = cap;
	}
	memset(&v->icons[v->n], 0, sizeof(v->icons[0]));
	return &v->icons[v->n++];
}

static void icon_label(struct wb *wb, struct wbicon *ic, const char *label)
{
	ic->label = strdup(label);
	ic->labelw = font_textlen(wb->env->iconfont, label, -1);
}

static int icon_w(const struct wbicon *ic)
{
	return ic->ic.w > ic->labelw ? ic->ic.w : ic->labelw;
}

static int icon_h(struct wb *wb, const struct wbicon *ic)
{
	return ic->ic.h + 1 + wb->env->iconfont->ysize;
}

/* the area the icon covers, view coordinates */
static struct rect icon_bounds(struct wb *wb, const struct wbicon *ic)
{
	int w = icon_w(ic);
	int cx = ic->x + ic->ic.w / 2;

	return rect_make(cx - w / 2, ic->y, w, icon_h(wb, ic));
}

/* --- drawing --- */

static void draw_icon(struct wb *wb, struct rp *rp, const struct wbicon *ic, int ox, int oy)
{
	const struct icon *i = &ic->ic;
	int sel = ic->selected || ic->open;
	int x = ic->x - ox, y = ic->y - oy;

	if (sel && i->img[1].px) {
		rp_blit(rp, x, y, &i->img[1], 0, 0, i->w, i->h, i->key);
	} else {
		rp_blit(rp, x, y, &i->img[0], 0, 0, i->w, i->h, i->key);
		if (sel)	/* GADGHCOMP: complement the image's planes */
			rp_xor_rect(rp, x, y, i->w, i->h, i->glow ? wb->s->xormask : 3);
	}
	rp_text(rp, wb->env->iconfont, x + i->w / 2 - ic->labelw / 2, y + i->h + 1,
		ic->label, -1, PEN(wb, P_TEXT), -1);
}

static void draw_view(struct wb *wb, struct rp *rp, struct iconview *v, int ox, int oy,
		      int w, int h, const struct bm *pattern)
{
	struct rect vis = rect_make(ox, oy, w, h);
	int i;

	if (pattern && pattern->px)
		rp_pattern(rp, 0, 0, w, h, pattern);
	else
		rp_fill(rp, 0, 0, w, h, PEN(wb, P_BACKGROUND));
	for (i = 0; i < v->n; i++) {
		struct rect b = icon_bounds(wb, &v->icons[i]), x;

		if (rect_intersect(&x, &b, &vis))
			draw_icon(wb, rp, &v->icons[i], ox, oy);
	}
}

/* text list (View By Name) */
static void draw_list(struct wb *wb, struct rp *rp, struct drawer *d, int w, int h)
{
	const struct font *f = wb->env->iconfont;
	int i, y;

	rp_fill(rp, 0, 0, w, h, PEN(wb, P_BACKGROUND));
	for (i = 0; i < d->view.n; i++) {
		struct wbicon *ic = &d->view.icons[i];
		char line[300], size[24];
		struct stat st;

		y = i * (f->ysize + 1) + 2 - d->oy;
		if (y + f->ysize < 0 || y >= h)
			continue;
		if (stat(ic->path, &st))
			memset(&st, 0, sizeof(st));
		if (ic->isdir)
			snprintf(size, sizeof(size), "Drawer");
		else
			snprintf(size, sizeof(size), "%lld", (long long)st.st_size);
		snprintf(line, sizeof(line), "%-30.30s %10s %c%c%c%c", ic->label, size,
			 st.st_mode & S_IRUSR ? 'r' : '-', st.st_mode & S_IWUSR ? 'w' : '-',
			 st.st_mode & S_IXUSR ? 'e' : '-', 'd');
		if (ic->selected) {
			rp_fill(rp, 2 - d->ox, y, font_textlen(f, line, -1) + 2, f->ysize, PEN(wb, P_FILL));
		}
		rp_text(rp, f, 3 - d->ox, y, line, -1,
			ic->selected ? PEN(wb, P_FILLTEXT) : PEN(wb, P_TEXT), -1);
	}
}

/* --- layout --- */

static void view_extent(struct wb *wb, struct iconview *v)
{
	int i;

	v->vw = v->vh = 0;
	for (i = 0; i < v->n; i++) {
		struct rect b = icon_bounds(wb, &v->icons[i]);

		if (b.x + b.w > v->vw)
			v->vw = b.x + b.w;
		if (b.y + b.h > v->vh)
			v->vh = b.y + b.h;
	}
	v->vw += 4;
	v->vh += 4;
}

static int name_cmp(const void *a, const void *b)
{
	const struct wbicon *x = a, *y = b;

	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;
	return strcasecmp(x->label, y->label);
}

/*
 * Snapshotted icons go where they were left; the rest flow in rows below
 * them, wrapped to the window's width, as Clean Up would put them.
 */
static void layout(struct wb *wb, struct iconview *v, int width, int force)
{
	int i, x = 8, y = 4, rowh = 0, top = 4;

	for (i = 0; i < v->n; i++) {
		struct wbicon *ic = &v->icons[i];

		if (!force && ic->ic.x != ICON_NOPOS) {
			struct rect b;

			ic->x = ic->ic.x;
			ic->y = ic->ic.y;
			if (ic->x < 0)
				ic->x = 0;
			if (ic->y < 0)
				ic->y = 0;
			b = icon_bounds(wb, ic);
			if (b.x < 2)
				ic->x += 2 - b.x;
			if (b.y + b.h + 4 > top)
				top = b.y + b.h + 4;
		}
	}
	y = top;
	for (i = 0; i < v->n; i++) {
		struct wbicon *ic = &v->icons[i];
		int w, h;

		if (!force && ic->ic.x != ICON_NOPOS)
			continue;
		w = icon_w(ic);
		h = icon_h(wb, ic);
		if (x > 8 && x + w > width - 4) {
			x = 8;
			y += rowh + 6;
			rowh = 0;
		}
		ic->x = x + w / 2 - ic->ic.w / 2;
		ic->y = y;
		x += w + 10;
		if (h > rowh)
			rowh = h;
	}
	view_extent(wb, v);
}

/* --- loading a directory --- */

static int load_icon(struct wb *wb, const char *dir, const char *name, int isdir,
		     int exec, struct icon *out)
{
	char *info = find_info(dir, name);

	if (info) {
		int ret = icon_load(info, out, env_pen, wb->env);

		free(info);
		if (!ret)
			return 0;
	}
	return env_default_icon(wb->env, isdir ? WBDRAWER : exec ? WBTOOL : WBPROJECT, out);
}

static int dir_has_icons(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d)))
		found = ends_with_info(e->d_name) && strcasecmp(e->d_name, "disk.info");
	closedir(d);
	return found;
}

static void drawer_scan(struct wb *wb, struct drawer *dr)
{
	DIR *d = opendir(dr->path);
	struct dirent *e;

	view_free(&dr->view);
	if (!d)
		return;
	while ((e = readdir(d))) {
		struct wbicon *ic;
		struct stat st;
		char *full, *info;
		int isdir, exec;

		if (e->d_name[0] == '.')
			continue;
		if (ends_with_info(e->d_name))
			continue;
		full = path_join(dr->path, e->d_name);
		if (!full)
			continue;
		if (stat(full, &st)) {
			free(full);
			continue;
		}
		info = find_info(dr->path, e->d_name);
		if (!info && !dr->showall) {
			free(full);
			continue;
		}
		free(info);
		isdir = S_ISDIR(st.st_mode);
		exec = !isdir && (st.st_mode & 0111);
		ic = view_add(&dr->view);
		if (!ic) {
			free(full);
			break;
		}
		if (load_icon(wb, dr->path, e->d_name, isdir, exec, &ic->ic)) {
			dr->view.n--;
			free(full);
			continue;
		}
		ic->path = full;
		ic->isdir = isdir;
		icon_label(wb, ic, e->d_name);
	}
	closedir(d);
	if (dr->view.n)
		qsort(dr->view.icons, (size_t)dr->view.n, sizeof(dr->view.icons[0]), name_cmp);
}

/* --- selection --- */

static void redraw_drawer(struct wb *wb, struct drawer *d);
static void redraw_root(struct wb *wb);

static void deselect_view(struct iconview *v, int *changed)
{
	int i;

	for (i = 0; i < v->n; i++)
		if (v->icons[i].selected) {
			v->icons[i].selected = 0;
			*changed = 1;
		}
}

static void deselect_all(struct wb *wb, struct iconview *except)
{
	struct drawer *d;
	int ch = 0;

	if (&wb->root != except) {
		deselect_view(&wb->root, &ch);
		if (ch)
			redraw_root(wb);
	}
	for (d = wb->drawers; d; d = d->next) {
		ch = 0;
		if (&d->view != except)
			deselect_view(&d->view, &ch);
		if (ch)
			redraw_drawer(wb, d);
	}
}

static struct wbicon *first_selected(struct wb *wb, struct iconview **in)
{
	struct drawer *d;
	int i;

	for (i = 0; i < wb->root.n; i++)
		if (wb->root.icons[i].selected) {
			if (in)
				*in = &wb->root;
			return &wb->root.icons[i];
		}
	for (d = wb->drawers; d; d = d->next)
		for (i = 0; i < d->view.n; i++)
			if (d->view.icons[i].selected) {
				if (in)
					*in = &d->view;
				return &d->view.icons[i];
			}
	return NULL;
}

static struct wbicon *view_hit(struct wb *wb, struct iconview *v, int x, int y)
{
	int i;

	for (i = v->n - 1; i >= 0; i--) {
		struct rect b = icon_bounds(wb, &v->icons[i]);

		if (rect_contains(&b, x, y))
			return &v->icons[i];
	}
	return NULL;
}

/* --- windows --- */

static void drawer_draw(struct win *w, struct rp *rp);
static void drawer_button(struct win *w, int x, int y, int button, int down, unsigned int qual);
static void drawer_motion(struct win *w, int x, int y);
static void drawer_close_cb(struct win *w);
static void drawer_resized(struct win *w);
static void drawer_scrolled(struct win *w);
static void any_key(struct win *w, const struct keyev *k);

static const struct win_ops drawer_ops = {
	.draw = drawer_draw,
	.button = drawer_button,
	.motion = drawer_motion,
	.key = any_key,
	.close = drawer_close_cb,
	.resized = drawer_resized,
	.scrolled = drawer_scrolled,
};

static void root_draw(struct win *w, struct rp *rp);
static void root_button(struct win *w, int x, int y, int button, int down, unsigned int qual);
static void root_motion(struct win *w, int x, int y);

static const struct win_ops root_ops = {
	.draw = root_draw,
	.button = root_button,
	.motion = root_motion,
	.key = any_key,
};

static void redraw_root(struct wb *wb)
{
	win_refresh(wb->s, wb->backdrop);
}

static void update_scrollers(struct wb *wb, struct drawer *d)
{
	struct rect in;
	int vh = d->view.vh, vw = d->view.vw;

	win_inner(d->win, &in);
	if (d->byname) {
		vh = d->view.n * (wb->env->iconfont->ysize + 1) + 4;
		vw = font_textlen(wb->env->iconfont, "M", 1) * 48;
	}
	if (d->oy > vh - in.h)
		d->oy = vh - in.h;
	if (d->ox > vw - in.w)
		d->ox = vw - in.w;
	if (d->oy < 0)
		d->oy = 0;
	if (d->ox < 0)
		d->ox = 0;
	win_set_scroll(wb->s, d->win, vh, in.h, d->oy, vw, in.w, d->ox);
}

static void redraw_drawer(struct wb *wb, struct drawer *d)
{
	update_scrollers(wb, d);
	win_refresh(wb->s, d->win);
}

static void drawer_draw(struct win *w, struct rp *rp)
{
	struct drawer *d = w->priv;
	struct rect in;

	win_inner(w, &in);
	if (d->byname)
		draw_list(WB, rp, d, in.w, in.h);
	else
		draw_view(WB, rp, &d->view, d->ox, d->oy, in.w, in.h, &WB->env->pattern[1]);
}

static void root_draw(struct win *w, struct rp *rp)
{
	struct rect in;

	win_inner(w, &in);
	draw_view(WB, rp, &WB->root, 0, 0, in.w, in.h, &WB->env->pattern[0]);
}

static void drawer_resized(struct win *w)
{
	struct drawer *d = w->priv;
	struct rect in;

	win_inner(w, &in);
	update_scrollers(WB, d);
	(void)in;
}

static void drawer_scrolled(struct win *w)
{
	struct drawer *d = w->priv;

	d->oy = w->vtop;
	d->ox = w->htop;
}

static struct drawer *find_drawer_path(struct wb *wb, const char *path)
{
	struct drawer *d;

	for (d = wb->drawers; d; d = d->next)
		if (!strcmp(d->path, path))
			return d;
	return NULL;
}

static void drawer_free(struct wb *wb, struct drawer *d)
{
	struct drawer **p, *o;

	/* drawers opened from this one forget where they came from */
	for (o = wb->drawers; o; o = o->next)
		if (o->opener_view == &d->view)
			o->opener_view = NULL;
	if (wb->dragview == &d->view) {
		wb->dragview = NULL;
		wb->dragging = wb->lasso = 0;
	}
	wb->lasticon = NULL;

	for (p = &wb->drawers; *p; p = &(*p)->next)
		if (*p == d) {
			*p = d->next;
			break;
		}
	if (d->opener_view) {
		int i;

		/* the icon that opened it is drawn closed again, if it still exists */
		for (i = 0; i < d->opener_view->n; i++)
			if (d->opener_view->icons[i].path &&
			    !strcmp(d->opener_view->icons[i].path, d->path)) {
				d->opener_view->icons[i].open = 0;
				if (d->opener_view == &wb->root) {
					redraw_root(wb);
				} else {
					struct drawer *o;

					for (o = wb->drawers; o; o = o->next)
						if (&o->view == d->opener_view)
							redraw_drawer(wb, o);
				}
			}
	}
	view_free(&d->view);
	free(d->path);
	free(d->volname);
	free(d);
}

static void drawer_close(struct wb *wb, struct drawer *d)
{
	struct win *w = d->win;

	drawer_free(wb, d);
	win_close(wb->s, w);
	wb_update_title(wb);
}

static void drawer_close_cb(struct win *w)
{
	drawer_close(WB, w->priv);
}

/* the stopwatch while something slow happens, on screen before it starts */
static void busy(struct wb *wb, int on)
{
	if (!wb->env->have_busy)
		return;
	if (on)
		screen_set_pointer(wb->s, &wb->env->busy, wb->env->busy_hx, wb->env->busy_hy, 0);
	else
		screen_set_pointer(wb->s, &wb->env->pointer, wb->env->ptr_hx, wb->env->ptr_hy, 0);
	if (on && wb->s->flush_now)
		wb->s->flush_now();
}

static struct drawer *open_drawer(struct wb *wb, const char *path, const char *title,
				  const struct icon *geom, struct iconview *from)
{
	struct drawer *d = find_drawer_path(wb, path);
	struct rect r;

	if (d) {
		win_to_front(wb->s, d->win);
		win_activate(wb->s, d->win);
		return d;
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->path = strdup(path);
	d->volname = strdup(title);
	d->opener_view = from;
	/* Linux directories without any .info: show everything */
	d->showall = !dir_has_icons(path);
	if (geom && geom->has_window && geom->win.w > 50 && geom->win.h > 30) {
		r = geom->win;
		if (geom->dd_flags & 2)
			d->showall = 1;
		if (geom->dd_viewmodes >= 2)
			d->byname = 1;
	} else {
		static int cascade;

		r = rect_make(40 + cascade * 16, wb->s->barh + 20 + cascade * 12, 400, 160);
		cascade = (cascade + 1) % 8;
	}
	d->win = win_open(wb->s, &r, WF_STD | WF_VSCROLL | WF_HSCROLL, title, &drawer_ops, d);
	if (!d->win) {
		free(d->path);
		free(d->volname);
		free(d);
		return NULL;
	}
	d->next = wb->drawers;
	wb->drawers = d;
	busy(wb, 1);
	drawer_scan(wb, d);
	busy(wb, 0);
	{
		struct rect in;

		win_inner(d->win, &in);
		layout(wb, &d->view, in.w, 0);
	}
	win_activate(wb->s, d->win);
	redraw_drawer(wb, d);
	wb_update_title(wb);
	return d;
}

/* --- launching --- */

static char *amiga_to_linux(struct wb *wb, const char *apath)
{
	const char *colon = strchr(apath, ':');
	char dev[64], *out;

	if (!colon)
		return strdup(apath);
	if ((size_t)(colon - apath) >= sizeof(dev))
		return NULL;
	memcpy(dev, apath, (size_t)(colon - apath));
	dev[colon - apath] = 0;
	if (!strcasecmp(dev, "SYS") || !strcasecmp(dev, wb->env->sysname) ||
	    !strcasecmp(dev, "C") || !strcasecmp(dev, "LIBS") || !strcasecmp(dev, "S")) {
		const char *sub = !strcasecmp(dev, "C") ? "C/" : !strcasecmp(dev, "S") ? "S/" :
				  !strcasecmp(dev, "LIBS") ? "Libs/" : "";

		if (asprintf(&out, "%s/%s%s", wb->env->sysdir, sub, colon + 1) < 0)
			return NULL;
		return out;
	}
	return NULL;
}

int wb_launch(struct wb *wb, const char *dir, const char *const argv[], const char *title)
{
	struct rect r = rect_make(0, wb->s->barh, wb->s->w, wb->s->h / 3);
	int err = 0;

	if (!con_spawn(wb->s, &r, title, dir, argv, 1, &err) || err) {
		char msg[300];

		snprintf(msg, sizeof(msg), "Unable to open your tool\n'%s'\n%s", argv[0],
			 strerror(err ? err : EIO));
		snprintf(wb->lastmsg, sizeof(wb->lastmsg), "Unable to open your tool '%s'", argv[0]);
		wb_request(wb, "Workbench Request", msg, "OK");
		return -1;
	}
	return 0;
}

static void open_shell(struct wb *wb, const struct icon *ic)
{
	struct rect r = rect_make(0, wb->s->barh, wb->s->w, 130);
	const char *spec = ic ? icon_tooltype(ic, "WINDOW") : NULL;
	char title[64] = "AmigaShell";
	int err = 0;

	/* WINDOW=CON:x/y/w/h/title/options, with empty fields meaning default */
	if (spec && !strncasecmp(spec, "CON:", 4)) {
		char buf[128], *f[6] = { 0 }, *p;
		int n = 0;

		snprintf(buf, sizeof(buf), "%s", spec + 4);
		for (p = buf; n < 6; n++) {
			f[n] = p;
			p = strchr(p, '/');
			if (!p)
				break;
			*p++ = 0;
		}
		if (f[0] && *f[0]) r.x = atoi(f[0]);
		if (f[1] && *f[1]) r.y = atoi(f[1]);
		if (f[2] && *f[2]) r.w = atoi(f[2]);
		if (f[3] && *f[3]) r.h = atoi(f[3]);
		if (f[4] && *f[4])
			snprintf(title, sizeof(title), "%s", f[4]);
	}
	if (r.y < wb->s->barh)
		r.y = wb->s->barh;
	con_spawn(wb->s, &r, title, getenv("HOME") ? getenv("HOME") : "/", NULL, 0, &err);
	if (err)
		wb_request(wb, "Workbench Request", "Unable to open a Shell", "OK");
}

static int is_shell(const char *tool)
{
	const char *b = tool ? strrchr(tool, '/') : NULL;

	b = b ? b + 1 : tool ? (strchr(tool, ':') ? strchr(tool, ':') + 1 : tool) : NULL;
	return b && (!strcasecmp(b, "CLI") || !strcasecmp(b, "Shell") ||
		     !strcasecmp(b, "NewShell"));
}

static void open_icon(struct wb *wb, struct iconview *v, struct wbicon *ic)
{
	char *dir, *slash;

	if (ic->isdir || ic->ic.type == WBDISK || ic->ic.type == WBDRAWER ||
	    ic->ic.type == WBGARBAGE) {
		struct drawer *d;

		if (!ic->isdir) {
			wb_request(wb, "Workbench Request", "This drawer cannot be opened", "OK");
			return;
		}
		d = open_drawer(wb, ic->path, ic->label, &ic->ic, v);
		if (d) {
			ic->open = 1;
			if (v == &wb->root) {
				redraw_root(wb);
			} else {
				struct drawer *o;

				for (o = wb->drawers; o; o = o->next)
					if (&o->view == v)
						redraw_drawer(wb, o);
			}
		}
		return;
	}

	dir = strdup(ic->path);
	if (!dir)
		return;
	slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;

	if (ic->ic.type == WBPROJECT && ic->ic.deftool && *ic->ic.deftool) {
		char *tool;

		if (is_shell(ic->ic.deftool) || is_shell(ic->label)) {
			open_shell(wb, &ic->ic);
			free(dir);
			return;
		}
		tool = amiga_to_linux(wb, ic->ic.deftool);
		if (!tool || access(tool, F_OK)) {
			char msg[300];

			snprintf(msg, sizeof(msg), "Unable to open your tool\n'%s'", ic->ic.deftool);
			snprintf(wb->lastmsg, sizeof(wb->lastmsg), "Unable to open your tool '%s'",
				 ic->ic.deftool);
			wb_request(wb, "Workbench Request", msg, "OK");
		} else {
			const char *argv[] = { tool, ic->path, NULL };

			wb_launch(wb, dir, argv, ic->label);
		}
		free(tool);
		free(dir);
		return;
	}
	if (is_shell(ic->label) && ic->ic.type == WBTOOL) {
		open_shell(wb, &ic->ic);
		free(dir);
		return;
	}
	if (ic->ic.type == WBTOOL || !access(ic->path, X_OK)) {
		const char *argv[] = { ic->path, NULL };

		wb_launch(wb, dir, argv, ic->label);
	} else {
		char msg[300];

		snprintf(msg, sizeof(msg), "'%s' has no default tool.\nTry Information...", ic->label);
		wb_request(wb, "Workbench Request", msg, "OK");
	}
	free(dir);
}

/* --- mouse in views --- */

static void view_button(struct wb *wb, struct win *w, struct iconview *v, int x, int y,
			int button, int down, unsigned int qual)
{
	struct wbicon *ic;

	if (button != 1)
		return;
	if (!down) {
		if (wb->lasso) {
			struct rect band = wb->s->outline, b;
			int i, ch = 0;

			wb->lasso = 0;
			wb->s->outline_on = 0;
			screen_damage(wb->s, &band);
			band.x -= w->r.x + w->bl;
			band.y -= w->r.y + w->bt;
			if (v != &wb->root) {
				struct drawer *d = w->priv;

				band.x += d->ox;
				band.y += d->oy;
			}
			for (i = 0; i < v->n; i++) {
				b = icon_bounds(wb, &v->icons[i]);
				if (rect_intersect(&b, &b, &band)) {
					v->icons[i].selected = 1;
					ch = 1;
				}
			}
			if (ch)
				win_refresh(wb->s, w);
		}
		if (wb->dragging) {
			screen_remove_overlay(wb->s, &wb->dragov);
			bm_free(&wb->dragbm);
			wb->dragging = 0;
			if (wb->dragmoved && win_at(wb->s, wb->s->mx, wb->s->my) == w) {
				int dx = wb->s->mx - wb->dragx0, dy = wb->s->my - wb->dragy0, i;

				/* moved within its own window: reposition, nothing is saved */
				for (i = 0; i < v->n; i++)
					if (v->icons[i].selected) {
						v->icons[i].x += dx;
						v->icons[i].y += dy;
						if (v->icons[i].x < 0)
							v->icons[i].x = 0;
						if (v->icons[i].y < 0)
							v->icons[i].y = 0;
					}
				view_extent(wb, v);
				if (v == &wb->root)
					redraw_root(wb);
				else
					redraw_drawer(wb, w->priv);
			}
		}
		return;
	}

	ic = view_hit(wb, v, x, y);
	if (!ic) {
		if (!(qual & QUAL_SHIFT))
			deselect_all(wb, NULL);
		wb->lasso = 1;
		wb->dragx0 = wb->s->mx;
		wb->dragy0 = wb->s->my;
		wb->dragwin = w;
		return;
	}
	if (wb->lasticon == ic && wb->now - wb->lastclick < DCLICK_MS) {
		wb->lasticon = NULL;
		open_icon(wb, v, ic);
		return;
	}
	wb->lasticon = ic;
	wb->lastclick = wb->now;
	if (!ic->selected) {
		if (!(qual & QUAL_SHIFT))
			deselect_all(wb, NULL);
		ic->selected = 1;
		win_refresh(wb->s, w);
	}
	/* get ready to drag the selection */
	wb->dragging = 1;
	wb->dragmoved = 0;
	wb->dragview = v;
	wb->dragwin = w;
	wb->dragx0 = wb->s->mx;
	wb->dragy0 = wb->s->my;
	{
		const struct icon *i = &ic->ic;

		if (!bm_alloc(&wb->dragbm, i->w, i->h)) {
			struct rp rp;

			rp_init(&rp, &wb->dragbm);
			rp_fill(&rp, 0, 0, i->w, i->h, 255);
			rp_blit(&rp, 0, 0, i->img[1].px ? &i->img[1] : &i->img[0], 0, 0, i->w,
				i->h, i->key);
			wb->dragov.bm = &wb->dragbm;
			wb->dragov.key = 255;
			wb->dragov.x = w->r.x + w->bl + ic->x - (v == &wb->root ? 0 : ((struct drawer *)w->priv)->ox);
			wb->dragov.y = w->r.y + w->bt + ic->y - (v == &wb->root ? 0 : ((struct drawer *)w->priv)->oy);
		} else {
			wb->dragging = 0;
		}
	}
}

static void view_motion(struct wb *wb, struct win *w, struct iconview *v)
{
	int dx = wb->s->mx - wb->dragx0, dy = wb->s->my - wb->dragy0;

	if (wb->lasso) {
		struct rect r;
		struct rect in;

		win_inner(w, &in);
		in.x += w->r.x;
		in.y += w->r.y;
		r = rect_make(dx < 0 ? wb->s->mx : wb->dragx0, dy < 0 ? wb->s->my : wb->dragy0,
			      abs(dx) + 1, abs(dy) + 1);
		rect_intersect(&r, &r, &in);
		if (wb->s->outline_on)
			screen_damage(wb->s, &wb->s->outline);
		wb->s->outline = r;
		wb->s->outline_on = !rect_empty(&r);
		screen_damage(wb->s, &r);
		return;
	}
	if (!wb->dragging)
		return;
	if (!wb->dragmoved && abs(dx) + abs(dy) < 4)
		return;
	if (!wb->dragmoved) {
		wb->dragmoved = 1;
		wb->dragov0x = wb->dragov.x;
		wb->dragov0y = wb->dragov.y;
		screen_add_overlay(wb->s, &wb->dragov);
	}
	screen_move_overlay(wb->s, &wb->dragov, wb->dragov0x + dx, wb->dragov0y + dy);
	(void)v;
}

static void drawer_button(struct win *w, int x, int y, int button, int down, unsigned int qual)
{
	struct drawer *d = w->priv;

	if (d->byname && down && button == 1) {
		int row = (y + d->oy - 2) / (WB->env->iconfont->ysize + 1);

		if (row >= 0 && row < d->view.n) {
			struct wbicon *ic = &d->view.icons[row];

			if (WB->lasticon == ic && WB->now - WB->lastclick < DCLICK_MS) {
				WB->lasticon = NULL;
				open_icon(WB, &d->view, ic);
				return;
			}
			WB->lasticon = ic;
			WB->lastclick = WB->now;
			deselect_all(WB, NULL);
			ic->selected = 1;
			redraw_drawer(WB, d);
		}
		return;
	}
	if (d->byname)
		return;
	view_button(WB, w, &d->view, x + d->ox, y + d->oy, button, down, qual);
}

static void drawer_motion(struct win *w, int x, int y)
{
	struct drawer *d = w->priv;

	(void)x;
	(void)y;
	view_motion(WB, w, &d->view);
}

static void root_button(struct win *w, int x, int y, int button, int down, unsigned int qual)
{
	view_button(WB, w, &WB->root, x, y, button, down, qual);
}

static void root_motion(struct win *w, int x, int y)
{
	(void)x;
	(void)y;
	view_motion(WB, w, &WB->root);
}

static void any_key(struct win *w, const struct keyev *k)
{
	(void)w;
	(void)k;
}

/* --- requesters --- */

struct req {
	struct wb *wb;
	char *lines[24];
	int nlines;
	char buttons[2][24];
	int nbuttons;
	int pressed;
	int string;		/* has a string gadget */
	char text[256];
	int cursor;
	void (*done)(struct wb *wb, int button, const char *text);
	struct icon *image;	/* Information shows the icon */
};

static void req_layout(struct req *q, struct win *w, struct rect *buttons, struct rect *str)
{
	const struct font *f = q->wb->s->font;
	struct rect in;
	int i, bw = 0, y;

	win_inner(w, &in);
	for (i = 0; i < q->nbuttons; i++) {
		int tw = font_textlen(f, q->buttons[i], -1) + 16;

		if (tw > bw)
			bw = tw;
	}
	y = in.h - f->ysize - 10;
	for (i = 0; i < q->nbuttons; i++) {
		int x = q->nbuttons == 1 ? (in.w - bw) / 2 : i == 0 ? 6 : in.w - bw - 6;

		buttons[i] = rect_make(x, y, bw, f->ysize + 6);
	}
	*str = rect_make(8, y - f->ysize - 12, in.w - 16, f->ysize + 6);
}

static void req_draw(struct win *w, struct rp *rp)
{
	struct req *q = w->priv;
	struct screen *s = q->wb->s;
	const struct font *f = s->font;
	struct rect in, b[2], str;
	int i, x0 = 8, y0 = 6;

	win_inner(w, &in);
	rp_fill(rp, 0, 0, in.w, in.h, s->pens[P_BACKGROUND]);
	req_layout(q, w, b, &str);
	/* the text sits in a recessed box, as EasyRequest draws it */
	rp_bevel(rp, 4, 3, in.w - 8, (q->string ? str.y : b[0].y) - 7, s->pens[P_SHADOW],
		 s->pens[P_SHINE]);
	if (q->image) {
		const struct icon *ic = q->image;

		rp_blit(rp, 10, 8, &ic->img[0], 0, 0, ic->w, ic->h, ic->key);
		x0 = 20 + ic->w;
	}
	for (i = 0; i < q->nlines; i++)
		rp_text(rp, f, x0, y0 + 2 + i * (f->ysize + 1), q->lines[i], -1, s->pens[P_TEXT], -1);
	if (q->string) {
		int tx = str.x + 4, cx;

		rp_bevel(rp, str.x, str.y, str.w, str.h, s->pens[P_SHADOW], s->pens[P_SHINE]);
		rp_bevel(rp, str.x + 1, str.y + 1, str.w - 2, str.h - 2, s->pens[P_SHINE],
			 s->pens[P_SHADOW]);
		rp_text(rp, f, tx, str.y + 3, q->text, -1, s->pens[P_TEXT], -1);
		cx = tx + font_textlen(f, q->text, q->cursor);
		rp_xor_rect(rp, cx, str.y + 3, f->xsize, f->ysize, s->xormask & 3 ? 3 : s->xormask);
	}
	for (i = 0; i < q->nbuttons; i++) {
		int in_ = q->pressed == i;

		if (in_)
			rp_fill(rp, b[i].x, b[i].y, b[i].w, b[i].h, s->pens[P_FILL]);
		rp_bevel(rp, b[i].x, b[i].y, b[i].w, b[i].h,
			 in_ ? s->pens[P_SHADOW] : s->pens[P_SHINE],
			 in_ ? s->pens[P_SHINE] : s->pens[P_SHADOW]);
		rp_text(rp, f, b[i].x + (b[i].w - font_textlen(f, q->buttons[i], -1)) / 2,
			b[i].y + 3, q->buttons[i], -1, in_ ? s->pens[P_FILLTEXT] : s->pens[P_TEXT], -1);
	}
}

static void req_finish(struct win *w, int button)
{
	struct req *q = w->priv;
	struct wb *wb = q->wb;
	int i;

	if (q->done)
		q->done(wb, button, q->text);
	for (i = 0; i < q->nlines; i++)
		free(q->lines[i]);
	if (q->image) {
		icon_free(q->image);
		free(q->image);
	}
	free(q);
	win_close(wb->s, w);
}

static void req_button(struct win *w, int x, int y, int button, int down, unsigned int qual)
{
	struct req *q = w->priv;
	struct rect b[2], str;
	int i, hit = -1;

	(void)qual;
	if (button != 1)
		return;
	req_layout(q, w, b, &str);
	for (i = 0; i < q->nbuttons; i++)
		if (rect_contains(&b[i], x, y))
			hit = i;
	if (down) {
		q->pressed = hit;
		win_refresh(q->wb->s, w);
		return;
	}
	if (q->pressed >= 0 && hit == q->pressed) {
		req_finish(w, hit);
		return;
	}
	q->pressed = -1;
	win_refresh(q->wb->s, w);
}

static void req_motion(struct win *w, int x, int y)
{
	(void)w;
	(void)x;
	(void)y;
}

static void req_key(struct win *w, const struct keyev *k)
{
	struct req *q = w->priv;
	size_t len = strlen(q->text);

	if (!k->down)
		return;
	if (k->code == 28 || k->code == 96) {	/* KEY_ENTER, KEY_KPENTER */
		req_finish(w, 0);
		return;
	}
	if (k->code == 1) {			/* KEY_ESC */
		req_finish(w, q->nbuttons - 1);
		return;
	}
	if (!q->string)
		return;
	if (k->code == 14) {			/* KEY_BACKSPACE */
		if (q->cursor > 0) {
			memmove(q->text + q->cursor - 1, q->text + q->cursor, len - (size_t)q->cursor + 1);
			q->cursor--;
		}
	} else if (k->code == 111) {		/* KEY_DELETE */
		if ((size_t)q->cursor < len)
			memmove(q->text + q->cursor, q->text + q->cursor + 1, len - (size_t)q->cursor);
	} else if (k->code == 105) {		/* KEY_LEFT */
		if (q->cursor > 0)
			q->cursor--;
	} else if (k->code == 106) {		/* KEY_RIGHT */
		if ((size_t)q->cursor < len)
			q->cursor++;
	} else if (k->ch >= 32 && k->ch < 256 && len < sizeof(q->text) - 1 &&
		   !(k->qual & (QUAL_LAMIGA | QUAL_RAMIGA))) {
		memmove(q->text + q->cursor + 1, q->text + q->cursor, len - (size_t)q->cursor + 1);
		q->text[q->cursor++] = (char)k->ch;
	} else {
		return;
	}
	win_refresh(q->wb->s, w);
}

static void req_close(struct win *w)
{
	struct req *q = w->priv;

	req_finish(w, q->nbuttons - 1);
}

static const struct win_ops req_ops = {
	.draw = req_draw,
	.button = req_button,
	.motion = req_motion,
	.key = req_key,
	.close = req_close,
};

static struct req *req_open(struct wb *wb, const char *title, const char *text,
			    const char *b1, const char *b2, int string, struct icon *image,
			    void (*done)(struct wb *, int, const char *))
{
	const struct font *f = wb->s->font;
	struct req *q = calloc(1, sizeof(*q));
	const char *p = text;
	struct rect r;
	struct win *w;
	int maxw = 0, i, extra = 0;

	if (!q)
		return NULL;
	q->wb = wb;
	q->pressed = -1;
	q->string = string;
	q->done = done;
	q->image = image;
	while (p && *p && q->nlines < 24) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);

		q->lines[q->nlines] = strndup(p, n);
		i = font_textlen(f, q->lines[q->nlines], -1);
		if (i > maxw)
			maxw = i;
		q->nlines++;
		p = nl ? nl + 1 : NULL;
	}
	snprintf(q->buttons[0], sizeof(q->buttons[0]), "%s", b1);
	q->nbuttons = 1;
	if (b2) {
		snprintf(q->buttons[1], sizeof(q->buttons[1]), "%s", b2);
		q->nbuttons = 2;
	}
	if (image) {
		maxw += image->w + 12;
		if (image->h + 10 > q->nlines * (f->ysize + 1))
			extra = image->h + 10 - q->nlines * (f->ysize + 1);
	}
	if (string && maxw < 320)
		maxw = 320;
	r.w = maxw + 40;
	if (r.w < 200)
		r.w = 200;
	r.h = wm_title_height(wb->s) + 2 + 14 + q->nlines * (f->ysize + 1) + extra + f->ysize + 22 +
	      (string ? f->ysize + 12 : 0);
	r.x = (wb->s->w - r.w) / 2;
	r.y = (wb->s->h - r.h) / 3;
	w = win_open(wb->s, &r, WF_DRAG | WF_DEPTH | (string ? WF_CLOSE : 0), title, &req_ops, q);
	if (!w) {
		for (i = 0; i < q->nlines; i++)
			free(q->lines[i]);
		free(q);
		return NULL;
	}
	win_activate(wb->s, w);
	win_refresh(wb->s, w);
	return q;
}

void wb_request(struct wb *wb, const char *title, const char *text, const char *button)
{
	req_open(wb, title, text, button, NULL, 0, NULL, NULL);
}

/* --- menus --- */

static struct drawer *active_drawer(struct wb *wb)
{
	struct drawer *d;

	for (d = wb->drawers; d; d = d->next)
		if (d->win == wb->s->active)
			return d;
	return NULL;
}

static unsigned int menu_state(void *arg, int id)
{
	struct wb *wb = arg;
	struct drawer *d = active_drawer(wb);
	struct wbicon *sel = first_selected(wb, NULL);

	switch (id) {
	case MN_PARENT:
	case MN_CLOSE:
	case MN_UPDATE:
	case MN_CLEANUP:
		return d ? 0 : MI_DISABLED;
	case MN_SHOW_ICONS:
		return !d ? MI_DISABLED : d->showall ? 0 : MI_CHECKED;
	case MN_SHOW_ALL:
		return !d ? MI_DISABLED : d->showall ? MI_CHECKED : 0;
	case MN_VIEW_ICON:
		return !d ? MI_DISABLED : d->byname ? 0 : MI_CHECKED;
	case MN_VIEW_NAME:
		return !d ? MI_DISABLED : d->byname ? MI_CHECKED : 0;
	case MN_OPEN:
	case MN_INFO:
		return sel ? 0 : MI_DISABLED;
	case MN_LASTMSG:
		return wb->lastmsg[0] ? 0 : MI_DISABLED;
	}
	return 0;
}

static void rescan(struct wb *wb, struct drawer *d)
{
	struct rect in;

	drawer_scan(wb, d);
	win_inner(d->win, &in);
	layout(wb, &d->view, in.w, 0);
	redraw_drawer(wb, d);
}

static void execute_done(struct wb *wb, int button, const char *text)
{
	const char *argv[] = { "/bin/sh", "-c", text, NULL };

	if (button != 0 || !text[0])
		return;
	wb_launch(wb, getenv("HOME") ? getenv("HOME") : "/", argv, text);
}

static void quit_done(struct wb *wb, int button, const char *text)
{
	(void)text;
	if (button == 0)
		wb->quit = 1;
}

static void show_info(struct wb *wb)
{
	struct iconview *v;
	struct wbicon *ic = first_selected(wb, &v);
	char text[2048], when[64];
	struct stat st;
	struct icon *img;
	size_t at;
	int i;
	static const char *types[] = {
		"?", "Disk", "Drawer", "Tool", "Project", "Trashcan", "Device", "Kick", "AppIcon",
	};

	if (!ic)
		return;
	if (stat(ic->path, &st))
		memset(&st, 0, sizeof(st));
	strftime(when, sizeof(when), "%d-%b-%y %H:%M:%S", localtime(&st.st_mtime));
	at = (size_t)snprintf(text, sizeof(text),
			      "Name: %s\nType: %s\nPath: %s\nSize: %lld bytes\nLast changed: %s\n"
			      "Protection: %c%c%c%c (mode %o)\nStack: %d\nDefault Tool: %s",
			      ic->label, types[ic->ic.type >= 0 && ic->ic.type < 9 ? ic->ic.type : 0],
			      ic->path, (long long)st.st_size, when,
			      st.st_mode & S_IRUSR ? 'r' : '-', st.st_mode & S_IWUSR ? 'w' : '-',
			      st.st_mode & S_IXUSR ? 'e' : '-', st.st_mode & S_IWUSR ? 'd' : '-',
			      (unsigned int)(st.st_mode & 07777), ic->ic.stack,
			      ic->ic.deftool ? ic->ic.deftool : "");
	if (ic->ic.ntooltypes && at < sizeof(text))
		at += (size_t)snprintf(text + at, sizeof(text) - at, "\nTool Types:");
	for (i = 0; i < ic->ic.ntooltypes && i < 8 && at < sizeof(text); i++)
		at += (size_t)snprintf(text + at, sizeof(text) - at, "\n  %s", ic->ic.tooltypes[i]);
	img = calloc(1, sizeof(*img));
	if (img && icon_dup(img, &ic->ic)) {
		free(img);
		img = NULL;
	}
	req_open(wb, ic->label, text, "OK", NULL, 0, img, NULL);
	(void)v;
}

static void about(struct wb *wb)
{
	char text[3000];

	snprintf(text, sizeof(text),
		 "wbde - a Workbench for Linux on the Amiga\n"
		 "Everything you see came from this machine:\n\n%s", wb->env->log);
	wb_request(wb, "About Workbench", text, "OK");
}

static void reset_wb(struct wb *wb);

static void menu_pick(void *arg, int id)
{
	struct wb *wb = arg;
	struct drawer *d = active_drawer(wb);
	struct wbicon *sel;
	struct iconview *v;

	switch (id) {
	case MN_EXECUTE:
		req_open(wb, "Execute a File", "Enter Command and its Arguments:\nCommand:",
			 "Ok", "Cancel", 1, NULL, execute_done);
		break;
	case MN_REDRAW:
		screen_damage_all(wb->s);
		break;
	case MN_UPDATEALL:
		for (d = wb->drawers; d; d = d->next)
			rescan(wb, d);
		break;
	case MN_LASTMSG:
		wb_request(wb, "Workbench Request", wb->lastmsg, "OK");
		break;
	case MN_ABOUT:
		about(wb);
		break;
	case MN_QUIT:
		req_open(wb, "Workbench Request", "Do you really want\nto quit Workbench?", "Ok",
			 "Cancel", 0, NULL, quit_done);
		break;
	case MN_PARENT:
		if (d) {
			char *parent = strdup(d->path), *slash;

			if (!parent)
				break;
			slash = strrchr(parent, '/');
			if (slash && slash != parent) {
				*slash = 0;
				slash = strrchr(parent, '/');
				open_drawer(wb, parent, slash ? slash + 1 : parent, NULL, NULL);
			} else if (strcmp(d->path, "/")) {
				open_drawer(wb, "/", "Linux", NULL, NULL);
			}
			free(parent);
		}
		break;
	case MN_CLOSE:
		if (d)
			drawer_close(wb, d);
		break;
	case MN_UPDATE:
		if (d)
			rescan(wb, d);
		break;
	case MN_SELECTALL:
		if (d) {
			int i;

			for (i = 0; i < d->view.n; i++)
				d->view.icons[i].selected = 1;
			redraw_drawer(wb, d);
		} else {
			int i;

			for (i = 0; i < wb->root.n; i++)
				wb->root.icons[i].selected = 1;
			redraw_root(wb);
		}
		break;
	case MN_CLEANUP:
		if (d) {
			struct rect in;

			win_inner(d->win, &in);
			layout(wb, &d->view, in.w, 1);
			redraw_drawer(wb, d);
		}
		break;
	case MN_SHOW_ICONS:
	case MN_SHOW_ALL:
		if (d) {
			d->showall = id == MN_SHOW_ALL;
			rescan(wb, d);
		}
		break;
	case MN_VIEW_ICON:
	case MN_VIEW_NAME:
		if (d) {
			d->byname = id == MN_VIEW_NAME;
			d->ox = d->oy = 0;
			redraw_drawer(wb, d);
		}
		break;
	case MN_OPEN:
		sel = first_selected(wb, &v);
		if (sel)
			open_icon(wb, v, sel);
		break;
	case MN_INFO:
		show_info(wb);
		break;
	case MN_RESETWB:
		reset_wb(wb);
		break;
	case MN_SHELL:
		open_shell(wb, NULL);
		break;
	}
}

/* --- volumes --- */

static int read_ext2_label(const char *dev, char *out, size_t len)
{
	uint8_t sb[1024];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = pread(fd, sb, sizeof(sb), 1024);
	close(fd);
	if (n != (ssize_t)sizeof(sb) || sb[56] != 0x53 || sb[57] != 0xef || !sb[120])
		return -1;
	snprintf(out, len, "%.16s", (const char *)sb + 120);
	return 0;
}

static void add_volume(struct wb *wb, const char *name, const char *path, int type)
{
	struct volume *v;
	int i;

	for (i = 0; i < wb->nvols; i++)
		if (!strcmp(wb->vols[i].path, path))
			return;
	if (wb->nvols >= 16)
		return;
	v = &wb->vols[wb->nvols++];
	snprintf(v->name, sizeof(v->name), "%s", name);
	snprintf(v->path, sizeof(v->path), "%s", path);
	v->type = type;
	snprintf(v->info, sizeof(v->info), "%s/Disk.info", path);
	if (access(v->info, R_OK))
		v->info[0] = 0;
}

static void find_volumes(struct wb *wb, const char *conf)
{
	char line[512];
	FILE *f;

	if (conf && (f = fopen(conf, "r"))) {
		while (fgets(line, sizeof(line), f)) {
			char name[64], path[256];

			if (sscanf(line, "volume %63s %255s", name, path) == 2)
				add_volume(wb, name, path, WBDISK);
		}
		fclose(f);
		if (wb->nvols)
			return;
	}
	if (access(wb->env->sysdir, R_OK) == 0)
		add_volume(wb, wb->env->sysname[0] ? wb->env->sysname : "System", wb->env->sysdir,
			   WBDISK);
	f = fopen("/proc/mounts", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char dev[256], dir[256], type[32], label[64];
			const char *base;

			if (sscanf(line, "%255s %255s %31s", dev, dir, type) != 3)
				continue;
			if (strncmp(dev, "/dev/", 5) || !strcmp(dir, "/"))
				continue;
			if (!strcmp(type, "affs")) {
				/* another AmigaOS partition; the System one is already in */
				base = strrchr(dir, '/');
				add_volume(wb, base ? base + 1 : dir, dir, WBDISK);
				continue;
			}
			if (strncmp(type, "ext", 3) && strcmp(type, "vfat") && strcmp(type, "msdos") &&
			    strcmp(type, "iso9660"))
				continue;
			if (read_ext2_label(dev, label, sizeof(label))) {
				base = strrchr(dir, '/');
				snprintf(label, sizeof(label), "%.63s", base ? base + 1 : dir);
			}
			add_volume(wb, label, dir, WBDISK);
		}
		fclose(f);
	}
	add_volume(wb, "Ram Disk", "/tmp", WBDISK);
	add_volume(wb, "Linux", "/", WBDISK);
}

/*
 * Icons the user left out on the backdrop: a volume's .backdrop lists them,
 * one ":path/name" a line relative to the volume, and their own snapshot
 * position is where they sit.
 */
static void add_left_out(struct wb *wb, const struct rect *in)
{
	int nvol = wb->root.n, v, auto_y = 6;

	for (v = 0; v < nvol && v < wb->nvols; v++) {
		char path[300], line[256];
		FILE *f;

		snprintf(path, sizeof(path), "%s/.backdrop", wb->vols[v].path);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			char *rel = line, *nl = strchr(line, '\n'), *full, *base;
			struct wbicon *ic;
			struct stat st;
			char *info;
			int taken;

			if (nl)
				*nl = 0;
			if (*rel == ':')
				rel++;
			if (!*rel)
				continue;
			full = path_join(wb->vols[v].path, rel);
			if (!full)
				continue;
			base = strrchr(full, '/');
			*base = 0;
			info = find_info(full, base + 1);
			*base = '/';
			if (!info || stat(full, &st)) {
				free(info);
				free(full);
				continue;
			}
			ic = view_add(&wb->root);
			if (!ic || icon_load(info, &ic->ic, env_pen, wb->env)) {
				if (ic)
					wb->root.n--;
				free(info);
				free(full);
				continue;
			}
			free(info);
			ic->path = full;
			ic->isdir = S_ISDIR(st.st_mode);
			icon_label(wb, ic, base + 1);
			wb->root.n--;	/* not itself */
			taken = view_hit(wb, &wb->root, ic->ic.x + ic->ic.w / 2, ic->ic.y + ic->ic.h / 2) != NULL;
			wb->root.n++;
			if (ic->ic.x != ICON_NOPOS && ic->ic.x < in->w - 40 && ic->ic.y < in->h - 20 && !taken) {
				ic->x = ic->ic.x;
				ic->y = ic->ic.y;
			} else {
				ic->x = 8;
				ic->y = auto_y;
				auto_y += icon_h(wb, ic) + 8;
			}
		}
		fclose(f);
	}
}

static void build_root(struct wb *wb)
{
	struct rect in;
	int i, y = 6, colw = 0;

	view_free(&wb->root);
	for (i = 0; i < wb->nvols; i++) {
		struct volume *v = &wb->vols[i];
		struct wbicon *ic = view_add(&wb->root);

		if (!ic)
			break;
		if (!v->info[0] || icon_load(v->info, &ic->ic, env_pen, wb->env))
			env_default_icon(wb->env, v->type, &ic->ic);
		ic->ic.type = WBDISK;
		ic->path = strdup(v->path);
		ic->isdir = 1;
		icon_label(wb, ic, v->name);
		if (icon_w(ic) > colw)
			colw = icon_w(ic);
	}
	/* disks down the right hand side, as Workbench puts them */
	win_inner(wb->backdrop, &in);
	for (i = 0; i < wb->nvols && i < wb->root.n; i++) {
		struct wbicon *ic = &wb->root.icons[i];

		ic->x = in.w - 8 - colw / 2 - ic->ic.w / 2;
		ic->y = y;
		y += icon_h(wb, ic) + 8;
		if (y > in.h - 40) {
			y = 6;
			in.w -= colw + 12;
		}
	}
	win_inner(wb->backdrop, &in);
	add_left_out(wb, &in);
	view_extent(wb, &wb->root);
}

static void reset_wb(struct wb *wb)
{
	while (wb->drawers)
		drawer_close(wb, wb->drawers);
	build_root(wb);
	redraw_root(wb);
}

/* --- the title --- */

static long meminfo(const char *key)
{
	char line[128];
	size_t n = strlen(key);
	long v = -1;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, n) && line[n] == ':') {
			v = atol(line + n + 1);
			break;
		}
	fclose(f);
	return v;
}

static void commas(char *out, size_t len, long long v)
{
	char tmp[32];
	int n, i, o = 0;

	n = snprintf(tmp, sizeof(tmp), "%lld", v);
	for (i = 0; i < n && o < (int)len - 1; i++) {
		if (i && (n - i) % 3 == 0 && o < (int)len - 1)
			out[o++] = ',';
		out[o++] = tmp[i];
	}
	out[o] = 0;
}

void wb_update_title(struct wb *wb)
{
	char title[160], other[32];
	long avail = meminfo("MemAvailable");

	if (avail < 0)
		avail = meminfo("MemFree");
	commas(other, sizeof(other), (long long)avail * 1024);
	/* Workbench 3.1: "Amiga Workbench  n graphics mem  n other mem"; no chip RAM here */
	snprintf(title, sizeof(title), "Amiga Workbench   %s other mem", other);
	screen_set_title(wb->s, title);
}

void wb_tick(struct wb *wb, uint64_t now)
{
	static uint64_t last;

	wb->now = now;
	if (now - last > 5000) {
		last = now;
		wb_update_title(wb);
	}
}

int wb_init(struct wb *wb, struct screen *s, struct wbenv *env, const char *conf)
{
	struct rect r = rect_make(0, s->barh, s->w, s->h - s->barh);

	memset(wb, 0, sizeof(*wb));
	WB = wb;
	wb->s = s;
	wb->env = env;
	wb->strip.menus = wb_menus;
	wb->strip.n = 4;
	wb->strip.arg = wb;
	wb->strip.pick = menu_pick;
	wb->strip.state = menu_state;
	s->menus = &wb->strip;
	wb->backdrop = win_open(s, &r, WF_BORDERLESS | WF_BACKDROP, "Workbench", &root_ops, wb);
	if (!wb->backdrop)
		return -1;
	find_volumes(wb, conf);
	build_root(wb);
	redraw_root(wb);
	wb_update_title(wb);
	return 0;
}
