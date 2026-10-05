#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <ctype.h>
#include <limits.h>

// ============================================================================
// ENHANCED COMMAND HISTORY & REPLAY TOOL
// Features: Dynamic Memory, Better Parsing, Process Groups, CPU Time,
//           Environment Context, Session Tracking, Security, Terminal Control
// ============================================================================

#define LOG_FILE "history.log"
#define MAX_CMD_LEN 4096
#define MAX_ARGS 256
#define INITIAL_HISTORY_SIZE 100
#define HISTORY_GROWTH_FACTOR 2

// Blacklisted dangerous commands (Security Hardening)
const char *BLACKLIST[] = {
    "rm -rf /",
    "rm -rf /*",
    ":(){ :|:& };:",
    "dd if=/dev/zero of=/dev/sda",
    "mkfs",
    NULL
};

// History entry structure with extended fields
typedef struct {
    int id;
    time_t timestamp;
    double duration_ms;        // Wall clock time
    double cpu_user_ms;        // CPU user time (NEW)
    double cpu_sys_ms;         // CPU system time (NEW)
    int exit_code;
    pid_t pid;                 // Process ID (NEW - Session Tracking)
    pid_t sid;                 // Session ID (NEW - Session Tracking)
    char *command;             // Dynamically allocated (NEW - Dynamic Memory)
    char *cwd;                 // Working directory (NEW - Environment & Context)
} HistoryEntry;

// Dynamic history array (NEW - Dynamic Memory)
HistoryEntry *history = NULL;
int history_count = 0;
int history_capacity = 0;

// Terminal settings for raw mode (NEW - Terminal Control)
struct termios orig_termios;
int terminal_raw_mode = 0;

// Current session info (NEW - Session Tracking)
pid_t session_pid;
pid_t session_sid;

// Signal handler flag
volatile sig_atomic_t got_sigint = 0;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

void init_history(void);
void grow_history(void);
void free_history(void);
void load_history_from_file(void);
void save_entry_to_file(HistoryEntry *entry);
void add_to_history(const char *cmd, double duration_ms, double cpu_user_ms, 
                    double cpu_sys_ms, int exit_code, pid_t pid, const char *cwd);

char **parse_command(const char *cmd);
void free_args(char **args);
int validate_command(const char *cmd);
char *resolve_command_path(const char *cmd);

void execute_command(const char *cmd);
void cmd_history(void);
void cmd_replay(int id);
void cmd_search(const char *keyword);
void cmd_stats(void);
void cmd_cd(const char *path);
void cmd_info(void);

void enable_raw_mode(void);
void disable_raw_mode(void);
char *read_line_with_history(void);

void setup_signal_handlers(void);
void sigint_handler(int sig);
void sigchld_handler(int sig);

// ============================================================================
// DYNAMIC MEMORY MANAGEMENT
// ============================================================================

void init_history(void) {
    history_capacity = INITIAL_HISTORY_SIZE;
    history = malloc(history_capacity * sizeof(HistoryEntry));
    if (!history) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    history_count = 0;
}

void grow_history(void) {
    int new_capacity = history_capacity * HISTORY_GROWTH_FACTOR;
    HistoryEntry *new_history = realloc(history, new_capacity * sizeof(HistoryEntry));
    if (!new_history) {
        perror("realloc");
        return;
    }
    history = new_history;
    history_capacity = new_capacity;
    printf("[Info] History capacity grown to %d entries\n", new_capacity);
}

void free_history(void) {
    for (int i = 0; i < history_count; i++) {
        free(history[i].command);
        free(history[i].cwd);
    }
    free(history);
}

void add_to_history(const char *cmd, double duration_ms, double cpu_user_ms,
                    double cpu_sys_ms, int exit_code, pid_t pid, const char *cwd) {
    if (history_count >= history_capacity) {
        grow_history();
    }
    
    HistoryEntry *entry = &history[history_count];
    entry->id = history_count + 1;
    entry->timestamp = time(NULL);
    entry->duration_ms = duration_ms;
    entry->cpu_user_ms = cpu_user_ms;
    entry->cpu_sys_ms = cpu_sys_ms;
    entry->exit_code = exit_code;
    entry->pid = pid;
    entry->sid = session_sid;
    entry->command = strdup(cmd);
    entry->cwd = strdup(cwd);
    
    if (!entry->command || !entry->cwd) {
        perror("strdup");
        return;
    }
    
    save_entry_to_file(entry);
    history_count++;
}

// ============================================================================
// FILE I/O (Enhanced with new fields)
// ============================================================================

void save_entry_to_file(HistoryEntry *entry) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        perror("open");
        return;
    }
    
    char buffer[MAX_CMD_LEN * 2];
    int len = snprintf(buffer, sizeof(buffer),
                       "%d|%ld|%.2f|%.2f|%.2f|%d|%d|%d|%s|%s\n",
                       entry->id,
                       (long)entry->timestamp,
                       entry->duration_ms,
                       entry->cpu_user_ms,
                       entry->cpu_sys_ms,
                       entry->exit_code,
                       entry->pid,
                       entry->sid,
                       entry->cwd,
                       entry->command);
    
    if (write(fd, buffer, len) != len) {
        perror("write");
    }
    close(fd);
}

void load_history_from_file(void) {
    int fd = open(LOG_FILE, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            perror("open");
        }
        return;
    }
    
    char buffer[MAX_CMD_LEN * 2];
    ssize_t bytes_read;
    int line_start = 0;
    
    while ((bytes_read = read(fd, buffer + line_start, sizeof(buffer) - line_start - 1)) > 0) {
        buffer[line_start + bytes_read] = '\0';
        
        char *line = buffer;
        char *newline;
        
        while ((newline = strchr(line, '\n')) != NULL) {
            *newline = '\0';
            
            // Parse: id|timestamp|duration|cpu_user|cpu_sys|exit_code|pid|sid|cwd|command
            int id, exit_code, pid, sid;
            long timestamp;
            double duration_ms, cpu_user_ms, cpu_sys_ms;
            char cwd[PATH_MAX], command[MAX_CMD_LEN];
            
            int fields = sscanf(line, "%d|%ld|%lf|%lf|%lf|%d|%d|%d|%[^|]|%[^\n]",
                               &id, &timestamp, &duration_ms, &cpu_user_ms, &cpu_sys_ms,
                               &exit_code, &pid, &sid, cwd, command);
            
            if (fields == 10) {
                if (history_count >= history_capacity) {
                    grow_history();
                }
                
                HistoryEntry *entry = &history[history_count];
                entry->id = id;
                entry->timestamp = (time_t)timestamp;
                entry->duration_ms = duration_ms;
                entry->cpu_user_ms = cpu_user_ms;
                entry->cpu_sys_ms = cpu_sys_ms;
                entry->exit_code = exit_code;
                entry->pid = pid;
                entry->sid = sid;
                entry->cwd = strdup(cwd);
                entry->command = strdup(command);
                
                history_count++;
            }
            
            line = newline + 1;
        }
        
        // Move remaining data to start of buffer
        int remaining = strlen(line);
        memmove(buffer, line, remaining + 1);
        line_start = remaining;
    }
    
    close(fd);
    printf("[Loaded %d entries from %s]\n", history_count, LOG_FILE);
}

// ============================================================================
// BETTER COMMAND PARSING (Handles quotes and escapes)
// ============================================================================

char **parse_command(const char *cmd) {
    char **args = malloc(MAX_ARGS * sizeof(char *));
    if (!args) return NULL;
    
    int arg_count = 0;
    const char *p = cmd;
    char buffer[MAX_CMD_LEN];
    int buf_pos = 0;
    int in_single_quote = 0;
    int in_double_quote = 0;
    
    while (*p && arg_count < MAX_ARGS - 1) {
        // Skip leading whitespace (unless in quotes)
        while (!in_single_quote && !in_double_quote && isspace(*p)) p++;
        
        if (!*p) break;
        
        buf_pos = 0;
        
        // Parse one argument
        while (*p && (in_single_quote || in_double_quote || !isspace(*p))) {
            if (*p == '\'' && !in_double_quote) {
                in_single_quote = !in_single_quote;
                p++;
            } else if (*p == '"' && !in_single_quote) {
                in_double_quote = !in_double_quote;
                p++;
            } else if (*p == '\\' && *(p + 1)) {
                // Handle escape sequences
                p++;
                if (buf_pos < MAX_CMD_LEN - 1) {
                    buffer[buf_pos++] = *p++;
                }
            } else {
                if (buf_pos < MAX_CMD_LEN - 1) {
                    buffer[buf_pos++] = *p++;
                } else {
                    p++;
                }
            }
        }
        
        buffer[buf_pos] = '\0';
        
        if (buf_pos > 0 || in_single_quote || in_double_quote) {
            args[arg_count++] = strdup(buffer);
        }
    }
    
    args[arg_count] = NULL;
    return args;
}

void free_args(char **args) {
    if (!args) return;
    for (int i = 0; args[i]; i++) {
        free(args[i]);
    }
    free(args);
}

// ============================================================================
// SECURITY HARDENING
// ============================================================================

int validate_command(const char *cmd) {
    // Check against blacklist
    for (int i = 0; BLACKLIST[i] != NULL; i++) {
        if (strstr(cmd, BLACKLIST[i])) {
            fprintf(stderr, "[Security] Command blocked: matches dangerous pattern '%s'\n", 
                    BLACKLIST[i]);
            return 0;
        }
    }
    
    // Check for suspicious patterns
    if (strstr(cmd, "/etc/shadow") || strstr(cmd, "/etc/passwd")) {
        fprintf(stderr, "[Security] Warning: accessing sensitive system files\n");
    }
    
    return 1;
}

char *resolve_command_path(const char *cmd) {
    char *path = NULL;
    
    // If command contains '/', use as-is
    if (strchr(cmd, '/')) {
        path = realpath(cmd, NULL);
        if (path && access(path, X_OK) == 0) {
            return path;
        }
        free(path);
        return NULL;
    }
    
    // Search in PATH
    const char *env_path = getenv("PATH");
    if (!env_path) return NULL;
    
    char *path_copy = strdup(env_path);
    char *dir = strtok(path_copy, ":");
    
    while (dir) {
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        
        char *resolved = realpath(full_path, NULL);
        if (resolved && access(resolved, X_OK) == 0) {
            free(path_copy);
            return resolved;
        }
        free(resolved);
        
        dir = strtok(NULL, ":");
    }
    
    free(path_copy);
    return NULL;
}

// ============================================================================
// PROCESS GROUP CONTROL & CPU TIME TRACKING
// ============================================================================

void execute_command(const char *cmd) {
    if (!validate_command(cmd)) {
        return;
    }
    
    // Get current working directory (Environment & Context)
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        strcpy(cwd, "unknown");
    }
    
    struct timespec start_time, end_time;
    struct rusage usage_before, usage_after;
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    getrusage(RUSAGE_CHILDREN, &usage_before);
    
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return;
    }
    
    if (pid == 0) {
        // Child process
        
        // Create new process group (Process Group Control)
        if (setpgid(0, 0) < 0) {
            perror("setpgid");
            _exit(126);
        }
        
        // Parse command with better parsing
        char **args = parse_command(cmd);
        if (!args || !args[0]) {
            fprintf(stderr, "Failed to parse command\n");
            _exit(126);
        }
        
        // Resolve command path (Security Hardening)
        char *resolved = resolve_command_path(args[0]);
        if (!resolved) {
            fprintf(stderr, "%s: command not found\n", args[0]);
            free_args(args);
            _exit(127);
        }
        
        execvp(resolved, args);
        
        // If execvp returns, it failed
        perror("execvp");
        free(resolved);
        free_args(args);
        _exit(127);
    }
    
    // Parent process
    int status;
    waitpid(pid, &status, 0);
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    getrusage(RUSAGE_CHILDREN, &usage_after);
    
    // Calculate wall clock time
    double duration_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                         (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    
    // Calculate CPU time (CPU Time Tracking)
    double cpu_user_ms = (usage_after.ru_utime.tv_sec - usage_before.ru_utime.tv_sec) * 1000.0 +
                         (usage_after.ru_utime.tv_usec - usage_before.ru_utime.tv_usec) / 1000.0;
    
    double cpu_sys_ms = (usage_after.ru_stime.tv_sec - usage_before.ru_stime.tv_sec) * 1000.0 +
                        (usage_after.ru_stime.tv_usec - usage_before.ru_stime.tv_usec) / 1000.0;
    
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    
    // Add to history with all new fields
    add_to_history(cmd, duration_ms, cpu_user_ms, cpu_sys_ms, exit_code, pid, cwd);
    
    printf("[Completed] exit=%d wall=%.2fms cpu_user=%.2fms cpu_sys=%.2fms pid=%d\n",
           exit_code, duration_ms, cpu_user_ms, cpu_sys_ms, pid);
}

// ============================================================================
// BUILT-IN COMMANDS
// ============================================================================

// ctime() appends '\n', which breaks one-line table rows.
// Returns pointer to a static buffer — do not free, not reentrant.
static const char *format_time(time_t *t) {
    static char buf[26];
    char *s = ctime(t);
    if (!s) return "unknown";
    snprintf(buf, sizeof(buf), "%s", s);
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

void cmd_history(void) {
    if (history_count == 0) {
        printf("No history yet.\n");
        return;
    }
    
    printf("\n=== Command History (%d entries) ===\n", history_count);
    for (int i = 0; i < history_count; i++) {
        HistoryEntry *e = &history[i];
        printf("%3d | %s | exit=%d | wall=%.0fms cpu=%.0fms | pid=%d | %s | %s\n",
               e->id,
               format_time(&e->timestamp),
               e->exit_code,
               e->duration_ms,
               e->cpu_user_ms + e->cpu_sys_ms,
               e->pid,
               e->cwd,
               e->command);
    }
    printf("\n");
}

void cmd_replay(int id) {
    if (id < 1 || id > history_count) {
        printf("Invalid history ID: %d\n", id);
        return;
    }
    
    HistoryEntry *e = &history[id - 1];
    printf("[Replaying #%d] %s\n", id, e->command);
    
    // Change to original directory if different (Environment & Context)
    char current_cwd[PATH_MAX];
    if (!getcwd(current_cwd, sizeof(current_cwd))) {
        perror("getcwd");
        return;
    }
    
    int dir_changed = 0;
    if (strcmp(current_cwd, e->cwd) != 0) {
        if (chdir(e->cwd) == 0) {
            printf("[Changed to original directory: %s]\n", e->cwd);
            dir_changed = 1;
        } else {
            printf("[Warning: Could not change to original directory: %s]\n", e->cwd);
        }
    }
    
    execute_command(e->command);
    
    // Restore original directory
    if (dir_changed) {
        if (chdir(current_cwd) < 0) {
            perror("chdir (restore)");
        }
    }
}

void cmd_search(const char *keyword) {
    printf("\n=== Search Results for '%s' ===\n", keyword);
    int found = 0;
    
    for (int i = 0; i < history_count; i++) {
        if (strstr(history[i].command, keyword)) {
            printf("%3d | %s | exit=%d | %s\n",
                   history[i].id,
                   format_time(&history[i].timestamp),
                   history[i].exit_code,
                   history[i].command);
            found++;
        }
    }
    
    if (found == 0) {
        printf("No matches found.\n");
    }
    printf("\n");
}

void cmd_stats(void) {
    if (history_count == 0) {
        printf("No statistics available.\n");
        return;
    }
    
    printf("\n=== Command Statistics ===\n");
    printf("Total commands: %d\n", history_count);
    
    // Most used command (track winning index, not a pointer into freed memory)
    int max_count = 0;
    int most_used_idx = -1;
    
    for (int i = 0; i < history_count; i++) {
        char *cmd_copy = strdup(history[i].command);
        char *base_cmd = strtok(cmd_copy, " ");
        
        int count = 0;
        for (int j = 0; j < history_count; j++) {
            char *cmp_copy = strdup(history[j].command);
            char *cmp_base = strtok(cmp_copy, " ");
            if (strcmp(base_cmd, cmp_base) == 0) count++;
            free(cmp_copy);
        }
        
        if (count > max_count) {
            max_count = count;
            most_used_idx = i;
        }
        
        free(cmd_copy);
    }
    
    if (most_used_idx >= 0) {
        char *winner = strdup(history[most_used_idx].command);
        char *base = strtok(winner, " ");
        printf("Most used command: %s (%d times)\n", base ? base : winner, max_count);
        free(winner);
    }
    
    // Slowest command (wall clock)
    int slowest_idx = 0;
    for (int i = 1; i < history_count; i++) {
        if (history[i].duration_ms > history[slowest_idx].duration_ms) {
            slowest_idx = i;
        }
    }
    
    printf("Slowest command (wall): #%d %s (%.2fms)\n",
           history[slowest_idx].id,
           history[slowest_idx].command,
           history[slowest_idx].duration_ms);
    
    // Highest CPU usage
    int cpu_idx = 0;
    double max_cpu = history[0].cpu_user_ms + history[0].cpu_sys_ms;
    for (int i = 1; i < history_count; i++) {
        double cpu = history[i].cpu_user_ms + history[i].cpu_sys_ms;
        if (cpu > max_cpu) {
            max_cpu = cpu;
            cpu_idx = i;
        }
    }
    
    printf("Highest CPU usage: #%d %s (%.2fms)\n",
           history[cpu_idx].id,
           history[cpu_idx].command,
           max_cpu);
    
    // Success rate
    int success_count = 0;
    for (int i = 0; i < history_count; i++) {
        if (history[i].exit_code == 0) success_count++;
    }
    
    printf("Success rate: %.1f%% (%d/%d)\n",
           100.0 * success_count / history_count,
           success_count,
           history_count);
    
    printf("\n");
}

void cmd_cd(const char *path) {
    if (!path || strlen(path) == 0) {
        path = getenv("HOME");
        if (!path) {
            printf("cd: HOME not set\n");
            return;
        }
    }
    
    if (chdir(path) < 0) {
        perror("cd");
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            printf("[Changed to: %s]\n", cwd);
        } else {
            printf("[Changed directory]\n");
        }
    }
}

void cmd_info(void) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "unknown");
    }
    
    printf("\n=== Session Information ===\n");
    printf("PID: %d\n", session_pid);
    printf("Session ID: %d\n", session_sid);
    printf("Current Directory: %s\n", cwd);
    printf("History Entries: %d / %d (capacity)\n", history_count, history_capacity);
    printf("Log File: %s\n", LOG_FILE);
    printf("\n");
}

// ============================================================================
// TERMINAL CONTROL (Arrow keys, line editing)
// ============================================================================

void enable_raw_mode(void) {
    if (terminal_raw_mode) return;
    
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    
    terminal_raw_mode = 1;
}

void disable_raw_mode(void) {
    if (!terminal_raw_mode) return;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    terminal_raw_mode = 0;
}

char *read_line_with_history(void) {
    char *buffer = malloc(MAX_CMD_LEN);
    if (!buffer) return NULL;
    
    int pos = 0;
    int history_idx = history_count;
    
    enable_raw_mode();
    
    while (1) {
        char c = getchar();
        
        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }
        
        if (c == 4) { // Ctrl-D (EOF)
            if (pos == 0) {
                free(buffer);
                disable_raw_mode();
                return NULL;
            }
            continue;
        }
        
        if (c == 3) { // Ctrl-C
            putchar('\n');
            free(buffer);
            disable_raw_mode();
            return strdup("");
        }
        
        if (c == 127 || c == 8) { // Backspace
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        
        if (c == 27) { // Escape sequence
            char seq[2];
            seq[0] = getchar();
            seq[1] = getchar();
            
            if (seq[0] == '[') {
                if (seq[1] == 'A') { // Up arrow
                    if (history_idx > 0) {
                        history_idx--;
                        // Clear current line
                        while (pos > 0) {
                            printf("\b \b");
                            pos--;
                        }
                        // Load from history
                        strcpy(buffer, history[history_idx].command);
                        pos = strlen(buffer);
                        printf("%s", buffer);
                        fflush(stdout);
                    }
                } else if (seq[1] == 'B') { // Down arrow
                    if (history_idx < history_count - 1) {
                        history_idx++;
                        // Clear current line
                        while (pos > 0) {
                            printf("\b \b");
                            pos--;
                        }
                        // Load from history
                        strcpy(buffer, history[history_idx].command);
                        pos = strlen(buffer);
                        printf("%s", buffer);
                        fflush(stdout);
                    } else if (history_idx == history_count - 1) {
                        history_idx++;
                        // Clear line
                        while (pos > 0) {
                            printf("\b \b");
                            pos--;
                        }
                        buffer[0] = '\0';
                    }
                }
            }
            continue;
        }
        
        // Regular character
        if (pos < MAX_CMD_LEN - 1) {
            buffer[pos++] = c;
            putchar(c);
            fflush(stdout);
        }
    }
    
    buffer[pos] = '\0';
    disable_raw_mode();
    
    return buffer;
}

// ============================================================================
// SIGNAL HANDLING (Process Group Control)
// ============================================================================

void sigint_handler(int sig) {
    (void)sig;
    got_sigint = 1;
    printf("\n[Interrupted]\n");
}

void sigchld_handler(int sig) {
    (void)sig;
    // Reap zombie processes
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void setup_signal_handlers(void) {
    struct sigaction sa;
    
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

// ============================================================================
// MAIN
// ============================================================================

int main(void) {
    printf("=== Enhanced Command History & Replay Tool ===\n");
    printf("Features: Dynamic Memory | Better Parsing | Process Groups | CPU Time\n");
    printf("          Environment Context | Session Tracking | Security | Terminal Control\n\n");
    
    // Initialize session tracking
    session_pid = getpid();
    session_sid = getsid(0);
    
    // Setup
    setup_signal_handlers();
    init_history();
    load_history_from_file();
    
    char cwd[PATH_MAX];
    
    while (1) {
        // Get current directory for prompt
        if (!getcwd(cwd, sizeof(cwd))) {
            strcpy(cwd, "?");
        }
        
        // Show prompt with directory
        char *home = getenv("HOME");
        if (home && strncmp(cwd, home, strlen(home)) == 0) {
            printf("~%s> ", cwd + strlen(home));
        } else {
            printf("%s> ", cwd);
        }
        fflush(stdout);
        
        // Read line with history support (Terminal Control)
        char *line = read_line_with_history();
        if (!line) {
            printf("\n[EOF received, exiting]\n");
            break;
        }
        
        // Trim whitespace
        char *cmd = line;
        while (isspace(*cmd)) cmd++;
        char *end = cmd + strlen(cmd) - 1;
        while (end > cmd && isspace(*end)) *end-- = '\0';
        
        if (strlen(cmd) == 0) {
            free(line);
            continue;
        }
        
        // Handle built-in commands
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            free(line);
            break;
        }
        
        if (strcmp(cmd, "history") == 0) {
            cmd_history();
        } else if (strcmp(cmd, "stats") == 0) {
            cmd_stats();
        } else if (strcmp(cmd, "info") == 0) {
            cmd_info();
        } else if (strncmp(cmd, "replay ", 7) == 0) {
            int id = atoi(cmd + 7);
            cmd_replay(id);
        } else if (strncmp(cmd, "search ", 7) == 0) {
            cmd_search(cmd + 7);
        } else if (strncmp(cmd, "cd ", 3) == 0) {
            cmd_cd(cmd + 3);
        } else if (strcmp(cmd, "cd") == 0) {
            cmd_cd(NULL);
        } else {
            // Execute external command
            execute_command(cmd);
        }
        
        free(line);
    }
    
    // Cleanup
    free_history();
    printf("[Session ended. Total commands: %d]\n", history_count);
    
    return 0;
}
