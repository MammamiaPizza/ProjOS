# Enhanced Command History & Replay Tool

A POSIX-based command wrapper with **persistent history**, **advanced tracking**, and **8 major improvements** over the original design.

---

## 🚀 New Features Implemented

### 1. **Dynamic Memory Management**
- No fixed `MAX_HISTORY` limit
- History array grows automatically with `realloc()`
- Starts at 100 entries, doubles when full
- All strings dynamically allocated

### 2. **Better Command Parsing**
- Handles quoted strings: `echo "hello world"`
- Supports escaped characters: `echo hello\ world`
- Proper tokenizer with quote state tracking
- Single and double quote support

### 3. **Process Group Control**
- Each command runs in its own process group via `setpgid()`
- Better cleanup of child processes
- `SIGCHLD` handler prevents zombie processes
- Graceful `SIGINT` (Ctrl+C) handling

### 4. **CPU Time Tracking**
- Measures **wall-clock time** (real elapsed time)
- Tracks **CPU user time** (user-space execution)
- Tracks **CPU system time** (kernel-space execution)
- Uses `getrusage(RUSAGE_CHILDREN)` for accurate metrics
- Distinguishes I/O wait from actual CPU usage

### 5. **Environment & Context Tracking**
- Logs working directory for each command (`getcwd()`)
- Built-in `cd` command support
- `replay` automatically changes to original directory
- Directory shown in prompt (with `~` for home)

### 6. **Session Tracking**
- Records **PID** of each executed command
- Records **Session ID** of the tool instance
- Distinguish commands from different terminal sessions
- New `info` command shows session details

### 7. **Security Hardening**
- Command blacklist (blocks `rm -rf /`, fork bombs, etc.)
- Path resolution with `realpath()` prevents traversal attacks
- Validates command exists before execution
- Warnings for sensitive file access (`/etc/shadow`, `/etc/passwd`)

### 8. **Terminal Control (Line Editing)**
- **Arrow key navigation**: Up/Down for history recall
- **Backspace/Delete**: Proper line editing
- **Raw mode terminal**: Character-by-character input
- **Ctrl+C**: Graceful interrupt
- **Ctrl+D**: EOF handling
- Bash-like REPL experience

---

## 📦 System Calls Used

| Category | System Calls | Purpose |
|----------|--------------|---------|
| **Process Control** | `fork()`, `execvp()`, `waitpid()`, `setpgid()`, `getpgrp()` | Create and manage child processes |
| **Time Measurement** | `clock_gettime()`, `getrusage()`, `time()` | Wall-clock and CPU time tracking |
| **File I/O** | `open()`, `read()`, `write()`, `close()` | Low-level log file operations |
| **Memory** | `malloc()`, `realloc()`, `free()`, `strdup()` | Dynamic history array |
| **Signals** | `sigaction()`, `signal()` | SIGINT and SIGCHLD handling |
| **Terminal** | `tcgetattr()`, `tcsetattr()` | Raw mode for line editing |
| **Environment** | `getcwd()`, `chdir()`, `getenv()` | Working directory tracking |
| **Session** | `getpid()`, `getsid()`, `getppid()` | Process/session identification |
| **Security** | `realpath()`, `access()`, `stat()` | Path validation and command resolution |

---

## 🔧 Build & Run

```bash
# Compile with optimizations and all warnings
gcc -O2 -Wall -Wextra command_history_tool.c -o history_tool

# Run the tool
./history_tool
```

**Requirements:**
- Linux or POSIX-compliant OS
- GCC or compatible C compiler
- Standard POSIX libraries

---

## 📝 Usage

### Built-in Commands

| Command | Description |
|---------|-------------|
| `history` | Show all commands with full metadata |
| `replay <id>` | Re-run command #id (restores working directory) |
| `search <keyword>` | Find commands containing keyword |
| `stats` | Show statistics: most used, slowest, success rate |
| `cd <path>` | Change working directory |
| `info` | Show session info (PID, SID, capacity, directory) |
| `exit` / `quit` | Exit the tool |

### Terminal Shortcuts

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate command history |
| `Ctrl+C` | Interrupt current input |
| `Ctrl+D` | Exit (on empty line) |
| `Backspace` | Delete character |

### Examples

```bash
# Run commands
> ls -la
> echo "hello world"
> grep -r "pattern" .

# View history with metadata
> history

# Replay a command (automatically changes to original directory)
> replay 3

# Search for commands
> search grep

# View statistics
> stats

# Change directory
> cd /tmp
> pwd

# Session info
> info

# Exit
> exit
```

---

## 📊 Enhanced Log Format

The `history.log` file now includes additional fields:

```
id|timestamp|duration_ms|cpu_user_ms|cpu_sys_ms|exit_code|pid|sid|cwd|command
```

**Example:**
```
1|1727764212|45.23|12.45|3.21|0|12345|12345|/home/user|ls -la
2|1727764225|1200.50|15.30|2.10|1|12346|12345|/home/user|cat nofile.txt
3|1727764240|12.00|2.50|0.80|0|12347|12345|/tmp|echo hello
```

**Fields:**
- `id`: Sequential command ID
- `timestamp`: Unix epoch time
- `duration_ms`: Wall-clock time (milliseconds)
- `cpu_user_ms`: CPU time in user space (NEW)
- `cpu_sys_ms`: CPU time in kernel space (NEW)
- `exit_code`: Command exit status
- `pid`: Process ID of executed command (NEW)
- `sid`: Session ID of tool instance (NEW)
- `cwd`: Working directory when executed (NEW)
- `command`: Full command line

---

## 🧪 Testing

### Test Cases

| Test | Command | Expected Result |
|------|---------|-----------------|
| Valid command | `echo hello` | Output shown, exit=0, CPU time logged |
| Invalid command | `foobar123` | "command not found", exit=127 |
| Quoted arguments | `echo "hello world"` | Parses correctly, outputs with space |
| Escaped characters | `echo hello\ world` | Parses correctly |
| Directory tracking | `cd /tmp` then `pwd` | Logs `/tmp` as cwd |
| Replay with cd | `replay 5` (ran in /tmp) | Changes to /tmp, runs, changes back |
| Security blacklist | `rm -rf /` | Blocked with security warning |
| Arrow key history | Press `↑` | Previous command loaded |
| Process groups | Run `sleep 10`, press Ctrl+C | Child process terminated |
| Dynamic growth | Run 150+ commands | History capacity grows automatically |
| Session tracking | Run `info` | Shows PID and Session ID |

---

## 📈 Performance Metrics

The tool now tracks three types of time:

1. **Wall-clock time** (`duration_ms`)
   - Real elapsed time from start to finish
   - Includes I/O wait, sleep, etc.

2. **CPU user time** (`cpu_user_ms`)
   - Time spent executing user-space code
   - Excludes kernel calls and I/O wait

3. **CPU system time** (`cpu_sys_ms`)
   - Time spent in kernel-space (system calls)
   - Includes file I/O, process management

**Example Analysis:**
```
Command: grep -r "pattern" /large/directory
Wall: 5000ms | CPU User: 300ms | CPU Sys: 200ms
→ Mostly I/O bound (4500ms waiting for disk)
```

---

## 🔒 Security Features

### Command Blacklist
Automatically blocks dangerous patterns:
- `rm -rf /`
- `rm -rf /*`
- Fork bombs: `:(){ :|:& };:`
- `dd if=/dev/zero of=/dev/sda`
- `mkfs`

### Path Validation
- Resolves symlinks with `realpath()`
- Checks execute permission with `access(X_OK)`
- Prevents path traversal attacks

### Warnings
- Alerts when accessing sensitive files (`/etc/shadow`, `/etc/passwd`)

---

## 🛠️ Architecture Improvements

### Original Design
```
Fixed array (MAX_HISTORY=100)
Simple whitespace parsing
No process groups
Wall-clock time only
No directory tracking
No session info
No security checks
Basic line input
```

### Enhanced Design
```
Dynamic array (realloc, no limit)
Quote-aware parser
Process groups (setpgid)
Wall + CPU time (getrusage)
Directory tracking (getcwd/chdir)
Session tracking (getpid/getsid)
Blacklist + path validation
Raw mode terminal (arrow keys)
```

---

## 📚 Code Structure

```
command_history_tool.c
├── Data Structures
│   └── HistoryEntry (extended with CPU time, PID, SID, CWD)
├── Dynamic Memory
│   ├── init_history()
│   ├── grow_history()
│   └── free_history()
├── File I/O
│   ├── load_history_from_file()
│   └── save_entry_to_file()
├── Command Parsing
│   ├── parse_command() (quote-aware)
│   └── free_args()
├── Security
│   ├── validate_command() (blacklist)
│   └── resolve_command_path() (realpath)
├── Execution
│   └── execute_command() (process groups, CPU time)
├── Built-in Commands
│   ├── cmd_history()
│   ├── cmd_replay()
│   ├── cmd_search()
│   ├── cmd_stats()
│   ├── cmd_cd()
│   └── cmd_info()
├── Terminal Control
│   ├── enable_raw_mode()
│   ├── disable_raw_mode()
│   └── read_line_with_history() (arrow keys)
├── Signal Handling
│   ├── setup_signal_handlers()
│   ├── sigint_handler()
│   └── sigchld_handler()
└── main()
```

---

## 🎓 Learning Outcomes

This project demonstrates:

1. **Process Management**: `fork()`, `execvp()`, `waitpid()`, process groups
2. **System Calls**: Low-level POSIX API usage
3. **Memory Management**: Dynamic allocation, `realloc()`, avoiding leaks
4. **Signal Handling**: `sigaction()`, asynchronous events
5. **Terminal Control**: Raw mode, `termios`, line editing
6. **File I/O**: Low-level `open()`/`read()`/`write()`
7. **Time Measurement**: `clock_gettime()`, `getrusage()`, CPU vs wall time
8. **Security**: Path validation, command filtering
9. **Parsing**: Tokenization, quote handling, state machines
10. **Environment**: Working directory, environment variables

---

## 🔄 Future Extensions

- **I/O Redirection**: `dup2()` for `>`, `<`, `>>`
- **Pipes**: `pipe()` for command pipelines
- **Background Jobs**: `&` support with job control
- **Command Tags**: Manual tagging (`#deploy`, `#debug`)
- **Range Replay**: `replay 5-10`
- **Export CSV**: Statistics export for analysis
- **Performance Alerts**: Warn when duration >> average
- **Multiple Sessions**: Concurrent session support

---

## 📄 License

This is a course project for Operating Systems. Feel free to use and modify.

---

## 👨‍💻 Author

Enhanced implementation with 8 major improvements over the original design document.

---

**Build Date:** 2026-10-05  
**Version:** 2.0 (Enhanced)  
**Lines of Code:** ~900
