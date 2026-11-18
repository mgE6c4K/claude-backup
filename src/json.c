#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ------------------------------------------------------------ build ---- */
static JVal *jnew(JType t) {
    JVal *v = calloc(1, sizeof *v);
    v->type = t;
    return v;
}
JVal *json_null(void) { return jnew(J_NULL); }
JVal *json_bool(bool b) { JVal *v = jnew(J_BOOL); v->b = b; return v; }
JVal *json_number(double d) { JVal *v = jnew(J_NUMBER); v->num = d; return v; }
JVal *json_string(const char *s) { JVal *v = jnew(J_STRING); v->str = strdup(s ? s : ""); return v; }
JVal *json_array(void) { return jnew(J_ARRAY); }
JVal *json_object(void) { return jnew(J_OBJECT); }

static void grow(JVal *v) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof *v->items);
        if (v->type == J_OBJECT) v->keys = realloc(v->keys, v->cap * sizeof *v->keys);
    }
}

void json_push(JVal *arr, JVal *val) {
    if (!arr || arr->type != J_ARRAY) { json_free(val); return; }
    grow(arr);
    arr->items[arr->n++] = val;
}

static long find_key(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJECT) return -1;
    for (size_t i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0) return (long)i;
    return -1;
}

JVal *json_get(const JVal *obj, const char *key) {
    long i = find_key(obj, key);
    return i < 0 ? NULL : obj->items[i];
}

void json_set(JVal *obj, const char *key, JVal *val) {
    if (!obj || obj->type != J_OBJECT) { json_free(val); return; }
    long i = find_key(obj, key);
    if (i >= 0) { json_free(obj->items[i]); obj->items[i] = val; return; }
    grow(obj);
    obj->keys[obj->n] = strdup(key);
    obj->items[obj->n++] = val;
}

void json_del(JVal *obj, const char *key) {
    long i = find_key(obj, key);
    if (i < 0) return;
    free(obj->keys[i]);
    json_free(obj->items[i]);
    memmove(&obj->keys[i], &obj->keys[i + 1], (obj->n - i - 1) * sizeof *obj->keys);
    memmove(&obj->items[i], &obj->items[i + 1], (obj->n - i - 1) * sizeof *obj->items);
    obj->n--;
}

const char *json_str(const JVal *obj, const char *key) {
    JVal *v = json_get(obj, key);
    return v && v->type == J_STRING ? v->str : NULL;
}
double json_num(const JVal *obj, const char *key, double dflt) {
    JVal *v = json_get(obj, key);
    return v && v->type == J_NUMBER ? v->num : dflt;
}
bool json_truthy(const JVal *obj, const char *key) {
    JVal *v = json_get(obj, key);
    return v && v->type == J_BOOL && v->b;
}

void json_free(JVal *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

JVal *json_copy(const JVal *v) {
    if (!v) return NULL;
    JVal *c = jnew(v->type);
    c->b = v->b; c->num = v->num;
    if (v->str) c->str = strdup(v->str);
    for (size_t i = 0; i < v->n; i++) {
        grow(c);
        c->items[c->n] = json_copy(v->items[i]);
        if (v->keys) c->keys[c->n] = strdup(v->keys[i]);
        c->n++;
    }
    return c;
}

bool json_equal(const JVal *a, const JVal *b) {
    if (a == b) return true;
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
    case J_NULL: return true;
    case J_BOOL: return a->b == b->b;
    case J_NUMBER: return a->num == b->num;
    case J_STRING: return strcmp(a->str, b->str) == 0;
    case J_ARRAY:
        if (a->n != b->n) return false;
        for (size_t i = 0; i < a->n; i++) if (!json_equal(a->items[i], b->items[i])) return false;
        return true;
    case J_OBJECT:
        if (a->n != b->n) return false;
        for (size_t i = 0; i < a->n; i++) {
            JVal *o = json_get(b, a->keys[i]);
            if (!o || !json_equal(a->items[i], o)) return false;
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ parse ---- */
typedef struct { const char *p, *end; } P;

static void ws(P *s) { while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(P *s, unsigned *out) {
    if (s->end - s->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(s->p[i]);
        if (h < 0) return 0;
        v = v * 16 + (unsigned)h;
    }
    s->p += 4;
    *out = v;
    return 1;
}

static void put_utf8(char **out, size_t *n, size_t *cap, unsigned cp) {
    char buf[4]; int len;
    if (cp < 0x80) { buf[0] = (char)cp; len = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
    else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
    else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
    if (*n + (size_t)len + 1 > *cap) { *cap = (*cap + len + 1) * 2; *out = realloc(*out, *cap); }
    memcpy(*out + *n, buf, (size_t)len);
    *n += (size_t)len;
}

static char *parse_string(P *s) {
    if (s->p >= s->end || *s->p != '"') return NULL;
    s->p++;
    size_t n = 0, cap = 64;
    char *out = malloc(cap);
    while (s->p < s->end && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p++;
        if (c == '\\') {
            if (s->p >= s->end) { free(out); return NULL; }
            char e = *s->p++;
            unsigned cp;
            switch (e) {
            case '"': cp = '"'; break; case '\\': cp = '\\'; break; case '/': cp = '/'; break;
            case 'b': cp = '\b'; break; case 'f': cp = '\f'; break; case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break; case 't': cp = '\t'; break;
            case 'u':
                if (!read_hex4(s, &cp)) { free(out); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                    P t = { s->p + 2, s->end }; unsigned lo;
                    if (read_hex4(&t, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        s->p = t.p;
                    }
                }
                break;
            default: free(out); return NULL;
            }
            put_utf8(&out, &n, &cap, cp);
        } else {
            if (n + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[n++] = (char)c;
        }
    }
    if (s->p >= s->end) { free(out); return NULL; }
    s->p++;
    out[n] = 0;
    return out;
}

static JVal *parse_value(P *s, int depth);

static JVal *parse_array(P *s, int depth) {
    s->p++;
    JVal *a = json_array();
    ws(s);
    if (s->p < s->end && *s->p == ']') { s->p++; return a; }
    for (;;) {
        JVal *v = parse_value(s, depth + 1);
        if (!v) { json_free(a); return NULL; }
        json_push(a, v);
        ws(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == ']') { s->p++; return a; }
        json_free(a); return NULL;
    }
}

static JVal *parse_object(P *s, int depth) {
    s->p++;
    JVal *o = json_object();
    ws(s);
    if (s->p < s->end && *s->p == '}') { s->p++; return o; }
    for (;;) {
        ws(s);
        char *k = parse_string(s);
        if (!k) { json_free(o); return NULL; }
        ws(s);
        if (s->p >= s->end || *s->p != ':') { free(k); json_free(o); return NULL; }
        s->p++;
        JVal *v = parse_value(s, depth + 1);
        if (!v) { free(k); json_free(o); return NULL; }
        grow(o);
        o->keys[o->n] = k; o->items[o->n++] = v;
        ws(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == '}') { s->p++; return o; }
        json_free(o); return NULL;
    }
}

static JVal *parse_value(P *s, int depth) {
    if (depth > 512) return NULL;
    ws(s);
    if (s->p >= s->end) return NULL;
    char c = *s->p;
    if (c == '{') return parse_object(s, depth);
    if (c == '[') return parse_array(s, depth);
    if (c == '"') { char *str = parse_string(s); if (!str) return NULL; JVal *v = jnew(J_STRING); v->str = str; return v; }
    if (!strncmp(s->p, "true", 4) && s->end - s->p >= 4) { s->p += 4; return json_bool(true); }
    if (!strncmp(s->p, "false", 5) && s->end - s->p >= 5) { s->p += 5; return json_bool(false); }
    if (!strncmp(s->p, "null", 4) && s->end - s->p >= 4) { s->p += 4; return json_null(); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp;
        double d = strtod(s->p, &endp);
        if (endp == s->p) return NULL;
        s->p = endp;
        return json_number(d);
    }
    return NULL;
}

JVal *json_parse(const char *text, size_t len) {
    P s = { text, text + len };
    JVal *v = parse_value(&s, 0);
    if (!v) return NULL;
    ws(&s);
    if (s.p != s.end) { json_free(v); return NULL; }
    return v;
}

/* ------------------------------------------------------------- dump ---- */
typedef struct { char *buf; size_t n, cap; } SB;

static void sb_put(SB *b, const char *s, size_t len) {
    if (b->n + len + 1 > b->cap) { b->cap = (b->n + len + 1) * 2; b->buf = realloc(b->buf, b->cap); }
    memcpy(b->buf + b->n, s, len);
    b->n += len;
    b->buf[b->n] = 0;
}
static void sb_puts(SB *b, const char *s) { sb_put(b, s, strlen(s)); }

static void dump_string(SB *b, const char *s) {
    sb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        case '\b': sb_puts(b, "\\b"); break;
        case '\f': sb_puts(b, "\\f"); break;
        default:
            if (*p < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", *p); sb_puts(b, t); }
            else sb_put(b, (const char *)p, 1);
        }
    }
    sb_put(b, "\"", 1);
}

static void nl(SB *b, int indent, int level) {
    if (!indent) return;
    sb_put(b, "\n", 1);
    for (int i = 0; i < indent * level; i++) sb_put(b, " ", 1);
}

static void dump(SB *b, const JVal *v, int indent, int level) {
    char t[64];
    switch (v->type) {
    case J_NULL: sb_puts(b, "null"); break;
    case J_BOOL: sb_puts(b, v->b ? "true" : "false"); break;
    case J_NUMBER:
        if (v->num == floor(v->num) && fabs(v->num) < 1e15) snprintf(t, sizeof t, "%lld", (long long)v->num);
        else snprintf(t, sizeof t, "%.17g", v->num);
        sb_puts(b, t);
        break;
    case J_STRING: dump_string(b, v->str); break;
    case J_ARRAY:
        sb_puts(b, "[");
        for (size_t i = 0; i < v->n; i++) {
            if (i) sb_puts(b, ",");
            nl(b, indent, level + 1);
            dump(b, v->items[i], indent, level + 1);
        }
        if (v->n) nl(b, indent, level);
        sb_puts(b, "]");
        break;
    case J_OBJECT:
        sb_puts(b, "{");
        for (size_t i = 0; i < v->n; i++) {
            if (i) sb_puts(b, ",");
            nl(b, indent, level + 1);
            dump_string(b, v->keys[i]);
            sb_puts(b, indent ? ": " : ":");
            dump(b, v->items[i], indent, level + 1);
        }
        if (v->n) nl(b, indent, level);
        sb_puts(b, "}");
        break;
    }
}

char *json_dump(const JVal *v, int indent) {
    SB b = {0};
    sb_put(&b, "", 0);
    dump(&b, v, indent, 0);
    return b.buf;
}
