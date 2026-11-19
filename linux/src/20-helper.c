/* ======================================================================================
 * HELPER PROCESS: X connection, clipboard, TCP 24801
 * ====================================================================================== */
static Display *dpy;
static Window xwin, xroot;
static int hfd;   /* socket to main */
static Atom A_CLIP, A_TARGETS, A_TIMESTAMP, A_INCR, A_UTF8, A_TEXT, A_PLAIN, A_PLAIN_UTF8, A_PNG, A_URIS, A_GNOME,
            A_PROP, A_ACTIVE, A_ATOM;
static struct { int kind; buf_t data; buf_t uris; buf_t gnome; Time t; int own; } held;   /* clipboard we own */
#define MAX_INCR 8
static struct { Window w; Atom prop, type; uint8_t *d; size_t n, off; int used; } incr[MAX_INCR];
static size_t incr_chunk = 65536, max_direct = 200000;
static char peer_name[128] = "the other computer";   /* the peer's name (link, then its HELLO) */
static int here = 1;                  /* the pointer is on this computer (told by main) */
static uid_t desk_uid; static gid_t desk_gid; static char desk_home[256] = "/root";   /* the desktop's user */
static int xfer_seq, xfer_in = -1;    /* transfers shown in the tray app */
static char held_dir[PATH_MAX];       /* the received copy whose files are on the clipboard */
/* Pasted copies: removed once no other process has their files open for 10 s (PROTOCOL.md Kept copies). */
#define MAXWATCH 8
static struct { char dir[PATH_MAX]; int64_t requested, busy; } pw[MAXWATCH];
static int xfer_skipped;                 /* files the receiving computer already had, for the next xfer() */
static void xfer(int id, const char *dir, const char *what, uint64_t total, uint64_t done, const char *state, const char *reason) {
    char w[240]; snprintf(w, sizeof w, "%s", what); if (strlen(what) >= sizeof w) strcpy(w + sizeof w - 4, "...");
    buf_t b = {0}; bprintf(&b, "{\"id\": %d, \"dir\": \"%s\", \"what\": ", id, dir); json_put_str(&b, w);
    bprintf(&b, ", \"total\": %llu, \"done\": %llu, \"state\": \"%s\", \"reason\": ", (unsigned long long)total, (unsigned long long)done, state);
    json_put_str(&b, reason ? reason : ""); bprintf(&b, ", \"skipped\": %d}", xfer_skipped); hsend(hfd, 'T', b.p, b.n); bfree(&b);
}
static pid_t push_pid;                /* background file push */
/* XFixes, loaded at run time (the image has the library but not its headers) */
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window; int subtype;
                 Window owner; Atom selection; Time timestamp; Time selection_timestamp; } FixSelEvent;
static int fix_event = -1;
static void (*fix_hide)(Display *, Window), (*fix_show)(Display *, Window);
static int cursor_hidden;
/* While this computer drives the peer, its own pointer is hidden (it stays where it left). */
static void cursor_follow(void) {
    int want = !here;
    if (!dpy || !fix_hide || !fix_show || want == cursor_hidden) return;
    if (want) fix_hide(dpy, xroot); else fix_show(dpy, xroot);
    XFlush(dpy); cursor_hidden = want;
    uint8_t h = cursor_hidden; hsend(hfd, 'K', &h, 1);
    logmsg("pointer %s", want ? "hidden here (it is on the other computer)" : "shown here again");
}

static void herr(const char *fmt, ...) {
    char m[300]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);
    logmsg("%s", m); hsend(hfd, 'E', m, strlen(m));
}
static int xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static int xioerr(Display *d) { (void)d; logmsg("lost the desktop's X server"); _exit(3); }

/* Find the desktop X server (Xwayland or Xorg) and its auth file from its command line. */
/* The desktop's X server: the one named "display" in config.json, else the lowest display number
   (the main desktop; a second screen may run a second desktop on the next number). */
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

static void send_size(void) {
    Window r; int x, y; unsigned w, h, bw, dep;
    XGetGeometry(dpy, xroot, &r, &x, &y, &w, &h, &bw, &dep);
    int32_t s[2] = {(int32_t)w, (int32_t)h}; hsend(hfd, 'Z', s, 8);
}

static int x_connect(void) {
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
    send_size();
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
    cursor_hidden = 0; cursor_follow();
    return 1;
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
    if (r->selection == A_CLIP && held.own && held.kind == 3 && held_dir[0] && r->requestor != xwin && (r->target == A_URIS || r->target == A_GNOME)) {
        int i, f = -1; int64_t now = mono_ms();   /* a paste of the received files */
        for (i = 0; i < MAXWATCH; i++) if (pw[i].dir[0] && !strcmp(pw[i].dir, held_dir)) break;
        if (i == MAXWATCH) { for (i = 0; i < MAXWATCH; i++) if (!pw[i].dir[0]) { f = i; break; } if (f >= 0) { i = f; snprintf(pw[i].dir, PATH_MAX, "%s", held_dir); pw[i].busy = 0; logmsg("received files asked for by another program (%s): removed once it has read them", strrchr(held_dir, '/') + 1); } }
        if (i < MAXWATCH) pw[i].requested = now;
    }
    if (r->selection == A_CLIP && held.own) {
        if (r->target == A_TARGETS) {
            Atom t[8]; int k = 0; t[k++] = A_TARGETS; t[k++] = A_TIMESTAMP;
            if (held.kind == 1) { t[k++] = A_UTF8; t[k++] = A_PLAIN_UTF8; t[k++] = A_PLAIN; t[k++] = XA_STRING; t[k++] = A_TEXT; }
            if (held.kind == 2) t[k++] = A_PNG;
            if (held.kind == 3) { t[k++] = A_URIS; t[k++] = A_GNOME; t[k++] = A_UTF8; t[k++] = A_PLAIN; }
            XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, k);
            ev.property = prop;
        } else if (r->target == A_TIMESTAMP) {
            long t = held.t; XChangeProperty(dpy, r->requestor, prop, XA_INTEGER, 32, PropModeReplace, (unsigned char *)&t, 1);
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
static void clip_push(void);
static void handle_xevent(XEvent *e) {
    if (fix_event >= 0 && e->type == fix_event) {
        FixSelEvent *f = (FixSelEvent *)e;
        if (f->selection == A_CLIP && f->owner != None && f->owner != xwin) clip_push();
        return;
    }
    if (e->type == SelectionRequest) serve_request(&e->xselectionrequest);
    else if (e->type == SelectionClear && e->xselectionclear.selection == A_CLIP) held.own = 0;
    else if (e->type == PropertyNotify && e->xproperty.window != xwin) incr_step(&e->xproperty);
    else if (e->type == ConfigureNotify && e->xconfigure.window == xroot) send_size();
}

/* --- reading the clipboard from another client --- */
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

/* files: collect (relpath, fullpath) and build CLIP_DATA body */
static buf_t *fl_out; static size_t fl_base; static int fl_count; static uint64_t fl_total; static int fl_fail;
static void add_file(const char *full, const char *rel) {
    int fd = open(full, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); return; }
    fl_total += st.st_size;
    if (fl_total > MAX_CLIP || fl_count >= 65535) { fl_fail = 1; close(fd); return; }
    uint8_t h[10]; size_t rl = strlen(rel); put16(h, rl); bput(fl_out, h, 2); bput(fl_out, rel, rl);
    put64(h, st.st_size); bput(fl_out, h, 8);
    uint8_t chunk[65536]; ssize_t r; uint64_t left = st.st_size;
    while (left && (r = read(fd, chunk, left < sizeof chunk ? left : sizeof chunk)) > 0) { bput(fl_out, chunk, r); left -= r; }
    while (left) { size_t k = left < sizeof chunk ? left : sizeof chunk; memset(chunk, 0, k); bput(fl_out, chunk, k); left -= k; }
    close(fd); fl_count++;
}
static int walk_cb(const char *path, const struct stat *st, int type, struct FTW *f) {
    (void)st; (void)f;
    if (type == FTW_F && !fl_fail) add_file(path, path + fl_base);
    return 0;
}
static void pct_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = {s[1], s[2], 0}; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
static int uris_to_files(char *list, buf_t *out) {
    uint8_t zero[3] = {3, 0, 0}; out->n = 0; bput(out, zero, 3);
    fl_out = out; fl_count = 0; fl_total = 0; fl_fail = 0;
    for (char *line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (strncmp(line, "file://", 7)) continue;
        char *p = line + 7; if (*p != '/') { p = strchr(p, '/'); if (!p) continue; }
        pct_decode(p);
        struct stat st; if (stat(p, &st)) continue;
        while (strlen(p) > 1 && p[strlen(p) - 1] == '/') p[strlen(p) - 1] = 0;
        const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
        fl_base = base - p;
        if (S_ISREG(st.st_mode)) add_file(p, base);
        else if (S_ISDIR(st.st_mode)) nftw(p, walk_cb, 16, FTW_PHYS);
    }
    put16(out->p + 1, fl_count);
    return fl_count > 0 && !fl_fail;
}

/* Local clipboard -> CLIP_DATA body. */
static void clip_read(buf_t *out) {
    out->n = 0;
    uint8_t k;
    if (!dpy) { k = 0; bput(out, &k, 1); return; }
    if (held.own && XGetSelectionOwner(dpy, A_CLIP) == xwin) {
        if (held.kind == 3) { buf_t u = {0}; bput(&u, held.uris.p, held.uris.n); bput(&u, "", 1); uris_to_files((char *)u.p, out); bfree(&u); return; }
        k = held.kind; bput(out, &k, 1); bput(out, held.data.p, held.data.n); return;
    }
    buf_t t = {0}, d = {0}; Atom type;
    if (XGetSelectionOwner(dpy, A_CLIP) == None || !x_convert(A_TARGETS, &t, &type)) { k = 0; bput(out, &k, 1); bfree(&t); return; }
    int n = t.n / sizeof(Atom); Atom *l = (Atom *)t.p;
    if (has_atom(l, n, A_URIS) && x_convert(A_URIS, &d, &type)) {
        bput(&d, "", 1); if (uris_to_files((char *)d.p, out)) goto done;
    }
    if (has_atom(l, n, A_GNOME) && x_convert(A_GNOME, &d, &type)) {
        bput(&d, "", 1); if (uris_to_files((char *)d.p, out)) goto done;
    }
    out->n = 0;
    if (has_atom(l, n, A_PNG) && x_convert(A_PNG, &d, &type) && d.n) { k = 2; bput(out, &k, 1); bput(out, d.p, d.n); goto done; }
    if ((has_atom(l, n, A_UTF8) && x_convert(A_UTF8, &d, &type)) || (has_atom(l, n, A_PLAIN_UTF8) && x_convert(A_PLAIN_UTF8, &d, &type)) ||
        (has_atom(l, n, XA_STRING) && x_convert(XA_STRING, &d, &type)) || (has_atom(l, n, A_PLAIN) && x_convert(A_PLAIN, &d, &type))) {
        k = 1; bput(out, &k, 1); bput(out, d.p, d.n); goto done;
    }
    k = 0; bput(out, &k, 1);
done:
    bfree(&t); bfree(&d);
}

/* Write received files under ~/Downloads/From <peer name>/<time>/ ; fills uri lists. */
static int mkdir_owned(const char *p) {
    if (mkdir(p, 0755) && errno != EEXIST) return -1;
    if (chown(p, desk_uid, desk_gid)) return -1;
    return 0;
}
static void pct_encode(buf_t *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = *s;
        if (isalnum(c) || strchr("/-._~", c)) bput(b, s, 1);
        else { char h[4]; snprintf(h, sizeof h, "%%%02X", c); bput(b, h, 3); }
    }
}
static int own_files_list(const char *dir, char tops[][256], int ntops);
static int tcp_frame_recv(int fd, buf_t *plain);
static int tcp_frame_send(int fd, const uint8_t *m, size_t n);
static int new_recv_dir(char *dir, size_t dn) {
    char stamp[64]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
    char base[600], dl[300]; snprintf(dl, sizeof dl, "%s/Downloads", desk_home); snprintf(base, sizeof base, "%s/From %s", dl, peer_name);
    mkdir_owned(dl); mkdir_owned(base);
    snprintf(dir, dn, "%s/%s", base, stamp);
    for (int i = 2; access(dir, F_OK) == 0; i++) snprintf(dir, dn, "%s/%s (%d)", base, stamp, i);
    return mkdir_owned(dir);
}
static int safe_rel(const char *rel) {
    if (!rel[0] || rel[0] == '/' || strchr(rel, '\\') || strstr(rel, "//")) return 0;
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *c = strtok(tmp, "/"); c; c = strtok(NULL, "/")) if (!strcmp(c, "..") || !strcmp(c, ".")) return 0;
    return 1;
}
static void clip_own_files(void);

/* ---------- kept copies of received files (PROTOCOL.md Kept copies) ---------- */
#define MARKER ".shenzhen-kvm.json"
#define KEPT_COPIES 5
static int dated_name(const char *n) {   /* "YYYY-MM-DD HH.MM.SS", optionally " 2" or " (2)" */
    if (strlen(n) < 19) return 0;
    for (int i = 0; i < 19; i++) {
        char c = n[i];
        if (i == 4 || i == 7) { if (c != '-') return 0; } else if (i == 10) { if (c != ' ') return 0; }
        else if (i == 13 || i == 16) { if (c != '.') return 0; } else if (!isdigit((unsigned char)c)) return 0;
    }
    const char *r = n + 19; if (!*r) return 1;
    if (*r++ != ' ') return 0;
    int paren = *r == '('; if (paren) r++;
    if (!isdigit((unsigned char)*r)) return 0; while (isdigit((unsigned char)*r)) r++;
    if (paren && *r++ != ')') return 0;
    return !*r;
}
static void recv_base(char *base, size_t n) { snprintf(base, n, "%s/Downloads/From %s", desk_home, peer_name); }
static char *read_file_all(const char *path, size_t max) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    buf_t b = {0}; char tmp[65536]; size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0 && b.n < max) bput(&b, tmp, k);
    fclose(f); bz(&b); return (char *)b.p;
}
typedef struct { char path[PATH_MAX]; double created; } kept_t;
static int kept_cmp(const void *a, const void *b) { double x = ((const kept_t *)a)->created, y = ((const kept_t *)b)->created; return x < y ? -1 : x > y; }
/* The folders the service made under base (date-time name and our marker), oldest first. */
static int kept_list(const char *base, kept_t **out) {
    DIR *d = opendir(base); struct dirent *e; int n = 0, cap = 0; *out = NULL;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (!dated_name(e->d_name)) continue;
        char p[PATH_MAX], m[PATH_MAX + 32]; struct stat sb;
        snprintf(p, sizeof p, "%s/%s", base, e->d_name);
        if (lstat(p, &sb) || !S_ISDIR(sb.st_mode)) continue;
        snprintf(m, sizeof m, "%s/%s", p, MARKER);
        char *j = read_file_all(m, 1 << 16); if (!j) continue;
        char by[32] = ""; json_str(j, "by", by, sizeof by); char cr[32] = ""; json_str(j, "created", cr, sizeof cr); free(j);
        if (strcmp(by, "shenzhen-kvm")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; *out = realloc(*out, cap * sizeof **out); }
        snprintf((*out)[n].path, PATH_MAX, "%s", p); (*out)[n].created = atof(cr); n++;
    }
    closedir(d);
    if (n) qsort(*out, n, sizeof **out, kept_cmp);
    return n;
}
static double mtime_of(const struct stat *sb) { return sb->st_mtim.tv_sec + sb->st_mtim.tv_nsec / 1e9; }
/* A file the kept copies hold with this SHA-256 and size, unchanged since it arrived: its path in src. */
static int held_file(const char *base, const char *shahex, uint64_t size, char *src, size_t sn) {
    kept_t *k; int n = kept_list(base, &k), found = 0;
    for (int i = n - 1; i >= 0 && !found; i--) {
        char m[PATH_MAX + 32]; snprintf(m, sizeof m, "%s/%s", k[i].path, MARKER);
        char *j = read_file_all(m, 64u << 20); if (!j) continue;
        const char *p = json_find(j, "files"); char obj[2400];
        while (p && !found && (p = json_next_obj(p, obj, sizeof obj))) {
            char sh[80] = "", rel[1100] = "", sz[32] = "", mt[48] = "";
            json_str(obj, "sha256", sh, sizeof sh); json_str(obj, "size", sz, sizeof sz);
            if (strcmp(sh, shahex) || strtoull(sz, NULL, 10) != size) continue;
            json_str(obj, "path", rel, sizeof rel); json_str(obj, "mtime", mt, sizeof mt);
            if (!safe_rel(rel)) continue;
            char full[PATH_MAX]; struct stat sb; snprintf(full, sizeof full, "%s/%s", k[i].path, rel);
            if (lstat(full, &sb) || !S_ISREG(sb.st_mode) || (uint64_t)sb.st_size != size || (mtime_of(&sb) - atof(mt) > 0.001 || atof(mt) - mtime_of(&sb) > 0.001)) continue;
            snprintf(src, sn, "%s", full); found = 1;
        }
        free(j);
    }
    free(k); return found;
}
/* Marks dir as ours and records what it holds. */
static void write_marker(const char *dir, char (*rel)[1024], const uint64_t *sz, uint8_t (*sha)[32], unsigned cnt) {
    buf_t b = {0}; bprintf(&b, "{\"by\": \"shenzhen-kvm\", \"v\": 1, \"created\": %.6f, \"files\": [", now_us() / 1e6);
    for (unsigned i = 0; i < cnt; i++) {
        char full[PATH_MAX], hx[65]; struct stat sb; snprintf(full, sizeof full, "%s/%s", dir, rel[i]);
        double mt = lstat(full, &sb) == 0 ? mtime_of(&sb) : 0; tohex(sha[i], 32, hx);
        bputs(&b, i ? ", {\"path\": " : "{\"path\": "); json_put_str(&b, rel[i]);
        bprintf(&b, ", \"size\": %llu, \"sha256\": \"%s\", \"mtime\": %.9f}", (unsigned long long)sz[i], hx, mt);
    }
    bputs(&b, "]}\n");
    char m[PATH_MAX + 32]; snprintf(m, sizeof m, "%s/%s", dir, MARKER);
    int f = open(m, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (f >= 0) { write_all(f, b.p, b.n); fchown(f, desk_uid, desk_gid); close(f); }
    bfree(&b);
}
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; return remove(p) ? 0 : 0; }
static int path_in(const char *list, const char *dir) {   /* a path in list (or its %-encoded form) is inside dir */
    if (!list) return 0;
    char pre[PATH_MAX + 2]; snprintf(pre, sizeof pre, "%s/", dir);
    if (strstr(list, pre)) return 1;
    buf_t enc = {0}; pct_encode(&enc, pre); bz(&enc); int hit = strstr(list, (char *)enc.p) != NULL; bfree(&enc);
    return hit;
}
/* Keeps the newest kept copies; never one whose files are on the clipboard or the one just made.
   Removes base when it ends up empty. */
static void roll_kept(const char *keep_dir) {
    char base[PATH_MAX]; recv_base(base, sizeof base);
    kept_t *k; int n = kept_list(base, &k), excess = n - KEPT_COPIES;
    char *other = NULL;   /* files another program put on the clipboard (a file manager) */
    if (excess > 0 && dpy) {
        Window o = XGetSelectionOwner(dpy, A_CLIP);
        if (o != None && o != xwin) { buf_t d = {0}; Atom ty; if (x_convert(A_URIS, &d, &ty)) { bz(&d); other = (char *)d.p; } else bfree(&d); }
    }
    buf_t mine = {0}; if (held.own) { bput(&mine, held.uris.p, held.uris.n); bput(&mine, "\n", 1); bput(&mine, held.data.p, held.data.n); } bz(&mine);
    for (int i = 0; i < n && excess > 0; i++) {
        if (!strcmp(k[i].path, keep_dir) || path_in((char *)mine.p, k[i].path) || path_in(other, k[i].path)) continue;
        struct stat sb; if (lstat(k[i].path, &sb) || !S_ISDIR(sb.st_mode)) continue;
        nftw(k[i].path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        logmsg("removed an older copy of received files (%s)", strrchr(k[i].path, '/') + 1);
        excess--;
    }
    rmdir(base);   /* only when empty */
    free(k); free(other); bfree(&mine);
}
/* The kept copy into the new folder: a reflink when the file system has them, else a plain copy. */
static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644); if (out < 0) { close(in); return -1; }
    int ok = ioctl(out, FICLONE, in) == 0;
    if (!ok) { char b[65536]; ssize_t r; ok = 1; while ((r = read(in, b, sizeof b)) > 0) if (write_all(out, b, r)) { ok = 0; break; } if (r < 0) ok = 0; }
    fchown(out, desk_uid, desk_gid); close(in); close(out); return ok ? 0 : -1;
}
static int sha256_file(const char *path, uint8_t out[32]) {
    int f = open(path, O_RDONLY | O_CLOEXEC); if (f < 0) return -1;
    crypto_hash_sha256_state h; crypto_hash_sha256_init(&h); static uint8_t b[1 << 20]; ssize_t r;
    while ((r = read(f, b, sizeof b)) > 0) crypto_hash_sha256_update(&h, b, r);
    close(f); crypto_hash_sha256_final(&h, out); return r < 0 ? -1 : 0;
}

/* FILES_OFFER (offer) or FILES_BEGIN header in p (after the type byte); FILES_DATA frames follow on fd. */
static int files_receive_stream(int fd, const uint8_t *p, size_t n, int offer) {
    if (n < 2) return 0;
    unsigned cnt = get16(p); size_t off = 2;
    char (*rel)[1024] = calloc(cnt ? cnt : 1, 1024); uint64_t *sz = calloc(cnt ? cnt : 1, 8); uint64_t total = 0;
    uint8_t (*want)[32] = calloc(cnt ? cnt : 1, 32), (*sha)[32] = calloc(cnt ? cnt : 1, 32); char **from = calloc(cnt ? cnt : 1, sizeof(char *));
    unsigned nheld = 0; char dir[PATH_MAX] = ""; buf_t fr = {0};
    for (unsigned i = 0; i < cnt; i++) {
        if (off + 2 > n) goto fail;
        unsigned nl = get16(p + off); off += 2;
        if (nl == 0 || nl >= 1024 || off + nl + 8 + (offer ? 32 : 0) > n) goto fail;
        memcpy(rel[i], p + off, nl); rel[i][nl] = 0; off += nl; sz[i] = get64(p + off); off += 8;
        if (offer) { memcpy(want[i], p + off, 32); off += 32; }
        if (!safe_rel(rel[i])) { herr("refused file name %s", rel[i]); goto fail; }
    }
    char base[PATH_MAX]; recv_base(base, sizeof base);
    if (offer) {   /* what the kept copies already hold is not sent again */
        buf_t have = {0}; uint8_t t = 0x2B, b2[2]; bput(&have, &t, 1); put16(b2, 0); bput(&have, b2, 2);
        for (unsigned i = 0; i < cnt; i++) {
            char hx[65], src[PATH_MAX]; tohex(want[i], 32, hx);
            if (held_file(base, hx, sz[i], src, sizeof src)) { from[i] = strdup(src); put16(b2, i); bput(&have, b2, 2); nheld++; }
        }
        put16(have.p + 1, nheld);
        int bad = tcp_frame_send(fd, have.p, have.n); bfree(&have);
        if (bad) goto fail;
    }
    for (unsigned i = 0; i < cnt; i++) if (!from[i]) total += sz[i];
    if (new_recv_dir(dir, sizeof dir)) { herr("cannot create %s", dir); dir[0] = 0; goto fail; }
    char tops[64][256]; int ntops = 0; size_t fo = 0; uint64_t got = 0; int64_t shown = 0;
    if (xfer_in < 0) xfer_in = ++xfer_seq;
    char what[300]; snprintf(what, sizeof what, "%.*s", (int)strcspn(cnt ? rel[0] : "files", "/"), cnt ? rel[0] : "files");
    if (cnt > 1) { int tops_n = 0; for (unsigned i = 0; i < cnt; i++) if (!i || strncmp(rel[i], rel[0], strcspn(rel[0], "/") + 1)) tops_n++; if (tops_n > 1) snprintf(what, sizeof what, "%u files", cnt); }
    xfer_skipped = nheld; xfer(xfer_in, "in", what, total, 0, "running", NULL);
    for (unsigned i = 0; i < cnt; i++) {
        char full[PATH_MAX]; snprintf(full, sizeof full, "%s/%s", dir, rel[i]);
        for (char *s2 = full + strlen(dir) + 1; (s2 = strchr(s2, '/')); s2++) { *s2 = 0; mkdir_owned(full); *s2 = '/'; }
        if (from[i]) {
            if (copy_file(from[i], full)) { herr("cannot copy %s", rel[i]); goto fail; }
            memcpy(sha[i], want[i], 32);
        } else {
            int f = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (f < 0) { herr("cannot write %s", full); goto fail; }
            crypto_hash_sha256_state h; crypto_hash_sha256_init(&h);
            uint64_t left = sz[i];
            while (left) {
                if (fo >= fr.n) {
                    if (tcp_frame_recv(fd, &fr) || fr.n < 1 || fr.p[0] != 0x28) { close(f); herr("%s stopped sending files", peer_name); goto fail; }
                    fo = 1; got += fr.n - 1;
                    if (mono_ms() - shown > 250) { shown = mono_ms(); xfer(xfer_in, "in", what, total, got, "running", NULL); }
                }
                size_t k = fr.n - fo < left ? fr.n - fo : left;
                write_all(f, fr.p + fo, k); crypto_hash_sha256_update(&h, fr.p + fo, k); fo += k; left -= k;
            }
            fchown(f, desk_uid, desk_gid); close(f);
            crypto_hash_sha256_final(&h, sha[i]);
            if (offer && memcmp(sha[i], want[i], 32)) { logmsg("refused %s: its content does not match what was announced", rel[i]); herr("%s arrived damaged and was not kept", rel[i]); goto fail; }
        }
        char top[256]; snprintf(top, sizeof top, "%.*s", (int)strcspn(rel[i], "/"), rel[i]);
        int seen = 0; for (int k2 = 0; k2 < ntops; k2++) if (!strcmp(tops[k2], top)) seen = 1;
        if (!seen && ntops < 64) strcpy(tops[ntops++], top);
    }
    write_marker(dir, rel, sz, sha, cnt);
    bfree(&fr);
    if (!own_files_list(dir, tops, ntops)) goto fail_kept;
    clip_own_files();
    xfer(xfer_in, "in", what, total, total, "done", NULL); xfer_in = -1; xfer_skipped = 0;
    logmsg("%u file(s) from %s in %s (%llu bytes received, %u already here)", cnt, peer_name, dir, (unsigned long long)total, nheld);
    roll_kept(dir);
    for (unsigned i = 0; i < cnt; i++) free(from[i]);
    free(rel); free(sz); free(want); free(sha); free(from);
    return 1;
fail:
    if (dir[0]) { nftw(dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }
fail_kept:
    if (xfer_in >= 0) { xfer(xfer_in, "in", "files", total, 0, "failed", "the transfer stopped before the end"); xfer_in = -1; }
    xfer_skipped = 0; bfree(&fr);
    for (unsigned i = 0; i < cnt; i++) free(from[i]);
    free(rel); free(sz); free(want); free(sha); free(from); return 0;
}

static int files_receive(const uint8_t *p, size_t n) {   /* files in one piece (CLIP_DATA kind 3) */
    char dir[PATH_MAX];
    if (n < 2) return 0;
    if (new_recv_dir(dir, sizeof dir)) { herr("cannot create %s", dir); return 0; }
    unsigned cnt = get16(p); size_t off = 2;
    char (*rel)[1024] = calloc(cnt ? cnt : 1, 1024); uint64_t *szs = calloc(cnt ? cnt : 1, 8); uint8_t (*sha)[32] = calloc(cnt ? cnt : 1, 32); unsigned made = 0;
    char tops[64][256]; int ntops = 0;
    for (unsigned i = 0; i < cnt; i++) {
        if (off + 2 > n) break;
        unsigned nl = get16(p + off); off += 2;
        if (off + nl + 8 > n || nl == 0 || nl >= 1024) break;
        char r1[1024]; memcpy(r1, p + off, nl); r1[nl] = 0; off += nl;
        uint64_t sz = get64(p + off); off += 8;
        if (sz > n - off) break;
        const uint8_t *data = p + off; off += sz;
        if (!safe_rel(r1)) { herr("refused file name %s", r1); continue; }
        char full[PATH_MAX]; snprintf(full, sizeof full, "%s/%s", dir, r1);
        for (char *s = full + strlen(dir) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir_owned(full); *s = '/'; }
        int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) { herr("cannot write %s", full); continue; }
        write_all(fd, data, sz); fchown(fd, desk_uid, desk_gid); close(fd);
        snprintf(rel[made], 1024, "%s", r1); szs[made] = sz; crypto_hash_sha256(sha[made], data, sz); made++;
        char top[256]; snprintf(top, sizeof top, "%.*s", (int)strcspn(r1, "/"), r1);
        int seen = 0; for (int k = 0; k < ntops; k++) if (!strcmp(tops[k], top)) seen = 1;
        if (!seen && ntops < 64) strcpy(tops[ntops++], top);
    }
    write_marker(dir, rel, szs, sha, made);
    free(rel); free(szs); free(sha);
    int ok = own_files_list(dir, tops, ntops);
    if (ok) roll_kept(dir);
    return ok;
}
static int own_files_list(const char *dir, char tops[][256], int ntops) {
    if (!ntops) { rmdir(dir); return 0; }
    snprintf(held_dir, sizeof held_dir, "%s", dir);
    held.uris.n = held.gnome.n = held.data.n = 0;
    bput(&held.gnome, "copy", 4);
    for (int k = 0; k < ntops; k++) {
        char full[PATH_MAX]; snprintf(full, sizeof full, "%s/%s", dir, tops[k]);
        bput(&held.uris, "file://", 7); pct_encode(&held.uris, full); bput(&held.uris, "\r\n", 2);
        bput(&held.gnome, "\nfile://", 8); pct_encode(&held.gnome, full);
        if (k) bput(&held.data, "\n", 1);
        bput(&held.data, full, strlen(full));
    }
    return ntops > 0;
}

static uint64_t sz_total;
static int size_cb(const char *path, const struct stat *s2, int type, struct FTW *f) {
    (void)path; (void)f; if (type == FTW_F && S_ISREG(s2->st_mode)) sz_total += s2->st_size; return 0;
}
static uint64_t uris_size(const char *list0) {
    char *list = strdup(list0); sz_total = 0;
    for (char *line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (strncmp(line, "file://", 7)) continue;
        char *p = line + 7; if (*p != '/') { p = strchr(p, '/'); if (!p) continue; }
        pct_decode(p); struct stat s2; if (stat(p, &s2)) continue;
        if (S_ISREG(s2.st_mode)) sz_total += s2.st_size; else if (S_ISDIR(s2.st_mode)) nftw(p, size_cb, 16, FTW_PHYS);
    }
    free(list); return sz_total;
}
/* --- files in pieces (PROTOCOL.md FILES_BEGIN / FILES_DATA) --- */
typedef struct { char rel[1024], full[PATH_MAX]; uint64_t size; } fent_t;
static fent_t *fents; static int nfents, cap_fents; static size_t fe_base;
static void fe_add(const char *full, const char *rel, uint64_t size) {
    if (nfents >= 65535 || strlen(rel) >= 1024) return;
    if (nfents == cap_fents) { cap_fents = cap_fents ? cap_fents * 2 : 64; fents = realloc(fents, cap_fents * sizeof *fents); }
    snprintf(fents[nfents].rel, sizeof fents[nfents].rel, "%s", rel); snprintf(fents[nfents].full, sizeof fents[nfents].full, "%s", full);
    fents[nfents++].size = size;
}
static int fe_cb(const char *path, const struct stat *s2, int type, struct FTW *f) {
    (void)f; if (type == FTW_F && S_ISREG(s2->st_mode)) fe_add(path, path + fe_base, s2->st_size); return 0;
}
static void collect_files(const char *list0) {
    char *list = strdup(list0); nfents = 0;
    for (char *line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (strncmp(line, "file://", 7)) continue;
        char *p = line + 7; if (*p != '/') { p = strchr(p, '/'); if (!p) continue; }
        pct_decode(p); struct stat s2; if (stat(p, &s2)) continue;
        while (strlen(p) > 1 && p[strlen(p) - 1] == '/') p[strlen(p) - 1] = 0;
        const char *base = strrchr(p, '/'); base = base ? base + 1 : p; fe_base = base - p;
        if (S_ISREG(s2.st_mode)) fe_add(p, base, s2.st_size); else if (S_ISDIR(s2.st_mode)) nftw(p, fe_cb, 16, FTW_PHYS);
    }
    free(list);
}
static int tcp_connect_peer(int ms);
static int tcp_frame_send(int fd, const uint8_t *m, size_t n);
static int tcp_frame_recv(int fd, buf_t *plain);
/* Send the collected files to the peer in pieces. Returns 0 when it has saved them. */
static int out_id; static char out_what[300]; static uint64_t out_total;
/* The FILES_DATA frames of the files not in skip, then the ACK. */
static int send_data(int fd, const char *skip) {
    uint64_t sent = 0, total = 0; int64_t shown = 0; int bad = 0;
    for (int i = 0; i < nfents; i++) if (!skip || !skip[i]) total += fents[i].size;
    out_total = total;
    static uint8_t chunk[1 + (1 << 20)]; chunk[0] = 0x28;
    for (int i = 0; i < nfents && !bad; i++) {
        if (skip && skip[i]) continue;
        int f = open(fents[i].full, O_RDONLY | O_CLOEXEC); uint64_t left = fents[i].size;
        while (left && !bad) {
            size_t want = left < (1u << 20) ? left : (1u << 20); ssize_t r = f >= 0 ? read(f, chunk + 1, want) : 0;
            if (r <= 0) { memset(chunk + 1, 0, want); r = want; }   /* file shrank: keep the promised size */
            bad = tcp_frame_send(fd, chunk, r + 1); left -= r; sent += r;
            if (mono_ms() - shown > 250) { shown = mono_ms(); xfer(out_id, "out", out_what, out_total, sent, "running", NULL); }
        }
        if (f >= 0) close(f);
    }
    buf_t rep = {0}; if (!bad) bad = tcp_frame_recv(fd, &rep) || rep.n < 1 || rep.p[0] != 0x02;
    bfree(&rep); return bad ? -1 : 0;
}
/* Send the collected files to the peer (PROTOCOL.md FILES_OFFER; FILES_BEGIN with everything when it
   is an older version). Returns 0 when it has saved them. */
static int send_files_stream(void) {
    uint8_t (*sha)[32] = calloc(nfents ? nfents : 1, 32); char *skip = calloc(nfents ? nfents : 1, 1); int r = -1;
    for (int i = 0; i < nfents; i++) if (sha256_file(fents[i].full, sha[i])) { free(sha); free(skip); return -1; }
    int fd = tcp_connect_peer(1500); if (fd < 0) { free(sha); free(skip); return -1; }
    int offered = 0;
    if (!getenv("SHENZHEN_KVM_NO_OFFER")) {
        buf_t h = {0}; uint8_t t = 0x2A, b[10]; bput(&h, &t, 1); put16(b, nfents); bput(&h, b, 2);
        for (int i = 0; i < nfents; i++) { size_t l = strlen(fents[i].rel); put16(b, l); bput(&h, b, 2); bput(&h, fents[i].rel, l); put64(b, fents[i].size); bput(&h, b, 8); bput(&h, sha[i], 32); }
        buf_t rep = {0};
        if (!tcp_frame_send(fd, h.p, h.n) && !tcp_frame_recv(fd, &rep) && rep.n >= 3 && rep.p[0] == 0x2B) {
            unsigned k = get16(rep.p + 1); offered = 1;
            for (unsigned i = 0; i < k && 3 + 2 * i + 1 < rep.n; i++) { unsigned x = get16(rep.p + 3 + 2 * i); if (x < (unsigned)nfents) { skip[x] = 1; xfer_skipped++; } }
        }
        bfree(&h); bfree(&rep);
        if (!offered) { close(fd); fd = tcp_connect_peer(1500); if (fd < 0) { free(sha); free(skip); return -1; } }   /* an older version */
    }
    if (offered) r = send_data(fd, skip);
    else {
        buf_t h = {0}; uint8_t t = 0x27, b[10]; bput(&h, &t, 1); put16(b, nfents); bput(&h, b, 2);
        for (int i = 0; i < nfents; i++) { size_t l = strlen(fents[i].rel); put16(b, l); bput(&h, b, 2); bput(&h, fents[i].rel, l); put64(b, fents[i].size); bput(&h, b, 8); }
        r = tcp_frame_send(fd, h.p, h.n) ? -1 : send_data(fd, NULL); bfree(&h);
    }
    close(fd); free(sha); free(skip); return r;
}

static int peer_request(const uint8_t *m, size_t n, buf_t *rep);
static void send_u64s(uint8_t type, uint64_t a, uint64_t b, int two) {
    uint8_t m[17] = {type}; put64(m + 1, a); put64(m + 9, b); buf_t rep = {0}; peer_request(m, two ? 17 : 9, &rep); bfree(&rep);
}
/* The clipboard here changed (another program copied) while the pointer is here: give it to the peer. */
static void clip_push(void) {
    if (!here || !st.sharing || !st.clipboard || !st.usual) return;
    buf_t t = {0}, d = {0}; Atom type;
    if (!x_convert(A_TARGETS, &t, &type)) { bfree(&t); return; }
    int n = t.n / sizeof(Atom); Atom *l = (Atom *)t.p;
    int files = (has_atom(l, n, A_URIS) && x_convert(A_URIS, &d, &type)) || (has_atom(l, n, A_GNOME) && x_convert(A_GNOME, &d, &type));
    if (files) {
        bput(&d, "", 1);
        uint64_t total = uris_size((char *)d.p);
        if (total > 0) {
            uint64_t limit = (uint64_t)st.limit_mb << 20;
            char what[300] = "files"; { char *nl = strpbrk((char *)d.p, "\r\n"); const char *f0 = strncmp((char *)d.p, "file://", 7) ? (char *)d.p : (char *)d.p + 7;
                size_t l = nl ? (size_t)(nl - f0) : strlen(f0); char tmp[1024]; snprintf(tmp, sizeof tmp, "%.*s", (int)(l < 1000 ? l : 1000), f0); pct_decode(tmp);
                const char *base = strrchr(tmp, '/'); snprintf(what, sizeof what, "%s", base && base[1] ? base + 1 : tmp);
                if (nl && strstr(nl, "file://")) snprintf(what, sizeof what, "several files"); }
            int id = ++xfer_seq;
            if (!st.big_files && total > limit) {
                char why[120]; snprintf(why, sizeof why, "not sent: over the %ld MB limit", st.limit_mb); xfer(id, "out", what, total, 0, "failed", why);
                send_u64s(0x25, total, limit, 1);
                herr("files copied here were not sent to %s: %llu MB is over the %ld MB limit", peer_name, (unsigned long long)(total >> 20), st.limit_mb);
            } else if (total > MAX_CLIP) {
                xfer(id, "out", what, total, 0, "failed", "too big to send");
                send_u64s(0x25, total, MAX_CLIP, 1);
                herr("files copied here are too big to send (%llu MB)", (unsigned long long)(total >> 20));
            } else {
                if (push_pid > 0) { kill(push_pid, SIGKILL); waitpid(push_pid, NULL, 0); push_pid = 0; }
                send_u64s(0x24, total, 0, 0);
                out_id = id; snprintf(out_what, sizeof out_what, "%s", what); out_total = total;
                xfer(id, "out", what, total, 0, "waiting", NULL);
                pid_t p = fork();
                if (p == 0) {   /* no X calls in here */
                    collect_files((char *)d.p);
                    int ok = nfents > 0 && send_files_stream() == 0;
                    if (!ok) send_u64s(0x24, 0, 0, 0);
                    char why[200]; snprintf(why, sizeof why, "%s did not take the files", peer_name);
                    xfer(id, "out", what, ok ? out_total : total, ok ? out_total : 0, ok ? "done" : "failed", ok ? NULL : why);
                    logmsg("files sent to %s: %s", peer_name, ok ? "done" : "failed");
                    _exit(0);
                }
                push_pid = p > 0 ? p : 0;
                logmsg("files copied here (%llu bytes) are being sent to %s", (unsigned long long)total, peer_name);
            }
            bfree(&t); bfree(&d); return;
        }
    }
    buf_t m = {0}, rep = {0}; uint8_t ty = 0x22, k = 0; bput(&m, &ty, 1);
    if (has_atom(l, n, A_PNG) && x_convert(A_PNG, &d, &type) && d.n) { k = 2; bput(&m, &k, 1); bput(&m, d.p, d.n); }
    else if ((has_atom(l, n, A_UTF8) && x_convert(A_UTF8, &d, &type)) || (has_atom(l, n, A_PLAIN_UTF8) && x_convert(A_PLAIN_UTF8, &d, &type)) ||
             (has_atom(l, n, XA_STRING) && x_convert(XA_STRING, &d, &type)) || (has_atom(l, n, A_PLAIN) && x_convert(A_PLAIN, &d, &type))) {
        k = 1; bput(&m, &k, 1); bput(&m, d.p, d.n);
    }
    if (k) {
        int id = ++xfer_seq, ok = peer_request(m.p, m.n, &rep) == 0;
        char why[200]; snprintf(why, sizeof why, "%s did not answer", peer_name);
        xfer(id, "out", k == 1 ? "text" : "picture", m.n - 2, ok ? m.n - 2 : 0, ok ? "done" : "failed", ok ? NULL : why);
    }
    bfree(&t); bfree(&d); bfree(&m); bfree(&rep);
}

static void clip_own_files(void) {
    held.kind = 3; held.t = x_time();
    XSetSelectionOwner(dpy, A_CLIP, xwin, held.t);
    held.own = XGetSelectionOwner(dpy, A_CLIP) == xwin; XFlush(dpy);
    if (!held.own) herr("could not take the clipboard");
}

/* CLIP_DATA body -> local clipboard */
static int clip_set(const uint8_t *p, size_t n) {
    if (!dpy) { herr("no desktop to put the clipboard on"); return 0; }
    if (n < 1) return 0;
    int kind = p[0];
    held.data.n = 0;
    if (kind == 1 || kind == 2) bput(&held.data, p + 1, n - 1);
    else if (kind == 3) { if (!files_receive(p + 1, n - 1)) return 0; }
    else return 1;
    held.kind = kind;
    held.t = x_time();
    XSetSelectionOwner(dpy, A_CLIP, xwin, held.t);
    held.own = XGetSelectionOwner(dpy, A_CLIP) == xwin;
    XFlush(dpy);
    if (!held.own) herr("could not take the clipboard");
    return held.own;
}

/* --- TCP: client side and server side --- */
static int tcp_frame_send(int fd, const uint8_t *m, size_t n) {
    uint8_t *f = malloc(n + 32); size_t c = seal(m, n, f + 4); put32(f, c);
    int r = write_all(fd, f, c + 4); free(f); return r;
}
static int tcp_frame_recv(int fd, buf_t *plain) {
    uint8_t h[4]; if (read_all(fd, h, 4)) return -1;
    uint32_t n = get32(h); if (n < 28 || n > MAX_CLIP + 4096) return -1;
    uint8_t *c = malloc(n); if (read_all(fd, c, n)) { free(c); return -1; }
    plain->n = 0; if (plain->cap < n) { plain->cap = n; plain->p = realloc(plain->p, n); }
    uint64_t ctr; long m = unseal(c, n, plain->p, &ctr); free(c);
    if (m < 1) return -1;
    plain->n = m; return 0;
}
static int tcp_connect_peer(int ms) {
    struct sockaddr_in a; if (peer_sockaddr(&a, shm->peer_dp + 1)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (void *)&a, sizeof a) && errno != EINPROGRESS) { close(fd); return -1; }
    struct pollfd q = {fd, POLLOUT, 0}; int err = 0; socklen_t el = sizeof err;
    if (poll(&q, 1, ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); return -1; }
    fcntl(fd, F_SETFL, 0);
    struct timeval tv = {15, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}
/* one request to the peer; reply in rep */
static int peer_request(const uint8_t *m, size_t n, buf_t *rep) {
    int fd = tcp_connect_peer(1500);
    if (fd < 0) { logmsg("%s does not answer", peer_name); return -1; }
    int r = tcp_frame_send(fd, m, n) || tcp_frame_recv(fd, rep) ? -1 : 0;
    close(fd);
    if (r) logmsg("clipboard exchange with %s failed", peer_name);
    return r;
}

static void tcp_serve(int lfd) {
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int fd = accept4(lfd, (void *)&from, &fl, SOCK_CLOEXEC); if (fd < 0) return;
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {15, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    buf_t in = {0}, out = {0};
    if (tcp_frame_recv(fd, &in) == 0) {
        uint8_t t = in.p[0];
        if (from.sin_family == AF_INET) peer_heard(from.sin_addr.s_addr, NULL);
        if (t == 0x20) {   /* HELLO: answer with this computer's name; the peer's name is the one people see */
            char j[512], v[128]; size_t n = in.n - 1 < sizeof j - 1 ? in.n - 1 : sizeof j - 1; memcpy(j, in.p + 1, n); j[n] = 0;
            if (json_str(j, "name", v, sizeof v) && v[0]) {
                for (char *c = v; *c; c++) if (*c == '/') *c = '-';
                if (strcmp(v, peer_name)) { snprintf(peer_name, sizeof peer_name, "%s", v); hsend_str(hfd, 'N', peer_name); }
            }
            buf_t h = {0}; uint8_t ty = 0x20; bput(&h, &ty, 1); bputs(&h, "{\"name\": "); json_put_str(&h, my_name); bputs(&h, ", \"version\": 1}");
            tcp_frame_send(fd, h.p, h.n); bfree(&h);
        }
        else if (t == 0x21) { uint8_t ty = 0x23; bput(&out, &ty, 1); buf_t b = {0}; clip_read(&b); bput(&out, b.p, b.n); bfree(&b); tcp_frame_send(fd, out.p, out.n); }
        else if (t == 0x22) {
            uint8_t a[2] = {0x02, 0x22}; tcp_frame_send(fd, a, 2);   /* ack first: the sender need not wait for the files to be written */
            if (st.sharing && st.clipboard && in.n > 1 && in.p[1] != 0) {
                int ok = clip_set(in.p + 1, in.n - 1), id = ++xfer_seq;
                xfer(id, "in", in.p[1] == 1 ? "text" : in.p[1] == 2 ? "picture" : "files", in.n - 2, ok ? in.n - 2 : 0, ok ? "done" : "failed", ok ? NULL : "it could not be put on the clipboard");
            }
            if (in.n > 1 && in.p[1] == 3) { uint8_t w = 0; hsend(hfd, 'W', &w, 1); }
        }
        else if (t == 0x27 || (t == 0x2A && !getenv("SHENZHEN_KVM_NO_OFFER"))) {
            int ok = dpy && st.sharing && st.clipboard && files_receive_stream(fd, in.p + 1, in.n - 1, t == 0x2A);
            if (ok) { uint8_t a[2] = {0x02, t}; tcp_frame_send(fd, a, 2); }
            uint8_t w = 0; hsend(hfd, 'W', &w, 1);
        }
        else if (t == 0x24 && in.n >= 9) {
            uint8_t a[2] = {0x02, 0x24}; tcp_frame_send(fd, a, 2);
            uint8_t w = get64(in.p + 1) && st.sharing && st.clipboard ? 1 : 0; hsend(hfd, 'W', &w, 1);
            if (w) { if (xfer_in < 0) xfer_in = ++xfer_seq; xfer(xfer_in, "in", "files", get64(in.p + 1), 0, "waiting", NULL); }
            else if (!get64(in.p + 1) && xfer_in >= 0) { char why[200]; snprintf(why, sizeof why, "%s stopped sending", peer_name); xfer(xfer_in, "in", "files", 0, 0, "failed", why); xfer_in = -1; }
        }
        else if (t == 0x25 && in.n >= 17) {
            uint8_t a[2] = {0x02, 0x25}; tcp_frame_send(fd, a, 2);
            char why[120]; snprintf(why, sizeof why, "not sent: over the %llu MB limit", (unsigned long long)(get64(in.p + 9) >> 20));
            xfer(++xfer_seq, "in", "files", get64(in.p + 1), 0, "failed", why);
        }
        else if (t == 0x26) {
            uint8_t a[2] = {0x02, 0x26}; tcp_frame_send(fd, a, 2);
            char j[4096]; size_t n = in.n - 1 < sizeof j - 1 ? in.n - 1 : sizeof j - 1; memcpy(j, in.p + 1, n); j[n] = 0;
            hsend(hfd, 'G', j, n);   /* main adopts it when newer and tells the helper back */
        }
        else if (t == 0x29) {   /* FORGET: the person forgot this link on the other computer */
            uint8_t a[2] = {0x02, 0x29}; tcp_frame_send(fd, a, 2);
            hsend(hfd, 'F', NULL, 0);
        }
    }
    bfree(&in); bfree(&out); close(fd);
}

static int active_is_terminal(void) {
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
static Window snap_owner; static long snap_ts;
static long sel_timestamp(void) {
    buf_t b = {0}; Atom t; long v = 0;
    if (x_convert(A_TIMESTAMP, &b, &t) && b.n >= sizeof(long)) memcpy(&v, b.p, sizeof v);
    bfree(&b); return v;
}

static void helper_cmd(uint8_t t, buf_t *b) {
    if (t == 'H' && b->n >= 1) { here = b->p[0]; cursor_follow(); return; }
    if (t == 'G') {   /* main's current settings */
        char j[4096]; size_t n = b->n < sizeof j - 2 ? b->n : sizeof j - 2; memcpy(j, b->p, n); j[n] = 0;
        parse_local_settings(j); return;
    }
    if (t == 'S') {   /* send these settings (wire form) to the peer */
        buf_t m = {0}, rep = {0}; uint8_t ty = 0x26; bput(&m, &ty, 1); bput(&m, b->p, b->n);
        int ok = peer_request(m.p, m.n, &rep) == 0 && rep.n >= 1 && rep.p[0] == 0x02; bfree(&m); bfree(&rep);
        uint8_t r = ok; hsend(hfd, 's', &r, 1); return;
    }
    if (t == 'f') {   /* tell the peer this link was forgotten here */
        uint8_t m = 0x29; buf_t rep = {0}; peer_request(&m, 1, &rep); bfree(&rep); hsend(hfd, 'f', NULL, 0); return;
    }
    if (t == 'Q') {
        int32_t r[6] = {0, 0, 0, 0, 0, b->n ? b->p[0] : 0};
        if (dpy) {
            Window rr, cr; int rx, ry, wx, wy; unsigned m;
            XQueryPointer(dpy, xroot, &rr, &cr, &rx, &ry, &wx, &wy, &m);
            r[0] = rx; r[1] = ry; r[2] = m; r[3] = DisplayWidth(dpy, DefaultScreen(dpy)); r[4] = DisplayHeight(dpy, DefaultScreen(dpy));
            Window g; int gx, gy; unsigned gw, gh, bw, dep; XGetGeometry(dpy, xroot, &g, &gx, &gy, &gw, &gh, &bw, &dep); r[3] = gw; r[4] = gh;
        }
        hsend(hfd, 'X', r, sizeof r);
    } else if (t == 'c' || t == 'v' || t == 'P' || t == 'R') {
        uint8_t term = dpy ? active_is_terminal() : 0;
        if (t == 'c') {   /* snapshot before the local copy keys */
            snap_owner = dpy ? XGetSelectionOwner(dpy, A_CLIP) : None;
            snap_ts = (dpy && snap_owner && snap_owner != xwin) ? sel_timestamp() : 0;
            hsend(hfd, 'k', &term, 1);
        } else if (t == 'v') {   /* local paste: fetch from the peer first */
            uint8_t g = 0x21; buf_t rep = {0}; uint8_t r[2] = {term, 0};
            if (peer_request(&g, 1, &rep) == 0 && rep.n >= 2 && rep.p[0] == 0x23) r[1] = clip_set(rep.p + 1, rep.n - 1) && rep.p[1] != 0;
            bfree(&rep); hsend(hfd, 'p', r, 2);
        } else if (t == 'P') {   /* remote paste: send ours to the peer first */
            buf_t m = {0}, rep = {0}, c = {0}; uint8_t ty = 0x22; bput(&m, &ty, 1); clip_read(&c); bput(&m, c.p, c.n);
            uint8_t ok = c.n > 1 && peer_request(m.p, m.n, &rep) == 0 && rep.n >= 1 && rep.p[0] == 0x02;
            bfree(&m); bfree(&rep); bfree(&c); hsend(hfd, 'o', &ok, 1);
        } else {   /* R: remote copy happened on the peer: give it time, then fetch */
            usleep(250000);
            uint8_t g = 0x21; buf_t rep = {0};
            if (peer_request(&g, 1, &rep) == 0 && rep.n >= 2 && rep.p[0] == 0x23) clip_set(rep.p + 1, rep.n - 1);
            bfree(&rep);
        }
    } else if (t == 'C') {   /* local copy keys were pressed: wait for the change, then put to the peer */
        if (dpy) {
            for (int64_t end = mono_ms() + 300; mono_ms() < end;) {
                Window o = XGetSelectionOwner(dpy, A_CLIP);
                if (o != snap_owner || (o && o != xwin && sel_timestamp() != snap_ts)) break;
                XEvent e; while (XPending(dpy)) { XNextEvent(dpy, &e); handle_xevent(&e); }
                usleep(15000);
            }
        }
        buf_t m = {0}, rep = {0}, c = {0}; uint8_t ty = 0x22; bput(&m, &ty, 1); clip_read(&c); bput(&m, c.p, c.n);
        if (c.n > 1) peer_request(m.p, m.n, &rep);
        bfree(&m); bfree(&rep); bfree(&c);
    }
}

/* Another process (not this service) has a file inside dir open. */
static int dir_in_use(const char *dir) {
    char pre[PATH_MAX + 2]; snprintf(pre, sizeof pre, "%s/", dir); size_t pl = strlen(pre);
    DIR *d = opendir("/proc"); struct dirent *e; int busy = 0;
    if (!d) return 0;
    while (!busy && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[64], comm[32] = ""; snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        if (read_small(p, comm, sizeof comm) > 0 && !strncmp(comm, "shenzhen-kvm", 12)) continue;
        snprintf(p, sizeof p, "/proc/%s/fd", e->d_name);
        DIR *fdd = opendir(p); struct dirent *f; if (!fdd) continue;
        while (!busy && (f = readdir(fdd))) {
            char l[PATH_MAX + 64], t[PATH_MAX]; snprintf(l, sizeof l, "%s/%s", p, f->d_name);
            ssize_t n = readlink(l, t, sizeof t - 1); if (n <= 0) continue; t[n] = 0;
            if (!strncmp(t, pre, pl)) busy = 1;
        }
        closedir(fdd);
    }
    closedir(d); return busy;
}
static int paste_watch_tick(void) {   /* returns 1 while something is watched */
    int64_t now = mono_ms(); int any = 0;
    for (int i = 0; i < MAXWATCH; i++) {
        if (!pw[i].dir[0]) continue;
        struct stat sb; if (lstat(pw[i].dir, &sb) || !S_ISDIR(sb.st_mode)) { pw[i].dir[0] = 0; continue; }
        any = 1;
        if (dir_in_use(pw[i].dir)) { pw[i].busy = now; continue; }
        int64_t last = pw[i].busy > pw[i].requested ? pw[i].busy : pw[i].requested;
        if (now - last < 10000) continue;
        nftw(pw[i].dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        logmsg("removed pasted files (%s)", strrchr(pw[i].dir, '/') + 1);
        if (held.own && !strcmp(held_dir, pw[i].dir) && dpy) {   /* the clipboard would offer files that are gone */
            XSetSelectionOwner(dpy, A_CLIP, None, CurrentTime); XFlush(dpy); held.own = 0; held_dir[0] = 0;
        }
        char base[PATH_MAX]; snprintf(base, sizeof base, "%s", pw[i].dir); char *sl = strrchr(base, '/'); if (sl) { *sl = 0; rmdir(base); }
        pw[i].dir[0] = 0;
    }
    return any;
}

static void helper_main(int fd) {
    hfd = fd; who_am_i = "helper";
    signal(SIGPIPE, SIG_IGN);
    snprintf(peer_name, sizeof peer_name, "%s", lk.peer_name);
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT_TCP)};
    if (bind(lfd, (void *)&a, sizeof a) || listen(lfd, 8)) { herr("cannot listen on TCP %d: %s", PORT_TCP, strerror(errno)); _exit(4); }
    int64_t next_try = 0;
    buf_t b = {0};
    for (;;) {
        if (!dpy && mono_ms() >= next_try) { if (!x_connect()) next_try = mono_ms() + 2000; }
        struct pollfd q[3] = {{hfd, POLLIN, 0}, {lfd, POLLIN, 0}, {dpy ? ConnectionNumber(dpy) : -1, POLLIN, 0}};
        if (dpy) while (XPending(dpy)) { XEvent e; XNextEvent(dpy, &e); handle_xevent(&e); }
        int watching = 0; for (int i = 0; i < MAXWATCH; i++) if (pw[i].dir[0]) watching = 1;
        if (poll(q, 3, watching ? 1000 : dpy ? 5000 : 2000) < 0 && errno != EINTR) _exit(5);
        if (watching) paste_watch_tick();
        if (push_pid > 0 && waitpid(push_pid, NULL, WNOHANG) == push_pid) push_pid = 0;
        if (q[0].revents) { uint8_t t; if (hrecv(hfd, &t, &b)) _exit(0); helper_cmd(t, &b); }
        if (q[1].revents & POLLIN) tcp_serve(lfd);
        if (dpy && (q[2].revents & (POLLHUP | POLLERR))) xioerr(dpy);
        if (dpy && (q[2].revents & POLLIN)) { XEvent e; if (!XPending(dpy)) XSync(dpy, False); while (XPending(dpy)) { XNextEvent(dpy, &e); handle_xevent(&e); } }
    }
}
