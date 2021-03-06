/*
 * gtkwin.c: the main code that runs a PuTTY terminal emulator and
 * backend in a GTK window.
 */

#define _GNU_SOURCE

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define PUTTY_DO_GLOBALS	       /* actually _define_ globals */

#include "putty.h"
#include "terminal.h"

#define CAT2(x,y) x ## y
#define CAT(x,y) CAT2(x,y)
#define ASSERT(x) enum {CAT(assertion_,__LINE__) = 1 / (x)}

/* Colours come in two flavours: configurable, and xterm-extended. */
#define NCFGCOLOURS (lenof(((Config *)0)->colours))
#define NEXTCOLOURS 240 /* 216 colour-cube plus 24 shades of grey */
#define NALLCOLOURS (NCFGCOLOURS + NEXTCOLOURS)

GdkAtom compound_text_atom, utf8_string_atom;

extern char **pty_argv;	       /* declared in pty.c */
extern int use_pty_argv;

/*
 * Timers are global across all sessions (even if we were handling
 * multiple sessions, which we aren't), so the current timer ID is
 * a global variable.
 */
static guint timer_id = 0;

struct gui_data {
    GtkWidget *window, *area, *sbar;
    GtkBox *hbox;
    GtkAdjustment *sbar_adjust;
    GtkWidget *menu, *specialsmenu, *specialsitem1, *specialsitem2,
	*restartitem;
    GtkWidget *sessionsmenu;
    GdkPixmap *pixmap;
    GdkFont *fonts[4];                 /* normal, bold, wide, widebold */
    struct {
	int charset;
	int is_wide;
    } fontinfo[4];
    int xpos, ypos, gotpos, gravity;
    GdkCursor *rawcursor, *textcursor, *blankcursor, *waitcursor, *currcursor;
    GdkColor cols[NALLCOLOURS];
    GdkColormap *colmap;
    wchar_t *pastein_data;
    int direct_to_font;
    int pastein_data_len;
    char *pasteout_data, *pasteout_data_ctext, *pasteout_data_utf8;
    int pasteout_data_len, pasteout_data_ctext_len, pasteout_data_utf8_len;
    int font_width, font_height;
    int width, height;
    int ignore_sbar;
    int mouseptr_visible;
    int busy_status;
    guint term_paste_idle_id;
    int alt_keycode;
    int alt_digits;
    char wintitle[sizeof(((Config *)0)->wintitle)];
    char icontitle[sizeof(((Config *)0)->wintitle)];
    int master_fd, master_func_id;
    void *ldisc;
    Backend *back;
    void *backhandle;
    Terminal *term;
    void *logctx;
    int exited;
    struct unicode_data ucsdata;
    Config cfg;
    void *eventlogstuff;
    char *progname, **gtkargvstart;
    int ngtkargs;
    guint32 input_event_time; /* Timestamp of the most recent input event. */
    int reconfiguring;
};

struct draw_ctx {
    GdkGC *gc;
    struct gui_data *inst;
};

static int send_raw_mouse;

static char *app_name = "pterm";

static void start_backend(struct gui_data *inst);

char *x_get_default(const char *key)
{
    return XGetDefault(GDK_DISPLAY(), app_name, key);
}

void connection_fatal(void *frontend, char *p, ...)
{
    struct gui_data *inst = (struct gui_data *)frontend;

    va_list ap;
    char *msg;
    va_start(ap, p);
    msg = dupvprintf(p, ap);
    va_end(ap);
    inst->exited = TRUE;
    fatal_message_box(inst->window, msg);
    sfree(msg);
    if (inst->cfg.close_on_exit == FORCE_ON)
        cleanup_exit(1);
}

/*
 * Default settings that are specific to pterm.
 */
FontSpec platform_default_fontspec(const char *name)
{
    FontSpec ret;
    if (!strcmp(name, "Font"))
	strcpy(ret.name, "fixed");
    else
	*ret.name = '\0';
    return ret;
}

Filename platform_default_filename(const char *name)
{
    Filename ret;
    if (!strcmp(name, "LogFileName"))
	strcpy(ret.path, "putty.log");
    else
	*ret.path = '\0';
    return ret;
}

char *platform_default_s(const char *name)
{
    if (!strcmp(name, "SerialLine"))
	return dupstr("/dev/ttyS0");
    return NULL;
}

int platform_default_i(const char *name, int def)
{
    if (!strcmp(name, "CloseOnExit"))
	return 2;  /* maps to FORCE_ON after painful rearrangement :-( */
    if (!strcmp(name, "WinNameAlways"))
	return 0;  /* X natively supports icon titles, so use 'em by default */
    return def;
}

void ldisc_update(void *frontend, int echo, int edit)
{
    /*
     * This is a stub in pterm. If I ever produce a Unix
     * command-line ssh/telnet/rlogin client (i.e. a port of plink)
     * then it will require some termios manoeuvring analogous to
     * that in the Windows plink.c, but here it's meaningless.
     */
}

char *get_ttymode(void *frontend, const char *mode)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return term_get_ttymode(inst->term, mode);
}

int from_backend(void *frontend, int is_stderr, const char *data, int len)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return term_data(inst->term, is_stderr, data, len);
}

int from_backend_untrusted(void *frontend, const char *data, int len)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return term_data_untrusted(inst->term, data, len);
}

int get_userpass_input(prompts_t *p, unsigned char *in, int inlen)
{
    struct gui_data *inst = (struct gui_data *)p->frontend;
    int ret;
    ret = cmdline_get_passwd_input(p, in, inlen);
    if (ret == -1)
	ret = term_get_userpass_input(inst->term, p, in, inlen);
    return ret;
}

void logevent(void *frontend, const char *string)
{
    struct gui_data *inst = (struct gui_data *)frontend;

    log_eventlog(inst->logctx, string);

    logevent_dlg(inst->eventlogstuff, string);
}

int font_dimension(void *frontend, int which)/* 0 for width, 1 for height */
{
    struct gui_data *inst = (struct gui_data *)frontend;

    if (which)
	return inst->font_height;
    else
	return inst->font_width;
}

/*
 * Translate a raw mouse button designation (LEFT, MIDDLE, RIGHT)
 * into a cooked one (SELECT, EXTEND, PASTE).
 * 
 * In Unix, this is not configurable; the X button arrangement is
 * rock-solid across all applications, everyone has a three-button
 * mouse or a means of faking it, and there is no need to switch
 * buttons around at all.
 */
static Mouse_Button translate_button(Mouse_Button button)
{
    /* struct gui_data *inst = (struct gui_data *)frontend; */

    if (button == MBT_LEFT)
	return MBT_SELECT;
    if (button == MBT_MIDDLE)
	return MBT_PASTE;
    if (button == MBT_RIGHT)
	return MBT_EXTEND;
    return 0;			       /* shouldn't happen */
}

/*
 * Return the top-level GtkWindow associated with a particular
 * front end instance.
 */
void *get_window(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return inst->window;
}

/*
 * Minimise or restore the window in response to a server-side
 * request.
 */
void set_iconic(void *frontend, int iconic)
{
    /*
     * GTK 1.2 doesn't know how to do this.
     */
#if GTK_CHECK_VERSION(2,0,0)
    struct gui_data *inst = (struct gui_data *)frontend;
    if (iconic)
	gtk_window_iconify(GTK_WINDOW(inst->window));
    else
	gtk_window_deiconify(GTK_WINDOW(inst->window));
#endif
}

/*
 * Move the window in response to a server-side request.
 */
void move_window(void *frontend, int x, int y)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    /*
     * I assume that when the GTK version of this call is available
     * we should use it. Not sure how it differs from the GDK one,
     * though.
     */
#if GTK_CHECK_VERSION(2,0,0)
    gtk_window_move(GTK_WINDOW(inst->window), x, y);
#else
    gdk_window_move(inst->window->window, x, y);
#endif
}

/*
 * Move the window to the top or bottom of the z-order in response
 * to a server-side request.
 */
void set_zorder(void *frontend, int top)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    if (top)
	gdk_window_raise(inst->window->window);
    else
	gdk_window_lower(inst->window->window);
}

/*
 * Refresh the window in response to a server-side request.
 */
void refresh_window(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    term_invalidate(inst->term);
}

/*
 * Maximise or restore the window in response to a server-side
 * request.
 */
void set_zoomed(void *frontend, int zoomed)
{
    /*
     * GTK 1.2 doesn't know how to do this.
     */
#if GTK_CHECK_VERSION(2,0,0)
    struct gui_data *inst = (struct gui_data *)frontend;
    if (iconic)
	gtk_window_maximize(GTK_WINDOW(inst->window));
    else
	gtk_window_unmaximize(GTK_WINDOW(inst->window));
#endif
}

/*
 * Report whether the window is iconic, for terminal reports.
 */
int is_iconic(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return !gdk_window_is_viewable(inst->window->window);
}

/*
 * Report the window's position, for terminal reports.
 */
void get_window_pos(void *frontend, int *x, int *y)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    /*
     * I assume that when the GTK version of this call is available
     * we should use it. Not sure how it differs from the GDK one,
     * though.
     */
#if GTK_CHECK_VERSION(2,0,0)
    gtk_window_get_position(GTK_WINDOW(inst->window), x, y);
#else
    gdk_window_get_position(inst->window->window, x, y);
#endif
}

/*
 * Report the window's pixel size, for terminal reports.
 */
void get_window_pixels(void *frontend, int *x, int *y)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    /*
     * I assume that when the GTK version of this call is available
     * we should use it. Not sure how it differs from the GDK one,
     * though.
     */
#if GTK_CHECK_VERSION(2,0,0)
    gtk_window_get_size(GTK_WINDOW(inst->window), x, y);
#else
    gdk_window_get_size(inst->window->window, x, y);
#endif
}

/*
 * Return the window or icon title.
 */
char *get_window_title(void *frontend, int icon)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return icon ? inst->icontitle : inst->wintitle;
}

gint delete_window(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    if (!inst->exited && inst->cfg.warn_on_close) {
	if (!reallyclose(inst))
	    return TRUE;
    }
    return FALSE;
}

static void update_mouseptr(struct gui_data *inst)
{
    switch (inst->busy_status) {
      case BUSY_NOT:
	if (!inst->mouseptr_visible) {
	    gdk_window_set_cursor(inst->area->window, inst->blankcursor);
	} else if (send_raw_mouse) {
	    gdk_window_set_cursor(inst->area->window, inst->rawcursor);
	} else {
	    gdk_window_set_cursor(inst->area->window, inst->textcursor);
	}
	break;
      case BUSY_WAITING:    /* XXX can we do better? */
      case BUSY_CPU:
	/* We always display these cursors. */
	gdk_window_set_cursor(inst->area->window, inst->waitcursor);
	break;
      default:
	assert(0);
    }
}

static void show_mouseptr(struct gui_data *inst, int show)
{
    if (!inst->cfg.hide_mouseptr)
	show = 1;
    inst->mouseptr_visible = show;
    update_mouseptr(inst);
}

void draw_backing_rect(struct gui_data *inst)
{
    GdkGC *gc = gdk_gc_new(inst->area->window);
    gdk_gc_set_foreground(gc, &inst->cols[258]);    /* default background */
    gdk_draw_rectangle(inst->pixmap, gc, 1, 0, 0,
		       inst->cfg.width * inst->font_width + 2*inst->cfg.window_border,
		       inst->cfg.height * inst->font_height + 2*inst->cfg.window_border);
    gdk_gc_unref(gc);
}

gint configure_area(GtkWidget *widget, GdkEventConfigure *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    int w, h, need_size = 0;

    /*
     * See if the terminal size has changed, in which case we must
     * let the terminal know.
     */
    w = (event->width - 2*inst->cfg.window_border) / inst->font_width;
    h = (event->height - 2*inst->cfg.window_border) / inst->font_height;
    if (w != inst->width || h != inst->height) {
	inst->cfg.width = inst->width = w;
	inst->cfg.height = inst->height = h;
	need_size = 1;
    }

    if (inst->pixmap) {
	gdk_pixmap_unref(inst->pixmap);
	inst->pixmap = NULL;
    }

    inst->pixmap = gdk_pixmap_new(widget->window,
				  (inst->cfg.width * inst->font_width +
				   2*inst->cfg.window_border),
				  (inst->cfg.height * inst->font_height +
				   2*inst->cfg.window_border), -1);

    draw_backing_rect(inst);

    if (need_size && inst->term) {
	term_size(inst->term, h, w, inst->cfg.savelines);
    }

    if (inst->term)
	term_invalidate(inst->term);

    return TRUE;
}

gint expose_area(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;

    /*
     * Pass the exposed rectangle to terminal.c, which will call us
     * back to do the actual painting.
     */
    if (inst->pixmap) {
	gdk_draw_pixmap(widget->window,
			widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
			inst->pixmap,
			event->area.x, event->area.y,
			event->area.x, event->area.y,
			event->area.width, event->area.height);
    }
    return TRUE;
}

#define KEY_PRESSED(k) \
    (inst->keystate[(k) / 32] & (1 << ((k) % 32)))

gint key_event(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    char output[32];
    wchar_t ucsoutput[2];
    int ucsval, start, end, special, use_ucsoutput;

    /* Remember the timestamp. */
    inst->input_event_time = event->time;

    /* By default, nothing is generated. */
    end = start = 0;
    special = use_ucsoutput = FALSE;

    /*
     * If Alt is being released after typing an Alt+numberpad
     * sequence, we should generate the code that was typed.
     * 
     * Note that we only do this if more than one key was actually
     * pressed - I don't think Alt+NumPad4 should be ^D or that
     * Alt+NumPad3 should be ^C, for example. There's no serious
     * inconvenience in having to type a zero before a single-digit
     * character code.
     */
    if (event->type == GDK_KEY_RELEASE &&
	(event->keyval == GDK_Meta_L || event->keyval == GDK_Alt_L ||
	 event->keyval == GDK_Meta_R || event->keyval == GDK_Alt_R) &&
	inst->alt_keycode >= 0 && inst->alt_digits > 1) {
#ifdef KEY_DEBUGGING
	printf("Alt key up, keycode = %d\n", inst->alt_keycode);
#endif
	output[0] = inst->alt_keycode;
	end = 1;
	goto done;
    }

    if (event->type == GDK_KEY_PRESS) {
#ifdef KEY_DEBUGGING
	{
	    int i;
	    printf("keypress: keyval = %04x, state = %08x; string =",
		   event->keyval, event->state);
	    for (i = 0; event->string[i]; i++)
		printf(" %02x", (unsigned char) event->string[i]);
	    printf("\n");
	}
#endif

	/*
	 * NYI: Compose key (!!! requires Unicode faff before even trying)
	 */

	/*
	 * If Alt has just been pressed, we start potentially
	 * accumulating an Alt+numberpad code. We do this by
	 * setting alt_keycode to -1 (nothing yet but plausible).
	 */
	if ((event->keyval == GDK_Meta_L || event->keyval == GDK_Alt_L ||
	     event->keyval == GDK_Meta_R || event->keyval == GDK_Alt_R)) {
	    inst->alt_keycode = -1;
            inst->alt_digits = 0;
	    goto done;		       /* this generates nothing else */
	}

	/*
	 * If we're seeing a numberpad key press with Mod1 down,
	 * consider adding it to alt_keycode if that's sensible.
	 * Anything _else_ with Mod1 down cancels any possibility
	 * of an ALT keycode: we set alt_keycode to -2.
	 */
	if ((event->state & GDK_MOD1_MASK) && inst->alt_keycode != -2) {
	    int digit = -1;
	    switch (event->keyval) {
	      case GDK_KP_0: case GDK_KP_Insert: digit = 0; break;
	      case GDK_KP_1: case GDK_KP_End: digit = 1; break;
	      case GDK_KP_2: case GDK_KP_Down: digit = 2; break;
	      case GDK_KP_3: case GDK_KP_Page_Down: digit = 3; break;
	      case GDK_KP_4: case GDK_KP_Left: digit = 4; break;
	      case GDK_KP_5: case GDK_KP_Begin: digit = 5; break;
	      case GDK_KP_6: case GDK_KP_Right: digit = 6; break;
	      case GDK_KP_7: case GDK_KP_Home: digit = 7; break;
	      case GDK_KP_8: case GDK_KP_Up: digit = 8; break;
	      case GDK_KP_9: case GDK_KP_Page_Up: digit = 9; break;
	    }
	    if (digit < 0)
		inst->alt_keycode = -2;   /* it's invalid */
	    else {
#ifdef KEY_DEBUGGING
		printf("Adding digit %d to keycode %d", digit,
		       inst->alt_keycode);
#endif
		if (inst->alt_keycode == -1)
		    inst->alt_keycode = digit;   /* one-digit code */
		else
		    inst->alt_keycode = inst->alt_keycode * 10 + digit;
                inst->alt_digits++;
#ifdef KEY_DEBUGGING
		printf(" gives new code %d\n", inst->alt_keycode);
#endif
		/* Having used this digit, we now do nothing more with it. */
		goto done;
	    }
	}

	/*
	 * Shift-PgUp and Shift-PgDn don't even generate keystrokes
	 * at all.
	 */
	if (event->keyval == GDK_Page_Up && (event->state & GDK_SHIFT_MASK)) {
	    term_scroll(inst->term, 0, -inst->cfg.height/2);
	    return TRUE;
	}
	if (event->keyval == GDK_Page_Up && (event->state & GDK_CONTROL_MASK)) {
	    term_scroll(inst->term, 0, -1);
	    return TRUE;
	}
	if (event->keyval == GDK_Page_Down && (event->state & GDK_SHIFT_MASK)) {
	    term_scroll(inst->term, 0, +inst->cfg.height/2);
	    return TRUE;
	}
	if (event->keyval == GDK_Page_Down && (event->state & GDK_CONTROL_MASK)) {
	    term_scroll(inst->term, 0, +1);
	    return TRUE;
	}

	/*
	 * Neither does Shift-Ins.
	 */
	if (event->keyval == GDK_Insert && (event->state & GDK_SHIFT_MASK)) {
	    request_paste(inst);
	    return TRUE;
	}

	special = FALSE;
	use_ucsoutput = FALSE;

	/* ALT+things gives leading Escape. */
	output[0] = '\033';
	strncpy(output+1, event->string, 31);
	if (!*event->string &&
	    (ucsval = keysym_to_unicode(event->keyval)) >= 0) {
	    ucsoutput[0] = '\033';
	    ucsoutput[1] = ucsval;
	    use_ucsoutput = TRUE;
	    end = 2;
	} else {
	    output[31] = '\0';
	    end = strlen(output);
	}
	if (event->state & GDK_MOD1_MASK) {
	    start = 0;
	    if (end == 1) end = 0;
	} else
	    start = 1;

	/* Control-` is the same as Control-\ (unless gtk has a better idea) */
	if (!event->string[0] && event->keyval == '`' &&
	    (event->state & GDK_CONTROL_MASK)) {
	    output[1] = '\x1C';
	    use_ucsoutput = FALSE;
	    end = 2;
	}

	/* Control-Break sends a Break special to the backend */
	if (event->keyval == GDK_Break &&
	    (event->state & GDK_CONTROL_MASK)) {
	    if (inst->back)
		inst->back->special(inst->backhandle, TS_BRK);
	    return TRUE;
	}

	/* We handle Return ourselves, because it needs to be flagged as
	 * special to ldisc. */
	if (event->keyval == GDK_Return) {
	    output[1] = '\015';
	    use_ucsoutput = FALSE;
	    end = 2;
	    special = TRUE;
	}

	/* Control-2, Control-Space and Control-@ are NUL */
	if (!event->string[0] &&
	    (event->keyval == ' ' || event->keyval == '2' ||
	     event->keyval == '@') &&
	    (event->state & (GDK_SHIFT_MASK |
			     GDK_CONTROL_MASK)) == GDK_CONTROL_MASK) {
	    output[1] = '\0';
	    use_ucsoutput = FALSE;
	    end = 2;
	}

	/* Control-Shift-Space is 160 (ISO8859 nonbreaking space) */
	if (!event->string[0] && event->keyval == ' ' &&
	    (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) ==
	    (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) {
	    output[1] = '\240';
	    use_ucsoutput = FALSE;
	    end = 2;
	}

	/* We don't let GTK tell us what Backspace is! We know better. */
	if (event->keyval == GDK_BackSpace &&
	    !(event->state & GDK_SHIFT_MASK)) {
	    output[1] = inst->cfg.bksp_is_delete ? '\x7F' : '\x08';
	    use_ucsoutput = FALSE;
	    end = 2;
	    special = TRUE;
	}
	/* For Shift Backspace, do opposite of what is configured. */
	if (event->keyval == GDK_BackSpace &&
	    (event->state & GDK_SHIFT_MASK)) {
	    output[1] = inst->cfg.bksp_is_delete ? '\x08' : '\x7F';
	    use_ucsoutput = FALSE;
	    end = 2;
	    special = TRUE;
	}

	/* Shift-Tab is ESC [ Z */
	if (event->keyval == GDK_ISO_Left_Tab ||
	    (event->keyval == GDK_Tab && (event->state & GDK_SHIFT_MASK))) {
	    end = 1 + sprintf(output+1, "\033[Z");
	    use_ucsoutput = FALSE;
	}

	/*
	 * NetHack keypad mode.
	 */
	if (inst->cfg.nethack_keypad) {
	    char *keys = NULL;
	    switch (event->keyval) {
	      case GDK_KP_1: case GDK_KP_End: keys = "bB\002"; break;
	      case GDK_KP_2: case GDK_KP_Down: keys = "jJ\012"; break;
	      case GDK_KP_3: case GDK_KP_Page_Down: keys = "nN\016"; break;
	      case GDK_KP_4: case GDK_KP_Left: keys = "hH\010"; break;
	      case GDK_KP_5: case GDK_KP_Begin: keys = "..."; break;
	      case GDK_KP_6: case GDK_KP_Right: keys = "lL\014"; break;
	      case GDK_KP_7: case GDK_KP_Home: keys = "yY\031"; break;
	      case GDK_KP_8: case GDK_KP_Up: keys = "kK\013"; break;
	      case GDK_KP_9: case GDK_KP_Page_Up: keys = "uU\025"; break;
	    }
	    if (keys) {
		end = 2;
		if (event->state & GDK_CONTROL_MASK)
		    output[1] = keys[2];
		else if (event->state & GDK_SHIFT_MASK)
		    output[1] = keys[1];
		else
		    output[1] = keys[0];
		use_ucsoutput = FALSE;
		goto done;
	    }
	}

	/*
	 * Application keypad mode.
	 */
	if (inst->term->app_keypad_keys && !inst->cfg.no_applic_k) {
	    int xkey = 0;
	    switch (event->keyval) {
	      case GDK_Num_Lock: xkey = 'P'; break;
	      case GDK_KP_Divide: xkey = 'Q'; break;
	      case GDK_KP_Multiply: xkey = 'R'; break;
	      case GDK_KP_Subtract: xkey = 'S'; break;
		/*
		 * Keypad + is tricky. It covers a space that would
		 * be taken up on the VT100 by _two_ keys; so we
		 * let Shift select between the two. Worse still,
		 * in xterm function key mode we change which two...
		 */
	      case GDK_KP_Add:
		if (inst->cfg.funky_type == FUNKY_XTERM) {
		    if (event->state & GDK_SHIFT_MASK)
			xkey = 'l';
		    else
			xkey = 'k';
		} else if (event->state & GDK_SHIFT_MASK)
			xkey = 'm';
		else
		    xkey = 'l';
		break;
	      case GDK_KP_Enter: xkey = 'M'; break;
	      case GDK_KP_0: case GDK_KP_Insert: xkey = 'p'; break;
	      case GDK_KP_1: case GDK_KP_End: xkey = 'q'; break;
	      case GDK_KP_2: case GDK_KP_Down: xkey = 'r'; break;
	      case GDK_KP_3: case GDK_KP_Page_Down: xkey = 's'; break;
	      case GDK_KP_4: case GDK_KP_Left: xkey = 't'; break;
	      case GDK_KP_5: case GDK_KP_Begin: xkey = 'u'; break;
	      case GDK_KP_6: case GDK_KP_Right: xkey = 'v'; break;
	      case GDK_KP_7: case GDK_KP_Home: xkey = 'w'; break;
	      case GDK_KP_8: case GDK_KP_Up: xkey = 'x'; break;
	      case GDK_KP_9: case GDK_KP_Page_Up: xkey = 'y'; break;
	      case GDK_KP_Decimal: case GDK_KP_Delete: xkey = 'n'; break;
	    }
	    if (xkey) {
		if (inst->term->vt52_mode) {
		    if (xkey >= 'P' && xkey <= 'S')
			end = 1 + sprintf(output+1, "\033%c", xkey);
		    else
			end = 1 + sprintf(output+1, "\033?%c", xkey);
		} else
		    end = 1 + sprintf(output+1, "\033O%c", xkey);
		use_ucsoutput = FALSE;
		goto done;
	    }
	}

	/*
	 * Next, all the keys that do tilde codes. (ESC '[' nn '~',
	 * for integer decimal nn.)
	 *
	 * We also deal with the weird ones here. Linux VCs replace F1
	 * to F5 by ESC [ [ A to ESC [ [ E. rxvt doesn't do _that_, but
	 * does replace Home and End (1~ and 4~) by ESC [ H and ESC O w
	 * respectively.
	 */
	{
	    int code = 0;
	    switch (event->keyval) {
	      case GDK_F1:
		code = (event->state & GDK_SHIFT_MASK ? 23 : 11);
		break;
	      case GDK_F2:
		code = (event->state & GDK_SHIFT_MASK ? 24 : 12);
		break;
	      case GDK_F3:
		code = (event->state & GDK_SHIFT_MASK ? 25 : 13);
		break;
	      case GDK_F4:
		code = (event->state & GDK_SHIFT_MASK ? 26 : 14);
		break;
	      case GDK_F5:
		code = (event->state & GDK_SHIFT_MASK ? 28 : 15);
		break;
	      case GDK_F6:
		code = (event->state & GDK_SHIFT_MASK ? 29 : 17);
		break;
	      case GDK_F7:
		code = (event->state & GDK_SHIFT_MASK ? 31 : 18);
		break;
	      case GDK_F8:
		code = (event->state & GDK_SHIFT_MASK ? 32 : 19);
		break;
	      case GDK_F9:
		code = (event->state & GDK_SHIFT_MASK ? 33 : 20);
		break;
	      case GDK_F10:
		code = (event->state & GDK_SHIFT_MASK ? 34 : 21);
		break;
	      case GDK_F11:
		code = 23;
		break;
	      case GDK_F12:
		code = 24;
		break;
	      case GDK_F13:
		code = 25;
		break;
	      case GDK_F14:
		code = 26;
		break;
	      case GDK_F15:
		code = 28;
		break;
	      case GDK_F16:
		code = 29;
		break;
	      case GDK_F17:
		code = 31;
		break;
	      case GDK_F18:
		code = 32;
		break;
	      case GDK_F19:
		code = 33;
		break;
	      case GDK_F20:
		code = 34;
		break;
	    }
	    if (!(event->state & GDK_CONTROL_MASK)) switch (event->keyval) {
	      case GDK_Home: case GDK_KP_Home:
		code = 1;
		break;
	      case GDK_Insert: case GDK_KP_Insert:
		code = 2;
		break;
	      case GDK_Delete: case GDK_KP_Delete:
		code = 3;
		break;
	      case GDK_End: case GDK_KP_End:
		code = 4;
		break;
	      case GDK_Page_Up: case GDK_KP_Page_Up:
		code = 5;
		break;
	      case GDK_Page_Down: case GDK_KP_Page_Down:
		code = 6;
		break;
	    }
	    /* Reorder edit keys to physical order */
	    if (inst->cfg.funky_type == FUNKY_VT400 && code <= 6)
		code = "\0\2\1\4\5\3\6"[code];

	    if (inst->term->vt52_mode && code > 0 && code <= 6) {
		end = 1 + sprintf(output+1, "\x1B%c", " HLMEIG"[code]);
		use_ucsoutput = FALSE;
		goto done;
	    }

	    if (inst->cfg.funky_type == FUNKY_SCO &&     /* SCO function keys */
		code >= 11 && code <= 34) {
		char codes[] = "MNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@[\\]^_`{";
		int index = 0;
		switch (event->keyval) {
		  case GDK_F1: index = 0; break;
		  case GDK_F2: index = 1; break;
		  case GDK_F3: index = 2; break;
		  case GDK_F4: index = 3; break;
		  case GDK_F5: index = 4; break;
		  case GDK_F6: index = 5; break;
		  case GDK_F7: index = 6; break;
		  case GDK_F8: index = 7; break;
		  case GDK_F9: index = 8; break;
		  case GDK_F10: index = 9; break;
		  case GDK_F11: index = 10; break;
		  case GDK_F12: index = 11; break;
		}
		if (event->state & GDK_SHIFT_MASK) index += 12;
		if (event->state & GDK_CONTROL_MASK) index += 24;
		end = 1 + sprintf(output+1, "\x1B[%c", codes[index]);
		use_ucsoutput = FALSE;
		goto done;
	    }
	    if (inst->cfg.funky_type == FUNKY_SCO &&     /* SCO small keypad */
		code >= 1 && code <= 6) {
		char codes[] = "HL.FIG";
		if (code == 3) {
		    output[1] = '\x7F';
		    end = 2;
		} else {
		    end = 1 + sprintf(output+1, "\x1B[%c", codes[code-1]);
		}
		use_ucsoutput = FALSE;
		goto done;
	    }
	    if ((inst->term->vt52_mode || inst->cfg.funky_type == FUNKY_VT100P) &&
		code >= 11 && code <= 24) {
		int offt = 0;
		if (code > 15)
		    offt++;
		if (code > 21)
		    offt++;
		if (inst->term->vt52_mode)
		    end = 1 + sprintf(output+1,
				      "\x1B%c", code + 'P' - 11 - offt);
		else
		    end = 1 + sprintf(output+1,
				      "\x1BO%c", code + 'P' - 11 - offt);
		use_ucsoutput = FALSE;
		goto done;
	    }
	    if (inst->cfg.funky_type == FUNKY_LINUX && code >= 11 && code <= 15) {
		end = 1 + sprintf(output+1, "\x1B[[%c", code + 'A' - 11);
		use_ucsoutput = FALSE;
		goto done;
	    }
	    if (inst->cfg.funky_type == FUNKY_XTERM && code >= 11 && code <= 14) {
		if (inst->term->vt52_mode)
		    end = 1 + sprintf(output+1, "\x1B%c", code + 'P' - 11);
		else
		    end = 1 + sprintf(output+1, "\x1BO%c", code + 'P' - 11);
		use_ucsoutput = FALSE;
		goto done;
	    }
	    if (inst->cfg.rxvt_homeend && (code == 1 || code == 4)) {
		end = 1 + sprintf(output+1, code == 1 ? "\x1B[H" : "\x1BOw");
		use_ucsoutput = FALSE;
		goto done;
	    }
	    if (code) {
		end = 1 + sprintf(output+1, "\x1B[%d~", code);
		use_ucsoutput = FALSE;
		goto done;
	    }
	}

	/*
	 * Cursor keys. (This includes the numberpad cursor keys,
	 * if we haven't already done them due to app keypad mode.)
	 * 
	 * Here we also process un-numlocked un-appkeypadded KP5,
	 * which sends ESC [ G.
	 */
	{
	    int xkey = 0;
	    switch (event->keyval) {
	      case GDK_Up: case GDK_KP_Up: xkey = 'A'; break;
	      case GDK_Down: case GDK_KP_Down: xkey = 'B'; break;
	      case GDK_Right: case GDK_KP_Right: xkey = 'C'; break;
	      case GDK_Left: case GDK_KP_Left: xkey = 'D'; break;
	      case GDK_Begin: case GDK_KP_Begin: xkey = 'G'; break;
	    }
	    if (xkey) {
		/*
		 * The arrow keys normally do ESC [ A and so on. In
		 * app cursor keys mode they do ESC O A instead.
		 * Ctrl toggles the two modes.
		 */
		if (inst->term->vt52_mode) {
		    end = 1 + sprintf(output+1, "\033%c", xkey);
		} else if (!inst->term->app_cursor_keys ^
			   !(event->state & GDK_CONTROL_MASK)) {
		    end = 1 + sprintf(output+1, "\033O%c", xkey);
		} else {		    
		    end = 1 + sprintf(output+1, "\033[%c", xkey);
		}
		use_ucsoutput = FALSE;
		goto done;
	    }
	}
	goto done;
    }

    done:

    if (end-start > 0) {
#ifdef KEY_DEBUGGING
	int i;
	printf("generating sequence:");
	for (i = start; i < end; i++)
	    printf(" %02x", (unsigned char) output[i]);
	printf("\n");
#endif

	if (special) {
	    /*
	     * For special control characters, the character set
	     * should never matter.
	     */
	    output[end] = '\0';	       /* NUL-terminate */
	    if (inst->ldisc)
		ldisc_send(inst->ldisc, output+start, -2, 1);
	} else if (!inst->direct_to_font) {
	    if (!use_ucsoutput) {
		/*
		 * The stuff we've just generated is assumed to be
		 * ISO-8859-1! This sounds insane, but `man
		 * XLookupString' agrees: strings of this type
		 * returned from the X server are hardcoded to
		 * 8859-1. Strictly speaking we should be doing
		 * this using some sort of GtkIMContext, which (if
		 * we're lucky) would give us our data directly in
		 * Unicode; but that's not supported in GTK 1.2 as
		 * far as I can tell, and it's poorly documented
		 * even in 2.0, so it'll have to wait.
		 */
		if (inst->ldisc)
		    lpage_send(inst->ldisc, CS_ISO8859_1, output+start,
			       end-start, 1);
	    } else {
		/*
		 * We generated our own Unicode key data from the
		 * keysym, so use that instead.
		 */
		if (inst->ldisc)
		    luni_send(inst->ldisc, ucsoutput+start, end-start, 1);
	    }
	} else {
	    /*
	     * In direct-to-font mode, we just send the string
	     * exactly as we received it.
	     */
	    if (inst->ldisc)
		ldisc_send(inst->ldisc, output+start, end-start, 1);
	}

	show_mouseptr(inst, 0);
	term_seen_key_event(inst->term);
    }

    return TRUE;
}

gint button_event(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    int shift, ctrl, alt, x, y, button, act;

    /* Remember the timestamp. */
    inst->input_event_time = event->time;

    show_mouseptr(inst, 1);

    if (event->button == 4 && event->type == GDK_BUTTON_PRESS) {
	term_scroll(inst->term, 0, -5);
	return TRUE;
    }
    if (event->button == 5 && event->type == GDK_BUTTON_PRESS) {
	term_scroll(inst->term, 0, +5);
	return TRUE;
    }

    shift = event->state & GDK_SHIFT_MASK;
    ctrl = event->state & GDK_CONTROL_MASK;
    alt = event->state & GDK_MOD1_MASK;

    if (event->button == 3 && ctrl) {
	gtk_menu_popup(GTK_MENU(inst->menu), NULL, NULL, NULL, NULL,
		       event->button, event->time);
	return TRUE;
    }

    if (event->button == 1)
	button = MBT_LEFT;
    else if (event->button == 2)
	button = MBT_MIDDLE;
    else if (event->button == 3)
	button = MBT_RIGHT;
    else
	return FALSE;		       /* don't even know what button! */

    switch (event->type) {
      case GDK_BUTTON_PRESS: act = MA_CLICK; break;
      case GDK_BUTTON_RELEASE: act = MA_RELEASE; break;
      case GDK_2BUTTON_PRESS: act = MA_2CLK; break;
      case GDK_3BUTTON_PRESS: act = MA_3CLK; break;
      default: return FALSE;	       /* don't know this event type */
    }

    if (send_raw_mouse && !(inst->cfg.mouse_override && shift) &&
	act != MA_CLICK && act != MA_RELEASE)
	return TRUE;		       /* we ignore these in raw mouse mode */

    x = (event->x - inst->cfg.window_border) / inst->font_width;
    y = (event->y - inst->cfg.window_border) / inst->font_height;

    term_mouse(inst->term, button, translate_button(button), act,
	       x, y, shift, ctrl, alt);

    return TRUE;
}

gint motion_event(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    int shift, ctrl, alt, x, y, button;

    /* Remember the timestamp. */
    inst->input_event_time = event->time;

    show_mouseptr(inst, 1);

    shift = event->state & GDK_SHIFT_MASK;
    ctrl = event->state & GDK_CONTROL_MASK;
    alt = event->state & GDK_MOD1_MASK;
    if (event->state & GDK_BUTTON1_MASK)
	button = MBT_LEFT;
    else if (event->state & GDK_BUTTON2_MASK)
	button = MBT_MIDDLE;
    else if (event->state & GDK_BUTTON3_MASK)
	button = MBT_RIGHT;
    else
	return FALSE;		       /* don't even know what button! */

    x = (event->x - inst->cfg.window_border) / inst->font_width;
    y = (event->y - inst->cfg.window_border) / inst->font_height;

    term_mouse(inst->term, button, translate_button(button), MA_DRAG,
	       x, y, shift, ctrl, alt);

    return TRUE;
}

void frontend_keypress(void *handle)
{
    struct gui_data *inst = (struct gui_data *)handle;

    /*
     * If our child process has exited but not closed, terminate on
     * any keypress.
     */
    if (inst->exited)
	exit(0);
}

void notify_remote_exit(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    int exitcode;

    if (!inst->exited &&
        (exitcode = inst->back->exitcode(inst->backhandle)) >= 0) {
	inst->exited = TRUE;
	if (inst->cfg.close_on_exit == FORCE_ON ||
	    (inst->cfg.close_on_exit == AUTO && exitcode == 0))
	    gtk_main_quit();	       /* just go */
	if (inst->ldisc) {
	    ldisc_free(inst->ldisc);
	    inst->ldisc = NULL;
	}
	if (inst->back) {
	    inst->back->free(inst->backhandle);
	    inst->backhandle = NULL;
	    inst->back = NULL;
            term_provide_resize_fn(inst->term, NULL, NULL);
	    update_specials_menu(inst);
	}
	gtk_widget_show(inst->restartitem);
    }
}

static gint timer_trigger(gpointer data)
{
    long now = GPOINTER_TO_INT(data);
    long next;
    long ticks;

    if (run_timers(now, &next)) {
	ticks = next - GETTICKCOUNT();
	timer_id = gtk_timeout_add(ticks > 0 ? ticks : 1, timer_trigger,
				   GINT_TO_POINTER(next));
    }

    /*
     * Never let a timer resume. If we need another one, we've
     * asked for it explicitly above.
     */
    return FALSE;
}

void timer_change_notify(long next)
{
    long ticks;

    if (timer_id)
	gtk_timeout_remove(timer_id);

    ticks = next - GETTICKCOUNT();
    if (ticks <= 0)
	ticks = 1;		       /* just in case */

    timer_id = gtk_timeout_add(ticks, timer_trigger,
			       GINT_TO_POINTER(next));
}

void fd_input_func(gpointer data, gint sourcefd, GdkInputCondition condition)
{
    /*
     * We must process exceptional notifications before ordinary
     * readability ones, or we may go straight past the urgent
     * marker.
     */
    if (condition & GDK_INPUT_EXCEPTION)
        select_result(sourcefd, 4);
    if (condition & GDK_INPUT_READ)
        select_result(sourcefd, 1);
    if (condition & GDK_INPUT_WRITE)
        select_result(sourcefd, 2);
}

void destroy(GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
}

gint focus_event(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    term_set_focus(inst->term, event->in);
    term_update(inst->term);
    show_mouseptr(inst, 1);
    return FALSE;
}

void set_busy_status(void *frontend, int status)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    inst->busy_status = status;
    update_mouseptr(inst);
}

/*
 * set or clear the "raw mouse message" mode
 */
void set_raw_mouse_mode(void *frontend, int activate)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    activate = activate && !inst->cfg.no_mouse_rep;
    send_raw_mouse = activate;
    update_mouseptr(inst);
}

void request_resize(void *frontend, int w, int h)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    int large_x, large_y;
    int offset_x, offset_y;
    int area_x, area_y;
    GtkRequisition inner, outer;

    /*
     * This is a heinous hack dreamed up by the gnome-terminal
     * people to get around a limitation in gtk. The problem is
     * that in order to set the size correctly we really need to be
     * calling gtk_window_resize - but that needs to know the size
     * of the _whole window_, not the drawing area. So what we do
     * is to set an artificially huge size request on the drawing
     * area, recompute the resulting size request on the window,
     * and look at the difference between the two. That gives us
     * the x and y offsets we need to translate drawing area size
     * into window size for real, and then we call
     * gtk_window_resize.
     */

    /*
     * We start by retrieving the current size of the whole window.
     * Adding a bit to _that_ will give us a value we can use as a
     * bogus size request which guarantees to be bigger than the
     * current size of the drawing area.
     */
    get_window_pixels(inst, &large_x, &large_y);
    large_x += 32;
    large_y += 32;

#if GTK_CHECK_VERSION(2,0,0)
    gtk_widget_set_size_request(inst->area, large_x, large_y);
#else
    gtk_widget_set_usize(inst->area, large_x, large_y);
#endif
    gtk_widget_size_request(inst->area, &inner);
    gtk_widget_size_request(inst->window, &outer);

    offset_x = outer.width - inner.width;
    offset_y = outer.height - inner.height;

    area_x = inst->font_width * w + 2*inst->cfg.window_border;
    area_y = inst->font_height * h + 2*inst->cfg.window_border;

    /*
     * Now we must set the size request on the drawing area back to
     * something sensible before we commit the real resize. Best
     * way to do this, I think, is to set it to what the size is
     * really going to end up being.
     */
#if GTK_CHECK_VERSION(2,0,0)
    gtk_widget_set_size_request(inst->area, area_x, area_y);
#else
    gtk_widget_set_usize(inst->area, area_x, area_y);
    gtk_drawing_area_size(GTK_DRAWING_AREA(inst->area), area_x, area_y);
#endif

    gtk_container_dequeue_resize_handler(GTK_CONTAINER(inst->window));

#if GTK_CHECK_VERSION(2,0,0)
    gtk_window_resize(GTK_WINDOW(inst->window),
		      area_x + offset_x, area_y + offset_y);
#else
    gdk_window_resize(inst->window->window,
		      area_x + offset_x, area_y + offset_y);
#endif
}

static void real_palette_set(struct gui_data *inst, int n, int r, int g, int b)
{
    gboolean success[1];

    inst->cols[n].red = r * 0x0101;
    inst->cols[n].green = g * 0x0101;
    inst->cols[n].blue = b * 0x0101;

    gdk_colormap_free_colors(inst->colmap, inst->cols + n, 1);
    gdk_colormap_alloc_colors(inst->colmap, inst->cols + n, 1,
			      FALSE, TRUE, success);
    if (!success[0])
	g_error("%s: couldn't allocate colour %d (#%02x%02x%02x)\n", appname,
		n, r, g, b);
}

void set_window_background(struct gui_data *inst)
{
    if (inst->area && inst->area->window)
	gdk_window_set_background(inst->area->window, &inst->cols[258]);
    if (inst->window && inst->window->window)
	gdk_window_set_background(inst->window->window, &inst->cols[258]);
}

void palette_set(void *frontend, int n, int r, int g, int b)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    if (n >= 16)
	n += 256 - 16;
    if (n > NALLCOLOURS)
	return;
    real_palette_set(inst, n, r, g, b);
    if (n == 258) {
	/* Default Background changed. Ensure space between text area and
	 * window border is redrawn */
	set_window_background(inst);
	draw_backing_rect(inst);
	gtk_widget_queue_draw(inst->area);
    }
}

void palette_reset(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    /* This maps colour indices in inst->cfg to those used in inst->cols. */
    static const int ww[] = {
	256, 257, 258, 259, 260, 261,
	0, 8, 1, 9, 2, 10, 3, 11,
	4, 12, 5, 13, 6, 14, 7, 15
    };
    gboolean success[NALLCOLOURS];
    int i;

    assert(lenof(ww) == NCFGCOLOURS);

    if (!inst->colmap) {
	inst->colmap = gdk_colormap_get_system();
    } else {
	gdk_colormap_free_colors(inst->colmap, inst->cols, NALLCOLOURS);
    }

    for (i = 0; i < NCFGCOLOURS; i++) {
	inst->cols[ww[i]].red = inst->cfg.colours[i][0] * 0x0101;
	inst->cols[ww[i]].green = inst->cfg.colours[i][1] * 0x0101;
	inst->cols[ww[i]].blue = inst->cfg.colours[i][2] * 0x0101;
    }

    for (i = 0; i < NEXTCOLOURS; i++) {
	if (i < 216) {
	    int r = i / 36, g = (i / 6) % 6, b = i % 6;
	    inst->cols[i+16].red = r ? r * 0x2828 + 0x3737 : 0;
	    inst->cols[i+16].green = g ? g * 0x2828 + 0x3737 : 0;
	    inst->cols[i+16].blue = b ? b * 0x2828 + 0x3737 : 0;
	} else {
	    int shade = i - 216;
	    shade = shade * 0x0a0a + 0x0808;
	    inst->cols[i+16].red = inst->cols[i+16].green =
		inst->cols[i+16].blue = shade;
	}
    }

    gdk_colormap_alloc_colors(inst->colmap, inst->cols, NALLCOLOURS,
			      FALSE, TRUE, success);
    for (i = 0; i < NALLCOLOURS; i++) {
	if (!success[i])
	    g_error("%s: couldn't allocate colour %d (#%02x%02x%02x)\n",
                    appname, i, inst->cfg.colours[i][0],
                    inst->cfg.colours[i][1], inst->cfg.colours[i][2]);
    }

    /* Since Default Background may have changed, ensure that space
     * between text area and window border is refreshed. */
    set_window_background(inst);
    if (inst->area) {
	draw_backing_rect(inst);
	gtk_widget_queue_draw(inst->area);
    }
}

/* Ensure that all the cut buffers exist - according to the ICCCM, we must
 * do this before we start using cut buffers.
 */
void init_cutbuffers()
{
    unsigned char empty[] = "";
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER0, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER1, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER2, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER3, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER4, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER5, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER6, XA_STRING, 8, PropModeAppend, empty, 0);
    XChangeProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(),
		    XA_CUT_BUFFER7, XA_STRING, 8, PropModeAppend, empty, 0);
}

/* Store the data in a cut-buffer. */
void store_cutbuffer(char * ptr, int len)
{
    /* ICCCM says we must rotate the buffers before storing to buffer 0. */
    XRotateBuffers(GDK_DISPLAY(), 1);
    XStoreBytes(GDK_DISPLAY(), ptr, len);
}

/* Retrieve data from a cut-buffer.
 * Returned data needs to be freed with XFree().
 */
char * retrieve_cutbuffer(int * nbytes)
{
    char * ptr;
    ptr = XFetchBytes(GDK_DISPLAY(), nbytes);
    if (*nbytes <= 0 && ptr != 0) {
	XFree(ptr);
	ptr = 0;
    }
    return ptr;
}

void write_clip(void *frontend, wchar_t * data, int *attr, int len, int must_deselect)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    if (inst->pasteout_data)
	sfree(inst->pasteout_data);
    if (inst->pasteout_data_ctext)
	sfree(inst->pasteout_data_ctext);
    if (inst->pasteout_data_utf8)
	sfree(inst->pasteout_data_utf8);

    /*
     * Set up UTF-8 and compound text paste data. This only happens
     * if we aren't in direct-to-font mode using the D800 hack.
     */
    if (!inst->direct_to_font) {
	wchar_t *tmp = data;
	int tmplen = len;
	XTextProperty tp;
	char *list[1];

	inst->pasteout_data_utf8 = snewn(len*6, char);
	inst->pasteout_data_utf8_len = len*6;
	inst->pasteout_data_utf8_len =
	    charset_from_unicode(&tmp, &tmplen, inst->pasteout_data_utf8,
				 inst->pasteout_data_utf8_len,
				 CS_UTF8, NULL, NULL, 0);
	if (inst->pasteout_data_utf8_len == 0) {
	    sfree(inst->pasteout_data_utf8);
	    inst->pasteout_data_utf8 = NULL;
	} else {
	    inst->pasteout_data_utf8 =
		sresize(inst->pasteout_data_utf8,
			inst->pasteout_data_utf8_len + 1, char);
	    inst->pasteout_data_utf8[inst->pasteout_data_utf8_len] = '\0';
	}

	/*
	 * Now let Xlib convert our UTF-8 data into compound text.
	 */
	list[0] = inst->pasteout_data_utf8;
	if (Xutf8TextListToTextProperty(GDK_DISPLAY(), list, 1,
					XCompoundTextStyle, &tp) == 0) {
	    inst->pasteout_data_ctext = snewn(tp.nitems+1, char);
	    memcpy(inst->pasteout_data_ctext, tp.value, tp.nitems);
	    inst->pasteout_data_ctext_len = tp.nitems;
	    XFree(tp.value);
	} else {
            inst->pasteout_data_ctext = NULL;
            inst->pasteout_data_ctext_len = 0;
        }
    } else {
	inst->pasteout_data_utf8 = NULL;
	inst->pasteout_data_utf8_len = 0;
	inst->pasteout_data_ctext = NULL;
	inst->pasteout_data_ctext_len = 0;
    }

    inst->pasteout_data = snewn(len*6, char);
    inst->pasteout_data_len = len*6;
    inst->pasteout_data_len = wc_to_mb(inst->ucsdata.line_codepage, 0,
				       data, len, inst->pasteout_data,
				       inst->pasteout_data_len,
				       NULL, NULL, NULL);
    if (inst->pasteout_data_len == 0) {
	sfree(inst->pasteout_data);
	inst->pasteout_data = NULL;
    } else {
	inst->pasteout_data =
	    sresize(inst->pasteout_data, inst->pasteout_data_len, char);
    }

    store_cutbuffer(inst->pasteout_data, inst->pasteout_data_len);

    if (gtk_selection_owner_set(inst->area, GDK_SELECTION_PRIMARY,
				inst->input_event_time)) {
	gtk_selection_add_target(inst->area, GDK_SELECTION_PRIMARY,
				 GDK_SELECTION_TYPE_STRING, 1);
	if (inst->pasteout_data_ctext)
	    gtk_selection_add_target(inst->area, GDK_SELECTION_PRIMARY,
				     compound_text_atom, 1);
	if (inst->pasteout_data_utf8)
	    gtk_selection_add_target(inst->area, GDK_SELECTION_PRIMARY,
				     utf8_string_atom, 1);
    }

    if (must_deselect)
	term_deselect(inst->term);
}

void selection_get(GtkWidget *widget, GtkSelectionData *seldata,
		   guint info, guint time_stamp, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    if (seldata->target == utf8_string_atom)
	gtk_selection_data_set(seldata, seldata->target, 8,
			       (unsigned char *)inst->pasteout_data_utf8,
			       inst->pasteout_data_utf8_len);
    else if (seldata->target == compound_text_atom)
	gtk_selection_data_set(seldata, seldata->target, 8,
			       (unsigned char *)inst->pasteout_data_ctext,
			       inst->pasteout_data_ctext_len);
    else
	gtk_selection_data_set(seldata, seldata->target, 8,
			       (unsigned char *)inst->pasteout_data,
			       inst->pasteout_data_len);
}

gint selection_clear(GtkWidget *widget, GdkEventSelection *seldata,
		     gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;

    term_deselect(inst->term);
    if (inst->pasteout_data)
	sfree(inst->pasteout_data);
    if (inst->pasteout_data_ctext)
	sfree(inst->pasteout_data_ctext);
    if (inst->pasteout_data_utf8)
	sfree(inst->pasteout_data_utf8);
    inst->pasteout_data = NULL;
    inst->pasteout_data_len = 0;
    inst->pasteout_data_ctext = NULL;
    inst->pasteout_data_ctext_len = 0;
    inst->pasteout_data_utf8 = NULL;
    inst->pasteout_data_utf8_len = 0;
    return TRUE;
}

void request_paste(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    /*
     * In Unix, pasting is asynchronous: all we can do at the
     * moment is to call gtk_selection_convert(), and when the data
     * comes back _then_ we can call term_do_paste().
     */

    if (!inst->direct_to_font) {
	/*
	 * First we attempt to retrieve the selection as a UTF-8
	 * string (which we will convert to the correct code page
	 * before sending to the session, of course). If that
	 * fails, selection_received() will be informed and will
	 * fall back to an ordinary string.
	 */
	gtk_selection_convert(inst->area, GDK_SELECTION_PRIMARY,
			      utf8_string_atom,
			      inst->input_event_time);
    } else {
	/*
	 * If we're in direct-to-font mode, we disable UTF-8
	 * pasting, and go straight to ordinary string data.
	 */
	gtk_selection_convert(inst->area, GDK_SELECTION_PRIMARY,
			      GDK_SELECTION_TYPE_STRING,
			      inst->input_event_time);
    }
}

gint idle_paste_func(gpointer data);   /* forward ref */

void selection_received(GtkWidget *widget, GtkSelectionData *seldata,
			guint time, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    XTextProperty tp;
    char **list;
    char *text;
    int length, count, ret;
    int free_list_required = 0;
    int free_required = 0;
    int charset;

    if (seldata->target == utf8_string_atom && seldata->length <= 0) {
	/*
	 * Failed to get a UTF-8 selection string. Try compound
	 * text next.
	 */
	gtk_selection_convert(inst->area, GDK_SELECTION_PRIMARY,
			      compound_text_atom,
			      inst->input_event_time);
	return;
    }

    if (seldata->target == compound_text_atom && seldata->length <= 0) {
	/*
	 * Failed to get UTF-8 or compound text. Try an ordinary
	 * string.
	 */
	gtk_selection_convert(inst->area, GDK_SELECTION_PRIMARY,
			      GDK_SELECTION_TYPE_STRING,
			      inst->input_event_time);
	return;
    }

    /*
     * If we have data, but it's not of a type we can deal with,
     * we have to ignore the data.
     */
    if (seldata->length > 0 &&
	seldata->type != GDK_SELECTION_TYPE_STRING &&
	seldata->type != compound_text_atom &&
	seldata->type != utf8_string_atom)
	return;

    /*
     * If we have no data, try looking in a cut buffer.
     */
    if (seldata->length <= 0) {
	text = retrieve_cutbuffer(&length);
	if (length == 0)
	    return;
	/* Xterm is rumoured to expect Latin-1, though I havn't checked the
	 * source, so use that as a de-facto standard. */
	charset = CS_ISO8859_1;
	free_required = 1;
    } else {
	/*
	 * Convert COMPOUND_TEXT into UTF-8.
	 */
	if (seldata->type == compound_text_atom) {
	    tp.value = seldata->data;
	    tp.encoding = (Atom) seldata->type;
	    tp.format = seldata->format;
	    tp.nitems = seldata->length;
	    ret = Xutf8TextPropertyToTextList(GDK_DISPLAY(), &tp,
					      &list, &count);
	    if (ret != 0 || count != 1) {
		/*
		 * Compound text failed; fall back to STRING.
		 */
		gtk_selection_convert(inst->area, GDK_SELECTION_PRIMARY,
				      GDK_SELECTION_TYPE_STRING,
				      inst->input_event_time);
		return;
	    }
	    text = list[0];
	    length = strlen(list[0]);
	    charset = CS_UTF8;
	    free_list_required = 1;
	} else {
	    text = (char *)seldata->data;
	    length = seldata->length;
	    charset = (seldata->type == utf8_string_atom ?
		       CS_UTF8 : inst->ucsdata.line_codepage);
	}
    }

    if (inst->pastein_data)
	sfree(inst->pastein_data);

    inst->pastein_data = snewn(length, wchar_t);
    inst->pastein_data_len = length;
    inst->pastein_data_len =
	mb_to_wc(charset, 0, text, length,
		 inst->pastein_data, inst->pastein_data_len);

    term_do_paste(inst->term);

    if (term_paste_pending(inst->term))
	inst->term_paste_idle_id = gtk_idle_add(idle_paste_func, inst);

    if (free_list_required)
	XFreeStringList(list);
    if (free_required)
	XFree(text);
}

gint idle_paste_func(gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;

    if (term_paste_pending(inst->term))
	term_paste(inst->term);
    else
	gtk_idle_remove(inst->term_paste_idle_id);

    return TRUE;
}


void get_clip(void *frontend, wchar_t ** p, int *len)
{
    struct gui_data *inst = (struct gui_data *)frontend;

    if (p) {
	*p = inst->pastein_data;
	*len = inst->pastein_data_len;
    }
}

static void set_window_titles(struct gui_data *inst)
{
    /*
     * We must always call set_icon_name after calling set_title,
     * since set_title will write both names. Irritating, but such
     * is life.
     */
    gtk_window_set_title(GTK_WINDOW(inst->window), inst->wintitle);
    if (!inst->cfg.win_name_always)
	gdk_window_set_icon_name(inst->window->window, inst->icontitle);
}

void set_title(void *frontend, char *title)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    strncpy(inst->wintitle, title, lenof(inst->wintitle));
    inst->wintitle[lenof(inst->wintitle)-1] = '\0';
    set_window_titles(inst);
}

void set_icon(void *frontend, char *title)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    strncpy(inst->icontitle, title, lenof(inst->icontitle));
    inst->icontitle[lenof(inst->icontitle)-1] = '\0';
    set_window_titles(inst);
}

void set_sbar(void *frontend, int total, int start, int page)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    if (!inst->cfg.scrollbar)
	return;
    inst->sbar_adjust->lower = 0;
    inst->sbar_adjust->upper = total;
    inst->sbar_adjust->value = start;
    inst->sbar_adjust->page_size = page;
    inst->sbar_adjust->step_increment = 1;
    inst->sbar_adjust->page_increment = page/2;
    inst->ignore_sbar = TRUE;
    gtk_adjustment_changed(inst->sbar_adjust);
    inst->ignore_sbar = FALSE;
}

void scrollbar_moved(GtkAdjustment *adj, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;

    if (!inst->cfg.scrollbar)
	return;
    if (!inst->ignore_sbar)
	term_scroll(inst->term, 1, (int)adj->value);
}

void sys_cursor(void *frontend, int x, int y)
{
    /*
     * This is meaningless under X.
     */
}

/*
 * This is still called when mode==BELL_VISUAL, even though the
 * visual bell is handled entirely within terminal.c, because we
 * may want to perform additional actions on any kind of bell (for
 * example, taskbar flashing in Windows).
 */
void do_beep(void *frontend, int mode)
{
    if (mode == BELL_DEFAULT)
	gdk_beep();
}

int char_width(Context ctx, int uc)
{
    /*
     * Under X, any fixed-width font really _is_ fixed-width.
     * Double-width characters will be dealt with using a separate
     * font. For the moment we can simply return 1.
     */
    return 1;
}

Context get_ctx(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    struct draw_ctx *dctx;

    if (!inst->area->window)
	return NULL;

    dctx = snew(struct draw_ctx);
    dctx->inst = inst;
    dctx->gc = gdk_gc_new(inst->area->window);
    return dctx;
}

void free_ctx(Context ctx)
{
    struct draw_ctx *dctx = (struct draw_ctx *)ctx;
    /* struct gui_data *inst = dctx->inst; */
    GdkGC *gc = dctx->gc;
    gdk_gc_unref(gc);
    sfree(dctx);
}

/*
 * Draw a line of text in the window, at given character
 * coordinates, in given attributes.
 *
 * We are allowed to fiddle with the contents of `text'.
 */
void do_text_internal(Context ctx, int x, int y, wchar_t *text, int len,
		      unsigned long attr, int lattr)
{
    struct draw_ctx *dctx = (struct draw_ctx *)ctx;
    struct gui_data *inst = dctx->inst;
    GdkGC *gc = dctx->gc;
    int ncombining, combining;
    int nfg, nbg, t, fontid, shadow, rlen, widefactor;
    int monochrome = gtk_widget_get_visual(inst->area)->depth == 1;

    if (attr & TATTR_COMBINING) {
	ncombining = len;
	len = 1;
    } else
	ncombining = 1;

    nfg = ((monochrome ? ATTR_DEFFG : (attr & ATTR_FGMASK)) >> ATTR_FGSHIFT);
    nbg = ((monochrome ? ATTR_DEFBG : (attr & ATTR_BGMASK)) >> ATTR_BGSHIFT);
    if (!!(attr & ATTR_REVERSE) ^ (monochrome && (attr & TATTR_ACTCURS))) {
	t = nfg;
	nfg = nbg;
	nbg = t;
    }
    if (inst->cfg.bold_colour && (attr & ATTR_BOLD)) {
	if (nfg < 16) nfg |= 8;
	else if (nfg >= 256) nfg |= 1;
    }
    if (inst->cfg.bold_colour && (attr & ATTR_BLINK)) {
	if (nbg < 16) nbg |= 8;
	else if (nbg >= 256) nbg |= 1;
    }
    if ((attr & TATTR_ACTCURS) && !monochrome) {
	nfg = 260;
	nbg = 261;
    }

    fontid = shadow = 0;

    if (attr & ATTR_WIDE) {
	widefactor = 2;
	fontid |= 2;
    } else {
	widefactor = 1;
    }

    if ((attr & ATTR_BOLD) && !inst->cfg.bold_colour) {
	if (inst->fonts[fontid | 1])
	    fontid |= 1;
	else
	    shadow = 1;
    }

    if ((lattr & LATTR_MODE) != LATTR_NORM) {
	x *= 2;
	if (x >= inst->term->cols)
	    return;
	if (x + len*2*widefactor > inst->term->cols)
	    len = (inst->term->cols-x)/2/widefactor;/* trim to LH half */
	rlen = len * 2;
    } else
	rlen = len;

    {
	GdkRectangle r;

	r.x = x*inst->font_width+inst->cfg.window_border;
	r.y = y*inst->font_height+inst->cfg.window_border;
	r.width = rlen*widefactor*inst->font_width;
	r.height = inst->font_height;
	gdk_gc_set_clip_rectangle(gc, &r);
    }

    gdk_gc_set_foreground(gc, &inst->cols[nbg]);
    gdk_draw_rectangle(inst->pixmap, gc, 1,
		       x*inst->font_width+inst->cfg.window_border,
		       y*inst->font_height+inst->cfg.window_border,
		       rlen*widefactor*inst->font_width, inst->font_height);

    gdk_gc_set_foreground(gc, &inst->cols[nfg]);
    {
	GdkWChar *gwcs;
	gchar *gcs;
	wchar_t *wcs;
	int i;

	wcs = snewn(len*ncombining+1, wchar_t);
	for (i = 0; i < len*ncombining; i++) {
	    wcs[i] = text[i];
	}

	if (inst->fonts[fontid] == NULL && (fontid & 2)) {
	    /*
	     * We've been given ATTR_WIDE, but have no wide font.
	     * Fall back to the non-wide font.
	     */
	    fontid &= ~2;
	}

	if (inst->fonts[fontid] == NULL) {
	    /*
	     * The font for this contingency does not exist. So we
	     * display nothing at all; such is life.
	     */
	} else if (inst->fontinfo[fontid].is_wide) {
	    /*
	     * At least one version of gdk_draw_text_wc() has a
	     * weird bug whereby it reads `len' elements of the
	     * input string, but only draws `len/2'. Hence I'm
	     * going to make its input array twice as long as it
	     * theoretically needs to be, and pass in twice the
	     * actual number of characters. If a fixed gdk actually
	     * takes the doubled length seriously, then (a) the
	     * array will stand scrutiny up to the full length, (b)
	     * the spare elements of the array are full of zeroes
	     * which will probably be an empty glyph in the font,
	     * and (c) the clip rectangle should prevent it causing
	     * trouble anyway.
	     */
	    gwcs = snewn(len*2+1, GdkWChar);
	    memset(gwcs, 0, sizeof(GdkWChar) * (len*2+1));
	    /*
	     * FIXME: when we have a wide-char equivalent of
	     * from_unicode, use it instead of this.
	     */
	    for (combining = 0; combining < ncombining; combining++) {
		for (i = 0; i <= len; i++)
		    gwcs[i] = wcs[i + combining];
		gdk_draw_text_wc(inst->pixmap, inst->fonts[fontid], gc,
				 x*inst->font_width+inst->cfg.window_border,
				 y*inst->font_height+inst->cfg.window_border+inst->fonts[0]->ascent,
				 gwcs, len*2);
		if (shadow)
		    gdk_draw_text_wc(inst->pixmap, inst->fonts[fontid], gc,
				     x*inst->font_width+inst->cfg.window_border+inst->cfg.shadowboldoffset,
				     y*inst->font_height+inst->cfg.window_border+inst->fonts[0]->ascent,
				     gwcs, len*2);
	    }
	    sfree(gwcs);
	} else {
	    gcs = snewn(len+1, gchar);
	    for (combining = 0; combining < ncombining; combining++) {
		wc_to_mb(inst->fontinfo[fontid].charset, 0,
			 wcs + combining, len, gcs, len, ".", NULL, NULL);
		gdk_draw_text(inst->pixmap, inst->fonts[fontid], gc,
			      x*inst->font_width+inst->cfg.window_border,
			      y*inst->font_height+inst->cfg.window_border+inst->fonts[0]->ascent,
			      gcs, len);
		if (shadow)
		    gdk_draw_text(inst->pixmap, inst->fonts[fontid], gc,
				  x*inst->font_width+inst->cfg.window_border+inst->cfg.shadowboldoffset,
				  y*inst->font_height+inst->cfg.window_border+inst->fonts[0]->ascent,
				  gcs, len);
	    }
	    sfree(gcs);
	}
	sfree(wcs);
    }

    if (attr & ATTR_UNDER) {
	int uheight = inst->fonts[0]->ascent + 1;
	if (uheight >= inst->font_height)
	    uheight = inst->font_height - 1;
	gdk_draw_line(inst->pixmap, gc, x*inst->font_width+inst->cfg.window_border,
		      y*inst->font_height + uheight + inst->cfg.window_border,
		      (x+len)*widefactor*inst->font_width-1+inst->cfg.window_border,
		      y*inst->font_height + uheight + inst->cfg.window_border);
    }

    if ((lattr & LATTR_MODE) != LATTR_NORM) {
	/*
	 * I can't find any plausible StretchBlt equivalent in the
	 * X server, so I'm going to do this the slow and painful
	 * way. This will involve repeated calls to
	 * gdk_draw_pixmap() to stretch the text horizontally. It's
	 * O(N^2) in time and O(N) in network bandwidth, but you
	 * try thinking of a better way. :-(
	 */
	int i;
	for (i = 0; i < len * widefactor * inst->font_width; i++) {
	    gdk_draw_pixmap(inst->pixmap, gc, inst->pixmap,
			    x*inst->font_width+inst->cfg.window_border + 2*i,
			    y*inst->font_height+inst->cfg.window_border,
			    x*inst->font_width+inst->cfg.window_border + 2*i+1,
			    y*inst->font_height+inst->cfg.window_border,
			    len * widefactor * inst->font_width - i, inst->font_height);
	}
	len *= 2;
	if ((lattr & LATTR_MODE) != LATTR_WIDE) {
	    int dt, db;
	    /* Now stretch vertically, in the same way. */
	    if ((lattr & LATTR_MODE) == LATTR_BOT)
		dt = 0, db = 1;
	    else
		dt = 1, db = 0;
	    for (i = 0; i < inst->font_height; i+=2) {
		gdk_draw_pixmap(inst->pixmap, gc, inst->pixmap,
				x*inst->font_width+inst->cfg.window_border,
				y*inst->font_height+inst->cfg.window_border+dt*i+db,
				x*inst->font_width+inst->cfg.window_border,
				y*inst->font_height+inst->cfg.window_border+dt*(i+1),
				len * widefactor * inst->font_width, inst->font_height-i-1);
	    }
	}
    }
}

void do_text(Context ctx, int x, int y, wchar_t *text, int len,
	     unsigned long attr, int lattr)
{
    struct draw_ctx *dctx = (struct draw_ctx *)ctx;
    struct gui_data *inst = dctx->inst;
    GdkGC *gc = dctx->gc;
    int widefactor;

    do_text_internal(ctx, x, y, text, len, attr, lattr);

    if (attr & ATTR_WIDE) {
	widefactor = 2;
    } else {
	widefactor = 1;
    }

    if ((lattr & LATTR_MODE) != LATTR_NORM) {
	x *= 2;
	if (x >= inst->term->cols)
	    return;
	if (x + len*2*widefactor > inst->term->cols)
	    len = (inst->term->cols-x)/2/widefactor;/* trim to LH half */
	len *= 2;
    }

    gdk_draw_pixmap(inst->area->window, gc, inst->pixmap,
		    x*inst->font_width+inst->cfg.window_border,
		    y*inst->font_height+inst->cfg.window_border,
		    x*inst->font_width+inst->cfg.window_border,
		    y*inst->font_height+inst->cfg.window_border,
		    len*widefactor*inst->font_width, inst->font_height);
}

void do_cursor(Context ctx, int x, int y, wchar_t *text, int len,
	       unsigned long attr, int lattr)
{
    struct draw_ctx *dctx = (struct draw_ctx *)ctx;
    struct gui_data *inst = dctx->inst;
    GdkGC *gc = dctx->gc;

    int active, passive, widefactor;

    if (attr & TATTR_PASCURS) {
	attr &= ~TATTR_PASCURS;
	passive = 1;
    } else
	passive = 0;
    if ((attr & TATTR_ACTCURS) && inst->cfg.cursor_type != 0) {
	attr &= ~TATTR_ACTCURS;
        active = 1;
    } else
        active = 0;
    do_text_internal(ctx, x, y, text, len, attr, lattr);

    if (attr & TATTR_COMBINING)
	len = 1;

    if (attr & ATTR_WIDE) {
	widefactor = 2;
    } else {
	widefactor = 1;
    }

    if ((lattr & LATTR_MODE) != LATTR_NORM) {
	x *= 2;
	if (x >= inst->term->cols)
	    return;
	if (x + len*2*widefactor > inst->term->cols)
	    len = (inst->term->cols-x)/2/widefactor;/* trim to LH half */
	len *= 2;
    }

    if (inst->cfg.cursor_type == 0) {
	/*
	 * An active block cursor will already have been done by
	 * the above do_text call, so we only need to do anything
	 * if it's passive.
	 */
	if (passive) {
	    gdk_gc_set_foreground(gc, &inst->cols[261]);
	    gdk_draw_rectangle(inst->pixmap, gc, 0,
			       x*inst->font_width+inst->cfg.window_border,
			       y*inst->font_height+inst->cfg.window_border,
			       len*widefactor*inst->font_width-1, inst->font_height-1);
	}
    } else {
	int uheight;
	int startx, starty, dx, dy, length, i;

	int char_width;

	if ((attr & ATTR_WIDE) || (lattr & LATTR_MODE) != LATTR_NORM)
	    char_width = 2*inst->font_width;
	else
	    char_width = inst->font_width;

	if (inst->cfg.cursor_type == 1) {
	    uheight = inst->fonts[0]->ascent + 1;
	    if (uheight >= inst->font_height)
		uheight = inst->font_height - 1;

	    startx = x * inst->font_width + inst->cfg.window_border;
	    starty = y * inst->font_height + inst->cfg.window_border + uheight;
	    dx = 1;
	    dy = 0;
	    length = len * widefactor * char_width;
	} else {
	    int xadjust = 0;
	    if (attr & TATTR_RIGHTCURS)
		xadjust = char_width - 1;
	    startx = x * inst->font_width + inst->cfg.window_border + xadjust;
	    starty = y * inst->font_height + inst->cfg.window_border;
	    dx = 0;
	    dy = 1;
	    length = inst->font_height;
	}

	gdk_gc_set_foreground(gc, &inst->cols[261]);
	if (passive) {
	    for (i = 0; i < length; i++) {
		if (i % 2 == 0) {
		    gdk_draw_point(inst->pixmap, gc, startx, starty);
		}
		startx += dx;
		starty += dy;
	    }
	} else if (active) {
	    gdk_draw_line(inst->pixmap, gc, startx, starty,
			  startx + (length-1) * dx, starty + (length-1) * dy);
	} /* else no cursor (e.g., blinked off) */
    }

    gdk_draw_pixmap(inst->area->window, gc, inst->pixmap,
		    x*inst->font_width+inst->cfg.window_border,
		    y*inst->font_height+inst->cfg.window_border,
		    x*inst->font_width+inst->cfg.window_border,
		    y*inst->font_height+inst->cfg.window_border,
		    len*widefactor*inst->font_width, inst->font_height);
}

GdkCursor *make_mouse_ptr(struct gui_data *inst, int cursor_val)
{
    /*
     * Truly hideous hack: GTK doesn't allow us to set the mouse
     * cursor foreground and background colours unless we've _also_
     * created our own cursor from bitmaps. Therefore, I need to
     * load the `cursor' font and draw glyphs from it on to
     * pixmaps, in order to construct my cursors with the fg and bg
     * I want. This is a gross hack, but it's more self-contained
     * than linking in Xlib to find the X window handle to
     * inst->area and calling XRecolorCursor, and it's more
     * futureproof than hard-coding the shapes as bitmap arrays.
     */
    static GdkFont *cursor_font = NULL;
    GdkPixmap *source, *mask;
    GdkGC *gc;
    GdkColor cfg = { 0, 65535, 65535, 65535 };
    GdkColor cbg = { 0, 0, 0, 0 };
    GdkColor dfg = { 1, 65535, 65535, 65535 };
    GdkColor dbg = { 0, 0, 0, 0 };
    GdkCursor *ret;
    gchar text[2];
    gint lb, rb, wid, asc, desc, w, h, x, y;

    if (cursor_val == -2) {
	gdk_font_unref(cursor_font);
	return NULL;
    }

    if (cursor_val >= 0 && !cursor_font)
	cursor_font = gdk_font_load("cursor");

    /*
     * Get the text extent of the cursor in question. We use the
     * mask character for this, because it's typically slightly
     * bigger than the main character.
     */
    if (cursor_val >= 0) {
	text[1] = '\0';
	text[0] = (char)cursor_val + 1;
	gdk_string_extents(cursor_font, text, &lb, &rb, &wid, &asc, &desc);
	w = rb-lb; h = asc+desc; x = -lb; y = asc;
    } else {
	w = h = 1;
	x = y = 0;
    }

    source = gdk_pixmap_new(NULL, w, h, 1);
    mask = gdk_pixmap_new(NULL, w, h, 1);

    /*
     * Draw the mask character on the mask pixmap.
     */
    gc = gdk_gc_new(mask);
    gdk_gc_set_foreground(gc, &dbg);
    gdk_draw_rectangle(mask, gc, 1, 0, 0, w, h);
    if (cursor_val >= 0) {
	text[1] = '\0';
	text[0] = (char)cursor_val + 1;
	gdk_gc_set_foreground(gc, &dfg);
	gdk_draw_text(mask, cursor_font, gc, x, y, text, 1);
    }
    gdk_gc_unref(gc);

    /*
     * Draw the main character on the source pixmap.
     */
    gc = gdk_gc_new(source);
    gdk_gc_set_foreground(gc, &dbg);
    gdk_draw_rectangle(source, gc, 1, 0, 0, w, h);
    if (cursor_val >= 0) {
	text[1] = '\0';
	text[0] = (char)cursor_val;
	gdk_gc_set_foreground(gc, &dfg);
	gdk_draw_text(source, cursor_font, gc, x, y, text, 1);
    }
    gdk_gc_unref(gc);

    /*
     * Create the cursor.
     */
    ret = gdk_cursor_new_from_pixmap(source, mask, &cfg, &cbg, x, y);

    /*
     * Clean up.
     */
    gdk_pixmap_unref(source);
    gdk_pixmap_unref(mask);

    return ret;
}

void modalfatalbox(char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void cmdline_error(char *p, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", appname);
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

char *get_x_display(void *frontend)
{
    return gdk_get_display();
}

long get_windowid(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;
    return (long)GDK_WINDOW_XWINDOW(inst->area->window);
}

static void help(FILE *fp) {
    if(fprintf(fp,
"pterm option summary:\n"
"\n"
"  --display DISPLAY         Specify X display to use (note '--')\n"
"  -name PREFIX              Prefix when looking up resources (default: pterm)\n"
"  -fn FONT                  Normal text font\n"
"  -fb FONT                  Bold text font\n"
"  -geometry GEOMETRY        Position and size of window (size in characters)\n"
"  -sl LINES                 Number of lines of scrollback\n"
"  -fg COLOUR, -bg COLOUR    Foreground/background colour\n"
"  -bfg COLOUR, -bbg COLOUR  Foreground/background bold colour\n"
"  -cfg COLOUR, -bfg COLOUR  Foreground/background cursor colour\n"
"  -T TITLE                  Window title\n"
"  -ut, +ut                  Do(default) or do not update utmp\n"
"  -ls, +ls                  Do(default) or do not make shell a login shell\n"
"  -sb, +sb                  Do(default) or do not display a scrollbar\n"
"  -log PATH                 Log all output to a file\n"
"  -nethack                  Map numeric keypad to hjklyubn direction keys\n"
"  -xrm RESOURCE-STRING      Set an X resource\n"
"  -e COMMAND [ARGS...]      Execute command (consumes all remaining args)\n"
	 ) < 0 || fflush(fp) < 0) {
	perror("output error");
	exit(1);
    }
}

int do_cmdline(int argc, char **argv, int do_everything, int *allow_launch,
               struct gui_data *inst, Config *cfg)
{
    int err = 0;
    char *val;

    /*
     * Macros to make argument handling easier. Note that because
     * they need to call `continue', they cannot be contained in
     * the usual do {...} while (0) wrapper to make them
     * syntactically single statements; hence it is not legal to
     * use one of these macros as an unbraced statement between
     * `if' and `else'.
     */
#define EXPECTS_ARG { \
    if (--argc <= 0) { \
	err = 1; \
	fprintf(stderr, "%s: %s expects an argument\n", appname, p); \
        continue; \
    } else \
	val = *++argv; \
}
#define SECOND_PASS_ONLY { if (!do_everything) continue; }

    while (--argc > 0) {
	char *p = *++argv;
        int ret;

	/*
	 * Shameless cheating. Debian requires all X terminal
	 * emulators to support `-T title'; but
	 * cmdline_process_param will eat -T (it means no-pty) and
	 * complain that pterm doesn't support it. So, in pterm
	 * only, we convert -T into -title.
	 */
	if ((cmdline_tooltype & TOOLTYPE_NONNETWORK) &&
	    !strcmp(p, "-T"))
	    p = "-title";

        ret = cmdline_process_param(p, (argc > 1 ? argv[1] : NULL),
                                    do_everything ? 1 : -1, cfg);

	if (ret == -2) {
	    cmdline_error("option \"%s\" requires an argument", p);
	} else if (ret == 2) {
	    --argc, ++argv;            /* skip next argument */
            continue;
	} else if (ret == 1) {
            continue;
        }

	if (!strcmp(p, "-fn") || !strcmp(p, "-font")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->font.name, val, sizeof(cfg->font.name));
	    cfg->font.name[sizeof(cfg->font.name)-1] = '\0';

	} else if (!strcmp(p, "-fb")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->boldfont.name, val, sizeof(cfg->boldfont.name));
	    cfg->boldfont.name[sizeof(cfg->boldfont.name)-1] = '\0';

	} else if (!strcmp(p, "-fw")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->widefont.name, val, sizeof(cfg->widefont.name));
	    cfg->widefont.name[sizeof(cfg->widefont.name)-1] = '\0';

	} else if (!strcmp(p, "-fwb")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->wideboldfont.name, val, sizeof(cfg->wideboldfont.name));
	    cfg->wideboldfont.name[sizeof(cfg->wideboldfont.name)-1] = '\0';

	} else if (!strcmp(p, "-cs")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->line_codepage, val, sizeof(cfg->line_codepage));
	    cfg->line_codepage[sizeof(cfg->line_codepage)-1] = '\0';

	} else if (!strcmp(p, "-geometry")) {
	    int flags, x, y;
	    unsigned int w, h;
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;

	    flags = XParseGeometry(val, &x, &y, &w, &h);
	    if (flags & WidthValue)
		cfg->width = (int)w;
	    if (flags & HeightValue)
		cfg->height = (int)h;

            if (flags & (XValue | YValue)) {
                inst->xpos = x;
                inst->ypos = y;
                inst->gotpos = TRUE;
                inst->gravity = ((flags & XNegative ? 1 : 0) |
                                 (flags & YNegative ? 2 : 0));
            }

	} else if (!strcmp(p, "-sl")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    cfg->savelines = atoi(val);

	} else if (!strcmp(p, "-fg") || !strcmp(p, "-bg") ||
		   !strcmp(p, "-bfg") || !strcmp(p, "-bbg") ||
		   !strcmp(p, "-cfg") || !strcmp(p, "-cbg")) {
	    GdkColor col;

	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    if (!gdk_color_parse(val, &col)) {
		err = 1;
		fprintf(stderr, "%s: unable to parse colour \"%s\"\n",
                        appname, val);
	    } else {
		int index;
		index = (!strcmp(p, "-fg") ? 0 :
			 !strcmp(p, "-bg") ? 2 :
			 !strcmp(p, "-bfg") ? 1 :
			 !strcmp(p, "-bbg") ? 3 :
			 !strcmp(p, "-cfg") ? 4 :
			 !strcmp(p, "-cbg") ? 5 : -1);
		assert(index != -1);
		cfg->colours[index][0] = col.red / 256;
		cfg->colours[index][1] = col.green / 256;
		cfg->colours[index][2] = col.blue / 256;
	    }

	} else if (use_pty_argv && !strcmp(p, "-e")) {
	    /* This option swallows all further arguments. */
	    if (!do_everything)
		break;

	    if (--argc > 0) {
		int i;
		pty_argv = snewn(argc+1, char *);
		++argv;
		for (i = 0; i < argc; i++)
		    pty_argv[i] = argv[i];
		pty_argv[argc] = NULL;
		break;		       /* finished command-line processing */
	    } else
		err = 1, fprintf(stderr, "%s: -e expects an argument\n",
                                 appname);

	} else if (!strcmp(p, "-title")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->wintitle, val, sizeof(cfg->wintitle));
	    cfg->wintitle[sizeof(cfg->wintitle)-1] = '\0';

	} else if (!strcmp(p, "-log")) {
	    EXPECTS_ARG;
	    SECOND_PASS_ONLY;
	    strncpy(cfg->logfilename.path, val, sizeof(cfg->logfilename.path));
	    cfg->logfilename.path[sizeof(cfg->logfilename.path)-1] = '\0';
	    cfg->logtype = LGTYP_DEBUG;

	} else if (!strcmp(p, "-ut-") || !strcmp(p, "+ut")) {
	    SECOND_PASS_ONLY;
	    cfg->stamp_utmp = 0;

	} else if (!strcmp(p, "-ut")) {
	    SECOND_PASS_ONLY;
	    cfg->stamp_utmp = 1;

	} else if (!strcmp(p, "-ls-") || !strcmp(p, "+ls")) {
	    SECOND_PASS_ONLY;
	    cfg->login_shell = 0;

	} else if (!strcmp(p, "-ls")) {
	    SECOND_PASS_ONLY;
	    cfg->login_shell = 1;

	} else if (!strcmp(p, "-nethack")) {
	    SECOND_PASS_ONLY;
	    cfg->nethack_keypad = 1;

	} else if (!strcmp(p, "-sb-") || !strcmp(p, "+sb")) {
	    SECOND_PASS_ONLY;
	    cfg->scrollbar = 0;

	} else if (!strcmp(p, "-sb")) {
	    SECOND_PASS_ONLY;
	    cfg->scrollbar = 0;

	} else if (!strcmp(p, "-name")) {
	    EXPECTS_ARG;
	    app_name = val;

	} else if (!strcmp(p, "-xrm")) {
	    EXPECTS_ARG;
	    provide_xrm_string(val);

	} else if(!strcmp(p, "-help") || !strcmp(p, "--help")) {
	    help(stdout);
	    exit(0);

        } else if (!strcmp(p, "-pgpfp")) {
            pgp_fingerprints();
            exit(1);

	} else if(p[0] != '-' && (!do_everything ||
                                  process_nonoption_arg(p, cfg,
							allow_launch))) {
            /* do nothing */

	} else {
	    err = 1;
	    fprintf(stderr, "%s: unrecognized option '%s'\n", appname, p);
	}
    }

    return err;
}

/*
 * This function retrieves the character set encoding of a font. It
 * returns the character set without the X11 hack (in case the user
 * asks to use the font's own encoding).
 */
static int set_font_info(struct gui_data *inst, int fontid)
{
    GdkFont *font = inst->fonts[fontid];
    XFontStruct *xfs = GDK_FONT_XFONT(font);
    Display *disp = GDK_FONT_XDISPLAY(font);
    Atom charset_registry, charset_encoding;
    unsigned long registry_ret, encoding_ret;
    int retval = CS_NONE;

    charset_registry = XInternAtom(disp, "CHARSET_REGISTRY", False);
    charset_encoding = XInternAtom(disp, "CHARSET_ENCODING", False);
    inst->fontinfo[fontid].charset = CS_NONE;
    inst->fontinfo[fontid].is_wide = 0;
    if (XGetFontProperty(xfs, charset_registry, &registry_ret) &&
	XGetFontProperty(xfs, charset_encoding, &encoding_ret)) {
	char *reg, *enc;
	reg = XGetAtomName(disp, (Atom)registry_ret);
	enc = XGetAtomName(disp, (Atom)encoding_ret);
	if (reg && enc) {
	    char *encoding = dupcat(reg, "-", enc, NULL);
	    retval = inst->fontinfo[fontid].charset =
		charset_from_xenc(encoding);
	    /* FIXME: when libcharset supports wide encodings fix this. */
	    if (!strcasecmp(encoding, "iso10646-1")) {
		inst->fontinfo[fontid].is_wide = 1;
		retval = CS_UTF8;
	    }

	    /*
	     * Hack for X line-drawing characters: if the primary
	     * font is encoded as ISO-8859-anything, and has valid
	     * glyphs in the first 32 char positions, it is assumed
	     * that those glyphs are the VT100 line-drawing
	     * character set.
	     * 
	     * Actually, we'll hack even harder by only checking
	     * position 0x19 (vertical line, VT100 linedrawing
	     * `x'). Then we can check it easily by seeing if the
	     * ascent and descent differ.
	     */
	    if (inst->fontinfo[fontid].charset == CS_ISO8859_1) {
		int lb, rb, wid, asc, desc;
		gchar text[2];

		text[1] = '\0';
		text[0] = '\x12';
		gdk_string_extents(inst->fonts[fontid], text,
				   &lb, &rb, &wid, &asc, &desc);
		if (asc != desc)
		    inst->fontinfo[fontid].charset = CS_ISO8859_1_X11;
	    }

	    sfree(encoding);
	}
    }

    return retval;
}

int uxsel_input_add(int fd, int rwx) {
    int flags = 0;
    if (rwx & 1) flags |= GDK_INPUT_READ;
    if (rwx & 2) flags |= GDK_INPUT_WRITE;
    if (rwx & 4) flags |= GDK_INPUT_EXCEPTION;
    return gdk_input_add(fd, flags, fd_input_func, NULL);
}

void uxsel_input_remove(int id) {
    gdk_input_remove(id);
}

char *guess_derived_font_name(GdkFont *font, int bold, int wide)
{
    XFontStruct *xfs = GDK_FONT_XFONT(font);
    Display *disp = GDK_FONT_XDISPLAY(font);
    Atom fontprop = XInternAtom(disp, "FONT", False);
    unsigned long ret;
    if (XGetFontProperty(xfs, fontprop, &ret)) {
	char *name = XGetAtomName(disp, (Atom)ret);
	if (name && name[0] == '-') {
	    char *strings[13];
	    char *dupname, *extrafree = NULL, *ret;
	    char *p, *q;
	    int nstr;

	    p = q = dupname = dupstr(name); /* skip initial minus */
	    nstr = 0;

	    while (*p && nstr < lenof(strings)) {
		if (*p == '-') {
		    *p = '\0';
		    strings[nstr++] = p+1;
		}
		p++;
	    }

	    if (nstr < lenof(strings))
		return NULL;	       /* XLFD was malformed */

	    if (bold)
		strings[2] = "bold";

	    if (wide) {
		/* 4 is `wideness', which obviously may have changed. */
		/* 5 is additional style, which may be e.g. `ja' or `ko'. */
		strings[4] = strings[5] = "*";
		strings[11] = extrafree = dupprintf("%d", 2*atoi(strings[11]));
	    }

	    ret = dupcat("-", strings[ 0], "-", strings[ 1], "-", strings[ 2],
			 "-", strings[ 3], "-", strings[ 4], "-", strings[ 5],
			 "-", strings[ 6], "-", strings[ 7], "-", strings[ 8],
			 "-", strings[ 9], "-", strings[10], "-", strings[11],
			 "-", strings[12], NULL);
	    sfree(extrafree);
	    sfree(dupname);

	    return ret;
	}
    }
    return NULL;
}

void setup_fonts_ucs(struct gui_data *inst)
{
    int font_charset;
    char *name;
    int guessed;

    if (inst->fonts[0])
        gdk_font_unref(inst->fonts[0]);
    if (inst->fonts[1])
        gdk_font_unref(inst->fonts[1]);
    if (inst->fonts[2])
        gdk_font_unref(inst->fonts[2]);
    if (inst->fonts[3])
        gdk_font_unref(inst->fonts[3]);

    inst->fonts[0] = gdk_font_load(inst->cfg.font.name);
    if (!inst->fonts[0]) {
	fprintf(stderr, "%s: unable to load font \"%s\"\n", appname,
		inst->cfg.font.name);
	exit(1);
    }
    font_charset = set_font_info(inst, 0);

    if (inst->cfg.shadowbold) {
	inst->fonts[1] = NULL;
    } else {
	if (inst->cfg.boldfont.name[0]) {
	    name = inst->cfg.boldfont.name;
	    guessed = FALSE;
	} else {
	    name = guess_derived_font_name(inst->fonts[0], TRUE, FALSE);
	    guessed = TRUE;
	}
	inst->fonts[1] = name ? gdk_font_load(name) : NULL;
	if (inst->fonts[1]) {
	    set_font_info(inst, 1);
	} else if (!guessed) {
	    fprintf(stderr, "%s: unable to load bold font \"%s\"\n", appname,
		    inst->cfg.boldfont.name);
	    exit(1);
	}
	if (guessed)
	    sfree(name);
    }

    if (inst->cfg.widefont.name[0]) {
	name = inst->cfg.widefont.name;
	guessed = FALSE;
    } else {
	name = guess_derived_font_name(inst->fonts[0], FALSE, TRUE);
	guessed = TRUE;
    }
    inst->fonts[2] = name ? gdk_font_load(name) : NULL;
    if (inst->fonts[2]) {
	set_font_info(inst, 2);
    } else if (!guessed) {
	fprintf(stderr, "%s: unable to load wide font \"%s\"\n", appname,
		inst->cfg.widefont.name);
	exit(1);
    }
    if (guessed)
	sfree(name);

    if (inst->cfg.shadowbold) {
	inst->fonts[3] = NULL;
    } else {
	if (inst->cfg.wideboldfont.name[0]) {
	    name = inst->cfg.wideboldfont.name;
	    guessed = FALSE;
	} else {
	    /*
	     * Here we have some choices. We can widen the bold font,
	     * bolden the wide font, or widen and bolden the standard
	     * font. Try them all, in that order!
	     */
	    if (inst->cfg.widefont.name[0])
		name = guess_derived_font_name(inst->fonts[2], TRUE, FALSE);
	    else if (inst->cfg.boldfont.name[0])
		name = guess_derived_font_name(inst->fonts[1], FALSE, TRUE);
	    else
		name = guess_derived_font_name(inst->fonts[0], TRUE, TRUE);
	    guessed = TRUE;
	}
	inst->fonts[3] = name ? gdk_font_load(name) : NULL;
	if (inst->fonts[3]) {
	    set_font_info(inst, 3);
	} else if (!guessed) {
	    fprintf(stderr, "%s: unable to load wide/bold font \"%s\"\n", appname,
		    inst->cfg.wideboldfont.name);
	    exit(1);
	}
	if (guessed)
	    sfree(name);
    }

    inst->font_width = gdk_char_width(inst->fonts[0], ' ');
    inst->font_height = inst->fonts[0]->ascent + inst->fonts[0]->descent;

    inst->direct_to_font = init_ucs(&inst->ucsdata, inst->cfg.line_codepage,
				    inst->cfg.utf8_override, font_charset,
				    inst->cfg.vtmode);
}

void set_geom_hints(struct gui_data *inst)
{
    GdkGeometry geom;
    geom.min_width = inst->font_width + 2*inst->cfg.window_border;
    geom.min_height = inst->font_height + 2*inst->cfg.window_border;
    geom.max_width = geom.max_height = -1;
    geom.base_width = 2*inst->cfg.window_border;
    geom.base_height = 2*inst->cfg.window_border;
    geom.width_inc = inst->font_width;
    geom.height_inc = inst->font_height;
    geom.min_aspect = geom.max_aspect = 0;
    gtk_window_set_geometry_hints(GTK_WINDOW(inst->window), inst->area, &geom,
                                  GDK_HINT_MIN_SIZE | GDK_HINT_BASE_SIZE |
                                  GDK_HINT_RESIZE_INC);
}

void clear_scrollback_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    term_clrsb(inst->term);
}

void reset_terminal_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    term_pwron(inst->term, TRUE);
    if (inst->ldisc)
	ldisc_send(inst->ldisc, NULL, 0, 0);
}

void copy_all_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    term_copyall(inst->term);
}

void special_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    int code = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(item),
						   "user-data"));

    if (inst->back)
	inst->back->special(inst->backhandle, code);
}

void about_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    about_box(inst->window);
}

void event_log_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    showeventlog(inst->eventlogstuff, inst->window);
}

void change_settings_menuitem(GtkMenuItem *item, gpointer data)
{
    /* This maps colour indices in inst->cfg to those used in inst->cols. */
    static const int ww[] = {
	256, 257, 258, 259, 260, 261,
	0, 8, 1, 9, 2, 10, 3, 11,
	4, 12, 5, 13, 6, 14, 7, 15
    };
    struct gui_data *inst = (struct gui_data *)data;
    char *title = dupcat(appname, " Reconfiguration", NULL);
    Config cfg2, oldcfg;
    int i, need_size;

    assert(lenof(ww) == NCFGCOLOURS);

    if (inst->reconfiguring)
      return;
    else
      inst->reconfiguring = TRUE;

    cfg2 = inst->cfg;                  /* structure copy */

    if (do_config_box(title, &cfg2, 1,
		      inst->back?inst->back->cfg_info(inst->backhandle):0)) {

        oldcfg = inst->cfg;            /* structure copy */
        inst->cfg = cfg2;              /* structure copy */

        /* Pass new config data to the logging module */
        log_reconfig(inst->logctx, &cfg2);
        /*
         * Flush the line discipline's edit buffer in the case
         * where local editing has just been disabled.
         */
        if (inst->ldisc)
	    ldisc_send(inst->ldisc, NULL, 0, 0);
        /* Pass new config data to the terminal */
        term_reconfig(inst->term, &cfg2);
        /* Pass new config data to the back end */
        if (inst->back)
	    inst->back->reconfig(inst->backhandle, &cfg2);

        /*
         * Just setting inst->cfg is sufficient to cause colour
         * setting changes to appear on the next ESC]R palette
         * reset. But we should also check whether any colour
         * settings have been changed, and revert the ones that
         * have to the new default, on the assumption that the user
         * is most likely to want an immediate update.
         */
        for (i = 0; i < NCFGCOLOURS; i++) {
            if (oldcfg.colours[i][0] != cfg2.colours[i][0] ||
                oldcfg.colours[i][1] != cfg2.colours[i][1] ||
                oldcfg.colours[i][2] != cfg2.colours[i][2]) {
                real_palette_set(inst, ww[i], cfg2.colours[i][0],
                                 cfg2.colours[i][1],
                                 cfg2.colours[i][2]);

		/*
		 * If the default background has changed, we must
		 * repaint the space in between the window border
		 * and the text area.
		 */
		if (i == 258) {
		    set_window_background(inst);
		    draw_backing_rect(inst);
		}
	    }
        }

        /*
         * If the scrollbar needs to be shown, hidden, or moved
         * from one end to the other of the window, do so now.
         */
        if (oldcfg.scrollbar != cfg2.scrollbar) {
            if (cfg2.scrollbar)
                gtk_widget_show(inst->sbar);
            else
                gtk_widget_hide(inst->sbar);
        }
        if (oldcfg.scrollbar_on_left != cfg2.scrollbar_on_left) {
            gtk_box_reorder_child(inst->hbox, inst->sbar,
                                  cfg2.scrollbar_on_left ? 0 : 1);
        }

        /*
         * Change the window title, if required.
         */
        if (strcmp(oldcfg.wintitle, cfg2.wintitle))
            set_title(inst, cfg2.wintitle);
	set_window_titles(inst);

        /*
         * Redo the whole tangled fonts and Unicode mess if
         * necessary.
         */
        if (strcmp(oldcfg.font.name, cfg2.font.name) ||
            strcmp(oldcfg.boldfont.name, cfg2.boldfont.name) ||
            strcmp(oldcfg.widefont.name, cfg2.widefont.name) ||
            strcmp(oldcfg.wideboldfont.name, cfg2.wideboldfont.name) ||
            strcmp(oldcfg.line_codepage, cfg2.line_codepage) ||
	    oldcfg.vtmode != cfg2.vtmode ||
	    oldcfg.shadowbold != cfg2.shadowbold) {
            setup_fonts_ucs(inst);
            need_size = 1;
        } else
            need_size = 0;

        /*
         * Resize the window.
         */
        if (oldcfg.width != cfg2.width || oldcfg.height != cfg2.height ||
            oldcfg.window_border != cfg2.window_border || need_size) {
            set_geom_hints(inst);
            request_resize(inst, cfg2.width, cfg2.height);
        } else {
	    /*
	     * The above will have caused a call to term_size() for
	     * us if it happened. If the user has fiddled with only
	     * the scrollback size, the above will not have
	     * happened and we will need an explicit term_size()
	     * here.
	     */
	    if (oldcfg.savelines != cfg2.savelines)
		term_size(inst->term, inst->term->rows, inst->term->cols,
			  cfg2.savelines);
	}

        term_invalidate(inst->term);

	/*
	 * We do an explicit full redraw here to ensure the window
	 * border has been redrawn as well as the text area.
	 */
	gtk_widget_queue_draw(inst->area);
    }
    sfree(title);
    inst->reconfiguring = FALSE;
}

void fork_and_exec_self(struct gui_data *inst, int fd_to_close, ...)
{
    /*
     * Re-execing ourself is not an exact science under Unix. I do
     * the best I can by using /proc/self/exe if available and by
     * assuming argv[0] can be found on $PATH if not.
     * 
     * Note that we also have to reconstruct the elements of the
     * original argv which gtk swallowed, since the user wants the
     * new session to appear on the same X display as the old one.
     */
    char **args;
    va_list ap;
    int i, n;
    int pid;

    /*
     * Collect the arguments with which to re-exec ourself.
     */
    va_start(ap, fd_to_close);
    n = 2;			       /* progname and terminating NULL */
    n += inst->ngtkargs;
    while (va_arg(ap, char *) != NULL)
	n++;
    va_end(ap);

    args = snewn(n, char *);
    args[0] = inst->progname;
    args[n-1] = NULL;
    for (i = 0; i < inst->ngtkargs; i++)
	args[i+1] = inst->gtkargvstart[i];

    i++;
    va_start(ap, fd_to_close);
    while ((args[i++] = va_arg(ap, char *)) != NULL);
    va_end(ap);

    assert(i == n);

    /*
     * Do the double fork.
     */
    pid = fork();
    if (pid < 0) {
	perror("fork");
	return;
    }

    if (pid == 0) {
	int pid2 = fork();
	if (pid2 < 0) {
	    perror("fork");
	    _exit(1);
	} else if (pid2 > 0) {
	    /*
	     * First child has successfully forked second child. My
	     * Work Here Is Done. Note the use of _exit rather than
	     * exit: the latter appears to cause destroy messages
	     * to be sent to the X server. I suspect gtk uses
	     * atexit.
	     */
	    _exit(0);
	}

	/*
	 * If we reach here, we are the second child, so we now
	 * actually perform the exec.
	 */
	if (fd_to_close >= 0)
	    close(fd_to_close);

	execv("/proc/self/exe", args);
	execvp(inst->progname, args);
	perror("exec");
	_exit(127);

    } else {
	int status;
	waitpid(pid, &status, 0);
    }

}

void dup_session_menuitem(GtkMenuItem *item, gpointer gdata)
{
    struct gui_data *inst = (struct gui_data *)gdata;
    /*
     * For this feature we must marshal cfg and (possibly) pty_argv
     * into a byte stream, create a pipe, and send this byte stream
     * to the child through the pipe.
     */
    int i, ret, size;
    char *data;
    char option[80];
    int pipefd[2];

    if (pipe(pipefd) < 0) {
	perror("pipe");
	return;
    }

    size = sizeof(inst->cfg);
    if (use_pty_argv && pty_argv) {
	for (i = 0; pty_argv[i]; i++)
	    size += strlen(pty_argv[i]) + 1;
    }

    data = snewn(size, char);
    memcpy(data, &inst->cfg, sizeof(inst->cfg));
    if (use_pty_argv && pty_argv) {
	int p = sizeof(inst->cfg);
	for (i = 0; pty_argv[i]; i++) {
	    strcpy(data + p, pty_argv[i]);
	    p += strlen(pty_argv[i]) + 1;
	}
	assert(p == size);
    }

    sprintf(option, "---[%d,%d]", pipefd[0], size);
    fcntl(pipefd[0], F_SETFD, 0);
    fork_and_exec_self(inst, pipefd[1], option, NULL);
    close(pipefd[0]);

    i = ret = 0;
    while (i < size && (ret = write(pipefd[1], data + i, size - i)) > 0)
	i += ret;
    if (ret < 0)
	perror("write to pipe");
    close(pipefd[1]);
    sfree(data);
}

int read_dupsession_data(struct gui_data *inst, Config *cfg, char *arg)
{
    int fd, i, ret, size;
    char *data;

    if (sscanf(arg, "---[%d,%d]", &fd, &size) != 2) {
	fprintf(stderr, "%s: malformed magic argument `%s'\n", appname, arg);
	exit(1);
    }

    data = snewn(size, char);
    i = ret = 0;
    while (i < size && (ret = read(fd, data + i, size - i)) > 0)
	i += ret;
    if (ret < 0) {
	perror("read from pipe");
	exit(1);
    } else if (i < size) {
	fprintf(stderr, "%s: unexpected EOF in Duplicate Session data\n",
		appname);
	exit(1);
    }

    memcpy(cfg, data, sizeof(Config));
    if (use_pty_argv && size > sizeof(Config)) {
	int n = 0;
	i = sizeof(Config);
	while (i < size) {
	    while (i < size && data[i]) i++;
	    if (i >= size) {
		fprintf(stderr, "%s: malformed Duplicate Session data\n",
			appname);
		exit(1);
	    }
	    i++;
	    n++;
	}
	pty_argv = snewn(n+1, char *);
	pty_argv[n] = NULL;
	n = 0;
	i = sizeof(Config);
	while (i < size) {
	    char *p = data + i;
	    while (i < size && data[i]) i++;
	    assert(i < size);
	    i++;
	    pty_argv[n++] = dupstr(p);
	}
    }

    return 0;
}

void new_session_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;

    fork_and_exec_self(inst, -1, NULL);
}

void restart_session_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;

    if (!inst->back) {
	logevent(inst, "----- Session restarted -----");
	term_pwron(inst->term, FALSE);
	start_backend(inst);
	inst->exited = FALSE;
    }
}

void saved_session_menuitem(GtkMenuItem *item, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    char *str = (char *)gtk_object_get_data(GTK_OBJECT(item), "user-data");

    fork_and_exec_self(inst, -1, "-load", str, NULL);
}

void saved_session_freedata(GtkMenuItem *item, gpointer data)
{
    char *str = (char *)gtk_object_get_data(GTK_OBJECT(item), "user-data");

    sfree(str);
}

static void update_savedsess_menu(GtkMenuItem *menuitem, gpointer data)
{
    struct gui_data *inst = (struct gui_data *)data;
    struct sesslist sesslist;
    int i;

    gtk_container_foreach(GTK_CONTAINER(inst->sessionsmenu),
			  (GtkCallback)gtk_widget_destroy, NULL);

    get_sesslist(&sesslist, TRUE);
    for (i = 1; i < sesslist.nsessions; i++) {
	GtkWidget *menuitem =
	    gtk_menu_item_new_with_label(sesslist.sessions[i]);
	gtk_container_add(GTK_CONTAINER(inst->sessionsmenu), menuitem);
	gtk_widget_show(menuitem);
	gtk_object_set_data(GTK_OBJECT(menuitem), "user-data",
			    dupstr(sesslist.sessions[i]));
	gtk_signal_connect(GTK_OBJECT(menuitem), "activate",
			   GTK_SIGNAL_FUNC(saved_session_menuitem),
			   inst);
	gtk_signal_connect(GTK_OBJECT(menuitem), "destroy",
			   GTK_SIGNAL_FUNC(saved_session_freedata),
			   inst);
    }
    get_sesslist(&sesslist, FALSE); /* free up */
}

void set_window_icon(GtkWidget *window, const char *const *const *icon,
		     int n_icon)
{
    GdkPixmap *iconpm;
    GdkBitmap *iconmask;
#if GTK_CHECK_VERSION(2,0,0)
    GList *iconlist;
    int n;
#endif

    if (!n_icon)
	return;

    gtk_widget_realize(window);
    iconpm = gdk_pixmap_create_from_xpm_d(window->window, &iconmask,
					  NULL, (gchar **)icon[0]);
    gdk_window_set_icon(window->window, NULL, iconpm, iconmask);

#if GTK_CHECK_VERSION(2,0,0)
    iconlist = NULL;
    for (n = 0; n < n_icon; n++) {
	iconlist =
	    g_list_append(iconlist,
			  gdk_pixbuf_new_from_xpm_data((const gchar **)
						       icon[n]));
    }
    gdk_window_set_icon_list(window->window, iconlist);
#endif
}

void update_specials_menu(void *frontend)
{
    struct gui_data *inst = (struct gui_data *)frontend;

    const struct telnet_special *specials;

    if (inst->back)
	specials = inst->back->get_specials(inst->backhandle);
    else
	specials = NULL;

    /* I believe this disposes of submenus too. */
    gtk_container_foreach(GTK_CONTAINER(inst->specialsmenu),
			  (GtkCallback)gtk_widget_destroy, NULL);
    if (specials) {
	int i;
	GtkWidget *menu = inst->specialsmenu;
	/* A lame "stack" for submenus that will do for now. */
	GtkWidget *saved_menu = NULL;
	int nesting = 1;
	for (i = 0; nesting > 0; i++) {
	    GtkWidget *menuitem = NULL;
	    switch (specials[i].code) {
	      case TS_SUBMENU:
		assert (nesting < 2);
		saved_menu = menu; /* XXX lame stacking */
		menu = gtk_menu_new();
		menuitem = gtk_menu_item_new_with_label(specials[i].name);
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuitem), menu);
		gtk_container_add(GTK_CONTAINER(saved_menu), menuitem);
		gtk_widget_show(menuitem);
		menuitem = NULL;
		nesting++;
		break;
	      case TS_EXITMENU:
		nesting--;
		if (nesting) {
		    menu = saved_menu; /* XXX lame stacking */
		    saved_menu = NULL;
		}
		break;
	      case TS_SEP:
		menuitem = gtk_menu_item_new();
		break;
	      default:
		menuitem = gtk_menu_item_new_with_label(specials[i].name);
		gtk_object_set_data(GTK_OBJECT(menuitem), "user-data",
				    GINT_TO_POINTER(specials[i].code));
		gtk_signal_connect(GTK_OBJECT(menuitem), "activate",
				   GTK_SIGNAL_FUNC(special_menuitem), inst);
		break;
	    }
	    if (menuitem) {
		gtk_container_add(GTK_CONTAINER(menu), menuitem);
		gtk_widget_show(menuitem);
	    }
	}
	gtk_widget_show(inst->specialsitem1);
	gtk_widget_show(inst->specialsitem2);
    } else {
	gtk_widget_hide(inst->specialsitem1);
	gtk_widget_hide(inst->specialsitem2);
    }
}

static void start_backend(struct gui_data *inst)
{
    extern Backend *select_backend(Config *cfg);
    char *realhost;
    const char *error;

    inst->back = select_backend(&inst->cfg);

    error = inst->back->init((void *)inst, &inst->backhandle,
			     &inst->cfg, inst->cfg.host, inst->cfg.port,
			     &realhost, inst->cfg.tcp_nodelay,
			     inst->cfg.tcp_keepalives);

    if (error) {
	char *msg = dupprintf("Unable to open connection to %s:\n%s",
			      inst->cfg.host, error);
	inst->exited = TRUE;
	fatal_message_box(inst->window, msg);
	sfree(msg);
	exit(0);
    }

    if (inst->cfg.wintitle[0]) {
	set_title(inst, inst->cfg.wintitle);
	set_icon(inst, inst->cfg.wintitle);
    } else {
	char *title = make_default_wintitle(realhost);
	set_title(inst, title);
	set_icon(inst, title);
	sfree(title);
    }
    sfree(realhost);

    inst->back->provide_logctx(inst->backhandle, inst->logctx);

    term_provide_resize_fn(inst->term, inst->back->size, inst->backhandle);

    inst->ldisc =
	ldisc_create(&inst->cfg, inst->term, inst->back, inst->backhandle,
		     inst);

    gtk_widget_hide(inst->restartitem);
}

int pt_main(int argc, char **argv)
{
    extern int cfgbox(Config *cfg);
    struct gui_data *inst;

    /*
     * Create an instance structure and initialise to zeroes
     */
    inst = snew(struct gui_data);
    memset(inst, 0, sizeof(*inst));
    inst->alt_keycode = -1;            /* this one needs _not_ to be zero */
    inst->busy_status = BUSY_NOT;

    /* defer any child exit handling until we're ready to deal with
     * it */
    block_signal(SIGCHLD, 1);

    inst->progname = argv[0];
    /*
     * Copy the original argv before letting gtk_init fiddle with
     * it. It will be required later.
     */
    {
	int i, oldargc;
	inst->gtkargvstart = snewn(argc-1, char *);
	for (i = 1; i < argc; i++)
	    inst->gtkargvstart[i-1] = dupstr(argv[i]);
	oldargc = argc;
	gtk_init(&argc, &argv);
	inst->ngtkargs = oldargc - argc;
    }

    if (argc > 1 && !strncmp(argv[1], "---", 3)) {
	read_dupsession_data(inst, &inst->cfg, argv[1]);
	/* Splatter this argument so it doesn't clutter a ps listing */
	memset(argv[1], 0, strlen(argv[1]));
    } else {
	/* By default, we bring up the config dialog, rather than launching
	 * a session. This gets set to TRUE if something happens to change
	 * that (e.g., a hostname is specified on the command-line). */
	int allow_launch = FALSE;
	if (do_cmdline(argc, argv, 0, &allow_launch, inst, &inst->cfg))
	    exit(1);		       /* pre-defaults pass to get -class */
	do_defaults(NULL, &inst->cfg);
	if (do_cmdline(argc, argv, 1, &allow_launch, inst, &inst->cfg))
	    exit(1);		       /* post-defaults, do everything */

	cmdline_run_saved(&inst->cfg);

	if (loaded_session)
	    allow_launch = TRUE;

	if ((!allow_launch || !cfg_launchable(&inst->cfg)) &&
	    !cfgbox(&inst->cfg))
	    exit(0);		       /* config box hit Cancel */
    }

    if (!compound_text_atom)
        compound_text_atom = gdk_atom_intern("COMPOUND_TEXT", FALSE);
    if (!utf8_string_atom)
        utf8_string_atom = gdk_atom_intern("UTF8_STRING", FALSE);

    setup_fonts_ucs(inst);
    init_cutbuffers();

    inst->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    /*
     * Set up the colour map.
     */
    palette_reset(inst);

    inst->width = inst->cfg.width;
    inst->height = inst->cfg.height;

    inst->area = gtk_drawing_area_new();
    gtk_drawing_area_size(GTK_DRAWING_AREA(inst->area),
			  inst->font_width * inst->cfg.width + 2*inst->cfg.window_border,
			  inst->font_height * inst->cfg.height + 2*inst->cfg.window_border);
    inst->sbar_adjust = GTK_ADJUSTMENT(gtk_adjustment_new(0,0,0,0,0,0));
    inst->sbar = gtk_vscrollbar_new(inst->sbar_adjust);
    inst->hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
    /*
     * We always create the scrollbar; it remains invisible if
     * unwanted, so we can pop it up quickly if it suddenly becomes
     * desirable.
     */
    if (inst->cfg.scrollbar_on_left)
        gtk_box_pack_start(inst->hbox, inst->sbar, FALSE, FALSE, 0);
    gtk_box_pack_start(inst->hbox, inst->area, TRUE, TRUE, 0);
    if (!inst->cfg.scrollbar_on_left)
        gtk_box_pack_start(inst->hbox, inst->sbar, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(inst->window), GTK_WIDGET(inst->hbox));

    set_geom_hints(inst);

    gtk_widget_show(inst->area);
    if (inst->cfg.scrollbar)
	gtk_widget_show(inst->sbar);
    else
	gtk_widget_hide(inst->sbar);
    gtk_widget_show(GTK_WIDGET(inst->hbox));

    if (inst->gotpos) {
        int x = inst->xpos, y = inst->ypos;
        GtkRequisition req;
        gtk_widget_size_request(GTK_WIDGET(inst->window), &req);
        if (inst->gravity & 1) x += gdk_screen_width() - req.width;
        if (inst->gravity & 2) y += gdk_screen_height() - req.height;
	gtk_window_set_position(GTK_WINDOW(inst->window), GTK_WIN_POS_NONE);
	gtk_widget_set_uposition(GTK_WIDGET(inst->window), x, y);
    }

    gtk_signal_connect(GTK_OBJECT(inst->window), "destroy",
		       GTK_SIGNAL_FUNC(destroy), inst);
    gtk_signal_connect(GTK_OBJECT(inst->window), "delete_event",
		       GTK_SIGNAL_FUNC(delete_window), inst);
    gtk_signal_connect(GTK_OBJECT(inst->window), "key_press_event",
		       GTK_SIGNAL_FUNC(key_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->window), "key_release_event",
		       GTK_SIGNAL_FUNC(key_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->window), "focus_in_event",
		       GTK_SIGNAL_FUNC(focus_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->window), "focus_out_event",
		       GTK_SIGNAL_FUNC(focus_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "configure_event",
		       GTK_SIGNAL_FUNC(configure_area), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "expose_event",
		       GTK_SIGNAL_FUNC(expose_area), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "button_press_event",
		       GTK_SIGNAL_FUNC(button_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "button_release_event",
		       GTK_SIGNAL_FUNC(button_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "motion_notify_event",
		       GTK_SIGNAL_FUNC(motion_event), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "selection_received",
		       GTK_SIGNAL_FUNC(selection_received), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "selection_get",
		       GTK_SIGNAL_FUNC(selection_get), inst);
    gtk_signal_connect(GTK_OBJECT(inst->area), "selection_clear_event",
		       GTK_SIGNAL_FUNC(selection_clear), inst);
    if (inst->cfg.scrollbar)
	gtk_signal_connect(GTK_OBJECT(inst->sbar_adjust), "value_changed",
			   GTK_SIGNAL_FUNC(scrollbar_moved), inst);
    gtk_widget_add_events(GTK_WIDGET(inst->area),
			  GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
			  GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
			  GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK);

    {
	extern const char *const *const main_icon[];
	extern const int n_main_icon;
	set_window_icon(inst->window, main_icon, n_main_icon);
    }

    gtk_widget_show(inst->window);

    set_window_background(inst);

    /*
     * Set up the Ctrl+rightclick context menu.
     */
    {
	GtkWidget *menuitem;
	char *s;
	extern const int use_event_log, new_session, saved_sessions;

	inst->menu = gtk_menu_new();

#define MKMENUITEM(title, func) do { \
    menuitem = title ? gtk_menu_item_new_with_label(title) : \
    gtk_menu_item_new(); \
    gtk_container_add(GTK_CONTAINER(inst->menu), menuitem); \
    gtk_widget_show(menuitem); \
    if (func != NULL) \
	gtk_signal_connect(GTK_OBJECT(menuitem), "activate", \
			       GTK_SIGNAL_FUNC(func), inst); \
} while (0)
	if (new_session)
	    MKMENUITEM("New Session", new_session_menuitem);
        MKMENUITEM("Restart Session", restart_session_menuitem);
	inst->restartitem = menuitem;
	gtk_widget_hide(inst->restartitem);
        MKMENUITEM("Duplicate Session", dup_session_menuitem);
	if (saved_sessions) {
	    inst->sessionsmenu = gtk_menu_new();
	    /* sessionsmenu will be updated when it's invoked */
	    /* XXX is this the right way to do dynamic menus in Gtk? */
	    MKMENUITEM("Saved Sessions", update_savedsess_menu);
	    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuitem),
				      inst->sessionsmenu);
	}
	MKMENUITEM(NULL, NULL);
        MKMENUITEM("Change Settings", change_settings_menuitem);
	MKMENUITEM(NULL, NULL);
	if (use_event_log)
	    MKMENUITEM("Event Log", event_log_menuitem);
	MKMENUITEM("Special Commands", NULL);
	inst->specialsmenu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuitem), inst->specialsmenu);
	inst->specialsitem1 = menuitem;
	MKMENUITEM(NULL, NULL);
	inst->specialsitem2 = menuitem;
	gtk_widget_hide(inst->specialsitem1);
	gtk_widget_hide(inst->specialsitem2);
	MKMENUITEM("Clear Scrollback", clear_scrollback_menuitem);
	MKMENUITEM("Reset Terminal", reset_terminal_menuitem);
	MKMENUITEM("Copy All", copy_all_menuitem);
	MKMENUITEM(NULL, NULL);
	s = dupcat("About ", appname, NULL);
	MKMENUITEM(s, about_menuitem);
	sfree(s);
#undef MKMENUITEM
    }

    inst->textcursor = make_mouse_ptr(inst, GDK_XTERM);
    inst->rawcursor = make_mouse_ptr(inst, GDK_LEFT_PTR);
    inst->waitcursor = make_mouse_ptr(inst, GDK_WATCH);
    inst->blankcursor = make_mouse_ptr(inst, -1);
    make_mouse_ptr(inst, -2);	       /* clean up cursor font */
    inst->currcursor = inst->textcursor;
    show_mouseptr(inst, 1);

    inst->eventlogstuff = eventlogstuff_new();

    inst->term = term_init(&inst->cfg, &inst->ucsdata, inst);
    inst->logctx = log_init(inst, &inst->cfg);
    term_provide_logctx(inst->term, inst->logctx);

    uxsel_init();

    term_size(inst->term, inst->cfg.height, inst->cfg.width, inst->cfg.savelines);

    start_backend(inst);

    ldisc_send(inst->ldisc, NULL, 0, 0);/* cause ldisc to notice changes */

    /* now we're reday to deal with the child exit handler being
     * called */
    block_signal(SIGCHLD, 0);

    /*
     * Block SIGPIPE: if we attempt Duplicate Session or similar
     * and it falls over in some way, we certainly don't want
     * SIGPIPE terminating the main pterm/PuTTY. Note that we do
     * this _after_ (at least pterm) forks off its child process,
     * since the child wants SIGPIPE handled in the usual way.
     */
    block_signal(SIGPIPE, 1);

    inst->exited = FALSE;

    gtk_main();

    return 0;
}
