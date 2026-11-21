/* ======================================================================================
 * THE DESKTOP under Weston (15-desk.c), through the kvm-desk.so module's socket
 * RUN_DIR/desk.sock. Frame: u32 big-endian payload length (the type byte not counted),
 * u8 type, payload: JSON for control frames, raw bytes for DATA (0x90).
 *
 * The module takes one client, so the main process owns the connection: it places the pointer
 * with WARP on the input path (never waiting on the helper's clipboard work) and passes the
 * helper's requests through. The module answers requests in order, so main keeps a queue of
 * who asked. Helper <-> main messages:
 *   helper -> main  'D'  u32 seq, then the request's frames (CLIP_SET with its DATA frames)
 *   main -> helper  'd'  u32 seq, u8 frame type, payload (the reply, then a CLIP_GET's DATA)
 *   main -> helper  'e'  u8 event type, JSON (COPIED, ASKED)
 *   main -> helper  'Y'  u8: 1 the module answered (HELLO), 0 the connection is gone
 * ====================================================================================== */
enum { WD_HELLO = 0x01, WD_SIZE = 0x02, WD_POINTER = 0x03, WD_COPIED = 0x04, WD_ASKED = 0x05,
       WD_GET_POINTER = 0x10, WD_HIDE_CURSOR = 0x11, WD_CLIP_GET = 0x12, WD_CLIP_SET = 0x13, WD_CLIP_DROP = 0x14,
       WD_CLIP_OURS = 0x15, WD_WARP = 0x16, WD_OK = 0x80, WD_ERR = 0x81, WD_DATA = 0x90 };
#include <grp.h>
#define WD_MAX_DATA (16u << 20)        /* the module's limit for one CLIP_SET and one CLIP_GET */
static void wd_path(char *p, size_t n) { snprintf(p, n, "%s/desk.sock", RUN_DIR); }
/* Weston's module is there: its socket exists and takes a connection (a socket left by a Weston that
   stopped does not). Only while main is not connected: the module takes one client at a time. */
static int wd_socket_alive(void) {
    char p[320]; struct stat sb; wd_path(p, sizeof p);
    if (stat(p, &sb) || !S_ISSOCK(sb.st_mode)) return 0;
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof a.sun_path, "%s", p);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), ok = fd >= 0 && connect(fd, (void *)&a, sizeof a) == 0;
    if (fd >= 0) close(fd);
    return ok;
}
static void wd_frame(buf_t *b, uint8_t t, const void *p, uint32_t n) {
    uint8_t h[5] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, t};
    bput(b, h, 5); if (n) bput(b, p, n);
}
static uint32_t wd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static double wd_num(const char *j, const char *name, double def) { char v[48]; return json_str(j, name, v, sizeof v) ? atof(v) : def; }

/* ------------------------------------------------------------------ main's half */
static int wm_fd = -1, wm_hello;
static buf_t wm_in, wm_out; static size_t wm_out_off;
#define WM_FIFO 1024
static struct { uint8_t who; uint32_t seq; } wm_fifo[WM_FIFO];   /* who asked, oldest first: 0 main's WARP, 1 the helper, 2 a helper that went away, 3 main's GET_POINTER */
static int wm_head, wm_count;
static int wm_data_who = -1; static uint32_t wm_data_seq;          /* who gets the DATA frame after a CLIP_GET's OK */
static int wm_warps, wm_has_target; static double wm_tx, wm_ty;    /* WARPs unanswered; a WARP waiting for them */
static int64_t wm_warp_ms, wm_next_try, wm_gone_since;
static int wm_errs, wm_queries;
static int64_t wm_pointer_ms, wm_query_ms;   /* the last POINTER event; the last GET_POINTER */

static int wm_busy(void) { return wm_fd >= 0 && (wm_warps > 0 || wm_has_target); }
static void wm_write(void);
static void wm_lost(const char *why);
static void wm_queue(uint8_t t, const void *p, uint32_t n) { wd_frame(&wm_out, t, p, n); wm_write(); }
static void wm_write(void) {
    while (wm_fd >= 0 && wm_out_off < wm_out.n) {
        ssize_t w = write(wm_fd, wm_out.p + wm_out_off, wm_out.n - wm_out_off);
        if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) return; wm_lost(strerror(errno)); return; }
        wm_out_off += w;
    }
    if (wm_out_off == wm_out.n) wm_out.n = wm_out_off = 0;
}
static int wm_push(uint8_t who, uint32_t seq) {
    if (wm_count == WM_FIFO) return -1;
    int i = (wm_head + wm_count++) % WM_FIFO; wm_fifo[i].who = who; wm_fifo[i].seq = seq; return 0;
}
static void wm_reply_helper(uint32_t seq, uint8_t t, const void *p, uint32_t n) {
    buf_t m = {0}; bput(&m, &seq, 4); bput(&m, &t, 1); if (n) bput(&m, p, n);
    desk_to_helper('d', m.p, m.n); bfree(&m);
}
static void wm_send_warp(double x, double y) {
    char j[96]; int n = snprintf(j, sizeof j, "{\"x\":%.2f,\"y\":%.2f}", x, y);
    wm_push(0, 0); wm_warps++; wm_warp_ms = mono_ms(); wm_queue(WD_WARP, j, n);
}
/* Puts the pointer at x, y (global layout coordinates). At most two WARPs are on their way; a newer
   position waits and replaces any older one waiting. */
static void wm_warp(double x, double y) {
    if (wm_fd < 0 || !wm_hello) return;
    if (wm_warps >= 2) { wm_has_target = 1; wm_tx = x; wm_ty = y; return; }
    wm_send_warp(x, y);
}
/* This computer's own mouse moved: Weston reports where the pointer went (POINTER); should that not
   come (the pointer held against an edge), ask (GET_POINTER). */
static void wm_query(void) {
    int64_t now = mono_ms();
    if (wm_fd < 0 || !wm_hello || wm_queries || now - wm_pointer_ms < 20 || now - wm_query_ms < 8) return;
    wm_queries++; wm_query_ms = now; wm_push(3, 0); wm_queue(WD_GET_POINTER, NULL, 0);
}
static void wm_lost(const char *why) {
    if (wm_fd < 0) return;
    close(wm_fd); wm_fd = -1;
    logmsg("lost the desktop (Weston): %s", why);
    static const char gone[] = "{\"msg\":\"the desktop went away\"}";
    for (; wm_count; wm_count--, wm_head = (wm_head + 1) % WM_FIFO)
        if (wm_fifo[wm_head].who == 1) wm_reply_helper(wm_fifo[wm_head].seq, WD_ERR, gone, sizeof gone - 1);
    wm_in.n = 0; wm_out.n = wm_out_off = 0; wm_head = 0; wm_data_who = -1;
    wm_warps = wm_has_target = wm_queries = 0; wm_hello = 0;
    wm_next_try = mono_ms() + 1000; wm_gone_since = mono_ms();
    uint8_t up = 0; desk_to_helper('Y', &up, 1);
    desk_settled(0, 0, 0);
}
static int wm_connect(void) {
    if (wm_fd >= 0) return 1;
    wm_next_try = mono_ms() + 1000;
    char p[320]; wd_path(p, sizeof p);
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof a.sun_path, "%s", p);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return 0;
    if (connect(fd, (void *)&a, sizeof a)) { close(fd); return 0; }
    wm_fd = fd; wm_hello = 0; wm_in.n = 0; wm_out.n = wm_out_off = 0; wm_head = wm_count = 0; wm_data_who = -1;
    wm_warps = wm_has_target = wm_queries = 0; wm_errs = 0; wm_gone_since = 0;
    logmsg("connected to the desktop (Weston, %s)", p);
    return 1;
}
static void wm_layout(const char *j) {
    int r[4 * 8], n = 0; char o[512];
    const char *p = json_find(j, "outputs");
    if (p && *p == '[')
        while (n < 8 && (p = json_next_obj(p, o, sizeof o))) {
            int w = (int)wd_num(o, "w", 0), h = (int)wd_num(o, "h", 0);
            if (w <= 0 || h <= 0) continue;
            r[4 * n] = (int)wd_num(o, "x", 0); r[4 * n + 1] = (int)wd_num(o, "y", 0); r[4 * n + 2] = w; r[4 * n + 3] = h; n++;
        }
    if (!n) { int w = (int)wd_num(j, "w", 0), h = (int)wd_num(j, "h", 0); if (w <= 0 || h <= 0) return; r[0] = r[1] = 0; r[2] = w; r[3] = h; n = 1; }
    desk_layout(n, r);
}
static void wm_got(uint8_t t, const uint8_t *p, uint32_t n) {
    static char *j; static size_t jcap;
    if (t != WD_DATA) {   /* control frames are JSON: a NUL-terminated copy */
        if (n + 1 > jcap) { jcap = n + 1; j = realloc(j, jcap); if (!j) abort(); }
        memcpy(j, p, n); j[n] = 0;
    }
    switch (t) {
    case WD_HELLO: case WD_SIZE:
        wm_layout(j);
        if (t == WD_HELLO && !wm_hello) { wm_hello = 1; uint8_t up = 1; desk_to_helper('Y', &up, 1); }
        return;
    case WD_POINTER: wm_pointer_ms = mono_ms(); desk_pointer_seen(wd_num(j, "x", 0), wd_num(j, "y", 0), (unsigned)wd_num(j, "b", 0)); return;
    case WD_COPIED: case WD_ASKED: {
        buf_t m = {0}; bput(&m, &t, 1); bput(&m, p, n); desk_to_helper('e', m.p, m.n); bfree(&m); return;
    }
    case WD_OK: case WD_ERR: {
        if (!wm_count) { logmsg("the desktop answered a request nobody made"); return; }
        int who = wm_fifo[wm_head].who; uint32_t seq = wm_fifo[wm_head].seq;
        wm_head = (wm_head + 1) % WM_FIFO; wm_count--;
        int data = t == WD_OK && json_find(j, "len");
        if (who == 0) {   /* a WARP */
            wm_warps--;
            if (t == WD_ERR && wm_errs++ < 3) { char m[200] = ""; json_str(j, "msg", m, sizeof m); logmsg("the desktop did not move the pointer: %s", m); }
            if (wm_has_target) { wm_has_target = 0; wm_send_warp(wm_tx, wm_ty); }
            else if (!wm_warps) desk_settled(t == WD_OK, wd_num(j, "x", 0), wd_num(j, "y", 0));
            return;
        }
        if (who == 3) { wm_queries--; if (t == WD_OK) desk_pointer_seen(wd_num(j, "x", 0), wd_num(j, "y", 0), (unsigned)wd_num(j, "b", 0)); return; }
        if (who == 1) wm_reply_helper(seq, t, p, n);
        if (data) { wm_data_who = who; wm_data_seq = seq; }
        return;
    }
    case WD_DATA:
        if (wm_data_who == 1) wm_reply_helper(wm_data_seq, WD_DATA, p, n);
        wm_data_who = -1;
        return;
    }
}
static void wm_read(void) {
    for (;;) {
        if (wm_in.cap - wm_in.n < 65536) { wm_in.cap = wm_in.n + 262144; wm_in.p = realloc(wm_in.p, wm_in.cap); if (!wm_in.p) abort(); }
        ssize_t r = read(wm_fd, wm_in.p + wm_in.n, wm_in.cap - wm_in.n);
        if (r == 0) { wm_lost("the module closed the connection"); return; }
        if (r < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) break; wm_lost(strerror(errno)); return; }
        wm_in.n += r;
    }
    size_t off = 0;
    while (wm_in.n - off >= 5) {
        uint32_t n = wd_be32(wm_in.p + off);
        if (n > WD_MAX_DATA + 1024) { wm_lost("a frame larger than the module sends"); return; }
        if (wm_in.n - off < 5 + (size_t)n) break;
        wm_got(wm_in.p[off + 4], wm_in.p + off + 5, n);
        if (wm_fd < 0) return;
        off += 5 + n;
    }
    memmove(wm_in.p, wm_in.p + off, wm_in.n - off); wm_in.n -= off;
}
static void wm_handle(short revents) {
    if (wm_fd < 0) return;
    if (revents & POLLOUT) wm_write();
    if (wm_fd >= 0 && (revents & (POLLIN | POLLHUP | POLLERR))) wm_read();
}
static short wm_events(void) { return POLLIN | (wm_out_off < wm_out.n ? POLLOUT : 0); }
/* The helper's request: forwarded in one piece (a CLIP_SET and its DATA frames stay together). */
static void wm_from_helper(const uint8_t *p, size_t n) {
    if (n < 4) return;
    uint32_t seq; memcpy(&seq, p, 4); p += 4; n -= 4;
    static const char no[] = "{\"msg\":\"no desktop\"}";
    int asks = 0;   /* every frame but DATA gets a reply */
    for (size_t o = 0; o + 5 <= n; o += 5 + wd_be32(p + o)) {
        if (o + 5 + wd_be32(p + o) > n) return;
        asks += p[o + 4] != WD_DATA;
    }
    if (wm_fd < 0 || !wm_hello || wm_count + asks > WM_FIFO) { while (asks--) wm_reply_helper(seq, WD_ERR, no, sizeof no - 1); return; }
    while (asks--) wm_push(1, seq);
    bput(&wm_out, p, n); wm_write();
}
static void wm_helper_gone(void) {   /* its answers go nowhere now */
    for (int k = 0; k < wm_count; k++) { int i = (wm_head + k) % WM_FIFO; if (wm_fifo[i].who == 1) wm_fifo[i].who = 2; }
    if (wm_data_who == 1) wm_data_who = 2;
}

/* ------------------------------------------------------------------ the helper's half */
static int wd_up, wd_attached;       /* main talks to the module; this helper took the desktop on */
static uint32_t wd_seq;
static buf_t wd_q; static size_t wd_q_off;   /* main's messages that came while waiting for an answer */
static uint64_t wd_stamp;
static char wd_mimes[16][128]; static int wd_nmimes = -1;   /* what the clipboard offers (-1: not known yet) */

static void wd_add_mime(const char *m, void *a) { (void)a; if (wd_nmimes >= 0 && wd_nmimes < 16) snprintf(wd_mimes[wd_nmimes++], 128, "%s", m); }
/* State from main's message, applied at once (its action comes when the message is handled). */
static void wd_note(uint8_t t, const buf_t *b) {
    if (t == 'Y' && b->n >= 1) {
        wd_up = b->p[0];
        if (!wd_up) { wd_attached = 0; held.own = 0; wd_nmimes = -1; }
    } else if (t == 'e' && b->n >= 1 && b->p[0] == WD_COPIED) {
        char j[8192]; size_t n = b->n - 1 < sizeof j - 1 ? b->n - 1 : sizeof j - 1; memcpy(j, b->p + 1, n); j[n] = 0;
        wd_stamp = (uint64_t)wd_num(j, "stamp", 0) + 1;
        wd_nmimes = 0; json_each_str(j, "mimes", wd_add_mime, NULL);
    }
}
static void wd_queue(uint8_t t, const buf_t *b) { uint32_t n = b->n; bput(&wd_q, &t, 1); bput(&wd_q, &n, 4); if (n) bput(&wd_q, b->p, n); }
static int wd_take_queued(uint8_t *t, buf_t *b) {
    if (wd_q_off >= wd_q.n) { wd_q.n = wd_q_off = 0; return 0; }
    uint32_t n; *t = wd_q.p[wd_q_off]; memcpy(&n, wd_q.p + wd_q_off + 1, 4);
    b->n = 0; if (n) bput(b, wd_q.p + wd_q_off + 5, n);
    wd_q_off += 5 + n; return 1;
}
/* main's Weston messages, from helper_cmd. */
static void wd_cmd(uint8_t t, buf_t *b) {
    wd_note(t, b);
    if (t != 'e' || b->n < 1) return;   /* 'Y' is all state; a stray 'd' is an answer nobody waits for any more */
    if (b->p[0] == WD_COPIED) desk_on_copied();
    else if (b->p[0] == WD_ASKED) {
        char j[1024]; size_t n = b->n - 1 < sizeof j - 1 ? b->n - 1 : sizeof j - 1; memcpy(j, b->p + 1, n); j[n] = 0;
        char m[256] = ""; json_str(j, "mime", m, sizeof m);
        if (held.own && held.kind == 3 && (!strcmp(m, "text/uri-list") || !strcmp(m, "x-special/gnome-copied-files"))) desk_on_files_asked();
    }
}
static void wd_drain(int ms) {   /* main's messages waiting on hfd, kept for later */
    buf_t b = {0}; uint8_t t;
    for (;;) {
        struct pollfd q = {hfd, POLLIN, 0};
        if (poll(&q, 1, ms) <= 0) break;
        ms = 0;
        if (hrecv(hfd, &t, &b)) _exit(0);
        wd_note(t, &b); wd_queue(t, &b);
    }
    bfree(&b);
}
/* One request through main: frames holds the request (and a CLIP_SET's DATA frames). The answer's
   JSON goes to rep, a CLIP_GET's bytes to data. Returns WD_OK or WD_ERR, 0 when no answer came. */
static int wd_rpc(const buf_t *frames, buf_t *rep, buf_t *data) {
    if (!wd_up) return 0;
    uint32_t seq = ++wd_seq;
    buf_t m = {0}; bput(&m, &seq, 4); bput(&m, frames->p, frames->n);
    int r = hsend(hfd, 'D', m.p, m.n); bfree(&m);
    if (r) return 0;
    int type = 0, want_data = 0; int64_t end = mono_ms() + 12000; buf_t b = {0};
    if (rep) rep->n = 0;
    while (mono_ms() < end) {
        struct pollfd q = {hfd, POLLIN, 0};
        if (poll(&q, 1, (int)(end - mono_ms())) <= 0) continue;
        uint8_t t; if (hrecv(hfd, &t, &b)) _exit(0);
        if (t == 'd' && b.n >= 5) {
            uint32_t s; memcpy(&s, b.p, 4);
            if (s != seq) continue;   /* the answer to a request given up on */
            if (b.p[4] == WD_DATA) { if (data) { data->n = 0; bput(data, b.p + 5, b.n - 5); } if (want_data) break; continue; }
            type = b.p[4];
            if (rep) { bput(rep, b.p + 5, b.n - 5); bz(rep); }
            if (type == WD_OK && rep && json_find((char *)rep->p, "len")) { want_data = 1; continue; }
            break;
        }
        wd_note(t, &b); wd_queue(t, &b);
    }
    if (!type) logmsg("the desktop did not answer");
    bfree(&b);
    return type;
}
static int wd_simple(uint8_t t, const char *json, buf_t *rep) {
    buf_t f = {0}; wd_frame(&f, t, json, json ? strlen(json) : 0);
    int r = wd_rpc(&f, rep, NULL); bfree(&f); return r;
}

static int wd_connect(void) {
    if (!wd_up) return 0;
    char p[320]; struct stat sb; wd_path(p, sizeof p);
    /* the desktop's user: the one named like the socket's group (the browser session's), else root */
    struct group *g = stat(p, &sb) == 0 ? getgrgid(sb.st_gid) : NULL;
    struct passwd *pw = g ? getpwnam(g->gr_name) : NULL;
    if (!pw) pw = getpwuid(0);
    desk_uid = pw ? pw->pw_uid : 0; desk_gid = pw ? pw->pw_gid : 0;
    snprintf(desk_home, sizeof desk_home, "%s", pw && pw->pw_dir ? pw->pw_dir : "/root");
    wd_attached = 1; wd_nmimes = -1;
    logmsg("connected to the desktop (Weston); received files go to %s", desk_home);
    return 1;
}
static int wd_connected(void) { return wd_up && wd_attached; }
static int wd_fd(void) { return -1; }   /* its events come from main */
static void wd_pump(short revents) { (void)revents; wd_drain(0); }
static int wd_pointer(int *x, int *y, unsigned *b, int *w, int *h) {
    buf_t r = {0}; int ok = wd_simple(WD_GET_POINTER, NULL, &r) == WD_OK;
    if (ok) { const char *j = (char *)r.p; *x = (int)wd_num(j, "x", 0); *y = (int)wd_num(j, "y", 0); *b = (unsigned)wd_num(j, "b", 0); *w = (int)wd_num(j, "w", 0); *h = (int)wd_num(j, "h", 0); }
    bfree(&r); return ok;
}
static int wd_hide_cursor(int hide) {
    buf_t r = {0}; int ok = wd_simple(WD_HIDE_CURSOR, hide ? "{\"hide\":true}" : "{\"hide\":false}", &r) == WD_OK; bfree(&r); return ok;
}
static int wd_clip_ours(void) {
    buf_t r = {0}; int ours = wd_simple(WD_CLIP_OURS, NULL, &r) == WD_OK && json_bool((char *)r.p, "ours", 0); bfree(&r);
    if (!ours) held.own = 0;
    return ours;
}
static const char *WD_TEXT[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
static int wd_offers(const char *m) {
    if (wd_nmimes < 0) return 1;   /* not known: ask */
    for (int i = 0; i < wd_nmimes; i++) if (!strcmp(wd_mimes[i], m)) return 1;
    return 0;
}
static int wd_clip_formats(void) {
    int f = 0;
    if (wd_offers("text/uri-list")) f |= 1 << CF_URIS;
    if (wd_offers("x-special/gnome-copied-files")) f |= 1 << CF_GNOME;
    if (wd_offers("image/png")) f |= 1 << CF_PNG;
    for (int i = 0; i < 5; i++) if (wd_offers(WD_TEXT[i])) f |= 1 << CF_TEXT;
    return f;
}
static int wd_clip_get(int cf, buf_t *out) {
    const char *one[1]; const char **list = one; int n = 1;
    if (cf == CF_URIS) one[0] = "text/uri-list"; else if (cf == CF_GNOME) one[0] = "x-special/gnome-copied-files";
    else if (cf == CF_PNG) one[0] = "image/png"; else { list = WD_TEXT; n = 5; }
    for (int i = 0; i < n; i++) {
        if (!wd_offers(list[i])) continue;
        buf_t f = {0}, r = {0}, req = {0};
        bputs(&req, "{\"mime\":"); json_put_str(&req, list[i]); bputs(&req, "}");
        wd_frame(&f, WD_CLIP_GET, req.p, req.n);
        out->n = 0;
        int ok = wd_rpc(&f, &r, out) == WD_OK && (cf != CF_PNG || out->n);
        bfree(&f); bfree(&r); bfree(&req);
        if (ok) return 1;
    }
    return 0;
}
static int wd_clip_take(void) {
    const char *mimes[3]; const buf_t *datas[3]; int n = 0;
    if (held.kind == 1) { mimes[0] = "text/plain;charset=utf-8"; mimes[1] = "text/plain"; mimes[2] = "UTF8_STRING"; datas[0] = datas[1] = datas[2] = &held.data; n = 3; }
    else if (held.kind == 2) { mimes[0] = "image/png"; datas[0] = &held.data; n = 1; }
    else if (held.kind == 3) { mimes[0] = "text/uri-list"; datas[0] = &held.uris; mimes[1] = "x-special/gnome-copied-files"; datas[1] = &held.gnome;
                               mimes[2] = "text/plain;charset=utf-8"; datas[2] = &held.data; n = 3; }
    else return 0;
    size_t total = 0; int k = 0;   /* the module takes at most 16 MiB in all: the first types that fit */
    while (k < n && total + datas[k]->n <= WD_MAX_DATA) total += datas[k++]->n;
    if (!k) { herr("too big for this desktop's clipboard (%zu MB; it takes 16 MB)", datas[0]->n >> 20); return 0; }
    buf_t j = {0}, f = {0}, r = {0};
    bputs(&j, "{\"mimes\":[");
    for (int i = 0; i < k; i++) { if (i) bputs(&j, ","); json_put_str(&j, mimes[i]); }
    bputs(&j, "]}");
    wd_frame(&f, WD_CLIP_SET, j.p, j.n);
    for (int i = 0; i < k; i++) wd_frame(&f, WD_DATA, datas[i]->p, datas[i]->n);
    int t = wd_rpc(&f, &r, NULL);
    if (t == WD_ERR) { char m[200] = ""; json_str((char *)r.p, "msg", m, sizeof m); logmsg("the desktop refused the clipboard: %s", m); }
    held.own = t == WD_OK;
    bfree(&j); bfree(&f); bfree(&r);
    return held.own;
}
static void wd_clip_drop(void) { buf_t r = {0}; wd_simple(WD_CLIP_DROP, NULL, &r); bfree(&r); held.own = 0; }
static uint64_t wd_clip_stamp(void) { return wd_stamp; }
