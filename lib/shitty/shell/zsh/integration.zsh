# MIT licensed; see the file LICENSE.MIT for the full license. Embedded in
# the binary of every brand, so it names none of them.
#
# zsh integration for the terminal's command-line editor
# (lib/vterm/prompt_editor.h). zsh keeps the line: this only tells the
# terminal what the line is and where it starts, and takes a new line back.
#
# - OSC 133 A, B, C and D: the prompt, where the input starts, the command
#   running, and its end.
# - OSC 7701 on every redraw of the line: `c=<CURSOR>;<BUFFER>`, the buffer
#   with its controls and backslashes written \xNN.
# - The key ESC [ 7701 ~ sets the line: `<cursor>:<escaped buffer>` follows
#   it, up to BEL. ESC [ 7702 ~ is redo and ESC [ 7703 ~ undo, bound here in
#   every keymap: ctrl+_ is undo only in emacs mode, and only until a plugin
#   takes it.
#
# The hooks go in at the first prompt, after the user's .zshrc: a keymap it
# picks (bindkey -v) and the widgets its plugins wrap are in place by then.

[[ -n "$__terminal_integrated" ]] && return
typeset -g __terminal_integrated=1
typeset -g __terminal_reported=""
typeset -g __terminal_ran=""
typeset -g __terminal_marked=""

autoload -Uz add-zsh-hook add-zle-hook-widget

# The buffer as the terminal reads it back: \xNN for the backslash, every
# C0 control and DEL, and nothing else touched.
__terminal_escape() {
    local text="$1" code char
    text="${text//\\/\\x5c}"
    for code in {1..31} 127; do
        char="${(#)code}"
        [[ "$text" == *"$char"* ]] && text="${text//$char/\\x$(( [##16] code >> 4 ))$(( [##16] code & 15 ))}"
    done
    REPLY="$text"
}

__terminal_report() {
    # The input starts where the cursor stands at the prompt's first report:
    # line-init runs after the prompt is drawn, and every later redraw comes
    # before the keystroke it redraws for is echoed. Marked here rather than
    # in line-init alone, since a plugin that does `zle -N zle-line-init`
    # after us takes that hook away while this one keeps running.
    if [[ -z "$__terminal_marked" ]]; then
        __terminal_marked=1
        builtin printf '\e]133;B\a' >"$TTY"
    fi
    __terminal_escape "$BUFFER"
    local report="c=$CURSOR;$REPLY"
    [[ "$report" == "$__terminal_reported" ]] && return
    __terminal_reported="$report"
    builtin printf '\e]7701;%s\a' "$report" >"$TTY"
}

__terminal_line_init() {
    __terminal_reported=""
    __terminal_report
}

__terminal_line_finish() {
    __terminal_report
    __terminal_reported=""
}

__terminal_set_line() {
    local key data=""
    while builtin read -rsk 1 key; do
        [[ "$key" == $'\a' ]] && break
        data+="$key"
    done
    local cursor="${data%%:*}"
    local text="${data#*:}"
    builtin printf -v BUFFER '%b' "$text"
    [[ "$cursor" == <-> ]] && CURSOR="$cursor"
    __terminal_report
}

__terminal_preexec() {
    builtin printf '\e]133;C\a' >"$TTY"
    __terminal_ran=1
}

__terminal_precmd() {
    local status_=$?
    if [[ -n "$__terminal_ran" ]]; then
        builtin printf '\e]133;D;%s\a' "$status_" >"$TTY"
        __terminal_ran=""
    fi
    builtin printf '\e]133;A\a' >"$TTY"
    __terminal_marked=""
    __terminal_reported=""
    __terminal_install
}

__terminal_install() {
    [[ -n "$__terminal_installed" ]] && return
    typeset -g __terminal_installed=1
    zle -N __terminal_line_init
    zle -N __terminal_line_finish
    zle -N __terminal_report
    zle -N __terminal_set_line
    add-zle-hook-widget line-init __terminal_line_init
    add-zle-hook-widget line-finish __terminal_line_finish
    add-zle-hook-widget line-pre-redraw __terminal_report
    local keymap
    for keymap in emacs viins vicmd; do
        bindkey -M "$keymap" '\e[7701~' __terminal_set_line
        bindkey -M "$keymap" '\e[7702~' redo
        bindkey -M "$keymap" '\e[7703~' undo
    done
}

add-zsh-hook preexec __terminal_preexec
add-zsh-hook precmd __terminal_precmd
