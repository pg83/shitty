/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <std/str/view.h>

namespace stl {
    class StringBuilder;
}

// Shell integration: the scripts that tell the terminal where a prompt's
// command line is and what is in it, for the command-line editor
// (lib/vterm/prompt_editor.h). zsh only, for now.
//
// Injected the way kitty and ghostty do it, with nothing for the user to
// source: the terminal writes a .zshenv of its own and points ZDOTDIR at
// it. That .zshenv puts the user's ZDOTDIR straight back - saved in
// TERMINAL_ZDOTDIR - reads the user's own .zshenv, and in an interactive
// shell loads the integration, which hooks in at the first prompt, after
// the user's .zshrc.

// Whether a shell path is zsh, by its base name: `/bin/zsh`, `zsh`, a login
// `-zsh`.
bool shellIsZsh(stl::StringView shell);

// Where the scripts are written: $TMPDIR, or /tmp, then
// `<identifier>-shell-<uid>/zsh`. Replaces what `out` held.
void shellIntegrationDirectory(stl::StringView identifier, stl::StringBuilder& out);

// Writes the scripts under `directory` - made for this user alone, and
// refused if it is someone else's or not a directory - and points ZDOTDIR
// at it, the user's own saved beside it. Before any thread exists: it
// calls setenv(). False, and the environment untouched, when anything
// could not be done; a shell without the integration is an ordinary one.
bool installShellIntegration(stl::StringView directory);
