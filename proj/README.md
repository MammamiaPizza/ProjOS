# Command History & Replay Tool (v2)

A terminal **flight recorder**. It runs your commands through POSIX system calls and records
*how each one behaved* — wall time, CPU time, peak memory, exit code — then lets you search,
replay, retry, time-limit and analyse them.

Implements **Tier A + B + C** of `command_history_replay_proposal.md`, plus a hand-rolled
`termios` line editor and a `linux` / `shell` execution-mode toggle.

Single C file, no external libraries: `command_history_tool.c` (1799 lines).

---

## Build & run

**This is POSIX-only code.** It uses `fork`, `execvp`, `wait4`, `setpgid`, `pipe`, `dup2`,
`termios` and `alarm`. It **cannot** be built or run on native Windows / MinGW / PowerShell.
On a Windows machine you must cross into WSL first.

### From Windows PowerShell

```powershell
wsl -d Ubuntu
cd /mnt/d/Code/OS_Tet/proj
make
./history_tool
```

If `wsl` drops you into a distro with no shell (e.g. `docker-desktop`), always pass `-d Ubuntu`.

### Directly, without make

```bash
gcc -O2 -Wall -Wextra -std=c99 command_history_tool.c -o history_tool
./history_tool
```

Builds with **zero warnings** under `-Wall -Wextra`.

### Make targets

| Target | What it does |
|---|---|
| `make` | Compile to `./history_tool` |
| `make run` | Compile and start the tool |
| `make test` | Non-interactive smoke test (see its limitation below) |
| `make clean` | Remove the binary, `history.log`, `history.csv`, `out.txt` |

> `make test` pipes its input, so the tool detects `!isatty()` and reads with `fgets()`
> instead of the line editor. That is deliberate — it is what makes the tool scriptable —
> but it means **`make test` cannot exercise the arrow keys, Tab completion or Ctrl+C**.
> Test those in a real terminal.

---

## Commands

| Command | Description |
|---|---|
| `<command>` | Run it and record how it behaved |
| `cmd1 \| cmd2` | Pipeline (linux mode) |
| `cmd > f`, `cmd >> f`, `cmd < f` | Redirection (linux mode) |
| `!!` | Rerun the previous command |
| `cd <dir>` | Change directory — built-in, because a child's `chdir()` dies with the child |
| `pwd` | Print the working directory |
| `history [--failed\|--slow]` | List recorded commands, optionally filtered |
| `replay <id>` | Rerun a command and diff it against its previous run |
| `retry <n> <cmd>` | Run up to *n* times, stopping at the first success |
| `timeout <sec> <cmd>` | Kill the command if it exceeds *sec* seconds |
| `search <keyword>` | Find commands containing a keyword |
| `stats` | Usage, slowest, heaviest, failure rate, flaky commands |
| `export <file.csv>` | Write the whole history as CSV |
| `mode [linux\|shell]` | Show or switch execution mode |
| `help` | Command list |
| `exit` | Quit |

Every built-in is reachable with **no argument** — a bare `replay` prints its usage rather than
trying to `execvp()` a program called `replay`.

### Line editing

| Key | Action |
|---|---|
| Up / Down | Recall previous and next commands |
| Left / Right | Move the cursor inside the line |
| Home / End | Jump to line start or end (also Ctrl-A / Ctrl-E) |
| Tab | Complete a command name (first word) or a filename (later words) |
| Backspace / Delete | Erase behind or ahead of the cursor |
| Ctrl-K | Delete to end of line |
| Ctrl-L | Clear the screen and redraw |
| Ctrl-C | Abandon the current line at the prompt; kill the running command during execution |
| Ctrl-D | Exit on an empty line |

Tab completion is hand-rolled, not GNU readline: `opendir`/`readdir` for filenames, a `PATH` scan
with `access(X_OK)` for command names, longest-common-prefix insertion, and a capped candidate
list (`LIST_MAX`) because on WSL the inherited `PATH` contains all of System32.

---

## Execution modes

| Mode | What runs the line |
|---|---|
| `linux` (default) | **This tool** parses the line and calls `fork` / `execvp` / `pipe` / `dup2` itself |
| `shell` | The line is handed to `sh -c`, so globs and `$VARS` expand |

```
[linux] ~/proj> echo *.c
*.c                                  <- literal, no globbing: the tool does not expand
[linux] ~/proj> mode shell
[shell] ~/proj> echo *.c
command_history_tool.c               <- sh expanded the glob
[shell] ~/proj> echo $HOME
/home/pizza
```

Shell mode is implemented as a **degenerate one-stage pipeline** (`sh -c <line>`), so timing,
`wait4` resource collection, signal forwarding, timeout and logging are the *same* code path in
both modes. Only the argv differs.

Use `linux` mode for the demo: it is the mode that actually demonstrates the system calls.

---

## Log file format

`history.log`, plain text, one entry per line, `|`-separated, with the command **last** so that it
may itself contain `|`:

```
id|timestamp|wall_ms|exit_code|user_ms|sys_ms|max_rss_kb|command
1|1791429312|4.94|0|0.00|3.78|7236|echo hello
3|1791429313|8.33|0|7.83|2.52|8016|ls | wc -l
```

Line 3 has **9** `|`-separated fields, not 8. That is correct and intentional: the parser splits on
the first seven `|` and treats the remainder as the verbatim command, which is what lets pipelines
survive a save/load round-trip.

| Field | Source |
|---|---|
| `timestamp` | `time(NULL)` |
| `wall_ms` | `clock_gettime(CLOCK_MONOTONIC)` delta |
| `exit_code` | `WEXITSTATUS`; `124` for timeout, `127` for exec failure, `130` for Ctrl+C |
| `user_ms`, `sys_ms`, `max_rss_kb` | `struct rusage` from `wait4()` |
| `command` | The raw command line |

**The log path is resolved to an absolute path once at startup** (`log_path_init()`), before any
command can run. A relative `history.log` would silently start appending to a different file in
every directory you `cd` into, scattering and effectively losing history.

`export` is different: it writes to the path you give it, relative to the current directory, which
is the shell-like behaviour you would expect from an explicit filename.

---

## System calls used

| System call | Header | Purpose here |
|---|---|---|
| `fork()` | `<unistd.h>` | One child per pipeline stage |
| `execvp()` | `<unistd.h>` | Replace the child with the requested program |
| `wait4()` | `<sys/wait.h>`, `<sys/resource.h>` | Wait for a child **and** get its `rusage` |
| `pipe()` | `<unistd.h>` | Connect pipeline stages |
| `dup2()` | `<unistd.h>` | Attach pipe ends or files to stdin/stdout |
| `open()` / `read()` / `write()` / `close()` | `<fcntl.h>`, `<unistd.h>` | Log I/O and redirection |
| `chdir()` / `getcwd()` | `<unistd.h>` | The `cd` and `pwd` built-ins, and the prompt |
| `sigaction()` | `<signal.h>` | Install the parent's `SIGINT` and `SIGALRM` handlers |
| `signal()` | `<signal.h>` | Reset `SIGINT`/`SIGALRM`/`SIGQUIT`/`SIGTERM` to `SIG_DFL` in children |
| `kill()` | `<signal.h>` | Signal the whole process group |
| `alarm()` | `<unistd.h>` | Arm the timeout |
| `setpgid()` | `<unistd.h>` | Give each pipeline its own process group |
| `_exit()` | `<unistd.h>` | Leave a failed child without running atexit handlers |
| `clock_gettime()` | `<time.h>` | Wall-clock timing |
| `time()` | `<time.h>` | Log timestamps |
| `tcgetattr()` / `tcsetattr()` | `<termios.h>` | Raw mode for the line editor |
| `select()` | `<sys/select.h>` | Timed read, to tell a bare Escape from an arrow sequence |
| `isatty()` | `<unistd.h>` | Fall back to `fgets()` when stdin is a pipe |
| `opendir()` / `readdir()` / `closedir()` | `<dirent.h>` | Filename tab completion |
| `access()` | `<unistd.h>` | Executable check during `PATH` completion |
| `stat()` | `<sys/stat.h>` | Mark completed directories with a trailing `/` |

Not used: `waitpid`, `execlp`, `ioctl`, `getrusage`. `wait4()` supersedes the first and last.

---

## Design notes

**Why `cd` must be a built-in.** `cd` changes the working directory of the *calling* process. Run
it in a forked child and the directory change vanishes when the child exits.

**Pipelines.** For `a | b`: create a pipe, fork per stage, and in each child `dup2()` the pipe end
onto stdout (left side) or stdin (right side). Every process closes every unused pipe end, or the
reader never sees EOF and the tool hangs.

**Exit code of a pipeline** is the **last** stage's status, matching shell convention. CPU times are
summed across stages; peak memory is the maximum.

**Ctrl+C handling.** Children get their own process group via `setpgid()`. Both parent and child
call it — the child's call closes the race where a signal arrives between `fork()` and the parent's
`setpgid()`. The parent's `SIGINT` handler forwards the signal to `-pgid`, so Ctrl+C kills the
running command and not the tool. `g_pgid` is cleared back to 0 after the wait loop: a stale group
id would let a later Ctrl+C signal an unrelated process group that reused the number.

**Signal handlers are async-signal-safe.** They only set a `volatile sig_atomic_t` flag and call
`kill()`. No `printf` — that is not safe inside a handler.

**Timeout.** The parent calls `alarm(n)` before waiting. If `SIGALRM` fires, the handler sets
`g_timed_out` and calls `kill(-pgid, SIGKILL)`. The run is logged with exit code `124`.

**Redirect vs pipe precedence.** File redirections are applied *after* the pipe wiring, so
`ls | grep x > out` writes to `out` rather than to the pipe — the same precedence a real shell uses.

**Replay diff.** `replay <id>` snapshots the old entry into a local copy *before* running, because
`hist_record()` may `realloc()` the history array and invalidate any pointer into it. It then
prints the change in exit code, wall time (with a percentage delta), CPU time and peak memory.

**Flaky detection.** A base command is flagged flaky if its history contains both exit code `0` and
non-zero results. `stats` reports passed/failed counts for each.

**Quoting.** The tokenizer handles single quotes (literal), double quotes (allowing `\"`, `\\`,
`\$`, `` \` ``) and backslash escapes outside quotes. It also splits operators written without
spaces, so `echo hi>out.txt` parses the same as `echo hi > out.txt`.

---

## Demo script

1. `ls`, `echo hello` → show the per-command summary line (exit, wall, CPU, memory)
2. `ls | wc -l` and `echo hi > out.txt` → Tier A works like a real shell
3. `foobar123` → "command not found", exit `127` logged
4. `sleep 30` then Ctrl+C → the command dies, the tool survives, exit `130` logged
5. `timeout 2 sleep 30` → killed at ~2 s, logged as exit `124`
6. `timeout 2 sh -c "sleep 30 & sleep 30 & wait"` → the **whole process group** dies
7. Compile something heavy → show real CPU time and peak RSS in the log
8. `replay <id>` → the diff against the previous run
9. `retry 3 false` → three attempts, then a failure report
10. `stats` → most used, slowest, heaviest, failure rate, flaky warnings
11. `mode shell` → `echo *.c` now expands; `mode linux` → it does not
12. Up / Down arrows, `←` `→`, Home / End, Tab, Ctrl-K, Ctrl-L
13. `export report.csv` → open it in a spreadsheet
14. `exit`, restart → history is reloaded from `history.log`

---

## Verified behaviour

All of the following were tested on WSL Ubuntu against a clean `-Wall -Wextra` build:

| Case | Result |
|---|---|
| `echo hello` | exit `0` logged with CPU and RSS |
| `foobar123` | "command not found", exit `127` |
| `cd /tmp` then `pwd` | Directory really changed; prompt followed |
| `ls \| wc -l` | Correct count, no hang, stored intact with its `\|` |
| `echo hi > out.txt` then `cat out.txt` | File contained `hi` |
| `cd` + logging | All entries stayed in the project's `history.log`; no stray `/tmp/history.log` |
| `sleep 30` + Ctrl+C | Command killed (exit `130`), tool survived |
| Ctrl+C on a pipeline | Both stages killed |
| `timeout 2 …` with background children | Whole process group killed; no survivors |
| `retry 3 false` | 3 attempts, then failure report |
| `replay` valid / invalid / bare | Diff printed / graceful error / usage message |
| Bare `search`, `retry`, `timeout`, `history` | Usage messages, never `execvp`'d |
| Restart | Previous history reloaded |
| Empty input | Prompt returned, nothing logged |
| `!!` | Expanded to the previous command and ran it |
| Flaky detection | Alternating-exit command flagged with passed/failed counts |
| `mode` toggle | Shell mode expanded `*.c` and `$HOME`; linux mode did not |
| CSV export | Every row parsed by Python's `csv` with 9 fields |
| Up / Down / Left / Right / Home / End / Delete | Verified over a real pty |
| Ctrl-A / Ctrl-E / Ctrl-K / Ctrl-L / Ctrl-C / Ctrl-D | Verified over a real pty |
| Tab completion | `ech⇥` → `echo`; `cat Make⇥` → `cat Makefile` |

---

## Known limitations

- **No globbing or variable expansion in linux mode.** That is what `mode shell` is for.
- **No shell scripting**: no `if` / `for`, no variables, no `&&` / `||`, no `;` chaining. A `;`
  inside a quoted string is fine and is passed through to the program.
- **`timeout` logs the command it ran, not the wrapper.** `timeout 2 sleep 10` is recorded as
  `sleep 10`, so `replay`ing that entry reruns it *without* a time limit.
- **Multi-line commands are not supported**; one line is one command.
- **The editor uses a 50 ms `select()` timeout** to tell a bare Escape from an arrow sequence, so a
  lone Escape press has a barely perceptible delay.

---

## Files

```
proj/
├── command_history_tool.c   the whole tool
├── Makefile                 all / run / test / clean
├── README.md                this file
├── history.log              created at runtime (append-only, absolute-anchored)
└── history.csv              created by 'export'
```

The v1 implementation is preserved in `D:\Code\OSProj\` (source, docs and a compiled
`history_tool_v1`) as a working fallback.
