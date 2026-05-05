# Changelog

## 1.3.0-alpha (2026-05-05)

### Added
- `lpm -K` / `lpm --key` command family (`init`, `list`, `recv`, `import`, `trust`) for keyring lifecycle in `/etc/lpm/gnupg`.
- Minimal `libllpm` C API skeleton (`llpm_initialize`, repo register/sync, transaction lifecycle) with public headers under `include/llpm/`.
- SHA-512 checksum support in PKGBUILD parsing and checksum selection path.
- Operation-scoped help support for `-S/-Sy/-Syu --help`.

### Changed
- Update flow now syncs `[core]`, `[extra]`, and `[lotus]` metadata view before version checks.
- Update flow refreshes PKGBUILDs before deciding upgrade candidates.
- Dependency resolution now caches parsed PKGBUILD content in-memory per resolve pass.

### Security
- Source fetch now aborts when checksum metadata is missing (requires sha256/sha512/md5).
