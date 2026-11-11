// sh: the shell. /bin/init runs it on the console.
//
// It reads keys raw from stdin and edits the line itself, the way the
// kernel shell does (Backspace, Delete, Left/Right, Home/End, Up/Down
// through the history, Ctrl+A/E/U/C/L), then splits it into words (double
// quotes keep spaces in one) and runs it:
//   - `help` and `exit [status]` are the shell's own
//   - a word with a slash runs the program at that path
//   - another word runs /bin/<word> if there is one, or else the kernel
//     shell's command of that name (mem, ps, reboot, ...: shell.h)
// A program runs in a child process, and the shell waits for it; one that
// crashes is killed by the kernel, and the shell says so and carries on.
// `> file` sends a program's stdout to the file.
//
// The line being edited never wraps: it is cut to what fits on one row after
// the prompt ($COLUMNS, from the kernel), so every change can be shown by
// redrawing that row ("\r", prompt, text, erase to the end, move the cursor
// back).

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROMPT     "\033[36mmyos>\033[0m "
#define PROMPT_LEN 6 // what PROMPT shows, without its escape codes

#define LINE_MAX     200
#define HISTORY_SIZE 32
#define MAX_ARGS     16
#define PATH_MAX     256

static char line[LINE_MAX + 1]; // what is being typed, NUL-terminated
static size_t len, pos;         // its length, and the cursor's place in it
static size_t limit;            // the longest line that fits on one row

// The last HISTORY_SIZE lines run, oldest first in the ring.
static char history[HISTORY_SIZE][LINE_MAX + 1];
static unsigned history_count; // lines added since the shell started
static unsigned browsing;      // 0: a new line; n: showing the n-th newest history line
static char draft[LINE_MAX + 1]; // the new line, kept while browsing the history

// ---------------------------------------------------------------------------
// Line editing
// ---------------------------------------------------------------------------

static void redraw(void) {
    char back[16] = "";
    if (pos < len) snprintf(back, sizeof back, "\033[%zuD", len - pos);
    printf("\r" PROMPT "%s\033[K%s", line, back);
}

static void set_line(const char *s) {
    len = strlen(s);
    if (len > limit) len = limit;
    memcpy(line, s, len);
    line[len] = '\0';
    pos = len;
}

static void new_line(void) {
    set_line("");
    browsing = 0;
    printf(PROMPT);
}

static void insert(char c) {
    if (len == limit) return;
    memmove(line + pos + 1, line + pos, len - pos + 1);
    line[pos++] = c;
    len++;
}

// Delete `n` characters starting at `at`.
static void remove_chars(size_t at, size_t n) {
    memmove(line + at, line + at + n, len - at - n + 1);
    len -= n;
}

// The n-th newest line in the history, from 1.
static const char *history_line(unsigned n) { return history[(history_count - n) % HISTORY_SIZE]; }

static void history_add(const char *s) {
    if (s[0] == '\0') return;
    if (history_count > 0 && strcmp(history_line(1), s) == 0) return;
    strcpy(history[history_count % HISTORY_SIZE], s);
    history_count++;
}

// Up (`older`) or Down through the history. Down from the newest line
// brings back the line that was being typed.
static void history_move(bool older) {
    unsigned kept = history_count < HISTORY_SIZE ? history_count : HISTORY_SIZE;
    if (older ? browsing == kept : browsing == 0) return;
    if (browsing == 0) memcpy(draft, line, len + 1);
    if (older) browsing++;
    else browsing--;
    set_line(browsing ? history_line(browsing) : draft);
}

// ---------------------------------------------------------------------------
// Running commands
// ---------------------------------------------------------------------------

// Split `s` into words in place. Spaces separate words, except inside
// double quotes, which are removed. Returns the number of words, or -1 if
// there are more than `max`.
static int split(char *s, char **argv, int max) {
    int argc = 0;
    for (;;) {
        while (*s == ' ') s++;
        if (*s == '\0') return argc;
        if (argc == max) return -1;
        argv[argc++] = s;
        char *out = s;
        bool quoted = false;
        for (; *s && (quoted || *s != ' '); s++) {
            if (*s == '"') quoted = !quoted;
            else *out++ = *s;
        }
        if (*s) s++; // past the space; `out` is behind it, so ending the word there is safe
        *out = '\0';
    }
}

// Run the program for argv[0] with stdout sent to `out_path` (if not NULL),
// and wait for it. False if argv[0] is a bare word and /bin has no such
// program, so the caller can try a kernel command.
static bool run_program(char **argv, const char *out_path) {
    bool is_path = strchr(argv[0], '/') != NULL;
    char path[PATH_MAX];
    snprintf(path, sizeof path, is_path ? "%s" : "/bin/%s", argv[0]);

    // The child gets the shell's descriptors, so point the shell's stdout
    // at the file while it starts, and put it back afterwards.
    int saved = -1;
    if (out_path) {
        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            printf("%s: %s\n", out_path, strerror(errno));
            return true;
        }
        saved = dup(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    pid_t pid = spawn(path, argv, environ);
    int err = errno;
    if (saved >= 0) {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    if (pid < 0) {
        if (err == ENOENT && !is_path) return false;
        printf("%s: %s\n", argv[0], strerror(err));
        return true;
    }

    int status;
    if (waitpid(pid, &status, 0) != pid) printf("%s: waitpid: %s\n", argv[0], strerror(errno));
    else if (WIFSIGNALED(status))
        printf("%s: killed by signal %d%s\n", argv[0], WTERMSIG(status),
               WTERMSIG(status) == SIGSEGV ? " (segmentation fault)" : "");
    else if (WEXITSTATUS(status)) printf("%s: exited with status %d\n", argv[0], WEXITSTATUS(status));
    return true;
}

static void help(void) {
    printf("A word with a slash runs that program (/bin/echo hi); another word runs /bin/<word>, or else\n"
           "the kernel command of that name:\n");
    char *argv[] = {"help", NULL};
    kernel_command(argv);
    printf("The shell's own: help, exit [status] (init then starts a new shell).\n"
           "`program args > file` sends what the program prints to the file.\n"
           "Keys: Left/Right, Home/End, Backspace/Delete, Up/Down for the history, Ctrl+C drops the line,\n"
           "Ctrl+U deletes to its start, Ctrl+L clears the screen.\n");
}

static void run(char *s) {
    char *argv[MAX_ARGS + 1];
    int argc = split(s, argv, MAX_ARGS);
    if (argc < 0) {
        printf("too many words (at most %d)\n", MAX_ARGS);
        return;
    }
    // Take out `> file` (or `>file`).
    const char *out_path = NULL;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '>') {
            argv[n++] = argv[i];
        } else if (argv[i][1]) {
            out_path = argv[i] + 1;
        } else if (i + 1 < argc) {
            out_path = argv[++i];
        } else {
            printf("sh: `>` needs a file name\n");
            return;
        }
    }
    argc = n;
    argv[argc] = NULL;
    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0) {
        help();
    } else if (strcmp(argv[0], "exit") == 0) {
        exit(argc > 1 ? atoi(argv[1]) : 0);
    } else if (!run_program(argv, out_path)) {
        if (out_path) printf("%s: `>` only works for programs; kernel commands print to the console\n", argv[0]);
        else if (kernel_command(argv) < 0) printf("%s: unknown command; `help` lists them\n", argv[0]);
    }
}

static void enter(void) {
    // Show the whole line, with the cursor at its end, before moving on.
    pos = len;
    redraw();
    printf("\n");
    history_add(line);
    run(line); // may cut `line` into words
    new_line();
}

// One key: a character, or the final byte of an ESC [ sequence (`seq`).
static void key(char c, bool seq, int param) {
    if (seq) {
        switch (c) {
        case 'A': history_move(true); break;
        case 'B': history_move(false); break;
        case 'C': if (pos < len) pos++; break;
        case 'D': if (pos > 0) pos--; break;
        case 'H': pos = 0; break;
        case 'F': pos = len; break;
        case '~':
            if (param == 3 && pos < len) remove_chars(pos, 1); // Delete
            else if (param == 1 || param == 7) pos = 0;        // Home, as some terminals send it
            else if (param == 4 || param == 8) pos = len;      // End
            break;
        default: return;
        }
        redraw();
        return;
    }
    switch (c) {
    case '\n':
    case '\r': enter(); return;
    case '\b':
    case 0x7f:
        if (pos > 0) remove_chars(--pos, 1);
        break;
    case 'a' & 0x1f: pos = 0; break;   // Ctrl+A: start of line
    case 'e' & 0x1f: pos = len; break; // Ctrl+E: end of line
    case 'u' & 0x1f: // Ctrl+U: delete to the start of the line
        remove_chars(0, pos);
        pos = 0;
        break;
    case 'c' & 0x1f: // Ctrl+C: drop the line
        printf("^C\n");
        new_line();
        return;
    case 'l' & 0x1f: // Ctrl+L: clear the screen, keep the line
        printf("\033[2J\033[H");
        break;
    default:
        if (c >= ' ' && c <= '~') insert(c);
        else return;
    }
    redraw();
}

int main(void) {
    const char *columns = getenv("COLUMNS");
    unsigned long cols = columns ? strtoul(columns, NULL, 10) : 0; // none: a serial terminal wraps itself
    limit = LINE_MAX;
    if (cols > PROMPT_LEN + 16 && cols - PROMPT_LEN - 1 < limit) limit = cols - PROMPT_LEN - 1;

    new_line();
    enum { TEXT, ESC, CSI } state = TEXT;
    int param = 0;
    for (;;) {
        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) return n < 0 ? 1 : 0; // no more input
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (state == ESC) {
                state = c == '[' ? CSI : TEXT;
                param = 0;
            } else if (state == CSI) {
                if (c >= '0' && c <= '9') {
                    param = param * 10 + (c - '0');
                } else {
                    state = TEXT;
                    key(c, true, param);
                }
            } else if (c == 0x1b) {
                state = ESC;
            } else {
                key(c, false, 0);
            }
        }
    }
}
