#pragma once

#include <sys/types.h>

#define WNOHANG 1 // return 0 at once if no child has ended

// What waitpid's status says: how the child ended.
#define WIFEXITED(s)   (((s) & 0x7f) == 0)   // it called exit()
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)   // ... with this status
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)   // it was killed (signal.h)
#define WTERMSIG(s)    ((s) & 0x7f)          // ... by this signal

// Wait for the child `pid` (or any child, with -1) to end, and reap it: its
// pid. -1 with ECHILD if there is no such child; 0 with WNOHANG if it
// hasn't ended yet.
pid_t waitpid(pid_t pid, int *status, int options);
