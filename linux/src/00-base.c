/* shenzhen-kvm: the Linux half of Shenzhen KVM (keyboard, mouse and clipboard shared with another
 * computer). Wire protocol: ../PROTOCOL.md.
 * Build: gcc -O2 -o shenzhen-kvm shenzhen-kvm.c -lsodium -lX11   (shenzhen-kvm.c includes the parts in src/)
 *
 *   shenzhen-kvm run         the service (root, systemd): input, clipboard, finding and linking
 *   shenzhen-kvm status      the service's state as JSON
 *   shenzhen-kvm visible     make this computer visible and link with the computer that asks
 *   shenzhen-kvm link [NAME] find visible computers and link with one
 *   shenzhen-kvm forget      forget the linked computer
 *
 * Processes of the service. "main": everything on the input path (UDP socket -> uinput, physical
 * devices -> UDP; no sleeps, no batching) and the linked peer's presence. "helper": the X connection
 * (pointer queries, clipboard) and the TCP clipboard channel. "agent": the control socket for the
 * tray app and the command line, finding computers (visible / looking, mDNS) and linking. The
 * main process restarts the others when they stop, and restarts itself when the link changes.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <ifaddrs.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sodium.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static char CONF_DIR[256] = "/etc/shenzhen-kvm";            /* config.json: edge, speed, modifier keys */
static char STATE_DIR[256] = "/var/lib/shenzhen-kvm";       /* id, link, settings.json */
static char RUN_DIR[256] = "/run/shenzhen-kvm";             /* control.sock */
static char CONF_FILE[300], ID_FILE[300], LINK_FILE[300], SETTINGS_FILE[300], CONTROL_SOCK[300], HELD_FILE[300];
static int PORT_UDP = 24800, PORT_TCP = 24801, PORT_DISC = 24802;
#define MAX_CLIP (2000u << 20)
#define MDNS_TYPE "_shenzhen-kvm._udp"
#define MCAST_GROUP "239.255.83.90"
#define MAGIC "SZKVM1"
#define VISIBLE_SECONDS 300

static void init_paths(void) {
    const char *e;
    if ((e = getenv("SHENZHEN_KVM_CONF")) && *e) snprintf(CONF_DIR, sizeof CONF_DIR, "%s", e);
    if ((e = getenv("SHENZHEN_KVM_STATE")) && *e) snprintf(STATE_DIR, sizeof STATE_DIR, "%s", e);
    if ((e = getenv("SHENZHEN_KVM_RUN")) && *e) snprintf(RUN_DIR, sizeof RUN_DIR, "%s", e);
    snprintf(CONF_FILE, sizeof CONF_FILE, "%s/config.json", CONF_DIR);
    snprintf(ID_FILE, sizeof ID_FILE, "%s/id", STATE_DIR);
    snprintf(LINK_FILE, sizeof LINK_FILE, "%s/link", STATE_DIR);
    snprintf(SETTINGS_FILE, sizeof SETTINGS_FILE, "%s/settings.json", STATE_DIR);
    snprintf(CONTROL_SOCK, sizeof CONTROL_SOCK, "%s/control.sock", RUN_DIR);
    snprintf(HELD_FILE, sizeof HELD_FILE, "%s/held", STATE_DIR);
    if ((e = getenv("SHENZHEN_KVM_PORT_BASE")) && atoi(e) > 0 && atoi(e) < 65530) { PORT_UDP = atoi(e); PORT_TCP = PORT_UDP + 1; PORT_DISC = PORT_UDP + 2; }
}

/* ---------- small helpers ---------- */
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static int64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (56 - 8 * i); }
static unsigned get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
static const char *who_am_i = "main";
static void logmsg(const char *fmt, ...) {
    va_list a; va_start(a, fmt); fprintf(stderr, "%s: ", who_am_i); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr);
}

typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static void bput(buf_t *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) abort(); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bputs(buf_t *b, const char *s) { bput(b, s, strlen(s)); }
static void bprintf(buf_t *b, const char *fmt, ...) {
    char tmp[2048]; va_list a; va_start(a, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, a); va_end(a);
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1; if (n > 0) bput(b, tmp, n);
}
static void bfree(buf_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static void bz(buf_t *b) { uint8_t z = 0; bput(b, &z, 1); b->n--; }   /* NUL-terminate without counting it */

static int write_all(int fd, const void *d, size_t n) {
    const uint8_t *p = d;
    while (n) { ssize_t w = write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { struct pollfd q = {fd, POLLOUT, 0}; poll(&q, 1, 1000); continue; } return -1; } p += w; n -= w; }
    return 0;
}
static int read_all(int fd, void *d, size_t n) {
    uint8_t *p = d;
    while (n) { ssize_t r = read(fd, p, n); if (r == 0) return -1; if (r < 0) { if (errno == EINTR) continue; return -1; } p += r; n -= r; }
    return 0;
}
static void tohex(const uint8_t *b, size_t n, char *out) { static const char h[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; } out[2 * n] = 0; }
static int fromhex(const char *s, uint8_t *b, size_t n) { size_t bl = 0; return sodium_hex2bin(b, n, s, strlen(s), NULL, &bl, NULL) == 0 && bl == n ? 0 : -1; }

/* ---------- JSON (flat objects, enough for this protocol) ---------- */
/* Finds "name": and returns a pointer to its value, or NULL. */
static const char *json_find(const char *j, const char *name) {
    char pat[80]; snprintf(pat, sizeof pat, "\"%s\"", name);
    for (const char *p = j; (p = strstr(p, pat)); p += strlen(pat)) {
        const char *q = p + strlen(pat); while (isspace((unsigned char)*q)) q++;
        if (*q != ':') continue;
        q++; while (isspace((unsigned char)*q)) q++;
        return q;
    }
    return NULL;
}
/* String value with escapes decoded (\uXXXX to UTF-8), or a bare value (number, true...). */
static int json_str(const char *j, const char *name, char *out, size_t n) {
    const char *p = json_find(j, name); size_t i = 0;
    if (!p || !n) return 0;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) {
            unsigned c = (unsigned char)*p;
            if (c == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': {
                    unsigned v = 0; for (int k = 1; k <= 4; k++) { if (!isxdigit((unsigned char)p[k])) return 0; v = v * 16 + (isdigit((unsigned char)p[k]) ? p[k] - '0' : (tolower((unsigned char)p[k]) - 'a' + 10)); }
                    p += 4;
                    if (v >= 0xD800 && v <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                        unsigned w = 0; for (int k = 3; k <= 6; k++) { if (!isxdigit((unsigned char)p[k])) return 0; w = w * 16 + (isdigit((unsigned char)p[k]) ? p[k] - '0' : (tolower((unsigned char)p[k]) - 'a' + 10)); }
                        if (w >= 0xDC00 && w <= 0xDFFF) { v = 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00); p += 6; }
                    }
                    char u[4]; int k = 0;
                    if (v < 0x80) u[k++] = v; else if (v < 0x800) { u[k++] = 0xC0 | v >> 6; u[k++] = 0x80 | (v & 63); }
                    else if (v < 0x10000) { u[k++] = 0xE0 | v >> 12; u[k++] = 0x80 | ((v >> 6) & 63); u[k++] = 0x80 | (v & 63); }
                    else { u[k++] = 0xF0 | v >> 18; u[k++] = 0x80 | ((v >> 12) & 63); u[k++] = 0x80 | ((v >> 6) & 63); u[k++] = 0x80 | (v & 63); }
                    for (int m = 0; m < k; m++) { if (i + 1 >= n) return 0; out[i++] = u[m]; }
                    continue;
                }
                default: c = (unsigned char)*p;
                }
            }
            if (i + 1 >= n) return 0;
            out[i++] = c;
        }
        out[i] = 0; return 1;
    }
    while (*p && !strchr(",}]\n \t", *p) && i + 1 < n) out[i++] = *p++;
    out[i] = 0; return i > 0;
}
static long json_long(const char *j, const char *name, long def) { char v[32]; return json_str(j, name, v, sizeof v) ? atol(v) : def; }
static int json_bool(const char *j, const char *name, int def) {
    char v[16]; if (!json_str(j, name, v, sizeof v)) return def; return !strcmp(v, "true") ? 1 : !strcmp(v, "false") ? 0 : def;
}
/* Appends s as a JSON string (with quotes). */
static void json_put_str(buf_t *b, const char *s) {
    bputs(b, "\"");
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') { char e[2] = {'\\', *c}; bput(b, e, 2); }
        else if (*c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", *c); bputs(b, e); }
        else bput(b, c, 1);
    }
    bputs(b, "\"");
}
/* The strings of a JSON array value: calls f for each. */
static void json_each_str(const char *j, const char *name, void (*f)(const char *, void *), void *arg) {
    const char *p = json_find(j, name); if (!p || *p != '[') return;
    for (p++; *p && *p != ']'; ) {
        if (*p == '"') { const char *e = strchr(p + 1, '"'); if (!e) return; char v[128]; size_t l = e - p - 1; if (l < sizeof v) { memcpy(v, p + 1, l); v[l] = 0; f(v, arg); } p = e + 1; }
        else p++;
    }
}

/* The objects of a JSON array value, one at a time: returns a pointer after the object copied into out. */
static const char *json_next_obj(const char *p, char *out, size_t on) {
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;
    int depth = 0, instr = 0; const char *s = p;
    for (; *p; p++) {
        if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
        if (*p == '"') instr = 1; else if (*p == '{') depth++; else if (*p == '}' && !--depth) { p++; break; }
    }
    size_t l = p - s; if (l >= on) l = on - 1; memcpy(out, s, l); out[l] = 0; return p;
}
/* ---------- identity and link ---------- */
static uint8_t my_id[16]; static char my_id_hex[33]; static char my_name[64];
static struct { int ok; uint8_t peer_id[16]; char peer_id_hex[33]; char peer_name[128]; uint8_t key[32]; uint8_t link_id[8]; int peer_dp; } lk;

static int write_file_atomic(const char *path, const void *d, size_t n, int mode) {
    char tmp[400]; snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode); if (fd < 0) return -1;
    if (fchmod(fd, mode) || write_all(fd, d, n) || fsync(fd)) { close(fd); unlink(tmp); return -1; }
    close(fd); return rename(tmp, path);
}
static int read_small(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    size_t k = fread(out, 1, n - 1, f); fclose(f); out[k] = 0; return (int)k;
}
static void mkdirs(const char *p, int mode) {
    char t[300]; snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    mkdir(t, mode);
}
static void load_identity(void) {
    char h[80];
    mkdirs(STATE_DIR, 0700);
    if (!(read_small(ID_FILE, h, sizeof h) >= 32 && (h[32] = 0, fromhex(h, my_id, 16) == 0))) {
        randombytes_buf(my_id, 16); tohex(my_id, 16, h); strcat(h, "\n");
        if (write_file_atomic(ID_FILE, h, strlen(h), 0644)) logmsg("cannot write %s: %s", ID_FILE, strerror(errno));
    }
    tohex(my_id, 16, my_id_hex);
    gethostname(my_name, sizeof my_name - 1);
    if (!my_name[0]) snprintf(my_name, sizeof my_name, "Linux");
}
static void load_identity_readonly(void) {
    char h[80];
    if (read_small(ID_FILE, h, sizeof h) >= 32) { h[32] = 0; fromhex(h, my_id, 16); }
    tohex(my_id, 16, my_id_hex);
    gethostname(my_name, sizeof my_name - 1);
    if (!my_name[0]) snprintf(my_name, sizeof my_name, "Linux");
}
static int load_link(void) {
    char j[2048], v[160];
    memset(&lk, 0, sizeof lk);
    if (read_small(LINK_FILE, j, sizeof j) <= 0) return 0;
    if (!json_str(j, "peer_id", v, sizeof v) || fromhex(v, lk.peer_id, 16)) return 0;
    tohex(lk.peer_id, 16, lk.peer_id_hex);
    if (!json_str(j, "key", v, sizeof v) || fromhex(v, lk.key, 32)) return 0;
    if (!json_str(j, "link_id", v, sizeof v) || fromhex(v, lk.link_id, 8)) return 0;
    if (!json_str(j, "peer_name", lk.peer_name, sizeof lk.peer_name)) snprintf(lk.peer_name, sizeof lk.peer_name, "the other computer");
    lk.peer_dp = (int)json_long(j, "peer_dp", 24800);
    lk.ok = 1; return 1;
}
static int save_link(const uint8_t peer_id[16], const char *peer_name, const uint8_t k[32], const uint8_t link_id[8], int peer_dp) {
    char a[33], b[65], c[17]; tohex(peer_id, 16, a); tohex(k, 32, b); tohex(link_id, 8, c);
    buf_t j = {0};
    bprintf(&j, "{\"peer_id\": \"%s\", \"peer_name\": ", a); json_put_str(&j, peer_name);
    bprintf(&j, ", \"key\": \"%s\", \"link_id\": \"%s\", \"peer_dp\": %d}\n", b, c, peer_dp);
    mkdirs(STATE_DIR, 0700);
    int r = write_file_atomic(LINK_FILE, j.p, j.n, 0600);
    sodium_memzero(j.p, j.n); bfree(&j); sodium_memzero(b, sizeof b);
    return r;
}
static void forget_link(void) { unlink(LINK_FILE); }

/* Memory shared by the processes of the service: nonce counter and the peer's current address. */
static struct shared {
    volatile uint64_t counter;
    volatile uint32_t peer_ip;            /* network order, 0 = not found yet */
    volatile int peer_dp;
    volatile int64_t peer_seen_ms;        /* last authentic packet from the peer (mono ms) */
    char found_by[16];
} *shm;
static void shm_init(void) {
    shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset((void *)shm, 0, sizeof *shm); shm->counter = now_us();
}
static void peer_heard(uint32_t ip, const char *by) {
    if (!ip) return;
    if (shm->peer_ip != ip) {
        struct in_addr a = {ip}; logmsg("%s is at %s (found by %s)", lk.peer_name, inet_ntoa(a), by);
        shm->peer_ip = ip;
    }
    if (by && strcmp((const char *)shm->found_by, by)) snprintf(shm->found_by, sizeof shm->found_by, "%s", by);
    shm->peer_seen_ms = mono_ms();
}
static int peer_sockaddr(struct sockaddr_in *a, int port) {
    if (!shm || !shm->peer_ip) return -1;
    *a = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port)}; a->sin_addr.s_addr = shm->peer_ip; return 0;
}

/* ---------- crypto: sealed packets (PROTOCOL.md Encryption) ---------- */
static size_t seal(const uint8_t *m, size_t mlen, uint8_t *out) {
    uint64_t now = now_us(), c, want;
    do { c = shm->counter; want = now > c + 1 ? now : c + 1; } while (!__sync_bool_compare_and_swap(&shm->counter, c, want));
    memcpy(out, my_id, 4); put64(out + 4, want);
    unsigned long long clen;
    crypto_aead_chacha20poly1305_ietf_encrypt(out + 12, &clen, m, mlen, NULL, 0, NULL, out, lk.key);
    return 12 + clen;
}
/* Returns plaintext length or -1. Only the linked peer's packets open. */
static long unseal(const uint8_t *in, size_t len, uint8_t *out, uint64_t *ctr) {
    if (!lk.ok || len < 28 || memcmp(in, lk.peer_id, 4)) return -1;
    unsigned long long mlen;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(out, &mlen, NULL, in + 12, len - 12, NULL, 0, in, lk.key) != 0) return -1;
    *ctr = get64(in + 4);
    return (long)mlen;
}

/* ---------- interfaces used to find computers (PROTOCOL.md Finding computers) ---------- */
#define MAXIF 16
static struct iface { char name[IFNAMSIZ]; int index; struct in_addr addr, mask, bcast; int v4, v6; } ifs[MAXIF];
static int nifs;
enum { M_MDNS = 1, M_BCAST = 2, M_MCAST = 4, M_V6 = 8, M_SWEEP = 16, M_ALL = 31 };
static const char *METHOD_NAMES[] = {"mdns", "broadcast", "multicast", "ipv6", "sweep"};
static int methods = M_ALL;
static void init_methods(void) {
    const char *e = getenv("SHENZHEN_KVM_METHODS"); if (!e || !*e) return;
    methods = 0;
    for (int i = 0; i < 5; i++) if (strstr(e, METHOD_NAMES[i])) methods |= 1 << i;
}
static const char *method_name(int m) { for (int i = 0; i < 5; i++) if (m == 1 << i) return METHOD_NAMES[i]; return "direct"; }
static int iface_allowed(const char *name, unsigned flags) {
    const char *only = getenv("SHENZHEN_KVM_INTERFACES");
    if (only && *only) {   /* tests: exactly these */
        char t[256]; snprintf(t, sizeof t, ",%s,", only); char n[40]; snprintf(n, sizeof n, ",%s,", name); return strstr(t, n) != NULL;
    }
    if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK) || (flags & IFF_POINTOPOINT)) return 0;
    static const char *skip[] = {"tun", "tap", "utun", "ppp", "ipsec", "wg", "awdl", "llw", "docker", "veth", "virbr", "lxc", "zt"};
    for (unsigned i = 0; i < sizeof skip / sizeof *skip; i++) if (!strncmp(name, skip[i], strlen(skip[i]))) return 0;
    return 1;
}
/* Returns 1 when the list changed. */
static int scan_ifaces(void) {
    struct ifaddrs *l, *a; struct iface n[MAXIF]; int k = 0;
    if (getifaddrs(&l)) return 0;
    memset(n, 0, sizeof n);
    for (a = l; a; a = a->ifa_next) {
        if (!a->ifa_addr || !iface_allowed(a->ifa_name, a->ifa_flags)) continue;
        int i; for (i = 0; i < k && strcmp(n[i].name, a->ifa_name); i++) ;
        if (i == k) { if (k == MAXIF) continue; snprintf(n[k].name, IFNAMSIZ, "%s", a->ifa_name); n[k].index = if_nametoindex(a->ifa_name); k++; }
        if (a->ifa_addr->sa_family == AF_INET && !n[i].v4) {
            n[i].v4 = 1; n[i].addr = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
            n[i].mask = a->ifa_netmask ? ((struct sockaddr_in *)a->ifa_netmask)->sin_addr : (struct in_addr){htonl(0xffffff00)};
            n[i].bcast.s_addr = (n[i].addr.s_addr & n[i].mask.s_addr) | ~n[i].mask.s_addr;
        } else if (a->ifa_addr->sa_family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr)) n[i].v6 = 1;
    }
    freeifaddrs(l);
    int changed = k != nifs || memcmp(n, ifs, sizeof n);
    memcpy(ifs, n, sizeof n); nifs = k;
    return changed;
}
/* "addrs": [...] of this computer */
static void put_addrs(buf_t *b) {
    bputs(b, "\"addrs\": ["); int first = 1;
    for (int i = 0; i < nifs; i++) if (ifs[i].v4) { bprintf(b, "%s\"%s\"", first ? "" : ", ", inet_ntoa(ifs[i].addr)); first = 0; }
    bputs(b, "]");
}
/* Picks the best IPv4 address of a peer: its packet's own IPv4 source, else one of its addrs on a subnet of ours. */
struct pick { uint32_t best, first; };
static void pick_cb(const char *s, void *arg) {
    struct pick *p = arg; struct in_addr a; if (inet_pton(AF_INET, s, &a) != 1) return;
    if (!p->first) p->first = a.s_addr;
    for (int i = 0; i < nifs; i++) if (ifs[i].v4 && !p->best && (a.s_addr & ifs[i].mask.s_addr) == (ifs[i].addr.s_addr & ifs[i].mask.s_addr)) p->best = a.s_addr;
}
static uint32_t pick_addr(const char *json, const struct sockaddr_storage *src) {
    if (src && src->ss_family == AF_INET) return ((const struct sockaddr_in *)src)->sin_addr.s_addr;
    struct pick p = {0, 0}; json_each_str(json, "addrs", pick_cb, &p);
    return p.best ? p.best : p.first;
}

/* Discovery sockets: IPv4 and IPv6 on one port (the input port for presence, the linking port for
   finding computers to link with). */
typedef struct { int s4, s6, port; } dsock_t;
static void dsock_open(dsock_t *d, int port, int existing4) {
    int one = 1; d->port = port;
    d->s4 = existing4;
    if (d->s4 < 0) {
        d->s4 = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        setsockopt(d->s4, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
        if (bind(d->s4, (void *)&a, sizeof a)) { logmsg("cannot use UDP port %d: %s", port, strerror(errno)); close(d->s4); d->s4 = -1; }
    }
    if (d->s4 >= 0) { setsockopt(d->s4, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); int on = 1; setsockopt(d->s4, IPPROTO_IP, IP_PKTINFO, &on, sizeof on); }
    d->s6 = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    setsockopt(d->s6, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    setsockopt(d->s6, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = IN6ADDR_ANY_INIT};
    if (bind(d->s6, (void *)&a6, sizeof a6)) { close(d->s6); d->s6 = -1; }
}
/* Joins the IPv4 group and ff02::1 on every interface (again after the interfaces change). */
static void dsock_join(dsock_t *d) {
    for (int i = 0; i < nifs; i++) {
        if (d->s4 >= 0 && ifs[i].v4) {
            struct ip_mreqn m = {.imr_ifindex = ifs[i].index}; inet_pton(AF_INET, MCAST_GROUP, &m.imr_multiaddr);
            setsockopt(d->s4, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
        }
        if (d->s6 >= 0 && ifs[i].v6) {
            struct ipv6_mreq m = {.ipv6mr_interface = ifs[i].index}; inet_pton(AF_INET6, "ff02::1", &m.ipv6mr_multiaddr);
            setsockopt(d->s6, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m, sizeof m);
        }
    }
}
/* Sends one datagram out of interface i, to an IPv4 destination. */
static void send4_if(int s, int i, uint32_t dst, int port, const uint8_t *m, size_t n) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port)}; to.sin_addr.s_addr = dst;
    char cb[CMSG_SPACE(sizeof(struct in_pktinfo))] = {0};
    struct iovec iov = {(void *)m, n};
    struct msghdr mh = {.msg_name = &to, .msg_namelen = sizeof to, .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb};
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c->cmsg_level = IPPROTO_IP; c->cmsg_type = IP_PKTINFO; c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo *pi = (void *)CMSG_DATA(c); pi->ipi_ifindex = ifs[i].index; pi->ipi_spec_dst = ifs[i].addr;
    sendmsg(s, &mh, MSG_DONTWAIT);
}
/* Builds a message for a method (fills "via") and sends it by that method on every interface.
   make(buf, method) writes the whole datagram. */
static void dsock_send_by(dsock_t *d, int dport, int mset, void (*make)(buf_t *, int, void *), void *arg) {
    buf_t b = {0};
    for (int m = M_BCAST; m <= M_V6; m <<= 1) {
        if (!(mset & methods & m)) continue;
        b.n = 0; make(&b, m, arg);
        for (int i = 0; i < nifs; i++) {
            if (m == M_BCAST && ifs[i].v4 && d->s4 >= 0) {
                send4_if(d->s4, i, ifs[i].bcast.s_addr, dport, b.p, b.n);
                send4_if(d->s4, i, INADDR_BROADCAST, dport, b.p, b.n);
            } else if (m == M_MCAST && ifs[i].v4 && d->s4 >= 0) {
                struct in_addr g; inet_pton(AF_INET, MCAST_GROUP, &g); send4_if(d->s4, i, g.s_addr, dport, b.p, b.n);
            } else if (m == M_V6 && ifs[i].v6 && d->s6 >= 0) {
                unsigned idx = ifs[i].index; setsockopt(d->s6, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof idx);
                struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_port = htons(dport), .sin6_scope_id = idx};
                inet_pton(AF_INET6, "ff02::1", &to.sin6_addr);
                sendto(d->s6, b.p, b.n, MSG_DONTWAIT, (void *)&to, sizeof to);
            }
        }
    }
    bfree(&b);
}
/* Unicast sweep of every interface's subnet (at most the 1024 addresses around this computer), a few
   addresses per call; returns 1 when a full pass is done. */
static struct { int iface; uint32_t next, end; int active; } sweep;
static void sweep_start(void) { sweep.iface = -1; sweep.active = 1; sweep.next = sweep.end = 0; }
static int sweep_step(dsock_t *d, int dport, int budget, void (*make)(buf_t *, int, void *), void *arg) {
    if (!sweep.active || !(methods & M_SWEEP) || d->s4 < 0) { sweep.active = 0; return 1; }
    buf_t b = {0}; make(&b, M_SWEEP, arg);
    while (budget > 0) {
        if (sweep.next >= sweep.end) {
            do sweep.iface++; while (sweep.iface < nifs && !ifs[sweep.iface].v4);
            if (sweep.iface >= nifs) { sweep.active = 0; bfree(&b); return 1; }
            uint32_t a = ntohl(ifs[sweep.iface].addr.s_addr), mk = ntohl(ifs[sweep.iface].mask.s_addr);
            uint32_t net = a & mk, last = net | ~mk;
            uint32_t lo = net + 1, hi = last - 1;
            if (hi > lo && hi - lo > 1023) { lo = a > lo + 512 ? a - 512 : lo; if (lo + 1023 < hi) hi = lo + 1023; }
            sweep.next = lo; sweep.end = hi + 1;
            continue;
        }
        uint32_t h = sweep.next++;
        if (htonl(h) == ifs[sweep.iface].addr.s_addr) continue;
        send4_if(d->s4, sweep.iface, htonl(h), dport, b.p, b.n); budget--;
    }
    bfree(&b); return 0;
}
