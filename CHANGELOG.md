# Changelog

## [1.5.0] — 2026-07-05

### Changed — PKGBUILD source/checksum format (BREAKING)

Lotus's own PKGBUILD spec replaces Arch-style scalar source fields with
plain bash arrays. **Old-format PKGBUILDs must be migrated** — the parser
no longer recognizes the fields removed below.

- **Removed**: `source`, `source2`, `source3`, `sha256sums`, `sha512sums`,
  `md5sums`. These no longer exist anywhere in the parser (bash or fast C) —
  a PKGBUILD still using them will simply have no sources/checksums parsed.
- **Added**: `sources=(...)` and `checksums=(...)`, both plain bash arrays,
  parsed with the existing `bash_array()` (bash parser) / `parse_array()`
  (fast C parser) — no bespoke per-field parsing loops. `checksums[i]`
  describes `sources[i]`, same index; each entry is `"algo:hex"`
  (`sha512:`/`sha256:`/`md5:`) or `"SKIP"`. This format already existed
  as the unified `checksums[]` field and `checksum_parse_unified()` — this
  release is what actually wires the parsers up to populate it.
- **Added**: `conflicts=(...)` and `backup=(...)` are now parsed the same
  way for the bash-parser `Pkg` struct (previously only available via the
  fast C parser's `PkgMeta`, or not at all for `conflicts`/`backup` on the
  `Pkg` side).
- Both array formats work: multi-line (one element per line, blank lines
  and all) and single-line `sources=("a" "b")`. Whitespace alone separates
  elements; a semicolon/comma between them is tolerated but was never
  required and still isn't.

### Fixed — two bugs found while doing this refactor

- **Silent truncation of long checksums/URLs.** The fast C parser staged
  `sources`/`checksums` array elements through a 128-byte-wide scratch
  buffer before copying them into their real (4096- and 200-byte) fields.
  A `sha512:` checksum is 135 characters — every SHA-512 checksum was
  being silently cut to 128 characters before this fix, which would have
  made every SHA-512-verified source fail with a bogus mismatch the first
  time someone actually used one.
- **Latent overflow risk in `bash_array()`.** It always read up to 4096
  bytes (`MAX_STR`) per line regardless of the destination row's actual
  width (128 bytes for `depends`/`makedepends`/etc.), and wrote into rows
  typed as exactly that width. Harmless in practice so far because no
  dependency name has ever approached 128 bytes, but not something to
  leave in place while extending the same function to more callers.
  `bash_array()` and `parse_array()` now take an explicit row-size
  parameter and read through a fixed-size local buffer first, so neither
  direction (truncate a wide row, overflow a narrow one) is possible
  regardless of which array a caller points them at.
- `pkgbuild_parse_fast()`'s bash-fallback path (used when the C parser
  can't handle a PKGBUILD) never copied `conflicts[]`/`nconflicts` into
  `PkgMeta` even though both already existed there — a pre-existing gap,
  unrelated to this refactor, closed while touching the same code.

### Housekeeping

- `Pkg`'s legacy `sha256sums`/`sha512sums`/`md5sums` fields removed
  (dead now that nothing populates them); added `nchecksums`,
  `conflicts[]`/`nconflicts`, `backup[]`/`nbackup`.
- `PkgMeta`'s on-disk cache layout changed (`nchecksums`, `backup[]`,
  `nbackup` added) — `LPM_META_VERSION` bumped 5 → 6 so old `.meta`
  cache files are correctly rejected and re-parsed rather than misread.
- `build.c`'s `hash_detect()` / `pick_checksum()` (auto-detect algorithm
  by hex length, fall back across three legacy fields) removed — dead
  now that there's exactly one checksum field with an explicit prefix.
  `verify_sources()` now delegates actual hashing to `cksum_verify()`
  (`checksum.c`) instead of re-invoking `sha256sum`/`sha512sum`/`md5sum`
  itself — one implementation of "hash a file and compare" instead of two.
- README's PKGBUILD example updated to the new format.

## [1.4.2] — 2026-07-04

### Fixed — stale PKGBUILD caching in the build pipeline

- **build.c: `fetch_pkgbuilds_parallel()` ROUND 0** treated any on-disk
  `pkgbuild_<name>` as fresh forever (existence + size > 16 bytes, no
  version/mtime check). A PKGBUILD fetched once — however old, however
  wrong — was never re-validated, only ever refreshed by the separate
  `lpm update`/`-Suy` diff path. Root cause confirmed by tracing
  `pkg.source[]` end-to-end through the parser, `FetchJob`, and the
  downloader: no code transformed a URL into a relative path — the
  pipeline was correctly parsing a stale file that never got re-fetched.
  Fixed by cross-checking the on-disk file's own pkgver-pkgrel (via the
  existing cache-aware `pkgbuild_parse_fast()`, no bash spawn) against
  the last-synced repo.db — a pure local comparison, no added network
  I/O — via new `pkgbuild_on_disk_is_stale()` / `pkgbuild_needs_fetch()`.
- **build.c: same "exists on disk = trust forever" check was duplicated
  three more times** — `build_queue()` (transitive deps), `cmd_install()`
  STEP 2 (dep pre-filter), and `cmd_bootstrap()`'s recursive-dep loop —
  meaning the ROUND-0 fix alone would not have covered dependency
  packages. All four sites now go through the single shared
  `pkgbuild_needs_fetch()` helper instead of re-implementing the check.
- **build.c: ROUND 1/2 failure cleanup** (`remove(dest)` on a failed
  refresh attempt) assumed a file reaching that path could only ever be
  a partial download, since ROUND 0 previously guaranteed pre-existing
  files never got that far. Fixing ROUND 0 made that assumption false —
  without a guard, a transient network failure while refreshing a stale
  PKGBUILD would have deleted the last good copy instead of just failing
  to update it. Now guarded: a stale-but-present file is only removed
  once a fetch actually succeeds; if every refresh attempt fails, lpm
  falls back to the existing copy with a warning instead of hard-failing
  the whole install.
- **Housekeeping:** `Makefile`'s `VERSION` (1.2.0, unused by any build
  rule) had drifted out of sync with the actual runtime version reported
  by `lpm --version` (`LPM_VERSION` in `include/lpm.h`, 1.4.1). Both now
  read 1.4.2.

## [1.4.0-alpha] — 2026-05-07

### libllpm — nâng cấp từ skeleton lên full implementation (~2800 LOC)

- **error.c** — mở rộng `llpm_errno_t` thêm 13 error codes mới:
  `DEP_MISSING`, `DEP_CYCLE`, `CONFLICT`, `SIG_MISSING`, `SIG_INVALID`,
  `KEY_MISSING`, `KEY_UNTRUSTED`, `DB_CORRUPT`, `LOCK`, `PERMISSION`,
  `DOWNLOAD`, `CHECKSUM`, plus detail string `h->last_err_detail`

- **handle.c** — handle đầy đủ: paths (root/dbpath/cachepath/logfile),
  repo array, in-memory keyring, options (`check_space`, `sig_level`,
  `parallel_dl`, `max_dl_jobs`), `llpm_set_option_int()`, `llpm_errmsg()`

- **repo.c** — `llpm_register_repo()` / `llpm_unregister_repo()`,
  parallel DB fetch (pthreads + Gentoo-style spinner), parser repo.db
  (fields: pkgtype/dlsize/instsize/desc/depends/provides/conflicts),
  `llpm_repo_find_pkg()` với provides-lookup, `llpm_repo_list_updates()`
  so sánh version DB vs installed

- **dep.c** *(mới)* — `llpm_dep_parse_spec()` parse version constraints
  (`>=`,`<=`,`>`,`<`,`=`), `llpm_dep_satisfied()` query installed DB,
  `llpm_dep_resolve()` DFS toposort + cycle detection + conflict check,
  `llpm_dep_reverse()` reverse-dep lookup

- **keyring.c** *(mới)* — `llpm_keyring_load()` parse `gpg --with-colons`,
  `llpm_keyring_save()` import-ownertrust, `llpm_keyring_recv()` thử 3
  keyserver, `llpm_keyring_import()`, `llpm_keyring_set_trust()`,
  `llpm_keyring_verify()` verify detached .sig (sig_required=0/1),
  `llpm_keyring_find()` by full fingerprint hoặc keyid (16 chars),
  `llpm_keyring_delete()`

- **trans.c** — `llpm_trans_add_install/remove/upgrade()`, `prepare()`
  gọi dep resolver → fills `missing_deps[]` + `conflicts[]`,
  `commit()` execute ops theo topo order, `rollback()` best-effort undo

- **Makefile** — thêm `dep.c` + `keyring.c` vào `LIBLLPM`, install headers
  vào `/usr/include/llpm/`, install `.a` vào `/usr/lib/`

### UX — Arch-style prompts (đồng bộ toàn hệ)

- **lpm -Suy confirm** — thay `"Continue [Y/n]"` → `":: Proceed to update? [Y/n]"`
- **lpm -S confirm** — thay `"Would you like to build these packages?"` → `":: Proceed with build? [Y/n]"`
- **lpm -R confirm** — thay `"Would you like to remove these packages?"` → `":: Proceed with removal? [Y/n]"`
- **lpm -U confirm** — thay `"Would you like to rebuild and reinstall?"` → `":: Proceed with rebuild? [Y/n]"`
- **Tất cả `"Aborted."`** → `"Operation cancelled."` (yellow, C_YELLOW)
- **`CHECK_CANCEL` macro** — `"Interrupted."` → `"warning: Interrupt received — operation aborted."`
- **main.c exit(130)** — cùng thông báo warning khi Ctrl+C

## [1.3.0-alpha] — 2026-05-06

- lpm -Suy: parallel repo.db fetch (3 pthreads), Gentoo spinner, diff table
- libllpm skeleton: handle/repo/trans/error (placeholder)
- dep.c: in-process PkgMeta cache (512 entries), DFS toposort
- safety.c: config backup/restore, symlink guard, toolchain check
- lpkg.c: binary .lpkg format
