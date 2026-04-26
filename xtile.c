#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
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

struct Dimensions {
  int width;
  int height;
};

static struct XContext x;
static struct Client *clients = NULL;
static struct Client *sel = NULL;
static struct Dimensions dim;
static volatile sig_atomic_t running = 1;

static void initlocale(void);
static void setup(void);
static void run(void);
static void cleanup(void);
static void checkconflicts(void);
static void focus(struct Client *c);
static void addclient(Window w);
static void removeclient(Window w);
static void arrange(void);
static void scan(void);
static struct Client *getclient(Window w);

static struct Client *getclient(Window w) {
  for (struct Client *c = clients; c; c = c->next)
    if (c->win == w)
      return c;
  return NULL;
}

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

  dim.width = DisplayWidth(x.dpy, x.screen);
  dim.height = DisplayHeight(x.dpy, x.screen);
}

static void checkconflicts(void) {
  XSetErrorHandler(xerrorstart);
  XSelectInput(x.dpy, x.root,
               SubstructureRedirectMask | SubstructureNotifyMask |
                   ButtonPressMask | PointerMotionMask);
  XSync(x.dpy, False);
  XSetErrorHandler(NULL);
}

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

static void focus(struct Client *c) {
  if (!c)
    return;
  sel = c;
  XSetInputFocus(x.dpy, c->win, RevertToPointerRoot, CurrentTime);

  for (struct Client *it = clients; it; it = it->next)
    XSetWindowBorder(x.dpy, it->win, (it == sel) ? 0xff0000 : 0x222222);
  XRaiseWindow(x.dpy, c->win);
}

static void addclient(Window w) {
  struct Client *c = ecalloc(1, sizeof(struct Client));
  c->win = w;
  c->next = clients;
  clients = c;
  XSetWindowBorderWidth(x.dpy, c->win, borderwidth);
  XSelectInput(x.dpy, w, EnterWindowMask | FocusChangeMask);
}

static void scan(void) {
  Window root, parent, *wins;
  unsigned int nwins;

  if (XQueryTree(x.dpy, x.root, &root, &parent, &wins, &nwins)) {
    for (unsigned int i = 0; i < nwins; i++) {
      XWindowAttributes wa;

      if (!XGetWindowAttributes(x.dpy, wins[i], &wa) || wa.override_redirect ||
          wa.map_state != IsViewable)
        continue;

      if (!getclient(wins[i]))
        addclient(wins[i]);
    }

    if (wins)
      XFree(wins);
  }
}

static void removeclient(Window w) {
  struct Client **tc = &clients;
  while (*tc) {
    if ((*tc)->win == w) {
      struct Client *tmp = *tc;
      *tc = (*tc)->next;

      if (sel == tmp)
        sel = clients;

      free(tmp);
      break;
    }
    tc = &(*tc)->next;
  }
  if (clients)
    focus(clients);
  else
    sel = NULL;
}

static void arrange(void) {
  int n = 0;
  for (struct Client *c = clients; c; c = c->next)
    n++;

  if (!n)
    return;

  int i = 0;
  for (struct Client *c = clients; c; c = c->next) {
    int wx = (i % 2) * (dim.width / 2);
    int wy = (i / 2) * (dim.height / (n / 2 + 1));

    XMoveResizeWindow(x.dpy, c->win, wx, wy, dim.width / 2,
                      dim.height / (n / 2 + 1));
    i++;
  }
}

static void run(void) {
  XEvent e;

  while (running) {
    XNextEvent(x.dpy, &e);

    switch (e.type) {

    case ButtonPress: {
      XButtonEvent *ev = &e.xbutton;
      struct Client *c = getclient(ev->window);
      if (c) {
        focus(c);
        XSync(x.dpy, False);
      }

      XAllowEvents(x.dpy, ReplayPointer, CurrentTime);
    }; break;

    case MapRequest: {
      XMapRequestEvent *ev = &e.xmaprequest;
      XWindowAttributes wa;

      if (!XGetWindowAttributes(x.dpy, ev->window, &wa) || wa.override_redirect)
        break;
      if (!getclient(ev->window))
        addclient(ev->window);
      XMapWindow(x.dpy, ev->window);
      focus(getclient(ev->window));
      arrange();
      XSync(x.dpy, False);
    }; break;

    case ConfigureRequest: {
      XConfigureRequestEvent *ev = &e.xconfigurerequest;

      if (getclient(ev->window)) {
        XWindowChanges wc;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;

        XConfigureWindow(
            x.dpy, ev->window,
            ev->value_mask & (CWBorderWidth | CWSibling | CWStackMode), &wc);
      } else {
        XWindowChanges wc;
        wc.x = ev->x;
        wc.y = ev->y;
        wc.width = ev->width;
        wc.height = ev->height;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;

        XConfigureWindow(x.dpy, ev->window, ev->value_mask, &wc);
      }
    }; break;

    case DestroyNotify: {
      removeclient(e.xdestroywindow.window);
      arrange();
    } break;

    case UnmapNotify: {
      removeclient(e.xunmap.window);
      arrange();
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
  scan();
  if (clients)
    focus(clients);
  arrange();

#ifdef __OpenBSD__
  if (pledge("stdio rpath proc", NULL) == -1)
    die("pledge:");
#endif

  run();
  cleanup();

  return EXIT_SUCCESS;
}
