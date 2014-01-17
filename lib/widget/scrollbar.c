/*
   Widgets for the Midnight Commander: scrollbar

   Copyright (C) 2013
   The Free Software Foundation, Inc.

   Authors:
   Slava Zanko <slavazanko@gmail.com>, 2013

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file scrollbar.c
 *  \brief Source: WScrollBar widget
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>

#include "lib/global.h"

#include "lib/tty/tty.h"
#include "lib/tty/color.h"
#include "lib/skin.h"
#include "lib/strutil.h"
#include "lib/widget.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

/*** file scope type declarations ****************************************************************/

/*** file scope variables ************************************************************************/

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static void
scrollbar_set_size (WScrollBar *scrollbar)
{
    Widget *w = WIDGET (scrollbar);
    Widget *p = scrollbar->parent;

    switch (scrollbar->type)
    {
    case SCROLLBAR_VERTICAL:
        w->y = p->y;
        w->x = p->x + p->cols - 1;
        w->lines = p->lines;
        w->cols = 1;
        break;
    default:
        w->x = p->x;
        w->y = p->y + p->lines - 1;
        w->cols = p->cols;
        w->lines = 1;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
scrollbar_draw_horizontal (WScrollBar * scrollbar)
{
    Widget *w = WIDGET (scrollbar);
    int column = 0;
    int i;

    /* Now draw the nice relative pointer */
    if (*scrollbar->total != 0)
        column = *scrollbar->current * w->cols / *scrollbar->total;

    for (i = 0; i < w->cols; i++)
    {
        widget_move (w, 0, i);
        if (i != column)
            tty_print_char ('!');
        else
            tty_print_char ('*');
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
scrollbar_draw_vertical (WScrollBar * scrollbar)
{
    Widget *w = WIDGET (scrollbar);
    int line = 0;
    int i;

    /* Now draw the nice relative pointer */
    if (*scrollbar->total != 0)
        line = *scrollbar->current * w->lines / *scrollbar->total;

    for (i = 0; i < w->lines; i++)
    {
        widget_move (w, i, 0);
        if (i != line)
            tty_print_char ('|');
        else
            tty_print_char ('*');
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
scrollbar_draw (WScrollBar * scrollbar)
{
    //    const gboolean disabled = (scrollbar->parent->options & W_DISABLED) != 0;
    //    const int normalc = disabled ? DISABLED_COLOR : scrollbar->parent->color[DLG_COLOR_NORMAL];
    const int normalc = DISABLED_COLOR;

    if (*scrollbar->total <= scrollbar->parent->lines)
        return;

    tty_setcolor (normalc);

    switch (scrollbar->type)
    {
    case SCROLLBAR_VERTICAL:
        scrollbar_draw_vertical (scrollbar);
    default:
        scrollbar_draw_horizontal (scrollbar);
    }
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
scrollbar_callback (Widget * w, Widget * sender, widget_msg_t msg, int parm, void *data)
{
    WScrollBar *scrollbar = SCROLLBAR (w);
    cb_ret_t ret = MSG_HANDLED;

    switch (msg)
    {
    case MSG_INIT:
//        w->pos_flags = WPOS_KEEP_RIGHT | WPOS_KEEP_BOTTOM;
        return MSG_HANDLED;

    case MSG_RESIZE:
        scrollbar_set_size (scrollbar);
        return MSG_HANDLED;

    case MSG_FOCUS:
        return MSG_NOT_HANDLED;

    case MSG_ACTION:
        ret = MSG_NOT_HANDLED;

    case MSG_DRAW:
        scrollbar_draw (scrollbar);
        return ret;

    case MSG_DESTROY:
        return MSG_HANDLED;

    default:
        return widget_default_callback (w, sender, msg, parm, data);
    }
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */
/**
 * Create new WScrollBar object.
 *
 * @param parent parent widget
 * @param type type of scrollbar (vertical or horizontal)
 * @return new WScrollBar object
 */

WScrollBar *
scrollbar_new (Widget * parent, scrollbar_type_t type)
{
    WScrollBar *scrollbar;
    Widget *widget;

    scrollbar = g_new (WScrollBar, 1);
    scrollbar->type = type;

    widget = WIDGET (scrollbar);
    widget_init (widget, 1, 1, 1, 1, scrollbar_callback, NULL);

    scrollbar->parent = parent;
    scrollbar_set_size (scrollbar);

    widget_want_cursor (widget, FALSE);
    widget_want_hotkey (widget, FALSE);

    return scrollbar;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Set total count of items.
 *
 * @param scrollbar WScrollBar object
 * @param total total count of items
 */

void
scrollbar_set_total (WScrollBar * scrollbar, int *total)
{
    if (scrollbar != NULL)
        scrollbar->total = total;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Set current position of item.
 *
 * @param scrollbar WScrollBar object
 * @param current current position of item
 */

void
scrollbar_set_current (WScrollBar * scrollbar, int *current)
{
    if (scrollbar != NULL)
        scrollbar->current = current;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Set position of first displayed item.
 *
 * @param scrollbar WScrollBar object
 * @param first_displayed position of first displayed item
 */

void
scrollbar_set_first_displayed (WScrollBar * scrollbar, int *first_displayed)
{
    if (scrollbar != NULL)
        scrollbar->first_displayed = first_displayed;
}

/* --------------------------------------------------------------------------------------------- */
