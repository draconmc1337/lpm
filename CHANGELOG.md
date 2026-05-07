# Changelog

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
