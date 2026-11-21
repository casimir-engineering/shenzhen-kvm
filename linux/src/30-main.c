/* ======================================================================================
 * MAIN PROCESS
 * ====================================================================================== */
enum { IDLE, DRIVEN, LEAVING, ENTERING, DRIVING };
static const char *STATE_NAMES[] = {"idle", "driven", "leaving", "entering", "driving"};
static int state = IDLE;
static int udp = -1, kfd = -1, pfd = -1, hsock = -1, asock = -1, ino_dev = -1, ino_conf = -1, linked;
static pid_t helper_pid, agent_pid;
static int64_t helper_restart_at, agent_restart_at;
static struct sockaddr_in last_src;
static dsock_t pres = {-1, -1, 0};   /* presence of the linked peer: the input port, IPv4 and IPv6 */
static uint64_t rx_high;
static int scr_w = 3440, scr_h = 1440;
static double px, py;                 /* tracked pointer position (driven) */
static int exit_edge;                 /* the edge the pointer last left through */
static int injected_down[KEY_CNT];    /* keys held on the virtual keyboard */
static int btn_down[8];
static double wheel_acc_y, wheel_acc_x;
static int64_t last_rx_ms, next_retx_ms, deadline_ms, next_alive_ms, cooldown_ms, last_query_ms;
static uint16_t enter_pos;
static int query_out, pos_dirty, injected_since_query;
static double loc_dx, loc_dy;          /* this computer's mouse moves since the last edge query */
static int64_t loc_at;
static char last_error[256] = "";
static int64_t last_peer_seen_ms = -1;
static uint64_t lat_n, lat_sum_ns, lat_max_ns, lat_hist[12];  /* <25,50,100,200,500us,1,2,5,10,20,50ms,more */
static int mac_down[256];             /* HID usages held on the peer (we drive) */
static int pending_copy, pending_paste;
static int paste_wait, paste_deferred, paste_shift, paste_v_swallowed; static int64_t paste_wait_until;
static void tell_here(void) { if (hsock >= 0) { uint8_t h = state != DRIVING; hsend(hsock, 'H', &h, 1); } }
/* Files from the peer are still on their way: hold a paste (Ctrl+V, Ctrl+Shift+V) until they are in place. */
static int paste_gate(int k, int down) {
    if (k == KEY_V && !down && paste_v_swallowed) { paste_v_swallowed = 0; return 1; }
    if (!paste_wait || k != KEY_V || !down) return 0;
    if (!(injected_down[KEY_LEFTCTRL] || injected_down[KEY_RIGHTCTRL])) return 0;
    paste_deferred = 1; paste_v_swallowed = 1;
    paste_shift = injected_down[KEY_LEFTSHIFT] || injected_down[KEY_RIGHTSHIFT];
    logmsg("paste held until the files from %s are in place", lk.peer_name);
    return 1;
}

static void set_error(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(last_error, sizeof last_error, fmt, a); va_end(a); logmsg("%s", last_error);
}

/* --- physical devices --- */
#define MAXDEV 32
static struct dev { int fd, kbd, mouse, hires, grabbed, btns; char name[96], path[64]; int dx, dy, wx, wy; } devs[MAXDEV];
static int phys_down[KEY_CNT], swallowed[KEY_CNT];

#define TB(bits, b) ((bits)[(b) / 8] & (1 << ((b) % 8)))
static void dev_open(const char *name) {
    char path[64]; snprintf(path, sizeof path, "/dev/input/%s", name);
    if (strncmp(name, "event", 5)) return;
    for (int i = 0; i < MAXDEV; i++) if (devs[i].fd > 0 && !strcmp(devs[i].path, path)) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) return;
    char nm[96] = ""; ioctl(fd, EVIOCGNAME(sizeof nm), nm);
    uint8_t kb[KEY_CNT / 8 + 1] = {0}, rb[REL_CNT / 8 + 1] = {0}, ab[ABS_CNT / 8 + 1] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb); ioctl(fd, EVIOCGBIT(EV_REL, sizeof rb), rb); ioctl(fd, EVIOCGBIT(EV_ABS, sizeof ab), ab);
    int kbd = TB(kb, KEY_A) && TB(kb, KEY_Z) && TB(kb, KEY_SPACE);
    int mouse = TB(rb, REL_X) && TB(rb, REL_Y) && TB(kb, BTN_LEFT);
    if (!strncmp(nm, "Shenzhen KVM", 12) || (!kbd && !mouse)) { close(fd); return; }
    int i; for (i = 0; i < MAXDEV && devs[i].fd > 0; i++) ;
    if (i == MAXDEV) { close(fd); return; }
    memset(&devs[i], 0, sizeof devs[i]);
    devs[i].fd = fd; devs[i].kbd = kbd; devs[i].mouse = mouse; devs[i].hires = TB(rb, REL_WHEEL_HI_RES);
    snprintf(devs[i].name, sizeof devs[i].name, "%s", nm); snprintf(devs[i].path, sizeof devs[i].path, "%s", path);
    /* keyboards are always grabbed (for the clipboard shortcuts) and replayed through our virtual
       keyboard; a device that is keyboard and mouse at once is only grabbed while driving the peer */
    if (kbd && !mouse) devs[i].grabbed = ioctl(fd, EVIOCGRAB, 1) == 0;
    if (state == DRIVING && mouse) devs[i].grabbed = ioctl(fd, EVIOCGRAB, 1) == 0;
    logmsg("input: %s (%s%s%s)", nm, kbd ? "keyboard" : "", kbd && mouse ? "+" : "", mouse ? "mouse" : "");
}
static void dev_close(struct dev *d) { close(d->fd); logmsg("input: %s removed", d->name); d->fd = 0; }
static void dev_scan(void) {
    DIR *dd = opendir("/dev/input"); struct dirent *e;
    if (!dd) return;
    while ((e = readdir(dd))) dev_open(e->d_name);
    closedir(dd);
}
static void grab_mice(int on) {
    for (int i = 0; i < MAXDEV; i++) if (devs[i].fd > 0 && devs[i].mouse && (devs[i].grabbed != on) && !(devs[i].kbd && !devs[i].mouse)) {
        ioctl(devs[i].fd, EVIOCGRAB, on); devs[i].grabbed = on; devs[i].dx = devs[i].dy = devs[i].wx = devs[i].wy = 0;
    }
}

/* --- uinput --- */
static int ui_create(int pointer) {
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct uinput_setup s = {.id = {BUS_VIRTUAL, 0x1209, pointer ? 0x4b56 : 0x4b4b, 1}};
    if (pointer) {
        snprintf(s.name, sizeof s.name, "Shenzhen KVM pointer");
        ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_ABS); ioctl(fd, UI_SET_EVBIT, EV_REL);
        int b[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
        for (unsigned i = 0; i < 5; i++) ioctl(fd, UI_SET_KEYBIT, b[i]);
        int r[] = {REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES};
        for (unsigned i = 0; i < 4; i++) ioctl(fd, UI_SET_RELBIT, r[i]);
        for (int ax = ABS_X; ax <= ABS_Y; ax++) {
            struct uinput_abs_setup a = {.code = ax, .absinfo = {.minimum = 0, .maximum = 65535}};
            ioctl(fd, UI_SET_ABSBIT, ax); ioctl(fd, UI_ABS_SETUP, &a);
        }
    } else {
        snprintf(s.name, sizeof s.name, "Shenzhen KVM keyboard");
        ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_LED);
        for (int k = 1; k < 256; k++) ioctl(fd, UI_SET_KEYBIT, k);
        ioctl(fd, UI_SET_LEDBIT, LED_NUML); ioctl(fd, UI_SET_LEDBIT, LED_CAPSL); ioctl(fd, UI_SET_LEDBIT, LED_SCROLLL);
    }
    if (ioctl(fd, UI_DEV_SETUP, &s) || ioctl(fd, UI_DEV_CREATE)) { close(fd); return -1; }
    return fd;
}
static struct input_event evq[64]; static int evn;
static void ev(int type, int code, int val) { if (evn < 63) { evq[evn].type = type; evq[evn].code = code; evq[evn].value = val; evn++; } }
static void flush_to(int fd) { if (!evn) return; ev(EV_SYN, SYN_REPORT, 0); write_all(fd, evq, evn * sizeof *evq); evn = 0; }
static int paste_gate(int k, int down);
static void inject_key(int k, int down) {
    if (k <= 0 || k >= 256) return;
    if (paste_gate(k, down)) return;
    if (down == injected_down[k] && down) return;
    injected_down[k] = down; ev(EV_KEY, k, down); flush_to(kfd);
}
static void place_pointer(double x, double y) {
    if (x < 0) x = 0; if (y < 0) y = 0; if (x > scr_w - 1) x = scr_w - 1; if (y > scr_h - 1) y = scr_h - 1;
    px = x; py = y;
    ev(EV_ABS, ABS_X, (int)(((int64_t)(int)x * 65536 + 32768) / scr_w));
    ev(EV_ABS, ABS_Y, (int)(((int64_t)(int)y * 65536 + 32768) / scr_h));
    flush_to(pfd);
}
static int raw_down[256];             /* the peer's keys as it sent them (HID usages), before mapping */
static int switcher_key, ctrl_tab_key;  /* modifier held here for the peer's Command+Tab / Control+Tab, 0 = none */
static void release_all(void) {
    memset(raw_down, 0, sizeof raw_down); switcher_key = ctrl_tab_key = 0;
    for (int k = 1; k < 256; k++) if (injected_down[k] && !phys_down[k]) { injected_down[k] = 0; ev(EV_KEY, k, 0); if (evn > 50) flush_to(kfd); }
    flush_to(kfd);
    int b[] = {0, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
    for (int i = 1; i <= 5; i++) if (btn_down[i]) { btn_down[i] = 0; ev(EV_KEY, b[i], 0); }
    flush_to(pfd);
}

/* --- UDP --- */
static void udp_send_to(struct sockaddr_in *to, const uint8_t *m, size_t n) {
    uint8_t out[1500]; size_t c = seal(m, n, out);
    sendto(udp, out, c, MSG_DONTWAIT, (void *)to, sizeof *to);
}
static void udp_send(const uint8_t *m, size_t n) { struct sockaddr_in a; if (peer_sockaddr(&a, shm->peer_dp) == 0) udp_send_to(&a, m, n); }
static void send_ack(uint8_t t, struct sockaddr_in *to) { uint8_t m[2] = {0x02, t}; udp_send_to(to, m, 2); }
static void send_enter(void) { uint8_t m[4] = {0x01, (uint8_t)(exit_edge ^ 1)}; put16(m + 2, enter_pos); udp_send(m, 4); }
static void send_leave(uint16_t pos) { uint8_t m[3] = {0x03}; put16(m + 1, pos); udp_send(m, 3); }
static void send_key_mac(unsigned usage, int down) {
    if (!usage || usage > 255) return;
    mac_down[usage] = down; uint8_t m[4] = {0x13}; put16(m + 1, usage); m[3] = down; udp_send(m, 4);
}

static uint16_t pos_along(int edge, double x, double y) {
    double f = (edge >= 2) ? x / (scr_w > 1 ? scr_w - 1 : 1) : y / (scr_h > 1 ? scr_h - 1 : 1);
    if (f < 0) f = 0; if (f > 1) f = 1; return (uint16_t)(f * 65535 + 0.5);
}
static void edge_point(int edge, uint16_t pos, int inset, double *x, double *y) {
    double f = pos / 65535.0;
    switch (edge) {
    case 0: *x = inset; *y = f * (scr_h - 1); break;
    case 1: *x = scr_w - 1 - inset; *y = f * (scr_h - 1); break;
    case 2: *x = f * (scr_w - 1); *y = inset; break;
    default: *x = f * (scr_w - 1); *y = scr_h - 1 - inset; break;
    }
}

/* PROTOCOL.md Edges: adds a move (dx, dy, no button held) that left the pointer at x, y to the
   outward push; returns the edge to cross once the push is enough, else -1. */
#define CROSS_PUSH 400.0
#define PUSH_GAP_MS 300
#define CORNER_ZONE 40
static int push_edge = -1; static double push_amt; static int64_t push_at;
static void push_reset(void) { push_edge = -1; push_amt = 0; }
static int pushed(double x, double y, double dx, double dy) {
    int64_t now = mono_ms(), last = push_at; push_at = now;
    int e = x <= 0 && dx < 0 ? 0 : x >= scr_w - 1 && dx > 0 ? 1 : y <= 0 && dy < 0 ? 2 : y >= scr_h - 1 && dy > 0 ? 3 : -1;
    if (e < 0) { push_reset(); return -1; }
    double out = e == 0 ? -dx : e == 1 ? dx : e == 2 ? -dy : dy;
    double along = e >= 2 ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    double a = e >= 2 ? x : y, b = e >= 2 ? scr_w - 1 - x : scr_h - 1 - y;   /* to the corners along the edge */
    if (a < CORNER_ZONE || b < CORNER_ZONE || along > 2 * out) { push_reset(); return -1; }
    if (e != push_edge || now - last > PUSH_GAP_MS) push_amt = 0;
    push_edge = e; push_amt += out;
    if (push_amt < CROSS_PUSH) return -1;
    push_reset(); return e;
}

static void query(uint8_t tag) { if (hsock >= 0 && !query_out) { query_out = 1; injected_since_query = 0; last_query_ms = mono_ms(); hsend(hsock, 'Q', &tag, 1); } }

static void end_driven(const char *why) {
    if (state != DRIVEN && state != LEAVING) return;
    release_all(); state = IDLE; logmsg("session ended: %s", why);
}
static double park_x, park_y;          /* where this computer's pointer was hidden */
static int64_t held_since_ms = -10000, leave_until_ms, next_leave_ms, stopping_grace_until;
static int claim;                      /* holds the pointer with a claim (PROTOCOL.md Restarts) */
/* The pointer goes to the peer (this computer becomes the remote): hide it here, hold the mice,
   forward all input. */
static void start_driving(void) {
    release_all(); claim = 0; stopping_grace_until = 0;
    park_x = px; park_y = py;
    grab_mice(1);
    state = DRIVING; last_rx_ms = mono_ms(); next_alive_ms = last_rx_ms + 500;
    memset(mac_down, 0, sizeof mac_down);
    for (int k = 1; k < 256; k++) if (phys_down[k] && key_to_usage[k] >= 0xE0) send_key_mac(key_to_usage[k], 1);  /* held modifiers */
    tell_here();
    logmsg("the pointer is on %s; hidden here", lk.peer_name);
}
static void stop_driving(const char *why, int have_pos, uint16_t pos) {
    if (state != DRIVING) return;
    for (int u = 1; u < 256; u++) if (mac_down[u]) send_key_mac(u, 0);
    grab_mice(0);
    state = IDLE; cooldown_ms = mono_ms() + 400; push_reset();
    double x, y;
    if (have_pos) { edge_point(exit_edge, pos, 1, &x, &y); place_pointer(x, y); }
    else place_pointer(park_x, park_y);   /* back where it was hidden */
    tell_here();
    logmsg("the pointer is back here: %s", why);
}
/* This computer holds the pointer and may apply the peer's input (from where the pointer is now). */
static int holding(void) {
    if (!st.sharing || !st.peer_drives) return 0;
    if (state == DRIVEN) return 1;
    if (state != IDLE) return 0;
    release_all(); state = DRIVEN; wheel_acc_x = wheel_acc_y = 0;
    pos_dirty = 1;   /* where the pointer really is, in case it moved without this computer's mice */
    logmsg("%s's keyboard and mouse act here", lk.peer_name);
    return 1;
}
/* The pointer was pushed past `edge` here (by either mouse): hand it to the peer (ENTER), which
   takes it in at its opposite edge. */
static void hand_over(double x, double y, int edge) {
    if (!st.sharing || !st.drives_peer || !shm->peer_ip || mono_ms() < cooldown_ms) return;
    if (state == DRIVEN) release_all();
    if (state != IDLE && state != DRIVEN) return;
    exit_edge = edge; enter_pos = pos_along(edge, x, y); push_reset();
    state = ENTERING; send_enter(); next_retx_ms = mono_ms() + 200; deadline_ms = mono_ms() + 1000;
}

/* MOVE timing while driven. The sender's nonce counter is its send time in microseconds (wall clock),
   so arrival - counter is the one-way delay plus a constant clock offset: its excess over the recent
   minimum is the network delay jitter, measured without synchronised clocks. */
static const double NET_LIM[] = {2, 4, 8, 12, 16, 24, 32, 50, 80, 120};   /* ms; last bucket = more */
#define NET_B 11
static uint64_t net_moves, net_send[NET_B], net_arr[NET_B], net_late[NET_B];
static uint64_t net_prev_ctr; static int64_t net_prev_arr_us, net_min_cur, net_min_prev, net_win_start;
static void net_hist(uint64_t *h, double ms) { int i = 0; while (i < NET_B - 1 && ms >= NET_LIM[i]) i++; h[i]++; }
static void net_record(uint64_t ctr) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    int64_t arr = (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000, off = arr - (int64_t)ctr;
    if (arr - net_win_start > 3000000) { net_min_prev = net_min_cur; net_min_cur = off; net_win_start = arr; }
    if (off < net_min_cur) net_min_cur = off;
    int64_t base = net_min_prev && net_min_prev < net_min_cur ? net_min_prev : net_min_cur;
    net_moves++; net_hist(net_late, (off - base) / 1000.0);
    if (net_prev_ctr && ctr - net_prev_ctr < 200000) {      /* same motion: gaps under 200 ms */
        net_hist(net_send, (ctr - net_prev_ctr) / 1000.0); net_hist(net_arr, (arr - net_prev_arr_us) / 1000.0);
    }
    net_prev_ctr = ctr; net_prev_arr_us = arr;
}
static int net_json(char *s, size_t n, const char *name, const uint64_t *h) {
    int k = snprintf(s, n, "\"%s\": {", name);
    for (int i = 0; i < NET_B; i++) {
        if (i < NET_B - 1) k += snprintf(s + k, n - k, "\"<%g\": %llu, ", NET_LIM[i], (unsigned long long)h[i]);
        else k += snprintf(s + k, n - k, "\"more\": %llu}", (unsigned long long)h[i]);
    }
    return k;
}

static void lat_record(int64_t t0) {
    int64_t d = mono_ns() - t0; lat_n++; lat_sum_ns += d; if ((uint64_t)d > lat_max_ns) lat_max_ns = d;
    static const int64_t lim[] = {25000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000, 50000000};
    int i = 0; while (i < 11 && d >= lim[i]) i++; lat_hist[i]++;
}

/* A key from the peer, driven. Modifiers follow the settings (the Mac's Command acts as Control or
   Super). The app switcher and tab switching keep their meaning: Command+Tab (repeated, with Shift)
   becomes Alt+Tab with Alt held until Command is released; Control+Tab stays Control+Tab. */
static int driven_key(unsigned u, int d) {
    if (u >= 256) return 0;
    raw_down[u] = d;
    int cmd = raw_down[0xE3] || raw_down[0xE7], ctl = raw_down[0xE0] || raw_down[0xE4];
    if (u == 0x2B && d && cmd && !switcher_key) {          /* Command+Tab: lift the Command keys, hold Alt */
        int ks[2] = {cfg.cmd_l, cfg.cmd_r};
        for (int i = 0; i < 2; i++) if (injected_down[ks[i]]) { injected_down[ks[i]] = 0; ev(EV_KEY, ks[i], 0); }
        switcher_key = KEY_LEFTALT; injected_down[KEY_LEFTALT] = 1; ev(EV_KEY, KEY_LEFTALT, 1); flush_to(kfd);
    } else if (u == 0x2B && d && ctl && !cmd && !ctrl_tab_key && cfg.ctl_l != KEY_LEFTCTRL) {   /* Control+Tab */
        int ks[2] = {cfg.ctl_l, cfg.ctl_r};
        for (int i = 0; i < 2; i++) if (injected_down[ks[i]]) { injected_down[ks[i]] = 0; ev(EV_KEY, ks[i], 0); }
        ctrl_tab_key = KEY_LEFTCTRL; injected_down[KEY_LEFTCTRL] = 1; ev(EV_KEY, KEY_LEFTCTRL, 1); flush_to(kfd);
    }
    if ((u == 0xE3 || u == 0xE7) && switcher_key) {        /* Command while the switcher is open */
        if (!cmd) { injected_down[switcher_key] = 0; ev(EV_KEY, switcher_key, 0); flush_to(kfd); switcher_key = 0; }
        return 1;
    }
    if ((u == 0xE0 || u == 0xE4) && ctrl_tab_key) {
        if (!ctl) { injected_down[ctrl_tab_key] = 0; ev(EV_KEY, ctrl_tab_key, 0); flush_to(kfd); ctrl_tab_key = 0; }
        return 1;
    }
    int k = usage_to_key(u);
    if (k <= 0 || k >= 256) return 0;
    if (paste_gate(k, d)) return 0;
    if (injected_down[k] == d) return 0;
    injected_down[k] = d; ev(EV_KEY, k, d); flush_to(kfd); return 1;
}

static void handle_present(const uint8_t *in, size_t n, struct sockaddr_storage *src);
static void handle_udp(void) {
    uint8_t in[2048], m[2048]; struct sockaddr_in src; socklen_t sl = sizeof src;
    for (;;) {
        sl = sizeof src;
        ssize_t n = recvfrom(udp, in, sizeof in, MSG_DONTWAIT, (void *)&src, &sl);
        if (n < 0) return;
        int64_t t0 = mono_ns();
        if (n > 7 && !memcmp(in, MAGIC, 6)) { struct sockaddr_storage ss; memcpy(&ss, &src, sizeof src); handle_present(in, n, &ss); continue; }
        uint64_t ctr; long ml = unseal(in, n, m, &ctr);
        if (ml < 1 || ctr <= rx_high) continue;
        rx_high = ctr; last_rx_ms = mono_ms(); last_peer_seen_ms = last_rx_ms; last_src = src;
        if (stopping_grace_until && m[0] != 0x04) stopping_grace_until = 0;   /* the holder is back */
        peer_heard(src.sin_addr.s_addr, NULL);
        if (state == DRIVEN && m[0] == 0x10) net_record(ctr);
        int moved = 0, keyed = 0;
        for (long i = 0; i < ml;) {
            uint8_t t = m[i];
            if (t == 0x01 && i + 4 <= ml) {          /* ENTER */
                int edge = m[i + 1] & 3; uint16_t pos = get16(m + i + 2); i += 4;
                if (!st.sharing || !st.peer_drives) continue;   /* switched off: the peer's pointer stays there */
                if (state == ENTERING && memcmp(my_id, lk.peer_id, 16) < 0) continue;   /* both handed over at once: the lower id keeps its handover */
                send_ack(0x01, &src);
                if ((state == DRIVEN || state == IDLE) && mono_ms() - held_since_ms < 1500) continue;   /* a repeat of the ENTER that gave it */
                if (state == DRIVING) stop_driving("it came back", 1, pos);
                release_all(); state = DRIVEN; wheel_acc_x = wheel_acc_y = 0; held_since_ms = mono_ms(); claim = 1;
                double x, y; edge_point(edge, pos, 0, &x, &y); place_pointer(x, y);
                logmsg("the pointer is here (from %s, in through the %s edge)", lk.peer_name, EDGE_NAMES[edge]);
            } else if (t == 0x02 && i + 2 <= ml) {   /* ACK */
                uint8_t a = m[i + 1]; i += 2;
                if (a == 0x01 && state == ENTERING) start_driving();
                else if (a == 0x03) leave_until_ms = 0;
            } else if (t == 0x03 && i + 3 <= ml) {   /* LEAVE */
                uint16_t pos = get16(m + i + 1); i += 3;
                (void)pos; send_ack(0x03, &src);
                /* the peer took the pointer back to its screen: this computer becomes the remote */
                if (state != DRIVING && st.sharing && st.drives_peer) { if (state == DRIVEN) end_driven("the other computer took the pointer back"); state = IDLE; start_driving(); }
            } else if (t == 0x10 && i + 5 <= ml) {   /* MOVE */
                int dx = (int16_t)get16(m + i + 1), dy = (int16_t)get16(m + i + 3); i += 5;
                if (!holding()) continue;
                double nx = px + dx * cfg.speed, ny = py + dy * cfg.speed;
                int nob = !btn_down[1] && !btn_down[2] && !btn_down[3];
                double cx = nx < 0 ? 0 : nx > scr_w - 1 ? scr_w - 1 : nx, cy = ny < 0 ? 0 : ny > scr_h - 1 ? scr_h - 1 : ny;
                int out = -1;
                /* outward: only what goes past the edge counts */
                if (!nob) push_reset(); else if (st.drives_peer) out = pushed(cx, cy, nx != cx ? nx - cx : dx * cfg.speed, ny != cy ? ny - cy : dy * cfg.speed);
                if (out >= 0) {   /* pushed past an edge by the peer's mouse: hand over */
                    place_pointer(cx, cy); hand_over(cx, cy, out);
                    continue;
                }
                px = cx; py = cy;
                ev(EV_ABS, ABS_X, (int)(((int64_t)(int)px * 65536 + 32768) / scr_w));
                ev(EV_ABS, ABS_Y, (int)(((int64_t)(int)py * 65536 + 32768) / scr_h));
                moved = 1; injected_since_query = 1;
            } else if (t == 0x11 && i + 3 <= ml) {   /* BUTTON */
                int b = m[i + 1], d = m[i + 2] ? 1 : 0; i += 3;
                static const int map[] = {0, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
                if (b < 1 || b > 5 || !holding()) continue;
                btn_down[b] = d; ev(EV_KEY, map[b], d); moved = 1;
            } else if (t == 0x12 && i + 5 <= ml) {   /* SCROLL */
                int dx = (int16_t)get16(m + i + 1), dy = (int16_t)get16(m + i + 3); i += 5;
                if (!holding()) continue;
                if (dy) { ev(EV_REL, REL_WHEEL_HI_RES, -dy); wheel_acc_y += -dy; }
                if (dx) { ev(EV_REL, REL_HWHEEL_HI_RES, dx); wheel_acc_x += dx; }
                int ny = (int)(wheel_acc_y / 120), nx = (int)(wheel_acc_x / 120);
                if (ny) { ev(EV_REL, REL_WHEEL, ny); wheel_acc_y -= ny * 120; }
                if (nx) { ev(EV_REL, REL_HWHEEL, nx); wheel_acc_x -= nx * 120; }
                moved = 1;
            } else if (t == 0x13 && i + 4 <= ml) {   /* KEY */
                unsigned u = get16(m + i + 1); int d = m[i + 3] ? 1 : 0; i += 4;
                if (state != DRIVEN) continue;
                keyed |= driven_key(u, d);
            } else if (t == 0x04) {                  /* STOPPING: the holder restarts; wait for it */
                i += 1; send_ack(0x04, &src);
                if (state == DRIVING) { stopping_grace_until = mono_ms() + 30000; logmsg("%s is restarting; waiting for it", lk.peer_name); }
                continue;
            } else if (t == 0x1F) {                  /* ALIVE */
                i += 1; send_ack(0x1F, &src);
            } else break;
            if (evn > 48 && moved) { flush_to(pfd); }
        }
        if (moved) flush_to(pfd);
        if (moved || keyed) lat_record(t0);
    }
}

static void local_shortcut(int code) {
    if (state == DRIVING) {           /* the pointer is on the peer: the shortcut works there */
        int ls = mac_down[0xE1], rs = mac_down[0xE5];
        if (code == KEY_C) {
            if (ls) send_key_mac(0xE1, 0); if (rs) send_key_mac(0xE5, 0);
            int ctl = mac_down[0xE0] || mac_down[0xE4]; if (!ctl) send_key_mac(0xE0, 1);
            send_key_mac(0x06, 1); send_key_mac(0x06, 0);
            if (!ctl) send_key_mac(0xE0, 0); if (ls) send_key_mac(0xE1, 1); if (rs) send_key_mac(0xE5, 1);
            hsend(hsock, 'R', NULL, 0);
        } else { hsend(hsock, 'P', NULL, 0); }
    } else {
        if (code == KEY_C) { pending_copy = 1; hsend(hsock, 'c', NULL, 0); }
        else { pending_paste = 1; hsend(hsock, 'v', NULL, 0); }
    }
}
/* Ctrl(+Shift if terminal)+letter on the virtual keyboard, then the held state is restored. */
static void local_combo(int letter, int terminal) {
    int ls = injected_down[KEY_LEFTSHIFT], rs = injected_down[KEY_RIGHTSHIFT];
    int ctl = injected_down[KEY_LEFTCTRL] || injected_down[KEY_RIGHTCTRL];
    if (!terminal) { if (ls) inject_key(KEY_LEFTSHIFT, 0); if (rs) inject_key(KEY_RIGHTSHIFT, 0); }
    else if (!ls && !rs) inject_key(KEY_LEFTSHIFT, 1);
    if (!ctl) inject_key(KEY_LEFTCTRL, 1);
    inject_key(letter, 1); inject_key(letter, 0);
    if (!ctl) inject_key(KEY_LEFTCTRL, 0);
    if (!terminal) { if (ls) inject_key(KEY_LEFTSHIFT, phys_down[KEY_LEFTSHIFT]); if (rs) inject_key(KEY_RIGHTSHIFT, phys_down[KEY_RIGHTSHIFT]); }
    else if (!ls && !rs) inject_key(KEY_LEFTSHIFT, 0);
}
static void remote_paste_keys(void) {
    int ls = mac_down[0xE1], rs = mac_down[0xE5], ctl = mac_down[0xE0] || mac_down[0xE4];
    if (ls) send_key_mac(0xE1, 0); if (rs) send_key_mac(0xE5, 0); if (!ctl) send_key_mac(0xE0, 1);
    send_key_mac(0x19, 1); send_key_mac(0x19, 0);
    if (!ctl) send_key_mac(0xE0, 0); if (ls) send_key_mac(0xE1, 1); if (rs) send_key_mac(0xE5, 1);
}

static void flush_motion(struct dev *d) {
    if (d->dx || d->dy || d->wx || d->wy) {
        uint8_t m[10]; int n = 0;
        if (d->dx || d->dy) {
            int dx = d->dx < -32768 ? -32768 : d->dx > 32767 ? 32767 : d->dx, dy = d->dy < -32768 ? -32768 : d->dy > 32767 ? 32767 : d->dy;
            m[n] = 0x10; put16(m + n + 1, (uint16_t)dx); put16(m + n + 3, (uint16_t)dy); n += 5;
        }
        if (d->wx || d->wy) { m[n] = 0x12; put16(m + n + 1, (uint16_t)d->wx); put16(m + n + 3, (uint16_t)d->wy); n += 5; }
        udp_send(m, n);
    }
    d->dx = d->dy = d->wx = d->wy = 0;
}

static void handle_dev(struct dev *d) {
    struct input_event e[64];
    ssize_t n = read(d->fd, e, sizeof e);
    if (n < 0) { if (errno == ENODEV || errno == EIO) dev_close(d); return; }
    for (int i = 0; i < (int)(n / sizeof *e); i++) {
        struct input_event *x = &e[i];
        if (x->type == EV_KEY && x->code < KEY_CNT && x->code < BTN_MISC) {   /* keyboard keys */
            int c = x->code, v = x->value;
            if (v == 2) continue;                 /* repeats are made by the side that receives */
            phys_down[c] = v;
            if (swallowed[c]) { if (!v) swallowed[c] = 0; continue; }
            int ctl = phys_down[KEY_LEFTCTRL] || phys_down[KEY_RIGHTCTRL], sh = phys_down[KEY_LEFTSHIFT] || phys_down[KEY_RIGHTSHIFT];
            int other = phys_down[KEY_LEFTALT] || phys_down[KEY_RIGHTALT] || phys_down[KEY_LEFTMETA] || phys_down[KEY_RIGHTMETA];
            if (v == 1 && (c == KEY_C || c == KEY_V) && ctl && sh && !other && hsock >= 0 && !st.usual && st.clipboard && st.sharing) { swallowed[c] = 1; local_shortcut(c); continue; }
            if (state == DRIVING) {
                if (phys_down[KEY_LEFTSHIFT] && phys_down[KEY_RIGHTSHIFT]) {   /* take the pointer back */
                    send_leave(0); leave_until_ms = mono_ms() + 1500; next_leave_ms = mono_ms() + 200;
                    stop_driving("both Shift keys", 0, 0); claim = 1;
                    continue;
                }
                if (c < 256 && key_to_usage[c]) send_key_mac(key_to_usage[c], v);
            } else if (d->grabbed) {
                if (c < 256) inject_key(c, v);
            }
        } else if (x->type == EV_KEY && x->code >= BTN_MOUSE && x->code < BTN_JOYSTICK) {
            int b = 0;
            switch (x->code) { case BTN_LEFT: b = 1; break; case BTN_RIGHT: b = 2; break; case BTN_MIDDLE: b = 3; break;
                               case BTN_SIDE: case BTN_BACK: b = 4; break; case BTN_EXTRA: case BTN_FORWARD: b = 5; break; }
            if (x->value) d->btns |= 1 << b; else d->btns &= ~(1 << b);
            if (state == DRIVING && b && x->value != 2) {
                flush_motion(d);
                uint8_t m[3] = {0x11, (uint8_t)b, (uint8_t)(x->value ? 1 : 0)}; udp_send(m, 3);
            }
        } else if (x->type == EV_REL) {
            switch (x->code) {
            case REL_X: d->dx += x->value; break;
            case REL_Y: d->dy += x->value; break;
            case REL_WHEEL_HI_RES: d->wy -= x->value; break;
            case REL_HWHEEL_HI_RES: d->wx += x->value; break;
            case REL_WHEEL: if (!d->hires) d->wy -= x->value * 120; break;
            case REL_HWHEEL: if (!d->hires) d->wx += x->value * 120; break;
            }
        } else if (x->type == EV_SYN && x->code == SYN_REPORT && d->mouse) {
            if (state == DRIVING) { flush_motion(d); continue; }
            int dx = d->dx, dy = d->dy; d->dx = d->dy = d->wx = d->wy = 0;
            if (!dx && !dy) continue;
            int hold = state == IDLE || state == DRIVEN;
            if (hold) {   /* this computer's own mouse moved the pointer: follow it (the query below corrects it) */
                px += dx; py += dy;
                if (px < 0) px = 0; if (py < 0) py = 0; if (px > scr_w - 1) px = scr_w - 1; if (py > scr_h - 1) py = scr_h - 1;
                pos_dirty = 1;
            }
            int64_t now = mono_ms();
            /* edges: ask where the pointer is now; the answer adds these moves to the push */
            if (hold && !(d->btns & 0x0E) && hsock >= 0) {
                if (now - loc_at > PUSH_GAP_MS) loc_dx = loc_dy = 0;
                loc_dx += dx; loc_dy += dy; loc_at = now;
                if (!query_out && now - last_query_ms >= 8) query(1);
            } else if (hold) { push_reset(); loc_dx = loc_dy = 0; }
        }
    }
}

