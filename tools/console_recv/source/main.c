// MPayne2-nx-recv: a file receiver for this port's own folder on the console's SD card (2026-11-22).
//
// Why: sys-ftpd-light (TCP 5000, how tools/switch_remote.py reaches the card) refused every connection on the morning
// of 2026-11-22 while sys-botbase answered. This homebrew does what switch_remote.py needs from the card, and nothing
// more, while it is open in the foreground: it listens on TCP 5005 (the PC connects to it; nothing on the PC listens)
// and reads and writes files ONLY under sdmc:/switch/MPayne2-nx. Every path is checked: relative, no "..", no ':' or '\\',
// no leading '/'. There is no delete, rename or truncate-in-place command.
//
// Protocol: one command line, '\n' ended, then any body; each reply starts with "OK" or "ERR <why>" and a '\n'.
//   PUT <size> <path>   then <size> bytes: the file written (parent folders made inside the root); reply OK <size>
//   GET <path>          reply OK <size>, then the bytes
//   LIST [<path>]       reply OK <n>, then n lines "d|f <size> <name>"
//   MKDIR <path>        a folder (one already there is kept)
//   SIZE <path>         reply OK <size> (or ERR missing)
//   QUIT                reply OK, and the app closes (back to hbmenu)
// PLUS on the console's controller closes it too.
//
// One exception to the folder rule (2026-11-23, the console owner: "Can you please do that for me", the Horizon-OC
// clock profile for this port's title, after "wherever you need access to make those changes in the ini file, let me
// know and I can give permission"): Horizon-OC's own config, sdmc:/config/horizon-oc/config.ini, and only these:
//   HOC_LIST            the listing of sdmc:/config/horizon-oc (read only)
//   CFG_LIST [<name>]   the listing of sdmc:/config or of one folder directly in it (read only: names and sizes)
//   SVC_HAS             whether Horizon-OC's (hoc:clk) and sys-clk's (sys:clk) services are registered (a question to sm)
//   HOC_GET             config.ini, replied as GET
//   HOC_INIT            config.ini made from the folder's own config.ini.template (byte for byte), only if missing
//   HOC_APPEND <size> <now>   then <size> bytes (at most 512) added at the END of config.ini, only when the file is
//                       now exactly <now> bytes (unchanged since it was read), has no section for hbmenu's title
//                       010091D01597C000 yet, and the body is printable ASCII lines holding that one section's header
//                       and key=value lines; the file as it was is first copied to sdmc:/switch/MPayne2-nx/hoc_config_before.ini.
//                       Nothing in config.ini is removed or rewritten; reply OK <new size>.
// No other path outside the port's folder is reachable, and there is still no delete, rename or truncate.
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#define ROOT "sdmc:/switch/MPayne2-nx"
#define PORT 5005
#define BUF (1 << 20)

static char g_buf[BUF];
static int g_quit = 0;
static PadState g_pad;

static void show(void) { consoleUpdate(NULL); }

static int path_ok(const char* p) {
    if (!p || !*p || p[0] == '/' || strlen(p) > 400) return 0;
    if (strstr(p, "..") || strchr(p, ':') || strchr(p, '\\')) return 0;
    for (const char* c = p; *c; ++c)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) return 0;
    return 1;
}

static void full_path(char* out, size_t n, const char* rel) {
    if (rel && *rel)
        snprintf(out, n, "%s/%s", ROOT, rel);
    else
        snprintf(out, n, "%s", ROOT);
}

// parent folders of a path inside the root (the root itself first)
static void make_parents(const char* full) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", full);
    size_t root_len = strlen(ROOT);
    for (char* c = tmp + root_len + 1; *c; ++c) {
        if (*c == '/') {
            *c = 0;
            mkdir(tmp, 0777);
            *c = '/';
        }
    }
}

static int send_all(int s, const void* data, size_t n) {
    const char* p = (const char*)data;
    while (n) {
        ssize_t k = send(s, p, n, 0);
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int reply(int s, const char* fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);
    return send_all(s, line, strlen(line));
}

// one '\n'-ended line (without it); -1 when the connection closed
static int read_line(int s, char* line, size_t n) {
    size_t got = 0;
    while (got + 1 < n) {
        char c;
        ssize_t k = recv(s, &c, 1, 0);
        if (k <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') line[got++] = c;
    }
    line[got] = 0;
    return (int)got;
}

static void do_put(int s, char* args) {
    char* sp = strchr(args, ' ');
    if (!sp) {
        reply(s, "ERR usage");
        return;
    }
    *sp = 0;
    long long size = atoll(args);
    const char* rel = sp + 1;
    char full[512];
    int ok = path_ok(rel) && size >= 0;
    FILE* f = NULL;
    if (ok) {
        full_path(full, sizeof(full), rel);
        make_parents(full);
        f = fopen(full, "wb");
    }
    // the body is read in full either way, so the connection stays in step
    long long left = size, written = 0;
    int write_failed = 0;
    while (left > 0) {
        ssize_t k = recv(s, g_buf, left < BUF ? (size_t)left : BUF, 0);
        if (k <= 0) {
            if (f) fclose(f);
            printf("  connection lost in %s\n", rel);
            show();
            return;
        }
        left -= k;
        if (f && !write_failed) {
            if (fwrite(g_buf, 1, (size_t)k, f) != (size_t)k)
                write_failed = 1;
            else
                written += k;
        }
    }
    if (f) {
        if (fclose(f) != 0) write_failed = 1;
    }
    if (!ok) {
        reply(s, "ERR refused path");
        printf("  refused: %s\n", rel);
    } else if (!f) {
        reply(s, "ERR open %d", errno);
        printf("  could not open %s (%d)\n", rel, errno);
    } else if (write_failed) {
        reply(s, "ERR write");
        printf("  write failed: %s\n", rel);
    } else {
        reply(s, "OK %lld", written);
        printf("  %s  %lld bytes\n", rel, written);
    }
    show();
}

static void do_get(int s, const char* rel) {
    char full[512];
    if (!path_ok(rel)) {
        reply(s, "ERR refused path");
        return;
    }
    full_path(full, sizeof(full), rel);
    FILE* f = fopen(full, "rb");
    if (!f) {
        reply(s, "ERR missing");
        return;
    }
    struct stat st;
    if (stat(full, &st) != 0) {
        fclose(f);
        reply(s, "ERR stat");
        return;
    }
    reply(s, "OK %lld", (long long)st.st_size);
    long long left = st.st_size;
    while (left > 0) {
        size_t k = fread(g_buf, 1, left < BUF ? (size_t)left : BUF, f);
        if (k == 0) break;
        if (send_all(s, g_buf, k) != 0) break;
        left -= (long long)k;
    }
    fclose(f);
    printf("  sent %s\n", rel);
    show();
}

static void do_list(int s, const char* rel) {
    char full[512];
    if (rel && *rel && !path_ok(rel)) {
        reply(s, "ERR refused path");
        return;
    }
    full_path(full, sizeof(full), rel);
    DIR* d = opendir(full);
    if (!d) {
        reply(s, "ERR missing");
        return;
    }
    // one pass into a buffer, then the count and the lines (rewinddir does not rewind on the console's SD: the first
    // version sent the count and then no lines, 2026-11-22)
    const size_t cap = 256 * 1024;
    char* out = (char*)malloc(cap);
    size_t len = 0;
    int n = 0;
    struct dirent* e;
    while (out && (e = readdir(d)) != NULL && len + 600 < cap) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char item[1024];
        snprintf(item, sizeof(item), "%s/%s", full, e->d_name);
        struct stat st;
        long long size = 0;
        int dir = 0;
        if (stat(item, &st) == 0) {
            dir = S_ISDIR(st.st_mode);
            size = (long long)st.st_size;
        }
        len += (size_t)snprintf(out + len, cap - len, "%c %lld %s\n", dir ? 'd' : 'f', size, e->d_name);
        ++n;
    }
    closedir(d);
    reply(s, "OK %d", n);
    if (out) {
        send_all(s, out, len);
        free(out);
    }
}

static void do_size(int s, const char* rel) {
    char full[512];
    struct stat st;
    if (!path_ok(rel)) {
        reply(s, "ERR refused path");
        return;
    }
    full_path(full, sizeof(full), rel);
    if (stat(full, &st) != 0) {
        reply(s, "ERR missing");
        return;
    }
    reply(s, "OK %lld", (long long)st.st_size);
}

static void do_mkdir(int s, const char* rel) {
    char full[512];
    if (!path_ok(rel)) {
        reply(s, "ERR refused path");
        return;
    }
    full_path(full, sizeof(full), rel);
    make_parents(full);
    mkdir(full, 0777);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
        reply(s, "OK");
    else
        reply(s, "ERR mkdir %d", errno);
}

// --- Horizon-OC's config.ini (the one exception above) ---
#define HOC_DIR "sdmc:/config/horizon-oc"
#define HOC_INI HOC_DIR "/config.ini"
#define HOC_BACKUP ROOT "/hoc_config_before.ini"
#define HOC_TID "010091D01597C000"

// CFG_LIST [<name>]: names and sizes in sdmc:/config or in one folder directly under it (read only), to find the
// clock manager's own folder (2026-11-23: sdmc:/config/horizon-oc was missing on the console)
static void do_list_dir(int s, const char* dir);
static void do_cfg_list(int s, const char* name) {
    char dir[512];
    if (name && *name) {
        if (!path_ok(name) || strchr(name, '/')) {
            reply(s, "ERR refused path");
            return;
        }
        snprintf(dir, sizeof(dir), "sdmc:/config/%s", name);
    } else
        snprintf(dir, sizeof(dir), "sdmc:/config");
    do_list_dir(s, dir);
}

static void do_hoc_list(int s) { do_list_dir(s, HOC_DIR); }

// SVC_HAS: whether a clock manager's service is registered (Atmosphere's sm extension 65100, HasService: a question to
// sm only, no session opened, nothing changed): Horizon-OC's hoc:clk and sys-clk's sys:clk
static int has_service(const char* name) {
    SmServiceName n = smEncodeName(name);
    u8 out = 0;
    Result rc = hosversionAtLeast(12, 0, 0) ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, n, out)
                                            : serviceDispatchInOut(smGetServiceSession(), 65100, n, out);
    return R_FAILED(rc) ? -1 : out != 0;
}

static void do_svc_has(int s) {
    reply(s, "OK hoc:clk=%d sys:clk=%d", has_service("hoc:clk"), has_service("sys:clk"));
}

static void do_list_dir(int s, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        reply(s, "ERR missing");
        return;
    }
    char out[8192];
    size_t len = 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && len + 600 < sizeof(out)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char item[1024];
        snprintf(item, sizeof(item), "%s/%s", dir, e->d_name);
        struct stat st;
        long long size = 0;
        int dir = 0;
        if (stat(item, &st) == 0) {
            dir = S_ISDIR(st.st_mode);
            size = (long long)st.st_size;
        }
        len += (size_t)snprintf(out + len, sizeof(out) - len, "%c %lld %s\n", dir ? 'd' : 'f', size, e->d_name);
        ++n;
    }
    closedir(d);
    reply(s, "OK %d", n);
    send_all(s, out, len);
}

// the whole of config.ini into g_buf; its size, or -1
static long read_hoc_ini(void) {
    FILE* f = fopen(HOC_INI, "rb");
    if (!f) return -1;
    size_t got = fread(g_buf, 1, BUF - 1, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (more) return -1;
    g_buf[got] = 0;
    return (long)got;
}

static void do_hoc_get(int s) {
    long n = read_hoc_ini();
    if (n < 0) {
        reply(s, "ERR missing");
        return;
    }
    reply(s, "OK %ld", n);
    send_all(s, g_buf, (size_t)n);
    printf("  sent " HOC_INI "\n");
    show();
}

static int has_tid(const char* text) {
    // either case of the title's hex
    const size_t n = strlen(HOC_TID);
    for (const char* p = text; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && toupper((unsigned char)p[i]) == HOC_TID[i]) ++i;
        if (i == n) return 1;
    }
    return 0;
}

// HOC_INIT: config.ini made from Horizon-OC's own config.ini.template in the same folder, byte for byte, only when
// there is no config.ini (2026-11-23: the console owner copied the pack's folder and restarted; Horizon-OC wrote no
// config.ini of its own, like sys-clk, whose users copy the template)
static void do_hoc_init(int s) {
    struct stat st;
    if (stat(HOC_INI, &st) == 0) {
        reply(s, "ERR exists");
        return;
    }
    FILE* t = fopen(HOC_DIR "/config.ini.template", "rb");
    if (!t) {
        reply(s, "ERR no template");
        return;
    }
    size_t n = fread(g_buf, 1, 65536, t);
    int more = fgetc(t) != EOF;
    fclose(t);
    if (more || n == 0) {
        reply(s, "ERR template size");
        return;
    }
    FILE* f = fopen(HOC_INI, "wb");
    if (!f) {
        reply(s, "ERR open %d", errno);
        return;
    }
    int bad = fwrite(g_buf, 1, n, f) != n;
    if (fclose(f) != 0) bad = 1;
    if (bad) {
        reply(s, "ERR write");
        return;
    }
    reply(s, "OK %zu", n);
    printf("  " HOC_INI " made from config.ini.template (%zu bytes)\n", n);
    show();
}

static void do_hoc_append(int s, char* args) {
    long long size = -1, now = -1;
    if (sscanf(args, "%lld %lld", &size, &now) != 2 || size < 0) {
        reply(s, "ERR usage");
        return;
    }
    // the body read in full either way, so the connection stays in step
    char body[513];
    long long left = size, kept = 0;
    while (left > 0) {
        char tmp[512];
        ssize_t k = recv(s, tmp, left < (long long)sizeof(tmp) ? (size_t)left : sizeof(tmp), 0);
        if (k <= 0) {
            printf("  connection lost in HOC_APPEND\n");
            show();
            return;
        }
        for (ssize_t i = 0; i < k; i++)
            if (kept < 512) body[kept++] = tmp[i];
        left -= k;
    }
    body[kept] = 0;
    if (size > 512 || size == 0) {
        reply(s, "ERR size");
        return;
    }
    // printable ASCII lines; exactly one section, this title's, and nothing before it but line ends
    int sections = 0;
    for (long long i = 0; i < kept; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '[') ++sections;
        if (!(c == '\n' || c == '\r' || (c >= 0x20 && c <= 0x7e))) {
            reply(s, "ERR body");
            return;
        }
    }
    const char* first = body;
    while (*first == '\r' || *first == '\n') ++first;
    if (sections != 1 || strncmp(first, "[" HOC_TID "]", strlen(HOC_TID) + 2) != 0) {
        reply(s, "ERR body");
        return;
    }
    long n = read_hoc_ini();
    if (n < 0) {
        reply(s, "ERR missing");
        return;
    }
    if ((long long)n != now) {
        reply(s, "ERR changed %ld", n);
        return;
    }
    if (has_tid(g_buf)) {
        reply(s, "ERR has section");
        return;
    }
    FILE* b = fopen(HOC_BACKUP, "wb");
    if (!b || fwrite(g_buf, 1, (size_t)n, b) != (size_t)n || fclose(b) != 0) {
        reply(s, "ERR backup");
        return;
    }
    FILE* f = fopen(HOC_INI, "ab");
    if (!f) {
        reply(s, "ERR open %d", errno);
        return;
    }
    int bad = fwrite(body, 1, (size_t)kept, f) != (size_t)kept;
    if (fclose(f) != 0) bad = 1;
    struct stat st;
    long long after = stat(HOC_INI, &st) == 0 ? (long long)st.st_size : -1;
    if (bad) {
        reply(s, "ERR write (now %lld bytes)", after);
        return;
    }
    reply(s, "OK %lld", after);
    printf("  " HOC_INI ": %lld bytes added (was %ld, now %lld); the old file is " HOC_BACKUP "\n", kept, n, after);
    show();
}

static void serve(int c) {
    char line[600];
    while (!g_quit && read_line(c, line, sizeof(line)) >= 0) {
        if (!strncmp(line, "PUT ", 4))
            do_put(c, line + 4);
        else if (!strncmp(line, "GET ", 4))
            do_get(c, line + 4);
        else if (!strcmp(line, "LIST"))
            do_list(c, "");
        else if (!strncmp(line, "LIST ", 5))
            do_list(c, line + 5);
        else if (!strncmp(line, "SIZE ", 5))
            do_size(c, line + 5);
        else if (!strncmp(line, "MKDIR ", 6))
            do_mkdir(c, line + 6);
        else if (!strcmp(line, "HOC_LIST"))
            do_hoc_list(c);
        else if (!strcmp(line, "SVC_HAS"))
            do_svc_has(c);
        else if (!strcmp(line, "CFG_LIST"))
            do_cfg_list(c, "");
        else if (!strncmp(line, "CFG_LIST ", 9))
            do_cfg_list(c, line + 9);
        else if (!strcmp(line, "HOC_GET"))
            do_hoc_get(c);
        else if (!strcmp(line, "HOC_INIT"))
            do_hoc_init(c);
        else if (!strncmp(line, "HOC_APPEND ", 11))
            do_hoc_append(c, line + 11);
        else if (!strcmp(line, "QUIT")) {
            reply(c, "OK");
            g_quit = 1;
        } else
            reply(c, "ERR unknown");
    }
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x20000,
        .tcp_rx_buf_size = 0x40000,
        .tcp_tx_buf_max_size = 0x40000,
        .tcp_rx_buf_max_size = 0x100000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 4,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_User,
    };
    Result rc = socketInitialize(&cfg);
    // 2026-11-22: a 10-minute upload with no button pressed let the console sleep, which dropped the connection;
    // auto-sleep stays off while this runs (back on at exit)
    appletSetAutoSleepDisabled(true);
    printf("MPayne2-nx-recv: files for %s only (no delete), and an append-only section in %s. PLUS closes.\n", ROOT,
           HOC_INI);
    printf("started as %s\n", argc > 0 ? argv[0] : "?");
    mkdir(ROOT, 0777);
    if (R_FAILED(rc)) {
        printf("socketInitialize failed: 0x%x\n", rc);
        show();
        while (appletMainLoop()) {
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
            svcSleepThread(50000000);
        }
        consoleExit(NULL);
        return 0;
    }

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
        printf("bind/listen on %d failed: %d\n", PORT, errno);
    } else {
        struct in_addr ip = {(in_addr_t)gethostid()};
        printf("listening on %s:%d\n", inet_ntoa(ip), PORT);
    }
    show();

    while (!g_quit && appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        struct timeval tv = {0, 100000};
        if (select(ls + 1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(ls, &r)) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                printf("connected\n");
                show();
                serve(c);
                close(c);
                printf("disconnected\n");
                show();
            }
        }
    }
    close(ls);
    socketExit();
    appletSetAutoSleepDisabled(false);
    consoleExit(NULL);
    return 0;
}
