#include "zip.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>
#include <zlib.h>

#define SIG_LOCAL 0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD 0x06054b50u
#define SIG_EOCD64 0x06064b50u
#define SIG_LOC64 0x07064b50u

typedef struct {
    char *name;
    uint32_t crc, mode;
    uint64_t csize, usize, offset;
    uint16_t dostime, dosdate;
    bool deflated;
} Entry;

struct ZipWriter {
    FILE *f;
    Entry *e;
    size_t n, cap;
    uint64_t pos;
};

/* ---------------------------------------------------------- little io --- */
static void w16(FILE *f, uint16_t v) { unsigned char b[2] = { v & 0xff, v >> 8 }; fwrite(b, 1, 2, f); }
static void w32(FILE *f, uint32_t v) { unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 }; fwrite(b, 1, 4, f); }
static void w64(FILE *f, uint64_t v) { for (int i = 0; i < 8; i++) fputc((int)((v >> (8 * i)) & 0xff), f); }
static uint16_t r16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const unsigned char *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

static void dos_time(time_t t, uint16_t *dtime, uint16_t *ddate) {
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80) { tm.tm_year = 80; tm.tm_mon = 0; tm.tm_mday = 1; tm.tm_hour = tm.tm_min = tm.tm_sec = 0; }
    *dtime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    *ddate = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

static time_t from_dos(uint16_t dtime, uint16_t ddate) {
    struct tm tm = {0};
    tm.tm_year = (ddate >> 9) + 80; tm.tm_mon = ((ddate >> 5) & 15) - 1; tm.tm_mday = ddate & 31;
    tm.tm_hour = dtime >> 11; tm.tm_min = (dtime >> 5) & 63; tm.tm_sec = (dtime & 31) * 2;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* ------------------------------------------------------------- writer --- */
ZipWriter *zip_create(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    ZipWriter *z = calloc(1, sizeof *z);
    z->f = f;
    return z;
}

static int deflate_buf(const unsigned char *in, size_t inlen, unsigned char **out, size_t *outlen) {
    z_stream s = {0};
    if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    size_t cap = deflateBound(&s, (uLong)inlen);
    *out = malloc(cap ? cap : 1);
    s.next_in = (unsigned char *)in; s.avail_in = (uInt)inlen;
    s.next_out = *out; s.avail_out = (uInt)cap;
    int rc = deflate(&s, Z_FINISH);
    *outlen = s.total_out;
    deflateEnd(&s);
    if (rc != Z_STREAM_END) { free(*out); return -1; }
    return 0;
}

static int add_entry(ZipWriter *z, const char *arcname, const unsigned char *data, size_t len, uint32_t mode, time_t mtime) {
    unsigned char *comp = NULL; size_t clen = 0;
    bool deflated = len > 0 && deflate_buf(data, len, &comp, &clen) == 0 && clen < len;
    if (!deflated) { free(comp); comp = (unsigned char *)data; clen = len; }

    if (z->n == z->cap) { z->cap = z->cap ? z->cap * 2 : 64; z->e = realloc(z->e, z->cap * sizeof *z->e); }
    Entry *e = &z->e[z->n++];
    memset(e, 0, sizeof *e);
    e->name = strdup(arcname);
    e->crc = (uint32_t)crc32(0, data, (uInt)len);
    e->csize = clen; e->usize = len; e->offset = z->pos; e->mode = mode; e->deflated = deflated;
    dos_time(mtime, &e->dostime, &e->dosdate);

    bool big = len >= 0xFFFFFFFFu || clen >= 0xFFFFFFFFu;
    size_t nlen = strlen(arcname);
    w32(z->f, SIG_LOCAL); w16(z->f, big ? 45 : 20); w16(z->f, 0x0800 /* utf-8 names */);
    w16(z->f, deflated ? 8 : 0); w16(z->f, e->dostime); w16(z->f, e->dosdate);
    w32(z->f, e->crc);
    w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)clen); w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)len);
    w16(z->f, (uint16_t)nlen); w16(z->f, big ? 20 : 0);
    fwrite(arcname, 1, nlen, z->f);
    if (big) { w16(z->f, 1); w16(z->f, 16); w64(z->f, len); w64(z->f, clen); }
    size_t header = 30 + nlen + (big ? 20 : 0);
    if (clen && fwrite(comp, 1, clen, z->f) != clen) { if (deflated) free(comp); return -1; }
    z->pos += header + clen;
    if (deflated) free(comp);
    return ferror(z->f) ? -1 : 0;
}

int zip_add_data(ZipWriter *z, const char *arcname, const void *data, size_t len) {
    return add_entry(z, arcname, data, len, 0644, time(NULL));
}

int zip_add_file(ZipWriter *z, const char *src, const char *arcname) {
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    size_t len = (size_t)st.st_size;
    unsigned char *buf = malloc(len ? len : 1);
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) { free(buf); return -1; }
    int rc = add_entry(z, arcname, buf, len, st.st_mode & 0777, st.st_mtime);
    free(buf);
    return rc;
}

int zip_close(ZipWriter *z) {
    uint64_t cd_start = z->pos, cd_size = 0;
    for (size_t i = 0; i < z->n; i++) {
        Entry *e = &z->e[i];
        bool big = e->usize >= 0xFFFFFFFFu || e->csize >= 0xFFFFFFFFu || e->offset >= 0xFFFFFFFFu;
        size_t nlen = strlen(e->name);
        uint16_t extra = big ? 28 : 0;
        w32(z->f, SIG_CENTRAL); w16(z->f, (3 << 8) | 45); w16(z->f, big ? 45 : 20); w16(z->f, 0x0800);
        w16(z->f, e->deflated ? 8 : 0); w16(z->f, e->dostime); w16(z->f, e->dosdate); w32(z->f, e->crc);
        w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)e->csize); w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)e->usize);
        w16(z->f, (uint16_t)nlen); w16(z->f, extra); w16(z->f, 0); w16(z->f, 0); w16(z->f, 0);
        w32(z->f, e->mode << 16); w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)e->offset);
        fwrite(e->name, 1, nlen, z->f);
        if (big) { w16(z->f, 1); w16(z->f, 24); w64(z->f, e->usize); w64(z->f, e->csize); w64(z->f, e->offset); }
        cd_size += 46 + nlen + extra;
    }
    uint64_t cd_end = cd_start + cd_size;
    bool big = z->n >= 0xFFFF || cd_start >= 0xFFFFFFFFu || cd_size >= 0xFFFFFFFFu;
    if (big) {
        w32(z->f, SIG_EOCD64); w64(z->f, 44); w16(z->f, (3 << 8) | 45); w16(z->f, 45); w32(z->f, 0); w32(z->f, 0);
        w64(z->f, z->n); w64(z->f, z->n); w64(z->f, cd_size); w64(z->f, cd_start);
        w32(z->f, SIG_LOC64); w32(z->f, 0); w64(z->f, cd_end); w32(z->f, 1);
    }
    w32(z->f, SIG_EOCD); w16(z->f, 0); w16(z->f, 0);
    w16(z->f, big ? 0xFFFF : (uint16_t)z->n); w16(z->f, big ? 0xFFFF : (uint16_t)z->n);
    w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)cd_size); w32(z->f, big ? 0xFFFFFFFFu : (uint32_t)cd_start); w16(z->f, 0);
    int rc = ferror(z->f) ? -1 : 0;
    if (fclose(z->f) != 0) rc = -1;
    for (size_t i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e); free(z);
    return rc;
}

/* ------------------------------------------------------------- reader --- */
typedef struct {
    unsigned char *map; size_t len;
    uint64_t cd_off, cd_size, count;
} Zr;

static int zr_open(const char *path, Zr *z, const char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "cannot open zip"; return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 22) { fclose(f); *err = "not a zip file"; return -1; }
    z->len = (size_t)sz;
    z->map = malloc(z->len);
    fseek(f, 0, SEEK_SET);
    if (fread(z->map, 1, z->len, f) != z->len) { fclose(f); free(z->map); *err = "cannot read zip"; return -1; }
    fclose(f);
    /* find EOCD (comment may follow) */
    long i;
    for (i = (long)z->len - 22; i >= 0 && i >= (long)z->len - 22 - 65535; i--)
        if (r32(z->map + i) == SIG_EOCD) break;
    if (i < 0 || r32(z->map + i) != SIG_EOCD) { free(z->map); *err = "zip directory not found"; return -1; }
    const unsigned char *eocd = z->map + i;
    z->count = r16(eocd + 10); z->cd_size = r32(eocd + 12); z->cd_off = r32(eocd + 16);
    if (z->count == 0xFFFF || z->cd_size == 0xFFFFFFFFu || z->cd_off == 0xFFFFFFFFu) {
        if (i >= 20 && r32(eocd - 20) == SIG_LOC64) {
            uint64_t off64 = r64(eocd - 20 + 8);
            if (off64 + 56 <= z->len && r32(z->map + off64) == SIG_EOCD64) {
                const unsigned char *e64 = z->map + off64;
                z->count = r64(e64 + 32); z->cd_size = r64(e64 + 40); z->cd_off = r64(e64 + 48);
            }
        }
    }
    if (z->cd_off + z->cd_size > z->len) { free(z->map); *err = "zip directory out of range"; return -1; }
    return 0;
}

typedef struct { const char *name; size_t nlen; uint64_t csize, usize, offset; uint16_t method, dtime, ddate; uint32_t mode, crc; } CE;

static const unsigned char *next_central(Zr *z, const unsigned char *p, CE *ce) {
    if (p + 46 > z->map + z->cd_off + z->cd_size || r32(p) != SIG_CENTRAL) return NULL;
    ce->method = r16(p + 10); ce->dtime = r16(p + 12); ce->ddate = r16(p + 14); ce->crc = r32(p + 16);
    ce->csize = r32(p + 20); ce->usize = r32(p + 24);
    ce->nlen = r16(p + 28);
    uint16_t xlen = r16(p + 30), clen = r16(p + 32);
    ce->mode = r32(p + 38) >> 16; ce->offset = r32(p + 42);
    ce->name = (const char *)p + 46;
    const unsigned char *x = p + 46 + ce->nlen, *xend = x + xlen;
    while (x + 4 <= xend) {
        uint16_t id = r16(x), sz = r16(x + 2);
        const unsigned char *d = x + 4;
        if (id == 1) {
            if (ce->usize == 0xFFFFFFFFu && d + 8 <= xend) { ce->usize = r64(d); d += 8; }
            if (ce->csize == 0xFFFFFFFFu && d + 8 <= xend) { ce->csize = r64(d); d += 8; }
            if (ce->offset == 0xFFFFFFFFu && d + 8 <= xend) { ce->offset = r64(d); }
        }
        x += 4 + sz;
    }
    return p + 46 + ce->nlen + xlen + clen;
}

static int mkdirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static bool safe_name(const char *n, size_t len) {
    if (!len || n[0] == '/' ) return false;
    if (memchr(n, 0, len)) return false;
    const char *p = n, *end = n + len;
    while (p < end) {
        const char *q = memchr(p, '/', (size_t)(end - p));
        if (!q) q = end;
        if (q - p == 2 && p[0] == '.' && p[1] == '.') return false;
        p = q + 1;
    }
    return true;
}

static int inflate_to_file(const unsigned char *src, uint64_t csize, uint64_t usize, uint16_t method, FILE *out, uint32_t want_crc) {
    uint32_t crc = 0;
    if (method == 0) {
        if (csize && fwrite(src, 1, csize, out) != csize) return -1;
        crc = (uint32_t)crc32(0, src, (uInt)csize);
    } else if (method == 8) {
        z_stream s = {0};
        if (inflateInit2(&s, -15) != Z_OK) return -1;
        s.next_in = (unsigned char *)src; s.avail_in = (uInt)csize;
        unsigned char buf[1 << 16];
        int rc;
        do {
            s.next_out = buf; s.avail_out = sizeof buf;
            rc = inflate(&s, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&s); return -1; }
            size_t got = sizeof buf - s.avail_out;
            crc = (uint32_t)crc32(crc, buf, (uInt)got);
            if (got && fwrite(buf, 1, got, out) != got) { inflateEnd(&s); return -1; }
        } while (rc != Z_STREAM_END);
        inflateEnd(&s);
        if (s.total_out != usize) return -1;
    } else return -1;
    return crc == want_crc ? 0 : -1;
}

int zip_extract(const char *zip, const char *dest, const char **err) {
    Zr z;
    if (zr_open(zip, &z, err) != 0) return -1;
    const unsigned char *p = z.map + z.cd_off;
    for (uint64_t i = 0; i < z.count; i++) {
        CE ce;
        p = next_central(&z, p, &ce);
        if (!p) { *err = "corrupt central directory"; free(z.map); return -1; }
        if (!safe_name(ce.name, ce.nlen)) { *err = "unsafe entry name in zip"; free(z.map); return -1; }
        char name[4096], path[8192];
        snprintf(name, sizeof name, "%.*s", (int)ce.nlen, ce.name);
        snprintf(path, sizeof path, "%s/%s", dest, name);
        if (name[ce.nlen - 1] == '/') { mkdirs(path); continue; }
        if (ce.offset + 30 > z.len || r32(z.map + ce.offset) != SIG_LOCAL) { *err = "corrupt local header"; free(z.map); return -1; }
        const unsigned char *lh = z.map + ce.offset;
        uint64_t data = ce.offset + 30 + r16(lh + 26) + r16(lh + 28);
        if (data + ce.csize > z.len) { *err = "entry data out of range"; free(z.map); return -1; }
        char *slash = strrchr(path, '/');
        if (slash) { *slash = 0; mkdirs(path); *slash = '/'; }
        FILE *out = fopen(path, "wb");
        if (!out) { *err = "cannot create extracted file"; free(z.map); return -1; }
        int rc = inflate_to_file(z.map + data, ce.csize, ce.usize, ce.method, out, ce.crc);
        fclose(out);
        if (rc != 0) { *err = "entry failed to inflate or checksum"; free(z.map); return -1; }
        chmod(path, (ce.mode & 0777) ? (ce.mode & 0777) : 0644);
        struct utimbuf ut = { time(NULL), from_dos(ce.dtime, ce.ddate) };
        utime(path, &ut);
    }
    free(z.map);
    return 0;
}

long zip_list(const char *zip, char ***names) {
    const char *err; Zr z;
    if (zr_open(zip, &z, &err) != 0) return -1;
    *names = calloc(z.count ? z.count : 1, sizeof **names);
    const unsigned char *p = z.map + z.cd_off;
    long n = 0;
    for (uint64_t i = 0; i < z.count; i++) {
        CE ce;
        p = next_central(&z, p, &ce);
        if (!p) break;
        (*names)[n] = malloc(ce.nlen + 1);
        memcpy((*names)[n], ce.name, ce.nlen); (*names)[n][ce.nlen] = 0; n++;
    }
    free(z.map);
    return n;
}
