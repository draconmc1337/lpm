#!/bin/sh
# Exercise the real recipes with fixture artifacts, never host destinations.
set -eu
work=$(mktemp -d "${TMPDIR:-/tmp}/lpm-install-XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
mkdir -p "$work/bin" "$work/src"
cp Makefile "$work/src/Makefile"
export STAGE="$work/stage with spaces" VIOLATION="$work/violation"
export REAL_INSTALL=$(command -v install) REAL_LN=$(command -v ln)
export REAL_RM=$(command -v rm) REAL_RMDIR=$(command -v rmdir)
# Guard every mutating recipe command before forwarding to the real tool.
cat > "$work/bin/guard" <<'EOF'
#!/bin/sh
set -eu
cmd=${0##*/}
for arg do
    case "$arg" in
        "$STAGE"/*) ;;
        -*) ;;
        *)
            case "$cmd" in
                install|ln) case "$arg" in /*) : ;; *) continue ;; esac ;;
            esac
            echo "outside staging directory: $cmd $arg" >> "$VIOLATION"
            exit 1 ;;
    esac
done
case "$cmd" in
    install) exec "$REAL_INSTALL" "$@" ;;
    ln) exec "$REAL_LN" "$@" ;;
    rm) exec "$REAL_RM" "$@" ;;
    rmdir) exec "$REAL_RMDIR" "$@" ;;
    ldconfig) echo 'host ldconfig called' >> "$VIOLATION"; exit 1 ;;
esac
EOF
chmod +x "$work/bin/guard"
for cmd in install ln rm rmdir ldconfig; do
    ln -s guard "$work/bin/$cmd"
done
cd "$work/src"
mkdir -p include/llpm completions
files='lpm libllpm.so.1 libllpm.a lpm.conf lpm.1 lpm.conf.5 PKGBUILD.5
include/llpm/llpm.h include/llpm/error.h include/llpm/handle.h
include/llpm/repo.h include/llpm/trans.h include/llpm/dep.h
include/llpm/keyring.h completions/_lpm completions/lpm.bash'
for file in $files; do printf 'fixture: %s\n' "$file" > "$file"; done
run_make() {
    PATH="$work/bin:$PATH" make --no-print-directory -o all -o manpages \
        "DESTDIR=$STAGE" "$@" > "$work/make.log" 2>&1 || {
        cat "$work/make.log" >&2
        return 1
    }
    test ! -e "$VIOLATION"
}
run_make install
for dir in /usr/src/lpm /var/lib/lpm/db /var/lib/lpm/files \
    /var/lib/lpm/buildmeta /var/cache/lpm /var/cache/lpm/packages \
    /var/log/lpm /etc/lpm; do
    test -d "$STAGE$dir"
done
for file in $files; do
    case "$file" in
        lpm) dest=usr/bin/lpm ;;
        libllpm*) dest=usr/lib/$file ;;
        lpm.conf) dest=etc/lpm/lpm.conf ;;
        lpm.1) dest=usr/share/man/man1/$file ;;
        *.5) dest=usr/share/man/man5/$file ;;
        include/*) dest=usr/$file ;;
        completions/_lpm) dest=usr/share/zsh/site-functions/_lpm ;;
        completions/lpm.bash) dest=usr/share/bash-completion/completions/lpm ;;
    esac
    cmp "$file" "$STAGE/$dest"
done
test -x "$STAGE/usr/bin/lpm"
test "$(readlink "$STAGE/usr/lib/libllpm.so")" = libllpm.so.1
run_make uninstall
test -z "$(find "$STAGE/usr" -type f -o -type l)"
# Preserve existing uninstall policy: configuration and runtime state remain.
cmp lpm.conf "$STAGE/etc/lpm/lpm.conf"
test -d "$STAGE/var/lib/lpm/db"
echo 'test_install: staged install/uninstall passed (including DESTDIR spaces)'
