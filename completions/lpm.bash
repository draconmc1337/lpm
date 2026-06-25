# lpm bash completion
# install: cp lpm.bash /usr/share/bash-completion/completions/lpm
# or: source this file in ~/.bashrc

# installed package names (for remove/verify/files/owns context)
_lpm_installed_pkgs() {
    lpm list 2>/dev/null | awk '{print $1}'
}

# cached PKGBUILD package names (for install/info/deps/test)
_lpm_cached_pkgs() {
    ls /usr/src/lpm 2>/dev/null | sed -n 's/^pkgbuild_//p'
}

_lpm_complete() {
    local cur prev
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    local commands="install remove upgrade update search info deps list owns files orphans cache verify test audit bootstrap repo package key -h --help -V --version"
    local global_opts="--no-confirm --no-check --dry-run --force --debug="

    # first arg: complete top-level commands
    if [[ ${COMP_CWORD} -eq 1 ]]; then
        COMPREPLY=( $(compgen -W "$commands" -- "$cur") )
        return 0
    fi

    local cmd="${COMP_WORDS[1]}"

    case "$cmd" in
        install|info|deps|test)
            # package names known via cached PKGBUILDs + global flags
            COMPREPLY=( $(compgen -W "$(_lpm_cached_pkgs) $global_opts" -- "$cur") )
            ;;

        remove|files)
            COMPREPLY=( $(compgen -W "$(_lpm_installed_pkgs) $global_opts" -- "$cur") )
            ;;

        upgrade)
            COMPREPLY=( $(compgen -W "$(_lpm_installed_pkgs) $global_opts" -- "$cur") )
            ;;

        verify)
            if [[ "$cur" == --* ]]; then
                COMPREPLY=( $(compgen -W "--deps $global_opts" -- "$cur") )
            else
                COMPREPLY=( $(compgen -W "$(_lpm_installed_pkgs)" -- "$cur") )
            fi
            ;;

        cache)
            COMPREPLY=( $(compgen -W "clean clear" -- "$cur") )
            ;;

        repo)
            COMPREPLY=( $(compgen -W "list add remove sync" -- "$cur") )
            ;;

        bootstrap)
            # lpm bootstrap -C <target> [pkg...]
            if [[ "$prev" == "bootstrap" ]]; then
                COMPREPLY=( $(compgen -W "-C" -- "$cur") )
            elif [[ "$prev" == "-C" ]]; then
                # complete target directory paths
                COMPREPLY=( $(compgen -d -- "$cur") )
            else
                COMPREPLY=( $(compgen -W "$(_lpm_cached_pkgs) $global_opts" -- "$cur") )
            fi
            ;;

        package)
            COMPREPLY=( $(compgen -W "build pack install query extract verify remove help" -- "$cur") )
            ;;

        key)
            COMPREPLY=( $(compgen -W "init genid list recv import trust help" -- "$cur") )
            ;;

        owns)
            # complete file paths on the live filesystem
            COMPREPLY=( $(compgen -f -- "$cur") )
            ;;
    esac

    return 0
}

complete -F _lpm_complete lpm
