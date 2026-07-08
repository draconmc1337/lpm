#!/bin/sh
# gen-repo-db.sh — generate repo.db for lpm
#
# Usage (run from repo-lotus/):
#   ./gen-repo-db.sh base
#   ./gen-repo-db.sh extra
#   ./gen-repo-db.sh lotus
#   ./gen-repo-db.sh        # all repos
#
# Output: <repo>/repo.db

set -e

# ── parse one field from a PKGBUILD ─────────────────────────────────────
# Handles both formats:
#   key=value          (bash style)
#   key = "value"      (lpm style, with spaces + quotes)
parse_field() {
    local file="$1" key="$2"
    # match "key=val", "key =val", "key= val", "key = val", with/without quotes
    grep -m1 -E "^[[:space:]]*${key}[[:space:]]*=" "$file" \
        | sed -E "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*//" \
        | tr -d '"'"'" \
        | tr -d '\r'
}

# ── generate one repo ────────────────────────────────────────────────────
gen_repo() {
    local repo="$1"
    [ -d "$repo" ] || { echo "error: $repo not found" >&2; return 1; }

    local out="$repo/repo.db"
    local tmp="${out}.tmp.$$"
    : > "$tmp"

    local count=0 bin_count=0 src_count=0

    # find ALL pkgbuild_ files — handles both flat and bucket layout
    while IFS= read -r pbfile; do
        [ -f "$pbfile" ] || continue

        pkgname=$(parse_field "$pbfile" pkgname)
        pkgver=$(parse_field  "$pbfile" pkgver)
        pkgrel=$(parse_field  "$pbfile" pkgrel)
        pkgtype=$(parse_field "$pbfile" pkgtype)
        pkgdesc=$(parse_field "$pbfile" pkgdesc)

        # fallback: empty rel = 1
        [ -z "$pkgrel" ]  && pkgrel="1"
        # fallback: empty type = source
        [ -z "$pkgtype" ] && pkgtype="source"

        # skip if no name or version
        [ -z "$pkgname" ] && continue
        [ -z "$pkgver"  ] && continue

        ver_rel="${pkgver}-${pkgrel}"
        dlsize=0
        instsize=0

        case "$pkgtype" in
            binary|bin)
                pkgtype="binary"
                # look for matching .lpkg in same directory as pkgbuild
                pbdir=$(dirname "$pbfile")
                lpkg_file=""
                for f in "${pbdir}/${pkgname}-${pkgver}-${pkgrel}"-*.lpkg; do
                    [ -f "$f" ] && lpkg_file="$f" && break
                done
                if [ -n "$lpkg_file" ]; then
                    dlsize=$(wc -c < "$lpkg_file" | tr -d ' ')
                    instsize=$(( dlsize * 3 ))
                fi
                bin_count=$(( bin_count + 1 ))
                ;;
            *)
                pkgtype="source"
                src_count=$(( src_count + 1 ))
                ;;
        esac

        # encode desc: space→%20, tab→%09
        desc_safe=$(printf '%s' "$pkgdesc" | sed 's/ /%20/g; s/	/%09/g')

        printf '%s=%s pkgtype=%s dlsize=%s instsize=%s desc=%s\n' \
            "$pkgname" "$ver_rel" "$pkgtype" \
            "$dlsize"  "$instsize" "$desc_safe" >> "$tmp"

        count=$(( count + 1 ))
    done << FINDEOF
$(find "$repo" -type f -name 'pkgbuild_*' | sort)
FINDEOF

    sort "$tmp" > "$out"
    rm -f "$tmp"

    # ── sign repo.db ─────────────────────────────────────────────────────
    # Requires the Lotus Linux signing key to be present in /etc/lpm/gnupg.
    # Produces <repo>/repo.db.sig (detached, ASCII-armored).
    # Abort if signing fails — a repo.db without a matching .sig is rejected
    # by lpm update (clients verify before trusting any package metadata).
    KEYRING="/etc/lpm/gnupg"
    SIGFILE="${out}.sig"
    rm -f "$SIGFILE"
    if [ -d "$KEYRING" ]; then
        if gpg --homedir "$KEYRING" --batch --yes \
               --detach-sign --armor -o "$SIGFILE" "$out" 2>/dev/null; then
            printf '   signed  → %s\n' "$SIGFILE"
        else
            printf 'error: signing failed for %s — aborting\n' "$out" >&2
            rm -f "$out"
            return 1
        fi
    else
        printf 'warning: %s not found — repo.db NOT signed (run: lpm key init)\n' \
            "$KEYRING" >&2
    fi

    printf ':: %-8s → %s  (%d packages: %d binary, %d source)\n' \
        "$repo" "$out" "$count" "$bin_count" "$src_count"
}

# ── main ─────────────────────────────────────────────────────────────────
if [ $# -eq 0 ]; then
    # no args — generate all repos found in current directory
    found=0
    for d in base extra lotus */; do
        d="${d%/}"
        [ -d "$d" ] || continue
        # skip hidden dirs and non-repo dirs
        case "$d" in .*|tools|.github) continue ;; esac
        # must contain at least one pkgbuild_ file
        found_pb=$(find "$d" -maxdepth 2 -name 'pkgbuild_*' | head -1)
        [ -n "$found_pb" ] || continue
        gen_repo "$d"
        found=$(( found + 1 ))
    done
    [ "$found" -eq 0 ] && echo "warning: no repos found in current directory" >&2
else
    for repo in "$@"; do
        gen_repo "$repo"
    done
fi
