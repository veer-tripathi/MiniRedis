#pragma once

// Print a plain message to stderr.
void msg(const char *m);

// Print a message with the current errno value to stderr.
void msg_errno(const char *m);

// Print a message with errno and abort().  Marked [[noreturn]] so the
// compiler knows execution cannot continue past this call.
[[noreturn]] void die(const char *m);
