#pragma once

// Serial console: line editor with insert/delete/history + arrow keys, plus
// the top-level command dispatcher. The full line editor is here because
// PuTTY-style history/edit is a lot of code to inline in main.

namespace console {

// Print the boot banner ("<board name> <chip> ... Type 'help' for commands.").
void print_banner();

// Handle one received character: ANSI escape sequences, edits, and
// dispatching a complete line to the command handlers.
//
// The console no longer reads Serial itself. The port is shared with the
// binary protocol, so node.cpp owns the reads and routes each byte to whoever
// it belongs to -- see node.cpp for why that split is safe.
void feed(char c);

}  // namespace console
