#pragma once

#include <sys/types.h>

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0100  // create the file if it does not exist
#define O_TRUNC   01000 // empty it (when opened for writing)

// Open a file, directory or device: the lowest free descriptor. Files have
// no permissions yet, so `mode` (for O_CREAT) is ignored.
int open(const char *path, int flags, ...);
