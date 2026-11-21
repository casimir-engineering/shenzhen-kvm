/* ---------- config (/etc/shenzhen-kvm/config.json: this computer only) ---------- */
static struct {
    double speed;
    int cmd_l, cmd_r;       /* evdev keys for the Mac's Command */
    int ctl_l, ctl_r;       /* evdev keys for the Mac's Control */
} cfg;
static char want_display[16];         /* config.json "display": which desktop (default: the lowest-numbered) */
static const char *EDGE_NAMES[] = {"left", "right", "top", "bottom"};   /* 0..3, as on the wire; ^1 = opposite */

static void mod_keys(const char *v, int *l, int *r) {
    if (!strcmp(v, "super")) { *l = KEY_LEFTMETA; *r = KEY_RIGHTMETA; }
    else if (!strcmp(v, "alt")) { *l = KEY_LEFTALT; *r = KEY_RIGHTALT; }
    else { *l = KEY_LEFTCTRL; *r = KEY_RIGHTCTRL; }
}
static void load_config(void) {
    cfg.speed = 1.0;
    char cmd[16] = "control", ctl[16] = "super", v[64], j[8192];
    if (read_small(CONF_FILE, j, sizeof j) > 0) {
        if (json_str(j, "speed", v, sizeof v) && atof(v) > 0.05 && atof(v) < 20) cfg.speed = atof(v);
        if (json_str(j, "mac_command", v, sizeof v)) snprintf(cmd, sizeof cmd, "%s", v);
        if (json_str(j, "mac_control", v, sizeof v)) snprintf(ctl, sizeof ctl, "%s", v);
        if (json_str(j, "display", v, sizeof v) && v[0] == ':') snprintf(want_display, sizeof want_display, "%s", v);
    }
    mod_keys(cmd, &cfg.cmd_l, &cfg.cmd_r); mod_keys(ctl, &cfg.ctl_l, &cfg.ctl_r);
}

/* ---------- settings shared by both computers (PROTOCOL.md SETTINGS) ----------
   Kept from this computer's side in settings.json; the newest change wins on both. */
static struct { int sharing, peer_drives, drives_peer, clipboard, usual, big_files; long limit_mb; int64_t changed; int command_super; } st = {1, 1, 1, 1, 1, 1, 100, 0, 0};
static int64_t wall_ms(void) { return (int64_t)(now_us() / 1000); }
static void settings_json(buf_t *b, int wire) {
    if (wire)   /* from the sender's side */
        bprintf(b, "{\"sharing\": %s, \"i_drive_you\": %s, \"you_drive_me\": %s, \"clipboard\": %s, \"usual_shortcuts\": %s, \"big_files\": %s, \"file_limit_mb\": %ld, \"command_as\": \"%s\", \"changed\": %lld}",
                st.sharing ? "true" : "false", st.drives_peer ? "true" : "false", st.peer_drives ? "true" : "false", st.clipboard ? "true" : "false",
                st.usual ? "true" : "false", st.big_files ? "true" : "false", st.limit_mb, st.command_super ? "super" : "control", (long long)st.changed);
    else
        bprintf(b, "{\"sharing\": %s, \"peer_drives_me\": %s, \"i_drive_peer\": %s, \"clipboard\": %s, \"usual_shortcuts\": %s, \"big_files\": %s, \"file_limit_mb\": %ld, \"command_as\": \"%s\", \"changed\": %lld}",
                st.sharing ? "true" : "false", st.peer_drives ? "true" : "false", st.drives_peer ? "true" : "false", st.clipboard ? "true" : "false",
                st.usual ? "true" : "false", st.big_files ? "true" : "false", st.limit_mb, st.command_super ? "super" : "control", (long long)st.changed);
}
static void save_settings(void) { buf_t b = {0}; settings_json(&b, 0); bputs(&b, "\n"); mkdirs(STATE_DIR, 0700); write_file_atomic(SETTINGS_FILE, b.p, b.n, 0600); bfree(&b); }
static void parse_local_settings(const char *j) {
    char v[32];
    st.sharing = json_bool(j, "sharing", 1); st.peer_drives = json_bool(j, "peer_drives_me", 1); st.drives_peer = json_bool(j, "i_drive_peer", 1);
    st.clipboard = json_bool(j, "clipboard", 1); st.usual = json_bool(j, "usual_shortcuts", 1); st.big_files = json_bool(j, "big_files", 1);
    st.limit_mb = json_long(j, "file_limit_mb", 100); if (st.limit_mb <= 0) st.limit_mb = 100;
    st.changed = json_long(j, "changed", 0);
    if (json_str(j, "command_as", v, sizeof v)) st.command_super = !strcmp(v, "super");
}
static void load_settings(void) { char j[4096]; if (read_small(SETTINGS_FILE, j, sizeof j) > 0) parse_local_settings(j); }
/* The values of the settings (not when they changed), to tell a real change from a repeat. */
static void settings_values(char *out, size_t n) {
    snprintf(out, n, "%d%d%d%d%d%d%ld/%d", st.sharing, st.peer_drives, st.drives_peer, st.clipboard, st.usual, st.big_files, st.limit_mb, st.command_super);
}
/* Settings from the peer: 0 = ours are newer, 1 = newer and something changed, 2 = newer but the
   same values (nothing to apply). */
static int settings_from_peer(const char *j) {
    long long ch = json_long(j, "changed", 0); char v[32], before[64], after[64];
    if (ch && ch <= st.changed) return 0;
    settings_values(before, sizeof before);
    st.sharing = json_bool(j, "sharing", st.sharing); st.peer_drives = json_bool(j, "i_drive_you", st.peer_drives);
    st.drives_peer = json_bool(j, "you_drive_me", st.drives_peer); st.clipboard = json_bool(j, "clipboard", st.clipboard);
    st.usual = json_bool(j, "usual_shortcuts", st.usual); st.big_files = json_bool(j, "big_files", st.big_files);
    long l = json_long(j, "file_limit_mb", st.limit_mb); if (l > 0) st.limit_mb = l;
    if (json_str(j, "command_as", v, sizeof v)) st.command_super = !strcmp(v, "super");
    st.changed = ch ? ch : wall_ms();
    settings_values(after, sizeof after);
    return strcmp(before, after) ? 1 : 2;
}
/* Settings changed here (tray app or command line): fields present in j are applied. Returns 1 when a
   value changed. */
static int settings_from_local(const char *j) {
    char v[32], before[64], after[64];
    settings_values(before, sizeof before);
    st.sharing = json_bool(j, "sharing", st.sharing); st.peer_drives = json_bool(j, "peer_drives_me", st.peer_drives);
    st.drives_peer = json_bool(j, "i_drive_peer", st.drives_peer); st.clipboard = json_bool(j, "clipboard", st.clipboard);
    st.usual = json_bool(j, "usual_shortcuts", st.usual); st.big_files = json_bool(j, "big_files", st.big_files);
    long l = json_long(j, "file_limit_mb", st.limit_mb); if (l > 0) st.limit_mb = l;
    if (json_str(j, "command_as", v, sizeof v)) st.command_super = !strcmp(v, "super");
    settings_values(after, sizeof after);
    if (!strcmp(before, after)) return 0;
    st.changed = wall_ms();
    return 1;
}

/* ---------- HID usage <-> evdev (Linux hid-input.c table) ---------- */
#define U 0
static const unsigned char hid_keyboard[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  U,  U,  U,121,  U, 89, 93,124, 92, 94, 95,  U,  U,  U,
    122,123, 90, 91, 85,  U,  U,  U,  U,  U,  U,  U,111,  U,  U,  U,
      U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,
      U,  U,  U,  U,  U,  U,179,180,  U,  U,  U,  U,  U,  U,  U,  U,
      U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,  U,
      U,  U,  U,  U,  U,  U,  U,  U,111,  U,  U,  U,  U,  U,  U,  U,
     29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
    150,158,159,128,136,177,178,176,142,152,173,140,  U,  U,  U,  U };
static unsigned short key_to_usage[256];
static void init_tables(void) {
    for (int u = 255; u >= 4; u--) if (hid_keyboard[u]) key_to_usage[hid_keyboard[u]] = u;
    for (int u = 0xE0; u <= 0xE7; u++) key_to_usage[hid_keyboard[u]] = u;  /* modifiers win */
    key_to_usage[KEY_BACKSLASH] = 0x31; key_to_usage[KEY_DELETE] = 0x4C;
    /* media keys travel as the usages 0xE8..0xEF (Linux's table): play/pause, stop, previous, next, eject, volume */
    key_to_usage[KEY_PLAYPAUSE] = 0xE8; key_to_usage[KEY_STOPCD] = 0xE9; key_to_usage[KEY_PREVIOUSSONG] = 0xEA; key_to_usage[KEY_NEXTSONG] = 0xEB;
    key_to_usage[KEY_EJECTCD] = 0xEC; key_to_usage[KEY_VOLUMEUP] = 0xED; key_to_usage[KEY_VOLUMEDOWN] = 0xEE; key_to_usage[KEY_MUTE] = 0xEF;
}
static int usage_to_key(unsigned u) {
    switch (u) {
    case 0xE3: return cfg.cmd_l; case 0xE7: return cfg.cmd_r;
    case 0xE0: return cfg.ctl_l; case 0xE4: return cfg.ctl_r;
    }
    return u < 256 ? hid_keyboard[u] : 0;
}

/* ---------- framing between the service's processes: u8 type, u32 len, payload (one write) ---------- */
static int hsend(int fd, uint8_t t, const void *d, uint32_t n) {
    if (fd < 0) return -1;
    uint8_t *f = malloc(5 + n); if (!f) return -1;
    f[0] = t; memcpy(f + 1, &n, 4); if (n) memcpy(f + 5, d, n);
    int r = write_all(fd, f, 5 + n); free(f); return r;
}
static int hrecv(int fd, uint8_t *t, buf_t *b) {
    uint8_t h[5]; uint32_t n;
    if (read_all(fd, h, 5)) return -1;
    *t = h[0]; memcpy(&n, h + 1, 4);
    if (n > MAX_CLIP + 1024) return -1;
    b->n = 0; if (n) { if (n > b->cap) { b->cap = n; b->p = realloc(b->p, n); } if (read_all(fd, b->p, n)) return -1; b->n = n; }
    return 0;
}
static int hsend_str(int fd, uint8_t t, const char *s) { return hsend(fd, t, s, strlen(s)); }
