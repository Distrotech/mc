/*
   Widgets for the Midnight Commander

   Copyright (C) 1994-2014
   Free Software Foundation, Inc.

   Authors:
   Radek Doulik, 1994, 1995
   Miguel de Icaza, 1994, 1995
   Jakub Jelinek, 1995
   Andrej Borsenkow, 1996
   Norbert Warmuth, 1997
   Andrew Borodin <aborodin@vmail.ru>, 2009, 2010, 2013

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

/** \file input.c
 *  \brief Source: WInput widget
 */

#include <config.h>

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "lib/global.h"

#include "lib/tty/tty.h"
#include "lib/tty/mouse.h"
#include "lib/tty/key.h"        /* XCTRL and ALT macros  */
#include "lib/fileloc.h"
#include "lib/skin.h"
#include "lib/strutil.h"
#include "lib/util.h"
#include "lib/keymap.h"
#include "lib/widget.h"
#include "lib/event.h"          /* mc_event_raise() */

#include "input_complete.h"

/*** global variables ****************************************************************************/

/* Color styles for input widgets */
input_colors_t input_colors;

/*** file scope macro definitions ****************************************************************/

#define LARGE_HISTORY_BUTTON 1

#ifdef LARGE_HISTORY_BUTTON
#define HISTORY_BUTTON_WIDTH 3
#else
#define HISTORY_BUTTON_WIDTH 1
#endif

#define should_show_history_button(in) \
    (in->history.list != NULL && WIDGET (in)->cols > HISTORY_BUTTON_WIDTH * 2 + 1 \
         && WIDGET (in)->owner != NULL)

/*** file scope type declarations ****************************************************************/

/*** file scope variables ************************************************************************/

/* Input widgets have a global kill ring */
/* Pointer to killed data */
static char *kill_buffer = NULL;

/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static size_t
get_history_length (const GList * history)
{
    size_t len = 0;

    for (; history != NULL; history = g_list_previous (history))
        len++;

    return len;
}

/* --------------------------------------------------------------------------------------------- */

static void
draw_history_button (WInput * in)
{
    char c;
    gboolean disabled = (WIDGET (in)->options & W_DISABLED) != 0;

    if (g_list_next (in->history.current) == NULL)
        c = '^';
    else if (g_list_previous (in->history.current) == NULL)
        c = 'v';
    else
        c = '|';

    widget_move (in, 0, WIDGET (in)->cols - HISTORY_BUTTON_WIDTH);
    tty_setcolor (disabled ? DISABLED_COLOR : in->color[WINPUTC_HISTORY]);

#ifdef LARGE_HISTORY_BUTTON
    tty_print_string ("[ ]");
    widget_move (in, 0, WIDGET (in)->cols - HISTORY_BUTTON_WIDTH + 1);
#endif

    tty_print_char (c);
}

/* --------------------------------------------------------------------------------------------- */

static void
input_set_markers (WInput * in, long m1)
{
    in->mark = m1;
}

/* --------------------------------------------------------------------------------------------- */

static void
input_mark_cmd (WInput * in, gboolean mark)
{
    if (mark == 0)
    {
        in->highlight = FALSE;
        input_set_markers (in, 0);
    }
    else
    {
        in->highlight = TRUE;
        input_set_markers (in, in->point);
    }
}

/* --------------------------------------------------------------------------------------------- */

static gboolean
input_eval_marks (WInput * in, long *start_mark, long *end_mark)
{
    if (in->highlight)
    {
        *start_mark = min (in->mark, in->point);
        *end_mark = max (in->mark, in->point);
        return TRUE;
    }
    else
    {
        *start_mark = *end_mark = 0;
        return FALSE;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
delete_region (WInput * in, int x_first, int x_last)
{
    int first = min (x_first, x_last);
    int last = max (x_first, x_last);
    size_t len;

    input_mark_cmd (in, FALSE);
    in->point = first;
    last = str_offset_to_pos (in->buffer, last);
    first = str_offset_to_pos (in->buffer, first);
    len = strlen (&in->buffer[last]) + 1;
    memmove (&in->buffer[first], &in->buffer[last], len);
    in->charpoint = 0;
    in->need_push = TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Strip password from incomplete url (just user:pass@host without VFS prefix).
 *
 * @param url partial URL
 * @return newly allocated string without password
 */

static char *
input_history_strip_password (char *url)
{
    char *at, *delim, *colon;

    at = strrchr (url, '@');
    if (at == NULL)
        return g_strdup (url);

    /* TODO: handle ':' and '@' in password */

    delim = strstr (url, VFS_PATH_URL_DELIMITER);
    if (delim != NULL)
        colon = strchr (delim + strlen (VFS_PATH_URL_DELIMITER), ':');
    else
        colon = strchr (url, ':');

    /* if 'colon' before 'at', 'colon' delimits user and password: user:password@host */
    /* if 'colon' after 'at', 'colon' delimits host and port: user@host:port */
    if (colon != NULL && colon > at)
        colon = NULL;

    if (colon == NULL)
        return g_strdup (url);
    *colon = '\0';

    return g_strconcat (url, at, NULL);
}

/* --------------------------------------------------------------------------------------------- */

static void
push_history (WInput * in, const char *text)
{
    char *t;
    gboolean empty;

    if (text == NULL)
        return;

    t = g_strstrip (g_strdup (text));
    empty = *t == '\0';
    g_free (t);
    t = g_strdup (empty ? "" : text);

    if (!empty && in->history.name != NULL && in->strip_password)
    {
        /*
           We got string user:pass@host without any VFS prefixes
           and vfs_path_to_str_flags (t, VPF_STRIP_PASSWORD) doesn't work.
           Therefore we want to strip password in separate algorithm
         */
        char *url_with_stripped_password;

        url_with_stripped_password = input_history_strip_password (t);
        g_free (t);
        t = url_with_stripped_password;
    }

    if (in->history.list == NULL || in->history.list->data == NULL
        || strcmp (in->history.list->data, t) != 0 || in->history.changed)
    {
        in->history.list = list_append_unique (in->history.list, t);
        in->history.current = in->history.list;
        in->history.changed = TRUE;
    }
    else
        g_free (t);

    in->need_push = FALSE;
}

/* --------------------------------------------------------------------------------------------- */

static void
move_buffer_backward (WInput * in, int start, int end)
{
    int i, pos, len;
    int str_len;

    str_len = str_length (in->buffer);
    if (start >= str_len || end > str_len + 1)
        return;

    pos = str_offset_to_pos (in->buffer, start);
    len = str_offset_to_pos (in->buffer, end) - pos;

    for (i = pos; in->buffer[i + len - 1]; i++)
        in->buffer[i] = in->buffer[i + len];
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
insert_char (WInput * in, int c_code)
{
    int res;

    if (in->highlight)
    {
        long m1, m2;
        if (input_eval_marks (in, &m1, &m2))
            delete_region (in, m1, m2);
    }
    if (c_code == -1)
        return MSG_NOT_HANDLED;

    if (in->charpoint >= MB_LEN_MAX)
        return MSG_HANDLED;

    in->charbuf[in->charpoint] = c_code;
    in->charpoint++;

    res = str_is_valid_char (in->charbuf, in->charpoint);
    if (res < 0)
    {
        if (res != -2)
            in->charpoint = 0;  /* broken multibyte char, skip */
        return MSG_HANDLED;
    }

    in->need_push = TRUE;
    if (strlen (in->buffer) + 1 + in->charpoint >= in->current_max_size)
    {
        /* Expand the buffer */
        size_t new_length;
        char *narea;

        new_length = in->current_max_size + WIDGET (in)->cols + in->charpoint;
        narea = g_try_renew (char, in->buffer, new_length);
        if (narea != NULL)
        {
            in->buffer = narea;
            in->current_max_size = new_length;
        }
    }

    if (strlen (in->buffer) + in->charpoint < in->current_max_size)
    {
        size_t i;
        /* bytes from begin */
        size_t ins_point = str_offset_to_pos (in->buffer, in->point);
        /* move chars */
        size_t rest_bytes = strlen (in->buffer + ins_point);

        for (i = rest_bytes + 1; i > 0; i--)
            in->buffer[ins_point + i + in->charpoint - 1] = in->buffer[ins_point + i - 1];

        memcpy (in->buffer + ins_point, in->charbuf, in->charpoint);
        in->point++;
    }

    in->charpoint = 0;
    return MSG_HANDLED;
}

/* --------------------------------------------------------------------------------------------- */

static void
delete_char (WInput * in)
{
    const char *act;
    int end = in->point;

    act = in->buffer + str_offset_to_pos (in->buffer, in->point);
    end += str_cnext_noncomb_char (&act);

    move_buffer_backward (in, in->point, end);
    in->charpoint = 0;
    in->need_push = TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static void
port_region_marked_for_delete (WInput * in)
{
    in->buffer[0] = '\0';
    in->point = 0;
    in->first = FALSE;
    in->charpoint = 0;
}

/* --------------------------------------------------------------------------------------------- */

/* "history_load" event handler */
static gboolean
input_load_history (const gchar * event_group_name, const gchar * event_name,
                    gpointer init_data, gpointer data)
{
    WInput *in = INPUT (init_data);
    ev_history_load_save_t *ev = (ev_history_load_save_t *) data;

    (void) event_group_name;
    (void) event_name;

    in->history.list = history_load (ev->cfg, in->history.name);
    in->history.current = in->history.list;

    if (in->init_from_history)
    {
        const char *def_text = "";

        if (in->history.list != NULL && in->history.list->data != NULL)
            def_text = (const char *) in->history.list->data;

        input_assign_text (in, def_text);
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

/* "history_save" event handler */
static gboolean
input_save_history (const gchar * event_group_name, const gchar * event_name,
                    gpointer init_data, gpointer data)
{
    WInput *in = INPUT (init_data);

    (void) event_group_name;
    (void) event_name;

    if (!in->is_password && (WIDGET (in)->owner->ret_value != B_CANCEL))
    {
        ev_history_load_save_t *ev = (ev_history_load_save_t *) data;

        push_history (in, in->buffer);
        if (in->history.changed)
            history_save (ev->cfg, in->history.name, in->history.list);
        in->history.changed = FALSE;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static void
input_destroy (WInput * in)
{
    if (in == NULL)
    {
        fprintf (stderr, "Internal error: null Input *\n");
        exit (EXIT_FAILURE);
    }

    input_free_completions (in);

    /* clean history */
    if (in->history.list != NULL)
    {
        /* history is already saved before this moment */
        in->history.list = g_list_first (in->history.list);
        g_list_free_full (in->history.list, g_free);
    }
    g_free (in->history.name);
    g_free (in->buffer);

    g_free (kill_buffer);
    kill_buffer = NULL;
}

/* --------------------------------------------------------------------------------------------- */

static int
input_event (Gpm_Event * event, void *data)
{
    WInput *in = INPUT (data);
    Widget *w = WIDGET (data);

    if (!mouse_global_in_widget (event, w))
        return MOU_UNHANDLED;

    if ((event->type & GPM_DOWN) != 0)
    {
        in->first = FALSE;
        input_mark_cmd (in, FALSE);
    }

    if ((event->type & (GPM_DOWN | GPM_DRAG)) != 0)
    {
        Gpm_Event local;

        local = mouse_get_local (event, w);

        dlg_select_widget (w);

        if (local.x >= w->cols - HISTORY_BUTTON_WIDTH + 1 && should_show_history_button (in))
            mc_event_raise (MC_WINPUT_EVENT_GROUP, "history_show", in);
        else
        {
            in->point = str_length (in->buffer);
            if (local.x + in->term_first_shown - 1 < str_term_width1 (in->buffer))
                in->point = str_column_to_pos (in->buffer, local.x + in->term_first_shown - 1);
        }

        input_update (in, TRUE);
    }

    /* A lone up mustn't do anything */
    if (in->highlight && (event->type & (GPM_UP | GPM_DRAG)) != 0)
        return MOU_NORMAL;

    if ((event->type & GPM_DRAG) == 0)
        input_mark_cmd (in, TRUE);

    return MOU_NORMAL;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Callback for applying new options to input widget.
 *
 * @param w       widget
 * @param options options set
 * @param enable  TRUE if specified options should be added, FALSE if options should be removed
 */
static void
input_set_options_callback (Widget * w, widget_options_t options, gboolean enable)
{
    WInput *in = INPUT (w);

    widget_default_set_options_callback (w, options, enable);
    if (in->label != NULL)
        widget_set_options (WIDGET (in->label), options, enable);
}

/* --------------------------------------------------------------------------------------------- */

static void
input_delete_selection (WInput * input)
{
    long m1, m2;
    if (input_eval_marks (input, &m1, &m2))
        delete_region (input, m1, m2);
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
input_raw_handle_char (WInput * in, int key)
{
    cb_ret_t v;

    input_free_completions (in);
    v = insert_char (in, key);
    input_update (in, TRUE);
    return v;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */
/**
 * A highlight command like shift-arrow
 */

static gboolean
mc_winput_cmd_start_highlight (const gchar * event_group_name, const gchar * event_name,
                               gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    if (!input->highlight)
    {
        input_mark_cmd (input, FALSE);  /* clear */
        input_mark_cmd (input, TRUE);   /* marking on */
    }

    input->is_highlight_cmd = TRUE;
    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_stop_highlight (const gchar * event_group_name, const gchar * event_name,
                              gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    if (input->highlight)
        input_mark_cmd (input, FALSE);


    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_begin_of_line (const gchar * event_group_name, const gchar * event_name,
                             gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    input->point = 0;
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_end_of_line (const gchar * event_group_name, const gchar * event_name,
                           gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    input->point = str_length (input->buffer);
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_backward_char (const gchar * event_group_name, const gchar * event_name,
                             gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    const char *act;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    act = input->buffer + str_offset_to_pos (input->buffer, input->point);
    if (input->point > 0)
        input->point -= str_cprev_noncomb_char (&act, input->buffer);
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_backward_word (const gchar * event_group_name, const gchar * event_name,
                             gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    const char *p, *p_tmp;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    for (p = input->buffer + str_offset_to_pos (input->buffer, input->point);
         (p != input->buffer) && (p[0] == '\0'); str_cprev_char (&p), input->point--);

    while (p != input->buffer)
    {
        p_tmp = p;
        str_cprev_char (&p);
        if (!str_isspace (p) && !str_ispunct (p))
        {
            p = p_tmp;
            break;
        }
        input->point--;
    }
    while (p != input->buffer)
    {
        str_cprev_char (&p);
        if (str_isspace (p) || str_ispunct (p))
            break;

        input->point--;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_forward_char (const gchar * event_group_name, const gchar * event_name,
                            gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    const char *act;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    act = input->buffer + str_offset_to_pos (input->buffer, input->point);
    if (act[0] != '\0')
        input->point += str_cnext_noncomb_char (&act);
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_forward_word (const gchar * event_group_name, const gchar * event_name,
                            gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    const char *p;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    p = input->buffer + str_offset_to_pos (input->buffer, input->point);
    while (p[0] != '\0' && (str_isspace (p) || str_ispunct (p)))
    {
        str_cnext_char (&p);
        input->point++;
    }
    while (p[0] != '\0' && !str_isspace (p) && !str_ispunct (p))
    {
        str_cnext_char (&p);
        input->point++;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_backspace (const gchar * event_group_name, const gchar * event_name,
                         gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    if (input->highlight)
        input_delete_selection (input);
    else
    {
        const char *act;
        int start;

        act = input->buffer + str_offset_to_pos (input->buffer, input->point);

        if (input->point != 0)
        {
            start = input->point - str_cprev_noncomb_char (&act, input->buffer);
            move_buffer_backward (input, start, input->point);
            input->charpoint = 0;
            input->need_push = TRUE;
            input->point = start;
        }
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_delete (const gchar * event_group_name, const gchar * event_name,
                      gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    if (input->first)
        port_region_marked_for_delete (input);
    else if (input->highlight)
        input_delete_selection (input);
    else
        delete_char (input);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_kill_word (const gchar * event_group_name, const gchar * event_name,
                         gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    int old_point = input->point;
    int new_point;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    mc_winput_cmd_forward_word (NULL, NULL, NULL, input);
    new_point = input->point;
    input->point = old_point;

    delete_region (input, old_point, new_point);
    input->need_push = TRUE;
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_back_kill_word (const gchar * event_group_name, const gchar * event_name,
                              gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    int old_point = input->point;
    int new_point;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    mc_winput_cmd_backward_word (NULL, NULL, NULL, input);
    new_point = input->point;
    input->point = old_point;

    delete_region (input, old_point, new_point);
    input->need_push = TRUE;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_mark (const gchar * event_group_name, const gchar * event_name,
                    gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    input_mark_cmd (input, TRUE);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_remove (const gchar * event_group_name, const gchar * event_name,
                      gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    delete_region (input, input->point, input->mark);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_delete_to_end (const gchar * event_group_name, const gchar * event_name,
                             gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    int chp;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    chp = str_offset_to_pos (input->buffer, input->point);
    g_free (kill_buffer);
    kill_buffer = g_strdup (&input->buffer[chp]);
    input->buffer[chp] = '\0';
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_clear_all (const gchar * event_group_name, const gchar * event_name,
                         gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    input->need_push = TRUE;
    input->buffer[0] = '\0';
    input->point = 0;
    input->mark = 0;
    input->highlight = FALSE;
    input->charpoint = 0;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_clipboard_copy (const gchar * event_group_name, const gchar * event_name,
                              gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    int first, last;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    first = min (input->mark, input->point);
    last = max (input->mark, input->point);

    if (last == first)
    {
        /* Copy selected files to clipboard */
        mc_event_raise (MCEVENT_GROUP_FILEMANAGER, "panel_save_current_file_to_clip_file", NULL);
        /* try use external clipboard utility */
        mc_event_raise (MCEVENT_GROUP_CORE, "clipboard_file_to_ext_clip", NULL);
    }
    else
    {

        g_free (kill_buffer);

        first = str_offset_to_pos (input->buffer, first);
        last = str_offset_to_pos (input->buffer, last);

        kill_buffer = g_strndup (input->buffer + first, last - first);

        mc_event_raise (MCEVENT_GROUP_CORE, "clipboard_text_to_file", kill_buffer);
        /* try use external clipboard utility */
        mc_event_raise (MCEVENT_GROUP_CORE, "clipboard_file_to_ext_clip", NULL);
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_clipboard_cut (const gchar * event_group_name, const gchar * event_name,
                             gpointer init_data, gpointer data)
{
    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    mc_winput_cmd_clipboard_copy (NULL, NULL, NULL, data);
    mc_winput_cmd_remove (NULL, NULL, NULL, data);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_yank (const gchar * event_group_name, const gchar * event_name,
                    gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    if (kill_buffer != NULL)
    {
        char *p;

        input->charpoint = 0;
        for (p = kill_buffer; *p != '\0'; p++)
            insert_char (input, *p);
        input->charpoint = 0;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_clipboard_paste (const gchar * event_group_name, const gchar * event_name,
                               gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    ev_clipboard_text_from_file_t event_data;
    char *p = NULL;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    /* try use external clipboard utility */
    mc_event_raise (MCEVENT_GROUP_CORE, "clipboard_file_from_ext_clip", NULL);

    event_data.text = &p;
    mc_event_raise (MCEVENT_GROUP_CORE, "clipboard_text_from_file", &event_data);
    if (event_data.ret)
    {
        char *pp;

        for (pp = p; *pp != '\0'; pp++)
            insert_char (input, *pp);

        g_free (p);
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_history_previous (const gchar * event_group_name, const gchar * event_name,
                                gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    GList *prev;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    if (input->history.list == NULL)
        return TRUE;

    if (input->need_push)
        push_history (input, input->buffer);

    prev = g_list_previous (input->history.current);
    if (prev != NULL)
    {
        input_assign_text (input, (char *) prev->data);
        input->history.current = prev;
        input->history.changed = TRUE;
        input->need_push = FALSE;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_history_next (const gchar * event_group_name, const gchar * event_name,
                            gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    GList *next;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    if (input->need_push)
    {
        push_history (input, input->buffer);
        input_assign_text (input, "");
        return TRUE;
    }

    if (input->history.list == NULL)
        return TRUE;

    next = g_list_next (input->history.current);
    if (next == NULL)
    {
        input_assign_text (input, "");
        input->history.current = input->history.list;
    }
    else
    {
        input_assign_text (input, (char *) next->data);
        input->history.current = next;
        input->history.changed = TRUE;
        input->need_push = FALSE;
    }

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_history_show (const gchar * event_group_name, const gchar * event_name,
                            gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;
    size_t len;
    char *r;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;


    len = get_history_length (input->history.list);

    r = history_show (&input->history.list, WIDGET (input),
                      g_list_position (input->history.list, input->history.list));
    if (r != NULL)
    {
        input_assign_text (input, r);
        g_free (r);
    }

    /* Has history cleaned up or not? */
    if (len != get_history_length (input->history.list))
        input->history.changed = TRUE;

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_complete (const gchar * event_group_name, const gchar * event_name,
                        gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    complete (input);
    input->is_complete_cmd = TRUE;


    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/* event callback */

static gboolean
mc_winput_cmd_enter_ctrl_sequence (const gchar * event_group_name, const gchar * event_name,
                                   gpointer init_data, gpointer data)
{
    WInput *input = (WInput *) data;

    (void) event_group_name;
    (void) event_name;
    (void) init_data;

    input_raw_handle_char (input, ascii_alpha_to_cntrl (tty_getch ()));

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */

static void
mc_winput_init_events (GError ** error)
{
    /* *INDENT-OFF* */
    event_init_t events[] =
    {
        {MC_WINPUT_EVENT_GROUP, "start_highlight", mc_winput_cmd_start_highlight, NULL},
        {MC_WINPUT_EVENT_GROUP, "stop_highlight", mc_winput_cmd_stop_highlight, NULL},
        {MC_WINPUT_EVENT_GROUP, "begin_of_line", mc_winput_cmd_begin_of_line, NULL},
        {MC_WINPUT_EVENT_GROUP, "end_of_line", mc_winput_cmd_end_of_line, NULL},
        {MC_WINPUT_EVENT_GROUP, "backward_char", mc_winput_cmd_backward_char, NULL},
        {MC_WINPUT_EVENT_GROUP, "backward_word", mc_winput_cmd_backward_word, NULL},
        {MC_WINPUT_EVENT_GROUP, "forward_char", mc_winput_cmd_forward_char, NULL},
        {MC_WINPUT_EVENT_GROUP, "forward_word", mc_winput_cmd_forward_word, NULL},
        {MC_WINPUT_EVENT_GROUP, "backspace", mc_winput_cmd_backspace, NULL},
        {MC_WINPUT_EVENT_GROUP, "delete", mc_winput_cmd_delete, NULL},
        {MC_WINPUT_EVENT_GROUP, "kill_word", mc_winput_cmd_kill_word, NULL},
        {MC_WINPUT_EVENT_GROUP, "back_kill_word", mc_winput_cmd_back_kill_word, NULL},
        {MC_WINPUT_EVENT_GROUP, "mark", mc_winput_cmd_mark, NULL},
        {MC_WINPUT_EVENT_GROUP, "remove", mc_winput_cmd_remove, NULL},
        {MC_WINPUT_EVENT_GROUP, "delete_to_end", mc_winput_cmd_delete_to_end, NULL},
        {MC_WINPUT_EVENT_GROUP, "clear_all", mc_winput_cmd_clear_all, NULL},
        {MC_WINPUT_EVENT_GROUP, "clipboard_copy", mc_winput_cmd_clipboard_copy, NULL},
        {MC_WINPUT_EVENT_GROUP, "clipboard_cut", mc_winput_cmd_clipboard_cut, NULL},
        {MC_WINPUT_EVENT_GROUP, "yank", mc_winput_cmd_yank, NULL},
        {MC_WINPUT_EVENT_GROUP, "clipboard_paste", mc_winput_cmd_clipboard_paste, NULL},
        {MC_WINPUT_EVENT_GROUP, "history_previous", mc_winput_cmd_history_previous, NULL},
        {MC_WINPUT_EVENT_GROUP, "history_next", mc_winput_cmd_history_next, NULL},
        {MC_WINPUT_EVENT_GROUP, "history_show", mc_winput_cmd_history_show, NULL},
        {MC_WINPUT_EVENT_GROUP, "complete", mc_winput_cmd_complete, NULL},
        {MC_WINPUT_EVENT_GROUP, "enter_ctrl_sequence", mc_winput_cmd_enter_ctrl_sequence, NULL},


        {NULL, NULL, NULL, NULL}
    };
    /* *INDENT-ON* */

    mc_event_mass_add (events, error);
}

/* --------------------------------------------------------------------------------------------- */

static void
mc_winput_bind_events_to_keymap (GError ** error)
{
    /* *INDENT-OFF* */
    mc_keymap_event_init_t keymap_events[] =
    {
        {MC_WINPUT_KEYMAP_GROUP, "MarkLeft", MC_WINPUT_EVENT_GROUP, "start_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkRight", MC_WINPUT_EVENT_GROUP, "start_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToWordBegin", MC_WINPUT_EVENT_GROUP, "start_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToWordEnd", MC_WINPUT_EVENT_GROUP, "start_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToHome", MC_WINPUT_EVENT_GROUP, "start_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToEnd", MC_WINPUT_EVENT_GROUP, "start_highlight"},

        {MC_WINPUT_KEYMAP_GROUP, "WordRight", MC_WINPUT_EVENT_GROUP, "stop_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "WordLeft", MC_WINPUT_EVENT_GROUP, "stop_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "Right", MC_WINPUT_EVENT_GROUP, "stop_highlight"},
        {MC_WINPUT_KEYMAP_GROUP, "Left", MC_WINPUT_EVENT_GROUP, "stop_highlight"},

        {MC_WINPUT_KEYMAP_GROUP, "Home", MC_WINPUT_EVENT_GROUP, "begin_of_line"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToHome", MC_WINPUT_EVENT_GROUP, "begin_of_line"},

        {MC_WINPUT_KEYMAP_GROUP, "End", MC_WINPUT_EVENT_GROUP, "end_of_line"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToEnd", MC_WINPUT_EVENT_GROUP, "end_of_line"},

        {MC_WINPUT_KEYMAP_GROUP, "Left", MC_WINPUT_EVENT_GROUP, "backward_char"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkLeft", MC_WINPUT_EVENT_GROUP, "backward_char"},

        {MC_WINPUT_KEYMAP_GROUP, "WordLeft", MC_WINPUT_EVENT_GROUP, "backward_word"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToWordBegin", MC_WINPUT_EVENT_GROUP, "backward_word"},

        {MC_WINPUT_KEYMAP_GROUP, "Right", MC_WINPUT_EVENT_GROUP, "forward_char"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkRight", MC_WINPUT_EVENT_GROUP, "forward_char"},

        {MC_WINPUT_KEYMAP_GROUP, "WordRight", MC_WINPUT_EVENT_GROUP, "forward_word"},
        {MC_WINPUT_KEYMAP_GROUP, "MarkToWordEnd", MC_WINPUT_EVENT_GROUP, "forward_word"},

        {MC_WINPUT_KEYMAP_GROUP, "Backspace", MC_WINPUT_EVENT_GROUP, "backspace"},

        {MC_WINPUT_KEYMAP_GROUP, "Delete", MC_WINPUT_EVENT_GROUP, "delete"},

        {MC_WINPUT_KEYMAP_GROUP, "DeleteToWordEnd", MC_WINPUT_EVENT_GROUP, "kill_word"},

        {MC_WINPUT_KEYMAP_GROUP, "DeleteToWordBegin", MC_WINPUT_EVENT_GROUP, "back_kill_word"},

        {MC_WINPUT_KEYMAP_GROUP, "Mark", MC_WINPUT_EVENT_GROUP, "mark"},

        {MC_WINPUT_KEYMAP_GROUP, "Remove", MC_WINPUT_EVENT_GROUP, "remove"},

        {MC_WINPUT_KEYMAP_GROUP, "DeleteToEnd", MC_WINPUT_EVENT_GROUP, "delete_to_end"},

        {MC_WINPUT_KEYMAP_GROUP, "Clear", MC_WINPUT_EVENT_GROUP, "clear_all"},

        {MC_WINPUT_KEYMAP_GROUP, "Store", MC_WINPUT_EVENT_GROUP, "clipboard_copy"},
        {MC_WINPUT_KEYMAP_GROUP, "ClipboardCopy", MC_WINPUT_EVENT_GROUP, "clipboard_copy"},

        {MC_WINPUT_KEYMAP_GROUP, "Cut", MC_WINPUT_EVENT_GROUP, "clipboard_cut"},
        {MC_WINPUT_KEYMAP_GROUP, "ClipboardCut", MC_WINPUT_EVENT_GROUP, "clipboard_cut"},

        {MC_WINPUT_KEYMAP_GROUP, "Yank", MC_WINPUT_EVENT_GROUP, "yank"},

        {MC_WINPUT_KEYMAP_GROUP, "Paste", MC_WINPUT_EVENT_GROUP, "clipboard_paste"},
        {MC_WINPUT_KEYMAP_GROUP, "ClipboardPaste", MC_WINPUT_EVENT_GROUP, "clipboard_paste"},

        {MC_WINPUT_KEYMAP_GROUP, "HistoryPrev", MC_WINPUT_EVENT_GROUP, "history_previous"},

        {MC_WINPUT_KEYMAP_GROUP, "HistoryNext", MC_WINPUT_EVENT_GROUP, "history_next"},

        {MC_WINPUT_KEYMAP_GROUP, "History", MC_WINPUT_EVENT_GROUP, "history_show"},

        {MC_WINPUT_KEYMAP_GROUP, "Complete", MC_WINPUT_EVENT_GROUP, "complete"},

        {MC_WINPUT_KEYMAP_GROUP, "EnterCtrlSeq", MC_WINPUT_EVENT_GROUP, "enter_ctrl_sequence"},

        {NULL, NULL, NULL, NULL}
    };
    /* *INDENT-ON* */

    mc_keymap_mass_bind_event (keymap_events, error);
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

void
mc_winput_set_default_colors (void)
{
    input_colors[WINPUTC_MAIN] = INPUT_COLOR;
    input_colors[WINPUTC_MARK] = INPUT_MARK_COLOR;
    input_colors[WINPUTC_UNCHANGED] = INPUT_UNCHANGED_COLOR;
    input_colors[WINPUTC_HISTORY] = INPUT_HISTORY_COLOR;
}

/* --------------------------------------------------------------------------------------------- */

void
mc_winput_init (GError ** error)
{
    mc_winput_set_default_colors ();
    mc_winput_init_events (error);
    mc_winput_bind_events_to_keymap (error);
}

/* --------------------------------------------------------------------------------------------- */

/** Create new instance of WInput object.
  * @param y                    Y coordinate
  * @param x                    X coordinate
  * @param input_colors         Array of used colors
  * @param width                Widget width
  * @param def_text             Default text filled in widget
  * @param histname             Name of history
  * @param completion_flags     Flags for specify type of completions
  * @return                     WInput object
  */
WInput *
input_new (int y, int x, const int *colors, int width, const char *def_text,
           const char *histname, input_complete_t completion_flags)
{
    WInput *in;
    Widget *w;

    in = g_new (WInput, 1);
    w = WIDGET (in);
    widget_init (w, y, x, 1, width, input_callback, input_event);
    w->options |= W_IS_INPUT;
    w->set_options = input_set_options_callback;

    in->color = colors;
    in->first = TRUE;
    in->highlight = FALSE;
    in->is_highlight_cmd = FALSE;
    in->is_complete_cmd = FALSE;
    in->term_first_shown = 0;
    in->disable_update = 0;
    in->is_password = FALSE;
    in->strip_password = FALSE;

    /* in->buffer will be corrected in "history_load" event handler */
    in->current_max_size = width + 1;
    in->buffer = g_new0 (char, in->current_max_size);

    /* init completions before input_assign_text() call */
    in->completions = NULL;
    in->completion_flags = completion_flags;

    in->init_from_history = (def_text == INPUT_LAST_TEXT);

    if (in->init_from_history || def_text == NULL)
        def_text = "";

    input_assign_text (in, def_text);

    /* prepare to history setup */
    in->history.list = NULL;
    in->history.current = NULL;
    in->history.changed = FALSE;
    in->history.name = NULL;
    if ((histname != NULL) && (*histname != '\0'))
        in->history.name = g_strdup (histname);
    /* history will be loaded later */

    in->label = NULL;

    return in;
}

/* --------------------------------------------------------------------------------------------- */

cb_ret_t
input_callback (Widget * w, Widget * sender, widget_msg_t msg, int parm, void *data)
{
    WInput *in = INPUT (w);
    cb_ret_t v;

    switch (msg)
    {
    case MSG_INIT:
        /* subscribe to "history_load" event */
        mc_event_add (w->owner->event_group, MCEVENT_HISTORY_LOAD, input_load_history, w, NULL);
        /* subscribe to "history_save" event */
        mc_event_add (w->owner->event_group, MCEVENT_HISTORY_SAVE, input_save_history, w, NULL);
        return MSG_HANDLED;

    case MSG_KEY:
        /* Keys we want others to handle */
        if (parm == KEY_UP || parm == KEY_DOWN || parm == ESC_CHAR
            || parm == KEY_F (10) || parm == '\n')
            return MSG_NOT_HANDLED;

        /* When pasting multiline text, insert literal Enter */
        if ((parm & ~KEY_M_MASK) == '\n')
        {
            v = input_raw_handle_char (in, '\n');
            return v;
        }

        return input_handle_char (in, parm);

    case MSG_RESIZE:
    case MSG_FOCUS:
    case MSG_UNFOCUS:
    case MSG_DRAW:
        input_update (in, FALSE);
        return MSG_HANDLED;

    case MSG_CURSOR:
        widget_move (in, 0, str_term_width2 (in->buffer, in->point) - in->term_first_shown);
        return MSG_HANDLED;

    case MSG_DESTROY:
        /* unsubscribe from "history_load" event */
        mc_event_del (w->owner->event_group, MCEVENT_HISTORY_LOAD, input_load_history, w);
        /* unsubscribe from "history_save" event */
        mc_event_del (w->owner->event_group, MCEVENT_HISTORY_SAVE, input_save_history, w);
        input_destroy (in);
        return MSG_HANDLED;

    default:
        return widget_default_callback (w, sender, msg, parm, data);
    }
}

/* --------------------------------------------------------------------------------------------- */

cb_ret_t
input_handle_char (WInput * in, int key)
{
    cb_ret_t v;
    gboolean keymap_handled;

    keymap_handled = mc_keymap_process_group (MC_WINPUT_KEYMAP_GROUP, key, in, NULL);

    if (!in->is_highlight_cmd)
        in->highlight = FALSE;
    in->is_highlight_cmd = FALSE;

    if (!keymap_handled)
    {
        if (key > 255)
            return MSG_NOT_HANDLED;
        if (in->first)
            port_region_marked_for_delete (in);
        input_free_completions (in);
        v = insert_char (in, key);

    }
    else
    {
        if (!in->is_complete_cmd)
            input_free_completions (in);
        in->is_complete_cmd = FALSE;
        v = MSG_HANDLED;
        if (in->first)
            input_update (in, TRUE);    /* needed to clear in->first */
    }

    input_update (in, TRUE);
    return v;
}

/* --------------------------------------------------------------------------------------------- */

/* This function is a test for a special input key used in complete.c */
/* Returns 0 if it is not a special key, 1 if it is a non-complete key
   and 2 if it is a complete key */
int
input_key_is_in_map (int key)
{
    const char *key_name;

    key_name = mc_keymap_get_key_name_by_code (MC_WINPUT_KEYMAP_GROUP, key, NULL);
    if (key_name == NULL)
        return 0;

    return strcmp (key_name, "Complete") == 0 ? 2 : 1;
}

/* --------------------------------------------------------------------------------------------- */

void
input_assign_text (WInput * in, const char *text)
{
    Widget *w = WIDGET (in);
    size_t text_len, buffer_len;

    if (text == NULL)
        text = "";

    input_free_completions (in);
    in->mark = 0;
    in->need_push = TRUE;
    in->charpoint = 0;

    text_len = strlen (text);
    buffer_len = 1 + max ((size_t) w->cols, text_len);
    in->current_max_size = buffer_len;
    if (buffer_len > (size_t) w->cols)
        in->buffer = g_realloc (in->buffer, buffer_len);
    memmove (in->buffer, text, text_len + 1);
    in->point = str_length (in->buffer);
    input_update (in, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

/* Inserts text in input line */
void
input_insert (WInput * in, const char *text, gboolean insert_extra_space)
{
    input_disable_update (in);
    while (*text != '\0')
        input_handle_char (in, (unsigned char) *text++);        /* unsigned extension char->int */
    if (insert_extra_space)
        input_handle_char (in, ' ');
    input_enable_update (in);
    input_update (in, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
input_set_point (WInput * in, int pos)
{
    int max_pos;

    max_pos = str_length (in->buffer);
    pos = min (pos, max_pos);
    if (pos != in->point)
        input_free_completions (in);
    in->point = pos;
    in->charpoint = 0;
    input_update (in, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
input_update (WInput * in, gboolean clear_first)
{
    Widget *w = WIDGET (in);
    int has_history = 0;
    int buf_len;
    const char *cp;
    int pw;

    if (in->disable_update != 0)
        return;

    /* don't draw widget not put into dialog */
    if (w->owner == NULL || w->owner->state != DLG_ACTIVE)
        return;

    if (should_show_history_button (in))
        has_history = HISTORY_BUTTON_WIDTH;

    buf_len = str_length (in->buffer);

    /* Adjust the mark */
    in->mark = min (in->mark, buf_len);

    pw = str_term_width2 (in->buffer, in->point);

    /* Make the point visible */
    if ((pw < in->term_first_shown) || (pw >= in->term_first_shown + w->cols - has_history))
    {
        in->term_first_shown = pw - (w->cols / 3);
        if (in->term_first_shown < 0)
            in->term_first_shown = 0;
    }

    if (has_history != 0)
        draw_history_button (in);

    if ((w->options & W_DISABLED) != 0)
        tty_setcolor (DISABLED_COLOR);
    else if (in->first)
        tty_setcolor (in->color[WINPUTC_UNCHANGED]);
    else
        tty_setcolor (in->color[WINPUTC_MAIN]);

    widget_move (in, 0, 0);

    if (!in->is_password)
    {
        if (!in->highlight)
            tty_print_string (str_term_substring (in->buffer, in->term_first_shown,
                                                  w->cols - has_history));
        else
        {
            long m1, m2;

            if (input_eval_marks (in, &m1, &m2))
            {
                tty_setcolor (in->color[WINPUTC_MAIN]);
                cp = str_term_substring (in->buffer, in->term_first_shown, w->cols - has_history);
                tty_print_string (cp);
                tty_setcolor (in->color[WINPUTC_MARK]);
                if (m1 < in->term_first_shown)
                {
                    widget_move (in, 0, 0);
                    tty_print_string (str_term_substring
                                      (in->buffer, in->term_first_shown,
                                       m2 - in->term_first_shown));
                }
                else
                {
                    int sel_width, buf_width;

                    widget_move (in, 0, m1 - in->term_first_shown);
                    buf_width = str_term_width2 (in->buffer, m1);
                    sel_width =
                        min (m2 - m1, (w->cols - has_history) - (buf_width - in->term_first_shown));
                    tty_print_string (str_term_substring (in->buffer, m1, sel_width));
                }
            }
        }
    }
    else
    {
        int i;

        cp = str_term_substring (in->buffer, in->term_first_shown, w->cols - has_history);
        tty_setcolor (in->color[WINPUTC_MAIN]);
        for (i = 0; i < w->cols - has_history; i++)
        {
            if (i < (buf_len - in->term_first_shown) && cp[0] != '\0')
                tty_print_char ('*');
            else
                tty_print_char (' ');
            if (cp[0] != '\0')
                str_cnext_char (&cp);
        }
    }

    if (clear_first)
        in->first = FALSE;
}

/* --------------------------------------------------------------------------------------------- */

void
input_enable_update (WInput * in)
{
    in->disable_update--;
    input_update (in, FALSE);
}

/* --------------------------------------------------------------------------------------------- */

void
input_disable_update (WInput * in)
{
    in->disable_update++;
}

/* --------------------------------------------------------------------------------------------- */

/**
  *  Cleans the input line and adds the current text to the history
  *
  *  @param in the input line
  */
void
input_clean (WInput * in)
{
    push_history (in, in->buffer);
    in->need_push = TRUE;
    in->buffer[0] = '\0';
    in->point = 0;
    in->charpoint = 0;
    in->mark = 0;
    in->highlight = FALSE;
    input_free_completions (in);
    input_update (in, FALSE);
}

/* --------------------------------------------------------------------------------------------- */

void
input_free_completions (WInput * in)
{
    g_strfreev (in->completions);
    in->completions = NULL;
}

/* --------------------------------------------------------------------------------------------- */
