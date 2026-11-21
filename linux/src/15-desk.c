/* ======================================================================================
 * THE DESKTOP: everything the service needs from the display server, behind one small
 * interface. Two backends:
 *   16-desk-x11.c     X11 (Xorg, or Xwayland under a Wayland compositor)
 *   17-desk-weston.c  Weston with the kvm-desk.so module (its socket RUN_DIR/desk.sock)
 * Chosen when the service starts (and again when the desktop changes): config.json "desk":
 * "x11" or "weston", else Weston when its socket exists, else X11.
 * The helper process calls desk_*. Under X11 the main process places the pointer with an
 * absolute uinput device; under Weston it places it with WARP (main owns the socket and
 * passes the helper's requests through).
 * ====================================================================================== */
enum { DESK_X11 = 1, DESK_WESTON = 2 };
static int desk_kind = DESK_X11;
static const char *desk_name(int k) { return k == DESK_WESTON ? "Weston" : "X11"; }

/* Clipboard formats, for desk_clip_formats (bits) and desk_clip_get. */
enum { CF_URIS, CF_GNOME, CF_PNG, CF_TEXT };   /* text/uri-list, x-special/gnome-copied-files, image/png, UTF-8 text */
/* desk_pointer's button bits */
#define DESK_LEFT 1
#define DESK_MIDDLE 2
#define DESK_RIGHT 4

/* What this service put on the clipboard; the backend offers it to other programs.
   kind 1 text, 2 PNG (in data), 3 files: uris (text/uri-list), gnome (x-special/gnome-copied-files),
   data (the paths as text). own: the clipboard is still ours. */
static struct { int kind; buf_t data; buf_t uris; buf_t gnome; int own; } held;
static int hfd;   /* helper: its socket to main */
static uid_t desk_uid; static gid_t desk_gid; static char desk_home[256] = "/root";   /* the desktop's user (set by desk_connect) */

/* --- helper: the backend calls these (defined in 20-helper.c) --- */
static void desk_on_size(int w, int h);   /* the screen space has this size (on connect and when it changes) */
static void desk_on_copied(void);         /* another program put something on the clipboard */
static void desk_on_files_asked(void);    /* another program asks for the files this service holds (a paste) */
static void herr(const char *fmt, ...);   /* an error people should see */

/* --- main, Weston: the backend calls these (defined in 30-main.c and 35-run.c) --- */
static void desk_layout(int n, const int *rects);          /* the screens: n times x, y, w, h */
static void desk_pointer_seen(double x, double y, unsigned b);   /* the pointer moved (this computer's own mice) */
static void desk_settled(int have, double x, double y);   /* every WARP is applied; have: where the pointer is */
static int desk_to_helper(uint8_t t, const void *d, uint32_t n);

/* --- the backends --- */
static int  x11_connect(void), wd_connect(void);
static int  x11_connected(void), wd_connected(void);
static int  x11_fd(void), wd_fd(void);
static void x11_pump(short revents), wd_pump(short revents);
static int  x11_pointer(int *x, int *y, unsigned *buttons, int *w, int *h), wd_pointer(int *x, int *y, unsigned *buttons, int *w, int *h);
static int  x11_hide_cursor(int hide), wd_hide_cursor(int hide);
static int  x11_clip_ours(void), wd_clip_ours(void);
static int  x11_clip_formats(void), wd_clip_formats(void);
static int  x11_clip_get(int cf, buf_t *out), wd_clip_get(int cf, buf_t *out);
static int  x11_clip_take(void), wd_clip_take(void);
static void x11_clip_drop(void), wd_clip_drop(void);
static uint64_t x11_clip_stamp(void), wd_clip_stamp(void);
static int  x11_active_is_terminal(void);
#define WD (desk_kind == DESK_WESTON)

/* --- the interface (helper) --- */
/* finds the desktop and connects; 1 when connected */
static int  desk_connect(void) { return WD ? wd_connect() : x11_connect(); }
static int  desk_connected(void) { return WD ? wd_connected() : x11_connected(); }
/* to poll for input; -1 when there is none to poll */
static int  desk_fd(void) { return WD ? wd_fd() : x11_fd(); }
/* handles what the desktop sent; revents from poll on desk_fd (0: just what is queued) */
static void desk_pump(short revents) { if (WD) wd_pump(revents); else x11_pump(revents); }
/* where the pointer is, which buttons are held (DESK_*), the screen size */
static int  desk_pointer(int *x, int *y, unsigned *b, int *w, int *h) { return WD ? wd_pointer(x, y, b, w, h) : x11_pointer(x, y, b, w, h); }
static int  desk_hide_cursor(int hide) { return WD ? wd_hide_cursor(hide) : x11_hide_cursor(hide); }   /* 1 when done */
/* the clipboard still holds what this service put there */
static int  desk_clip_ours(void) { return WD ? wd_clip_ours() : x11_clip_ours(); }
/* CF_* bits the clipboard offers now (0: empty or unreadable) */
static int  desk_clip_formats(void) { return WD ? wd_clip_formats() : x11_clip_formats(); }
/* the clipboard in that format; 1 when read */
static int  desk_clip_get(int cf, buf_t *out) { return WD ? wd_clip_get(cf, out) : x11_clip_get(cf, out); }
/* offers `held` as the clipboard; 1 when it is ours */
static int  desk_clip_take(void) { return WD ? wd_clip_take() : x11_clip_take(); }
/* empties the clipboard while it is ours */
static void desk_clip_drop(void) { if (WD) wd_clip_drop(); else x11_clip_drop(); }
/* changes whenever something is copied anew */
static uint64_t desk_clip_stamp(void) { return WD ? wd_clip_stamp() : x11_clip_stamp(); }
/* the focused window is a terminal (its copy keys differ); Weston's single-app browser never is */
static int  desk_active_is_terminal(void) { return WD ? 0 : x11_active_is_terminal(); }
