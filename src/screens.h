/*
 * The screen stack (main.c): the Workbench screen and client screens, the
 * front one on the display.  Intuition's ScreenToFront/Back, Left Amiga +
 * M (front screen to the back) and + N (Workbench to the front), the
 * screen depth gadget.
 */
#ifndef WBDE_SCREENS_H
#define WBDE_SCREENS_H

#include "wm.h"

void screens_add(struct screen *s);		/* in front */
void screens_remove(struct screen *s);
void screens_to_front(struct screen *s);
void screens_to_back(struct screen *s);
struct screen *screens_front(void);
struct screen *screens_wb(void);
/* s's palette changed: the display follows if it shows s */
void screens_palette(struct screen *s);
/* a new screen of w x h scaled sx, sy on this display: its view */
void screens_fit(struct screen *s);
/* the largest whole scale at which a w x h screen fits the display */
int screens_fit_scale(int w, int h);
/* the mouse buttons held, as IEQUALIFIER_LEFTBUTTON/RBUTTON/MIDBUTTON */
unsigned int screens_buttons(void);
/* the standard pointer (busy: the stopwatch) on s */
void screens_std_pointer(struct screen *s, int busy);

#endif
