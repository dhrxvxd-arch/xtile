#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define VERSION "0.1.0"

struct XContext {
  Display *dpy;
  Window root;
  int screen;
  Cursor cursor;
};

struct Client {
  Window win;
  struct Client *next;
};

static struct XContext x;
static struct Client *clients = NULL;
static volatile sig_atomic_t running = 1;

static void initlocale(void);
static void setup(void);
static void run(void);
static void cleanup(void);
static void checkconflicts(void);
static void addclient(Window w);
static void removeclient(Window w);

static void initlocale(void) {
  if (!setlocale(LC_CTYPE, "")) {
    fputs("warning: cannot set locale\n", stderr);
    return;
  }
  if (!XSupportsLocale())
    fputs("warning: X does not support locale\n", stderr);
  if (!XSetLocaleModifiers(""))
    fputs("warning: cannot set locale modifiers\n", stderr);
}

static int xerrorstart(Display *dpy, XErrorEvent *ee) {
  (void)dpy;
  (void)ee;
  die("xtile: another window manager is already running");
  return -1;
}

static void setup(void) {
  x.screen = DefaultScreen(x.dpy);
  x.root = RootWindow(x.dpy, x.screen);

  x.cursor = XCreateFontCursor(x.dpy, XC_left_ptr);
  XDefineCursor(x.dpy, x.root, x.cursor);
}

static void checkconflicts(void) {
  XSetErrorHandler(xerrorstart);
  XSelectInput(x.dpy, x.root,
               SubstructureRedirectMask | SubstructureNotifyMask);
  XSync(x.dpy, False);
  XSetErrorHandler(NULL);
}

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

static void addclient(Window w) {
  struct Client *c = ecalloc(1, sizeof(struct Client));
  c->win = w;
  c->next = clients;
  clients = c;
}

static void removeclient(Window w) {
  struct Client **tc = &clients;

  while (*tc) {
    if ((*tc)->win == w) {
      struct Client *tmp = *tc;
      *tc = (*tc)->next;
      free(tmp);
      return;
    }
    tc = &(*tc)->next;
  }
}

static void run(void) {
  XEvent e;

  while (running) {
    XNextEvent(x.dpy, &e);

    switch (e.type) {

    case ButtonPress: {
      XAllowEvents(x.dpy, ReplayPointer, CurrentTime);
    }; break;

    case MapRequest: {
      XMapRequestEvent *ev = &e.xmaprequest;
      addclient(ev->window);
      XMapWindow(x.dpy, ev->window);
    }; break;

    case ConfigureRequest: {
      XConfigureRequestEvent *ev = &e.xconfigurerequest;

      XWindowChanges wc;
      wc.x = ev->x;
      wc.y = ev->y;
      wc.width = ev->width;
      wc.height = ev->height;
      wc.border_width = ev->border_width;
      wc.sibling = ev->above;
      wc.stack_mode = ev->detail;

      XConfigureWindow(x.dpy, ev->window, ev->value_mask, &wc);
    }; break;

    case DestroyNotify: {
      removeclient(e.xdestroywindow.window);
    } break;

    default:
      break;
    }
  }
}

static void cleanup(void) {
  if (x.dpy) {
    if (x.cursor)
      XFreeCursor(x.dpy, x.cursor);

    XCloseDisplay(x.dpy);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp("-v", argv[1])) {
    puts("xtile-" VERSION);
    return 0;
  } else if (argc != 1) {
    die("usage: xtile [-v]");
  }

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  initlocale();

  if (!(x.dpy = XOpenDisplay(NULL)))
    die("xtile: cannot open display");

  setup();
  checkconflicts();

#ifdef __OpenBSD__
  if (pledge("stdio rpath proc", NULL) == -1)
    die("pledge:");
#endif

  run();
  cleanup();

  return EXIT_SUCCESS;
}
