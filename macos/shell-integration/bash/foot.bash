# foot's shell integration for bash: marks the prompts and the commands'
# output (OSC 133), and reports the working directory (OSC 7).
#
# bash has no startup file of its own for this. foot starts it in POSIX
# mode, where it reads only $ENV: this file (macos/shell-integration.c).
# POSIX mode is turned off again here, and bash's own startup files are
# read as bash would have read them.
# macos-shell-integration=no in foot.ini turns this off.

if [[ -n ${FOOT_BASH_INJECT-} ]]; then
    builtin unset FOOT_BASH_INJECT

    if [[ -n ${FOOT_BASH_ENV+set} ]]; then
        ENV=$FOOT_BASH_ENV
        builtin unset FOOT_BASH_ENV
    else
        builtin unset ENV
    fi

    builtin set +o posix

    # POSIX mode's default; the history isn't read yet
    [[ ${HISTFILE-} == "$HOME/.sh_history" ]] && HISTFILE=$HOME/.bash_history

    if builtin shopt -q login_shell; then
        [[ -r /etc/profile ]] && builtin source /etc/profile
        for _foot_file in "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.profile"; do
            if [[ -r $_foot_file ]]; then
                builtin source "$_foot_file"
                break
            fi
        done
    else
        # Where distributions have bash's system-wide one
        for _foot_file in /etc/bash.bashrc /etc/bash/bashrc; do
            if [[ -r $_foot_file ]]; then
                builtin source "$_foot_file"
                break
            fi
        done
        [[ -r $HOME/.bashrc ]] && builtin source "$HOME/.bashrc"
    fi
    builtin unset _foot_file
fi

if [[ $- == *i* && -t 1 && -z ${_foot_integration-} ]]; then
    _foot_integration=1
    _foot_executing=
    _foot_pwd=
    _foot_url=

    # First in PROMPT_COMMAND: the exit status is still the command's
    _foot_prompt() {
        builtin local ret=$?

        # The end of a command's output, with its exit status
        if [[ -n $_foot_executing ]]; then
            builtin printf '\e]133;D;%s\a' "$ret"
            _foot_executing=
        fi

        # The working directory, as a file:// URL
        if [[ $PWD != "$_foot_pwd" ]]; then
            builtin local LC_ALL=C i c
            _foot_pwd=$PWD
            _foot_url=
            for ((i = 0; i < ${#PWD}; i++)); do
                c=${PWD:i:1}
                case $c in
                [-/:_.~a-zA-Z0-9]) ;;
                *) builtin printf -v c '%%%02X' "'$c" ;;
                esac
                _foot_url+=$c
            done
        fi
        builtin printf '\e]7;file://%s%s\e\\' "$HOSTNAME" "$_foot_url"

        builtin return "$ret"
    }

    # Last in PROMPT_COMMAND, as the others may set the prompt. The
    # start of the prompt: in the prompt itself, so that it's there
    # again when bash redraws it
    _foot_prompt_mark() {
        builtin local ret=$?
        [[ $PS1 == *'\e]133;A\a'* ]] || PS1='\[\e]133;A\a\]'$PS1
        builtin return "$ret"
    }

    if [[ $(builtin declare -p PROMPT_COMMAND 2>/dev/null) == 'declare -a'* ]]; then
        PROMPT_COMMAND=(_foot_prompt "${PROMPT_COMMAND[@]}" _foot_prompt_mark)
    else
        PROMPT_COMMAND="_foot_prompt${PROMPT_COMMAND:+; $PROMPT_COMMAND}; _foot_prompt_mark"
    fi

    # The start of a command's output: bash 4.4's PS0. The subscript is
    # evaluated in this shell, and notes that a command runs
    if ((BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4))) &&
        builtin shopt -q promptvars
    then
        _foot_empty=('')
        PS0='${_foot_empty[_foot_executing=1, 0]}\e]133;C\a'${PS0-}
    fi
fi
