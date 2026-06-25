# lpm bash completion
# install: cp lpm.bash /usr/share/bash-completion/completions/lpm
# or: source this file in ~/.bashrc

_lpm_complete() {
    local cur prev words
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    local commands="install remove upgrade update search info deps list owns files orphans cache verify audit repo -P -K -h --help -V --version"
    local global_opts="--no-confirm --no-check --dry-run --force --debug="

    # first arg: complete commands
    if [[ ${COMP_CWORD} -eq 1 ]]; then
        COMPREPLY=( $(compgen -W "$commands" -- "$cur") )
        return 0
    fi

    local cmd="${COMP_WORDS[1]}"

    case "$cmd" in
        install|remove|upgrade|info|deps|verify|files)
            COMPREPLY=( $(compgen -W "$global_opts" -- "$cur") )
            ;;

        cache)
            COMPREPLY=( $(compgen -W "clean clear" -- "$cur") )
            ;;

        repo)
            COMPREPLY=( $(compgen -W "list add remove sync" -- "$cur") )
            ;;

        -P)
            COMPREPLY=( $(compgen -W "build pack install query extract verify remove help" -- "$cur") )
            ;;

        -K)
            COMPREPLY=( $(compgen -W "init genid list recv import trust help" -- "$cur") )
            ;;

        owns)
            # complete file paths
            COMPREPLY=( $(compgen -f -- "$cur") )
            ;;
    esac

    return 0
}

complete -F _lpm_complete lpm
