/* --- presence of the linked peer (PROTOCOL.md PRESENT) --- */
static int cursor_hidden_here;          /* the helper hid this computer's pointer (it is on the peer) */
static uint64_t present_high;
static int64_t next_present_ms, next_sweep_ms, next_ifscan_ms, missing_since_ms, settings_sent_ms;
static int was_online;
static int peer_online(void) { return shm->peer_seen_ms > 0 && mono_ms() - shm->peer_seen_ms < 40000; }
static void make_present(buf_t *b, int method, void *arg) {
    int reply = *(int *)arg;
    buf_t j = {0};
    bprintf(&j, "{\"id\": \"%s\", \"dp\": %d, ", my_id_hex, PORT_UDP); put_addrs(&j);
    bprintf(&j, ", \"reply\": %s, \"via\": \"%s\"}", reply ? "true" : "false", method_name(method));
    uint8_t out[1600]; size_t c = j.n < 1400 ? seal(j.p, j.n, out) : 0; bfree(&j);
    uint8_t t = 0x03; b->n = 0; bput(b, MAGIC, 6); bput(b, &t, 1); bput(b, lk.link_id, 8); bput(b, out, c);
}
static void send_present(int reply, int sweep_too) {
    if (!lk.ok) return;
    int dport = shm->peer_dp ? shm->peer_dp : lk.peer_dp;
    dsock_send_by(&pres, dport, M_BCAST | M_MCAST | M_V6, make_present, &reply);
    struct sockaddr_in a;
    if (peer_sockaddr(&a, dport) == 0) { buf_t b = {0}; make_present(&b, 0, &reply); sendto(udp, b.p, b.n, MSG_DONTWAIT, (void *)&a, sizeof a); bfree(&b); }
    if (sweep_too && (methods & M_SWEEP)) sweep_start();
}
static void handle_present(const uint8_t *in, size_t n, struct sockaddr_storage *src) {
    if (!lk.ok || n < 15 + 28 || in[6] != 0x03 || memcmp(in + 7, lk.link_id, 8)) return;
    uint8_t m[1600]; uint64_t ctr; long ml = unseal(in + 15, n - 15, m, &ctr);
    if (ml < 2 || ml > 1500 || ctr <= present_high) return;
    present_high = ctr; m[ml] = 0;
    char id[40] = "", via[16] = "direct";
    if (!json_str((char *)m, "id", id, sizeof id) || strcmp(id, lk.peer_id_hex)) return;
    json_str((char *)m, "via", via, sizeof via);
    int dp = (int)json_long((char *)m, "dp", 0); if (dp > 0 && dp < 65536) shm->peer_dp = dp;
    int missing = !peer_online();
    peer_heard(pick_addr((char *)m, src), via);
    if (json_bool((char *)m, "reply", 0) || missing) send_present(0, 0);
}
static void handle_present6(void) {
    uint8_t in[2048]; struct sockaddr_storage src; socklen_t sl;
    for (;;) {
        sl = sizeof src;
        ssize_t n = recvfrom(pres.s6, in, sizeof in, MSG_DONTWAIT, (void *)&src, &sl);
        if (n < 0) return;
        if (n > 7 && !memcmp(in, MAGIC, 6)) handle_present(in, n, &src);
    }
}

/* --- the other processes: helper (X, clipboard) and agent (control socket, finding, linking) --- */
static void agent_main(int fd);
static void close_inherited(void) {
    if (udp >= 0) close(udp); if (kfd >= 0) close(kfd); if (pfd >= 0) close(pfd); if (pres.s6 >= 0) close(pres.s6);
    if (ino_dev >= 0) close(ino_dev); if (ino_conf >= 0) close(ino_conf);
    for (int i = 0; i < MAXDEV; i++) if (devs[i].fd > 0) close(devs[i].fd);
}
static void send_local_settings(int to) { buf_t b = {0}; settings_json(&b, 0); hsend(to, 'G', b.p, b.n); bfree(&b); }
static void helper_start(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return;
    pid_t p = fork();
    if (p == 0) { close(sv[0]); if (asock >= 0) close(asock); close_inherited(); helper_main(sv[1]); _exit(0); }
    close(sv[1]);
    if (p < 0) { close(sv[0]); return; }
    helper_pid = p; hsock = sv[0]; query_out = 0; tell_here(); send_local_settings(hsock);
}
static void helper_lost(void) {
    if (hsock >= 0) close(hsock);
    hsock = -1; query_out = 0; pending_copy = pending_paste = 0; cursor_hidden_here = 0;
    int stt; waitpid(helper_pid, &stt, 0);
    helper_restart_at = mono_ms() + 1000;
}
static void agent_start(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return;
    pid_t p = fork();
    if (p == 0) { close(sv[0]); if (hsock >= 0) close(hsock); close_inherited(); agent_main(sv[1]); _exit(0); }
    close(sv[1]);
    if (p < 0) { close(sv[0]); return; }
    agent_pid = p; asock = sv[0];
}
static void agent_lost(void) {
    if (asock >= 0) close(asock);
    asock = -1; int stt; waitpid(agent_pid, &stt, 0);
    agent_restart_at = mono_ms() + 1000;
}

/* State for the tray app and the command line (sent to the agent when it changes). */
static char last_state[4096];
static int64_t forget_deadline;
static void state_json(buf_t *b) {
    const char *mode = state == DRIVEN || state == LEAVING ? "driven" : state == DRIVING ? "driving" : "idle";
    struct in_addr a = {shm->peer_ip};
    int nk = 0, nm = 0; for (int i = 0; i < MAXDEV; i++) if (devs[i].fd > 0) { nk += devs[i].kbd; nm += devs[i].mouse; }
    bprintf(b, "{\"linked\": %s, \"peer_name\": ", lk.ok ? "true" : "false"); json_put_str(b, lk.ok ? lk.peer_name : "");
    bprintf(b, ", \"peer_id\": \"%s\", \"online\": %s, \"peer_address\": \"%s\", \"found_by\": \"%s\", \"mode\": \"%s\", \"cursor_hidden\": %s, \"desktop\": %s, \"keyboards\": %d, \"mice\": %d, \"paste_waiting\": %s, \"settings\": ",
            lk.ok ? lk.peer_id_hex : "", peer_online() ? "true" : "false", shm->peer_ip ? inet_ntoa(a) : "", (const char *)shm->found_by, mode, cursor_hidden_here ? "true" : "false",
            hsock >= 0 ? "true" : "false", nk, nm, paste_wait ? "true" : "false");
    settings_json(b, 0);
    bputs(b, ", \"error\": "); json_put_str(b, last_error); bputs(b, "}");
}
static void tell_agent(int force) {
    if (asock < 0) return;
    buf_t b = {0}; state_json(&b); bz(&b);
    if (force || strcmp((char *)b.p, last_state)) { snprintf(last_state, sizeof last_state, "%s", (char *)b.p); hsend(asock, 'S', b.p, b.n); }
    bfree(&b);
}
static void diag_json(buf_t *b) {
    uint64_t avg = lat_n ? lat_sum_ns / lat_n / 1000 : 0;
    bprintf(b, "{\"state\": \"%s\", \"pointer\": [%d, %d], \"screen\": [%d, %d], \"edge\": \"%s\", \"speed\": %.2f, \"inject_us\": {\"count\": %llu, \"avg\": %llu, \"max\": %llu}, \"net_ms\": {\"moves\": %llu, ",
            STATE_NAMES[state], (int)px, (int)py, scr_w, scr_h, EDGE_NAMES[cfg.edge], cfg.speed, (unsigned long long)lat_n, (unsigned long long)avg, (unsigned long long)(lat_max_ns / 1000), (unsigned long long)net_moves);
    char t[1024]; int k = net_json(t, sizeof t, "arrival_gap", net_arr); bput(b, t, k); bputs(b, ", ");
    k = net_json(t, sizeof t, "delay_above_min", net_late); bput(b, t, k); bputs(b, "}}");
}
/* Settings changed (here or by the peer): apply now. */
static void settings_applied(int tell_peer) {
    save_settings();
    mod_keys(st.command_super ? "super" : "control", &cfg.cmd_l, &cfg.cmd_r); mod_keys(st.command_super ? "control" : "super", &cfg.ctl_l, &cfg.ctl_r);
    if ((!st.sharing || !st.peer_drives) && (state == DRIVEN || state == LEAVING)) end_driven("switched off");
    if ((!st.sharing || !st.drives_peer) && state == DRIVING) { send_leave(0); stop_driving("switched off", 0, 0); }
    edge_pushes = 0;
    if (hsock >= 0) {
        send_local_settings(hsock);
        if (tell_peer) { buf_t b = {0}; settings_json(&b, 1); hsend(hsock, 'S', b.p, b.n); bfree(&b); settings_sent_ms = mono_ms(); }
    }
    tell_agent(0);
}
static void handle_helper(void) {
    static buf_t b; uint8_t t;
    if (hrecv(hsock, &t, &b)) { helper_lost(); return; }
    if (t == 'Z' && b.n >= 8) { int32_t s[2]; memcpy(s, b.p, 8); if (s[0] > 0 && s[1] > 0) { scr_w = s[0]; scr_h = s[1]; } }
    else if (t == 'E') { set_error("%.*s", (int)b.n, b.p); tell_agent(0); }
    else if (t == 'K' && b.n) { cursor_hidden_here = b.p[0]; tell_agent(0); }
    else if (t == 'T' && asock >= 0) hsend(asock, 'T', b.p, b.n);
    else if (t == 'N' && b.n) { snprintf(lk.peer_name, sizeof lk.peer_name, "%.*s", (int)b.n, b.p); tell_agent(0); }
    else if (t == 'G') {   /* settings from the peer */
        char j[4096]; size_t n = b.n < sizeof j - 1 ? b.n : sizeof j - 1; memcpy(j, b.p, n); j[n] = 0;
        if (settings_from_peer(j)) { logmsg("settings from %s adopted", lk.peer_name); settings_applied(0); }
        else { buf_t w = {0}; settings_json(&w, 1); hsend(hsock, 'S', w.p, w.n); bfree(&w); }   /* ours are newer */
    }
    else if (t == 'F') { logmsg("%s forgot this link", lk.peer_name); forget_link(); }
    else if (t == 'f') { forget_link(); }
    else if (t == 'W' && b.n >= 1) {
        paste_wait = b.p[0]; paste_wait_until = mono_ms() + 600000;
        if (!paste_wait && paste_deferred) { paste_deferred = 0; local_combo(KEY_V, paste_shift); logmsg("held paste done"); }
    }
    else if (t == 'X' && b.n >= 24) {
        int32_t r[6]; memcpy(r, b.p, 24); query_out = 0;
        if (r[3] > 0 && r[4] > 0) { scr_w = r[3]; scr_h = r[4]; }
        int hold = state == IDLE || state == DRIVEN;
        if ((r[5] == 1 || r[5] == 2) && hold) { px = r[0]; py = r[1]; pos_dirty = 0; }
        if (r[5] == 1 && hold && mono_ms() >= cooldown_ms && st.sharing && st.drives_peer && shm->peer_ip) {
            int at = (cfg.edge == 0 && r[0] <= 0) || (cfg.edge == 1 && r[0] >= scr_w - 1) || (cfg.edge == 2 && r[1] <= 0) || (cfg.edge == 3 && r[1] >= scr_h - 1);
            if (at && !(r[2] & (Button1Mask | Button2Mask | Button3Mask))) {
                if (++edge_pushes >= 2) hand_over(r[0], r[1]);
            } else edge_pushes = 0;
        }
        if (r[5] == 3) { px = r[0]; py = r[1]; }
    }
    else if (t == 'k' && b.n >= 1 && pending_copy) { pending_copy = 0; local_combo(KEY_C, b.p[0]); hsend(hsock, 'C', NULL, 0); }
    else if (t == 'p' && b.n >= 2 && pending_paste) { pending_paste = 0; if (b.p[1]) local_combo(KEY_V, b.p[0]); }
    else if (t == 'o' && b.n >= 1 && state == DRIVING) { if (b.p[0]) remote_paste_keys(); }
}
static void handle_agent(void) {
    static buf_t b; uint8_t t;
    if (hrecv(asock, &t, &b)) { agent_lost(); return; }
    char j[4096]; size_t n = b.n < sizeof j - 1 ? b.n : sizeof j - 1; memcpy(j, b.p, n); j[n] = 0;
    if (t == 'G') { settings_from_local(j); settings_applied(1); }
    else if (t == 'F' && lk.ok) {   /* forget: tell the peer first (at most 3 s), then forget here */
        if (hsock >= 0 && peer_online()) { hsend(hsock, 'f', NULL, 0); forget_deadline = mono_ms() + 3000; } else forget_link();
    }
    else if (t == 'B') {
        if (state == DRIVING) { send_leave(0); leave_until_ms = mono_ms() + 1500; next_leave_ms = mono_ms() + 200; stop_driving("asked from the menu", 0, 0); }

    }
    else if (t == 'q') { buf_t d = {0}; diag_json(&d); hsend(asock, 'D', d.p, d.n); bfree(&d); tell_agent(1); }
}

static int64_t next_deadline(void) {
    int64_t d = INT64_MAX;
    if (state == ENTERING) { d = next_retx_ms < deadline_ms ? next_retx_ms : deadline_ms; }
    if (leave_until_ms && next_leave_ms < d) d = next_leave_ms;
    if (state == DRIVEN) d = last_rx_ms + 2000;
    if (state == DRIVING) { d = next_alive_ms < last_rx_ms + 2000 ? next_alive_ms : last_rx_ms + 2000; }
    if (hsock < 0 && linked && helper_restart_at < d) d = helper_restart_at;
    if (asock < 0 && agent_restart_at < d) d = agent_restart_at;
    if (paste_wait && paste_wait_until < d) d = paste_wait_until;
    if (linked) {
        if (next_present_ms < d) d = next_present_ms;
        if (next_ifscan_ms < d) d = next_ifscan_ms;
        if (sweep.active) { int64_t s = mono_ms() + 50; if (s < d) d = s; }
        int64_t off = shm->peer_seen_ms + 40000; if (was_online && off < d) d = off;
    }
    if (forget_deadline && forget_deadline < d) d = forget_deadline;
    return d;
}
static void timers(void) {
    int64_t now = mono_ms();
    if (hsock < 0 && linked && now >= helper_restart_at) helper_start();
    if (asock < 0 && now >= agent_restart_at) { agent_start(); tell_agent(1); }
    if (forget_deadline && now >= forget_deadline) { forget_deadline = 0; forget_link(); }
    if (paste_wait && now >= paste_wait_until) { paste_wait = 0; paste_deferred = 0; set_error("files from %s did not arrive; the held paste was dropped", lk.peer_name); }
    if (leave_until_ms) {   /* LEAVE repeated until the peer confirms it gave the pointer back */
        if (now >= leave_until_ms) leave_until_ms = 0;
        else if (now >= next_leave_ms) { send_leave(0); next_leave_ms = now + 200; }
    }
    if (state == ENTERING) {
        if (now >= deadline_ms) { state = IDLE; cooldown_ms = now + 1000; set_error("%s did not answer when the pointer reached the %s edge", lk.peer_name, EDGE_NAMES[cfg.edge]); }
        else if (now >= next_retx_ms) { send_enter(); next_retx_ms = now + 200; }
    } else if (state == DRIVEN) {
        if (now - last_rx_ms >= 2000) { end_driven("no packet for 2 s"); set_error("%s went silent while its keyboard was in use here; keys released", lk.peer_name); }
    } else if (state == DRIVING) {
        if (now - last_rx_ms >= 2000) { stop_driving("no answer for 2 s", 0, 0); set_error("%s stopped answering; the pointer is back here", lk.peer_name); }
        else if (now >= next_alive_ms) { uint8_t m = 0x1F; udp_send(&m, 1); next_alive_ms = now + 500; }
    }
    if (!linked) return;
    if (now >= next_ifscan_ms) {
        next_ifscan_ms = now + 10000;
        if (scan_ifaces()) { dsock_join(&pres); next_present_ms = now; logmsg("network changed"); }
    }
    int online = peer_online();
    if (online != was_online) {
        was_online = online;
        if (online) { missing_since_ms = 0; next_present_ms = now + 30000; logmsg("%s found", lk.peer_name); if (hsock >= 0) { buf_t w = {0}; settings_json(&w, 1); hsend(hsock, 'S', w.p, w.n); bfree(&w); } }
        else { missing_since_ms = now; next_present_ms = now; logmsg("%s is missing", lk.peer_name); }
    }
    if (now >= next_present_ms) {
        int sweep_now = !online && missing_since_ms && now - missing_since_ms > 30000 && now >= next_sweep_ms;
        if (sweep_now) next_sweep_ms = now + 120000;
        send_present(!online, sweep_now);
        next_present_ms = now + (online ? 30000 : 5000);
    }
    if (sweep.active) { int reply = 1; sweep_step(&pres, shm->peer_dp ? shm->peer_dp : lk.peer_dp, 10, make_present, &reply); }
    tell_agent(0);
}

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

/* Restart when the link or this computer's config file changes. */
static int conf_changed(int fd) {
    char b[4096] __attribute__((aligned(8))); ssize_t n; int hit = 0;
    while ((n = read(fd, b, sizeof b)) > 0)
        for (char *p = b; p < b + n; ) {
            struct inotify_event *e = (void *)p;
            if (e->len && (!strcmp(e->name, "link") || !strcmp(e->name, "config.json"))) hit = 1;
            p += sizeof *e + e->len;
        }
    return hit;
}

static int run(void) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (sodium_init() < 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    init_methods(); load_identity(); load_config(); load_settings(); init_tables(); shm_init();
    mod_keys(st.command_super ? "super" : "control", &cfg.cmd_l, &cfg.cmd_r); mod_keys(st.command_super ? "control" : "super", &cfg.ctl_l, &cfg.ctl_r);
    mkdirs(RUN_DIR, 0755); mkdirs(CONF_DIR, 0755);
    linked = load_link();
    ino_conf = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    inotify_add_watch(ino_conf, CONF_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM);
    inotify_add_watch(ino_conf, STATE_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM);
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
    if (linked) {
        shm->peer_dp = lk.peer_dp;
        udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT_UDP)};
        /* DSCP CS6 (bcmdhd maps DSCP>>3 = 6 to the Wi-Fi voice queue) and priority 256+6 (mac80211 voice) */
        int tos = 0xc0; setsockopt(udp, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        int prio = 256 + 6; setsockopt(udp, SOL_SOCKET, SO_PRIORITY, &prio, sizeof prio);
        int one = 1; setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(udp, (void *)&a, sizeof a)) { logmsg("cannot bind UDP %d: %s", PORT_UDP, strerror(errno)); return 1; }
        scan_ifaces(); dsock_open(&pres, PORT_UDP, udp); dsock_join(&pres);
        kfd = ui_create(0); pfd = ui_create(1);
        if (kfd < 0 || pfd < 0) logmsg("cannot create virtual input devices: the keyboard and mouse can't be shared");
        ino_dev = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); inotify_add_watch(ino_dev, "/dev/input", IN_CREATE | IN_ATTRIB);
        usleep(300000);   /* let the compositor pick up the virtual devices */
        dev_scan();
        helper_start();
        next_present_ms = mono_ms(); next_ifscan_ms = mono_ms() + 10000; missing_since_ms = mono_ms();
        logmsg("running: linked with %s, edge %s, speed %.2f, ports %d-%d", lk.peer_name, EDGE_NAMES[cfg.edge], cfg.speed, PORT_UDP, PORT_DISC);
    } else logmsg("not linked yet: waiting for a link (shenzhen-kvm visible, shenzhen-kvm link, or the tray app)");
    agent_start(); tell_agent(1);
    struct pollfd q[8 + MAXDEV]; int restart = 0;
    while (!quit && !restart) {
        int n = 0;
        q[n++] = (struct pollfd){udp, POLLIN, 0};
        q[n++] = (struct pollfd){hsock, POLLIN, 0};
        q[n++] = (struct pollfd){ino_dev, POLLIN, 0};
        q[n++] = (struct pollfd){ino_conf, POLLIN, 0};
        q[n++] = (struct pollfd){asock, POLLIN, 0};
        q[n++] = (struct pollfd){kfd, POLLIN, 0};
        q[n++] = (struct pollfd){pres.s6, POLLIN, 0};
        int base = n, map[MAXDEV];
        for (int i = 0; i < MAXDEV; i++) if (devs[i].fd > 0) { map[n - base] = i; q[n++] = (struct pollfd){devs[i].fd, POLLIN, 0}; }
        int64_t dl = next_deadline(), now = mono_ms();
        int to = dl == INT64_MAX ? -1 : dl <= now ? 0 : (int)(dl - now);
        if (poll(q, n, to) < 0 && errno != EINTR) break;
        if (q[0].revents & POLLIN) handle_udp();
        if (hsock >= 0 && q[1].revents) handle_helper();
        if (q[2].revents & POLLIN) { char b[4096]; while (read(ino_dev, b, sizeof b) > 0) ; dev_scan(); }
        if (q[3].revents & POLLIN) restart = conf_changed(ino_conf);
        if (asock >= 0 && q[4].revents) handle_agent();
        if (q[5].revents & POLLIN) {   /* LED state from the compositor -> physical keyboards */
            struct input_event e[16]; ssize_t r = read(kfd, e, sizeof e);
            for (int i = 0; i < (int)(r > 0 ? r / sizeof *e : 0); i++) if (e[i].type == EV_LED)
                for (int k = 0; k < MAXDEV; k++) if (devs[k].fd > 0 && devs[k].kbd) { struct input_event o[2] = {e[i], {.type = EV_SYN}}; write(devs[k].fd, o, sizeof o); }
        }
        if (q[6].revents & POLLIN) handle_present6();
        for (int i = base; i < n; i++) if (q[i].revents) { struct dev *d = &devs[map[i - base]]; if (q[i].revents & (POLLERR | POLLHUP)) dev_close(d); else handle_dev(d); }
        if ((state == DRIVEN || state == IDLE) && pos_dirty && !query_out && hsock >= 0) query(2);
        timers();
    }
    if (state == DRIVING) { send_leave(0); stop_driving("service stopping", 0, 0); }
    release_all();
    if (hsock >= 0) { close(hsock); kill(helper_pid, SIGTERM); waitpid(helper_pid, NULL, 0); }
    if (asock >= 0) { close(asock); kill(agent_pid, SIGTERM); waitpid(agent_pid, NULL, 0); }
    if (kfd >= 0) ioctl(kfd, UI_DEV_DESTROY); if (pfd >= 0) ioctl(pfd, UI_DEV_DESTROY);
    if (restart) { logmsg("link or settings changed: restarting"); execl("/proc/self/exe", "shenzhen-kvm", "run", (char *)NULL); }
    return 0;
}
