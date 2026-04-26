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
#define CLEANMASK(mask)                                                        \
  (mask & ~(numlockmask | LockMask) &                                          \
   (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask |      \
    Mod5Mask))
#define ISVISIBLE(C) (C->tags & tagset)
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
  unsigned int tags;
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
  void (*func)(const union Key *);
  const union Key key;
};

enum { SchemeNorm, SchemeSel };

enum { CurNormal, CurResize, CurMove, CurLast };

static const unsigned short borderwidth = 2;
static unsigned long scheme[2][3];
static const char *termcmd[] = {"st", NULL};
static const char col_1[] = "#222222";
static const char col_2[] = "#444444";
static const char col_3[] = "#bbbbbb";
static const char col_4[] = "#eeeeee";
static const char col_accent[] = "#005577";
static const char *colors[][3] = {
    [SchemeNorm] = {col_3, col_1, col_2},
    [SchemeSel] = {col_4, col_accent, col_accent},
};

static struct XContext x;
static struct Client *clients;
static struct Client *sel;
static struct Dimensions dim;
static unsigned int numlockmask;
static volatile sig_atomic_t running = 1;
static unsigned int tagset = 1;

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
static unsigned long getcolor(const char *col);
static void updatenumlockmask(void);
static void quit(const union Key *k);
static void focusnext(const union Key *k);
static void removeclient(Window w);
static void arrange(void);
static void scan(void);
static void view(const union Key *k);
static void tag(const union Key *k);
static struct Client *getclient(Window w);

static struct KeyGr keys[] = {
    {MODMASK, XK_Return, spawn, {.v = termcmd}},
    {MODMASK, XK_q, quit, {0}},
    {MODMASK, XK_w, killclient, {0}},
    {MODMASK, XK_j, focusnext, {0}},

    {MODMASK, XK_1, view, {.ui = 1 << 0}},
    {MODMASK, XK_2, view, {.ui = 1 << 1}},
    {MODMASK, XK_3, view, {.ui = 1 << 2}},

    {MODMASK | ShiftMask, XK_1, tag, {.ui = 1 << 0}},
    {MODMASK | ShiftMask, XK_2, tag, {.ui = 1 << 1}},
    {MODMASK | ShiftMask, XK_3, tag, {.ui = 1 << 2}},
};

static void view(const union Key *k) {
  if (tagset == k->ui)
    return;

  tagset = k->ui;
  arrange();

  sel = NULL;
  for (struct Client *c = clients; c; c = c->next) {
    if (ISVISIBLE(c)) {
      focus(c);
      break;
    }
  }
}

static void tag(const union Key *k) {
  if (!sel)
    return;

  sel->tags = k->ui;
  arrange();

  sel = NULL;
  for (struct Client *c = clients; c; c = c->next) {
    if (ISVISIBLE(c)) {
      focus(c);
      break;
    }
  }
}

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

static void updatenumlockmask(void) {
  XModifierKeymap *modmap;
  KeyCode numlock;

  numlockmask = 0;
  modmap = XGetModifierMapping(x.dpy);
  numlock = XKeysymToKeycode(x.dpy, XK_Num_Lock);

  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < modmap->max_keypermod; j++) {
      if (modmap->modifiermap[i * modmap->max_keypermod + j] == numlock)
        numlockmask = (1 << i);
    }
  }

  XFreeModifiermap(modmap);
}

static unsigned long getcolor(const char *col) {
  XColor color;
  Colormap cmap;

  cmap = DefaultColormap(x.dpy, x.screen);
  if (!XAllocNamedColor(x.dpy, cmap, col, &color, &color))
    die("cannot allocate color");

  return color.pixel;
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
  struct Client *c;
  (void)k;

  if (!clients)
    return;

  c = sel ? sel->next : clients;

  for (; c && !ISVISIBLE(c); c = c->next)
    ;

  if (!c)
    for (c = clients; c && !ISVISIBLE(c); c = c->next)
      ;

  if (!c)
    return;

  if (c && c != sel)
    focus(c);
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
    KeyCode code = XKeysymToKeycode(x.dpy, keys[i].keysym);
    XGrabKey(x.dpy, code, keys[i].mod, x.root, True, GrabModeAsync,
             GrabModeAsync);
  }

  updatenumlockmask();

  for (size_t i = 0; i < LENGTH(colors); i++)
    for (size_t j = 0; j < 3; j++)
      scheme[i][j] = getcolor(colors[i][j]);
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
  struct Client *old;

  old = sel;

  if (!c) {
    sel = NULL;
    XSetInputFocus(x.dpy, x.root, RevertToPointerRoot, CurrentTime);
    return;
  }

  if (c == old)
    return;

  sel = c;

  if (old)
    XSetWindowBorder(x.dpy, old->win, scheme[SchemeNorm][2]);

  XSetWindowBorder(x.dpy, c->win, scheme[SchemeSel][2]);

  XSetInputFocus(x.dpy, c->win, RevertToPointerRoot, CurrentTime);
  XRaiseWindow(x.dpy, c->win);
}

static void addclient(Window w) {
  struct Client *c;

  c = ecalloc(1, sizeof(*c));
  c->win = w;
  c->next = clients;
  clients = c;
  c->tags = tagset;

  XSetWindowBorderWidth(x.dpy, c->win, borderwidth);
  XSelectInput(x.dpy, w, EnterWindowMask | FocusChangeMask);
  XSetWindowBorder(x.dpy, c->win, scheme[SchemeNorm][2]);
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

  for (c = clients; c; c = c->next) {
    if (ISVISIBLE(c))
      XMapWindow(x.dpy, c->win);
    else
      XUnmapWindow(x.dpy, c->win);
  }

  n = 0;
  for (c = clients; c; c = c->next)
    if (ISVISIBLE(c))
      n++;

  if (!n)
    return;

  master_w = (n > 1) ? (dim.width * 3 / 5) : dim.width;
  stack_w = dim.width - master_w;

  for (c = clients; c && !ISVISIBLE(c); c = c->next)
    ;

  if (!c)
    return;

  XMoveResizeWindow(x.dpy, c->win, 0, 0, master_w, dim.height);

  if (n == 1)
    return;

  stack_h = dim.height / (n - 1);
  i = 0;

  for (c = c->next; c; c = c->next) {
    if (!ISVISIBLE(c))
      continue;

    y = i * stack_h;
    h = (i == n - 2) ? (dim.height - y) : stack_h;

    XMoveResizeWindow(x.dpy, c->win, master_w, y, stack_w, h);
    i++;
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

    case ConfigureNotify: {
      if (e.xconfigure.window == x.root) {
        dim.width = e.xconfigure.width;
        dim.height = e.xconfigure.height;
        arrange();
      }
    } break;

    case DestroyNotify: {
      if (!getclient(e.xdestroywindow.window))
        break;
      removeclient(e.xdestroywindow.window);
      arrange();
    } break;

    case UnmapNotify: {
      if (e.xunmap.event == x.root)
        break;

      if (!getclient(e.xunmap.window))
        break;

      removeclient(e.xunmap.window);
      arrange();
    } break;

    case KeyPress: {
      kev = &e.xkey;
      KeySym sym = XLookupKeysym(kev, 0);
      for (i = 0; i < LENGTH(keys); i++) {
        if (sym == keys[i].keysym &&
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
