#pragma once
// Error numbers. Kernel functions return them negated (-ENOENT). The values
// are Linux's, so user programs will see the numbers they expect.

#define EPERM        1
#define ENOENT       2
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD       10
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38

#ifndef __ASSEMBLER__
// A short description: "No such file or directory". `err` may be negated.
const char *strerror(int err);
#endif
