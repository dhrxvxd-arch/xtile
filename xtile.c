#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define MODMASK Mod4Mask
#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))
#define CLEANMASK(mask)                                                        \
  (mask & ~(numlockmask | LockMask) &                                          \
   (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask |      \
    Mod5Mask))
#define ISVISIBLE(C) (C->tags & tagset)

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

struct Layout {
  const char *symbol;
  void (*layout)(void);
};

static const char col_1[] = "#222222";
static const char col_2[] = "#444444";
static const char col_3[] = "#bbbbbb";
static const char col_4[] = "#eeeeee";
static const char col_accent[] = "#005577";

static const char *colors[][3] = {
    {col_3, col_1, col_2},
    {col_4, col_accent, col_accent},
};

static const unsigned short borderpx = 2;
static const char font[] = "monospace:size=10";
static const char *termcmd[] = {"st", NULL};

static struct XContext x;
static struct Client *clients;
static struct Client *sel;
static struct Dimensions dim;
static unsigned int numlockmask;
static volatile sig_atomic_t running = 1;
static unsigned int tagset = 1;
static unsigned long scheme[2][3];

static Atom wm_delete;
static Atom wm_protocols;
static Atom wm_state;

static float mfact = 0.6;
static int layout_idx = 0;

static void arrange(void);
static void tile(void);
static void monocle(void);

static const struct Layout layouts[] = {
    {"[]=", tile},
    {"><>", NULL},
    {"[M]", monocle},
};

static void updatenumlockmask(void) {
  XModifierKeymap *modmap;
  KeyCode numlock;
  numlockmask = 0;

  modmap = XGetModifierMapping(x.dpy);
  numlock = XKeysymToKeycode(x.dpy, XK_Num_Lock);

  for (int i = 0; i < 8; i++)
    for (int j = 0; j < modmap->max_keypermod; j++)
      if (modmap->modifiermap[i * modmap->max_keypermod + j] == numlock)
        numlockmask = (1 << i);

  XFreeModifiermap(modmap);
}

static void focus(struct Client *c) {
  if (sel && sel != c)
    XSetWindowBorder(x.dpy, sel->win, scheme[0][2]);

  if (!c) {
    sel = NULL;
    XSetInputFocus(x.dpy, x.root, RevertToPointerRoot, CurrentTime);
    return;
  }

  sel = c;
  XSetInputFocus(x.dpy, c->win, RevertToPointerRoot, CurrentTime);
  XRaiseWindow(x.dpy, c->win);
  XSetWindowBorder(x.dpy, c->win, scheme[1][2]);
}

static void restack(void) {
  if (!sel)
    return;

  XRaiseWindow(x.dpy, sel->win);

  for (struct Client *c = clients; c; c = c->next)
    if (c != sel && ISVISIBLE(c))
      XLowerWindow(x.dpy, c->win);
}

static void tile(void) {
  struct Client *c;
  int n = 0;

  for (c = clients; c; c = c->next)
    if (ISVISIBLE(c))
      n++;
    else
      XUnmapWindow(x.dpy, c->win);

  if (n == 0)
    return;

  int mw = (n > 1) ? dim.width * mfact : dim.width;
  int sw = dim.width - mw;

  int i = 0;
  int sh = (n > 1) ? dim.height / (n - 1) : 0;

  for (c = clients; c; c = c->next) {
    if (!ISVISIBLE(c))
      continue;

    XMapWindow(x.dpy, c->win);

    if (i == 0) {
      XMoveResizeWindow(x.dpy, c->win, 0, 0, mw - 2 * borderpx,
                        dim.height - 2 * borderpx);
    } else {
      int y = (i - 1) * sh;
      int h = (i == n - 1) ? dim.height - y : sh;

      XMoveResizeWindow(x.dpy, c->win, mw, y, sw - 2 * borderpx,
                        h - 2 * borderpx);
    }
    i++;
  }

  restack();
}

static void monocle(void) {
  for (struct Client *c = clients; c; c = c->next) {
    if (!ISVISIBLE(c)) {
      XUnmapWindow(x.dpy, c->win);
      continue;
    }

    XMapWindow(x.dpy, c->win);
    XMoveResizeWindow(x.dpy, c->win, 0, 0, dim.width - 2 * borderpx,
                      dim.height - 2 * borderpx);
  }

  restack();
}

static void arrange(void) {
  if (!layouts[layout_idx].layout)
    return;
  layouts[layout_idx].layout();
}

static void setmfact(const union Key *k) {
  float f = mfact + k->f;
  if (f < 0.1 || f > 0.9)
    return;
  mfact = f;
  arrange();
}

static void nextlayout(const union Key *k) {
  (void)k;
  layout_idx = (layout_idx + 1) % LENGTH(layouts);
  arrange();
}

static void spawn(const union Key *key) {
  if (fork() == 0) {
    if (x.dpy)
      close(ConnectionNumber(x.dpy));
    setsid();
    execvp(((char **)key->v)[0], (char **)key->v);
    _exit(1);
  }
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
  } else
    XKillClient(x.dpy, sel->win);
}

static struct Client *getclient(Window w) {
  for (struct Client *c = clients; c; c = c->next)
    if (c->win == w)
      return c;
  return NULL;
}

static void addclient(Window w) {
  struct Client *c = calloc(1, sizeof(*c));
  c->win = w;
  c->tags = tagset;
  c->next = clients;
  clients = c;

  XSetWindowBorderWidth(x.dpy, w, borderpx);
  XSetWindowBorder(x.dpy, w, scheme[0][2]);

  Atom *protos;
  int n;
  if (XGetWMProtocols(x.dpy, w, &protos, &n)) {
    for (int i = 0; i < n; i++)
      if (protos[i] == wm_delete)
        c->can_delete = 1;
    XFree(protos);
  }
}

static void removeclient(Window w) {
  struct Client **tc = &clients;
  while (*tc) {
    if ((*tc)->win == w) {
      struct Client *t = *tc;
      *tc = t->next;
      free(t);
      break;
    }
    tc = &(*tc)->next;
  }
}

static unsigned long getcolor(const char *col) {
  XColor c;
  Colormap cmap = DefaultColormap(x.dpy, x.screen);
  XAllocNamedColor(x.dpy, cmap, col, &c, &c);
  return c.pixel;
}

static void setup(void) {
  x.screen = DefaultScreen(x.dpy);
  x.root = RootWindow(x.dpy, x.screen);

  updatenumlockmask();

  dim.width = DisplayWidth(x.dpy, x.screen);
  dim.height = DisplayHeight(x.dpy, x.screen);

  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 3; j++)
      scheme[i][j] = getcolor(colors[i][j]);

  wm_delete = XInternAtom(x.dpy, "WM_DELETE_WINDOW", False);
  wm_protocols = XInternAtom(x.dpy, "WM_PROTOCOLS", False);
  wm_state = XInternAtom(x.dpy, "WM_STATE", False);

  XSelectInput(x.dpy, x.root,
               SubstructureRedirectMask | SubstructureNotifyMask);
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
    } else if (e.type == DestroyNotify) {
      removeclient(e.xdestroywindow.window);
      arrange();
    } else if (e.type == ConfigureNotify) {
      dim.width = e.xconfigure.width;
      dim.height = e.xconfigure.height;
      arrange();
    } else if (e.type == KeyPress) {
      KeySym sym = XLookupKeysym(&e.xkey, 0);

      if (sym == XK_Return)
        spawn(&(union Key){.v = termcmd});
      else if (sym == XK_q)
        running = 0;
      else if (sym == XK_Tab)
        nextlayout(NULL);
      else if (sym == XK_h)
        setmfact(&(union Key){.f = -0.05});
      else if (sym == XK_l)
        setmfact(&(union Key){.f = 0.05});
      else if (sym == XK_w)
        killclient(NULL);
    }
  }
}

int main(void) {
  setlocale(LC_CTYPE, "");

  x.dpy = XOpenDisplay(NULL);
  if (!x.dpy)
    exit(1);

  setup();
  run();

  XCloseDisplay(x.dpy);
  return 0;
}