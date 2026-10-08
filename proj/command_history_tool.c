/*
 * Command History & Replay Tool  (v2)
 * Mini Project - Operating Systems and System Calls
 *
 * A terminal "flight recorder": runs your commands through POSIX system calls
 * and records how each one behaved - wall time, CPU time, peak memory, exit
 * code - then lets you search, replay, retry, time-limit and analyse them.
 *
 * Implements Tier A, B and C of command_history_replay_proposal.md, plus a
 * hand-rolled termios line editor and a linux/shell execution mode toggle.
 *
 * Build: gcc -O2 -Wall -Wextra command_history_tool.c -o history_tool
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/select.h>

/* ------------------------------------------------------------------ config */

#define LOG_FILE      "history.log"
#define MAX_LINE      8192
#define MAX_TOKENS    512
#define MAX_STAGES    32
#define MAX_ARGS      128
#define INIT_CAP      64
#define EXIT_TIMEOUT  124     /* conventional exit code for "timed out"      */
#define EXIT_NOEXEC   126     /* could not exec                              */
#define EXIT_NOTFOUND 127     /* command not found                           */
#define TOP_N         10      /* rows shown by "history --slow"              */
#define MAX_MATCHES   512     /* tab completion cap                          */
#define LIST_MAX      64      /* ambiguous matches printed before "N more"   */

/* ------------------------------------------------------------------- types */

/* One lexical token. is_op marks an unquoted shell operator so that a pipe
 * character inside quotes is never mistaken for a real pipeline separator. */
typedef struct {
    char *text;
    int   is_op;
} Token;

/* One stage of a pipeline, with its redirections resolved. */
typedef struct {
    char **argv;
    int    argc;
    char  *infile;      /* '<'  */
    char  *outfile;     /* '>' or '>>' */
    int    append;      /* true when the operator was '>>' */
} Stage;

/* One row of history.log, matching proposal section 9 exactly. */
typedef struct {
    int     id;
    time_t  timestamp;
    double  wall_ms;
    int     exit_code;
    double  user_ms;
    double  sys_ms;
    long    max_rss_kb;
    char   *command;
} HistoryEntry;

/* What one execution measured. */
typedef struct {
    int    exit_code;
    double wall_ms;
    double user_ms;
    double sys_ms;
    long   max_rss_kb;
    int    timed_out;
} RunMetrics;

/* ---------------------------------------------------------------- globals */

static HistoryEntry *g_hist;
static int  g_count;
static int  g_cap;

/* Written from signal handlers, read from the main flow. sig_atomic_t is the
 * only type guaranteed safe to touch across a signal. */
static volatile sig_atomic_t g_timed_out;
static volatile pid_t        g_pgid;   /* running command's group, 0 = none */

static int g_shell_mode;               /* 0 = linux mode, 1 = shell mode */

static struct termios g_orig_tio;
static int g_raw;                      /* true while the terminal is in raw mode */

/* Absolute path of the log, resolved once at startup. It must not stay
 * relative: the 'cd' built-in changes the process working directory, and a
 * relative "history.log" would silently start appending to a different file in
 * every directory the user visits, scattering and effectively losing history.
 * Sized past PATH_MAX so cwd + "/" + LOG_FILE cannot truncate. */
static char g_log_path[PATH_MAX + 32];

/* --------------------------------------------------------------- utilities */

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { perror("malloc"); exit(EXIT_FAILURE); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) { perror("realloc"); exit(EXIT_FAILURE); }
    return q;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p) { perror("strdup"); exit(EXIT_FAILURE); }
    return p;
}

/* write() warns if its result is ignored, but the line editor genuinely has
 * nowhere useful to report a dead terminal to. */
static void out(const char *s, size_t n)
{
    ssize_t r = write(STDOUT_FILENO, s, n);
    (void)r;
}

static void out_str(const char *s)
{
    out(s, strlen(s));
}

/* ctime() returns a trailing newline, which wrecks table rows. Caller must not
 * free the result and must copy it before calling again. */
static const char *fmt_time(time_t t)
{
    static char buf[32];
    time_t copy = t;
    char *s = ctime(&copy);
    if (!s) return "unknown";
    snprintf(buf, sizeof buf, "%s", s);
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

static void trim(char *s)
{
    size_t len;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

/* ------------------------------------------------- dynamic history storage */

static void hist_init(void)
{
    g_cap = INIT_CAP;
    g_hist = xmalloc((size_t)g_cap * sizeof *g_hist);
    g_count = 0;
}

static void hist_grow(void)
{
    g_cap *= 2;
    g_hist = xrealloc(g_hist, (size_t)g_cap * sizeof *g_hist);
}

static void hist_free(void)
{
    int i;
    for (i = 0; i < g_count; i++) free(g_hist[i].command);
    free(g_hist);
    g_hist = NULL;
    g_count = g_cap = 0;
}

/* Resolve the log to an absolute path. Called once from main(), before any
 * command can run and therefore before any cd. */
static void log_path_init(void)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd))
        snprintf(g_log_path, sizeof g_log_path, "%s/%s", cwd, LOG_FILE);
    else
        snprintf(g_log_path, sizeof g_log_path, "%s", LOG_FILE);
}

/* ------------------------------------------------------------ log file I/O */

/* Format (proposal section 9). The command is last on purpose: it may itself
 * contain '|' once pipelines are supported.
 *   id|timestamp|wall_ms|exit_code|user_ms|sys_ms|max_rss_kb|command
 */
static void log_append(const HistoryEntry *e)
{
    char buf[MAX_LINE * 2];
    int fd, len;
    ssize_t w;

    fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror(g_log_path); return; }

    len = snprintf(buf, sizeof buf, "%d|%ld|%.2f|%d|%.2f|%.2f|%ld|%s\n",
                   e->id, (long)e->timestamp, e->wall_ms, e->exit_code,
                   e->user_ms, e->sys_ms, e->max_rss_kb, e->command);
    if (len > 0 && len < (int)sizeof buf) {
        w = write(fd, buf, (size_t)len);
        if (w != len) perror(g_log_path);
    }
    close(fd);
}

/* Split a log line into the 7 leading fields; everything after the seventh
 * '|' is the command, verbatim. */
static int log_parse_line(char *line, HistoryEntry *e)
{
    char *fields[7];
    char *p = line;
    int i;

    for (i = 0; i < 7; i++) {
        char *bar;
        fields[i] = p;
        bar = strchr(p, '|');
        if (!bar) return 0;
        *bar = '\0';
        p = bar + 1;
    }

    e->id         = atoi(fields[0]);
    e->timestamp  = (time_t)atol(fields[1]);
    e->wall_ms    = atof(fields[2]);
    e->exit_code  = atoi(fields[3]);
    e->user_ms    = atof(fields[4]);
    e->sys_ms     = atof(fields[5]);
    e->max_rss_kb = atol(fields[6]);

    p[strcspn(p, "\r\n")] = '\0';
    e->command = xstrdup(p);
    return 1;
}

static void log_load(void)
{
    FILE *fp = fopen(g_log_path, "r");
    char line[MAX_LINE * 2];

    if (!fp) {
        if (errno != ENOENT) perror(g_log_path);
        return;
    }

    while (fgets(line, sizeof line, fp)) {
        HistoryEntry e;
        if (line[0] == '\n' || line[0] == '\0') continue;
        memset(&e, 0, sizeof e);
        if (!log_parse_line(line, &e)) continue;
        if (g_count >= g_cap) hist_grow();
        g_hist[g_count++] = e;
    }
    fclose(fp);

    /* Re-sequence ids so a hand-edited or truncated log cannot collide. */
    for (int i = 0; i < g_count; i++) g_hist[i].id = i + 1;

    printf("[Loaded %d %s from %s]\n", g_count,
           g_count == 1 ? "entry" : "entries", g_log_path);
}

static void hist_record(const char *cmd, const RunMetrics *m)
{
    HistoryEntry e;
    /* Copy before growing: callers such as replay pass a pointer that lives
     * inside g_hist, and hist_grow() may relocate that whole array. */
    char *copy = xstrdup(cmd);

    if (g_count >= g_cap) hist_grow();

    e.id         = g_count + 1;
    e.timestamp  = time(NULL);
    e.wall_ms    = m->wall_ms;
    e.exit_code  = m->exit_code;
    e.user_ms    = m->user_ms;
    e.sys_ms     = m->sys_ms;
    e.max_rss_kb = m->max_rss_kb;
    e.command    = copy;

    g_hist[g_count++] = e;
    log_append(&e);
}

/* -------------------------------------------------------------- tokenising */

/* Flush the word being accumulated into the token array. A zero-length word
 * carries no information and is dropped. Operators are emitted inline by the
 * tokeniser instead, so what lands here is always an ordinary word. */
static void tok_push(Token *toks, int *n, int max, char *buf, int *blen)
{
    if (*blen == 0) return;
    if (*n >= max) { *blen = 0; return; }
    buf[*blen] = '\0';
    toks[*n].text  = xstrdup(buf);
    toks[*n].is_op = 0;
    (*n)++;
    *blen = 0;
}

/*
 * Quote-aware tokeniser. Splits on unquoted whitespace and pulls the shell
 * operators | < > >> out as standalone tokens, even when written without
 * surrounding spaces (echo hi>out.txt). A quoted operator is ordinary text,
 * so: echo "a | b" yields one argument, not a pipeline.
 *
 * Returns the token count, or -1 on a malformed quote.
 */
static int tokenize(const char *line, Token *toks, int max)
{
    char buf[MAX_LINE];
    int  blen = 0;
    int  n = 0;
    const char *p = line;
    int in_single = 0, in_double = 0;

    while (*p) {
        char c = *p;

        if (in_single) {
            if (c == '\'') { in_single = 0; p++; continue; }
            if (blen < (int)sizeof buf - 1) buf[blen++] = c;
            p++;
            continue;
        }

        if (in_double) {
            if (c == '"') { in_double = 0; p++; continue; }
            if (c == '\\' && p[1] &&
                (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) {
                p++;
                if (blen < (int)sizeof buf - 1) buf[blen++] = *p++;
                continue;
            }
            if (blen < (int)sizeof buf - 1) buf[blen++] = c;
            p++;
            continue;
        }

        /* unquoted context */
        if (c == '\'') { in_single = 1; p++; continue; }
        if (c == '"')  { in_double = 1; p++; continue; }

        if (c == '\\' && p[1]) {
            p++;
            if (blen < (int)sizeof buf - 1) buf[blen++] = *p++;
            continue;
        }

        if (isspace((unsigned char)c)) {
            tok_push(toks, &n, max, buf, &blen);
            p++;
            continue;
        }

        if (c == '|' || c == '<' || c == '>') {
            char op[3];
            int  oplen = 1;

            /* finish whatever word preceded the operator */
            tok_push(toks, &n, max, buf, &blen);

            op[0] = c;
            if (c == '>' && p[1] == '>') { op[1] = '>'; oplen = 2; }
            op[oplen] = '\0';

            if (n < max) {
                toks[n].text  = xstrdup(op);
                toks[n].is_op = 1;
                n++;
            }
            p += oplen;
            continue;
        }

        if (blen < (int)sizeof buf - 1) buf[blen++] = c;
        p++;
    }

    if (in_single || in_double) return -1;

    tok_push(toks, &n, max, buf, &blen);
    return n;
}

static void tokens_free(Token *toks, int n)
{
    int i;
    for (i = 0; i < n; i++) free(toks[i].text);
}

/* --------------------------------------------------------- stage splitting */

static void stages_free(Stage *st, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        for (j = 0; j < st[i].argc; j++) free(st[i].argv[j]);
        free(st[i].argv);
        free(st[i].infile);
        free(st[i].outfile);
    }
}

/*
 * Turn a flat token list into pipeline stages. Splits on '|' operator tokens,
 * then within each stage pulls out '<', '>' and '>>' together with the
 * filename that follows.
 *
 * Returns the stage count, or -1 on a syntax error (message already printed).
 */
static int build_stages(Token *toks, int ntok, Stage *stages, int max)
{
    int nst = 0;
    int i = 0;

    if (ntok == 0) return -1;

    while (i < ntok) {
        Stage *s;
        char *words[MAX_ARGS];
        int   nw = 0;

        if (nst >= max) {
            fprintf(stderr, "parse: too many pipeline stages (max %d)\n", max);
            return -1;
        }

        s = &stages[nst];
        memset(s, 0, sizeof *s);

        for (; i < ntok; i++) {
            Token *t = &toks[i];

            if (t->is_op && strcmp(t->text, "|") == 0) { i++; break; }

            if (t->is_op && (strcmp(t->text, "<") == 0 ||
                             strcmp(t->text, ">") == 0 ||
                             strcmp(t->text, ">>") == 0)) {
                const char *kind = t->text;
                i++;
                if (i >= ntok || toks[i].is_op) {
                    fprintf(stderr, "parse: '%s' needs a filename after it\n", kind);
                    return -1;
                }
                if (strcmp(kind, "<") == 0) {
                    free(s->infile);
                    s->infile = xstrdup(toks[i].text);
                } else {
                    free(s->outfile);
                    s->outfile = xstrdup(toks[i].text);
                    s->append = (strcmp(kind, ">>") == 0);
                }
                continue;
            }

            if (nw >= MAX_ARGS - 1) {
                fprintf(stderr, "parse: too many arguments (max %d)\n", MAX_ARGS - 1);
                return -1;
            }
            words[nw++] = t->text;
        }

        if (nw == 0) {
            fprintf(stderr, "parse: empty pipeline stage\n");
            return -1;
        }

        s->argc = nw;
        s->argv = xmalloc((size_t)(nw + 1) * sizeof(char *));
        for (int k = 0; k < nw; k++) s->argv[k] = xstrdup(words[k]);
        s->argv[nw] = NULL;

        nst++;
    }

    /* tokens_free() owns the text; the stages hold their own copies */
    return nst;
}

/* ----------------------------------------------------------------- signals */

/*
 * Ctrl+C reaches the tool's own process group. The running command sits in a
 * different group, so the terminal does not signal it directly - we forward.
 * Only async-signal-safe calls are permitted in here.
 */
static void on_sigint(int sig)
{
    pid_t grp = g_pgid;
    (void)sig;
    if (grp > 0) kill(-grp, SIGINT);
}

static void on_sigalrm(int sig)
{
    pid_t grp = g_pgid;
    (void)sig;
    g_timed_out = 1;
    if (grp > 0) kill(-grp, SIGKILL);
}

static void signals_setup(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;              /* let wait4() resume after the handler */
    if (sigaction(SIGINT, &sa, NULL) < 0) perror("sigaction SIGINT");

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigalrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGALRM, &sa, NULL) < 0) perror("sigaction SIGALRM");

    /* We reap every child explicitly with wait4(), so the default action is
     * correct and no SIGCHLD handler is needed. */
}

/* --------------------------------------------------------------- execution */

static double tv_ms(const struct timeval *tv)
{
    return (double)tv->tv_sec * 1000.0 + (double)tv->tv_usec / 1000.0;
}

static int status_to_exit(int status)
{
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/*
 * Run an already-parsed pipeline and measure it.
 *
 * Every stage becomes one child. All children share a single process group so
 * that Ctrl+C and timeout can signal the whole pipeline at once. Parent and
 * child both call setpgid() to close the race where the child execs before the
 * parent gets a chance to move it.
 */
static int exec_stages(Stage *stages, int nst, int timeout_sec, RunMetrics *m)
{
    int   pfd[MAX_STAGES][2];
    pid_t pid[MAX_STAGES];
    pid_t pgid = 0;
    int   i, j;
    int   npipes;
    int   nstarted = 0;
    int   last_status = 0;
    struct timespec t0, t1;

    memset(m, 0, sizeof *m);

    /* Captured up front: nst is reduced if a fork fails partway, and the
     * close loops below must still cover every pipe that was created. */
    npipes = nst - 1;

    /* Pipes between adjacent stages: stage i writes pfd[i][1], stage i+1
     * reads pfd[i][0]. */
    for (i = 0; i < npipes; i++) {
        if (pipe(pfd[i]) < 0) {
            perror("pipe");
            while (--i >= 0) { close(pfd[i][0]); close(pfd[i][1]); }
            m->exit_code = EXIT_NOEXEC;
            return -1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (i = 0; i < nst; i++) {
        pid[i] = fork();
        if (pid[i] < 0) {
            perror("fork");
            /* Kill whatever already started; the reap loop below covers only
             * the children that exist. */
            if (pgid > 0) kill(-pgid, SIGKILL);
            nst = nstarted;
            break;
        }
        nstarted++;

        if (pid[i] == 0) {
            /* ---- child ---- */

            /* Join the shared group. For stage 0 pgid is still 0, and
             * setpgid(0, 0) makes this child its own group leader; the parent
             * performs the same call from its side to close the race where the
             * child execs first. Later stages already know the group id. */
            setpgid(0, pgid);

            /* A child must not keep the tool's signal behaviour. */
            signal(SIGINT,  SIG_DFL);
            signal(SIGALRM, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);

            /* Pipe wiring first, so an explicit file redirection overrides it,
             * matching shell semantics. */
            if (i > 0) {
                if (dup2(pfd[i - 1][0], STDIN_FILENO) < 0) _exit(EXIT_NOEXEC);
            }
            if (i < nst - 1) {
                if (dup2(pfd[i][1], STDOUT_FILENO) < 0) _exit(EXIT_NOEXEC);
            }

            if (stages[i].infile) {
                int fd = open(stages[i].infile, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "%s: %s\n", stages[i].infile, strerror(errno));
                    _exit(1);
                }
                if (dup2(fd, STDIN_FILENO) < 0) _exit(EXIT_NOEXEC);
                close(fd);
            }

            if (stages[i].outfile) {
                int flags = O_WRONLY | O_CREAT |
                            (stages[i].append ? O_APPEND : O_TRUNC);
                int fd = open(stages[i].outfile, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "%s: %s\n", stages[i].outfile, strerror(errno));
                    _exit(1);
                }
                if (dup2(fd, STDOUT_FILENO) < 0) _exit(EXIT_NOEXEC);
                close(fd);
            }

            /* Leaving any pipe end open in a child keeps the reader from ever
             * seeing EOF, which hangs the pipeline. Close them all. */
            for (j = 0; j < npipes; j++) {
                close(pfd[j][0]);
                close(pfd[j][1]);
            }

            execvp(stages[i].argv[0], stages[i].argv);
            fprintf(stderr, "%s: %s\n", stages[i].argv[0],
                    errno == ENOENT ? "command not found" : strerror(errno));
            _exit(EXIT_NOTFOUND);
        }

        /* ---- parent ---- */
        if (i == 0) {
            pgid = pid[0];
            setpgid(pgid, pgid);
        } else {
            setpgid(pid[i], pgid);
        }
    }

    /* The parent holds no interest in any pipe end. Uses npipes, not nst: a
     * failed fork may have reduced nst while every pipe was already created. */
    for (j = 0; j < npipes; j++) {
        close(pfd[j][0]);
        close(pfd[j][1]);
    }

    g_timed_out = 0;
    g_pgid = pgid;

    if (timeout_sec > 0) alarm((unsigned)timeout_sec);

    /* wait4() gives us the child's rusage directly, which is how peak memory
     * and per-command CPU time are obtained without sampling other children. */
    for (i = 0; i < nst; i++) {
        struct rusage ru;
        int status = 0;
        pid_t w;

        memset(&ru, 0, sizeof ru);
        do {
            w = wait4(pid[i], &status, 0, &ru);
        } while (w < 0 && errno == EINTR);

        if (w < 0) { perror("wait4"); continue; }

        m->user_ms += tv_ms(&ru.ru_utime);
        m->sys_ms  += tv_ms(&ru.ru_stime);
        if (ru.ru_maxrss > m->max_rss_kb) m->max_rss_kb = ru.ru_maxrss;

        last_status = status;   /* shell convention: the last stage decides */
    }

    if (timeout_sec > 0) alarm(0);

    /* Clear before anything else can observe it: a stale group id would let a
     * later Ctrl+C signal an unrelated process group that reused the number. */
    g_pgid = 0;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    m->wall_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                 (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    if (g_timed_out) {
        m->timed_out = 1;
        m->exit_code = EXIT_TIMEOUT;
    } else {
        m->exit_code = status_to_exit(last_status);
    }

    return 0;
}

/*
 * Run one raw command line end to end: parse (or delegate to /bin/sh in shell
 * mode), execute, measure, print the summary, append to the log.
 *
 * Returns the command's exit code.
 */
static int run_and_log(const char *raw, int timeout_sec)
{
    Token toks[MAX_TOKENS];
    Stage stages[MAX_STAGES];
    RunMetrics m;
    int ntok, nst;
    int rc;

    memset(toks, 0, sizeof toks);
    memset(stages, 0, sizeof stages);

    if (g_shell_mode) {
        /* Shell mode collapses to a single-stage pipeline whose argv asks sh
         * to interpret the line. Everything else - timing, rusage, signals,
         * logging - is the same code path as linux mode. */
        char *copy = xstrdup(raw);
        stages[0].argv = xmalloc(4 * sizeof(char *));
        stages[0].argv[0] = xstrdup("sh");
        stages[0].argv[1] = xstrdup("-c");
        stages[0].argv[2] = copy;
        stages[0].argv[3] = NULL;
        stages[0].argc = 3;
        nst = 1;
    } else {
        ntok = tokenize(raw, toks, MAX_TOKENS);
        if (ntok < 0) {
            fprintf(stderr, "parse: unbalanced quote\n");
            return -1;
        }
        if (ntok == 0) return -1;

        nst = build_stages(toks, ntok, stages, MAX_STAGES);
        tokens_free(toks, ntok);
        if (nst < 0) return -1;
    }

    rc = exec_stages(stages, nst, timeout_sec, &m);
    stages_free(stages, nst);

    if (rc < 0) {
        fprintf(stderr, "[Failed to run] exit=%d\n", m.exit_code);
        hist_record(raw, &m);
        return m.exit_code;
    }

    printf("[%s] exit=%d wall=%.2fms cpu=%.2f/%.2fms mem=%ldKB%s\n",
           m.timed_out ? "Timed out" : "Done",
           m.exit_code, m.wall_ms, m.user_ms, m.sys_ms, m.max_rss_kb,
           g_shell_mode ? " (shell mode)" : "");

    hist_record(raw, &m);
    return m.exit_code;
}

/* ------------------------------------------------------------ line editor */

static void raw_enable(void)
{
    struct termios raw;

    if (g_raw) return;
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_orig_tio) < 0) return;

    raw = g_orig_tio;
    /* ICANON off: deliver bytes immediately, no line buffering.
     * ECHO off:   the editor draws the line itself.
     * ISIG off:   we interpret Ctrl+C as "cancel this line" instead of letting
     *             the terminal raise SIGINT at the tool. */
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return;
    g_raw = 1;
}

static void raw_disable(void)
{
    if (!g_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_tio);
    g_raw = 0;
}

/* Redraw prompt + buffer, erase any leftover tail from a longer previous
 * render, then park the cursor at pos. */
static void ed_refresh(const char *prompt, size_t plen,
                       const char *buf, int len, int pos)
{
    char seq[32];
    int back = len - pos;

    out("\r", 1);
    out(prompt, plen);
    if (len > 0) out(buf, (size_t)len);
    out("\x1b[K", 3);
    if (back > 0) {
        int n = snprintf(seq, sizeof seq, "\x1b[%dD", back);
        if (n > 0) out(seq, (size_t)n);
    }
}

/* Read one byte, or -1 if nothing arrives within ms milliseconds. Used only
 * for the follow-up bytes of an escape sequence, to disambiguate a bare
 * Escape press from the start of an arrow-key sequence without blocking
 * forever. ms must be positive. */
static int read_byte_timeout(int ms)
{
    fd_set rfds;
    struct timeval tv;
    unsigned char c;
    ssize_t r;

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (long)(ms % 1000) * 1000L;

    for (;;) {
        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sel == 0) return -1;              /* timed out */
        r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        return (r == 1) ? (int)c : -1;
    }
}

static int is_cmd_char(int c)
{
    return c && !isspace(c) && c != '|' && c != '<' && c != '>';
}

/* Longest common prefix across matches, so typing "mk" then Tab lands on
 * "make" when that is all the candidates share. */
static size_t common_prefix(char **m, int n)
{
    size_t len;
    int i;

    if (n == 0) return 0;
    len = strlen(m[0]);
    for (i = 1; i < n; i++) {
        size_t k = 0;
        while (k < len && m[0][k] && m[0][k] == m[i][k]) k++;
        len = k;
    }
    return len;
}

static int match_add(char **m, int *n, const char *s)
{
    int i;
    if (*n >= MAX_MATCHES) return 0;
    for (i = 0; i < *n; i++) {
        if (strcmp(m[i], s) == 0) return 0;   /* same name reachable via two PATH dirs */
    }
    m[(*n)++] = xstrdup(s);
    return 1;
}

/* Candidates for the first word: our own built-ins plus every executable
 * reachable through PATH. */
static int complete_commands(const char *prefix, char **m, int *n)
{
    static const char *builtins[] = {
        "cd", "history", "replay", "retry", "timeout", "search", "stats",
        "export", "mode", "help", "exit", "quit", NULL
    };
    const char *pathenv;
    char *copy, *dir;
    int i;

    for (i = 0; builtins[i]; i++) {
        if (strncmp(builtins[i], prefix, strlen(prefix)) == 0)
            match_add(m, n, builtins[i]);
    }

    pathenv = getenv("PATH");
    if (!pathenv) return *n;

    copy = xstrdup(pathenv);
    dir = strtok(copy, ":");
    while (dir) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                char full[PATH_MAX];
                size_t plen = strlen(prefix);
                if (strncmp(ent->d_name, prefix, plen) != 0) continue;
                snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
                if (access(full, X_OK) == 0) match_add(m, n, ent->d_name);
            }
            closedir(d);
        }
        dir = strtok(NULL, ":");
    }
    free(copy);
    return *n;
}

/* Candidates for any later word: filenames. A prefix containing '/' is
 * resolved against its own directory. */
static int complete_files(const char *prefix, char **m, int *n)
{
    char dirpart[PATH_MAX];
    const char *base;
    const char *slash = strrchr(prefix, '/');
    DIR *d;
    struct dirent *ent;

    if (slash) {
        size_t dlen = (size_t)(slash - prefix);
        if (dlen == 0) {
            snprintf(dirpart, sizeof dirpart, "/");
        } else {
            if (dlen >= sizeof dirpart) dlen = sizeof dirpart - 1;
            memcpy(dirpart, prefix, dlen);
            dirpart[dlen] = '\0';
        }
        base = slash + 1;
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        base = prefix;
    }

    d = opendir(dirpart);
    if (!d) return *n;

    while ((ent = readdir(d)) != NULL) {
        /* Sized for a full dirpart plus a full d_name; PATH_MAX alone is not
         * enough once the two are joined. */
        char cand[PATH_MAX + 256];

        /* Hide dotfiles unless the user has explicitly started typing one. */
        if (base[0] != '.' && ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, base, strlen(base)) != 0) continue;

        if (!slash)
            snprintf(cand, sizeof cand, "%s", ent->d_name);
        else if (strcmp(dirpart, "/") == 0)
            snprintf(cand, sizeof cand, "/%s", ent->d_name);   /* avoid "//name" */
        else
            snprintf(cand, sizeof cand, "%s/%s", dirpart, ent->d_name);

        match_add(m, n, cand);
    }
    closedir(d);
    return *n;
}

/*
 * Tab completion. Extends the word under the cursor as far as the candidates
 * agree; on a unique match inserts it whole and appends '/' for directories.
 * With several candidates and nothing left to extend, lists them and redraws.
 */
static void ed_complete(char *buf, int *len, int *pos)
{
    char *matches[MAX_MATCHES];
    char  prefix[MAX_LINE];
    int   n = 0;
    int   start = *pos;
    int   i, first_word;
    size_t cp, plen;

    while (start > 0 && is_cmd_char(buf[start - 1])) start--;

    plen = (size_t)(*pos - start);
    if (plen >= sizeof prefix) plen = sizeof prefix - 1;
    memcpy(prefix, buf + start, plen);
    prefix[plen] = '\0';

    /* A word is completable as a command only when nothing but whitespace
     * precedes it on the line. */
    first_word = 1;
    for (i = 0; i < start; i++) {
        if (!isspace((unsigned char)buf[i])) { first_word = 0; break; }
    }

    if (first_word) complete_commands(prefix, matches, &n);
    else            complete_files(prefix, matches, &n);

    if (n == 0) {
        out("\a", 1);                       /* nothing matches: beep */
    } else {
        cp = common_prefix(matches, n);
        if (cp > plen) {
            int add = (int)(cp - plen);
            if (*len + add < MAX_LINE - 2) {
                memmove(buf + *pos + add, buf + *pos, (size_t)(*len - *pos));
                /* The extra bytes live in the candidates, not in prefix: prefix
                 * ends at plen, so reading past it copies NULs into the line. */
                memcpy(buf + *pos, matches[0] + plen, (size_t)add);
                *len += add;
                *pos += add;
                buf[*len] = '\0';
                plen = cp;
            }
        }

        if (n == 1) {
            /* One candidate: take it fully, and mark directories so the next
             * completion continues inside them. Only filenames get the trailing
             * slash - a command name is not a path even when a directory in the
             * current folder happens to share it. */
            size_t mlen = strlen(matches[0]);
            int add = (int)(mlen - plen);
            struct stat sb;
            int is_dir = 0;

            if (add > 0 && *len + add + 1 < MAX_LINE - 2) {
                memmove(buf + *pos + add, buf + *pos, (size_t)(*len - *pos));
                memcpy(buf + *pos, matches[0] + plen, (size_t)add);
                *len += add;
                *pos += add;
            }
            if (!first_word && stat(matches[0], &sb) == 0 && S_ISDIR(sb.st_mode))
                is_dir = 1;
            if (is_dir && (*len == 0 || buf[*pos - 1] != '/')) {
                if (*len + 1 < MAX_LINE - 2) {
                    memmove(buf + *pos + 1, buf + *pos, (size_t)(*len - *pos));
                    buf[*pos] = '/';
                    (*len)++;
                    (*pos)++;
                }
            }
            buf[*len] = '\0';
        } else if (cp <= plen) {
            /* Ambiguous and already as far as it goes: show the choices. Capped,
             * because on WSL the inherited PATH contains all of System32 and a
             * one-letter prefix can match hundreds of programs. */
            int shown = n > LIST_MAX ? LIST_MAX : n;
            out("\n", 1);
            for (i = 0; i < shown; i++) {
                out_str(matches[i]);
                out(i + 1 < shown ? "   " : "\n", i + 1 < shown ? 3 : 1);
            }
            if (shown < n)
                printf("[... %d more matches - keep typing to narrow it down]\n",
                       n - shown);
        }
    }

    for (i = 0; i < n; i++) free(matches[i]);
}

/*
 * Read one line with editing. Returns a malloc'd string the caller frees, or
 * NULL on EOF.
 *
 * Falls back to plain fgets() when stdin is not a terminal, which keeps piped
 * input and scripted testing working.
 */
static char *read_line(const char *prompt)
{
    char  buf[MAX_LINE];
    char  saved[MAX_LINE];
    int   len = 0, pos = 0;
    int   hist_idx;
    size_t plen;
    int   have_saved = 0;

    plen = strlen(prompt);

    if (!isatty(STDIN_FILENO)) {
        out_str(prompt);
        if (!fgets(buf, sizeof buf, stdin)) return NULL;
        buf[strcspn(buf, "\r\n")] = '\0';
        return xstrdup(buf);
    }

    raw_enable();
    hist_idx = g_count;
    out_str(prompt);

    for (;;) {
        unsigned char raw_byte;
        ssize_t rr;
        int c;

        /* Block on the first byte. A return of 0 is a real EOF (Ctrl-D from
         * the terminal driver is handled below as byte 4). */
        do {
            rr = read(STDIN_FILENO, &raw_byte, 1);
        } while (rr < 0 && errno == EINTR);

        if (rr <= 0) break;                   /* EOF or unrecoverable read error */
        c = (int)raw_byte;

        if (c == '\r' || c == '\n') {
            out("\n", 1);
            buf[len] = '\0';
            raw_disable();
            return xstrdup(buf);
        }

        if (c == 4) {                       /* Ctrl-D */
            if (len == 0) {
                out("\n", 1);
                raw_disable();
                return NULL;
            }
            continue;
        }

        if (c == 3) {                       /* Ctrl-C: abandon this line, stay alive */
            out("^C\n", 3);
            len = pos = 0;
            buf[0] = '\0';
            hist_idx = g_count;
            have_saved = 0;
            out_str(prompt);
            continue;
        }

        if (c == 127 || c == 8) {           /* Backspace */
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, (size_t)(len - pos));
                len--; pos--;
                buf[len] = '\0';
                ed_refresh(prompt, plen, buf, len, pos);
            }
            continue;
        }

        if (c == 1) { pos = 0; ed_refresh(prompt, plen, buf, len, pos); continue; }        /* Ctrl-A */
        if (c == 5) { pos = len; ed_refresh(prompt, plen, buf, len, pos); continue; }       /* Ctrl-E */
        if (c == 11) { len = pos; buf[len] = '\0'; ed_refresh(prompt, plen, buf, len, pos); continue; } /* Ctrl-K */
        if (c == 12) {                                                                 /* Ctrl-L */
            out("\x1b[H\x1b[2J", 7);
            ed_refresh(prompt, plen, buf, len, pos);
            continue;
        }

        if (c == 9) {                       /* Tab */
            buf[len] = '\0';
            ed_complete(buf, &len, &pos);
            ed_refresh(prompt, plen, buf, len, pos);
            continue;
        }

        if (c == 27) {                      /* escape sequence */
            int c1 = read_byte_timeout(50);
            int c2;

            if (c1 < 0) continue;           /* a lone Escape press */
            if (c1 != '[' && c1 != 'O') { continue; }

            c2 = read_byte_timeout(50);
            if (c2 < 0) continue;

            switch (c2) {
            case 'A':                                   /* Up: older command */
                if (hist_idx > 0) {
                    if (hist_idx == g_count) {
                        memcpy(saved, buf, (size_t)len + 1);
                        have_saved = 1;
                    }
                    hist_idx--;
                    snprintf(buf, sizeof buf, "%s", g_hist[hist_idx].command);
                    len = pos = (int)strlen(buf);
                    ed_refresh(prompt, plen, buf, len, pos);
                }
                break;
            case 'B':                                   /* Down: newer command */
                if (hist_idx < g_count) {
                    hist_idx++;
                    if (hist_idx == g_count && have_saved) {
                        snprintf(buf, sizeof buf, "%s", saved);
                    } else if (hist_idx < g_count) {
                        snprintf(buf, sizeof buf, "%s", g_hist[hist_idx].command);
                    } else {
                        buf[0] = '\0';
                    }
                    len = pos = (int)strlen(buf);
                    ed_refresh(prompt, plen, buf, len, pos);
                }
                break;
            case 'C':                                   /* Right */
                if (pos < len) { pos++; ed_refresh(prompt, plen, buf, len, pos); }
                break;
            case 'D':                                   /* Left */
                if (pos > 0) { pos--; ed_refresh(prompt, plen, buf, len, pos); }
                break;
            case 'H':                                   /* Home */
                pos = 0; ed_refresh(prompt, plen, buf, len, pos);
                break;
            case 'F':                                   /* End */
                pos = len; ed_refresh(prompt, plen, buf, len, pos);
                break;
            case '3':                                   /* Delete: ESC [ 3 ~ */
                if (read_byte_timeout(50) == '~' && pos < len) {
                    memmove(buf + pos, buf + pos + 1, (size_t)(len - pos - 1));
                    len--;
                    buf[len] = '\0';
                    ed_refresh(prompt, plen, buf, len, pos);
                }
                break;
            default:
                break;
            }
            continue;
        }

        if (c < 32 || c > 126) continue;    /* ignore other control bytes */

        if (len < MAX_LINE - 2) {           /* ordinary printable character */
            memmove(buf + pos + 1, buf + pos, (size_t)(len - pos));
            buf[pos] = (char)c;
            len++; pos++;
            buf[len] = '\0';
            ed_refresh(prompt, plen, buf, len, pos);
        }
    }

    /* Reached only on EOF or a read error: report end of input so main exits
     * rather than looping on a partial line. */
    raw_disable();
    return NULL;
}

/* -------------------------------------------------------- prompt building */

static void build_prompt(char *dst, size_t n)
{
    char cwd[PATH_MAX];
    const char *home;
    const char *shown;

    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");

    home = getenv("HOME");
    if (home && *home && strncmp(cwd, home, strlen(home)) == 0 &&
        (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0')) {
        static char tilde[PATH_MAX];
        snprintf(tilde, sizeof tilde, "~%s", cwd + strlen(home));
        shown = tilde;
    } else {
        shown = cwd;
    }

    snprintf(dst, n, "[%s] %s> ", g_shell_mode ? "shell" : "linux", shown);
}

/* --------------------------------------------------------- built-in: help */

static void cmd_help(void)
{
    printf(
    "\n"
    "Commands\n"
    "  <command>            run it and record how it behaved\n"
    "  cmd1 | cmd2          pipeline (linux mode)\n"
    "  cmd > f  >> f  < f   redirection (linux mode)\n"
    "  !!                   rerun the previous command\n"
    "  cd <dir>             change directory (built-in; a child cannot do this)\n"
    "  history [--failed|--slow]   list recorded commands, optionally filtered\n"
    "  replay <id>          rerun a command and diff it against its last run\n"
    "  retry <n> <cmd>      run up to n times, stopping at the first success\n"
    "  timeout <sec> <cmd>  kill the command if it exceeds sec seconds\n"
    "  search <keyword>     find commands containing a keyword\n"
    "  stats                usage, slowest, heaviest, failure rate, flaky\n"
    "  export <file.csv>    write the whole history as CSV\n"
    "  mode [linux|shell]   show or switch execution mode\n"
    "  help                 this text\n"
    "  exit                 quit\n"
    "\n"
    "Line editing\n"
    "  Up / Down            recall previous and next commands\n"
    "  Left / Right         move the cursor inside the line\n"
    "  Home / End           jump to line start or end (also Ctrl-A / Ctrl-E)\n"
    "  Tab                  complete a command name or a filename\n"
    "  Backspace / Delete   erase behind or ahead of the cursor\n"
    "  Ctrl-K               delete to end of line\n"
    "  Ctrl-L               clear the screen\n"
    "  Ctrl-C               abandon the current line (the tool keeps running)\n"
    "  Ctrl-D               exit on an empty line\n"
    "\n"
    "Modes\n"
    "  linux   this tool parses the line and calls fork/execvp/pipe/dup2 itself\n"
    "  shell   the line is handed to /bin/sh -c, so globs and $VARS work\n"
    "\n");
}

/* ------------------------------------------------------- built-in: history */

/* The CPU column is pre-rendered into a string so the two numbers stay glued
 * together; padding them independently with printf left the unit stranded
 * ("3/3     ms") and broke the header alignment. */
static void hist_print_row(const HistoryEntry *e)
{
    char cpu[32];
    snprintf(cpu, sizeof cpu, "%.0f/%.0fms", e->user_ms, e->sys_ms);
    printf("  %4d  %-24s  %4d  %7.0fms  %9s  %8ldKB  %s\n",
           e->id, fmt_time(e->timestamp), e->exit_code, e->wall_ms,
           cpu, e->max_rss_kb, e->command);
}

static void hist_print_header(void)
{
    printf("\n");
    printf("  %4s  %-24s  %4s  %9s  %9s  %10s  %s\n",
           "ID", "TIME", "EXIT", "WALL", "CPU(u/s)", "MEM", "COMMAND");
    printf("  --------------------------------------------------------------------------\n");
}

static void cmd_history(const char *arg)
{
    int failed = 0, slow = 0;
    int shown = 0;

    if (arg && *arg) {
        if (strcmp(arg, "--failed") == 0)      failed = 1;
        else if (strcmp(arg, "--slow") == 0)   slow = 1;
        else {
            printf("history: unknown option '%s' (try --failed or --slow)\n", arg);
            return;
        }
    }

    if (g_count == 0) { printf("No history yet.\n"); return; }

    if (slow) {
        /* Indices sorted by wall time, descending; no need to copy the entries. */
        int *idx = xmalloc((size_t)g_count * sizeof *idx);
        int i, j, limit;

        for (i = 0; i < g_count; i++) idx[i] = i;
        for (i = 0; i < g_count - 1; i++)
            for (j = 0; j < g_count - 1 - i; j++)
                if (g_hist[idx[j]].wall_ms < g_hist[idx[j + 1]].wall_ms) {
                    int t = idx[j]; idx[j] = idx[j + 1]; idx[j + 1] = t;
                }

        printf("\n=== %d slowest commands ===\n", TOP_N < g_count ? TOP_N : g_count);
        hist_print_header();
        limit = TOP_N < g_count ? TOP_N : g_count;
        for (i = 0; i < limit; i++) { hist_print_row(&g_hist[idx[i]]); shown++; }
        free(idx);
    } else {
        printf("\n=== Command History (%d %s%s) ===\n", g_count,
               g_count == 1 ? "entry" : "entries",
               failed ? ", failed only" : "");
        hist_print_header();
        for (int i = 0; i < g_count; i++) {
            if (failed && g_hist[i].exit_code == 0) continue;
            hist_print_row(&g_hist[i]);
            shown++;
        }
    }

    if (shown == 0) printf("  (no matching entries)\n");
    printf("\n");
}

/* -------------------------------------------------------- built-in: replay */

/* The entry most recently before idx that ran the same command text, or -1. */
static int find_previous_same(int idx)
{
    int i;
    for (i = idx - 1; i >= 0; i--) {
        if (strcmp(g_hist[i].command, g_hist[idx].command) == 0) return i;
    }
    return -1;
}

static void cmd_replay(const char *arg)
{
    int id, idx, prev;
    HistoryEntry before;
    RunMetrics after;

    if (!arg || !*arg) { printf("usage: replay <id>\n"); return; }

    id = atoi(arg);
    if (id < 1 || id > g_count) {
        printf("replay: no such id %d (history holds 1..%d)\n", id, g_count);
        return;
    }

    idx = id - 1;
    prev = find_previous_same(idx);

    /* Snapshot the old run. g_hist can be relocated by hist_record() during
     * the new run, so nothing we compare against may alias it. */
    before = g_hist[idx];
    before.command = xstrdup(g_hist[idx].command);

    printf("[Replaying #%d] %s\n", id, before.command);

    run_and_log(before.command, 0);

    /* The run we just made is the newest entry. */
    after = (RunMetrics){
        .exit_code  = g_hist[g_count - 1].exit_code,
        .wall_ms    = g_hist[g_count - 1].wall_ms,
        .user_ms    = g_hist[g_count - 1].user_ms,
        .sys_ms     = g_hist[g_count - 1].sys_ms,
        .max_rss_kb = g_hist[g_count - 1].max_rss_kb,
        .timed_out  = 0
    };

    printf("\n--- diff vs run #%d ---\n", before.id);
    printf("exit code : %d -> %d%s\n", before.exit_code, after.exit_code,
           before.exit_code == after.exit_code ? "  (unchanged)" : "  CHANGED");

    if (before.wall_ms > 0.0) {
        double pct = (after.wall_ms - before.wall_ms) / before.wall_ms * 100.0;
        printf("wall time : %.0fms -> %.0fms  (%+.1f%%)\n",
               before.wall_ms, after.wall_ms, pct);
    } else {
        printf("wall time : %.0fms -> %.0fms\n", before.wall_ms, after.wall_ms);
    }

    printf("cpu u/s   : %.0f/%.0fms -> %.0f/%.0fms\n",
           before.user_ms, before.sys_ms, after.user_ms, after.sys_ms);
    printf("peak mem  : %ldKB -> %ldKB\n", before.max_rss_kb, after.max_rss_kb);

    if (prev >= 0) {
        int prior = 0, k;
        for (k = 0; k < idx; k++)
            if (strcmp(g_hist[k].command, before.command) == 0) prior++;
        printf("(this exact command had run %d %s before)\n",
               prior, prior == 1 ? "time" : "times");
    }

    free(before.command);
    printf("\n");
}

/* -------------------------------------------------------- built-in: search */

static void cmd_search(const char *arg)
{
    int found = 0, i;

    if (!arg || !*arg) { printf("usage: search <keyword>\n"); return; }
    if (g_count == 0) { printf("No history yet.\n"); return; }

    printf("\n=== Search results for '%s' ===\n", arg);
    hist_print_header();
    for (i = 0; i < g_count; i++) {
        if (strstr(g_hist[i].command, arg)) { hist_print_row(&g_hist[i]); found++; }
    }
    if (found == 0) printf("  (no matches)\n");
    else printf("\n%d %s matched.\n", found, found == 1 ? "entry" : "entries");
    printf("\n");
}

/* --------------------------------------------------------- built-in: stats */

/* Group commands by their first word so "most used" and flaky detection reflect
 * the program invoked, not the exact argument list. */
typedef struct { char *name; int total; int ok; int fail; } CmdAgg;

static char *base_cmd(const char *s)
{
    const char *sp = strchr(s, ' ');
    size_t n = sp ? (size_t)(sp - s) : strlen(s);
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static void cmd_stats(void)
{
    CmdAgg *agg;
    int nagg = 0, i, j;
    int total = g_count, ok = 0;
    int slow_i = -1, mem_i = -1, used_i = -1;

    if (g_count == 0) { printf("No statistics available.\n"); return; }

    agg = xmalloc((size_t)g_count * sizeof *agg);

    for (i = 0; i < g_count; i++) {
        char *b = base_cmd(g_hist[i].command);
        int slot = -1;

        for (j = 0; j < nagg; j++) {
            if (strcmp(agg[j].name, b) == 0) { slot = j; break; }
        }
        if (slot < 0) {
            slot = nagg++;
            agg[slot].name  = b;            /* ownership moves into agg */
            agg[slot].total = agg[slot].ok = agg[slot].fail = 0;
            b = NULL;
        }
        free(b);

        agg[slot].total++;
        if (g_hist[i].exit_code == 0) { agg[slot].ok++; ok++; }
        else                            agg[slot].fail++;
    }

    used_i = 0;
    for (i = 1; i < nagg; i++)
        if (agg[i].total > agg[used_i].total) used_i = i;

    slow_i = 0;
    for (i = 1; i < g_count; i++)
        if (g_hist[i].wall_ms > g_hist[slow_i].wall_ms) slow_i = i;

    mem_i = 0;
    for (i = 1; i < g_count; i++)
        if (g_hist[i].max_rss_kb > g_hist[mem_i].max_rss_kb) mem_i = i;

    printf("\n=== Command Statistics ===\n");
    printf("Total commands   : %d\n", total);
    printf("Distinct commands: %d\n", nagg);

    if (nagg > 0)
        printf("Most used        : \"%s\" (%d times)\n",
               agg[used_i].name, agg[used_i].total);

    printf("Slowest          : \"%s\" (%.0f ms)\n",
           g_hist[slow_i].command, g_hist[slow_i].wall_ms);
    printf("Highest memory   : \"%s\" (%ld KB)\n",
           g_hist[mem_i].command, g_hist[mem_i].max_rss_kb);
    printf("Failure rate     : %.0f%% (%d/%d)\n",
           100.0 * (double)(total - ok) / (double)total, total - ok, total);

    /* A command is flaky when the same text has both succeeded and failed. */
    printf("\nFlaky commands (both passed and failed):\n");
    {
        int any = 0;
        for (i = 0; i < nagg; i++) {
            if (agg[i].ok > 0 && agg[i].fail > 0) {
                printf("  ! \"%s\" (passed %d, failed %d)\n",
                       agg[i].name, agg[i].ok, agg[i].fail);
                any = 1;
            }
        }
        if (!any) printf("  (none)\n");
    }

    for (i = 0; i < nagg; i++) free(agg[i].name);
    free(agg);
    printf("\n");
}

/* --------------------------------------------------------- built-in: retry */

static void cmd_retry(const char *arg)
{
    char *copy, *sp, *rest;
    long n, attempt;
    int code = -1;

    if (!arg || !*arg) { printf("usage: retry <n> <command>\n"); return; }

    copy = xstrdup(arg);
    sp = copy;
    while (*sp && isspace((unsigned char)*sp)) sp++;

    n = strtol(sp, &rest, 10);
    if (rest == sp || n < 1) {
        printf("retry: expected a positive attempt count, e.g. retry 3 ./run_tests\n");
        free(copy);
        return;
    }
    while (*rest && isspace((unsigned char)*rest)) rest++;
    if (!*rest) {
        printf("retry: no command given\n");
        free(copy);
        return;
    }

    for (attempt = 1; attempt <= n; attempt++) {
        printf("\n[attempt %ld/%ld] %s\n", attempt, n, rest);
        code = run_and_log(rest, 0);
        if (code == 0) break;
    }

    if (code == 0) printf("\nretry: succeeded on attempt %ld\n", attempt);
    else           printf("\nretry: still failing after %ld attempts (last exit %d)\n", n, code);

    free(copy);
}

/* -------------------------------------------------------- built-in: export */

/* Quote a field for CSV: wrap in double quotes and double any inner quote. */
static void csv_field(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"') fputc('"', fp);
        fputc(*s, fp);
    }
    fputc('"', fp);
}

static void cmd_export(const char *arg)
{
    const char *path = (arg && *arg) ? arg : "history.csv";
    FILE *fp;
    int i;

    fp = fopen(path, "w");
    if (!fp) { perror(path); return; }

    fprintf(fp, "id,timestamp,datetime,wall_ms,exit_code,user_ms,sys_ms,max_rss_kb,command\n");
    for (i = 0; i < g_count; i++) {
        const HistoryEntry *e = &g_hist[i];
        fprintf(fp, "%d,%ld,", e->id, (long)e->timestamp);
        csv_field(fp, fmt_time(e->timestamp));
        fprintf(fp, ",%.2f,%d,%.2f,%.2f,%ld,",
                e->wall_ms, e->exit_code, e->user_ms, e->sys_ms, e->max_rss_kb);
        csv_field(fp, e->command);
        fputc('\n', fp);
    }
    fclose(fp);

    printf("Exported %d %s to %s\n", g_count, g_count == 1 ? "entry" : "entries", path);
}

/* ---------------------------------------------------------- built-in: mode */

static void cmd_mode(const char *arg)
{
    if (!arg || !*arg) {
        printf("mode: %s\n", g_shell_mode ? "shell" : "linux");
        printf("  linux  this tool parses the line and calls fork/execvp/pipe/dup2 itself\n");
        printf("  shell  the line is passed to /bin/sh -c, so globs and $VARS expand\n");
        return;
    }
    if (strcmp(arg, "linux") == 0)      { g_shell_mode = 0; printf("mode: linux\n"); }
    else if (strcmp(arg, "shell") == 0) { g_shell_mode = 1; printf("mode: shell\n"); }
    else printf("mode: unknown mode '%s' (use linux or shell)\n", arg);
}

/* Match a built-in name at the start of a line, requiring a word boundary so
 * that e.g. 'timeoutx' is not treated as 'timeout'. Returns the argument text
 * (possibly empty), or NULL when the line is not this built-in. Every built-in
 * must be reachable with no argument: otherwise a bare 'replay' falls through to
 * execvp() and the user gets a confusing 'command not found' for a command the
 * tool itself implements. */
static char *builtin_arg(char *cmd, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(cmd, name, n) != 0) return NULL;
    if (cmd[n] != '\0' && cmd[n] != ' ' && cmd[n] != '\t') return NULL;
    return cmd + n;
}

/* -------------------------------------------------------------------- main */

int main(void)
{
    char prompt[PATH_MAX + 64];

    /* Line-buffer stdout even when it is a pipe. Children write straight to fd 1,
     * so a fully buffered parent interleaves its summaries after theirs and the
     * transcript reads out of order. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== Command History & Replay Tool (v2) ===\n");
    printf("Tier A: pipes, redirects, cd, Ctrl+C isolation\n");
    printf("Tier B: wait4 memory report, replay diff, timeout, retry, flaky detection\n");
    printf("Tier C: history filters, !!, CSV export\n");
    printf("Type 'help' for the command list.\n\n");

    signals_setup();
    hist_init();
    log_path_init();      /* must precede log_load and any user cd */
    log_load();

    for (;;) {
        char *line, *cmd, *arg;
        int handled = 0;

        build_prompt(prompt, sizeof prompt);
        line = read_line(prompt);
        if (!line) { printf("\n[EOF - exiting]\n"); break; }

        cmd = line;
        trim(cmd);

        if (!*cmd) { free(line); continue; }

        /* '!!' expands to the previous command before anything else sees it. */
        if (strcmp(cmd, "!!") == 0) {
            if (g_count == 0) {
                printf("!!: no previous command\n");
                free(line);
                continue;
            }
            free(line);
            line = xstrdup(g_hist[g_count - 1].command);
            cmd = line;
            printf("!! -> %s\n", cmd);
        }

        /* Built-ins run in the parent. cd has to: a child's directory change
         * dies with the child. */
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            free(line);
            break;
        }
        else if (strcmp(cmd, "help") == 0) { cmd_help(); handled = 1; }
        else if ((arg = builtin_arg(cmd, "history"))) { trim(arg); cmd_history(arg); handled = 1; }
        else if (strcmp(cmd, "stats") == 0)   { cmd_stats(); handled = 1; }
        else if ((arg = builtin_arg(cmd, "replay"))) { trim(arg); cmd_replay(arg); handled = 1; }
        else if ((arg = builtin_arg(cmd, "search"))) { trim(arg); cmd_search(arg); handled = 1; }
        else if ((arg = builtin_arg(cmd, "retry")))  { trim(arg); cmd_retry(arg);  handled = 1; }
        else if ((arg = builtin_arg(cmd, "export"))) { trim(arg); cmd_export(arg); handled = 1; }
        else if ((arg = builtin_arg(cmd, "mode")))   { trim(arg); cmd_mode(arg);   handled = 1; }
        else if (strcmp(cmd, "cd") == 0) {
            const char *home = getenv("HOME");
            if (!home) printf("cd: HOME is not set\n");
            else if (chdir(home) < 0) perror("cd");
            handled = 1;
        }
        else if (strncmp(cmd, "cd ", 3) == 0) {
            char *target = cmd + 3;
            trim(target);
            if (chdir(target) < 0) perror("cd");
            handled = 1;
        }
        else if (strcmp(cmd, "pwd") == 0) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof cwd)) printf("%s\n", cwd);
            else perror("getcwd");
            handled = 1;
        }
        /* 'timeout <sec> <cmd>' is a built-in wrapper: it arms alarm() and
         * then runs the remainder through the normal path. */
        else if ((arg = builtin_arg(cmd, "timeout"))) {
            char *p = arg, *rest;
            long sec = strtol(p, &rest, 10);
            if (rest == p || sec <= 0) {
                printf("timeout: expected a positive number of seconds, e.g. timeout 2 sleep 10\n");
            } else {
                while (*rest && isspace((unsigned char)*rest)) rest++;
                if (!*rest) printf("timeout: no command given\n");
                else run_and_log(rest, (int)sec);
            }
            handled = 1;
        }

        if (!handled) run_and_log(cmd, 0);

        free(line);
    }

    raw_disable();
    printf("[Session ended. %d %s recorded.]\n",
           g_count, g_count == 1 ? "command" : "commands");
    hist_free();
    return 0;
}
