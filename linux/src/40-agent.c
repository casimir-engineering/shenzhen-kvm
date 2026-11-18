/* ======================================================================================
 * AGENT PROCESS: control socket (tray app, command line), finding computers, linking
 * ====================================================================================== */
static int afd;                                  /* to main */
static char main_state[4096] = "{}", main_diag[2048] = "{}";
#define NXFER 5
static struct { int id; char json[1400]; int64_t ended; } xfers[NXFER];
#define MAXCL 12
static struct client { int fd; int sub, looking, want_status; buf_t in; } cl[MAXCL];
static int ctl_fd = -1;
static int64_t visible_until;                    /* mono ms, 0 = not visible */
#define NFOUND 16
static struct found { char id[33], name[128]; uint32_t ip; int dp, by; int64_t seen; } found[NFOUND];
static dsock_t disc = {-1, -1, 0};
static int link_lfd = -1, mdns_fd = -1;
static int64_t next_announce_ms, next_look_ms, next_mdns_ms, next_sweep_look_ms, mdns_announced, mdns_queries;

/* --- the linking session (PROTOCOL.md Linking) --- */
enum { S_NONE, S_CONNECTING, S_WAIT_HELLO, S_WAIT_REQUEST, S_WAIT_REVEAL, S_CONFIRM, S_DONE, S_FAILED };
static struct session {
    int fd, stage; char role; buf_t in;
    uint8_t sk[32], pk[32], peer_pk[32], n_me[32], n_peer[32], commit[32], peer_id[16], key[32], link_id[8], th[32];
    char peer_name[128], peer_id_hex[33], code[8], message[300]; int peer_dp, me_ok, peer_ok; int64_t deadline, shown_until;
} ss = {.fd = -1};

static int looking_now(void) { for (int i = 0; i < MAXCL; i++) if (cl[i].fd > 0 && cl[i].looking) return 1; return 0; }
static int visible_now(void) { return visible_until && mono_ms() < visible_until; }

static void sha256_parts(uint8_t out[32], int n, ...) {
    crypto_hash_sha256_state h; crypto_hash_sha256_init(&h);
    va_list a; va_start(a, n);
    for (int i = 0; i < n; i++) { const void *p = va_arg(a, const void *); size_t l = va_arg(a, size_t); crypto_hash_sha256_update(&h, p, l); }
    va_end(a); crypto_hash_sha256_final(&h, out);
}
static void hmac256(uint8_t out[32], const uint8_t *k, size_t kl, const void *m, size_t ml, const void *m2, size_t ml2) {
    crypto_auth_hmacsha256_state h; crypto_auth_hmacsha256_init(&h, k, kl);
    crypto_auth_hmacsha256_update(&h, m, ml); if (m2) crypto_auth_hmacsha256_update(&h, m2, ml2);
    crypto_auth_hmacsha256_final(&h, out);
}

/* --- state for clients --- */
static void found_json(buf_t *b) {
    bputs(b, "["); int first = 1; int64_t now = mono_ms();
    for (int i = 0; i < NFOUND; i++) {
        if (!found[i].id[0] || now - found[i].seen > 45000) continue;
        struct in_addr a = {found[i].ip};
        bprintf(b, "%s{\"id\": \"%s\", \"name\": ", first ? "" : ", ", found[i].id); json_put_str(b, found[i].name);
        bprintf(b, ", \"address\": \"%s\", \"by\": [", inet_ntoa(a));
        int f2 = 1; for (int m = 0; m < 5; m++) if (found[i].by & (1 << m)) { bprintf(b, "%s\"%s\"", f2 ? "" : ", ", METHOD_NAMES[m]); f2 = 0; }
        bputs(b, "]}"); first = 0;
    }
    bputs(b, "]");
}
static void full_state(buf_t *b) {
    size_t l = strlen(main_state); while (l && main_state[l - 1] != '}') l--;
    if (l < 2) { bputs(b, "{"); } else { bput(b, main_state, l - 1); bputs(b, ", "); }
    bputs(b, "\"type\": \"state\", \"this_name\": "); json_put_str(b, my_name);
    int64_t left = visible_now() ? (visible_until - mono_ms() + 999) / 1000 : 0;
    bprintf(b, ", \"visible_left\": %lld, \"looking\": %s, \"found\": ", (long long)left, looking_now() ? "true" : "false");
    found_json(b);
    static const char *SN[] = {"none", "connecting", "waiting", "waiting", "waiting", "confirm", "done", "failed"};
    bprintf(b, ", \"linking\": {\"stage\": \"%s\", \"role\": \"%s\", \"peer_name\": ", SN[ss.stage], ss.role == 'V' ? "asked" : "asking");
    json_put_str(b, ss.peer_name); bprintf(b, ", \"code\": \"%s\", \"you_confirmed\": %s, \"they_confirmed\": %s, \"message\": ", ss.stage == S_CONFIRM || ss.stage == S_DONE ? ss.code : "",
                                           ss.me_ok ? "true" : "false", ss.peer_ok ? "true" : "false");
    json_put_str(b, ss.message); bputs(b, "}, \"transfers\": [");
    int first = 1;   /* newest first */
    int order[NXFER], no = 0;
    for (int i = 0; i < NXFER; i++) if (xfers[i].id && !(xfers[i].ended && mono_ms() - xfers[i].ended > 600000)) order[no++] = i;
    for (int a = 0; a < no; a++) for (int c = a + 1; c < no; c++) if (xfers[order[c]].id > xfers[order[a]].id) { int t = order[a]; order[a] = order[c]; order[c] = t; }
    for (int i = 0; i < no; i++) { bprintf(b, "%s%s", first ? "" : ", ", xfers[order[i]].json); first = 0; }
    bputs(b, "]}");
}
static char last_pushed[16384];
static void push_state(int force) {
    buf_t b = {0}; full_state(&b); bz(&b);
    if (force || strcmp((char *)b.p, last_pushed)) {
        snprintf(last_pushed, sizeof last_pushed, "%s", (char *)b.p);
        bputs(&b, "\n");
        for (int i = 0; i < MAXCL; i++) if (cl[i].fd > 0 && cl[i].sub) write_all(cl[i].fd, b.p, b.n);
    }
    bfree(&b);
}

/* --- discovery messages (PROTOCOL.md Finding computers) --- */
static void make_disc(buf_t *b, int method, void *arg) {
    int type = *(int *)arg; uint8_t t = type;
    b->n = 0; bput(b, MAGIC, 6); bput(b, &t, 1);
    bprintf(b, "{\"id\": \"%s\", \"name\": ", my_id_hex); json_put_str(b, my_name);
    bprintf(b, ", \"dp\": %d, ", PORT_UDP); put_addrs(b);
    if (type == 0x02) bprintf(b, ", \"left\": %lld", (long long)((visible_until - mono_ms()) / 1000));
    bprintf(b, ", \"via\": \"%s\", \"v\": 1}", method_name(method));
}
static void found_add(const char *id, const char *name, uint32_t ip, int dp, int by) {
    if (!id[0] || !strcmp(id, my_id_hex) || !ip) return;
    int i, free_i = -1, old = 0;
    for (i = 0; i < NFOUND && strcmp(found[i].id, id); i++) ;
    if (i == NFOUND) {
        for (int k = 0; k < NFOUND; k++) if (!found[k].id[0] || mono_ms() - found[k].seen > 45000) { free_i = k; break; }
        if (free_i < 0) { for (int k = 1; k < NFOUND; k++) if (found[k].seen < found[old].seen) old = k; free_i = old; }
        i = free_i; memset(&found[i], 0, sizeof found[i]); snprintf(found[i].id, sizeof found[i].id, "%s", id);
    }
    if (!(found[i].by & by) || found[i].ip != ip) { struct in_addr a = {ip}; logmsg("found %s at %s by %s", name, inet_ntoa(a), method_name(by)); }
    snprintf(found[i].name, sizeof found[i].name, "%s", name[0] ? name : "Computer"); found[i].ip = ip; found[i].dp = dp > 0 ? dp : 24800; found[i].by |= by; found[i].seen = mono_ms();
}
static int method_bit(const char *via) { for (int i = 0; i < 5; i++) if (!strcmp(via, METHOD_NAMES[i])) return 1 << i; return M_SWEEP; }
static void reply_to(int s, struct sockaddr_storage *src, socklen_t sl, int type, int method) {
    buf_t b = {0}; make_disc(&b, method, &type); sendto(s, b.p, b.n, MSG_DONTWAIT, (void *)src, sl); bfree(&b);
}
static void disc_recv(int s) {
    uint8_t in[2048]; struct sockaddr_storage src; socklen_t sl;
    for (;;) {
        sl = sizeof src;
        ssize_t n = recvfrom(s, in, sizeof in - 1, MSG_DONTWAIT, (void *)&src, &sl);
        if (n < 0) return;
        if (n < 8 || memcmp(in, MAGIC, 6)) continue;
        in[n] = 0; const char *j = (char *)in + 7;
        char id[40] = "", name[128] = "", via[16] = "direct";
        if (!json_str(j, "id", id, sizeof id) || !strcmp(id, my_id_hex)) continue;
        json_str(j, "name", name, sizeof name); json_str(j, "via", via, sizeof via);
        int m = method_bit(via);
        if (!(methods & m)) continue;   /* tests: a method switched off here is off both ways */
        if (in[6] == 0x01 && visible_now()) reply_to(s, &src, sl, 0x02, m);
        else if (in[6] == 0x02 && looking_now()) found_add(id, name, pick_addr(j, &src), (int)json_long(j, "dp", 24800), m);
    }
}

/* --- mDNS (a small responder and browser of our own; it shares 5353 with Avahi) --- */
static void dns_label(buf_t *b, const char *s, size_t n) { uint8_t l = n > 63 ? 63 : n; bput(b, &l, 1); bput(b, s, l); }
static void dns_service(buf_t *b) { dns_label(b, "_shenzhen-kvm", 13); dns_label(b, "_udp", 4); dns_label(b, "local", 5); uint8_t z = 0; bput(b, &z, 1); }
static void dns_instance(buf_t *b) { dns_label(b, my_name, strlen(my_name)); dns_service(b); }
static void dns_host(buf_t *b) { char h[40]; snprintf(h, sizeof h, "szkvm-%.8s", my_id_hex); dns_label(b, h, strlen(h)); dns_label(b, "local", 5); uint8_t z = 0; bput(b, &z, 1); }
static void dns_rr(buf_t *b, void (*name)(buf_t *), unsigned type, unsigned cls, uint32_t ttl, const void *rd, size_t rl) {
    uint8_t h[10]; name(b); put16(h, type); put16(h + 2, cls); put32(h + 4, ttl); put16(h + 8, rl); bput(b, h, 10); bput(b, rd, rl);
}
static void mdns_answer(int bye, struct sockaddr_storage *to, socklen_t tl, uint16_t qid, int legacy) {
    if (mdns_fd < 0) return;
    buf_t b = {0}, rd = {0}; uint8_t hdr[12] = {0};
    int nadd = 0; for (int i = 0; i < nifs; i++) if (ifs[i].v4) nadd++;
    put16(hdr, legacy ? qid : 0); put16(hdr + 2, 0x8400); put16(hdr + 6, 1); put16(hdr + 10, 2 + nadd);
    bput(&b, hdr, 12);
    uint32_t ttl = bye ? 0 : 120;
    dns_instance(&rd); dns_rr(&b, dns_service, 12, 1, ttl, rd.p, rd.n); rd.n = 0;                  /* PTR */
    uint8_t srv[6] = {0}; put16(srv + 4, PORT_DISC); bput(&rd, srv, 6); dns_host(&rd);
    dns_rr(&b, dns_instance, 33, 0x8001, ttl, rd.p, rd.n); rd.n = 0;                               /* SRV */
    char kv[200]; int k;
    k = snprintf(kv, sizeof kv, "id=%s", my_id_hex); dns_label(&rd, kv, k);
    k = snprintf(kv, sizeof kv, "name=%s", my_name); dns_label(&rd, kv, k);
    k = snprintf(kv, sizeof kv, "dp=%d", PORT_UDP); dns_label(&rd, kv, k);
    dns_label(&rd, "v=1", 3);
    dns_rr(&b, dns_instance, 16, 0x8001, ttl, rd.p, rd.n); rd.n = 0;                               /* TXT */
    for (int i = 0; i < nifs; i++) if (ifs[i].v4) dns_rr(&b, dns_host, 1, 0x8001, ttl, &ifs[i].addr, 4);   /* A */
    if (to) sendto(mdns_fd, b.p, b.n, MSG_DONTWAIT, (void *)to, tl);
    else for (int i = 0; i < nifs; i++) if (ifs[i].v4) { struct in_addr g; inet_pton(AF_INET, "224.0.0.251", &g); send4_if(mdns_fd, i, g.s_addr, 5353, b.p, b.n); }
    bfree(&b); bfree(&rd);
}
static void mdns_query(void) {
    if (mdns_fd < 0) return;
    buf_t b = {0}; uint8_t hdr[12] = {0}, q[4]; put16(hdr + 4, 1); bput(&b, hdr, 12); dns_service(&b);
    put16(q, 12); put16(q + 2, mdns_queries < 1 ? 0x8001 : 1); bput(&b, q, 4);   /* the first asks for unicast answers */
    for (int i = 0; i < nifs; i++) if (ifs[i].v4) { struct in_addr g; inet_pton(AF_INET, "224.0.0.251", &g); send4_if(mdns_fd, i, g.s_addr, 5353, b.p, b.n); }
    mdns_queries++; bfree(&b);
}
/* Reads a (possibly compressed) name at *off into labels joined by '\x01'; returns 0 on error. */
static int dns_read_name(const uint8_t *p, size_t n, size_t *off, char *out, size_t on) {
    size_t o = *off, w = 0; int jumped = 0, hops = 0;
    for (;;) {
        if (o >= n || hops > 20) return 0;
        uint8_t l = p[o];
        if (l == 0) { o++; break; }
        if ((l & 0xC0) == 0xC0) { if (o + 1 >= n) return 0; if (!jumped) *off = o + 2; jumped = 1; o = ((l & 0x3F) << 8) | p[o + 1]; hops++; continue; }
        if (o + 1 + l > n || w + l + 2 >= on) return 0;
        if (w) out[w++] = 1;
        memcpy(out + w, p + o + 1, l); w += l; o += 1 + l;
    }
    out[w] = 0; if (!jumped) *off = o;
    return 1;
}
static int ends_with_service(const char *nm) {
    const char *suf = "\x01_shenzhen-kvm\x01_udp\x01local"; size_t a = strlen(nm), b = strlen(suf);
    if (a < b) return !strcasecmp(nm, suf + 1);
    return !strcasecmp(nm + a - b, suf) || !strcasecmp(nm, suf + 1);
}
static void mdns_recv(void) {
    uint8_t p[9000]; struct sockaddr_storage src; socklen_t sl;
    for (;;) {
        sl = sizeof src;
        ssize_t n = recvfrom(mdns_fd, p, sizeof p, MSG_DONTWAIT, (void *)&src, &sl);
        if (n < 12) { if (n < 0) return; continue; }
        unsigned flags = get16(p + 2), qd = get16(p + 4), an = get16(p + 6), ns = get16(p + 8), ar = get16(p + 10);
        size_t off = 12; char nm[512];
        if (!(flags & 0x8000)) {   /* a query: answer it while visible */
            int ask = 0, unicast = 0;
            for (unsigned i = 0; i < qd; i++) {
                if (!dns_read_name(p, n, &off, nm, sizeof nm) || off + 4 > (size_t)n) break;
                unsigned qt = get16(p + off), qc = get16(p + off + 2); off += 4;
                char host[40]; snprintf(host, sizeof host, "szkvm-%.8s\x01local", my_id_hex);
                if ((ends_with_service(nm) && (qt == 12 || qt == 255 || qt == 16 || qt == 33)) || (!strcasecmp(nm, host) && (qt == 1 || qt == 255))) { ask = 1; if (qc & 0x8000) unicast = 1; }
            }
            int legacy = src.ss_family == AF_INET && ntohs(((struct sockaddr_in *)&src)->sin_port) != 5353;
            if (ask && visible_now()) mdns_answer(0, unicast || legacy ? &src : NULL, sl, get16(p), legacy);
            continue;
        }
        if (!looking_now()) continue;
        /* a response: TXT records of our service type give id, name and port; A records the address */
        char id[40] = "", name[128] = "", host[256] = "", target[256] = ""; int dp = 24800; uint32_t ip = 0, aip[8] = {0}; char ahost[8][256]; int na = 0;
        unsigned total = an + ns + ar;
        for (unsigned i = 0; i < qd; i++) { if (!dns_read_name(p, n, &off, nm, sizeof nm)) goto next; off += 4; }
        for (unsigned i = 0; i < total; i++) {
            if (!dns_read_name(p, n, &off, nm, sizeof nm) || off + 10 > (size_t)n) break;
            unsigned type = get16(p + off), rl = get16(p + off + 8); off += 10;
            if (off + rl > (size_t)n) break;
            const uint8_t *rd = p + off;
            if (type == 16 && ends_with_service(nm)) {
                for (unsigned o = 0; o < rl; ) {
                    unsigned l = rd[o++]; if (o + l > rl) break;
                    char kv[256]; snprintf(kv, sizeof kv, "%.*s", (int)l, rd + o); o += l;
                    if (!strncmp(kv, "id=", 3)) snprintf(id, sizeof id, "%s", kv + 3);
                    else if (!strncmp(kv, "name=", 5)) snprintf(name, sizeof name, "%s", kv + 5);
                    else if (!strncmp(kv, "dp=", 3)) dp = atoi(kv + 3);
                }
            } else if (type == 33 && ends_with_service(nm) && rl > 6) {
                size_t o2 = off + 6; dns_read_name(p, n, &o2, target, sizeof target);
            } else if (type == 1 && rl == 4 && na < 8) {
                memcpy(&aip[na], rd, 4); snprintf(ahost[na], sizeof ahost[na], "%s", nm); na++;
            }
            off += rl;
        }
        (void)host;
        for (int i = 0; i < na; i++) if (!target[0] || !strcasecmp(ahost[i], target)) {
            for (int k = 0; k < nifs; k++) if (ifs[k].v4 && (aip[i] & ifs[k].mask.s_addr) == (ifs[k].addr.s_addr & ifs[k].mask.s_addr)) { ip = aip[i]; break; }
            if (ip) break;
        }
        if (!ip && src.ss_family == AF_INET) ip = ((struct sockaddr_in *)&src)->sin_addr.s_addr;
        if (id[0]) found_add(id, name, ip, dp, M_MDNS);
    next:;
    }
}
static void mdns_open(void) {
    if (mdns_fd >= 0 || !(methods & M_MDNS)) return;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(5353)};
    if (bind(s, (void *)&a, sizeof a)) { logmsg("mDNS: cannot use port 5353: %s", strerror(errno)); close(s); return; }
    int ttl = 255; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    int on = 1; setsockopt(s, IPPROTO_IP, IP_PKTINFO, &on, sizeof on);
    for (int i = 0; i < nifs; i++) if (ifs[i].v4) {
        struct ip_mreqn m = {.imr_ifindex = ifs[i].index}; inet_pton(AF_INET, "224.0.0.251", &m.imr_multiaddr);
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
    }
    mdns_fd = s; mdns_announced = 0; mdns_queries = 0;
}

/* Opens what visible / looking need, closes it when neither is on. */
static void sockets_sync(void) {
    int want = visible_now() || looking_now();
    if (want && disc.s4 < 0) { scan_ifaces(); dsock_open(&disc, PORT_DISC, -1); dsock_join(&disc); }
    if (want) mdns_open();
    if (visible_now() && link_lfd < 0) {
        link_lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); int one = 1;
        setsockopt(link_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT_DISC)};
        if (bind(link_lfd, (void *)&a, sizeof a) || listen(link_lfd, 4)) { logmsg("cannot listen for linking on TCP %d: %s", PORT_DISC, strerror(errno)); close(link_lfd); link_lfd = -1; }
    }
    if (!visible_now() && link_lfd >= 0) { close(link_lfd); link_lfd = -1; }
    if (!want) {
        if (disc.s4 >= 0) close(disc.s4); if (disc.s6 >= 0) close(disc.s6); disc.s4 = disc.s6 = -1;
        if (mdns_fd >= 0) { close(mdns_fd); mdns_fd = -1; }
        sweep.active = 0;
    }
}

/* --- linking frames: u32 length + JSON --- */
static int frame_send(int fd, buf_t *j) { uint8_t h[4]; put32(h, j->n); buf_t f = {0}; bput(&f, h, 4); bput(&f, j->p, j->n); int r = write_all(fd, f.p, f.n); bfree(&f); return r; }
static void session_end(const char *msg, int failed) {
    if (ss.fd >= 0) close(ss.fd);
    ss.fd = -1; bfree(&ss.in);
    sodium_memzero(ss.sk, 32); sodium_memzero(ss.key, 32);
    ss.stage = failed ? S_FAILED : S_DONE; snprintf(ss.message, sizeof ss.message, "%s", msg ? msg : "");
    ss.shown_until = mono_ms() + 15000;
    logmsg("linking: %s", msg ? msg : "ended");
}
static void session_send(const char *t, const char *extra) {
    buf_t j = {0}; bprintf(&j, "{\"t\": \"%s\"%s%s}", t, extra ? ", " : "", extra ? extra : ""); frame_send(ss.fd, &j); bfree(&j);
}
static void session_derive(const uint8_t *pk_l, const uint8_t *pk_v, const uint8_t *n_l, const uint8_t *n_v, const uint8_t *id_l, const uint8_t *id_v) {
    static const char L1[] = "shenzhen-kvm link v1", L2[] = "shenzhen-kvm code v1", L3[] = "shenzhen-kvm key v1", L4[] = "shenzhen-kvm link id v1";
    sha256_parts(ss.th, 7, L1, strlen(L1), pk_l, (size_t)32, pk_v, (size_t)32, n_l, (size_t)32, n_v, (size_t)32, id_l, (size_t)16, id_v, (size_t)16);
    uint8_t h[32]; sha256_parts(h, 2, L2, strlen(L2), ss.th, (size_t)32);
    unsigned code = (unsigned)(get64(h) % 1000000ULL);
    snprintf(ss.code, sizeof ss.code, "%03u %03u", code / 1000, code % 1000);
    uint8_t shared[32], prk[32], one = 1;
    if (crypto_scalarmult(shared, ss.sk, ss.peer_pk) != 0) { session_end("The link could not be made. Try again.", 1); return; }
    hmac256(prk, ss.th, 32, shared, 32, NULL, 0);
    hmac256(ss.key, prk, 32, L3, strlen(L3), &one, 1);
    sha256_parts(h, 2, L4, strlen(L4), ss.th, (size_t)32); memcpy(ss.link_id, h, 8);
    sodium_memzero(shared, 32); sodium_memzero(prk, 32);
    ss.stage = S_CONFIRM; ss.deadline = mono_ms() + 180000;
    logmsg("linking with %s: code %s", ss.peer_name, ss.code);
}
static void session_maybe_done(void) {
    if (ss.stage != S_CONFIRM || !ss.me_ok || !ss.peer_ok) return;
    if (save_link(ss.peer_id, ss.peer_name, ss.key, ss.link_id, ss.peer_dp)) { session_end("The link could not be saved.", 1); return; }
    visible_until = 0;
    char msg[300]; snprintf(msg, sizeof msg, "Linked with %s.", ss.peer_name);
    session_end(msg, 0);
    /* main sees the new link and restarts the service with it */
}
static void session_match(void) {
    static const char M[] = "shenzhen-kvm match v1"; uint8_t r = ss.role, p[32]; char hx[65];
    hmac256(p, ss.key, 32, M, strlen(M), &r, 1); tohex(p, 32, hx);
    char extra[100]; snprintf(extra, sizeof extra, "\"proof\": \"%s\"", hx); session_send("match", extra);
    ss.me_ok = 1; session_maybe_done();
}
static void session_check_match(const char *j) {
    static const char M[] = "shenzhen-kvm match v1"; uint8_t r = ss.role == 'L' ? 'V' : 'L', p[32], got[32]; char hx[80];
    if (!json_str(j, "proof", hx, sizeof hx) || fromhex(hx, got, 32)) { session_end("The link could not be made. Try again.", 1); return; }
    hmac256(p, ss.key, 32, M, strlen(M), &r, 1);
    if (sodium_memcmp(p, got, 32)) { session_end("The codes did not match, so the computers were not linked.", 1); return; }
    ss.peer_ok = 1; session_maybe_done();
}
static void session_frame(const char *j) {
    char t[16] = "", v[200];
    json_str(j, "t", t, sizeof t);
    if (!strcmp(t, "cancel")) { char m[300]; snprintf(m, sizeof m, "%s did not link.", ss.peer_name[0] ? ss.peer_name : "The other computer"); session_end(m, 1); return; }
    if (!strcmp(t, "busy")) { char m[300]; snprintf(m, sizeof m, "%s is busy with another computer. Try again in a moment.", ss.peer_name); session_end(m, 1); return; }
    if (ss.stage == S_WAIT_REQUEST && !strcmp(t, "request")) {
        if (!json_str(j, "id", v, sizeof v) || fromhex(v, ss.peer_id, 16) || !json_str(j, "pub", v, sizeof v) || fromhex(v, ss.peer_pk, 32) ||
            !json_str(j, "commit", v, sizeof v) || fromhex(v, ss.commit, 32)) { session_end(NULL, 1); return; }
        tohex(ss.peer_id, 16, ss.peer_id_hex);
        if (!json_str(j, "name", ss.peer_name, sizeof ss.peer_name) || !ss.peer_name[0]) snprintf(ss.peer_name, sizeof ss.peer_name, "A computer");
        ss.peer_dp = (int)json_long(j, "dp", 24800);
        randombytes_buf(ss.sk, 32); crypto_scalarmult_base(ss.pk, ss.sk); randombytes_buf(ss.n_me, 32);
        char a[65], b[65], extra[600]; tohex(ss.pk, 32, a); tohex(ss.n_me, 32, b);
        buf_t e = {0}; bprintf(&e, "\"id\": \"%s\", \"name\": ", my_id_hex); json_put_str(&e, my_name); bprintf(&e, ", \"dp\": %d, \"pub\": \"%s\", \"nonce\": \"%s\"", PORT_UDP, a, b); bz(&e);
        snprintf(extra, sizeof extra, "%s", (char *)e.p); bfree(&e);
        session_send("hello", extra); ss.stage = S_WAIT_REVEAL;
        logmsg("linking: %s asks to link", ss.peer_name);
    } else if (ss.stage == S_WAIT_HELLO && !strcmp(t, "hello")) {
        uint8_t pid[16];
        if (!json_str(j, "id", v, sizeof v) || fromhex(v, pid, 16) || memcmp(pid, ss.peer_id, 16) || !json_str(j, "pub", v, sizeof v) || fromhex(v, ss.peer_pk, 32) ||
            !json_str(j, "nonce", v, sizeof v) || fromhex(v, ss.n_peer, 32)) { session_end("The link could not be made. Try again.", 1); return; }
        json_str(j, "name", ss.peer_name, sizeof ss.peer_name);
        ss.peer_dp = (int)json_long(j, "dp", ss.peer_dp);
        char nh[65], extra[100]; tohex(ss.n_me, 32, nh); snprintf(extra, sizeof extra, "\"nonce\": \"%s\"", nh); session_send("reveal", extra);
        session_derive(ss.pk, ss.peer_pk, ss.n_me, ss.n_peer, my_id, ss.peer_id);
    } else if (ss.stage == S_WAIT_REVEAL && !strcmp(t, "reveal")) {
        static const char C[] = "shenzhen-kvm commit v1"; uint8_t c[32];
        if (!json_str(j, "nonce", v, sizeof v) || fromhex(v, ss.n_peer, 32)) { session_end(NULL, 1); return; }
        sha256_parts(c, 3, C, strlen(C), ss.peer_pk, (size_t)32, ss.n_peer, (size_t)32);
        if (sodium_memcmp(c, ss.commit, 32)) { session_end("The link could not be made. Try again.", 1); return; }
        session_derive(ss.peer_pk, ss.pk, ss.n_peer, ss.n_me, ss.peer_id, my_id);
    } else if (ss.stage == S_CONFIRM && !strcmp(t, "match")) session_check_match(j);
    else session_end("The link could not be made. Try again.", 1);
}
static void session_read(void) {
    uint8_t b[4096]; ssize_t n = read(ss.fd, b, sizeof b);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (ss.stage == S_CONFIRM && ss.me_ok && ss.peer_ok) return;
        char m[300]; snprintf(m, sizeof m, "%s stopped answering, so the computers were not linked.", ss.peer_name[0] ? ss.peer_name : "The other computer");
        session_end(m, 1); return;
    }
    bput(&ss.in, b, n);
    while (ss.fd >= 0 && ss.in.n >= 4) {
        uint32_t l = get32(ss.in.p); if (l > 8192) { session_end(NULL, 1); return; }
        if (ss.in.n < 4 + l) break;
        char j[8200]; memcpy(j, ss.in.p + 4, l); j[l] = 0;
        memmove(ss.in.p, ss.in.p + 4 + l, ss.in.n - 4 - l); ss.in.n -= 4 + l;
        session_frame(j);
    }
}
static void session_connected(void) {
    int err = 0; socklen_t el = sizeof err; getsockopt(ss.fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { char m[300]; snprintf(m, sizeof m, "%s could not be reached. Check that it is still visible.", ss.peer_name); session_end(m, 1); return; }
    static const char C[] = "shenzhen-kvm commit v1";
    randombytes_buf(ss.sk, 32); crypto_scalarmult_base(ss.pk, ss.sk); randombytes_buf(ss.n_me, 32);
    sha256_parts(ss.commit, 3, C, strlen(C), ss.pk, (size_t)32, ss.n_me, (size_t)32);
    char a[65], c[65]; tohex(ss.pk, 32, a); tohex(ss.commit, 32, c);
    buf_t e = {0}; bprintf(&e, "\"id\": \"%s\", \"name\": ", my_id_hex); json_put_str(&e, my_name); bprintf(&e, ", \"dp\": %d, \"pub\": \"%s\", \"commit\": \"%s\"", PORT_UDP, a, c); bz(&e);
    session_send("request", (char *)e.p); bfree(&e);
    ss.stage = S_WAIT_HELLO;
}
static void session_start_link(const char *id) {
    if (ss.stage >= S_CONNECTING && ss.stage <= S_CONFIRM) return;
    int i; for (i = 0; i < NFOUND && strcmp(found[i].id, id); i++) ;
    if (i == NFOUND) { snprintf(ss.message, sizeof ss.message, "That computer is no longer visible."); ss.stage = S_FAILED; ss.shown_until = mono_ms() + 15000; return; }
    memset(&ss, 0, sizeof ss); ss.fd = -1; ss.role = 'L';
    fromhex(found[i].id, ss.peer_id, 16); snprintf(ss.peer_id_hex, sizeof ss.peer_id_hex, "%s", found[i].id);
    snprintf(ss.peer_name, sizeof ss.peer_name, "%s", found[i].name); ss.peer_dp = found[i].dp;
    ss.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(found[i].dp + 2)}; a.sin_addr.s_addr = found[i].ip;
    if (connect(ss.fd, (void *)&a, sizeof a) && errno != EINPROGRESS) { session_end("That computer could not be reached.", 1); return; }
    ss.stage = S_CONNECTING; ss.deadline = mono_ms() + 180000;
    logmsg("linking: asking %s", ss.peer_name);
}
static void session_accept(void) {
    int fd = accept4(link_lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK); if (fd < 0) return;
    if (ss.stage >= S_CONNECTING && ss.stage <= S_CONFIRM) { buf_t j = {0}; bputs(&j, "{\"t\": \"busy\"}"); frame_send(fd, &j); bfree(&j); close(fd); return; }
    memset(&ss, 0, sizeof ss); ss.fd = fd; ss.role = 'V'; ss.stage = S_WAIT_REQUEST; ss.deadline = mono_ms() + 180000;
}
static void session_answer(int yes) {
    if (ss.stage != S_CONFIRM || ss.me_ok) return;
    if (!yes) { session_send("cancel", NULL); session_end("You didn't link the computers.", 1); return; }
    session_match();
}

/* --- control socket --- */
static int owns_desktop(uid_t want) {   /* runs an X server (the desktop's user) */
    DIR *d = opendir("/proc"); struct dirent *e; int yes = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[64], c[256]; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        int fd = open(p, O_RDONLY); if (fd < 0) continue; ssize_t n = read(fd, c, sizeof c - 1); close(fd); if (n <= 0) continue; c[n] = 0;
        const char *b = strrchr(c, '/'); b = b ? b + 1 : c;
        if (strcmp(b, "Xwayland") && strcmp(b, "Xorg") && strcmp(b, "X") && strcmp(b, "Xvfb")) continue;
        struct stat ps; snprintf(p, sizeof p, "/proc/%s", e->d_name); if (stat(p, &ps) == 0 && ps.st_uid == want) { yes = 1; break; }
    }
    closedir(d); return yes;
}
static void client_close(struct client *c) { close(c->fd); c->fd = 0; c->sub = c->looking = c->want_status = 0; bfree(&c->in); }
static void client_cmd(struct client *c, const char *j) {
    char cmd[32] = "", v[64]; json_str(j, "cmd", cmd, sizeof cmd);
    if (!strcmp(cmd, "subscribe")) { c->sub = 1; push_state(1); }
    else if (!strcmp(cmd, "status")) { c->want_status = 1; hsend(afd, 'q', NULL, 0); }
    else if (!strcmp(cmd, "visible")) {
        int on = json_bool(j, "on", 1);
        visible_until = on ? mono_ms() + VISIBLE_SECONDS * 1000 : 0; next_announce_ms = mono_ms(); mdns_announced = 0;
        if (on) { sockets_sync(); logmsg("visible for %d minutes", VISIBLE_SECONDS / 60); sweep_start(); next_sweep_look_ms = mono_ms() + 30000; }
        else { if (mdns_fd >= 0) mdns_answer(1, NULL, 0, 0, 0); if (ss.role == 'V' && ss.stage > S_NONE && ss.stage < S_DONE) session_end("You stopped being visible.", 1); }
    }
    else if (!strcmp(cmd, "look")) {
        c->looking = json_bool(j, "on", 1);
        if (c->looking) { memset(found, 0, sizeof found); sockets_sync(); next_look_ms = mono_ms(); next_mdns_ms = mono_ms(); mdns_queries = 0; next_sweep_look_ms = mono_ms() + 500; }
    }
    else if (!strcmp(cmd, "link") && json_str(j, "id", v, sizeof v)) session_start_link(v);
    else if (!strcmp(cmd, "answer")) session_answer(json_bool(j, "yes", 0));
    else if (!strcmp(cmd, "cancel")) { if (ss.stage > S_NONE && ss.stage < S_DONE) { if (ss.fd >= 0) session_send("cancel", NULL); session_end("You didn't link the computers.", 1); } else ss.stage = S_NONE; }
    else if (!strcmp(cmd, "forget")) hsend(afd, 'F', NULL, 0);
    else if (!strcmp(cmd, "bring_back")) hsend(afd, 'B', NULL, 0);
    else if (!strcmp(cmd, "settings")) hsend_str(afd, 'G', j);
    sockets_sync(); push_state(0);
}
static void client_read(struct client *c) {
    char b[4096]; ssize_t n = read(c->fd, b, sizeof b);
    if (n <= 0) { client_close(c); sockets_sync(); return; }
    bput(&c->in, b, n);
    if (c->in.n > 65536) { client_close(c); return; }
    for (;;) {
        uint8_t *nl = memchr(c->in.p, '\n', c->in.n); if (!nl) break;
        *nl = 0; client_cmd(c, (char *)c->in.p);
        size_t used = nl - c->in.p + 1; if (!c->fd) return;
        memmove(c->in.p, c->in.p + used, c->in.n - used); c->in.n -= used;
    }
}
static void control_accept(void) {
    int fd = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC); if (fd < 0) return;
    struct ucred cr; socklen_t l = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) || (cr.uid != 0 && !owns_desktop(cr.uid))) {
        const char *m = "{\"type\": \"error\", \"message\": \"Only the person using this computer's desktop can change Shenzhen KVM.\"}\n";
        write_all(fd, m, strlen(m)); close(fd); return;
    }
    for (int i = 0; i < MAXCL; i++) if (cl[i].fd <= 0) { memset(&cl[i], 0, sizeof cl[i]); cl[i].fd = fd; return; }
    close(fd);
}

static void agent_timers(void) {
    int64_t now = mono_ms();
    if (visible_until && now >= visible_until) {
        visible_until = 0; logmsg("no longer visible");
        if (mdns_fd >= 0) mdns_answer(1, NULL, 0, 0, 0);
        if (ss.role == 'V' && ss.stage > S_NONE && ss.stage < S_CONFIRM) session_end(NULL, 1);
        sockets_sync();
    }
    if (ss.stage > S_NONE && ss.stage < S_DONE && now >= ss.deadline) session_end("It took too long, so the computers were not linked.", 1);
    if ((ss.stage == S_DONE || ss.stage == S_FAILED) && now >= ss.shown_until) { ss.stage = S_NONE; ss.message[0] = 0; }
    int vis = visible_now(), look = looking_now();
    if ((vis || look) && disc.s4 >= 0) {
        static int64_t next_scan; if (now >= next_scan) { next_scan = now + 10000; if (scan_ifaces()) dsock_join(&disc); }
    }
    if (vis && now >= next_announce_ms) { int t = 0x02; dsock_send_by(&disc, PORT_DISC, M_ALL, make_disc, &t); next_announce_ms = now + 2000; }
    if (vis && mdns_fd >= 0 && mdns_announced < 3 && now >= next_mdns_ms) { mdns_answer(0, NULL, 0, 0, 0); mdns_announced++; next_mdns_ms = now + 1000 * mdns_announced; }
    if (look && now >= next_look_ms) { int t = 0x01; dsock_send_by(&disc, PORT_DISC, M_ALL, make_disc, &t); next_look_ms = now + 2000; }
    if (look && mdns_fd >= 0 && now >= next_mdns_ms) { mdns_query(); next_mdns_ms = now + (mdns_queries < 4 ? 2000 : 10000); }
    if ((vis || look) && now >= next_sweep_look_ms && !sweep.active) { sweep_start(); next_sweep_look_ms = now + 30000; }
    if ((vis || look) && sweep.active) { int t = look ? 0x01 : 0x02; sweep_step(&disc, PORT_DISC, 10, make_disc, &t); }
}

static void agent_main(int fd) {
    afd = fd; who_am_i = "agent";
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
    mkdirs(RUN_DIR, 0755);
    unlink(CONTROL_SOCK);
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un su = {.sun_family = AF_UNIX}; snprintf(su.sun_path, sizeof su.sun_path, "%s", CONTROL_SOCK);
    if (bind(ctl_fd, (void *)&su, sizeof su) || listen(ctl_fd, 8)) { logmsg("cannot open %s: %s", CONTROL_SOCK, strerror(errno)); _exit(4); }
    chmod(CONTROL_SOCK, 0666);   /* who may use it is checked per connection */
    scan_ifaces();
    buf_t b = {0};
    for (;;) {
        struct pollfd q[8 + MAXCL]; int n = 0, cmap[MAXCL];
        q[n++] = (struct pollfd){afd, POLLIN, 0};
        q[n++] = (struct pollfd){ctl_fd, POLLIN, 0};
        q[n++] = (struct pollfd){disc.s4, POLLIN, 0};
        q[n++] = (struct pollfd){disc.s6, POLLIN, 0};
        q[n++] = (struct pollfd){mdns_fd, POLLIN, 0};
        q[n++] = (struct pollfd){link_lfd, POLLIN, 0};
        q[n++] = (struct pollfd){ss.fd, ss.stage == S_CONNECTING ? POLLOUT : POLLIN, 0};
        int base = n;
        for (int i = 0; i < MAXCL; i++) if (cl[i].fd > 0) { cmap[n - base] = i; q[n++] = (struct pollfd){cl[i].fd, POLLIN, 0}; }
        int busy = visible_now() || looking_now() || sweep.active || (ss.stage != S_NONE);
        if (poll(q, n, sweep.active ? 50 : busy ? 250 : 1000) < 0 && errno != EINTR) _exit(5);
        if (q[0].revents) {
            uint8_t t; if (hrecv(afd, &t, &b)) _exit(0);
            if (t == 'S') { size_t l = b.n < sizeof main_state - 1 ? b.n : sizeof main_state - 1; memcpy(main_state, b.p, l); main_state[l] = 0; }
            else if (t == 'D') {
                size_t l = b.n < sizeof main_diag - 1 ? b.n : sizeof main_diag - 1; memcpy(main_diag, b.p, l); main_diag[l] = 0;
                buf_t o = {0}; full_state(&o); o.n--; bputs(&o, ", \"diagnostics\": "); bputs(&o, main_diag); bputs(&o, "}\n");
                for (int i = 0; i < MAXCL; i++) if (cl[i].fd > 0 && cl[i].want_status) { write_all(cl[i].fd, o.p, o.n); cl[i].want_status = 0; }
                bfree(&o);
            }
            else if (t == 'T' && b.n < 1390) {
                char j[1400]; memcpy(j, b.p, b.n); j[b.n] = 0; int id = (int)json_long(j, "id", 0), slot = -1, oldest = 0;
                for (int i = 0; i < NXFER; i++) if (xfers[i].id == id) slot = i;
                if (slot < 0) { for (int i = 1; i < NXFER; i++) if (xfers[i].id < xfers[oldest].id) oldest = i; slot = oldest; }
                xfers[slot].id = id; snprintf(xfers[slot].json, sizeof xfers[slot].json, "%s", j);
                xfers[slot].ended = strstr(j, "\"state\": \"done\"") || strstr(j, "\"state\": \"failed\"") ? mono_ms() : 0;
            }
        }
        if (q[1].revents & POLLIN) control_accept();
        if (q[2].revents & POLLIN) disc_recv(disc.s4);
        if (q[3].revents & POLLIN) disc_recv(disc.s6);
        if (q[4].revents & POLLIN) mdns_recv();
        if (q[5].revents & POLLIN) session_accept();
        if (ss.fd >= 0 && q[6].revents) { if (ss.stage == S_CONNECTING) session_connected(); else session_read(); }
        for (int i = base; i < n; i++) if (q[i].revents && cl[cmap[i - base]].fd > 0) client_read(&cl[cmap[i - base]]);
        agent_timers();
        push_state(0);
    }
}
