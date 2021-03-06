/**
 * vimb - a webkit based vim like browser.
 *
 * Copyright (C) 2012-2013 Daniel Carl
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

/**
 * This file contains function to handle input editing, parsing of called ex
 * commands from inputbox and the ex commands.
 */
#include <ctype.h>
#include <sys/wait.h>
#include "config.h"
#include "main.h"
#include "ex.h"
#include "ascii.h"
#include "completion.h"
#include "hints.h"
#include "mode.h"
#include "command.h"
#include "history.h"
#include "dom.h"
#include "setting.h"
#include "util.h"
#include "bookmark.h"
#include "shortcut.h"
#include "map.h"

typedef enum {
    EX_BMA,
    EX_BMR,
    EX_EVAL,
    EX_HARDCOPY,
    EX_CMAP,
    EX_CNOREMAP,
    EX_IMAP,
    EX_NMAP,
    EX_NNOREMAP,
    EX_CUNMAP,
    EX_IUNMAP,
    EX_INOREMAP,
    EX_NUNMAP,
    EX_NORMAL,
    EX_OPEN,
#ifdef FEATURE_QUEUE
    EX_QCLEAR,
    EX_QPOP,
    EX_QPUSH,
    EX_QUNSHIFT,
#endif
    EX_QUIT,
    EX_SAVE,
    EX_SCA,
    EX_SCD,
    EX_SCR,
    EX_SET,
    EX_SHELLCMD,
    EX_TABOPEN,
} ExCode;

typedef struct {
    int        count;    /* commands count */
    int        idx;      /* index in commands array */
    const char *name;    /* name of the command */
    ExCode     code;     /* id of the command */
    gboolean   bang;     /* if the command was called with a bang ! */
    GString    *lhs;     /* left hand side of the command - single word */
    GString    *rhs;     /* right hand side of the command - multiple words */
    int        flags;    /* flags for the already parsed command */
} ExArg;

typedef gboolean (*ExFunc)(const ExArg *arg);

typedef struct {
    const char *name;         /* full name of the command even if called abreviated */
    ExCode    code;           /* constant id for the command */
    ExFunc    func;
#define EX_FLAG_NONE   0x000  /* no flags set */
#define EX_FLAG_BANG   0x001  /* command uses the bang ! after command name */
#define EX_FLAG_LHS    0x002  /* command has a single word after the command name */
#define EX_FLAG_RHS    0x004  /* command has a right hand side */
#define EX_FLAG_EXP    0x008  /* expand pattern like % or %:p in rhs */
    int        flags;
} ExInfo;

static void input_activate(void);
static gboolean parse(const char **input, ExArg *arg);
static gboolean parse_count(const char **input, ExArg *arg);
static gboolean parse_command_name(const char **input, ExArg *arg);
static gboolean parse_bang(const char **input, ExArg *arg);
static gboolean parse_lhs(const char **input, ExArg *arg);
static gboolean parse_rhs(const char **input, ExArg *arg);
static void expand_input(const char **input, ExArg *arg);
static void skip_whitespace(const char **input);
static void free_cmdarg(ExArg *arg);
static gboolean execute(const ExArg *arg);

static gboolean ex_bookmark(const ExArg *arg);
static gboolean ex_eval(const ExArg *arg);
static gboolean ex_hardcopy(const ExArg *arg);
static gboolean ex_map(const ExArg *arg);
static gboolean ex_unmap(const ExArg *arg);
static gboolean ex_normal(const ExArg *arg);
static gboolean ex_open(const ExArg *arg);
#ifdef FEATURE_QUEUE
static gboolean ex_queue(const ExArg *arg);
#endif
static gboolean ex_quit(const ExArg *arg);
static gboolean ex_save(const ExArg *arg);
static gboolean ex_set(const ExArg *arg);
static gboolean ex_shellcmd(const ExArg *arg);
static gboolean ex_shortcut(const ExArg *arg);

static gboolean complete(short direction);
static void completion_select(char *match);
static gboolean history(gboolean prev);
static void history_rewind(void);

/* The order of following command names is significant. If there exists
 * ambiguous commands matching to the users input, the first defined will be
 * the prefered match.
 * Also the sorting and grouping of command names matters, so we give up
 * searching for a matching command if the next compared character did not
 * match. */
static ExInfo commands[] = {
    /* command           code            func           flags */
    {"bma",              EX_BMA,         ex_bookmark,   EX_FLAG_RHS},
    {"bmr",              EX_BMR,         ex_bookmark,   EX_FLAG_RHS},
    {"cmap",             EX_CMAP,        ex_map,        EX_FLAG_LHS|EX_FLAG_RHS},
    {"cnoremap",         EX_CNOREMAP,    ex_map,        EX_FLAG_LHS|EX_FLAG_RHS},
    {"cunmap",           EX_CUNMAP,      ex_unmap,      EX_FLAG_LHS},
    {"hardcopy",         EX_HARDCOPY,    ex_hardcopy,   EX_FLAG_NONE},
    {"eval",             EX_EVAL,        ex_eval,       EX_FLAG_RHS},
    {"imap",             EX_IMAP,        ex_map,        EX_FLAG_LHS|EX_FLAG_RHS},
    {"inoremap",         EX_INOREMAP,    ex_map,        EX_FLAG_LHS|EX_FLAG_RHS},
    {"iunmap",           EX_IUNMAP,      ex_unmap,      EX_FLAG_LHS},
    {"nmap",             EX_NMAP,        ex_map,        EX_FLAG_LHS|EX_FLAG_RHS},
    {"nnoremap",         EX_NNOREMAP,    ex_map,        EX_FLAG_LHS|EX_FLAG_RHS},
    {"normal",           EX_NORMAL,      ex_normal,     EX_FLAG_BANG|EX_FLAG_LHS},
    {"nunmap",           EX_NUNMAP,      ex_unmap,      EX_FLAG_LHS},
    {"open",             EX_OPEN,        ex_open,       EX_FLAG_RHS},
    {"quit",             EX_QUIT,        ex_quit,       EX_FLAG_NONE},
#ifdef FEATURE_QUEUE
    {"qunshift",         EX_QUNSHIFT,    ex_queue,      EX_FLAG_RHS},
    {"qclear",           EX_QCLEAR,      ex_queue,      EX_FLAG_RHS},
    {"qpop",             EX_QPOP,        ex_queue,      EX_FLAG_NONE},
    {"qpush",            EX_QPUSH,       ex_queue,      EX_FLAG_RHS},
#endif
    {"save",             EX_SAVE,        ex_save,       EX_FLAG_RHS|EX_FLAG_EXP},
    {"set",              EX_SET,         ex_set,        EX_FLAG_RHS},
    {"shellcmd",         EX_SHELLCMD,    ex_shellcmd,   EX_FLAG_RHS|EX_FLAG_EXP},
    {"shortcut-add",     EX_SCA,         ex_shortcut,   EX_FLAG_RHS},
    {"shortcut-default", EX_SCD,         ex_shortcut,   EX_FLAG_RHS},
    {"shortcut-remove",  EX_SCR,         ex_shortcut,   EX_FLAG_RHS},
    {"tabopen",          EX_TABOPEN,     ex_open,       EX_FLAG_RHS},
};

static struct {
    guint count;
    char  *prefix;  /* completion prefix like :, ? and / */
    char  *current; /* holds the current written input box content */
} excomp;

static struct {
    char  *prefix;  /* prefix that is prepended to the history item to for the complete command */
    char  *query;   /* part of input text to match the history items */
    GList *active;
} exhist;

extern VbCore vb;


/**
 * Function called when vimb enters the command mode.
 */
void ex_enter(void)
{
    gtk_widget_grab_focus(GTK_WIDGET(vb.gui.input));
    dom_clear_focus(vb.gui.webview);
}

/**
 * Called when the command mode is left.
 */
void ex_leave(void)
{
    completion_clean();
    hints_clear();
}

/**
 * Handles the keypress events from webview and inputbox.
 */
VbResult ex_keypress(int key)
{
    GtkTextIter start, end;
    GtkTextBuffer *buffer = vb.gui.buffer;
    GtkTextMark *mark;

    /* delegate call to the submode */
    if (RESULT_COMPLETE == hints_keypress(key)) {
        return RESULT_COMPLETE;
    }

    switch (key) {
        case KEY_TAB:
            complete(1);
            break;

        case KEY_SHIFT_TAB:
            complete(-1);
            break;

        case CTRL('['):
        case CTRL('C'):
            mode_enter('n');
            vb_set_input_text("");
            break;

        case KEY_CR:
            input_activate();
            break;

        case KEY_UP:
            history(true);
            break;

        case KEY_DOWN:
            history(false);
            break;

        /* basic command line editing */
        case CTRL('H'):
            /* delete the last char before the cursor */
            mark = gtk_text_buffer_get_insert(buffer);
            gtk_text_buffer_get_iter_at_mark(buffer, &start, mark);
            gtk_text_buffer_backspace(buffer, &start, true, true);
            break;

        case CTRL('W'):
            /* delete word backward from cursor */
            mark = gtk_text_buffer_get_insert(buffer);
            gtk_text_buffer_get_iter_at_mark(buffer, &end, mark);

            /* copy the iter to build start and end point for deletion */
            start = end;

            /* move the iterator to the beginning of previous word */
            if (gtk_text_iter_backward_word_start(&start)) {
                gtk_text_buffer_delete(buffer, &start, &end);
            }
            break;

        case CTRL('B'):
            /* move the cursor direct behind the prompt */
            gtk_text_buffer_get_iter_at_offset(buffer, &start, strlen(vb.state.prompt));
            gtk_text_buffer_place_cursor(buffer, &start);
            break;

        case CTRL('E'):
            /* move the cursor to the end of line */
            gtk_text_buffer_get_end_iter(buffer, &start);
            gtk_text_buffer_place_cursor(buffer, &start);
            break;

        case CTRL('U'):
            /* remove everythings between cursor and prompt */
            mark = gtk_text_buffer_get_insert(buffer);
            gtk_text_buffer_get_iter_at_mark(buffer, &end, mark);
            gtk_text_buffer_get_iter_at_offset(buffer, &start, strlen(vb.state.prompt));
            gtk_text_buffer_delete(buffer, &start, &end);
            break;

        default:
            /* if is printable ascii char, than write it at the cursor
             * position into input box */
            if (key >= 0x20 && key <= 0x7e) {
                gtk_text_buffer_insert_at_cursor(buffer, (char[2]){key, 0}, 1);
            } else {
                vb.state.processed_key = false;
            }
    }

    return RESULT_COMPLETE;
}

/**
 * Handles changes in the inputbox.
 */
void ex_input_changed(const char *text)
{
    gboolean forward = false;
    GtkTextIter start, end;
    GtkTextBuffer *buffer = vb.gui.buffer;

    if (gtk_text_buffer_get_line_count(buffer) > 1) {
        /* remove everething from the buffer, except of the first line */
        gtk_text_buffer_get_iter_at_line(buffer, &start, 0);
        if (gtk_text_iter_forward_to_line_end(&start)) {
            gtk_text_buffer_get_end_iter(buffer, &end);
            gtk_text_buffer_delete(buffer, &start, &end);
        }
    }

    switch (*text) {
        case ';':
            hints_create(text);
            break;

        case '/': forward = true; /* fall through */
        case '?':
            webkit_web_view_unmark_text_matches(vb.gui.webview);
            webkit_web_view_search_text(vb.gui.webview, &text[1], false, forward, false);
            break;
    }
}

gboolean ex_fill_completion(GtkListStore *store, const char *input)
{
    GtkTreeIter iter;
    ExInfo *cmd;
    gboolean found = false;

    if (!input || *input == '\0') {
        for (int i = 0; i < LENGTH(commands); i++) {
            cmd = &commands[i];
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, COMPLETION_STORE_FIRST, cmd->name, -1);
            found = true;
        }
    } else {
        for (int i = 0; i < LENGTH(commands); i++) {
            cmd = &commands[i];
            if (g_str_has_prefix(cmd->name, input)) {
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, COMPLETION_STORE_FIRST, cmd->name, -1);
                found = true;
            }
        }
    }

    return found;
}

/**
 * This is called if the user typed <nl> or <cr> into the inputbox.
 */
static void input_activate(void)
{
    int count = -1;
    char *text, *cmd;
    text = vb_get_input_text();

    /* skip leading promt char like ':' or '/' */
    /* TODO should we use a flag to determine if we should record the command
     * into the history - maybe it's not good to save commands in history that
     * where triggered by a map like ':name \, :set scripts!<cr>' - by the way
     * does vim also skip history recording for such mapped commands */
    cmd = text + 1;
    switch (*text) {
        case '/': count = 1; /* fall throught */
        case '?':
            history_add(HISTORY_SEARCH, cmd, NULL);
            mode_enter('n');
            command_search(&((Arg){count, cmd}));
            break;

        case ';':
            hints_fire();
            break;

        case ':':
            history_add(HISTORY_COMMAND, cmd, NULL);
            mode_enter('n');
            ex_run_string(cmd);
            break;

    }
    g_free(text);
}

gboolean ex_run_string(const char *input)
{
    ExArg *arg = g_new0(ExArg, 1);
    arg->lhs   = g_string_new("");
    arg->rhs   = g_string_new("");

    while (input && *input) {
        if (!parse(&input, arg) || !execute(arg)) {
            free_cmdarg(arg);
            return false;
        }
    }
    free_cmdarg(arg);

    return true;
}

/**
 * Parses given input string into given ExArg pointer.
 */
static gboolean parse(const char **input, ExArg *arg)
{
    if (!*input || !**input) {
        return false;
    }

    /* trunkacate string from potentially previous run */
    g_string_truncate(arg->lhs, 0);
    g_string_truncate(arg->rhs, 0);

    /* remove leading whitespace and : */
    while (**input && (**input == ':' || **input == ' ')) {
        (*input)++;
    }
    parse_count(input, arg);

    skip_whitespace(input);
    if (!parse_command_name(input, arg)) {
        return false;
    }

    /* parse the bang if this is allowed */
    if (arg->flags & EX_FLAG_BANG) {
        parse_bang(input, arg);
    }

    /* parse the lhs if this is available */
    skip_whitespace(input);
    if (arg->flags & EX_FLAG_LHS) {
        parse_lhs(input, arg);
    }
    /* parse the rhs if this is available */
    skip_whitespace(input);
    if (arg->flags & EX_FLAG_RHS) {
        parse_rhs(input, arg);
    }

    if (**input) {
        (*input)++;
    }

    return true;
}

/**
 * Parses possible found count of given input into ExArg pointer.
 */
static gboolean parse_count(const char **input, ExArg *arg)
{
    if (!*input || !isdigit(**input)) {
        arg->count = 0;
    } else {
        do {
            arg->count = arg->count * 10 + (**input - '0');
            (*input)++;
        } while (isdigit(**input));
    }
    return true;
}

/**
 * Parse the command name from given input.
 */
static gboolean parse_command_name(const char **input, ExArg *arg)
{
    int len      = 0;
    int first    = 0;   /* number of first found command */
    int matches  = 0;   /* number of commands that matches the input */
    char cmd[20] = {0}; /* name of found command */

    do {
        /* copy the next char into the cmd buffer */
        cmd[len++] = **input;
        int i;
        for (i = first, matches = 0; i < LENGTH(commands); i++) {
            /* commands are grouped by their first letters, if we reached the
             * end of the group there are no more possible matches to find */
            if (len > 1 && strncmp(commands[i].name, cmd, len - 1)) {
                break;
            }
            if (commands[i].name[len - 1] == **input) {
                /* partial match found */
                if (!matches) {
                    /* if this is the first then remeber it */
                    first = i;
                }
                matches++;
            }
        }
        (*input)++;
    } while (matches > 0 && **input && **input != ' ' && **input != '!');

    if (!matches) {
        /* read until next whitespace or end of input to get command name for
         * error message - vim uses the whole rest of the input string - but
         * the first word seems to bee enough for the error message */
        for (; len < LENGTH(cmd) && *input && **input != ' '; (*input)++) {
            cmd[len++] = **input;
        }
        cmd[len] = '\0';

        vb_echo(VB_MSG_ERROR, true, "Unknown command: %s", cmd);
        return false;
    }

    arg->idx   = first;
    arg->code  = commands[first].code;
    arg->name  = commands[first].name;
    arg->flags = commands[first].flags;

    return true;
}

/**
 * Parse a single bang ! after the command.
 */
static gboolean parse_bang(const char **input, ExArg *arg)
{
    if (*input && **input == '!') {
        arg->bang = true;
        (*input)++;
    }
    return true;
}

/**
 * Parse a single word left hand side of a command arg.
 */
static gboolean parse_lhs(const char **input, ExArg *arg)
{
    char quote = '\\';

    if (!*input || !**input) {
        return false;
    }

    /* get the char until the next none escaped whitespace and save it into
     * the lhs */
    while (**input && **input != ' ') {
        /* if we find a backslash this escapes the next whitespace */
        if (**input == quote) {
            /* move pointer to the next char */
            (*input)++;
            if (!*input) {
                /* if input ends here - use only the backslash */
                g_string_append_c(arg->lhs, quote);
            } else if (**input == ' ') {
                /* escaped whitespace becomes only whitespace */
                g_string_append_c(arg->lhs, **input);
            } else {
                /* put escape char and next char into the result string */
                g_string_append_c(arg->lhs, quote);
                g_string_append_c(arg->lhs, **input);
            }
        } else {
            /* unquoted char */
            g_string_append_c(arg->lhs, **input);
        }
        (*input)++;
    }
    return true;
}

/**
 * Parses the right hand side of command args.
 */
static gboolean parse_rhs(const char **input, ExArg *arg)
{
    char quote = '\\';

    if (!*input || !**input) {
        return false;
    }

    /* get char until the end of command */
    while (**input && **input != '\n' && **input != '|') {
        /* if we find a backslash this escapes the next char */
        if (**input == quote) {
            /* move pointer to the next char */
            (*input)++;
            if (!*input) {
                /* if input ends here - use only the backslash */
                g_string_append_c(arg->rhs, quote);
            } else if (**input == '|') {
                /* escaped char becomes only char */
                g_string_append_c(arg->rhs, **input);
            } else {
                /* put escape char and next char into the result string */
                g_string_append_c(arg->rhs, quote);
                g_string_append_c(arg->rhs, **input);
            }
        } else { /* unquoted char */
            /* check for expansion placeholder */
            if (arg->flags & EX_FLAG_EXP && strchr("%~", **input)) {
                /* handle expansion */
                expand_input(input, arg);
            } else {
                g_string_append_c(arg->rhs, **input);
            }
        }
        (*input)++;
    }
    return true;
}

static void expand_input(const char **input, ExArg *arg)
{
    const char *uri;
    switch (**input) {
        case '%':
            if ((uri = GET_URI())) {
                /* TODO check for modifiers like :h:t:r:e */
                g_string_append(arg->rhs, uri);
            }
            break;

        case '~':
            (*input)++;
            /* expand only ~/ because ~user is not handled at the moment */
            if (**input == '/') {
                g_string_append(arg->rhs, g_get_home_dir());
                g_string_append_c(arg->rhs, **input);
            }
            break;
    }
}

/**
 * Executes the command given by ExArg.
 */
static gboolean execute(const ExArg *arg)
{
    return (commands[arg->idx].func)(arg);
}

static void skip_whitespace(const char **input)
{
    /* TODO should \t also be skipped here? */
    while (**input && **input == ' ') {
        (*input)++;
    }
}

static void free_cmdarg(ExArg *arg)
{
    if (arg->lhs) {
        g_string_free(arg->lhs, true);
    }
    if (arg->rhs) {
        g_string_free(arg->rhs, true);
    }
    g_free(arg);
}

static gboolean ex_bookmark(const ExArg *arg)
{
    if (arg->code == EX_BMR) {
        if (bookmark_remove(*arg->rhs->str ? arg->rhs->str : GET_URI())) {
            vb_echo_force(VB_MSG_NORMAL, false, "  Bookmark removed");

            return true;
        }
    } else if (bookmark_add(GET_URI(), webkit_web_view_get_title(vb.gui.webview), arg->rhs->str)) {
        vb_echo_force(VB_MSG_NORMAL, false, "  Bookmark added");

        return true;
    }

    return false;
}

static gboolean ex_eval(const ExArg *arg)
{
    gboolean success;
    char *value = NULL;

    if (!arg->rhs->len) {
        return false;
    }

    success = vb_eval_script(
        webkit_web_view_get_main_frame(vb.gui.webview), arg->rhs->str, NULL, &value
    );
    if (success) {
        vb_echo(VB_MSG_NORMAL, false, "%s", value);
    } else {
        vb_echo(VB_MSG_ERROR, true, "%s", value);
    }
    g_free(value);

    return success;
}

static gboolean ex_hardcopy(const ExArg *arg)
{
    webkit_web_frame_print(webkit_web_view_get_main_frame(vb.gui.webview));
    return true;
}

static gboolean ex_map(const ExArg *arg)
{
    if (!arg->lhs->len || !arg->rhs->len) {
        return false;
    }

    /* instead of using the EX_XMAP constants we use the first char of the
     * command name as mode and the second to determine if noremap is used */
    map_insert(arg->lhs->str, arg->rhs->str, arg->name[0], arg->name[1] != 'n');

    return true;;
}

static gboolean ex_unmap(const ExArg *arg)
{
    char *lhs;
    if (!arg->lhs->len) {
        return false;
    }

    lhs = arg->lhs->str;

    if (arg->code == EX_NUNMAP) {
        map_delete(lhs, 'n');
    } else if (arg->code == EX_CUNMAP) {
        map_delete(lhs, 'c');
    } else {
        map_delete(lhs, 'i');
    }
    return true;
}

static gboolean ex_normal(const ExArg *arg)
{
    mode_enter('n');

    /* if called with bang - don't apply mapping */
    map_handle_string(arg->lhs->str, !arg->bang);

    return true;
}

static gboolean ex_open(const ExArg *arg)
{
    if (arg->code == EX_TABOPEN) {
        return vb_load_uri(&((Arg){VB_TARGET_NEW, arg->rhs->str}));
    } else {
        return vb_load_uri(&((Arg){VB_TARGET_CURRENT, arg->rhs->str}));
    }
}

#ifdef FEATURE_QUEUE
static gboolean ex_queue(const ExArg *arg)
{
    Arg a = {0};

    switch (arg->code) {
        case EX_QPUSH:
            a.i = COMMAND_QUEUE_PUSH;
            break;

        case EX_QUNSHIFT:
            a.i = COMMAND_QUEUE_UNSHIFT;
            break;

        case EX_QPOP:
            a.i = COMMAND_QUEUE_POP;
            break;

        case EX_QCLEAR:
            a.i = COMMAND_QUEUE_CLEAR;
            break;

        default:
            return false;
    }

    /* if no argument is found in rhs, keep the uri in arg null to force
     * command_queue() to use current URI */
    if (arg->rhs->len) {
        a.s = arg->rhs->str;
    }

    return command_queue(&a);
}
#endif

static gboolean ex_quit(const ExArg *arg)
{
    vb_quit();
    return true;
}

static gboolean ex_save(const ExArg *arg)
{
    return command_save(&((Arg){COMMAND_SAVE_CURRENT, arg->rhs->str}));
}

static gboolean ex_set(const ExArg *arg)
{
    gboolean success;
    char *param = NULL;

    if (!arg->rhs->len) {
        return false;
    }

    /* split the input string into parameter and value part */
    if ((param = strchr(arg->rhs->str, '='))) {
        *param++ = '\0';
        success  = setting_run(arg->rhs->str, param ? param : NULL);
    } else {
        success = setting_run(arg->rhs->str, NULL);
    }

    return success;
}

static gboolean ex_shellcmd(const ExArg *arg)
{
    int status, argc;
    char *cmd, *error = NULL, *out = NULL, **argv;

    if (!*arg->rhs->str) {
        return false;
    }

    cmd = g_strdup_printf(SHELL_CMD, arg->rhs->str);
    if (!g_shell_parse_argv(cmd, &argc, &argv, NULL)) {
        vb_echo(VB_MSG_ERROR, true, "Could not parse command args");
        g_free(cmd);

        return false;
    }
    g_free(cmd);

    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &error, &status, NULL);
    g_strfreev(argv);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        vb_echo(VB_MSG_NORMAL, true, "%s", out);
        return true;
    }

    vb_echo(VB_MSG_ERROR, true, "[%d] %s", WEXITSTATUS(status), error);
    return false;
}

static gboolean ex_shortcut(const ExArg *arg)
{
    char *p;

    /* TODO allow to set shortcust with set command like ':set
     * shortcut[name]=http://donain.tld/?q=$0' */
    switch (arg->code) {
        case EX_SCA:
            if (arg->rhs->len && (p = strchr(arg->rhs->str, '='))) {
                *p++ = '\0';
                return shortcut_add(arg->rhs->str, p);
            }
            return false;

        case EX_SCR:
            return shortcut_remove(arg->rhs->str);

        case EX_SCD:
            return shortcut_set_default(arg->rhs->str);

        default:
            return false;
    }
}

/**
 * Manage the generation and stepping through completions.
 * This function prepared some prefix and suffix string that are required to
 * put hte matched data back to inputbox, and prepares the tree list store
 * model containing matched values.
 */
static gboolean complete(short direction)
{
    char *input;            /* input read from inputbox */
    const char *in;         /* pointer to input that we move */
    gboolean found = false;
    gboolean sort  = false;
    GtkListStore *store;

    /* if direction is 0 stop the completion */
    if (!direction) {
        completion_clean();

        return true;
    }

    input = vb_get_input_text();
    /* if completion was already started move to the next/prev item */
    if (vb.mode->flags & FLAG_COMPLETION) {
        if (excomp.current && !strcmp(input, excomp.current)) {
            /* step through the next/prev completion item */
            completion_next(direction < 0);
            g_free(input);

            return true;
        }

        /* if current input isn't the content of the completion item, stop
         * completion and start it after that again */
        completion_clean();
    }

    store = gtk_list_store_new(COMPLETION_STORE_NUM, G_TYPE_STRING, G_TYPE_STRING);

    in = (const char*)input;
    if (*in == ':') {
        const char *before_cmdname;
        /* skipt the first : */
        in++;

        ExArg *arg = g_new0(ExArg, 1);

        skip_whitespace(&in);
        parse_count(&in, arg);

        /* packup the current pointer so that we can restore the input pointer
         * if tha command name parsing fails */
        before_cmdname = in;

        if (parse_command_name(&in, arg) && *in == ' ') {
            OVERWRITE_NSTRING(excomp.prefix, input, in - input + 1);

            skip_whitespace(&in);
            switch (arg->code) {
                case EX_OPEN:
                case EX_TABOPEN:
                    if (*in == '!') {
                        found = bookmark_fill_completion(store, in + 1);
                    } else {
                        found = history_fill_completion(store, HISTORY_URL, in);
                    }
                    break;

                case EX_SET:
                    sort = true;
                    found = setting_fill_completion(store, in);
                    break;

                case EX_BMA:
                    sort  = true;
                    found = bookmark_fill_tag_completion(store, in);
                    break;

                default:
                    break;
            }
        } else { /* complete command names */
            /* restore the 'in' pointer after try to parse command name */
            in = before_cmdname;

            /* backup the parsed data so we can access them in
             * completion_select function */
            excomp.count = arg->count;

            if (ex_fill_completion(store, in)) {
                OVERWRITE_STRING(excomp.prefix, ":");
                found = true;
            }
        }
        free_cmdarg(arg);
    } else if (*in == '/' || *in == '?') {
        if (history_fill_completion(store, HISTORY_SEARCH, in + 1)) {
            OVERWRITE_NSTRING(excomp.prefix, in, 1);
            sort  = true;
            found = true;
        }
    }

    /* if the input could be parsed and the tree view could be filled */
    if (sort) {
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(store), COMPLETION_STORE_FIRST, GTK_SORT_ASCENDING
        );
    }

    if (found) {
        completion_create(GTK_TREE_MODEL(store), completion_select, direction < 0);
    }

    g_free(input);
    return true;
}

/**
 * Callback called from the completion if a item is selected to write the
 * matche item accordings with previously saved prefix and command name to the
 * inputbox.
 */
static void completion_select(char *match)
{
    OVERWRITE_STRING(excomp.current, NULL);

    if (excomp.count) {
        excomp.current = g_strdup_printf("%s%d%s", excomp.prefix, excomp.count, match);
    } else {
        excomp.current = g_strconcat(excomp.prefix, match, NULL);
    }
    vb_set_input_text(excomp.current);
}

static gboolean history(gboolean prev)
{
    char *input;
    GList *new = NULL;

    input = vb_get_input_text();
    if (exhist.active) {
        /* calculate the actual content of the inpubox from history data, if
         * the theoretical content and the actual given input are different
         * rewind the history to recreate it later new */
        char *current = g_strconcat(exhist.prefix, (char*)exhist.active->data, NULL);
        if (strcmp(input, current)) {
            history_rewind();
        }
        g_free(current);
    }

    /* create the history list if the lookup is started or input was changed */
    if (!exhist.active) {
        int type;
        char prefix[2] = {0};
        const char *in = (const char*)input;

        skip_whitespace(&in);

        /* save the first char as prefix - this should be : / or ? */
        prefix[0] = *in;

        /* check which type of history we should use */
        if (*in == ':') {
            type = VB_INPUT_COMMAND;
        } else if (*in == '/' || *in == '?') {
            /* the history does not distinguish between forward and backward
             * search, so we don't need the backward search here too */
            type = VB_INPUT_SEARCH_FORWARD;
        } else {
            goto failed;
        }

        exhist.active = history_get_list(type, in + 1);
        if (!exhist.active) {
            goto failed;
        }
        OVERWRITE_STRING(exhist.prefix, prefix);
    }

    if (prev) {
        if ((new = g_list_next(exhist.active))) {
            exhist.active = new;
        }
    } else if ((new = g_list_previous(exhist.active))) {
        exhist.active = new;
    }

    vb_echo_force(VB_MSG_NORMAL, false, "%s%s", exhist.prefix, (char*)exhist.active->data);

    g_free(input);
    return true;

failed:
    g_free(input);
    return false;
}

static void history_rewind(void)
{
    if (exhist.active) {
        /* free temporary used history list */
        g_list_free_full(exhist.active, (GDestroyNotify)g_free);
        exhist.active = NULL;

        OVERWRITE_STRING(exhist.prefix, NULL);
    }
}
