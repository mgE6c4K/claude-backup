/* Shared paths, options and file helpers. */
#ifndef CB_COMMON_H
#define CB_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "json.h"

extern char HOME[], CLAUDE[], CLAUDE_JSON[], STORE_ROOT[], STAMP[];
extern const char *SKIP_DIRS[];        /* ~/.claude entries never backed up or snapshotted */
extern const char *CURRENT_MODELS[];
extern struct Opts { bool dry, yes; } opts;

void common_init(void);

void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void head(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
const char *tilde(const char *path);             /* static buffer */
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *path_join(const char *a, const char *b);   /* malloc'd */

bool in_list(const char *s, const char **list);
bool starts_with(const char *s, const char *pre);
bool ends_with(const char *s, const char *suf);
char *str_replace_all(const char *s, const char *from, const char *to);  /* malloc'd */
char *slug(const char *path);                                            /* malloc'd */
bool is_uuid(const char *s);
char *new_uuid(void);                                                     /* malloc'd "xxxxxxxx-..." */
char *title_key(const char *title, const char *source);                   /* malloc'd or NULL */
long long iso_ms(const char *iso);                                        /* -1 if unparsable */
long long now_ms(void);

bool is_dir(const char *p);
bool is_file(const char *p);
bool exists(const char *p);
bool is_link(const char *p);
int  mkdir_p(const char *p);
int  copy_file(const char *src, const char *dst);         /* keeps mode and mtime */
char *read_file(const char *p, size_t *len);               /* malloc'd, NUL-terminated; NULL on error */
int  write_file(const char *p, const char *data, size_t len, mode_t mode); /* atomic */
int  append_file(const char *p, const char *data);
JVal *read_json_file(const char *p);                       /* NULL if missing/invalid */
int  write_json_file(const char *p, const JVal *v, mode_t mode);
int  remove_tree(const char *p);

/* Sorted list of entry names in a directory (malloc'd). Returns count or -1. */
long list_dir(const char *dir, char ***names);
void free_names(char **names, long n);

/* Recursive walk: cb(path, rel, is_dir, ctx). Directories are reported before their contents.
   Returning nonzero from cb for a directory skips descending into it. */
typedef int (*walk_cb)(const char *path, const char *rel, bool isdir, void *ctx);
void walk(const char *root, walk_cb cb, void *ctx);

/* growable string list */
typedef struct { char **v; size_t n, cap; } StrList;
void sl_push(StrList *l, const char *s);        /* copies */
void sl_free(StrList *l);
bool sl_has(const StrList *l, const char *s);

/* growable string buffer */
typedef struct { char *s; size_t n, cap; } Buf;
void buf_add(Buf *b, const char *s, size_t len);
void buf_adds(Buf *b, const char *s);
void buf_free(Buf *b);

/* the Claude app */
bool app_running(void);
void confirm_app_quit(void);
char *store_dir(void);                           /* malloc'd or NULL */

typedef struct { JVal *rec; char *path; } Rec;
typedef struct { Rec *v; size_t n; long unreadable; } Records;
Records load_records(const char *store);
const Rec *find_record(const Records *r, const char *cli_id);
void free_records(Records *r);

void fix_ownership(void);

#endif
