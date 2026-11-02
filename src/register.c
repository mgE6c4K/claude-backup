/* register / dedupe: app session records for chats in ~/.claude/projects. */
#include "commands.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char *sid, *folder;
    char *cwd, *title, *agent, *mode, *model, *prompt;
    long long first, last;
    long turns;
    /* cwd candidates */
    StrList cwds; long *cwd_counts;
    const char *source; char *key;
} Cand;

static void cand_free(Cand *c) {
    free(c->sid); free(c->folder); free(c->cwd); free(c->title); free(c->agent); free(c->mode); free(c->model); free(c->prompt);
    sl_free(&c->cwds); free(c->cwd_counts); free(c->key);
}

static void set_str(char **dst, const char *s) { free(*dst); *dst = strdup(s); }

static const char *text_of(const JVal *content) {
    if (!content) return NULL;
    if (content->type == J_STRING) return content->str;
    if (content->type == J_ARRAY)
        for (size_t i = 0; i < content->n; i++) {
            const JVal *b = content->items[i];
            const char *t = json_str(b, "type");
            if (t && strcmp(t, "text") == 0) return json_str(b, "text") ? json_str(b, "text") : "";
        }
    return NULL;
}

static bool good_prompt(const char *t) {
    while (*t && isspace((unsigned char)*t)) t++;
    if (!*t || *t == '<' || *t == '/') return false;
    return !starts_with(t, "This session is being continued") && !starts_with(t, "[Request interrupted");
}

static void scan_transcript(const char *path, Cand *c) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        JVal *o = json_parse(line, (size_t)len);
        if (!o || o->type != J_OBJECT) { json_free(o); continue; }
        const char *type = json_str(o, "type"), *ts = json_str(o, "timestamp"), *cwd = json_str(o, "cwd");
        if (ts) { long long ms = iso_ms(ts); if (ms > 0) { if (c->first < 0 || ms < c->first) c->first = ms; if (ms > c->last) c->last = ms; } }
        if (cwd) {
            size_t i; for (i = 0; i < c->cwds.n; i++) if (strcmp(c->cwds.v[i], cwd) == 0) break;
            if (i == c->cwds.n) { sl_push(&c->cwds, cwd); c->cwd_counts = realloc(c->cwd_counts, c->cwds.n * sizeof *c->cwd_counts); c->cwd_counts[i] = 0; }
            c->cwd_counts[i]++;
        }
        if (type) {
            if (!strcmp(type, "custom-title") && json_str(o, "customTitle")) set_str(&c->title, json_str(o, "customTitle"));
            else if (!strcmp(type, "agent-name") && json_str(o, "agentName")) set_str(&c->agent, json_str(o, "agentName"));
            else if (!strcmp(type, "mode") && json_str(o, "mode")) set_str(&c->mode, json_str(o, "mode"));
            else if (!strcmp(type, "assistant")) {
                const char *m = json_str(json_get(o, "message"), "model");
                if (m && m[0] != '<') set_str(&c->model, m);
            } else if (!strcmp(type, "user") && !json_truthy(o, "isSidechain")) {
                const char *t = text_of(json_get(json_get(o, "message"), "content"));
                if (t && good_prompt(t)) { c->turns++; if (!c->prompt) { while (isspace((unsigned char)*t)) t++; c->prompt = strdup(t); } }
            }
        }
        json_free(o);
    }
    free(line); fclose(f);
    /* the app finds the transcript under slug(cwd): prefer the cwd that names this folder */
    long best = -1; size_t bi = 0;
    for (size_t i = 0; i < c->cwds.n; i++) {
        char *s = slug(c->cwds.v[i]); bool match = strcmp(s, c->folder) == 0; free(s);
        if (match) { c->cwd = strdup(c->cwds.v[i]); return; }
        if (c->cwd_counts[i] > best) { best = c->cwd_counts[i]; bi = i; }
    }
    if (c->cwds.n) c->cwd = strdup(c->cwds.v[bi]);
}

static char *auto_title(const char *text) {
    const char *src = text && *text ? text : "Untitled session";
    char *t = malloc(strlen(src) + 4); size_t n = 0; bool sp = false;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (isspace(*p)) { if (n && !sp) t[n++] = ' '; sp = true; } else { t[n++] = (char)*p; sp = false; }
    }
    while (n && t[n - 1] == ' ') n--;
    t[n] = 0;
    if (n <= 60) return t;
    size_t cut = 60; while (cut > 0 && t[cut] != ' ') cut--;
    if (cut <= 20) cut = 60;
    while (cut > 0 && (t[cut] & 0xC0) == 0x80) cut--;   /* do not split a UTF-8 sequence */
    t[cut] = 0; strcat(t, "\xe2\x80\xa6");
    return t;
}

typedef struct { Cand *v; size_t n, cap; } Cands;
typedef struct { long known, older, deleted, missing, empty; } Skipped;

static void collect(const char *pdir, const Records *recs, bool include_deleted, Cands *out, Skipped *sk) {
    char **folders; long nf = list_dir(pdir, &folders);
    for (long i = 0; i < nf; i++) {
        char *d = path_join(pdir, folders[i]);
        if (is_dir(d)) {
            char **names; long nn = list_dir(d, &names);
            for (long j = 0; j < nn; j++) {
                if (!ends_with(names[j], ".jsonl")) continue;
                char sid[64]; snprintf(sid, sizeof sid, "%.*s", (int)(strlen(names[j]) - 6), names[j]);
                if (!is_uuid(sid)) continue;
                if (find_record(recs, sid)) { sk->known++; continue; }
                char *marker = xasprintf("%s/%s.desktop-released.json", d, sid);
                bool deleted = exists(marker); free(marker);
                if (deleted && !include_deleted) { sk->deleted++; continue; }
                Cand c = {0}; c.first = -1; c.sid = strdup(sid); c.folder = strdup(folders[i]);
                char *tp = path_join(d, names[j]); scan_transcript(tp, &c); free(tp);
                if (!c.cwd || c.first < 0) { sk->empty++; cand_free(&c); continue; }
                if (!c.title) {
                    char *side = xasprintf("%s/%s/custom-title.json", d, sid); JVal *sj = read_json_file(side); free(side);
                    if (sj) { if (sj->type == J_STRING) set_str(&c.title, sj->str); else if (json_str(sj, "customTitle")) set_str(&c.title, json_str(sj, "customTitle")); else if (json_str(sj, "title")) set_str(&c.title, json_str(sj, "title")); json_free(sj); }
                }
                if (c.title) c.source = "user";
                else if (c.agent) { c.title = strdup(c.agent); c.source = "user"; }
                else { c.title = auto_title(c.prompt); c.source = "auto"; }
                c.key = title_key(c.title, c.source);
                if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 64; out->v = realloc(out->v, out->cap * sizeof *out->v); }
                out->v[out->n++] = c;
            }
            free_names(names, nn);
        }
        free(d);
    }
    free_names(folders, nf);
}

/* one transcript per title: the newest, counting records the app already has */
static void keep_latest(Cands *c, const Records *recs, Skipped *sk) {
    StrList keys = {0}; long long *newest = NULL; size_t nk = 0;
#define NEWEST(k, ms) do { size_t _i; for (_i = 0; _i < nk; _i++) if (!strcmp(keys.v[_i], (k))) break; \
        if (_i == nk) { sl_push(&keys, (k)); newest = realloc(newest, ++nk * sizeof *newest); newest[_i] = 0; } \
        if ((ms) > newest[_i]) newest[_i] = (ms); } while (0)
    for (size_t i = 0; i < recs->n; i++) {
        const JVal *r = recs->v[i].rec;
        char *k = title_key(json_str(r, "title"), json_str(r, "titleSource"));
        if (k && !json_truthy(r, "isArchived")) NEWEST(k, (long long)json_num(r, "lastActivityAt", 0));
        free(k);
    }
    for (size_t i = 0; i < c->n; i++) if (c->v[i].key) NEWEST(c->v[i].key, c->v[i].last);
    size_t w = 0;
    for (size_t i = 0; i < c->n; i++) {
        Cand *x = &c->v[i]; bool drop = false;
        if (x->key) for (size_t j = 0; j < nk; j++) if (!strcmp(keys.v[j], x->key) && x->last < newest[j]) drop = true;
        if (drop) { sk->older++; cand_free(x); } else c->v[w++] = *x;
    }
    c->n = w;
    sl_free(&keys); free(newest);
#undef NEWEST
}

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b; int r = strcmp(x->cwd, y->cwd); return r ? r : strcmp(x->title, y->title);
}

void cmd_register(const Args *a, bool app_checked) {
    if (!app_checked) confirm_app_quit();
    char *store = store_dir();
    if (!store) die("the app's session store does not exist yet; open the Code tab once first");
    char *pdir = path_join(CLAUDE, "projects");
    if (!is_dir(pdir)) die("no chats at ~/.claude/projects");
    Records recs = load_records(store);
    if (recs.unreadable) {
        if (opts.dry) logf_("note: %ld existing record(s) are not readable (owned by root from a sudo run); run fix-ownership first", recs.unreadable);
        else die("%ld existing record(s) are not readable (owned by root from a sudo run); run fix-ownership first", recs.unreadable);
    }
    const char *fallback = "claude-fable-5-1";
    for (size_t i = 0; i < recs.n; i++) { const char *m = json_str(recs.v[i].rec, "model"); if (m && in_list(m, CURRENT_MODELS)) { fallback = m; break; } }
    head("Register chats with the Claude app %s", a->latest ? "(latest copy of each title)" : "(all transcripts)");

    Cands cands = {0}; Skipped sk = {0};
    collect(pdir, &recs, a->include_deleted, &cands, &sk);
    if (a->latest) keep_latest(&cands, &recs, &sk);
    if (cands.n) qsort(cands.v, cands.n, sizeof *cands.v, cand_cmp);

    long made = 0; const char *last_cwd = NULL;
    for (size_t i = 0; i < cands.n; i++) {
        Cand *c = &cands.v[i];
        if (!last_cwd || strcmp(last_cwd, c->cwd)) { logf_("\n%s", tilde(c->cwd)); last_cwd = c->cwd; }
        if (!is_dir(c->cwd) && !a->include_missing) { sk.missing++; logf_("   - %s   [skip (no folder)]", c->title); continue; }
        char *sid = new_uuid(); char *local = xasprintf("local_%s", sid); free(sid);
        JVal *r = json_object();
        json_set(r, "sessionId", json_string(local)); json_set(r, "cliSessionId", json_string(c->sid));
        json_set(r, "cwd", json_string(c->cwd)); json_set(r, "originCwd", json_string(c->cwd));
        json_set(r, "createdAt", json_number((double)c->first)); json_set(r, "lastActivityAt", json_number((double)c->last));
        json_set(r, "lastFocusedAt", json_number((double)c->last));
        json_set(r, "model", json_string(c->model && in_list(c->model, CURRENT_MODELS) ? c->model : fallback));
        json_set(r, "effort", json_string("high")); json_set(r, "isArchived", json_bool(a->archived));
        json_set(r, "title", json_string(c->title)); json_set(r, "titleSource", json_string(c->source));
        json_set(r, "permissionMode", json_string(!c->mode || !strcmp(c->mode, "normal") ? "default" : c->mode));
        json_set(r, "completedTurns", json_number((double)c->turns));
        json_set(r, "enabledMcpTools", json_object()); json_set(r, "remoteMcpServersConfig", json_array());
        json_set(r, "alwaysAllowedReasons", json_array()); json_set(r, "sessionPermissionUpdates", json_array());
        char *out = xasprintf("%s/%s.json", store, local);
        if (write_json_file(out, r, 0600) != 0) die("cannot write %s", out);
        logf_("   + %s", c->title);
        made++;
        free(out); free(local); json_free(r);
    }
    logf_("\n%s: %ld", opts.dry ? "would register" : "registered", made);
    logf_("skipped: already in app %ld, older copy of same title %ld, deleted in app %ld, folder missing %ld, empty %ld",
          sk.known, sk.older, sk.deleted, sk.missing, sk.empty);
    if (sk.missing) logf_("  create the missing folders and run again, or pass --include-missing-folders");
    if (sk.older) logf_("  pass --all to register every transcript, including older copies");
    for (size_t i = 0; i < cands.n; i++) cand_free(&cands.v[i]);
    free(cands.v); free_records(&recs); free(store); free(pdir);
}

static int rec_newest_first(const void *a, const void *b) {
    double x = json_num((*(const Rec *const *)a)->rec, "lastActivityAt", 0), y = json_num((*(const Rec *const *)b)->rec, "lastActivityAt", 0);
    return x < y ? 1 : x > y ? -1 : 0;
}

void cmd_dedupe(const Args *a) {
    (void)a;
    confirm_app_quit();
    char *store = store_dir();
    if (!store) die("no app session store");
    Records recs = load_records(store);
    if (recs.unreadable) die("%ld record(s) unreadable; run fix-ownership first", recs.unreadable);
    head("Archive older app records that share a title");
    StrList keys = {0};
    for (size_t i = 0; i < recs.n; i++) {
        char *k = title_key(json_str(recs.v[i].rec, "title"), json_str(recs.v[i].rec, "titleSource"));
        if (k && !json_truthy(recs.v[i].rec, "isArchived") && !sl_has(&keys, k)) sl_push(&keys, k);
        free(k);
    }
    long n = 0;
    for (size_t ki = 0; ki < keys.n; ki++) {
        const Rec **grp = NULL; size_t gn = 0;
        for (size_t i = 0; i < recs.n; i++) {
            char *k = title_key(json_str(recs.v[i].rec, "title"), json_str(recs.v[i].rec, "titleSource"));
            if (k && !strcmp(k, keys.v[ki]) && !json_truthy(recs.v[i].rec, "isArchived")) { grp = realloc(grp, (gn + 1) * sizeof *grp); grp[gn++] = &recs.v[i]; }
            free(k);
        }
        if (gn >= 2) {
            qsort(grp, gn, sizeof *grp, rec_newest_first);
            logf_("   %s: keeping newest, archiving %zu", json_str(grp[0]->rec, "title"), gn - 1);
            for (size_t i = 1; i < gn; i++) {
                json_set(grp[i]->rec, "isArchived", json_bool(true));
                if (write_json_file(grp[i]->path, grp[i]->rec, 0600) != 0) die("cannot write %s", grp[i]->path);
                n++;
            }
        }
        free(grp);
    }
    logf_("\n%s: %ld", opts.dry ? "would archive" : "archived", n);
    sl_free(&keys); free_records(&recs); free(store);
}
