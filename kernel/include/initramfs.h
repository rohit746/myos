#pragma once
// The initramfs: a USTAR (POSIX tar) archive that Limine loads next to the
// kernel as a module (module_path: in limine.conf), unpacked into the root
// tmpfs at boot. `make` builds it from the initramfs/ directory.

#include <stdbool.h>
#include <stddef.h>

// Unpack the archive at `tar` (`size` bytes) into "/": regular files and
// directories, making missing parent directories. Other entries (links,
// devices) are skipped and counted. Writes a line for the boot log saying
// what it did, or what was wrong with the archive. False on a bad archive;
// what came before the problem stays unpacked.
bool initramfs_unpack(const void *tar, size_t size, char *line, size_t line_size);
