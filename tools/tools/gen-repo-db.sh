#!/bin/sh
# gen-repo-db.sh — generate repo.db for lpm
#
# Usage:
#   ./tools/gen-repo-db.sh <repo-root> <repo-name>
#
# Example (run from lotus-repository/):
#   ./tools/gen-repo-db.sh . base
#   ./tools/gen-repo-db.sh . extra
#   ./tools/gen-repo-db.sh . lotus
#
# Output: <repo-root>/<repo-name>/repo.db
#
# repo.db format (one line per package):
#   pkgname=VER-REL pkgtype=binary|source dlsize=BYTES instsize=BYTES
#
# Binary packages: a .lpkg file must exist in the letter-bucket dir.
#   e.g. base/n/neofetch-7.1.0-1-x86_64.lpkg
# Source packages: only a pkgbuild_<name> file is needed.

set -e

REPO_ROOT="${1:-.}"
REPO_NAME="${2:-lotus}"
REPO_DIR="$REPO_ROOT/$REPO_NAME"
OUT="$REPO_DIR/repo.db"

if [ ! -d "$REPO_DIR" ]; then
    echo "error: repo dir not found: $REPO_DIR" >&2
    exit 1
fi

TMP="$OUT.tmp.$$"
: > "$TMP"

count=0
bin_count=0
src_count=0

for letter_dir in "$REPO_DIR"/*/; do
    [ -d "$letter_dir" ] || continue
    letter=$(basename "$letter_dir")
    # skip non-single-letter dirs (like .git)
    case "$letter" in
        [a-z0-9]) ;;
        *) continue ;;
    esac

    for pbfile in "$letter_dir"pkgbuild_*; do
        [ -f "$pbfile" ] || continue

        pkgname=""
        pkgver=""
        pkgrel=""
        pkgtype="source"   # default

        pkgdesc=""
        while IFS= read -r line; do
            case "$line" in
                pkgname=*)  pkgname="${line#pkgname=}" ;;
                pkgver=*)   pkgver="${line#pkgver=}" ;;
                pkgrel=*)   pkgrel="${line#pkgrel=}" ;;
                pkgtype=*)  pkgtype="${line#pkgtype=}" ;;
                pkgdesc=*)  pkgdesc="${line#pkgdesc=}"
                            # strip surrounding quotes if any
                            pkgdesc="${pkgdesc#[\'\"]}"
                            pkgdesc="${pkgdesc%[\'\"]}" ;;
            esac
        done < "$pbfile"

        [ -z "$pkgname" ] && continue
        [ -z "$pkgver"  ] && continue
        [ -z "$pkgrel"  ] && pkgrel="1"

        ver_rel="${pkgver}-${pkgrel}"

        dlsize=0
        instsize=0

        if [ "$pkgtype" = "binary" ] || [ "$pkgtype" = "bin" ]; then
            pkgtype="binary"
            # look for matching .lpkg file
            # pattern: <name>-<ver>-<rel>-<arch>.lpkg
            lpkg_file=""
            for f in "$letter_dir"${pkgname}-${pkgver}-${pkgrel}-*.lpkg; do
                [ -f "$f" ] && lpkg_file="$f" && break
            done
            if [ -n "$lpkg_file" ]; then
                dlsize=$(wc -c < "$lpkg_file" | tr -d ' ')
                # instsize: try to read from .lpkg manifest, else 3× dl
                instsize=$(( dlsize * 3 ))
            fi
            bin_count=$(( bin_count + 1 ))
        else
            pkgtype="source"
            src_count=$(( src_count + 1 ))
        fi

        # escape spaces/special chars in desc with %20 — keep format one-token
        desc_safe=$(printf "%s" "$pkgdesc" | sed "s/ /%20/g; s/	/%09/g")
        echo "${pkgname}=${ver_rel} pkgtype=${pkgtype} dlsize=${dlsize} instsize=${instsize} desc=${desc_safe}" >> "$TMP"
        count=$(( count + 1 ))
    done
done

# sort for reproducibility
sort "$TMP" > "$OUT"
rm -f "$TMP"

echo ":: Generated $OUT — $count packages ($bin_count binary, $src_count source)"
