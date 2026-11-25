/*
 * Menus the way Intuition 3.x draws them with "NewLook": the screen title
 * bar turns into the menu strip while the right button is down, drop-down
 * boxes in BARBLOCKPEN edged with BARTRIMPEN, text in BARDETAILPEN, the
 * item under the pointer complemented, disabled items ghosted, shortcuts as
 * the Amiga key glyph and a letter on the right, submenus marked with ».
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "menu.h"

#define MAXMENUS 16

static struct {
	int open;
	struct screen *s;
	const struct menustrip *strip;
	int titlex[MAXMENUS], titlew[MAXMENUS];
	int menu, item, sub;
	struct bm barbm, dropbm, subbm;
	struct overlay bar, drop, subo;
	int dropon, subon;
	int itemh;
	int dropw, subw;
} M;

int menu_is_open(void)
{
	return M.open;
}

static unsigned int item_flags(const struct menuitem *it)
{
	unsigned int f = it->flags;

	if (M.strip->state && it->id)
		f |= M.strip->state(M.strip->arg, it->id);
	return f;
}

void menu_amigakey(struct screen *s, struct rp *rp, int x, int y, int fg, int bg)
{
	int h = s->font->ysize, w = 11;
	struct rp t = *rp;
	struct rect c = rect_make(x, y, w, h);

	rp_fill(rp, x, y, w, h, fg);
	rp_clip(&t, &c);
	rp_text(&t, s->font, x + 2, y, "A", 1, bg, -1);
}

static void checkmark(struct rp *rp, int x, int y, int h, int pen)
{
	int i;

	/* a tick: short stroke down, long stroke up */
	for (i = 0; i < 3; i++)
		rp_fill(rp, x + i, y + h / 2 + i - 1, 2, 1, pen);
	for (i = 0; i < 6; i++)
		rp_fill(rp, x + 3 + i, y + h / 2 + 1 - i, 2, 1, pen);
}

static int needs_check(const struct menuitem *items, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (items[i].flags & MI_CHECKIT)
			return 1;
	return 0;
}

static int box_width(struct screen *s, const struct menuitem *items, int n)
{
	int i, w = 0, check = needs_check(items, n) ? 14 : 0;

	for (i = 0; i < n; i++) {
		int iw;

		if ((items[i].flags & MI_BAR) || !items[i].label)
			continue;
		iw = font_textlen(s->font, items[i].label, -1) + check;

		if (items[i].key)
			iw += 11 + 4 + font_textlen(s->font, "M", 1) + 8;
		if (items[i].sub)
			iw += 16;
		if (iw > w)
			w = iw;
	}
	return w + 10;
}

static int item_height(const struct menuitem *it)
{
	return (it->flags & MI_BAR) ? 6 : M.itemh;
}

static int box_height(const struct menuitem *items, int n)
{
	int i, h = 4;

	for (i = 0; i < n; i++)
		h += item_height(&items[i]);
	return h;
}

static void draw_box(struct screen *s, struct bm *bm, const struct menuitem *items, int n,
		     int hl)
{
	int fg = s->pens[P_BARDETAIL], bg = s->pens[P_BARBLOCK];
	int check = needs_check(items, n) ? 14 : 0;
	struct rp rp;
	int i, y = 2;

	rp_init(&rp, bm);
	rp_fill(&rp, 0, 0, bm->w, bm->h, bg);
	rp_box(&rp, 0, 0, bm->w, bm->h, s->pens[P_BARTRIM]);
	for (i = 0; i < n; i++) {
		const struct menuitem *it = &items[i];
		unsigned int f = item_flags(it);
		int h = item_height(it), ifg = fg, ibg = bg;

		if (f & MI_BAR) {
			rp_ghost(&rp, 2, y + 2, bm->w - 4, 2, fg);
			y += h;
			continue;
		}
		if (i == hl && !(f & MI_DISABLED)) {
			rp_fill(&rp, 2, y, bm->w - 4, h, fg);
			ifg = bg;
			ibg = fg;
		}
		if ((f & MI_CHECKIT) && (f & MI_CHECKED))
			checkmark(&rp, 4, y, h, ifg);
		rp_text(&rp, s->font, 4 + check, y + 1, it->label, -1, ifg, -1);
		if (it->key) {
			char k[2] = { (char)toupper((unsigned char)it->key), 0 };
			int kw = font_textlen(s->font, k, 1);

			rp_text(&rp, s->font, bm->w - 6 - kw, y + 1, k, 1, ifg, -1);
			menu_amigakey(s, &rp, bm->w - 6 - kw - 4 - 11, y + 1, ifg, ibg);
		}
		if (it->sub)
			rp_text(&rp, s->font, bm->w - 6 - font_textlen(s->font, "\xbb", 1),
				y + 1, "\xbb", 1, ifg, -1);
		if (f & MI_DISABLED)
			rp_ghost(&rp, 2, y, bm->w - 4, h, ibg);
		y += h;
	}
}

static void draw_bar(struct screen *s)
{
	int fg = s->pens[P_BARDETAIL], bg = s->pens[P_BARBLOCK];
	struct rp rp;
	int i;

	rp_init(&rp, &M.barbm);
	rp_fill(&rp, 0, 0, M.barbm.w, M.barbm.h - 1, bg);
	rp_hline(&rp, 0, M.barbm.h - 1, M.barbm.w, s->pens[P_BARTRIM]);
	for (i = 0; i < M.strip->n; i++) {
		int hl = (i == M.menu);

		if (hl)
			rp_fill(&rp, M.titlex[i], 0, M.titlew[i], M.barbm.h - 1, fg);
		rp_text(&rp, s->font, M.titlex[i] + 4, 1, M.strip->menus[i].title, -1,
			hl ? bg : fg, -1);
	}
}

static void close_sub(struct screen *s)
{
	if (M.subon) {
		screen_remove_overlay(s, &M.subo);
		bm_free(&M.subbm);
		M.subon = 0;
	}
	M.sub = -1;
}

static void close_drop(struct screen *s)
{
	close_sub(s);
	if (M.dropon) {
		screen_remove_overlay(s, &M.drop);
		bm_free(&M.dropbm);
		M.dropon = 0;
	}
	M.item = -1;
}

static void open_drop(struct screen *s, int m)
{
	const struct menu *mn = &M.strip->menus[m];
	int x;

	close_drop(s);
	M.menu = m;
	if (!mn->nitems)
		return;
	M.dropw = box_width(s, mn->items, mn->nitems);
	if (bm_alloc(&M.dropbm, M.dropw, box_height(mn->items, mn->nitems)))
		return;
	x = M.titlex[m];
	if (x + M.dropw > s->w)
		x = s->w - M.dropw;
	draw_box(s, &M.dropbm, mn->items, mn->nitems, -1);
	M.drop.bm = &M.dropbm;
	M.drop.x = x;
	M.drop.y = s->barh - 1;
	M.drop.key = -1;
	screen_add_overlay(s, &M.drop);
	M.dropon = 1;
}

static void open_sub(struct screen *s, int item, int itemy)
{
	const struct menuitem *it = &M.strip->menus[M.menu].items[item];
	int x;

	close_sub(s);
	if (!it->sub || (item_flags(it) & MI_DISABLED))
		return;
	M.subw = box_width(s, it->sub, it->nsub);
	if (bm_alloc(&M.subbm, M.subw, box_height(it->sub, it->nsub)))
		return;
	draw_box(s, &M.subbm, it->sub, it->nsub, -1);
	x = M.drop.x + M.dropw - 8;
	if (x + M.subw > s->w)
		x = M.drop.x - M.subw + 8;
	M.subo.bm = &M.subbm;
	M.subo.x = x;
	M.subo.y = itemy;
	if (M.subo.y + M.subbm.h > s->h)
		M.subo.y = s->h - M.subbm.h;
	M.subo.key = -1;
	screen_add_overlay(s, &M.subo);
	M.subon = 1;
}

int menu_open(struct screen *s, const struct menustrip *strip)
{
	int i, x = 0;

	if (!strip || !strip->n || M.open)
		return 0;
	memset(&M, 0, sizeof(M));
	M.s = s;
	M.strip = strip;
	M.menu = M.item = M.sub = -1;
	M.itemh = s->font->ysize + 2;
	for (i = 0; i < strip->n && i < MAXMENUS; i++) {
		M.titlex[i] = x;
		M.titlew[i] = font_textlen(s->font, strip->menus[i].title, -1) + 8;
		x += M.titlew[i] + 4;
	}
	if (bm_alloc(&M.barbm, s->w, s->barh))
		return 0;
	draw_bar(s);
	M.bar.bm = &M.barbm;
	M.bar.key = -1;
	screen_add_overlay(s, &M.bar);
	M.open = 1;
	menu_motion(s, s->mx, s->my);
	return 1;
}

/* which item of a box is at y (box coordinates); -1 none */
static int item_at(const struct menuitem *items, int n, int y, int *itemy)
{
	int i, at = 2;

	for (i = 0; i < n; i++) {
		int h = item_height(&items[i]);

		if (y >= at && y < at + h) {
			*itemy = at;
			return (items[i].flags & MI_BAR) ? -1 : i;
		}
		at += h;
	}
	return -1;
}

void menu_motion(struct screen *s, int x, int y)
{
	int i, itemy = 0;

	if (!M.open)
		return;

	/* in a submenu? */
	if (M.subon && rect_contains(&(struct rect){ M.subo.x, M.subo.y, M.subbm.w, M.subbm.h }, x, y)) {
		const struct menuitem *it = &M.strip->menus[M.menu].items[M.item];
		int si = item_at(it->sub, it->nsub, y - M.subo.y, &itemy);

		if (si != M.sub) {
			M.sub = si;
			draw_box(s, &M.subbm, it->sub, it->nsub, si);
			screen_move_overlay(s, &M.subo, M.subo.x, M.subo.y);
		}
		return;
	}
	if (M.subon && M.sub >= 0) {
		const struct menuitem *it = &M.strip->menus[M.menu].items[M.item];

		M.sub = -1;
		draw_box(s, &M.subbm, it->sub, it->nsub, -1);
		screen_move_overlay(s, &M.subo, M.subo.x, M.subo.y);
	}

	/* the strip */
	if (y < s->barh) {
		for (i = 0; i < M.strip->n; i++) {
			if (x >= M.titlex[i] && x < M.titlex[i] + M.titlew[i]) {
				if (i != M.menu) {
					open_drop(s, i);
					draw_bar(s);
					screen_move_overlay(s, &M.bar, 0, 0);
				}
				break;
			}
		}
		if (M.item >= 0 && M.dropon) {
			const struct menu *mn = &M.strip->menus[M.menu];

			close_sub(s);
			M.item = -1;
			draw_box(s, &M.dropbm, mn->items, mn->nitems, -1);
			screen_move_overlay(s, &M.drop, M.drop.x, M.drop.y);
		}
		return;
	}

	/* the drop-down */
	if (M.dropon) {
		const struct menu *mn = &M.strip->menus[M.menu];
		int it = -1;

		if (rect_contains(&(struct rect){ M.drop.x, M.drop.y, M.dropbm.w, M.dropbm.h }, x, y))
			it = item_at(mn->items, mn->nitems, y - M.drop.y, &itemy);
		else if (M.subon)
			return;	/* on the way to the submenu: keep it */
		if (it != M.item) {
			M.item = it;
			draw_box(s, &M.dropbm, mn->items, mn->nitems, it);
			screen_move_overlay(s, &M.drop, M.drop.x, M.drop.y);
			close_sub(s);
			if (it >= 0 && mn->items[it].sub)
				open_sub(s, it, M.drop.y + itemy - 2);
		}
	}
}

void menu_close(struct screen *s)
{
	int id = 0;

	if (!M.open)
		return;
	if (M.menu >= 0 && M.item >= 0) {
		const struct menuitem *it = &M.strip->menus[M.menu].items[M.item];

		if (it->sub) {
			if (M.sub >= 0 && !(item_flags(&it->sub[M.sub]) & MI_DISABLED))
				id = it->sub[M.sub].id;
		} else if (!(item_flags(it) & MI_DISABLED)) {
			id = it->id;
		}
	}
	close_drop(s);
	screen_remove_overlay(s, &M.bar);
	bm_free(&M.barbm);
	M.open = 0;
	if (id && M.strip->pick)
		M.strip->pick(M.strip->arg, id);
}

static int find_key(const struct menustrip *strip, const struct menuitem *items, int n, int ch)
{
	int i;

	for (i = 0; i < n; i++) {
		unsigned int f = items[i].flags;

		if (strip->state && items[i].id)
			f |= strip->state(strip->arg, items[i].id);
		if (items[i].sub) {
			int id = find_key(strip, items[i].sub, items[i].nsub, ch);

			if (id)
				return id;
		}
		if (items[i].key && toupper((unsigned char)items[i].key) == toupper(ch) &&
		    !(f & MI_DISABLED))
			return items[i].id;
	}
	return 0;
}

int menu_shortcut(const struct menustrip *strip, int ch)
{
	int i, id;

	if (!strip)
		return 0;
	for (i = 0; i < strip->n; i++) {
		id = find_key(strip, strip->menus[i].items, strip->menus[i].nitems, ch);
		if (id) {
			if (strip->pick)
				strip->pick(strip->arg, id);
			return 1;
		}
	}
	return 0;
}
