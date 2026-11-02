#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <utime.h>

char HOME[4096], CLAUDE[4096], CLAUDE_JSON[4096], STORE_ROOT[4096], STAMP[32];
const char *SKIP_DIRS[] = { "cache", "telemetry", "shell-snapshots", "paste-cache", "downloads", "backups",
                            "ide", "daemon", "sessions", "spares", "jobs", "statsig", "todos", ".DS_Store", NULL };
const char *CURRENT_MODELS[] = { "claude-fable-5-1", "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", NULL };
struct Opts opts;

void common_init(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) { struct passwd *pw = getpwuid(getuid()); h = pw ? pw->pw_dir : "/"; }
    snprintf(HOME, sizeof HOME, "%s", h);
    snprintf(CLAUDE, sizeof CLAUDE, "%s/.claude", HOME);
    snprintf(CLAUDE_JSON, sizeof CLAUDE_JSON, "%s/.claude.json", HOME);
#ifdef __APPLE__
    snprintf(STORE_ROOT, sizeof STORE_ROOT, "%s/Library/Application Support/Claude/claude-code-sessions", HOME);
#else
    snprintf(STORE_ROOT, sizeof STORE_ROOT, "%s/.config/Claude/claude-code-sessions", HOME);
#endif
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    strftime(STAMP, sizeof STAMP, "%Y%m%d-%H%M%S", &tm);
}

/* ------------------------------------------------------------ output ---- */
void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); fflush(stdout); }
void head(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("\n\033[1m== "); vprintf(fmt, ap); printf("\033[0m\n"); va_end(ap); fflush(stdout); }
void die(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fflush(stdout); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); exit(1); }

const char *tilde(const char *path) {
    static char buf[8192];
    size_t hl = strlen(HOME);
    if (path && strncmp(path, HOME, hl) == 0 && (path[hl] == '/' || path[hl] == 0)) { snprintf(buf, sizeof buf, "~%s", path + hl); return buf; }
    return path ? path : "";
}

char *xasprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) s = strdup("");
    va_end(ap);
    return s;
}
char *path_join(const char *a, const char *b) { return xasprintf("%s/%s", a, b); }

/* ----------------------------------------------------------- strings ---- */
bool in_list(const char *s, const char **list) { for (; *list; list++) if (strcmp(s, *list) == 0) return true; return false; }
bool starts_with(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }
bool ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && strcmp(s + a - b, suf) == 0; }

char *str_replace_all(const char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (!fl) return strdup(s);
    Buf b = {0};
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, from);
        if (!q) { buf_adds(&b, p); break; }
        buf_add(&b, p, (size_t)(q - p));
        buf_add(&b, to, tl);
        p = q + fl;
    }
    return b.s ? b.s : strdup("");
}

char *slug(const char *path) {
    char *s = strdup(path);
    for (char *p = s; *p; p++) if (!isalnum((unsigned char)*p)) *p = '-';
    return s;
}

bool is_uuid(const char *s) {
    if (strlen(s) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return false;
    }
    return true;
}

char *new_uuid(void) {
    unsigned char b[16];
    arc4random_buf(b, sizeof b);
    b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
    return xasprintf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

char *title_key(const char *title, const char *source) {
    if (!title || !*title || !source || strcmp(source, "user") != 0) return NULL;
    char *k = malloc(strlen(title) + 1);
    size_t n = 0; bool space = true;
    for (const unsigned char *p = (const unsigned char *)title; *p; p++) {
        if (isspace(*p)) { if (!space && n) k[n++] = ' '; space = true; }
        else { k[n++] = (char)tolower(*p); space = false; }
    }
    while (n && k[n - 1] == ' ') n--;
    k[n] = 0;
    if (!n) { free(k); return NULL; }
    return k;
}

long long iso_ms(const char *iso) {
    struct tm tm = {0}; int ms = 0; char tz[8] = {0};
    int n = sscanf(iso, "%d-%d-%dT%d:%d:%d.%d%7s", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms, tz);
    if (n < 6) return -1;
    if (n == 6) { /* no fraction: try to catch "Z" directly after seconds */ ms = 0; }
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) return -1;
    /* normalise fraction digits (e.g. ".41" vs ".410") */
    const char *dot = strchr(iso, '.');
    if (dot) { int digits = 0; for (const char *d = dot + 1; isdigit((unsigned char)*d); d++) digits++; while (digits > 3) { ms /= 10; digits--; } while (digits && digits < 3) { ms *= 10; digits++; } }
    long long off = 0;
    const char *sign = strpbrk(dot ? dot : iso + 19, "+-");
    if (sign && (sign[0] == '+' || sign[0] == '-') && strlen(sign) >= 6) {
        int hh = atoi(sign + 1), mm = atoi(sign + 4);
        off = (hh * 3600 + mm * 60) * (sign[0] == '+' ? 1 : -1);
    }
    return ((long long)t - off) * 1000 + ms;
}

long long now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }

/* ------------------------------------------------------------- files ---- */
bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
bool is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
bool exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }
bool is_link(const char *p) { struct stat st; return lstat(p, &st) == 0 && S_ISLNK(st.st_mode); }

int mkdir_p(const char *p) {
    char tmp[8192];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *c = tmp + 1; *c; c++)
        if (*c == '/') { *c = 0; if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1; *c = '/'; }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int copy_file(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) { close(in); return -1; }
    char buf[1 << 16]; ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) { ssize_t w = write(out, buf + off, (size_t)(n - off)); if (w < 0) { close(in); close(out); return -1; } off += w; }
    }
    close(in);
    if (close(out) != 0 || n < 0) return -1;
    struct utimbuf ut = { st.st_atime, st.st_mtime };
    utime(dst, &ut);
    return 0;
}

char *read_file(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = 0;
    if (len) *len = (size_t)sz;
    return buf;
}

int write_file(const char *p, const char *data, size_t len, mode_t mode) {
    if (opts.dry) return 0;
    char *tmp = xasprintf("%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { free(tmp); return -1; }
    size_t off = 0;
    while (off < len) { ssize_t w = write(fd, data + off, len - off); if (w < 0) { close(fd); unlink(tmp); free(tmp); return -1; } off += (size_t)w; }
    fchmod(fd, mode);
    if (close(fd) != 0 || rename(tmp, p) != 0) { unlink(tmp); free(tmp); return -1; }
    free(tmp);
    return 0;
}

int append_file(const char *p, const char *data) {
    if (opts.dry) return 0;
    FILE *f = fopen(p, "ab");
    if (!f) return -1;
    fputs(data, f);
    return fclose(f);
}

JVal *read_json_file(const char *p) {
    size_t len; char *s = read_file(p, &len);
    if (!s) return NULL;
    JVal *v = json_parse(s, len);
    free(s);
    return v;
}

int write_json_file(const char *p, const JVal *v, mode_t mode) {
    char *s = json_dump(v, 2);
    int rc = write_file(p, s, strlen(s), mode);
    free(s);
    return rc;
}

static int rm_cb(const char *path, const char *rel, bool isdir, void *ctx) { (void)rel; (void)ctx; if (!isdir) unlink(path); return 0; }
int remove_tree(const char *p) {
    if (!exists(p)) return 0;
    if (!is_dir(p) || is_link(p)) return unlink(p);
    walk(p, rm_cb, NULL);
    /* remove directories bottom-up */
    char **names; long n = list_dir(p, &names);
    for (long i = 0; i < n; i++) { char *c = path_join(p, names[i]); if (is_dir(c) && !is_link(c)) remove_tree(c); else unlink(c); free(c); }
    free_names(names, n);
    return rmdir(p);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
long list_dir(const char *dir, char ***names) {
    DIR *d = opendir(dir);
    if (!d) { *names = NULL; return -1; }
    StrList l = {0};
    struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) sl_push(&l, e->d_name);
    closedir(d);
    if (l.n) qsort(l.v, l.n, sizeof *l.v, cmp_str);
    *names = l.v;
    return (long)l.n;
}
void free_names(char **names, long n) { for (long i = 0; i < n; i++) free(names[i]); free(names); }

static void walk_rec(const char *root, const char *path, walk_cb cb, void *ctx) {
    char **names; long n = list_dir(path, &names);
    for (long i = 0; i < n; i++) {
        char *full = path_join(path, names[i]);
        const char *rel = full + strlen(root) + 1;
        struct stat st;
        if (lstat(full, &st) == 0) {
            bool isdir = S_ISDIR(st.st_mode);
            int skip = cb(full, rel, isdir, ctx);
            if (isdir && !skip) walk_rec(root, full, cb, ctx);
        }
        free(full);
    }
    free_names(names, n);
}
void walk(const char *root, walk_cb cb, void *ctx) { walk_rec(root, root, cb, ctx); }

/* ------------------------------------------------------------- lists ---- */
void sl_push(StrList *l, const char *s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = strdup(s);
}
void sl_free(StrList *l) { for (size_t i = 0; i < l->n; i++) free(l->v[i]); free(l->v); l->v = NULL; l->n = l->cap = 0; }
bool sl_has(const StrList *l, const char *s) { for (size_t i = 0; i < l->n; i++) if (strcmp(l->v[i], s) == 0) return true; return false; }

void buf_add(Buf *b, const char *s, size_t len) {
    if (b->n + len + 1 > b->cap) { b->cap = (b->n + len + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, len); b->n += len; b->s[b->n] = 0;
}
void buf_adds(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_free(Buf *b) { free(b->s); b->s = NULL; b->n = b->cap = 0; }

/* ----------------------------------------------------------- the app ---- */
static int run_quiet(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { int nul = open("/dev/null", O_RDWR); dup2(nul, 1); dup2(nul, 2); execvp(argv[0], argv); _exit(127); }
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool app_running(void) { char *argv[] = { "pgrep", "-xq", "Claude", NULL }; return run_quiet(argv) == 0; }

void confirm_app_quit(void) {
    if (opts.dry || !app_running()) return;
    logf_("The Claude app is running. It reads session records only at startup and rewrites");
    logf_("~/.claude.json on its own, so quit it first.");
    if (opts.yes) return;
    printf("Continue anyway? [y/N] "); fflush(stdout);
    char line[16] = {0};
    if (!fgets(line, sizeof line, stdin) || (tolower((unsigned char)line[0]) != 'y')) die("Aborted. Quit Claude and run again.");
}

char *store_dir(void) {
    char **orgs; long no = list_dir(STORE_ROOT, &orgs);
    char *best = NULL; long best_n = -1;
    for (long i = 0; i < no; i++) {
        char *od = path_join(STORE_ROOT, orgs[i]);
        if (is_dir(od)) {
            char **users; long nu = list_dir(od, &users);
            for (long j = 0; j < nu; j++) {
                char *ud = path_join(od, users[j]);
                if (is_dir(ud)) {
                    char **f; long nf = list_dir(ud, &f); long recs = 0;
                    for (long k = 0; k < nf; k++) if (starts_with(f[k], "local_") && ends_with(f[k], ".json")) recs++;
                    free_names(f, nf);
                    if (recs > best_n) { free(best); best = strdup(ud); best_n = recs; }
                }
                free(ud);
            }
            free_names(users, nu);
        }
        free(od);
    }
    free_names(orgs, no);
    return best;
}

Records load_records(const char *store) {
    Records r = {0};
    char **f; long n = list_dir(store, &f);
    size_t cap = 0;
    for (long i = 0; i < n; i++) {
        if (!starts_with(f[i], "local_") || !ends_with(f[i], ".json")) continue;
        char *p = path_join(store, f[i]);
        size_t len; char *s = read_file(p, &len);
        if (!s) { if (errno == EACCES) r.unreadable++; free(p); continue; }
        JVal *v = json_parse(s, len); free(s);
        if (!v || v->type != J_OBJECT) { json_free(v); free(p); continue; }
        if (r.n == cap) { cap = cap ? cap * 2 : 64; r.v = realloc(r.v, cap * sizeof *r.v); }
        r.v[r.n].rec = v; r.v[r.n].path = p; r.n++;
    }
    free_names(f, n);
    return r;
}
const Rec *find_record(const Records *r, const char *cli_id) {
    for (size_t i = 0; i < r->n; i++) { const char *c = json_str(r->v[i].rec, "cliSessionId"); if (c && strcmp(c, cli_id) == 0) return &r->v[i]; }
    return NULL;
}
void free_records(Records *r) { for (size_t i = 0; i < r->n; i++) { json_free(r->v[i].rec); free(r->v[i].path); } free(r->v); r->v = NULL; r->n = 0; }

/* --------------------------------------------------------- ownership ---- */
typedef struct { StrList foreign; bool blocked; } OwnCtx;
static int own_cb(const char *path, const char *rel, bool isdir, void *ctx) {
    (void)rel; OwnCtx *c = ctx; struct stat st;
    if (lstat(path, &st) != 0) { c->blocked = true; return 1; }
    if (st.st_uid != getuid()) {
        sl_push(&c->foreign, path);
        if (isdir && access(path, R_OK | X_OK) != 0) c->blocked = true;
    }
    return 0;
}

void fix_ownership(void) {
    head("Ownership check");
    struct passwd *pw = getpwuid(getuid()); struct group *gr = getgrgid(getgid());
    const char *me = pw ? pw->pw_name : "me", *group = gr ? gr->gr_name : "staff";
    StrList paths = {0};
    const char *cands[] = { CLAUDE, CLAUDE_JSON, STORE_ROOT, NULL };
    for (int i = 0; cands[i]; i++) if (exists(cands[i])) sl_push(&paths, cands[i]);
    char *desk = path_join(HOME, "Desktop"); char **names; long n = list_dir(desk, &names);
    for (long i = 0; i < n; i++) if (starts_with(names[i], "claude-") && strstr(names[i], "snapshot")) { char *p = path_join(desk, names[i]); sl_push(&paths, p); free(p); }
    free_names(names, n); free(desk);

    OwnCtx c = {0};
    for (size_t i = 0; i < paths.n; i++) {
        struct stat st;
        if (lstat(paths.v[i], &st) == 0 && st.st_uid != getuid()) sl_push(&c.foreign, paths.v[i]);
        if (is_dir(paths.v[i])) walk(paths.v[i], own_cb, &c);
    }
    if (!c.foreign.n && !c.blocked) { logf_("   all files belong to %s", me); sl_free(&paths); sl_free(&c.foreign); return; }
    logf_("   %zu file(s) belong to another user (root, from a sudo run)%s", c.foreign.n, c.blocked ? ", and some folders could not be inspected" : "");
    for (size_t i = 0; i < c.foreign.n && i < 8; i++) logf_("      %s", tilde(c.foreign.v[i]));
    if (c.foreign.n > 8) logf_("      ...");
    if (opts.dry) {
        printf("   [dry-run] would run: sudo chown -R %s:%s on", me, group);
        for (size_t i = 0; i < paths.n; i++) printf(" %s", tilde(paths.v[i]));
        printf("\n");
    } else {
        logf_("   handing them back to %s (sudo will ask for your password)", me);
        char *owner = xasprintf("%s:%s", me, group);
        char **argv = calloc(paths.n + 6, sizeof *argv);
        size_t a = 0; argv[a++] = "sudo"; argv[a++] = "chown"; argv[a++] = "-R"; argv[a++] = "-h"; argv[a++] = owner;
        for (size_t i = 0; i < paths.n; i++) argv[a++] = paths.v[i];
        pid_t pid = fork();
        if (pid == 0) { execvp("sudo", argv); _exit(127); }
        int st = 0; waitpid(pid, &st, 0);
        free(argv); free(owner);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) die("chown failed; fix ownership by hand and run again");
        logf_("   done");
    }
    sl_free(&paths); sl_free(&c.foreign);
}
