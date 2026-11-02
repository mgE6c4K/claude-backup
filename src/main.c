/* claude-backup: back up, restore and re-register local Claude Code data. */
#include "commands.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "1.0.0"

static const struct { const char *name, *desc, *detail, *usage; } CMDS[] = {
    { "backup", "zip local Claude data into PATH/ClaudeBackups/<host>_<user>_<timestamp>.zip",
      "Backs up ~/.claude (chats, memory, rewind data, skills, plugins, settings), ~/.claude.json\n"
      "and the Claude app's session records (titles, folders) into one zip. Caches and machine\n"
      "state are left out. PATH is the folder that receives ClaudeBackups/ (default: .).",
      "  -p, --path PATH   folder that receives ClaudeBackups/ (default: .)" },
    { "restore", "add everything from a backup into this account, then register its chats with the app",
      "Never overwrites what is already here; safe to run again. PATH is a backup zip, a folder\n"
      "holding zips (newest is used), or a folder holding a .claude directory (default: .).\n"
      "Give -p more than once to restore several backups. Chats whose old folder has a known new\n"
      "location on this Mac are relocated; add --map OLD=NEW for others. Root-owned files left by\n"
      "an earlier sudo run are handed back to you first (asks for your password).",
      "  -p, --path PATH          backup zip, folder of zips, or folder holding .claude (repeatable)\n"
      "      --map OLD=NEW        relocate chats from OLD path prefix to NEW (repeatable)\n"
      "      --no-remap           keep chats under their original paths\n"
      "      --register MODE      latest (default), all, or none\n"
      "      --include-missing-folders   register chats whose folder does not exist here\n"
      "      --include-deleted    register chats you had deleted in the app\n"
      "      --archived           register restored chats as archived" },
    { "register", "write an app session record for every chat that has none, so it shows in the Code tab",
      "The Claude app lists only chats it has a record for. By default one record is written per\n"
      "chat title, for the most recent transcript: clearing a chat in the app starts a new\n"
      "transcript under the same title, and the older ones would otherwise show as duplicates.\n"
      "--all registers every transcript instead.",
      "      --latest             one record per chat title, the newest transcript (default)\n"
      "      --all                a record for every transcript\n"
      "      --include-missing-folders   register chats whose folder does not exist here\n"
      "      --include-deleted    register chats you had deleted in the app\n"
      "      --archived           register as archived" },
    { "dedupe", "archive older app records that share a title with a newer one",
      "Keeps the most recently active record of each title and archives the rest. Use it after\n"
      "`register --all`, or when clears in the app have piled up duplicates.", "" },
    { "fix-ownership", "give Claude files owned by root (from a sudo run) back to you",
      "Checks ~/.claude, ~/.claude.json and the app's session store. Files owned by root are\n"
      "unreadable to Claude Code and the Claude app. Runs sudo chown on what it finds.", "" },
    { NULL, NULL, NULL, NULL }
};

static void usage(void) {
    printf("usage: claude-backup [--dry-run] [--yes] COMMAND [options]\n\n"
           "Back up, restore and re-register local Claude Code data.\n\n"
           "commands:\n");
    for (int i = 0; CMDS[i].name; i++) printf("  %-14s %s\n", CMDS[i].name, CMDS[i].desc);
    printf("\noptions:\n"
           "  --dry-run        show what would happen, change nothing\n"
           "  --yes            do not ask about the running Claude app\n"
           "  -h, --help       this help; `claude-backup COMMAND -h` for one command\n"
           "  --version\n\n"
           "examples:\n"
           "  claude-backup backup                       zip into ./ClaudeBackups/\n"
           "  claude-backup backup -p /Volumes/Drive      zip into /Volumes/Drive/ClaudeBackups/\n"
           "  claude-backup restore -p backup.zip         restore one zip, register latest chats\n"
           "  claude-backup restore -p A -p B --register all\n"
           "  claude-backup --dry-run register --all     preview only\n\n"
           "Never run with sudo. Quit the Claude app before restore, register or dedupe.\n");
}

static void cmd_usage(int i) {
    printf("usage: claude-backup %s [options]\n\n%s.\n\n%s\n", CMDS[i].name, CMDS[i].desc, CMDS[i].detail);
    if (CMDS[i].usage[0]) printf("\noptions:\n%s\n", CMDS[i].usage);
}

static const char *need_value(int argc, char **argv, int *i, const char *flag) {
    const char *eq = strchr(argv[*i], '=');
    if (eq && argv[*i][1] == '-') return eq + 1;
    if (*i + 1 >= argc) die("%s needs a value", flag);
    return argv[++*i];
}

int main(int argc, char **argv) {
    if (geteuid() == 0) die("Do not run this with sudo: files it creates would belong to root and Claude could not read them.");
    common_init();
    Args a = { .path = ".", .reg = "latest", .latest = true };
    const char **paths = calloc((size_t)argc, sizeof *paths), **maps = calloc((size_t)argc, sizeof *maps);
    a.paths = paths; a.maps = maps;
    int cmd = -1;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (cmd < 0) {
            if (!strcmp(s, "--dry-run")) opts.dry = true;
            else if (!strcmp(s, "--yes") || !strcmp(s, "-y")) opts.yes = true;
            else if (!strcmp(s, "-h") || !strcmp(s, "--help")) { usage(); return 0; }
            else if (!strcmp(s, "--version")) { printf("claude-backup %s\n", VERSION); return 0; }
            else if (s[0] == '-') die("unknown option: %s (try --help)", s);
            else { for (int c = 0; CMDS[c].name; c++) if (!strcmp(s, CMDS[c].name)) cmd = c; if (cmd < 0) die("unknown command: %s (try --help)", s); }
            continue;
        }
        if (!strcmp(s, "-h") || !strcmp(s, "--help")) { cmd_usage(cmd); return 0; }
        if (!strcmp(s, "--dry-run")) { opts.dry = true; continue; }
        if (!strcmp(s, "--yes") || !strcmp(s, "-y")) { opts.yes = true; continue; }
        const char *name = CMDS[cmd].name;
        if ((!strcmp(s, "-p") || !strcmp(s, "--path") || starts_with(s, "--path=")) && (!strcmp(name, "backup") || !strcmp(name, "restore"))) {
            const char *v = need_value(argc, argv, &i, "--path");
            if (!strcmp(name, "backup")) a.path = v; else paths[a.npaths++] = v;
        }
        else if ((!strcmp(s, "--map") || starts_with(s, "--map=")) && !strcmp(name, "restore")) maps[a.nmaps++] = need_value(argc, argv, &i, "--map");
        else if (!strcmp(s, "--no-remap") && !strcmp(name, "restore")) a.no_remap = true;
        else if ((!strcmp(s, "--register") || starts_with(s, "--register=")) && !strcmp(name, "restore")) {
            a.reg = need_value(argc, argv, &i, "--register");
            if (strcmp(a.reg, "latest") && strcmp(a.reg, "all") && strcmp(a.reg, "none")) die("--register must be latest, all or none");
        }
        else if (!strcmp(s, "--latest") && !strcmp(name, "register")) a.latest = true;
        else if (!strcmp(s, "--all") && !strcmp(name, "register")) a.latest = false;
        else if (!strcmp(s, "--include-missing-folders") && (!strcmp(name, "register") || !strcmp(name, "restore"))) a.include_missing = true;
        else if (!strcmp(s, "--include-deleted") && (!strcmp(name, "register") || !strcmp(name, "restore"))) a.include_deleted = true;
        else if (!strcmp(s, "--archived") && (!strcmp(name, "register") || !strcmp(name, "restore"))) a.archived = true;
        else die("unknown option for %s: %s (try `claude-backup %s -h`)", name, s, name);
    }
    if (cmd < 0) { usage(); return 0; }
    if (opts.dry) logf_("[dry-run] nothing will be changed");
    const char *name = CMDS[cmd].name;
    if (!strcmp(name, "backup")) cmd_backup(&a);
    else if (!strcmp(name, "restore")) cmd_restore(&a);
    else if (!strcmp(name, "register")) cmd_register(&a, false);
    else if (!strcmp(name, "dedupe")) cmd_dedupe(&a);
    else fix_ownership();
    free(paths); free(maps);
    return 0;
}
