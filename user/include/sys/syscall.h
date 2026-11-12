#pragma once
// System call numbers: the same as kernel/include/syscall.h (Linux's, and
// myos's own from 500).

#define SYS_read           0
#define SYS_write          1
#define SYS_open           2
#define SYS_close          3
#define SYS_mmap           9
#define SYS_dup            32
#define SYS_dup2           33
#define SYS_nanosleep      35
#define SYS_getpid         39
#define SYS_socket         41
#define SYS_connect        42
#define SYS_sendto         44
#define SYS_recvfrom       45
#define SYS_bind           49
#define SYS_setsockopt     54
#define SYS_exit           60
#define SYS_wait4          61
#define SYS_getppid        110
#define SYS_clock_gettime  228
#define SYS_spawn          500
#define SYS_kernel_command 501
