#pragma once

// Serial console: line editor with insert/delete/history + arrow keys, plus
// the top-level command dispatcher. The full line editor is here because
// PuTTY-style history/edit is a lot of code to inline in main.

namespace console {

// Print the boot banner ("<board name> <chip> ... Type 'help' for commands.").
void print_banner();

// Poll Serial for input. Handles ANSI escape sequences, edits, and
// dispatches complete lines to the command handlers. Non-blocking.
void poll();

}  // namespace console
