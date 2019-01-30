#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <math.h>
#include <signal.h>
#include <inttypes.h>
#include <ctype.h>
#include <locale.h>
#include <wchar.h>
#include <setjmp.h>
#include <pty.h>

#include <sys/time.h>
#include <sys/ioctl.h>

#include <linux/input.h>

#include <cairo.h>
#include <libudev.h>
#include <libinput.h>
#include <xkbcommon/xkbcommon.h>

#include "fbtools.h"
#include "drmtools.h"
#include "tmt.h"

static gfxstate *gfx;
static cairo_surface_t *surface1;
static cairo_surface_t *surface2;
static TMT *vt;
static int dirty;
static struct udev *udev;
static struct libinput *kbd;

static struct xkb_context *xkb;
static struct xkb_keymap *map;
static struct xkb_state *state;
static struct xkb_rule_names layout = {
    .rules   = NULL,
    .model   = "pc105",
    .layout  = "us",
    .variant = NULL,
    .options = NULL,
};

/* ---------------------------------------------------------------------- */

static jmp_buf fb_fatal_cleanup;

static void catch_exit_signal(int signal)
{
    siglongjmp(fb_fatal_cleanup,signal);
}

static void exit_signals_init(void)
{
    struct sigaction act,old;
    int termsig;

    memset(&act,0,sizeof(act));
    act.sa_handler = catch_exit_signal;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act,&old);
    sigaction(SIGQUIT,&act,&old);
    sigaction(SIGTERM,&act,&old);

    sigaction(SIGABRT,&act,&old);
    sigaction(SIGTSTP,&act,&old);

    sigaction(SIGBUS, &act,&old);
    sigaction(SIGILL, &act,&old);
    sigaction(SIGSEGV,&act,&old);

    if (0 == (termsig = sigsetjmp(fb_fatal_cleanup,0)))
	return;

    /* cleanup */
    gfx->cleanup_display();
    fprintf(stderr,"Oops: %s\n",strsignal(termsig));
    exit(42);
}

static void cleanup_and_exit(int code)
{
    gfx->cleanup_display();
    exit(code);
}

/* ---------------------------------------------------------------------- */

static int open_restricted(const char *path, int flags, void *user_data)
{
    int fd;

    fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return fd;
    }

    ioctl(fd, EVIOCGRAB, 1);
    return fd;
}

static void close_restricted(int fd, void *user_data)
{
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
}

static const struct libinput_interface interface = {
    .open_restricted  = open_restricted,
    .close_restricted = close_restricted,
};

/* ---------------------------------------------------------------------- */

struct color {
    float r;
    float g;
    float b;
};

static struct color tmt_colors[] = {
    /* normal */
    [ TMT_COLOR_BLACK   ] = { .r = 0.0, .g = 0.0, .b = 0.0 },
    [ TMT_COLOR_RED     ] = { .r = 0.7, .g = 0.0, .b = 0.0 },
    [ TMT_COLOR_GREEN   ] = { .r = 0.0, .g = 0.7, .b = 0.0 },
    [ TMT_COLOR_YELLOW  ] = { .r = 0.7, .g = 0.7, .b = 0.0 },
    [ TMT_COLOR_BLUE    ] = { .r = 0.0, .g = 0.0, .b = 0.7 },
    [ TMT_COLOR_MAGENTA ] = { .r = 0.7, .g = 0.0, .b = 0.7 },
    [ TMT_COLOR_CYAN    ] = { .r = 0.0, .g = 0.7, .b = 0.7 },
    [ TMT_COLOR_WHITE   ] = { .r = 0.7, .g = 0.7, .b = 0.7 },
    /* bold */
    [ TMT_COLOR_BLACK   + 8 ] = { .r = 0.3, .g = 0.3, .b = 0.3 },
    [ TMT_COLOR_RED     + 8 ] = { .r = 1.0, .g = 0.3, .b = 0.3 },
    [ TMT_COLOR_GREEN   + 8 ] = { .r = 0.3, .g = 1.0, .b = 0.3 },
    [ TMT_COLOR_YELLOW  + 8 ] = { .r = 1.0, .g = 1.0, .b = 0.3 },
    [ TMT_COLOR_BLUE    + 8 ] = { .r = 0.3, .g = 0.3, .b = 1.0 },
    [ TMT_COLOR_MAGENTA + 8 ] = { .r = 1.0, .g = 0.3, .b = 1.0 },
    [ TMT_COLOR_CYAN    + 8 ] = { .r = 0.3, .g = 1.0, .b = 1.0 },
    [ TMT_COLOR_WHITE   + 8 ] = { .r = 1.0, .g = 1.0, .b = 1.0 },
};

struct color *tmt_foreground(struct TMTATTRS *a)
{
    int fg = a->fg;

    if (fg == TMT_COLOR_DEFAULT)
        fg = TMT_COLOR_WHITE;
    if (a->bold)
        fg += 8;
    return tmt_colors + fg;
}

struct color *tmt_background(struct TMTATTRS *a)
{
    int bg = a->bg;

    if (bg == TMT_COLOR_DEFAULT)
       bg = TMT_COLOR_BLACK;
    return tmt_colors + bg;
}

static void render(void)
{
    static bool second;
    const TMTSCREEN *s = tmt_screen(vt);
    const TMTPOINT *cursor = tmt_cursor(vt);
    cairo_t *context;
    cairo_font_extents_t extents;
    int line, col, tx, ty;
    wchar_t ws[2];
    char utf8[10];

    if (surface2)
        second = !second;
    context = cairo_create(second ? surface2 : surface1);

    cairo_select_font_face(context, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 16);
    cairo_font_extents(context, &extents);
    tx = (gfx->hdisplay - (extents.max_x_advance * s->ncol)) / 2;
    ty = (gfx->vdisplay - (extents.height * s->nline)) / 2;

    cairo_set_source_rgb(context, 0, 0, 0);
    cairo_paint(context);

    for (line = 0; line < s->nline; line++) {
        for (col = 0; col < s->ncol; col++) {
            TMTCHAR *c = &s->lines[line]->chars[col];
            struct color *bg = tmt_background(&c->a);
            struct color *fg = tmt_foreground(&c->a);

            if (cursor->r == line && cursor->c == col) {
                bg = tmt_colors + TMT_COLOR_WHITE;
                fg = tmt_colors + TMT_COLOR_BLACK;
            }

            /* background */
            cairo_rectangle(context,
                            tx + col * extents.max_x_advance,
                            ty + line * extents.height,
                            extents.max_x_advance,
                            extents.height);
            cairo_set_source_rgb(context, bg->r, bg->g, bg->b);
            cairo_fill(context);

            /* char */
            cairo_move_to(context,
                          tx + col * extents.max_x_advance,
                          ty + line * extents.height + extents.ascent);
            cairo_set_source_rgb(context, fg->r, fg->g, fg->b);
            ws[0] = c->c;
            ws[1] = 0;
            wcstombs(utf8, ws, sizeof(utf8));
            cairo_show_text(context, utf8);
        }
    }

    cairo_show_page(context);
    cairo_destroy(context);

    if (gfx->flush_display)
        gfx->flush_display(second);
}

void tmt_callback(tmt_msg_t m, TMT *vt, const void *a, void *p)
{
    switch (m) {
    case TMT_MSG_UPDATE:
        dirty++;
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    pid_t child;
    int pty, input;

    setlocale(LC_ALL,"");

    /* init drm */
    gfx = drm_init(NULL, NULL, NULL, true);
    if (!gfx) {
        fprintf(stderr, "drm init failed\n");
        exit(1);
    }
    exit_signals_init();
    signal(SIGTSTP,SIG_IGN);

    /* init cairo */
    surface1 = cairo_image_surface_create_for_data(gfx->mem,
                                                   CAIRO_FORMAT_ARGB32,
                                                   gfx->hdisplay,
                                                   gfx->vdisplay,
                                                   gfx->stride);
    if (gfx->mem2) {
        surface2 = cairo_image_surface_create_for_data(gfx->mem2,
                                                       CAIRO_FORMAT_ARGB32,
                                                       gfx->hdisplay,
                                                       gfx->vdisplay,
                                                       gfx->stride);
    }

    /* init udev + libinput */
    udev = udev_new();
    kbd = libinput_udev_create_context(&interface, NULL, udev);
    libinput_udev_assign_seat(kbd, "seat0");
    input = libinput_get_fd(kbd);

    /* init udev + xkbcommon */
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    map = xkb_keymap_new_from_names(xkb, &layout, XKB_KEYMAP_COMPILE_NO_FLAGS);
    state = xkb_state_new(map);

    vt = tmt_open(25, 80, tmt_callback, NULL, NULL);
    child = forkpty(&pty, NULL, NULL, NULL);
    if (0 == child) {
        /* child */
        setenv("TERM", "ansi", true);
        setenv("LINES", "25", true);
        setenv("COLUMNS", "80", true);
        execl("/bin/sh", "-sh", NULL);
        fprintf(stderr, "failed to exec /bin/sh: %s\n", strerror(errno));
        sleep(3);
        exit(0);
    }
    dirty++;

    for (;;) {
        fd_set set;
        int rc, max;

        if (dirty)
            render();

        max = 0;
        FD_ZERO(&set);
        FD_SET(pty, &set);
        if (max < pty)
            max = pty;
        FD_SET(input, &set);
        if (max < input)
            max = input;

        rc = select(max+ 1, &set, NULL, NULL, NULL);
        if (rc < 0)
            break;

        if (FD_ISSET(pty, &set)) {
            char buf[1024];
            rc = read(pty, buf, sizeof(buf));
            if (rc < 0 && errno != EAGAIN && errno != EINTR)
                break; /* read error */
            if (rc == 0)
                break; /* no data -> EOF */
            if (rc > 0)
                tmt_write(vt, buf, rc);
        }

        if (FD_ISSET(input, &set)) {
            struct libinput_event *evt;
            struct libinput_event_keyboard *kevt;
            xkb_keycode_t key;
            bool down;
            char buf[32];

            rc = libinput_dispatch(kbd);
            if (rc < 0)
                break;
            while ((evt = libinput_get_event(kbd)) != NULL) {
                switch (libinput_event_get_type(evt)) {
                case LIBINPUT_EVENT_KEYBOARD_KEY:
                    kevt = libinput_event_get_keyboard_event(evt);
                    key = libinput_event_keyboard_get_key(kevt) + 8;
                    down = libinput_event_keyboard_get_key_state(kevt);
                    xkb_state_update_key(state, key, down);
                    if (down) {
                        rc = xkb_state_key_get_utf8(state, key,
                                                    buf, sizeof(buf));
                        if (rc > 0)
                            write(pty, buf, rc);
                    }
                    break;
                default:
                    /* ignore event */
                    break;
                }
                libinput_event_destroy(evt);
            }
        }
    }

    cleanup_and_exit(0);
    return 0;
}
