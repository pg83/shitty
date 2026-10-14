# MIT licensed; see the file LICENSE.MIT for the full license. Embedded in
# the binary of every brand, so it names none of them.
#
# Read because the terminal pointed ZDOTDIR here (shell_integration.cpp).
# First of all the user's ZDOTDIR goes back, so that every file zsh reads
# after this one - .zprofile, .zshrc, .zlogin - is the user's own, and so
# that no shell started from this one ever sees this directory.
if [[ -n "${TERMINAL_ZDOTDIR+x}" ]]; then
    ZDOTDIR="$TERMINAL_ZDOTDIR"
    unset TERMINAL_ZDOTDIR
else
    unset ZDOTDIR
fi

typeset -g __terminal_integration="${${(%):-%x}:A:h}/integration.zsh"

# The user's own .zshenv, where zsh would have found it.
if [[ -f "${ZDOTDIR:-$HOME}/.zshenv" ]]; then
    source "${ZDOTDIR:-$HOME}/.zshenv"
fi

if [[ -o interactive && -r "$__terminal_integration" ]]; then
    source "$__terminal_integration"
fi
