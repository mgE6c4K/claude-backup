# claude-backup

Back up, restore and re-register local Claude Code data on macOS. A single C
binary with no runtime dependencies beyond the system zlib.

**What it covers**

- `~/.claude` — chats (`projects/`), per-project memory, rewind data
  (`file-history/`), session env, skills, plugins, `settings.json`,
  `settings.local.json`, `CLAUDE.md`, prompt history
- `~/.claude.json` — project list, repo paths, skill usage, onboarding flag
- `~/Library/Application Support/Claude/claude-code-sessions` — the desktop
  app's session records (what the Code tab lists, with titles)
- Skipped: caches, telemetry, shell snapshots, IDE locks, daemon state

Never run with `sudo`: files created as root are unreadable to Claude Code and
the Claude app (the tool refuses to start as root and can repair the damage).

## Build

```bash
make            # ./claude-backup for this Mac
make dist       # universal binary (Apple Silicon + Intel), stripped
make install    # copies to ~/.local/bin
```

## Commands

| Command | What it does |
|---|---|
| `backup [-p DIR]` | One zip at `DIR/ClaudeBackups/<host>_<user>_<timestamp>.zip` with a manifest. Default `DIR` is `.`. Prints the restore command. |
| `restore [-p PATH ...]` | Adds everything from a backup, never overwrites. `PATH` is a zip, a folder of zips (newest used), or a folder holding `.claude`. Repeatable. Default `.`. Then registers chats. |
| `register [--latest\|--all]` | Writes an app record for every chat that has none so it appears in the Code tab. `--latest` (default): one per title, newest transcript. `--all`: every transcript. |
| `dedupe` | Archives older app records that share a title with a newer one. |
| `fix-ownership` | Hands root-owned files from an earlier `sudo` run back to you (one password prompt). |

Global flags: `--dry-run` previews any command without changing anything;
`--yes` skips the "Claude app is running" prompt. `claude-backup COMMAND -h`
explains one command.

```bash
./claude-backup backup -p "/Volumes/Transfer Drive"
./claude-backup restore -p "/Volumes/Transfer Drive/ClaudeBackups"   # newest zip
./claude-backup restore -p old-mac-A -p old-mac-B --register all
./claude-backup --dry-run register --all
```

## How registration works

The Claude app lists only chats that have a record file in its session store.
`register` builds one from each transcript in `~/.claude/projects` (title,
folder, timestamps, model). Clearing a chat in the app starts a new transcript
under the same title, so by default only the most recent transcript per title
is registered; `--all` registers every one and `dedupe` archives the extras.

## Restoring from another machine

Old project paths are relocated with the table in `src/restore.c`
(`DEFAULT_MAP`), applied only when the new folder exists. Add rows with
`--map OLD=NEW`, or pass `--no-remap`. Transcripts, memory files, history, plugin
manifests and the project list are rewritten to match, so the app finds each
chat under its new folder. Settings are merged (permission lists unioned, your
current values win, hooks whose command is not installed are dropped),
`CLAUDE.md` content that is not already present is appended, memory indexes are
merged line by line. Restores are idempotent: a second run adds nothing.

## Layout

```
src/main.c      command line, help
src/common.c    paths, options, file helpers, app store access, ownership repair
src/json.c      small JSON parser / serializer
src/zip.c       zip writer (deflate, ZIP64) and extractor
src/backup.c    backup
src/restore.c   restore, path relocation, config merges
src/register.c  register (latest / all) and dedupe
```
