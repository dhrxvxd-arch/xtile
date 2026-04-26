#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define MODMASK Mod4Mask
#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))
#define CLEANMASK(mask) ((mask) & ~(LockMask))

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_FMT(a, b)
#endif

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

union Key {
  int i;
  unsigned int ui;
  float f;
  const void *v;
};

struct KeyGr {
  unsigned int mod;
  KeySym keysym;
  KeyCode keycode;
  void (*func)(const union Key *);
  const union Key key;
};

static const unsigned short borderwidth = 2;
static const char *termcmd[] = {"kitty", NULL};

static struct XContext x;
static struct Client *clients;
static struct Client *sel;
static struct Dimensions dim;
static volatile sig_atomic_t running = 1;

_Noreturn void die(const char *fmt, ...) PRINTF_FMT(1, 2);
void *ecalloc(size_t nmemb, size_t size);

static void initlocale(void);
static void setup(void);
static void run(void);
static void cleanup(void);
static void checkconflicts(void);
static void focus(struct Client *c);
static void addclient(Window w);
static void spawn(const union Key *key);
static void killclient(const union Key *k);
static void quit(const union Key *k);
static void focusnext(const union Key *k);
static void removeclient(Window w);
static void arrange(void);
static void scan(void);
static struct Client *getclient(Window w);

static struct KeyGr keys[] = {
    {MODMASK, XK_Return, 0, spawn, {.v = termcmd}},
    {MODMASK, XK_q, 0, quit, {0}},
    {MODMASK, XK_w, 0, killclient, {0}},
    {MODMASK, XK_j, 0, focusnext, {0}},
};

_Noreturn void die(const char *fmt, ...) {
  va_list ap;
  int saved_errno;

  saved_errno = errno;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
    fprintf(stderr, " %s", strerror(saved_errno));

  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

void *ecalloc(size_t nmemb, size_t size) {
  void *p;

  p = calloc(nmemb, size);
  if (!p)
    die("calloc:");
  return p;
}

static void spawn(const union Key *key) {
  pid_t pid;
  struct sigaction sa;
  char **argv;

  if (!key || !key->v)
    return;

  pid = fork();
  if (pid == 0) {
    if (x.dpy)
      close(ConnectionNumber(x.dpy));
    setsid();

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    argv = (char **)key->v;
    execvp(argv[0], argv);
    _exit(EXIT_FAILURE);
  } else if (pid < 0) {
    die("xtile: fork failed");
  }
}

static void killclient(const union Key *k) {
  (void)k;

  if (!sel)
    return;

  XKillClient(x.dpy, sel->win);
}

static void focusnext(const union Key *k) {
  (void)k;

  if (!sel || !clients)
    return;

  if (sel->next)
    focus(sel->next);
  else
    focus(clients);
}

static void quit(const union Key *k) {
  (void)k;
  running = 0;
}

static struct Client *getclient(Window w) {
  struct Client *c;

  for (c = clients; c; c = c->next)
    if (c->win == w)
      return c;

  return NULL;
}

static void initlocale(void) {
  if (!setlocale(LC_CTYPE, ""))
    fputs("warning: cannot set locale\n", stderr);
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
  size_t i;

  x.screen = DefaultScreen(x.dpy);
  x.root = RootWindow(x.dpy, x.screen);
  x.cursor = XCreateFontCursor(x.dpy, XC_left_ptr);
  XDefineCursor(x.dpy, x.root, x.cursor);

  dim.width = DisplayWidth(x.dpy, x.screen);
  dim.height = DisplayHeight(x.dpy, x.screen);

  for (i = 0; i < LENGTH(keys); i++) {
    keys[i].keycode = XKeysymToKeycode(x.dpy, keys[i].keysym);
    XGrabKey(x.dpy, keys[i].keycode, keys[i].mod, x.root, True, GrabModeAsync,
             GrabModeAsync);
  }
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
  struct Client *it;

  if (!c) {
    sel = NULL;
    XSetInputFocus(x.dpy, x.root, RevertToPointerRoot, CurrentTime);
    return;
  }

  sel = c;
  XSetInputFocus(x.dpy, c->win, RevertToPointerRoot, CurrentTime);

  for (it = clients; it; it = it->next)
    XSetWindowBorder(x.dpy, it->win, (it == sel) ? 0xff0000 : 0x222222);

  XRaiseWindow(x.dpy, c->win);
}

static void addclient(Window w) {
  struct Client *c;

  c = ecalloc(1, sizeof(struct Client));
  c->win = w;
  c->next = clients;
  clients = c;

  XSetWindowBorderWidth(x.dpy, c->win, borderwidth);
  XSelectInput(x.dpy, w, EnterWindowMask | FocusChangeMask);
}

static void scan(void) {
  Window root, parent, *wins;
  unsigned int nwins, i;
  XWindowAttributes wa;

  if (XQueryTree(x.dpy, x.root, &root, &parent, &wins, &nwins)) {
    for (i = 0; i < nwins; i++) {
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
  struct Client **tc;
  struct Client *tmp;

  tc = &clients;
  while (*tc) {
    if ((*tc)->win == w) {
      tmp = *tc;
      *tc = (*tc)->next;
      if (sel == tmp)
        sel = clients;
      free(tmp);
      break;
    }
    tc = &(*tc)->next;
  }

  if (sel)
    focus(sel);
  else
    XSetInputFocus(x.dpy, x.root, RevertToPointerRoot, CurrentTime);
}

static void arrange(void) {
  struct Client *c;
  int n, i, master_w, stack_w, stack_h, y, h;

  if (!clients)
    return;

  n = 0;
  for (c = clients; c; c = c->next)
    n++;

  if (!n)
    return;

  master_w = (n > 1) ? (int)(dim.width * 0.6) : dim.width;
  stack_w = dim.width - master_w;

  c = clients;
  XMoveResizeWindow(x.dpy, c->win, 0, 0, master_w, dim.height);

  if (n == 1)
    return;

  stack_h = dim.height / (n - 1);
  i = 0;

  for (c = c->next; c; c = c->next, i++) {
    y = i * stack_h;
    h = (i == n - 2) ? (dim.height - y) : stack_h;
    XMoveResizeWindow(x.dpy, c->win, master_w, y, stack_w, h);
  }
}

static void run(void) {
  XEvent e;
  XKeyEvent *kev;
  XButtonEvent *bev;
  XCrossingEvent *cev;
  XMapRequestEvent *mrev;
  XConfigureRequestEvent *crev;
  XWindowAttributes wa;
  XWindowChanges wc;
  struct Client *c;
  size_t i;

  while (running) {
    XNextEvent(x.dpy, &e);

    switch (e.type) {
    case ButtonPress: {
      bev = &e.xbutton;
      c = getclient(bev->window);
      if (c) {
        focus(c);
        XSync(x.dpy, False);
      }
      XAllowEvents(x.dpy, ReplayPointer, CurrentTime);
    } break;

    case MapRequest: {
      mrev = &e.xmaprequest;
      if (!XGetWindowAttributes(x.dpy, mrev->window, &wa) ||
          wa.override_redirect)
        break;
      if (!getclient(mrev->window))
        addclient(mrev->window);
      XMapWindow(x.dpy, mrev->window);
      focus(getclient(mrev->window));
      arrange();
      XSync(x.dpy, False);
    } break;

    case ConfigureRequest: {
      crev = &e.xconfigurerequest;
      if (getclient(crev->window)) {
        wc.border_width = crev->border_width;
        wc.sibling = crev->above;
        wc.stack_mode = crev->detail;
        XConfigureWindow(
            x.dpy, crev->window,
            crev->value_mask & (CWBorderWidth | CWSibling | CWStackMode), &wc);
      } else {
        wc.x = crev->x;
        wc.y = crev->y;
        wc.width = crev->width;
        wc.height = crev->height;
        wc.border_width = crev->border_width;
        wc.sibling = crev->above;
        wc.stack_mode = crev->detail;
        XConfigureWindow(x.dpy, crev->window, crev->value_mask, &wc);
      }
    } break;

    case DestroyNotify: {
      if (!getclient(e.xdestroywindow.window))
        break;
      removeclient(e.xdestroywindow.window);
      arrange();
    } break;

    case UnmapNotify: {
      if (!getclient(e.xunmap.window))
        break;
      removeclient(e.xunmap.window);
      arrange();
    } break;

    case KeyPress: {
      kev = &e.xkey;
      for (i = 0; i < LENGTH(keys); i++) {
        if (kev->keycode == keys[i].keycode &&
            CLEANMASK(kev->state) == CLEANMASK(keys[i].mod))
          keys[i].func(&keys[i].key);
      }
    } break;

    case EnterNotify: {
      cev = &e.xcrossing;
      c = getclient(cev->window);
      if (c && c != sel)
        focus(c);
    } break;
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

  x.dpy = XOpenDisplay(NULL);
  if (!x.dpy)
    die("xtile: cannot open display");

  setup();
  checkconflicts();
  scan();

  if (clients)
    focus(clients);

  arrange();

  run();
  cleanup();

  return EXIT_SUCCESS;
}