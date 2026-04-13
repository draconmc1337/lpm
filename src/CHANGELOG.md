# Changelog — LPM (Lotus Package Manager)

All notable changes to LPM are documented here.  
Format: newest first. Versions follow the project's alpha versioning scheme.

---

## 1.1.5-alpha — current

**Stability + Control release.** First version reliable enough for real-world usage.

### New features

- **`--bootstrap` flag** — LFS/fresh system mode: forces `--yes`, skips `check()`, disables strict mode, overrides all config booleans. Designed for automated LFS builds where you just want things installed without any prompts or test noise.
- **`lpm -l --count`** — prints bare integer count of installed packages. Clean output, no extra text — pipe-friendly.
- **`MAKEFLAGS` in lpm.conf** — configurable via config file. Default: `ceil(nproc/2)` (conservative, avoids OOM on large builds). Override with `-j$(nproc)` for max speed.
- **`DEFAULT_YES`, `DEFAULT_STRICT`, `RUN_CHECK`, `STRICT_BUILD`** — all config booleans now wired into the build loop. CLI flags always take priority over config.

### Fetch system rewrite

- **Retry logic** — 3 attempts with 1s delay between retries
- **DNS failure detection** — `getent hosts` check before retrying; no pointless retries on DNS fail
- **Error classification** — distinct messages for DNS fail, timeout, 404/small response
- **Detailed fetch log** — `FETCH OK`, `FETCH DNS FAIL`, `FETCH attempt N SMALL`, `FETCH FAILED after N attempts` — every fetch event logged with size and attempt count
- **Cache log** — `CACHE hit`, `CACHE partial`, `CACHE corrupt` — know exactly why a re-download happened
- **Local source log** — `LOCAL source: /sources/<file>` logged explicitly

### Build loop

- **Shared `build_queue()`** — `cmd_sync` and `cmd_local` now share one build loop, no duplicate code
- **`--bootstrap` overrides config** — `cfg.run_check`, `cfg.strict_build`, `cfg.default_strict` all forced off when bootstrap is active
- **`cmd_fetch` standalone** — extracted from `cmd_sync`, callable independently

### Cache behavior

- **`-S` uses cached pkgbuild** — if `pkgbuild_<pkg>` already exists locally, skip fetch. Use `-Sy` to force refresh.
- **"Fetched from" vs "Using cached"** — output now distinguishes between a fresh fetch and a cache hit

### Error context

- Build failures now report phase explicitly:
  ```
  error: Build failed: gcc
    Phase: build()
    Log:   /var/log/lpm/gcc.log
  ```
  ```
  error: Install failed for gcc
    Phase: package()
    Log:   /var/log/lpm/gcc.log
  ```

---

## 1.1.4.1-alpha

- **Config system** — `/etc/lpm/lpm.conf` with `CriticalPkg`, `IgnorePkg`, `LogDir`, `FilesDir`; replaces the old hardcoded critical package array
- **Tiered removal UX** — NORMAL / IMPORTANT / CRITICAL, each with appropriate confirmation level
- **`IgnorePkg`** — packages skipped during `lpm -u`; still updatable manually
- **Config validation** — exits hard if config is missing, empty, or comment-only; boolean keys only accept `y`/`n`

---

## 1.1.4-alpha

Skipped to 1.1.4.1 due to config system rework during development.

---

## 1.1.3-alpha

- **Per-package logs** — `/var/log/lpm/<pkg>.log`, overwritten each build. No 50k-line pile-up.
- **`LpmFlags` struct** — centralized flag parsing for `--yes`, `--strict`, `--force`, `--no-confirm`
- **`--yes`** — skip all prompts, auto-run `check()`
- **`--strict`** — `check()` failure is a hard install block
- **`lpm-dev`** — separate binary, `-bi` only; `build_dev.c` + `main_dev.c`
- Error messages now always include log path on failure
- **Progress counter** — `[1/10] Building bash...` during multi-package builds

---

## 1.1.2-alpha — Hotfix 2

- **`uninstall()` removed from execution** — PKGBUILD scripts can no longer touch the filesystem on removal; supply-chain attack vector closed
- **File ownership database** — `db_files_save()` + `db_files_remove()`; removal is `unlink()` per recorded file, no script execution
- **Symlink escape protection** — `scan_pkgdir()` rejects symlinks that escape `$pkgdir`
- **Audit log** — security-sensitive actions logged to `/var/log/lpm/audit.log` with UID
- **`confirm_word()`** — destructive operations require typing an exact word (`YES`, `DELETE`)

---

## 1.1.1-alpha

- **Dependency resolution** — toposorted build queue with cycle detection
- **`lpm -D`** — dependency tree visualization
- **`lpm -qi`** — package info including reverse dependencies
- **Checksum verification** — sha256 and md5 support, `SKIP` to bypass

---

## 1.1.0-alpha — first public release

- Basic install, remove, update, search
- PKGBUILD parser
- Simple installed DB (`/var/lib/lpm/installed`)
- `check()` support with prompt
- Build cache management (`-rcc`)
