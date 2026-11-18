/* ======================================================================================
 * COMMAND LINE: talks to the running service over its control socket
 * ====================================================================================== */
static int ctl_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un su = {.sun_family = AF_UNIX}; snprintf(su.sun_path, sizeof su.sun_path, "%s", CONTROL_SOCK);
    if (connect(fd, (void *)&su, sizeof su)) {
        fprintf(stderr, "Shenzhen KVM is not running on this computer (start it with: sudo systemctl start shenzhen-kvm).\n");
        exit(1);
    }
    return fd;
}
static void ctl_send(int fd, const char *s) { write_all(fd, s, strlen(s)); write_all(fd, "\n", 1); }
/* Next line from the service (blocking, or until stdin has input when watch_stdin). Returns 1 = line, 2 = stdin ready, 0 = closed. */
static int ctl_line(int fd, buf_t *in, char *out, size_t on, int watch_stdin) {
    for (;;) {
        uint8_t *nl = in->n ? memchr(in->p, '\n', in->n) : NULL;
        if (nl) {
            size_t l = nl - in->p; if (l >= on) l = on - 1;
            memcpy(out, in->p, l); out[l] = 0;
            size_t used = nl - in->p + 1; memmove(in->p, in->p + used, in->n - used); in->n -= used;
            return 1;
        }
        struct pollfd q[2] = {{fd, POLLIN, 0}, {watch_stdin ? 0 : -1, POLLIN, 0}};
        if (poll(q, 2, -1) < 0) { if (errno == EINTR) continue; return 0; }
        if (q[1].revents) return 2;
        char b[8192]; ssize_t n = read(fd, b, sizeof b); if (n <= 0) return 0;
        bput(in, b, n);
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
static const char *method_words(const char *obj) {
    static char w[200]; w[0] = 0;
    static const char *names[] = {"mdns", "the network's name service", "broadcast", "a broadcast", "multicast", "a multicast", "ipv6", "IPv6", "sweep", "asking each address"};
    const char *p = json_find(obj, "by"); if (!p) return "";
    for (int i = 0; i < 10; i += 2) {
        char pat[20]; snprintf(pat, sizeof pat, "\"%s\"", names[i]);
        const char *e = strchr(p, ']'); const char *f = strstr(p, pat);
        if (f && (!e || f < e)) { if (w[0]) strncat(w, ", ", sizeof w - strlen(w) - 1); strncat(w, names[i + 1], sizeof w - strlen(w) - 1); }
    }
    return w;
}
static int ask_yes(void) {
    char a[16] = ""; if (!fgets(a, sizeof a, stdin)) return 0;
    return a[0] == 'y' || a[0] == 'Y';
}
static int fd_global = -1;
static void on_int(int s) { (void)s; if (fd_global >= 0) { ctl_send(fd_global, "{\"cmd\": \"visible\", \"on\": false}"); ctl_send(fd_global, "{\"cmd\": \"cancel\"}"); } _exit(130); }

/* Follows a linking session to its end once the code is known. Returns the exit status. */
/* The service restarts with a new link (its socket closes): ask it again whether it is linked now. */
static int linked_after_restart(const char *peer) {
    for (int i = 0; i < 30; i++) {
        usleep(300000);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un su = {.sun_family = AF_UNIX}; snprintf(su.sun_path, sizeof su.sun_path, "%s", CONTROL_SOCK);
        if (connect(fd, (void *)&su, sizeof su)) { close(fd); continue; }
        ctl_send(fd, "{\"cmd\": \"subscribe\"}");
        buf_t in = {0}; char line[16384], name[128] = "";
        int r = ctl_line(fd, &in, line, sizeof line, 0); close(fd); bfree(&in);
        if (r != 1) continue;
        if (json_bool(line, "linked", 0)) { json_str(line, "peer_name", name, sizeof name); printf("Linked with %s.\n", name[0] ? name : peer); return 0; }
        printf("%s did not link.\n", peer); return 1;
    }
    printf("Shenzhen KVM did not come back; check it with: shenzhen-kvm status\n"); return 1;
}
static int follow_linking(int fd, buf_t *in, int asked) {
    char line[16384], stage[16], code[16], peer[128] = "", msg[300]; int prompted = 0, waiting_said = 0, said_yes = 0;
    for (;;) {
        int r = ctl_line(fd, in, line, sizeof line, 0); if (r != 1) return said_yes ? linked_after_restart(peer) : 1;
        const char *l = json_find(line, "linking"); if (!l) continue;
        stage[0] = code[0] = peer[0] = msg[0] = 0;
        json_str(l, "stage", stage, sizeof stage); json_str(l, "code", code, sizeof code); json_str(l, "peer_name", peer, sizeof peer); json_str(l, "message", msg, sizeof msg);
        if (!strcmp(stage, "confirm") && !prompted) {
            prompted = 1;
            if (asked) printf("\n%s wants to link with this computer.\nCode: %s\nLink only if %s shows the same code. Link? [y/N] ", peer, code, peer);
            else printf("\nDoes %s show the code %s? [y/N] ", peer, code);
            fflush(stdout);
            said_yes = ask_yes();
            ctl_send(fd, said_yes ? "{\"cmd\": \"answer\", \"yes\": true}" : "{\"cmd\": \"answer\", \"yes\": false}");
        } else if (!strcmp(stage, "confirm") && json_bool(l, "you_confirmed", 0) && !json_bool(l, "they_confirmed", 0) && !waiting_said) {
            waiting_said = 1; printf("Waiting for %s to confirm the code there...\n", peer); fflush(stdout);
        } else if (!strcmp(stage, "done")) { printf("%s\n", msg); return 0; }
        else if (!strcmp(stage, "failed")) { printf("%s\n", msg); return 1; }
        else if (!strcmp(stage, "none") && prompted) return 1;
    }
}

static int cmd_visible(void) {
    int fd = ctl_connect(); fd_global = fd; signal(SIGINT, on_int); signal(SIGTERM, on_int);
    ctl_send(fd, "{\"cmd\": \"visible\", \"on\": true}"); ctl_send(fd, "{\"cmd\": \"subscribe\"}");
    buf_t in = {0}; char line[16384], stage[16], role[16];
    printf("This computer (%s) is now visible on this network for %d minutes.\n"
           "On your other computer, open Shenzhen KVM and choose %s. Press Ctrl+C to stop.\n", my_name, VISIBLE_SECONDS / 60, my_name);
    fflush(stdout);
    int seen_visible = 0;
    for (;;) {
        int r = ctl_line(fd, &in, line, sizeof line, 0); if (r != 1) return 1;
        if (strstr(line, "\"type\": \"error\"")) { char m[300] = ""; json_str(line, "message", m, sizeof m); fprintf(stderr, "%s\n", m); return 1; }
        long left = json_long(line, "visible_left", 0);
        if (left > 0) seen_visible = 1;
        const char *l = json_find(line, "linking"); stage[0] = role[0] = 0;
        if (l) { json_str(l, "stage", stage, sizeof stage); json_str(l, "role", role, sizeof role); }
        if (!strcmp(stage, "confirm") && !strcmp(role, "asked")) {
            /* replay this line through the follower */
            buf_t again = {0}; bputs(&again, line); bputs(&again, "\n"); bput(&again, in.p, in.n); bfree(&in);
            int rc = follow_linking(fd, &again, 1); bfree(&again);
            if (rc == 0) { ctl_send(fd, "{\"cmd\": \"visible\", \"on\": false}"); return 0; }
            printf("Still visible. Press Ctrl+C to stop.\n"); fflush(stdout);
            in = (buf_t){0};
            continue;
        }
        if (seen_visible && left == 0 && strcmp(stage, "waiting")) { printf("This computer is no longer visible; no computer linked.\n"); return 1; }
    }
}

static int cmd_link(const char *want) {
    int fd = ctl_connect(); fd_global = fd; signal(SIGINT, on_int); signal(SIGTERM, on_int);
    ctl_send(fd, "{\"cmd\": \"look\", \"on\": true}"); ctl_send(fd, "{\"cmd\": \"subscribe\"}");
    printf("Looking for visible computers on this network");
    if (!want) printf(" (on the other computer, choose \"Make this computer visible\" first).\nType a number and press Return to link, or Ctrl+C to stop.\n");
    else printf(" named \"%s\"...\n", want);
    fflush(stdout);
    buf_t in = {0}; char line[16384], shown[4096] = "", ids[16][40]; int nids = 0;
    for (;;) {
        int r = ctl_line(fd, &in, line, sizeof line, want == NULL);
        if (r == 0) return 1;
        if (r == 2) {   /* a number typed */
            char a[32]; if (!fgets(a, sizeof a, stdin)) return 1;
            int k = atoi(a);
            if (k >= 1 && k <= nids) {
                char c[100]; snprintf(c, sizeof c, "{\"cmd\": \"link\", \"id\": \"%s\"}", ids[k - 1]); ctl_send(fd, c);
                printf("Asking... choose Link on the other computer when it asks.\n"); fflush(stdout);
                int rc = follow_linking(fd, &in, 0); ctl_send(fd, "{\"cmd\": \"look\", \"on\": false}"); return rc;
            }
            printf("Type one of the numbers above.\n"); fflush(stdout); continue;
        }
        if (strstr(line, "\"type\": \"error\"")) { char m[300] = ""; json_str(line, "message", m, sizeof m); fprintf(stderr, "%s\n", m); return 1; }
        const char *f = json_find(line, "found"); if (!f) continue;
        char list[4096] = "", obj[1024], name[128], id[40]; int n = 0; const char *p = f;
        while (n < 16 && (p = json_next_obj(p, obj, sizeof obj))) {
            json_str(obj, "name", name, sizeof name); json_str(obj, "id", id, sizeof id);
            snprintf(ids[n], sizeof ids[n], "%s", id); n++;
            char row[400]; snprintf(row, sizeof row, "  %d) %s  (found by %s)\n", n, name, method_words(obj)); strncat(list, row, sizeof list - strlen(list) - 1);
            if (want && strcasestr(name, want)) {
                char c[100]; snprintf(c, sizeof c, "{\"cmd\": \"link\", \"id\": \"%s\"}", id); ctl_send(fd, c);
                printf("Found %s. Asking... choose Link on %s when it asks.\n", name, name); fflush(stdout);
                int rc = follow_linking(fd, &in, 0); ctl_send(fd, "{\"cmd\": \"look\", \"on\": false}"); return rc;
            }
        }
        nids = n;
        if (!want && strcmp(list, shown)) { snprintf(shown, sizeof shown, "%s", list); printf("%s", n ? list : "  (none yet)\n"); fflush(stdout); }
    }
}

static int cmd_forget(int yes) {
    int fd = ctl_connect(); ctl_send(fd, "{\"cmd\": \"subscribe\"}");
    buf_t in = {0}; char line[16384], peer[128] = "";
    if (ctl_line(fd, &in, line, sizeof line, 0) != 1) return 1;
    if (!json_bool(line, "linked", 0)) { printf("This computer is not linked with another one.\n"); return 0; }
    json_str(line, "peer_name", peer, sizeof peer);
    if (!yes) {
        printf("Forget %s? Its keyboard, mouse and clipboard stop working with this computer until you link them again. [y/N] ", peer); fflush(stdout);
        if (!ask_yes()) return 1;
    }
    ctl_send(fd, "{\"cmd\": \"forget\"}");
    /* the service restarts without the link; its control socket closes */
    for (int i = 0; i < 50; i++) { struct stat s; if (stat(LINK_FILE, &s) && errno == ENOENT) { printf("Forgot %s.\n", peer); return 0; } if (ctl_line(fd, &in, line, sizeof line, 0) != 1) usleep(100000); }
    printf("Forgot %s.\n", peer); return 0;
}

static int cmd_status(void) {
    int fd = ctl_connect(); ctl_send(fd, "{\"cmd\": \"status\"}");
    buf_t in = {0}; char line[32768];
    if (ctl_line(fd, &in, line, sizeof line, 0) != 1) return 1;
    printf("%s\n", line); return 0;
}

static int cmd_send(const char *json) { int fd = ctl_connect(); ctl_send(fd, json); usleep(200000); return 0; }

int main(int argc, char **argv) {
    init_paths(); signal(SIGPIPE, SIG_IGN);
    if (sodium_init() < 0) return 1;
    if (argc >= 2 && !strcmp(argv[1], "run")) return run();
    load_identity_readonly();
    if (argc == 2 && !strcmp(argv[1], "status")) return cmd_status();
    if (argc == 2 && !strcmp(argv[1], "visible")) return cmd_visible();
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "link")) return cmd_link(argc == 3 ? argv[2] : NULL);
    if (argc >= 2 && !strcmp(argv[1], "forget")) return cmd_forget(argc == 3 && !strcmp(argv[2], "--yes"));
    if (argc == 2 && !strcmp(argv[1], "bring-back")) return cmd_send("{\"cmd\": \"bring_back\"}");
    if (argc == 3 && !strcmp(argv[1], "settings")) { char b[2048]; snprintf(b, sizeof b, "{\"cmd\": \"settings\", %s}", argv[2][0] == '{' ? argv[2] + 1 : argv[2]); size_t l = strlen(b); if (l > 2 && b[l - 2] == '}' && b[l - 1] == '}') b[l - 1] = 0; return cmd_send(b); }
    fprintf(stderr, "usage: shenzhen-kvm run | status | visible | link [NAME] | forget [--yes] | bring-back | settings '{\"sharing\": false}'\n");
    return 2;
}
