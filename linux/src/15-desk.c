/* ======================================================================================
 * THE DESKTOP: everything the service needs from the display server, behind one small
 * interface. 16-desk-x11.c implements it for X11 (Xorg, or Xwayland under a Wayland
 * compositor). A Wayland backend (for example wlroots: virtual pointer, data control) would
 * implement the same desk_* functions; nothing else changes.
 * Only the helper process calls these.
 * ====================================================================================== */

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
static uid_t desk_uid; static gid_t desk_gid; static char desk_home[256] = "/root";   /* the desktop's user (set by desk_connect) */

/* --- the backend calls these (defined in 20-helper.c) --- */
static void desk_on_size(int w, int h);   /* the screen space has this size (on connect and when it changes) */
static void desk_on_copied(void);         /* another program put something on the clipboard */
static void desk_on_files_asked(void);    /* another program asks for the files this service holds (a paste) */
static void herr(const char *fmt, ...);   /* an error people should see */

/* --- the backend --- */
static int  desk_connect(void);                   /* finds the desktop and connects; 1 when connected */
static int  desk_connected(void);
static int  desk_fd(void);                        /* to poll for input; -1 when not connected */
static void desk_pump(short revents);             /* handles what the desktop sent; revents from poll on desk_fd (0: just what is queued) */
static int  desk_pointer(int *x, int *y, unsigned *buttons, int *w, int *h);   /* where the pointer is, which buttons are held, the screen size */
static int  desk_hide_cursor(int hide);           /* 1 when done */
static int  desk_clip_ours(void);                 /* the clipboard still holds what this service put there */
static int  desk_clip_formats(void);              /* CF_* bits the clipboard offers now (0: empty or unreadable) */
static int  desk_clip_get(int cf, buf_t *out);    /* the clipboard in that format; 1 when read */
static int  desk_clip_take(void);                 /* offers `held` as the clipboard; 1 when it is ours */
static void desk_clip_drop(void);                 /* empties the clipboard while it is ours */
static uint64_t desk_clip_stamp(void);            /* changes whenever something is copied anew */
static int  desk_active_is_terminal(void);        /* the focused window is a terminal (its copy keys differ) */
