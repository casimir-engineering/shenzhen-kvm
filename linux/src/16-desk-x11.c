/* ======================================================================================
 * THE DESKTOP over X11 (15-desk.c): Xorg, or Xwayland under a Wayland compositor.
 * ====================================================================================== */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Display *dpy;
static Window xwin, xroot;
static Atom A_CLIP, A_TARGETS, A_TIMESTAMP, A_INCR, A_UTF8, A_TEXT, A_PLAIN, A_PLAIN_UTF8, A_PNG, A_URIS, A_GNOME,
            A_PROP, A_ACTIVE, A_ATOM;
static Time held_t;                   /* when this service took the clipboard */
#define MAX_INCR 8
static struct { Window w; Atom prop, type; uint8_t *d; size_t n, off; int used; } incr[MAX_INCR];
static size_t incr_chunk = 65536, max_direct = 200000;
/* XFixes, loaded at run time (the image has the library but not its headers) */
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window; int subtype;
                 Window owner; Atom selection; Time timestamp; Time selection_timestamp; } FixSelEvent;
static int fix_event = -1;
static void (*fix_hide)(Display *, Window), (*fix_show)(Display *, Window);

static int xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static int xioerr(Display *d) { (void)d; logmsg("lost the desktop's X server"); _exit(3); }

/* The desktop's X server (Xwayland or Xorg) and its auth file, from its command line: the one named
   "display" in config.json, else the lowest display number (the main desktop; a second screen may run
   a second desktop on the next number). */
static int find_display(char *disp, size_t dn, char *auth, size_t an, uid_t *owner) {
    DIR *d = opendir("/proc"); struct dirent *e; int found = 0, best = INT_MAX;
    char bd[32] = "", ba[512] = ""; uid_t bo = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[64], c[4096]; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        int fd = open(p, O_RDONLY); if (fd < 0) continue;
        ssize_t n = read(fd, c, sizeof c - 1); close(fd); if (n <= 0) continue; c[n] = 0;
        const char *b = strrchr(c, '/'); b = b ? b + 1 : c;
        if (strcmp(b, "Xwayland") && strcmp(b, "Xorg") && strcmp(b, "X") && strcmp(b, "Xvfb")) continue;
        struct stat ps; snprintf(p, sizeof p, "/proc/%s", e->d_name); if (stat(p, &ps) == 0) *owner = ps.st_uid;
        char dd[32] = "", aa[512] = "";
        for (char *a = c; a < c + n; a += strlen(a) + 1) {
            if (a[0] == ':' && isdigit((unsigned char)a[1])) snprintf(dd, sizeof dd, "%s", a);
            if (!strcmp(a, "-auth") && a + strlen(a) + 1 < c + n) snprintf(aa, sizeof aa, "%s", a + strlen(a) + 1);
        }
        if (!dd[0]) continue;
        int num = atoi(dd + 1);
        if (want_display[0] ? !strcmp(dd, want_display) : num < best) { best = want_display[0] ? -1 : num; snprintf(bd, sizeof bd, "%s", dd); snprintf(ba, sizeof ba, "%s", aa); bo = *owner; found = 1; }
        if (want_display[0] && found) break;
    }
    closedir(d);
    if (found) { snprintf(disp, dn, "%s", bd); snprintf(auth, an, "%s", ba); *owner = bo; }
    return found;
}

static void root_size(int *w, int *h) {
    Window r; int x, y; unsigned gw, gh, bw, dep;
    XGetGeometry(dpy, xroot, &r, &x, &y, &gw, &gh, &bw, &dep); *w = (int)gw; *h = (int)gh;
}

static int x11_connected(void) { return dpy != NULL; }
static int x11_fd(void) { return dpy ? ConnectionNumber(dpy) : -1; }

static int x11_connect(void) {
    char disp[32], auth[512];
    uid_t owner = 0;
    if (!find_display(disp, sizeof disp, auth, sizeof auth, &owner)) return 0;
    struct passwd *pw = getpwuid(owner);
    desk_uid = owner; desk_gid = pw ? pw->pw_gid : owner; snprintf(desk_home, sizeof desk_home, "%s", pw && pw->pw_dir ? pw->pw_dir : "/root");
    if (auth[0]) setenv("XAUTHORITY", auth, 1);
    dpy = XOpenDisplay(disp);
    if (!dpy) return 0;
    XSetErrorHandler(xerr); XSetIOErrorHandler(xioerr);
    xroot = DefaultRootWindow(dpy);
    xwin = XCreateSimpleWindow(dpy, xroot, -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(dpy, xwin, PropertyChangeMask);
    XSelectInput(dpy, xroot, StructureNotifyMask);
    const char *names[] = {"CLIPBOARD", "TARGETS", "TIMESTAMP", "INCR", "UTF8_STRING", "TEXT", "text/plain",
                           "text/plain;charset=utf-8", "image/png", "text/uri-list", "x-special/gnome-copied-files",
                           "SHENZHEN_KVM_SEL", "_NET_ACTIVE_WINDOW", "ATOM"};
    Atom *dst[] = {&A_CLIP, &A_TARGETS, &A_TIMESTAMP, &A_INCR, &A_UTF8, &A_TEXT, &A_PLAIN, &A_PLAIN_UTF8, &A_PNG, &A_URIS,
                   &A_GNOME, &A_PROP, &A_ACTIVE, &A_ATOM};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) *dst[i] = XInternAtom(dpy, names[i], False);
    long mr = XExtendedMaxRequestSize(dpy); if (!mr) mr = XMaxRequestSize(dpy);
    max_direct = (size_t)mr * 4 - 1024; if (max_direct > 4u << 20) max_direct = 4u << 20;
    if (incr_chunk > max_direct) incr_chunk = max_direct;
    int w, h; root_size(&w, &h); desk_on_size(w, h);
    void *lib = dlopen("libXfixes.so.3", RTLD_NOW);
    Bool (*qe)(Display *, int *, int *) = lib ? dlsym(lib, "XFixesQueryExtension") : NULL;
    Status (*qv)(Display *, int *, int *) = lib ? dlsym(lib, "XFixesQueryVersion") : NULL;
    void (*ssi)(Display *, Window, Atom, unsigned long) = lib ? dlsym(lib, "XFixesSelectSelectionInput") : NULL;
    int eb, erb, maj = 5, min = 0;
    if (qe && qv && ssi && qe(dpy, &eb, &erb)) {
        qv(dpy, &maj, &min); ssi(dpy, xwin, A_CLIP, 1); fix_event = eb;
        fix_hide = dlsym(lib, "XFixesHideCursor"); fix_show = dlsym(lib, "XFixesShowCursor");
    }
    else herr("cannot watch the clipboard (libXfixes missing): copy and paste between the computers is off");
    XFlush(dpy);
    logmsg("connected to the desktop on %s", disp);
    return 1;
}

static int x11_pointer(int *x, int *y, unsigned *buttons, int *w, int *h) {
    if (!dpy) return 0;
    Window rr, cr; int rx, ry, wx, wy; unsigned m;
    XQueryPointer(dpy, xroot, &rr, &cr, &rx, &ry, &wx, &wy, &m);
    *x = rx; *y = ry;
    *buttons = (m & Button1Mask ? DESK_LEFT : 0) | (m & Button2Mask ? DESK_MIDDLE : 0) | (m & Button3Mask ? DESK_RIGHT : 0);
    root_size(w, h);
    return 1;
}

static int x11_hide_cursor(int hide) {
    if (!dpy || !fix_hide || !fix_show) return 0;
    if (hide) fix_hide(dpy, xroot); else fix_show(dpy, xroot);
    XFlush(dpy); return 1;
}

static Time x_time(void) {
    XChangeProperty(dpy, xwin, A_PROP, XA_STRING, 8, PropModeAppend, NULL, 0);
    XEvent e;
    for (;;) { XWindowEvent(dpy, xwin, PropertyChangeMask, &e); if (e.xproperty.atom == A_PROP) return e.xproperty.time; }
}

/* --- serving the clipboard we hold --- */
static void serve_request(XSelectionRequestEvent *r) {
    XSelectionEvent ev = {.type = SelectionNotify, .display = r->display, .requestor = r->requestor, .selection = r->selection,
                          .target = r->target, .property = None, .time = r->time};
    Atom prop = r->property ? r->property : r->target;
    const uint8_t *d = NULL; size_t n = 0;
    if (r->selection == A_CLIP && held.own && held.kind == 3 && r->requestor != xwin && (r->target == A_URIS || r->target == A_GNOME))
        desk_on_files_asked();
    if (r->selection == A_CLIP && held.own) {
        if (r->target == A_TARGETS) {
            Atom t[8]; int k = 0; t[k++] = A_TARGETS; t[k++] = A_TIMESTAMP;
            if (held.kind == 1) { t[k++] = A_UTF8; t[k++] = A_PLAIN_UTF8; t[k++] = A_PLAIN; t[k++] = XA_STRING; t[k++] = A_TEXT; }
            if (held.kind == 2) t[k++] = A_PNG;
            if (held.kind == 3) { t[k++] = A_URIS; t[k++] = A_GNOME; t[k++] = A_UTF8; t[k++] = A_PLAIN; }
            XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, k);
            ev.property = prop;
        } else if (r->target == A_TIMESTAMP) {
            long t = held_t; XChangeProperty(dpy, r->requestor, prop, XA_INTEGER, 32, PropModeReplace, (unsigned char *)&t, 1);
            ev.property = prop;
        } else {
            if (held.kind == 1 && (r->target == A_UTF8 || r->target == A_PLAIN_UTF8 || r->target == A_PLAIN || r->target == XA_STRING || r->target == A_TEXT)) { d = held.data.p; n = held.data.n; }
            else if (held.kind == 2 && r->target == A_PNG) { d = held.data.p; n = held.data.n; }
            else if (held.kind == 3 && r->target == A_URIS) { d = held.uris.p; n = held.uris.n; }
            else if (held.kind == 3 && r->target == A_GNOME) { d = held.gnome.p; n = held.gnome.n; }
            else if (held.kind == 3 && (r->target == A_UTF8 || r->target == A_PLAIN)) { d = held.data.p; n = held.data.n; }
            if (d || (held.kind == 1 && r->target != None)) {
                Atom type = r->target == A_TEXT ? A_UTF8 : r->target;
                if (n <= max_direct) {
                    XChangeProperty(dpy, r->requestor, prop, type, 8, PropModeReplace, d ? d : (const uint8_t *)"", (int)n);
                    ev.property = prop;
                } else {
                    int i; for (i = 0; i < MAX_INCR && incr[i].used; i++) ;
                    if (i < MAX_INCR) {
                        incr[i].used = 1; incr[i].w = r->requestor; incr[i].prop = prop; incr[i].type = type;
                        incr[i].d = malloc(n); memcpy(incr[i].d, d, n); incr[i].n = n; incr[i].off = 0;
                        XSelectInput(dpy, r->requestor, PropertyChangeMask);
                        long sz = (long)n; XChangeProperty(dpy, r->requestor, prop, A_INCR, 32, PropModeReplace, (unsigned char *)&sz, 1);
                        ev.property = prop;
                    }
                }
            }
        }
    }
    XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&ev);
    XFlush(dpy);
}
static void incr_step(XPropertyEvent *p) {
    if (p->state != PropertyDelete) return;
    for (int i = 0; i < MAX_INCR; i++) {
        if (!incr[i].used || incr[i].w != p->window || incr[i].prop != p->atom) continue;
        size_t k = incr[i].n - incr[i].off; if (k > incr_chunk) k = incr_chunk;
        XChangeProperty(dpy, p->window, p->atom, incr[i].type, 8, PropModeReplace, incr[i].d + incr[i].off, (int)k);
        incr[i].off += k;
        if (k == 0) { free(incr[i].d); incr[i].used = 0; XSelectInput(dpy, p->window, NoEventMask); }
        XFlush(dpy);
        return;
    }
}
static void handle_xevent(XEvent *e) {
    if (fix_event >= 0 && e->type == fix_event) {
        FixSelEvent *f = (FixSelEvent *)e;
        if (f->selection == A_CLIP && f->owner != None && f->owner != xwin) desk_on_copied();
        return;
    }
    if (e->type == SelectionRequest) serve_request(&e->xselectionrequest);
    else if (e->type == SelectionClear && e->xselectionclear.selection == A_CLIP) held.own = 0;
    else if (e->type == PropertyNotify && e->xproperty.window != xwin) incr_step(&e->xproperty);
    else if (e->type == ConfigureNotify && e->xconfigure.window == xroot) { int w, h; root_size(&w, &h); desk_on_size(w, h); }
}
static void x11_pump(short revents) {
    if (!dpy) return;
    if (revents & (POLLHUP | POLLERR)) xioerr(dpy);
    if ((revents & POLLIN) && !XPending(dpy)) XSync(dpy, False);
    while (XPending(dpy)) { XEvent e; XNextEvent(dpy, &e); handle_xevent(&e); }
}

/* --- reading the clipboard from another program --- */
static int wait_event(int (*match)(XEvent *, void *), void *arg, XEvent *out, int ms) {
    int64_t end = mono_ms() + ms;
    for (;;) {
        while (XPending(dpy)) { XNextEvent(dpy, out); if (match(out, arg)) return 1; handle_xevent(out); }
        int64_t left = end - mono_ms(); if (left <= 0) return 0;
        struct pollfd q = {ConnectionNumber(dpy), POLLIN, 0}; poll(&q, 1, (int)left);
        if (q.revents & (POLLHUP | POLLERR)) xioerr(dpy);
        if (q.revents & POLLIN) { if (!XPending(dpy)) { XSync(dpy, False); } }
    }
}
static int m_selnotify(XEvent *e, void *a) { (void)a; return e->type == SelectionNotify && e->xselection.requestor == xwin; }
static int m_newval(XEvent *e, void *a) { (void)a; return e->type == PropertyNotify && e->xproperty.window == xwin && e->xproperty.atom == A_PROP && e->xproperty.state == PropertyNewValue; }

static int read_prop(buf_t *out, Atom *type, int del) {
    Atom t; int fmt; unsigned long n, after; unsigned char *d = NULL; long off = 0;
    out->n = 0;
    do {
        if (XGetWindowProperty(dpy, xwin, A_PROP, off, 1 << 20, False, AnyPropertyType, &t, &fmt, &n, &after, &d) != Success) return 0;
        size_t unit = fmt == 32 ? sizeof(long) : fmt / 8;
        bput(out, d, n * unit); off += n * unit / 4; XFree(d);
        *type = t;
    } while (after && out->n < MAX_CLIP);
    if (del) XDeleteProperty(dpy, xwin, A_PROP);
    return 1;
}
/* Convert CLIPBOARD to target into out. Returns 1 on success. */
static int x_convert(Atom target, buf_t *out, Atom *type) {
    XEvent e;
    XDeleteProperty(dpy, xwin, A_PROP);
    XConvertSelection(dpy, A_CLIP, target, A_PROP, xwin, CurrentTime);
    XFlush(dpy);
    if (!wait_event(m_selnotify, NULL, &e, 1500) || e.xselection.property == None) return 0;
    if (!read_prop(out, type, 1)) return 0;
    if (*type != A_INCR) return 1;
    out->n = 0; buf_t part = {0};
    for (;;) {
        if (!wait_event(m_newval, NULL, &e, 3000)) { bfree(&part); return 0; }
        Atom t; read_prop(&part, &t, 1);
        if (!part.n) break;
        *type = t; bput(out, part.p, part.n);
        if (out->n > MAX_CLIP) { bfree(&part); return 0; }
    }
    bfree(&part);
    return 1;
}

static int has_atom(Atom *l, int n, Atom a) { for (int i = 0; i < n; i++) if (l[i] == a) return 1; return 0; }
static Atom offered[64]; static int n_offered;   /* the targets the clipboard offered at the last desk_clip_formats */

static int x11_clip_ours(void) { return dpy && held.own && XGetSelectionOwner(dpy, A_CLIP) == xwin; }

static int x11_clip_formats(void) {
    buf_t t = {0}; Atom type; int f = 0;
    n_offered = 0;
    if (dpy && XGetSelectionOwner(dpy, A_CLIP) != None && x_convert(A_TARGETS, &t, &type)) {
        int n = t.n / sizeof(Atom); if (n > 64) n = 64;
        memcpy(offered, t.p, n * sizeof(Atom)); n_offered = n;
        Atom *l = offered;
        if (has_atom(l, n, A_URIS)) f |= 1 << CF_URIS;
        if (has_atom(l, n, A_GNOME)) f |= 1 << CF_GNOME;
        if (has_atom(l, n, A_PNG)) f |= 1 << CF_PNG;
        if (has_atom(l, n, A_UTF8) || has_atom(l, n, A_PLAIN_UTF8) || has_atom(l, n, XA_STRING) || has_atom(l, n, A_PLAIN)) f |= 1 << CF_TEXT;
    }
    bfree(&t); return f;
}

static int x11_clip_get(int cf, buf_t *out) {
    if (!dpy) return 0;
    Atom type;
    if (cf == CF_URIS) return x_convert(A_URIS, out, &type);
    if (cf == CF_GNOME) return x_convert(A_GNOME, out, &type);
    if (cf == CF_PNG) return x_convert(A_PNG, out, &type) && out->n;
    Atom text[] = {A_UTF8, A_PLAIN_UTF8, XA_STRING, A_PLAIN};
    for (int i = 0; i < 4; i++) if (has_atom(offered, n_offered, text[i]) && x_convert(text[i], out, &type)) return 1;
    return 0;
}

static int x11_clip_take(void) {
    if (!dpy) return 0;
    held_t = x_time();
    XSetSelectionOwner(dpy, A_CLIP, xwin, held_t);
    held.own = XGetSelectionOwner(dpy, A_CLIP) == xwin; XFlush(dpy);
    return held.own;
}

static void x11_clip_drop(void) {
    if (dpy && held.own) { XSetSelectionOwner(dpy, A_CLIP, None, CurrentTime); XFlush(dpy); }
    held.own = 0;
}

static uint64_t x11_clip_stamp(void) {
    if (!dpy) return 0;
    Window o = XGetSelectionOwner(dpy, A_CLIP);
    long ts = 0;
    if (o != None && o != xwin) { buf_t b = {0}; Atom t; if (x_convert(A_TIMESTAMP, &b, &t) && b.n >= sizeof(long)) memcpy(&ts, b.p, sizeof ts); bfree(&b); }
    return (uint64_t)o << 32 ^ (uint32_t)ts;
}

static int x11_active_is_terminal(void) {
    if (!dpy) return 0;
    Atom t; int f; unsigned long n, a; unsigned char *d = NULL; int term = 0;
    if (XGetWindowProperty(dpy, xroot, A_ACTIVE, 0, 1, False, XA_WINDOW, &t, &f, &n, &a, &d) != Success || !d || !n) { if (d) XFree(d); return 0; }
    Window w = *(Window *)d; XFree(d);
    XClassHint ch = {0};
    if (w && XGetClassHint(dpy, w, &ch)) {
        const char *s[] = {ch.res_name, ch.res_class};
        for (int i = 0; i < 2; i++) {
            if (!s[i]) continue; char l[128]; snprintf(l, sizeof l, "%s", s[i]); for (char *c = l; *c; c++) *c = tolower((unsigned char)*c);
            if (strstr(l, "term") || strstr(l, "rxvt") || !strcmp(l, "kitty") || !strcmp(l, "alacritty") || !strcmp(l, "konsole") || !strcmp(l, "tilix") || !strcmp(l, "foot") || !strcmp(l, "st")) term = 1;
        }
        if (ch.res_name) XFree(ch.res_name); if (ch.res_class) XFree(ch.res_class);
    }
    return term;
}
