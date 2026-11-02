/* restore: add everything from one or more backups into this account. */
#include "commands.h"
#include "common.h"
#include "zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* old path prefix -> new path prefix (relative to HOME when not absolute); most specific first,
   old home folders last. A row applies only when the new folder exists. Extend with --map OLD=NEW. */
static const char *DEFAULT_MAP[][2] = {
    { "/Users/procare/Projects", "Desktop/ProCare/Projects" },
    { "/Users/Shared/Projects", "Desktop/Personal/Personal Projects" },
    { "/Library/Shared UserSpace/Personal Projects", "Desktop/Personal/Personal Projects" },
    { "/Users/personal/Documents/BSU/26-27", "Desktop/BSU/26-27" },
    { "/Users/personal/Documents/12th", "Desktop/Personal/Personal Docs/High School/12th" },
    { "/Users/personal", "" },
    { "/Users/procare", "" },
    { NULL, NULL }
};

/* ------------------------------------------------------------ rewriter --- */
typedef struct { StrList from, to; } Rewriter;

static void rw_add(Rewriter *rw, const char *from, const char *to) {
    if (is_dir(to)) { sl_push(&rw->from, from); sl_push(&rw->to, to); }
}
static char *rw_text(const Rewriter *rw, const char *s) {
    char *cur = strdup(s);
    for (size_t i = 0; i < rw->from.n; i++) { char *n = str_replace_all(cur, rw->from.v[i], rw->to.v[i]); free(cur); cur = n; }
    return cur;
}
static char *rw_slug(const Rewriter *rw, const char *b) {
    for (size_t i = 0; i < rw->from.n; i++) {
        char *so = slug(rw->from.v[i]), *sn = slug(rw->to.v[i]);
        size_t l = strlen(so);
        bool hit = strcmp(b, so) == 0 || (strncmp(b, so, l) == 0 && b[l] == '-');
        char *r = hit ? xasprintf("%s%s", sn, b + l) : NULL;
        free(so); free(sn);
        if (r) return r;
    }
    return strdup(b);
}
static bool is_text(const char *p) { return ends_with(p, ".jsonl") || ends_with(p, ".json") || ends_with(p, ".md"); }
static void rw_file(const Rewriter *rw, const char *path) {
    if (!rw->from.n || opts.dry || !is_text(path)) return;
    size_t len; char *s = read_file(path, &len);
    if (!s) return;
    char *t = rw_text(rw, s);
    if (strcmp(s, t) != 0) { struct stat st; stat(path, &st); write_file(path, t, strlen(t), st.st_mode & 0777); }
    free(s); free(t);
}

/* ------------------------------------------------------------- sources --- */
static char *newest_zip(const char *dir) {
    char **names; long n = list_dir(dir, &names); char *best = NULL; time_t bt = 0;
    for (long i = 0; i < n; i++) if (ends_with(names[i], ".zip")) {
        char *p = path_join(dir, names[i]); struct stat st;
        if (stat(p, &st) == 0 && (!best || st.st_mtime > bt)) { free(best); best = p; bt = st.st_mtime; } else free(p);
    }
    free_names(names, n);
    return best;
}

static char *resolve_source(const char *given) {
    char abs[8192];
    if (!realpath(given, abs)) die("%s: not found", given);
    if (is_file(abs)) { if (ends_with(abs, ".zip")) return strdup(abs); die("%s: not a backup zip", abs); }
    if (!is_dir(abs)) die("%s: not found", abs);
    char *c = path_join(abs, ".claude"), *p = path_join(abs, "projects");
    bool folder = is_dir(c) || is_dir(p); free(c); free(p);
    if (folder) return strdup(abs);
    char *z = newest_zip(abs);
    if (!z) { char *sub = path_join(abs, "ClaudeBackups"); if (is_dir(sub)) z = newest_zip(sub); free(sub); }
    if (!z) die("%s: no backup here (expected a .zip, a ClaudeBackups folder, or a .claude folder)", abs);
    return z;
}

static char *unpack_if_zip(const char *src, StrList *temps) {
    if (!ends_with(src, ".zip")) return strdup(src);
    char tmpl[] = "/tmp/claude-restore-XXXXXX";
    const char *tmpdir = getenv("TMPDIR");
    char *dir = tmpdir && *tmpdir ? xasprintf("%s/claude-restore-XXXXXX", tmpdir) : strdup(tmpl);
    if (!mkdtemp(dir)) die("cannot create temp folder");
    sl_push(temps, dir);
    logf_("   unpacking %s ...", src);
    const char *err = NULL;
    if (zip_extract(src, dir, &err) != 0) die("%s: %s", src, err ? err : "extract failed");
    return dir;
}

/* --------------------------------------------------------------- pieces --- */
typedef struct { const char *src, *dst; const Rewriter *rw; bool rewrite; StrList added; } CopyCtx;

static int copy_cb(const char *path, const char *rel, bool isdir, void *ctx) {
    CopyCtx *c = ctx;
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    if (!strcmp(base, ".DS_Store")) return 1;
    if (isdir) return 0;
    char *d = path_join(c->dst, rel);
    if (!exists(d)) {
        sl_push(&c->added, rel);
        if (!opts.dry) {
            char *parent = strdup(d); char *sl = strrchr(parent, '/'); if (sl) { *sl = 0; mkdir_p(parent); } free(parent);
            if (is_link(path)) { char target[8192]; ssize_t n = readlink(path, target, sizeof target - 1); if (n > 0) { target[n] = 0; symlink(target, d); } }
            else if (copy_file(path, d) == 0 && c->rw && c->rewrite) rw_file(c->rw, d);
        }
    }
    free(d);
    return 0;
}

/* copies files missing in dst; returns number added */
static long copy_missing(const char *src, const char *dst, const Rewriter *rw, bool rewrite) {
    CopyCtx c = { src, dst, rw, rewrite, {0} };
    walk(src, copy_cb, &c);
    long n = (long)c.added.n;
    sl_free(&c.added);
    return n;
}

/* append the lines of src that dst lacks (compared after path rewriting); returns count */
static long merge_markdown(const char *src_file, const char *dst_file, const Rewriter *rw, bool rewrite, const char *label) {
    size_t sl, dl; char *s0 = read_file(src_file, &sl), *d = read_file(dst_file, &dl);
    if (!s0 || !d) { free(s0); free(d); return 0; }
    char *s = rw && rewrite ? rw_text(rw, s0) : strdup(s0); free(s0);
    if (strcmp(s, d) == 0) { free(s); free(d); return 0; }
    StrList have = {0};
    for (char *p = d, *q; p && *p; p = q ? q + 1 : NULL) { q = strchr(p, '\n'); if (q) *q = 0; sl_push(&have, p); }
    Buf out = {0}; long n = 0;
    for (char *p = s, *q; p && *p; p = q ? q + 1 : NULL) {
        q = strchr(p, '\n'); if (q) *q = 0;
        bool blank = true; for (const char *t = p; *t; t++) if (*t != ' ' && *t != '\t' && *t != '\r') { blank = false; break; }
        if (!blank && !sl_has(&have, p)) { buf_adds(&out, p); buf_adds(&out, "\n"); n++; }
    }
    if (n) { char *hdr = xasprintf("\n<!-- merged from %s on %s -->\n", label, STAMP); append_file(dst_file, hdr); append_file(dst_file, out.s); free(hdr); }
    sl_free(&have); buf_free(&out); free(s); free(d);
    return n;
}

typedef struct { long files, md, records; } Totals;

typedef struct { const char *sp, *dst; const Rewriter *rw; bool rewrite; const char *b, *newb; Totals *t; } MdCtx;
static int md_cb(const char *path, const char *rel, bool isdir, void *ctx) {
    MdCtx *c = ctx;
    if (isdir || !ends_with(rel, ".md")) return 0;
    char *d = path_join(c->dst, rel);
    if (is_file(d)) { long n = merge_markdown(path, d, c->rw, c->rewrite, c->b); if (n) { c->t->md++; logf_("   merged %ld line(s) into %s/%s", n, c->newb, rel); } }
    free(d);
    return 0;
}

static void restore_projects(const char *pdir, const Rewriter *rw, Totals *t) {
    char **names; long n = list_dir(pdir, &names);
    char *root = path_join(CLAUDE, "projects");
    for (long i = 0; i < n; i++) {
        char *sp = path_join(pdir, names[i]);
        if (is_dir(sp)) {
            char *newb = rw_slug(rw, names[i]);
            char *dst = path_join(root, newb);
            bool rewrite = true;
            if (!opts.dry && mkdir_p(dst) != 0) {
                logf_("   !! cannot create %s (name too long) - keeping %s and its original paths", newb, names[i]);
                free(newb); free(dst); newb = strdup(names[i]); dst = path_join(root, newb); rewrite = false; mkdir_p(dst);
            }
            MdCtx mc = { sp, dst, rw, rewrite, names[i], newb, t };
            walk(sp, md_cb, &mc);
            long added = copy_missing(sp, dst, rw, rewrite);
            if (added) { t->files += added; if (!strcmp(newb, names[i])) logf_("   %s: +%ld file(s)", names[i], added); else logf_("   %s -> %s: +%ld file(s)", names[i], newb, added); }
            free(newb); free(dst);
        }
        free(sp);
    }
    free_names(names, n); free(root);
}

/* "/Users/<anyone>/.claude" -> this account's ~/.claude */
static char *rehome_claude_paths(const char *s) {
    Buf b = {0}; const char *p = s;
    for (;;) {
        const char *q = strstr(p, "/Users/");
        if (!q) { buf_adds(&b, p); break; }
        const char *name = q + 7, *end = name;
        while (*end && *end != '/' && *end != '"') end++;
        if (end > name && starts_with(end, "/.claude")) { buf_add(&b, p, (size_t)(q - p)); buf_adds(&b, CLAUDE); p = end + 8; }
        else { buf_add(&b, p, (size_t)(end - p)); p = end; }
    }
    return b.s ? b.s : strdup("");
}

static void merge_into(JVal *dst, const JVal *src) {  /* shallow: src keys that dst lacks */
    for (size_t i = 0; i < src->n; i++) if (!json_get(dst, src->keys[i])) json_set(dst, src->keys[i], json_copy(src->items[i]));
}

static void restore_plugins(const char *cd) {
    const char *subs[] = { "plugins/marketplaces", "plugins/cache", "plugins/data", NULL };
    for (int i = 0; subs[i]; i++) {
        char *s = path_join(cd, subs[i]), *d = path_join(CLAUDE, subs[i]);
        if (is_dir(s)) { long n = copy_missing(s, d, NULL, false); if (n) logf_("   %s: +%ld file(s)", subs[i], n); }
        free(s); free(d);
    }
    const char *mfs[] = { "installed_plugins.json", "known_marketplaces.json", NULL };
    for (int i = 0; mfs[i]; i++) {
        char *sp = xasprintf("%s/plugins/%s", cd, mfs[i]); size_t len; char *raw = read_file(sp, &len); free(sp);
        if (!raw) continue;
        char *rehomed = rehome_claude_paths(raw); free(raw);
        JVal *src = json_parse(rehomed, strlen(rehomed)); free(rehomed);
        if (!src || src->type != J_OBJECT) { json_free(src); continue; }
        char *dp = xasprintf("%s/plugins/%s", CLAUDE, mfs[i]);
        JVal *cur = read_json_file(dp); if (!cur || cur->type != J_OBJECT) { json_free(cur); cur = json_object(); }
        JVal *merged = json_copy(cur);
        merge_into(merged, src);
        JVal *sp_ = json_get(src, "plugins"), *mp = json_get(merged, "plugins");
        if (sp_ && sp_->type == J_OBJECT && mp && mp->type == J_OBJECT) merge_into(mp, sp_);
        if (!json_equal(merged, cur)) {
            char *pd = path_join(CLAUDE, "plugins"); if (!opts.dry) mkdir_p(pd); free(pd);
            write_json_file(dp, merged, 0600); logf_("   plugins/%s: merged", mfs[i]);
        }
        json_free(src); json_free(cur); json_free(merged); free(dp);
    }
}

static bool hook_available(const char *cmd) {
    if (!cmd || cmd[0] != '/') return true;
    char exe[4096]; snprintf(exe, sizeof exe, "%s", cmd); char *sp = strchr(exe, ' '); if (sp) *sp = 0;
    return exists(exe);
}

static void union_into(JVal *dst_arr, const JVal *src_arr) {
    if (!src_arr || src_arr->type != J_ARRAY) return;
    for (size_t i = 0; i < src_arr->n; i++) {
        bool have = false;
        for (size_t j = 0; j < dst_arr->n && !have; j++) have = json_equal(dst_arr->items[j], src_arr->items[i]);
        if (!have) json_push(dst_arr, json_copy(src_arr->items[i]));
    }
}

static int cmp_jstr(const void *a, const void *b) { return strcmp((*(JVal *const *)a)->str, (*(JVal *const *)b)->str); }

/* later sources win on scalars; permission lists are unioned; hooks for missing tools dropped */
static JVal *merge_settings(JVal **sources, int n, const JVal *current, StrList *dropped) {
    JVal *merged = json_object();
    JVal *perm_lists[3] = { json_array(), json_array(), json_array() }; const char *pn[3] = { "allow", "deny", "ask" };
    for (int si = 0; si <= n; si++) {
        const JVal *s = si < n ? sources[si] : current;
        if (!s || s->type != J_OBJECT) continue;
        for (size_t i = 0; i < s->n; i++) {
            const char *k = s->keys[i]; const JVal *v = s->items[i];
            if (!strcmp(k, "permissions")) { for (int p = 0; p < 3; p++) union_into(perm_lists[p], json_get(v, pn[p])); }
            else if (v->type == J_OBJECT && json_get(merged, k) && json_get(merged, k)->type == J_OBJECT) {
                JVal *m = json_get(merged, k);
                for (size_t j = 0; j < v->n; j++) json_set(m, v->keys[j], json_copy(v->items[j]));
            } else json_set(merged, k, json_copy(v));
        }
    }
    JVal *perms = json_object();
    for (int p = 0; p < 3; p++) {
        if (perm_lists[p]->n) { qsort(perm_lists[p]->items, perm_lists[p]->n, sizeof *perm_lists[p]->items, cmp_jstr); json_set(perms, pn[p], perm_lists[p]); }
        else json_free(perm_lists[p]);
    }
    if (perms->n) json_set(merged, "permissions", perms); else json_free(perms);
    JVal *hooks = json_get(merged, "hooks");
    if (hooks && hooks->type == J_OBJECT) {
        for (size_t e = 0; e < hooks->n;) {
            JVal *groups = hooks->items[e]; JVal *kept = json_array();
            if (groups->type == J_ARRAY) for (size_t g = 0; g < groups->n; g++) {
                JVal *grp = groups->items[g], *hs = json_get(grp, "hooks"), *ok = json_array();
                if (hs && hs->type == J_ARRAY) for (size_t h = 0; h < hs->n; h++) {
                    const char *cmd = json_str(hs->items[h], "command");
                    if (hook_available(cmd)) json_push(ok, json_copy(hs->items[h])); else sl_push(dropped, cmd ? cmd : "?");
                }
                if (ok->n) { JVal *ng = json_copy(grp); json_set(ng, "hooks", ok); json_push(kept, ng); } else json_free(ok);
            }
            if (kept->n) { json_free(hooks->items[e]); hooks->items[e] = kept; e++; }
            else { json_free(kept); json_del(hooks, hooks->keys[e]); }
        }
        if (!hooks->n) json_del(merged, "hooks");
    }
    return merged;
}

typedef struct { JVal *settings[16]; int nsettings; StrList histories, cjsons, records; } Gathered;

static void restore_config(const char *cd, const char *label, const Rewriter *rw, Gathered *g) {
    char *p = path_join(cd, "settings.json"); JVal *sj = read_json_file(p); free(p);
    if (sj && sj->type == J_OBJECT && g->nsettings < 16) g->settings[g->nsettings++] = sj; else json_free(sj);

    char *sl = path_join(cd, "settings.local.json"), *dl = path_join(CLAUDE, "settings.local.json");
    if (is_file(sl) && !exists(dl)) { if (!opts.dry) { copy_file(sl, dl); rw_file(rw, dl); } logf_("   settings.local.json: copied"); }
    free(sl); free(dl);

    char *md = path_join(cd, "CLAUDE.md"), *dst = path_join(CLAUDE, "CLAUDE.md");
    if (is_file(md)) {
        if (!exists(dst)) { if (!opts.dry) copy_file(md, dst); logf_("   CLAUDE.md: copied from %s", label); }
        else {
            size_t a, b; char *src = read_file(md, &a), *have = read_file(dst, &b);
            if (src && have) {
                char *t = src; while (*t && (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r')) t++;
                size_t tl = strlen(t); while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\n' || t[tl - 1] == '\t' || t[tl - 1] == '\r')) tl--;
                char *trim = strndup(t, tl);
                if (!strstr(have, trim)) { char *hdr = xasprintf("\n\n<!-- merged from %s on %s -->\n\n", label, STAMP); append_file(dst, hdr); append_file(dst, src); free(hdr); logf_("   CLAUDE.md: appended content from %s", label); }
                free(trim);
            }
            free(src); free(have);
        }
    }
    free(md); free(dst);

    const char *small[] = { ".ponytail-active", ".ponytail-statusline-nudged", NULL };
    for (int i = 0; small[i]; i++) { char *s = path_join(cd, small[i]), *d = path_join(CLAUDE, small[i]); if (is_file(s) && !exists(d) && !opts.dry) copy_file(s, d); free(s); free(d); }

    char *h = path_join(cd, "history.jsonl"); if (is_file(h)) sl_push(&g->histories, h); free(h);
}

static void write_settings(Gathered *g) {
    char *p = path_join(CLAUDE, "settings.json"); JVal *cur = read_json_file(p);
    StrList dropped = {0};
    JVal *merged = merge_settings(g->settings, g->nsettings, cur, &dropped);
    if (!json_equal(merged, cur)) {
        write_json_file(p, merged, 0600);
        if (dropped.n) { Buf b = {0}; for (size_t i = 0; i < dropped.n; i++) { if (i) buf_adds(&b, ", "); buf_adds(&b, dropped.v[i]); } logf_("   settings.json: merged; dropped hooks whose command is not installed here: %s", b.s); buf_free(&b); }
        else logf_("   settings.json: merged");
    }
    json_free(merged); json_free(cur); sl_free(&dropped); free(p);
}

typedef struct { double ts; char *line; } HLine;
static int hcmp(const void *a, const void *b) { double x = ((const HLine *)a)->ts, y = ((const HLine *)b)->ts; return x < y ? -1 : x > y; }

static void write_history(Gathered *g, const Rewriter *rw) {
    char *mine = path_join(CLAUDE, "history.jsonl");
    StrList lines = {0};
    for (size_t i = 0; i <= g->histories.n; i++) {
        const char *f = i < g->histories.n ? g->histories.v[i] : mine;
        if (i == g->histories.n && !is_file(mine)) break;
        size_t len; char *s = read_file(f, &len); if (!s) continue;
        for (char *p = s, *q; p && *p; p = q ? q + 1 : NULL) {
            q = strchr(p, '\n'); if (q) *q = 0;
            bool blank = true; for (const char *t = p; *t; t++) if (*t > ' ') { blank = false; break; }
            if (blank) continue;
            char *rl = rw_text(rw, p);
            if (!sl_has(&lines, rl)) sl_push(&lines, rl);
            free(rl);
        }
        free(s);
    }
    HLine *hl = calloc(lines.n ? lines.n : 1, sizeof *hl); size_t n = 0;
    for (size_t i = 0; i < lines.n; i++) {
        JVal *o = json_parse(lines.v[i], strlen(lines.v[i]));
        if (o) { hl[n].ts = json_num(o, "timestamp", 0); hl[n++].line = lines.v[i]; json_free(o); }
    }
    qsort(hl, n, sizeof *hl, hcmp);
    Buf b = {0}; for (size_t i = 0; i < n; i++) { buf_adds(&b, hl[i].line); buf_adds(&b, "\n"); }
    write_file(mine, b.s ? b.s : "", b.n, 0600);
    logf_("   history.jsonl: %zu entries", n);
    buf_free(&b); free(hl); sl_free(&lines); free(mine);
}

static void deep_merge(JVal *acc, const JVal *part) {  /* one level deep, part wins */
    if (!part || part->type != J_OBJECT) return;
    for (size_t i = 0; i < part->n; i++) {
        JVal *have = json_get(acc, part->keys[i]);
        if (have && have->type == J_OBJECT && part->items[i]->type == J_OBJECT) { for (size_t j = 0; j < part->items[i]->n; j++) json_set(have, part->items[i]->keys[j], json_copy(part->items[i]->items[j])); }
        else json_set(acc, part->keys[i], json_copy(part->items[i]));
    }
}

/* project list and a few preferences; current values win */
static void merge_claude_json(Gathered *g, const Rewriter *rw) {
    JVal *cur = read_json_file(CLAUDE_JSON); if (!cur || cur->type != J_OBJECT) { json_free(cur); cur = json_object(); }
    JVal *out = json_copy(cur);
    const char *keys[] = { "projects", "githubRepoPaths", "skillUsage", "tipsHistory", NULL };
    bool onboarding = false;
    JVal **parsed = calloc(g->cjsons.n ? g->cjsons.n : 1, sizeof *parsed);
    for (size_t i = 0; i < g->cjsons.n; i++) {
        size_t len; char *raw = read_file(g->cjsons.v[i], &len);
        if (!raw) continue;
        char *rewritten = rw_text(rw, raw); free(raw);
        parsed[i] = json_parse(rewritten, strlen(rewritten)); free(rewritten);
        if (parsed[i] && json_truthy(parsed[i], "hasCompletedOnboarding")) onboarding = true;
    }
    for (int k = 0; keys[k]; k++) {
        JVal *acc = json_object();
        for (size_t i = 0; i < g->cjsons.n; i++) if (parsed[i]) deep_merge(acc, json_get(parsed[i], keys[k]));
        deep_merge(acc, json_get(cur, keys[k]));
        json_set(out, keys[k], acc);
    }
    if (!json_get(out, "hasCompletedOnboarding")) json_set(out, "hasCompletedOnboarding", json_bool(onboarding));
    if (!json_equal(out, cur)) write_json_file(CLAUDE_JSON, out, 0600);
    JVal *pr = json_get(out, "projects");
    logf_("   ~/.claude.json: %zu projects listed", pr ? pr->n : 0);
    for (size_t i = 0; i < g->cjsons.n; i++) json_free(parsed[i]);
    free(parsed); json_free(cur); json_free(out);
}

/* app session records carried inside a backup (keeps exact titles); returns count */
static long restore_records(Gathered *g, const Rewriter *rw) {
    char *store = store_dir();
    if (!store) return 0;
    Records known = load_records(store);
    if (known.unreadable && !opts.dry) die("%ld app record(s) unreadable; run fix-ownership", known.unreadable);
    long n = 0;
    for (size_t i = 0; i < g->records.n; i++) {
        JVal *rec = read_json_file(g->records.v[i]);
        const char *cli = rec ? json_str(rec, "cliSessionId") : NULL;
        if (!cli || find_record(&known, cli)) { json_free(rec); continue; }
        const char *fields[] = { "cwd", "originCwd", "gitAnchorsFolderRealpath", NULL };
        for (int f = 0; fields[f]; f++) { const char *v = json_str(rec, fields[f]); if (v) { char *t = rw_text(rw, v); json_set(rec, fields[f], json_string(t)); free(t); } }
        const char *cwd = json_str(rec, "cwd");
        if (!cwd) { json_free(rec); continue; }
        char *sl = slug(cwd); char *tr = xasprintf("%s/projects/%s/%s.jsonl", CLAUDE, sl, cli); free(sl);
        bool have_tr = is_file(tr); free(tr);
        if (!have_tr) { json_free(rec); continue; }
        const char *sid0 = json_str(rec, "sessionId");
        char *sid = sid0 ? strdup(sid0) : NULL;
        char *out = sid ? xasprintf("%s/%s.json", store, sid) : NULL;
        if (!sid || exists(out)) { free(sid); free(out); char *u = new_uuid(); sid = xasprintf("local_%s", u); free(u); out = xasprintf("%s/%s.json", store, sid); }
        json_set(rec, "sessionId", json_string(sid));
        write_json_file(out, rec, 0600);
        /* remember it so a duplicate inside the same backup is skipped */
        known.v = realloc(known.v, (known.n + 1) * sizeof *known.v); known.v[known.n].rec = rec; known.v[known.n].path = out; known.n++;
        free(sid); n++;
    }
    if (n) logf_("   app records restored from backup: %ld", n);
    free_records(&known); free(store);
    return n;
}

static int snap_cb(const char *path, const char *rel, bool isdir, void *ctx) {
    const char *snap = ctx;
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    if (!strchr(rel, '/') && (!strcmp(base, "projects") || !strcmp(base, "file-history") || in_list(base, SKIP_DIRS))) return 1;
    char *d = xasprintf("%s/.claude/%s", snap, rel);
    if (isdir) mkdir_p(d); else if (!is_link(path)) copy_file(path, d);
    free(d);
    return 0;
}

/* small copy of the config files (chats and rewind data are only ever added to) */
static void snapshot_config(void) {
    if (opts.dry) return;
    char *snap = xasprintf("%s/Desktop/claude-pre-restore-snapshot-%s", HOME, STAMP);
    char *sc = path_join(snap, ".claude"); mkdir_p(sc); free(sc);
    if (is_dir(CLAUDE)) walk(CLAUDE, snap_cb, snap);
    if (is_file(CLAUDE_JSON)) { char *d = path_join(snap, ".claude.json"); copy_file(CLAUDE_JSON, d); free(d); }
    logf_("   config snapshot: %s", tilde(snap));
    free(snap);
}

/* ----------------------------------------------------------------- main --- */
void cmd_restore(const Args *a) {
    StrList sources = {0};
    if (a->npaths) for (int i = 0; i < a->npaths; i++) { char *s = resolve_source(a->paths[i]); sl_push(&sources, s); free(s); }
    else { char *s = resolve_source("."); sl_push(&sources, s); free(s); }
    confirm_app_quit();
    fix_ownership();

    Rewriter rw = {0};
    for (int i = 0; i < a->nmaps; i++) {
        const char *eq = strchr(a->maps[i], '=');
        if (!eq) die("--map expects OLD=NEW, got %s", a->maps[i]);
        char *old = strndup(a->maps[i], (size_t)(eq - a->maps[i]));
        rw_add(&rw, old, eq + 1); free(old);
    }
    if (!a->no_remap) for (int i = 0; DEFAULT_MAP[i][0]; i++) {
        char *to = DEFAULT_MAP[i][1][0] ? path_join(HOME, DEFAULT_MAP[i][1]) : strdup(HOME);
        rw_add(&rw, DEFAULT_MAP[i][0], to); free(to);
    }

    head("Restore");
    for (size_t i = 0; i < sources.n; i++) logf_("   from %s", sources.v[i]);
    if (rw.from.n) { logf_("   chats are placed under the folders that exist on this Mac:"); for (size_t i = 0; i < rw.from.n; i++) logf_("      %s  ->  %s", rw.from.v[i], tilde(rw.to.v[i])); }

    StrList temps = {0}, labels = {0}, dirs = {0};
    for (size_t i = 0; i < sources.n; i++) {
        const char *base = strrchr(sources.v[i], '/'); sl_push(&labels, base ? base + 1 : sources.v[i]);
        char *d = unpack_if_zip(sources.v[i], &temps); sl_push(&dirs, d); free(d);
    }

    snapshot_config();
    if (!opts.dry) { char *p = path_join(CLAUDE, "projects"); mkdir_p(p); free(p); }

    Totals t = {0}; Gathered g = {0};
    for (size_t i = 0; i < dirs.n; i++) {
        const char *src = dirs.v[i];
        char *cd = path_join(src, ".claude");
        if (!is_dir(cd)) { free(cd); cd = strdup(src); }
        char *pd = path_join(cd, "projects");
        if (!is_dir(pd)) die("%s: no .claude/projects inside", sources.v[i]);
        restore_projects(pd, &rw, &t);
        const char *subs[] = { "file-history", "session-env", "skills", NULL };
        for (int s = 0; subs[s]; s++) {
            char *sp = path_join(cd, subs[s]), *dp = path_join(CLAUDE, subs[s]);
            if (is_dir(sp)) { long n = copy_missing(sp, dp, NULL, false); if (n) { t.files += n; logf_("   %s: +%ld file(s)", subs[s], n); } }
            free(sp); free(dp);
        }
        restore_plugins(cd);
        restore_config(cd, labels.v[i], &rw, &g);
        char *cj = path_join(src, ".claude.json"); if (is_file(cj)) sl_push(&g.cjsons, cj); free(cj);
        char *st = path_join(src, "claude-code-sessions");
        if (is_dir(st)) {
            char **orgs; long no = list_dir(st, &orgs);
            for (long o = 0; o < no; o++) {
                char *od = path_join(st, orgs[o]); char **users; long nu = list_dir(od, &users);
                for (long u = 0; u < nu; u++) {
                    char *ud = path_join(od, users[u]); char **fs; long nf = list_dir(ud, &fs);
                    for (long f = 0; f < nf; f++) if (starts_with(fs[f], "local_") && ends_with(fs[f], ".json")) { char *rp = path_join(ud, fs[f]); sl_push(&g.records, rp); free(rp); }
                    free_names(fs, nf); free(ud);
                }
                free_names(users, nu); free(od);
            }
            free_names(orgs, no);
        }
        free(st); free(pd); free(cd);
    }

    if (g.nsettings) write_settings(&g);
    if (g.histories.n) write_history(&g, &rw);
    if (g.cjsons.n) merge_claude_json(&g, &rw);
    if (g.records.n) t.records = restore_records(&g, &rw);

    head("Restore summary");
    logf_("   %ld file(s) added, %ld markdown merge(s), %ld app record(s)", t.files, t.md, t.records);
    if (!t.files && !t.md) logf_("   everything from the backup is already here");
    char *pdir = path_join(CLAUDE, "projects");
    if (is_dir(pdir)) {
        char **fo; long nf = list_dir(pdir, &fo); long chats = 0;
        for (long i = 0; i < nf; i++) { char *d = path_join(pdir, fo[i]); char **fs; long n = list_dir(d, &fs); for (long j = 0; j < n; j++) if (ends_with(fs[j], ".jsonl")) chats++; free_names(fs, n); free(d); }
        logf_("   chats on disk: %ld in %ld folders", chats, nf);
        free_names(fo, nf);
    }
    free(pdir);

    for (size_t i = 0; i < temps.n; i++) remove_tree(temps.v[i]);
    for (int i = 0; i < g.nsettings; i++) json_free(g.settings[i]);
    sl_free(&g.histories); sl_free(&g.cjsons); sl_free(&g.records);
    sl_free(&temps); sl_free(&labels); sl_free(&dirs); sl_free(&sources); sl_free(&rw.from); sl_free(&rw.to);

    if (strcmp(a->reg, "none") != 0) {
        Args ra = *a; ra.latest = strcmp(a->reg, "latest") == 0;
        cmd_register(&ra, true);
    }
}
