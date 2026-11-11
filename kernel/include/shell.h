#pragma once
// Kernel shell: a command line for poking at the system. The shell people
// use is /bin/sh, in user space; this one runs when /bin/init can't, and its
// commands are what /bin/sh runs for a word that isn't a program. It reads keys from the keyboard queue, edits the line (Backspace,
// Delete, Left/Right, Home/End, Up/Down through the history, Ctrl+C, Ctrl+L)
// and runs commands from the command table.
//
// Any file can add a command to the table:
//
//   static void cmd_lspci(int argc, char **argv) { ... }
//   SHELL_COMMAND(lspci, "", "list PCI devices");
//
// The linker gathers every SHELL_COMMAND into one array (the
// .shell_commands section, see linker.ld), so there is no list to edit.

#include <stdbool.h>

struct shell_command {
    const char *name;
    const char *usage; // the arguments, for `help`: "[fault]", or "" for none
    const char *help;  // one line
    void (*run)(int argc, char **argv); // argv[0] is the command's name
};

// Add a command: `name` is a bare word, and its function is cmd_<name>.
#define SHELL_COMMAND(name, usage, help)                                                                        \
    static void cmd_##name(int argc, char **argv);                                                             \
    __attribute__((used, section(".shell_commands"), aligned(8))) static const struct shell_command            \
        shell_command_##name = {#name, usage, help, cmd_##name}

// The command called `name`, or NULL. The user-space shell runs these
// through a system call (SYS_kernel_command).
const struct shell_command *shell_find_command(const char *name);

// Show the prompt and run commands forever. Needs the keyboard (ps2_init),
// the scheduler and interrupts on; between keys the shell's thread sleeps.
__attribute__((noreturn)) void shell_run(void);
