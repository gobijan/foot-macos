# foot's shell integration for zsh: marks the prompts and the commands'
# output (OSC 133), and reports the working directory (OSC 7).
#
# foot starts zsh with ZDOTDIR set to this file's directory
# (macos/shell-integration.c), so that zsh reads this file instead of
# the user's .zshenv. That one is read from here, with ZDOTDIR as it
# was; zsh then goes on with the user's other startup files.
# macos-shell-integration=no in foot.ini turns this off.

if [[ -o interactive && -t 1 ]]; then
    # Runs after the other precmd hooks: they may set the prompt
    _foot_precmd() {
        builtin local ret=$? percent=
        [[ -o prompt_percent ]] && percent=1
        builtin emulate -L zsh
        builtin setopt extended_glob

        if [[ ${precmd_functions[-1]} != _foot_precmd ]]; then
            precmd_functions=(${precmd_functions:#_foot_precmd} _foot_precmd)
        fi

        # The end of a command's output, with its exit status
        if [[ -n $_foot_executing ]]; then
            builtin print -n "\e]133;D;$ret\a"
            _foot_executing=
        fi

        # The working directory, as a file:// URL
        builtin local LC_ALL=C
        builtin printf '\e]7;file://%s%s\e\\' "$HOST" \
            "${PWD//(#m)([^@-Za-z&-;_~])/%${(l:2::0:)$(([##16]#MATCH))}}"

        # The start of the prompt: in the prompt itself, so that it's
        # there again when zsh redraws it
        if [[ -z $percent ]]; then
            builtin print -n '\e]133;A\a'
        elif [[ $PS1 != *$'\e]133;A\a'* ]]; then
            PS1=$'%{\e]133;A\a%}'$PS1
        fi
    }

    # The start of a command's output
    _foot_preexec() {
        builtin print -n '\e]133;C\a'
        builtin typeset -g _foot_executing=1
    }

    builtin typeset -g _foot_executing=
    precmd_functions+=(_foot_precmd)
    preexec_functions+=(_foot_preexec)
fi

# The user's ZDOTDIR, and their .zshenv
if [[ -n ${FOOT_ZSH_ZDOTDIR+set} ]]; then
    builtin export ZDOTDIR=$FOOT_ZSH_ZDOTDIR
    builtin unset FOOT_ZSH_ZDOTDIR
else
    builtin unset ZDOTDIR
fi

[[ ! -r ${ZDOTDIR-$HOME}/.zshenv ]] || builtin source -- "${ZDOTDIR-$HOME}/.zshenv"
