/* backup: zip ~/.claude, ~/.claude.json and the app's session records. */
#include "commands.h"
#include "common.h"
#include "zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

typedef struct { ZipWriter *z; const char *arc_root; bool top_skip; long files; unsigned long long bytes; StrList problems; } BCtx;

static int backup_cb(const char *path, const char *rel, bool isdir, void *ctx) {
    BCtx *c = ctx;
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    if (strcmp(base, ".DS_Store") == 0) return 1;
    if (c->top_skip && !strchr(rel, '/') && in_list(base, SKIP_DIRS)) return 1;
    if (isdir || is_link(path)) return 0;
    struct stat st;
    if (stat(path, &st) != 0 || access(path, R_OK) != 0) { char *m = xasprintf("%s: permission denied", tilde(path)); sl_push(&c->problems, m); free(m); return 0; }
    c->files++; c->bytes += (unsigned long long)st.st_size;
    if (c->z) {
        char *arc = path_join(c->arc_root, rel);
        if (zip_add_file(c->z, path, arc) != 0) { char *m = xasprintf("%s: could not be read", tilde(path)); sl_push(&c->problems, m); free(m); }
        free(arc);
    }
    return 0;
}

void cmd_backup(const Args *a) {
    char host[256] = "mac"; gethostname(host, sizeof host); char *dot = strchr(host, '.'); if (dot) *dot = 0;
    char abs[8192]; if (!realpath(a->path ? a->path : ".", abs)) snprintf(abs, sizeof abs, "%s", a->path ? a->path : ".");
    const char *user = strrchr(HOME, '/'); user = user ? user + 1 : HOME;
    char *dir = path_join(abs, "ClaudeBackups");
    char *out = xasprintf("%s/%s_%s_%s.zip", dir, host, user, STAMP);
    head("Backup -> %s", out);

    BCtx c = {0};
    if (!opts.dry) {
        if (mkdir_p(dir) != 0) die("cannot create %s", dir);
        c.z = zip_create(out);
        if (!c.z) die("cannot create %s", out);
    }
    if (is_dir(CLAUDE)) { c.arc_root = ".claude"; c.top_skip = true; walk(CLAUDE, backup_cb, &c); }
    if (is_file(CLAUDE_JSON)) {
        struct stat st; stat(CLAUDE_JSON, &st);
        c.files++; c.bytes += (unsigned long long)st.st_size;
        if (c.z && zip_add_file(c.z, CLAUDE_JSON, ".claude.json") != 0) sl_push(&c.problems, "~/.claude.json: could not be read");
    }
    if (is_dir(STORE_ROOT)) { c.arc_root = "claude-code-sessions"; c.top_skip = false; walk(STORE_ROOT, backup_cb, &c); }

    if (c.z) {
        char ts[32]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
        JVal *m = json_object();
        json_set(m, "tool", json_string("claude-backup")); json_set(m, "created", json_string(ts));
        json_set(m, "host", json_string(host)); json_set(m, "home", json_string(HOME));
        json_set(m, "files", json_number((double)c.files)); json_set(m, "bytes", json_number((double)c.bytes));
        char *s = json_dump(m, 2); zip_add_data(c.z, "manifest.json", s, strlen(s)); free(s); json_free(m);
        if (zip_close(c.z) != 0) die("writing %s failed", out);
    }
    struct stat zs;
    if (!opts.dry && stat(out, &zs) == 0) logf_("   %ld files, %.0f MB uncompressed, zip %.0f MB", c.files, c.bytes / 1e6, zs.st_size / 1e6);
    else logf_("   %ld files, %.0f MB uncompressed  [dry-run, nothing written]", c.files, c.bytes / 1e6);
    for (size_t i = 0; i < c.problems.n && i < 10; i++) logf_("   !! %s", c.problems.v[i]);
    if (c.problems.n > 10) logf_("   !! ... %zu more", c.problems.n - 10);
    if (c.problems.n) logf_("   some files could not be read; run `fix-ownership` and back up again");
    logf_("   restore later with: claude-backup restore -p \"%s\"", out);
    sl_free(&c.problems); free(dir); free(out);
}
