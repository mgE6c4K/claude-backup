#ifndef CB_COMMANDS_H
#define CB_COMMANDS_H
#include <stdbool.h>

typedef struct {
    const char *path;            /* backup: destination folder */
    const char **paths; int npaths;   /* restore: sources */
    const char **maps; int nmaps;     /* restore: OLD=NEW */
    bool no_remap;
    const char *reg;             /* restore: "latest" | "all" | "none" */
    bool latest;                 /* register */
    bool include_missing, include_deleted, archived;
} Args;

void cmd_backup(const Args *a);
void cmd_restore(const Args *a);
void cmd_register(const Args *a, bool app_checked);
void cmd_dedupe(const Args *a);

#endif
