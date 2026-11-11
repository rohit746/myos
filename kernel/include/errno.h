#pragma once
// Error numbers. Kernel functions return them negated (-ENOENT). The values
// are Linux's, so user programs will see the numbers they expect.

#define EPERM        1
#define ENOENT       2
#define EIO          5
#define EBADF        9
#define ENOMEM       12
#define EBUSY        16
#define EEXIST       17
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define EROFS        30
#define ENAMETOOLONG 36

// A short description: "No such file or directory". `err` may be negated.
const char *strerror(int err);
