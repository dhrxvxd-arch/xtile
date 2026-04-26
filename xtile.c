#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
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
  unsigned int can_delete;
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
static const char *roficmd[] = {"rofi", "-show", "run", NULL};

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

static Atom wm_delete;
static Atom wm_protocols;
static Atom wm_state;
static Atom wm_take_focus;
static Atom wm_normal;

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
static void setwmstate(Window w, long state);
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
    {MODMASK, 			XK_Return, 	spawn, 		{.v = termcmd}},
    {MODMASK, 			XK_space,  	spawn, 		{.v = roficmd}},
    {MODMASK, 			XK_q, 		quit, 		{0}},
    {MODMASK, 			XK_w, 		killclient, 	{0}},
    {MODMASK, 			XK_j, 		focusnext, 	{0}},
    {MODMASK, 			XK_1, 		view, 		{.ui = 1 << 0}},
    {MODMASK, 			XK_2, 		view, 		{.ui = 1 << 1}},
    {MODMASK, 			XK_3, 		view, 		{.ui = 1 << 2}},
    {MODMASK|ShiftMask, 	XK_1, 		tag, 		{.ui = 1 << 0}},
    {MODMASK|ShiftMask, 	XK_2, 		tag, 		{.ui = 1 << 1}},
    {MODMASK|ShiftMask, 	XK_3, 		tag, 		{.ui = 1 << 2}},
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
  int saved_errno = errno;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
    fprintf(stderr, " %s", strerror(saved_errno));

  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

void *ecalloc(size_t nmemb, size_t size) {
  void *p = calloc(nmemb, size);
  if (!p)
    die("calloc failed");
  return p;
}

static unsigned long getcolor(const char *col) {
  XColor c;
  Colormap cmap = DefaultColormap(x.dpy, x.screen);
  if (!XAllocNamedColor(x.dpy, cmap, col, &c, &c))
    die("color alloc failed");
  return c.pixel;
}

static void updatenumlockmask(void) {
  XModifierKeymap *modmap = XGetModifierMapping(x.dpy);
  KeyCode numlock = XKeysymToKeycode(x.dpy, XK_Num_Lock);
  numlockmask = 0;
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < modmap->max_keypermod; j++) {
      if (modmap->modifiermap[i * modmap->max_keypermod + j] == numlock)
        numlockmask = (1 << i);
    }
  }
  XFreeModifiermap(modmap);
}

static void spawn(const union Key *key) {
  pid_t pid = fork();
  if (pid == 0) {
    if (x.dpy)
      close(ConnectionNumber(x.dpy));
    setsid();
    char **argv = (char **)key->v;
    execvp(argv[0], argv);
    _exit(EXIT_FAILURE);
  }
}

static void setwmstate(Window w, long state) {
  long data[2] = {state, None};
  XChangeProperty(x.dpy, w, wm_state, wm_state, 32, PropModeReplace,
                  (unsigned char *)data, 2);
}

static void focus(struct Client *c) {
  if (!c) {
    sel = NULL;
    XSetInputFocus(x.dpy, x.root, RevertToPointerRoot, CurrentTime);
    return;
  }
  sel = c;
  XSetInputFocus(x.dpy, c->win, RevertToPointerRoot, CurrentTime);
  XRaiseWindow(x.dpy, c->win);
}

static struct Client *getclient(Window w) {
  for (struct Client *c = clients; c; c = c->next)
    if (c->win == w)
      return c;
  return NULL;
}

static void addclient(Window w) {
  struct Client *c = ecalloc(1, sizeof(*c));
  c->win = w;
  c->tags = tagset;
  c->next = clients;
  clients = c;

  XSetWindowBorderWidth(x.dpy, w, borderwidth);
  XSelectInput(x.dpy, w,
               EnterWindowMask | FocusChangeMask | StructureNotifyMask);
  XSetWindowBorder(x.dpy, w, scheme[SchemeNorm][2]);

  Atom *protos = NULL;
  int n;
  if (XGetWMProtocols(x.dpy, w, &protos, &n)) {
    for (int i = 0; i < n; i++) {
      if (protos[i] == wm_delete)
        c->can_delete = 1;
    }
    XFree(protos);
  }

  XSetWMProtocols(x.dpy, w, &wm_delete, 1);
  XSetWMProtocols(x.dpy, w, &wm_take_focus, 1);
  setwmstate(w, NormalState);
}

static void killclient(const union Key *k) {
  (void)k;
  if (!sel)
    return;
  if (sel->can_delete) {
    XEvent ev = {0};
    ev.type = ClientMessage;
    ev.xclient.window = sel->win;
    ev.xclient.message_type = wm_protocols;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = wm_delete;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(x.dpy, sel->win, False, NoEventMask, &ev);
  } else {
    XKillClient(x.dpy, sel->win);
  }
}

static void focusnext(const union Key *k) {
  (void)k;
  if (!clients)
    return;
  struct Client *c = sel ? sel->next : clients;
  for (; c && !ISVISIBLE(c); c = c->next)
    ;
  if (!c)
    for (c = clients; c && !ISVISIBLE(c); c = c->next)
      ;
  if (c)
    focus(c);
}

static void quit(const union Key *k) {
  (void)k;
  running = 0;
}

static void removeclient(Window w) {
  struct Client **tc = &clients;
  while (*tc) {
    if ((*tc)->win == w) {
      struct Client *t = *tc;
      *tc = t->next;
      if (sel == t)
        sel = clients;
      free(t);
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
  int n = 0;
  for (struct Client *c = clients; c; c = c->next)
    if (ISVISIBLE(c))
      n++;
  if (!n)
    return;

  int mw = (n > 1) ? dim.width * 3 / 5 : dim.width;
  int sw = dim.width - mw;

  struct Client *c = clients;
  while (c && !ISVISIBLE(c))
    c = c->next;
  if (!c)
    return;

  XMoveResizeWindow(x.dpy, c->win, 0, 0, mw, dim.height);

  int i = 0;
  int sh = dim.height / (n - 1);

  for (c = c->next; c; c = c->next) {
    if (!ISVISIBLE(c))
      continue;
    int y = i * sh;
    int h = (i == n - 2) ? (dim.height - y) : sh;
    XMoveResizeWindow(x.dpy, c->win, mw, y, sw, h);
    i++;
  }
}

static void scan(void) {
  Window r, p, *wins;
  unsigned int n;
  XWindowAttributes wa;

  if (XQueryTree(x.dpy, x.root, &r, &p, &wins, &n)) {
    for (unsigned int i = 0; i < n; i++) {
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

static void checkconflicts(void) {
  XSetErrorHandler(NULL);
  XSelectInput(x.dpy, x.root,
               SubstructureRedirectMask | SubstructureNotifyMask |
                   ButtonPressMask | PointerMotionMask | StructureNotifyMask);
  XSync(x.dpy, False);
}

static void initlocale(void) {
  setlocale(LC_CTYPE, "");
  XSupportsLocale();
  XSetLocaleModifiers("");
}

static void setup(void) {
  x.screen = DefaultScreen(x.dpy);
  x.root = RootWindow(x.dpy, x.screen);
  x.cursor = XCreateFontCursor(x.dpy, XC_left_ptr);
  XDefineCursor(x.dpy, x.root, x.cursor);

  dim.width = DisplayWidth(x.dpy, x.screen);
  dim.height = DisplayHeight(x.dpy, x.screen);

  for (size_t i = 0; i < LENGTH(keys); i++) {
    KeyCode code = XKeysymToKeycode(x.dpy, keys[i].keysym);
    XGrabKey(x.dpy, code, keys[i].mod, x.root, True, GrabModeAsync,
             GrabModeAsync);
  }

  wm_delete = XInternAtom(x.dpy, "WM_DELETE_WINDOW", False);
  wm_protocols = XInternAtom(x.dpy, "WM_PROTOCOLS", False);
  wm_state = XInternAtom(x.dpy, "WM_STATE", False);
  wm_take_focus = XInternAtom(x.dpy, "WM_TAKE_FOCUS", False);
  wm_normal = XInternAtom(x.dpy, "NormalState", False);

  updatenumlockmask();

  for (size_t i = 0; i < LENGTH(colors); i++)
    for (size_t j = 0; j < 3; j++)
      scheme[i][j] = getcolor(colors[i][j]);
}

static void run(void) {
  XEvent e;
  while (running) {
    XNextEvent(x.dpy, &e);
    if (e.type == MapRequest) {
      Window w = e.xmaprequest.window;
      if (!getclient(w))
        addclient(w);
      XMapWindow(x.dpy, w);
      focus(getclient(w));
      arrange();
      setwmstate(w, NormalState);
    } else if (e.type == ConfigureNotify) {
      if (e.xconfigure.window == x.root) {
        dim.width = e.xconfigure.width;
        dim.height = e.xconfigure.height;
        arrange();
      }
    } else if (e.type == DestroyNotify) {
      removeclient(e.xdestroywindow.window);
      arrange();
    } else if (e.type == UnmapNotify) {
      if (!getclient(e.xunmap.window))
        continue;
      removeclient(e.xunmap.window);
      arrange();
    } else if (e.type == KeyPress) {
      XKeyEvent *ke = &e.xkey;
      KeySym sym = XLookupKeysym(ke, 0);
      for (size_t i = 0; i < LENGTH(keys); i++) {
        if (sym == keys[i].keysym &&
            CLEANMASK(ke->state) == CLEANMASK(keys[i].mod))
          keys[i].func(&keys[i].key);
      }
    } else if (e.type == EnterNotify) {
      struct Client *c = getclient(e.xcrossing.window);
      if (c)
        focus(c);
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
  if (argc == 2 && !strcmp(argv[1], "-v")) {
    puts("xtile-" VERSION);
    return 0;
  }

  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);

  initlocale();

  x.dpy = XOpenDisplay(NULL);
  if (!x.dpy)
    die("cannot open display");

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
