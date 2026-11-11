# foot's shell integration for fish: marks the prompts and the commands'
# output (OSC 133), and reports the working directory (OSC 7).
#
# fish reads this file because foot starts it with this directory in
# XDG_DATA_DIRS (macos/shell-integration.c), which is put back here.
# fish 4 does all of the rest itself; this is for older ones.
# macos-shell-integration=no in foot.ini turns this off.

if set -q FOOT_FISH_XDG_DATA_DIR
    if set -q FOOT_FISH_XDG_UNSET
        set -e -g XDG_DATA_DIRS
    else if set -q XDG_DATA_DIRS
        set -l dirs (string split : -- $XDG_DATA_DIRS)
        if set -l i (contains -i -- $FOOT_FISH_XDG_DATA_DIR $dirs)
            set -e dirs[$i]
        end
        set -g -x XDG_DATA_DIRS (string join : -- $dirs)
    end
    set -e -g FOOT_FISH_XDG_DATA_DIR
    set -e -g FOOT_FISH_XDG_UNSET
end

if status is-interactive; and string match -rq '^[1-3]\.' -- $version
    function __foot_prompt --on-event fish_prompt
        printf '\e]133;A\a'
    end

    function __foot_preexec --on-event fish_preexec
        printf '\e]133;C\a'
    end

    function __foot_postexec --on-event fish_postexec
        printf '\e]133;D;%s\a' $status
    end

    function __foot_report_pwd --on-variable PWD
        status is-command-substitution; and return
        printf '\e]7;file://%s%s\e\\' $hostname (string escape --style=url -- $PWD)
    end

    __foot_report_pwd
end
