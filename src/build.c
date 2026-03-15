#include "lpm.h"

/* run a shell command, return exit code */
static int run(const char *cmd) {
    return system(cmd);
}

/* ensure build workspace exists, download sources */
static int prepare_workspace(Pkg *pkg) {
    char ws[MAX_STR];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg->pkgname);
    mkdir(ws, 0755);

    for (int i = 0; i < pkg->nsources; i++) {
        if (!pkg->source[i][0]) continue;
        char *fname = strrchr(pkg->source[i], '/');
        if (!fname) continue;
        fname++;

        char dest[MAX_STR];
        snprintf(dest, sizeof(dest), "%s/%s", ws, fname);

        /* check if already downloaded AND not partial */
        struct stat st;
        if (stat(dest, &st) == 0) {
            /* partial download check: size must be > 1KB */
            if (st.st_size < (200 * 1024)) {
                fprintf(stderr,
                    C_YELLOW "warning: " C_RESET
                    "%s looks partial (%ld bytes), re-downloading...\n",
                    fname, (long)st.st_size);
                remove(dest);
            } else {
                /* verify tarball integrity if it's a tar archive */
                char ext[32] = "";
                char *dot = strrchr(fname, '.');
                if (dot) strncpy(ext, dot, sizeof(ext) - 1);

                int is_tar = (strstr(fname, ".tar") != NULL);
                if (is_tar) {
                    char check_cmd[MAX_STR];
                    snprintf(check_cmd, sizeof(check_cmd),
                        "tar -tf '%s' &>/dev/null", dest);
                    if (run(check_cmd) != 0) {
                        fprintf(stderr,
                            C_YELLOW "warning: " C_RESET
                            "%s is corrupt, re-downloading...\n", fname);
                        remove(dest);
                    } else {
                        continue; /* file ok, skip download */
                    }
                } else {
                    continue; /* not a tar, trust size check */
                }
            }
        }

        printf(C_CYAN "  ->" C_RESET " Fetching %s\n", fname);
        lpm_log("Downloading %s", pkg->source[i]);

        /* use .part file — atomic download */
        char part[MAX_STR];
        snprintf(part, sizeof(part), "%s.part", dest);
        remove(part);  /* remove stale .part from previous interrupted run */

        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "wget -q --show-progress -O '%s' '%s' "
            "|| curl -L --progress-bar -o '%s' '%s'",
            part, pkg->source[i], part, pkg->source[i]);

        int dl_rc = run(cmd);  /* run() exits on SIGINT, so .part gets cleaned below */
        if (dl_rc != 0) {
            remove(part);  /* cleanup partial .part file */
            fprintf(stderr,
                C_RED "error: " C_RESET
                "Failed to fetch %s\n"
                "  Check network or source URL in PKGBUILD.\n",
                pkg->source[i]);
            return -1;
        }

        /* verify .part file is not empty/partial */
        struct stat part_st;
        if (stat(part, &part_st) != 0 || part_st.st_size < (200 * 1024)) {
            remove(part);
            fprintf(stderr,
                C_RED "error: " C_RESET
                "Downloaded file too small — likely a 404 or network error.\n"
                "  URL: %s\n", pkg->source[i]);
            return -1;
        }

        /* atomic rename: .part -> final filename */
        if (rename(part, dest) != 0) {
            remove(part);
            fprintf(stderr, C_RED "error: " C_RESET
                    "Failed to finalize download of %s\n", fname);
            return -1;
        }
    }
    return 0;
}




/* ── verify_sources ─────────────────────────────────────────────────────── */
static int verify_sources(Pkg *pkg, const char *ws) {
    int ok = 1;
    for (int i = 0; i < pkg->nsources; i++) {
        const char *expected = NULL;
        const char *algo     = NULL;
        const char *tool     = NULL;

        if (pkg->sha256sums[i][0] && strcmp(pkg->sha256sums[i], "SKIP") != 0) {
            expected = pkg->sha256sums[i];
            algo     = "sha256";
            tool     = "sha256sum";
        } else if (pkg->md5sums[i][0] && strcmp(pkg->md5sums[i], "SKIP") != 0) {
            expected = pkg->md5sums[i];
            algo     = "md5";
            tool     = "md5sum";
        } else {
            continue;  /* no checksum or SKIP */
        }

        char *fname = strrchr(pkg->source[i], '/');
        if (!fname) continue;
        fname++;

        char filepath[MAX_STR];
        snprintf(filepath, sizeof(filepath), "%s/%s", ws, fname);

        char cmd[MAX_STR];
        snprintf(cmd, sizeof(cmd),
            "%s '%s' 2>/dev/null | cut -d' ' -f1", tool, filepath);
        FILE *p = popen(cmd, "r");
        if (!p) { ok = 0; continue; }

        char actual[MAX_STR] = "";
        if (fgets(actual, sizeof(actual), p))
            actual[strcspn(actual, "\n")] = '\0';
        pclose(p);

        if (strcmp(actual, expected) != 0) {
            fprintf(stderr,
                C_RED "error: " C_RESET
                "%s mismatch for " C_BOLD "%s" C_RESET "\n"
                "  expected: " C_CYAN "%s" C_RESET "\n"
                "  got:      " C_RED "%s" C_RESET "\n",
                algo, fname, expected, actual);
            ok = 0;
        } else {
            printf(C_GREEN "  ok" C_RESET " [%s] %s\n", algo, fname);
        }
    }
    return ok;
}

/* ── cmd_check ───────────────────────────────────────────────────────────── */
void cmd_check(int argc, char **argv) {
    check_root(); init_dirs();
    if (argc == 0) die("No package specified.\nUsage: lpm -c <package>");

    for (int i = 0; i < argc; i++) {
        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);

        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) != 0)
            die("PKGBUILD not found for '%s'", argv[i]);

        char ws[MAX_STR];
        snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);

        char built[MAX_STR];
        snprintf(built, sizeof(built), "%s/.built", ws);
        struct stat st;
        if (stat(built, &st) != 0)
            die("%s has not been built yet. Run 'lpm -b %s' first.", argv[i], argv[i]);

        if (!pkg.has_check) {
            warn("No check() defined in pkgbuild_%s — skipping", argv[i]);
            continue;
        }

        char check_log[MAX_STR];
        snprintf(check_log, sizeof(check_log), "%s/check.log", ws);

        {
            char prompt[MAX_STR];
            snprintf(prompt, sizeof(prompt),
                "Run test suite for " C_BOLD "%s" C_RESET "? [Y/N] ", argv[i]);
            if (!confirm(prompt)) { printf("Skipped.\n"); continue; }
        }
        printf(C_CYAN "::" C_RESET " Running test suite for " C_BOLD "%s" C_RESET "...\n", argv[i]);
        printf("   Log: " C_CYAN "%s" C_RESET "\n", check_log);
        lpm_log("Running check() for %s", argv[i]);

        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "bash -c 'source \"%s\" && cd \"%s\" && check' > \"%s\" 2>&1",
            pbfile, ws, check_log);

        int rc = run(cmd);

        /* show last 400 lines */
        printf("\n" C_BOLD "── Last 400 lines of check.log ──────────────────" C_RESET "\n");
        char tail_cmd[MAX_STR];
        snprintf(tail_cmd, sizeof(tail_cmd), "tail -400 '%s'", check_log);
        run(tail_cmd);

        printf("\n" C_BOLD "─────────────────────────────────────────────────" C_RESET "\n");

        /* count PASS/FAIL/XFAIL/ERROR */
        char count_cmd[MAX_STR];
        int passes, fails, xfails, errors;

        #define COUNT(var, prefix) \
            snprintf(count_cmd, sizeof(count_cmd), \
                "grep -c '^" prefix ":' '%s' 2>/dev/null || echo 0", check_log); \
            { FILE *p = popen(count_cmd, "r"); var = 0; \
              if (p) { fscanf(p, "%d", &var); pclose(p); } }

        COUNT(passes, "PASS")
        COUNT(fails,  "FAIL")
        COUNT(xfails, "XFAIL")
        COUNT(errors, "ERROR")
        #undef COUNT

        if (passes + fails + xfails + errors > 0) {
            printf("  PASS : " C_GREEN "%d" C_RESET "\n", passes);
            if (xfails) printf("  XFAIL: " C_CYAN "%d" C_RESET "  (expected — OK)\n", xfails);
            if (fails)  printf("  FAIL : " C_RED "%d" C_RESET "\n", fails);
            if (errors) printf("  ERROR: " C_RED "%d" C_RESET "\n", errors);
            printf("\n");
        }

        if (rc == 0) {
            printf(C_CYAN "::" C_RESET " check() exited " C_GREEN "clean" C_RESET "\n");
            char cmarker[MAX_STR];
            snprintf(cmarker, sizeof(cmarker), "%s/.checked", ws);
            FILE *f = fopen(cmarker, "w"); if (f) fclose(f);
            lpm_log("check() passed for %s", argv[i]);
        } else {
            printf(C_CYAN "::" C_RESET " check() exited with " C_RED "errors (rc=%d)" C_RESET "\n", rc);
            printf("   Full log: " C_CYAN "%s" C_RESET "\n", check_log);
            lpm_log("check() failed for %s (rc=%d)", argv[i], rc);
            if (!confirm("Failures detected. Continue anyway? [y/N] ")) {
                printf("Aborted.\n");
                exit(1);
            }
        }
    }
}

/* ── cmd_remove ──────────────────────────────────────────────────────────── */
void cmd_remove(int argc, char **argv) {
    check_root(); init_dirs();

    /* separate packages from --force flag */
    char *pkgs[64]; int npkgs = 0;
    int force = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) force = 1;
        else pkgs[npkgs++] = argv[i];
    }
    if (npkgs == 0) die("No package specified.\nUsage: lpm -r <package>");

    /* reverse dep check */
    int blocked = 0;
    for (int i = 0; i < npkgs; i++) {
        char *rdeps = reverse_deps(pkgs[i]);
        if (rdeps && rdeps[0]) {
            fprintf(stderr, C_RED "error: " C_RESET
                    C_BOLD "%s" C_RESET " is required by: " C_YELLOW "%s" C_RESET "\n",
                    pkgs[i], rdeps);
            blocked = 1;
        }
    }
    if (blocked) {
        printf("\n");
        if (!force) {
            printf("Remove the dependent packages first, "
                   "or use " C_BOLD "-r --force" C_RESET " to override.\n");
            exit(1);
        }
        warn("--force passed, ignoring reverse dependencies.");
        printf("\n");
    }

    /* critical packages — cannot remove on a running system */
    static const char *critical[] = {
        /* libc + toolchain */
        "glibc", "gcc", "binutils", "libgcc", "libstdc++",
        /* shell + core */
        "bash", "coreutils", "util-linux", "shadow", "login",
        /* init system */
        "dinit", "sysvinit", "runit", "s6", "openrc",
        /* kernel + modules */
        "linux", "kmod", "udev",
        /* filesystem */
        "e2fsprogs", "btrfs-progs", "dosfstools",
        /* bootloader */
        "grub",
        /* network base */
        "iproute2",
        /* package manager itself */
        "lpm",
        NULL
    };
    for (int i = 0; i < npkgs; i++) {
        for (int c = 0; critical[c]; c++) {
            if (strcmp(pkgs[i], critical[c]) == 0) {
                fprintf(stderr, C_RED "error: " C_RESET
                        "%s is a critical package and cannot be removed "
                        "on a running system\n", pkgs[i]);
                exit(1);
            }
        }
    }

    /* check all targets are actually installed */
    int not_found = 0;
    for (int i = 0; i < npkgs; i++) {
        if (!db_is_installed(pkgs[i])) {
            fprintf(stderr, C_RED "error: " C_RESET
                    "target not found: %s\n", pkgs[i]);
            not_found++;
        }
    }
    if (not_found) exit(1);

    printf("Packages to remove (" C_BOLD "%d" C_RESET "):\n", npkgs);
    for (int i = 0; i < npkgs; i++)
        printf("  %s\n", pkgs[i]);
    printf("\n");
    if (!confirm("\nWould you like to remove these packages? [" C_GREEN "Yes" C_RESET "/" C_RED "No" C_RESET "] ")) { printf("Aborted.\n"); exit(0); }

    for (int i = 0; i < npkgs; i++) {
        printf(C_CYAN "::" C_RESET " Removing " C_BOLD "%s" C_RESET "...", pkgs[i]);
        fflush(stdout);
        lpm_log("Removing %s", pkgs[i]);

        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgs[i]);

        struct stat st;
        if (stat(pbfile, &st) == 0) {
            /* check if uninstall() exists */
            Pkg pkg;
            pkgbuild_parse(pbfile, &pkg);
            if (pkg.has_uninstall) {
                char cmd[1024];
                snprintf(cmd, sizeof(cmd),
                    "bash -c \"source '%s' && uninstall\" >> '%s' 2>&1",
                    pbfile, LPM_LOG);
                run(cmd);
            }
        }

        db_remove(pkgs[i]);

        /* clean build cache */
        char cache[MAX_STR];
        snprintf(cache, sizeof(cache), "%s/%s", LPM_BUILD_DIR, pkgs[i]);
        char rm_cmd[MAX_STR];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cache);
        run(rm_cmd);

        printf(" " C_GREEN "done" C_RESET "\n");
        lpm_log("Removed %s", pkgs[i]);
    }
}

/* ── cmd_update ──────────────────────────────────────────────────────────── */
void cmd_update(int argc, char **argv) {
    check_root(); init_dirs();

    /* collect targets */
    char *targets[256]; int ntargets = 0;

    if (argc == 0) {
        /* all installed */
        FILE *f = fopen(LPM_DB, "r");
        if (!f) { printf("No packages installed via lpm.\n"); return; }
        char line[MAX_STR];
        while (fgets(line, sizeof(line), f) && ntargets < 256) {
            line[strcspn(line, "\n")] = '\0';
            if (!line[0]) continue;
            char *eq = strchr(line, '=');
            if (eq) *eq = '\0';
            targets[ntargets++] = strdup(line);
        }
        fclose(f);
        printf(C_CYAN "::" C_RESET " Checking updates for " C_BOLD "%d" C_RESET
               " installed package(s)...\n\n", ntargets);
    } else {
        for (int i = 0; i < argc && i < 256; i++)
            targets[ntargets++] = argv[i];
    }

    char *to_update[256]; int nupdate = 0;

    for (int i = 0; i < ntargets; i++) {
        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, targets[i]);
        struct stat st;
        if (stat(pbfile, &st) != 0) { warn("No PKGBUILD for '%s', skipping", targets[i]); continue; }

        Pkg pkg;
        pkgbuild_parse(pbfile, &pkg);

        char pb_full[MAX_STR];
        snprintf(pb_full, sizeof(pb_full), "%s-%s", pkg.pkgver, pkg.pkgrel);

        char *installed_ver = db_get_version(targets[i]);

        if (!installed_ver) {
            printf("  " C_BOLD "%-24s" C_RESET "  " C_YELLOW "installed (version unknown)" C_RESET
                   " -> " C_CYAN "%s" C_RESET "\n", targets[i], pb_full);
            to_update[nupdate++] = targets[i];
        } else if (strcmp(installed_ver, pb_full) == 0) {
            printf("  " C_BOLD "%-24s" C_RESET "  " C_GREEN "up to date" C_RESET " (%s)\n",
                   targets[i], pb_full);
            free(installed_ver);
        } else {
            printf("  " C_BOLD "%-24s" C_RESET "  " C_YELLOW "%s" C_RESET
                   " -> " C_CYAN "%s" C_RESET "\n", targets[i], installed_ver, pb_full);
            to_update[nupdate++] = targets[i];
            free(installed_ver);
        }
    }

    printf("\n");
    if (nupdate == 0) { printf(C_CYAN "::" C_RESET " " C_GREEN "All packages are up to date." C_RESET "\n"); return; }

    printf("Packages to update (" C_BOLD "%d" C_RESET "):\n", nupdate);
    for (int i = 0; i < nupdate; i++) printf("  %s\n", to_update[i]);
    printf("\n");
    if (!confirm("Proceed to rebuild and reinstall? [y/N] ")) { printf("Aborted.\n"); exit(0); }

    for (int i = 0; i < nupdate; i++) {
        printf("\n" C_BOLD "==> Updating %s" C_RESET "\n", to_update[i]);
        /* wipe old cache */
        char cache[MAX_STR];
        snprintf(cache, sizeof(cache), "%s/%s", LPM_BUILD_DIR, to_update[i]);
        char rm_cmd[MAX_STR];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cache);
        run(rm_cmd);

        /* re-use sync logic: wipe cache then sync */
        char *pair[1] = { to_update[i] };
        cmd_sync(1, pair);
        lpm_log("Updated %s", to_update[i]);
    }

    printf("\n" C_CYAN "::" C_RESET " " C_GREEN "Update complete." C_RESET "\n");
}

/* ── cmd_sync ─────────────────────────────────────────────────────────────── */
void cmd_sync(int argc, char **argv) {
    check_root(); init_dirs();
    if (argc == 0) die("No package specified.\nUsage: lpm -S <package>");

#define REPO_BASE "https://raw.githubusercontent.com/draconmc1337/lotus-repository/main"
#define REPO_TRIES 3
    static const char *folders[REPO_TRIES] = { "base", "extra", "lotus" };

    for (int i = 0; i < argc; i++) {
        char url[1024];
        char dest[MAX_STR];
        snprintf(dest, sizeof(dest), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);

        printf(C_CYAN "::" C_RESET " Fetching pkgbuild_%s...\n", argv[i]);

        int fetched = 0;
        for (int f = 0; f < REPO_TRIES; f++) {
            snprintf(url, sizeof(url), "%s/%s/pkgbuild_%s",
                     REPO_BASE, folders[f], argv[i]);

            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "wget -q -O '%s' '%s' 2>/dev/null || curl -sfL -o '%s' '%s' 2>/dev/null",
                dest, url, dest, url);

            run(cmd);

            /* verify file non-empty (not a 404 page) */
            struct stat st;
            if (stat(dest, &st) == 0 && st.st_size > 32) {
                printf(C_GREEN "  ->" C_RESET
                       " Found in " C_CYAN "[%s]" C_RESET " -> %s\n",
                       folders[f], dest);
                lpm_log("Fetched pkgbuild_%s from %s/%s",
                        argv[i], REPO_BASE, folders[f]);
                fetched = 1;
                break;
            }
            remove(dest);
        }

        if (!fetched) {
            die("pkgbuild_%s not found in base/, extra/, or lotus/\n"
                "    Check the package name or push PKGBUILD to the repo.", argv[i]);
        }
    }

    /* build full dep queue with toposort */
    char queue[256][MAX_STR];
    int  nqueue = 0;
    for (int i = 0; i < argc; i++) {
        char subq[256][MAX_STR];
        int  nsub = dep_resolve_queue(argv[i], subq, 256);
        for (int j = 0; j < nsub && nqueue < 256; j++) {
            /* dedup */
            int dup = 0;
            for (int k = 0; k < nqueue; k++)
                if (strcmp(queue[k], subq[j]) == 0) { dup = 1; break; }
            if (!dup) strncpy(queue[nqueue++], subq[j], MAX_STR - 1);
        }
        /* ensure target itself is last */
        int dup = 0;
        for (int k = 0; k < nqueue; k++)
            if (strcmp(queue[k], argv[i]) == 0) { dup = 1; break; }
        if (!dup) strncpy(queue[nqueue++], argv[i], MAX_STR - 1);
    }

    /* show combined dep tree for all packages */
    cmd_deptree(argc, argv);

    if (nqueue > 0) {
        printf(C_CYAN "::" C_RESET " Will build " C_BOLD "%d" C_RESET
               " package(s) in order:\n", nqueue);
        for (int i = 0; i < nqueue; i++)
            printf("    " C_CYAN "%d." C_RESET " %s\n", i + 1, queue[i]);
        printf("\n");
    }

    if (!confirm("\nWould you like to build these packages? [" C_GREEN "Yes" C_RESET "/" C_RED "No" C_RESET "] ")) { printf("Aborted.\n"); exit(0); }

    /* build + install each package in toposorted order */
    for (int qi = 0; qi < nqueue; qi++) {
        char *pkgname_q = queue[qi];
        if (db_is_installed(pkgname_q)) continue;

        /* fetch PKGBUILD if not local */
        char pbfile_check[MAX_STR];
        snprintf(pbfile_check, sizeof(pbfile_check), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, pkgname_q);
        struct stat st_check;
        if (stat(pbfile_check, &st_check) != 0) {
            printf(C_CYAN "  ->" C_RESET " Fetching pkgbuild_%s...\n", pkgname_q);
            char fetch_argv_buf[MAX_STR];
            strncpy(fetch_argv_buf, pkgname_q, MAX_STR - 1);
            char *fetch_argv[1] = { fetch_argv_buf };
            cmd_fetch(1, fetch_argv);
        }
    }

    /* now build+install loop */
    for (int qi = 0; qi < nqueue; qi++) {
        int i = qi; /* reuse variable name below */
        char argv_buf[MAX_STR];
        strncpy(argv_buf, queue[qi], MAX_STR - 1);
        char *cur_argv = argv_buf;
        (void)i;

        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, cur_argv);

        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) != 0) {
            warn("No PKGBUILD for '%s', skipping", cur_argv);
            continue;
        }

        if (db_is_installed(cur_argv)) {
            printf(C_CYAN "  ->" C_RESET " %s already installed, skipping\n", cur_argv);
            continue;
        }

        if (prepare_workspace(&pkg) != 0)
            die("Failed to prepare workspace for %s", argv[i]);

        char ws[MAX_STR];
        snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);

        /* verify checksums */
        if (pkg.sha256sums[0][0]) {
            printf(C_CYAN "::" C_RESET " Verifying checksums...\n");
            if (!verify_sources(&pkg, ws)) {
                lpm_log("Checksum FAILED: %s", cur_argv);
                die("Source integrity check failed for %s", cur_argv);
            }
        }

        /* build */
        printf(C_BOLD "==> Building %s %s-%s" C_RESET "\n",
               pkg.pkgname, pkg.pkgver, pkg.pkgrel);
        lpm_log("Building %s %s-%s", pkg.pkgname, pkg.pkgver, pkg.pkgrel);

        char build_cmd[2048];
        snprintf(build_cmd, sizeof(build_cmd),
            "bash -c 'source \"%s\" && cd \"%s\" && build' >> \"%s\" 2>&1",
            pbfile, ws, LPM_LOG);

        if (run(build_cmd) != 0) {
            fprintf(stderr, C_RED "error: " C_RESET "Build failed for %s. See %s\n",
                    argv[i], LPM_LOG);
            lpm_log("Build FAILED: %s", argv[i]);
            exit(1);
        }

        /* check() — optional, run between build and install */
        if (pkg.has_check) {
            char check_log[MAX_STR];
            snprintf(check_log, sizeof(check_log), "%s/check.log", ws);

            char prompt[MAX_STR];
            snprintf(prompt, sizeof(prompt),
                "Run test suite for " C_BOLD "%s" C_RESET "? [Y/N] ", argv[i]);
            if (confirm(prompt)) {
                printf(C_CYAN "::" C_RESET " Running check()...\n");
                char check_cmd[2048];
                snprintf(check_cmd, sizeof(check_cmd),
                    "bash -c 'source \"%s\" && cd \"%s\" && check' > \"%s\" 2>&1",
                    pbfile, ws, check_log);
                int rc = run(check_cmd);

                /* tail last 40 lines */
                char tail_cmd[MAX_STR];
                snprintf(tail_cmd, sizeof(tail_cmd), "tail -40 '%s'", check_log);
                run(tail_cmd);

                if (rc != 0) {
                    printf(C_RED "  check() failed (rc=%d)" C_RESET
                           " — log: %s\n", rc, check_log);
                    if (!confirm("Failures detected. Continue to install anyway? [y/N] "))
                        exit(1);
                } else {
                    printf(C_GREEN "  check() passed" C_RESET "\n");
                }
            }
        }

        /* install via pkgdir */
        char pkgdir[MAX_STR];
        snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", ws);
        char mk_cmd[MAX_STR];
        snprintf(mk_cmd, sizeof(mk_cmd), "rm -rf '%s' && mkdir -p '%s'", pkgdir, pkgdir);
        run(mk_cmd);

        printf(C_BOLD "==> Installing %s" C_RESET "\n", argv[i]);
        lpm_log("Installing %s", argv[i]);

        char inst_cmd[2048];
        snprintf(inst_cmd, sizeof(inst_cmd),
            "bash -c 'source \"%s\" && cd \"%s\" && pkgdir=\"%s\" && package' >> \"%s\" 2>&1",
            pbfile, ws, pkgdir, LPM_LOG);

        if (run(inst_cmd) != 0) {
            fprintf(stderr, C_RED "error: " C_RESET "Install failed for %s. See %s\n",
                    argv[i], LPM_LOG);
            lpm_log("Install FAILED: %s", argv[i]);
            exit(1);
        }

        /* merge into / */
        printf(C_CYAN "  ->" C_RESET " Merging into /...\n");
        char merge_cmd[MAX_STR];
        snprintf(merge_cmd, sizeof(merge_cmd), "cp -a '%s'/. /", pkgdir);
        if (run(merge_cmd) != 0) {
            fprintf(stderr, C_RED "error: " C_RESET "Merge failed for %s\n", argv[i]);
            exit(1);
        }

        db_add(pkg.pkgname, pkg.pkgver, pkg.pkgrel);
        printf(C_GREEN "==> Installed %s %s-%s" C_RESET "\n",
               pkg.pkgname, pkg.pkgver, pkg.pkgrel);
        lpm_log("Installed %s %s-%s", pkg.pkgname, pkg.pkgver, pkg.pkgrel);
    }

#undef REPO_BASE
#undef REPO_TRIES
}

/* ── cmd_fetch ───────────────────────────────────────────────────────────────
 * lpm -Sy <pkg>  — fetch PKGBUILD only, do NOT build
 * ─────────────────────────────────────────────────────────────────────────── */
void cmd_fetch(int argc, char **argv) {
    check_root(); init_dirs();
    if (argc == 0) die("No package specified.\nUsage: lpm -Sy <package>");

#define REPO_BASE2 "https://raw.githubusercontent.com/draconmc1337/lotus-repository/main"
#define REPO_TRIES2 3
    static const char *folders2[REPO_TRIES2] = { "base", "extra", "lotus" };

    for (int i = 0; i < argc; i++) {
        char url[1024];
        char dest[MAX_STR];
        snprintf(dest, sizeof(dest), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);

        printf(C_CYAN "::" C_RESET " Fetching pkgbuild_%s...\n", argv[i]);

        int fetched = 0;
        for (int f = 0; f < REPO_TRIES2; f++) {
            snprintf(url, sizeof(url), "%s/%s/pkgbuild_%s",
                     REPO_BASE2, folders2[f], argv[i]);

            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "wget -q -O '%s' '%s' 2>/dev/null || curl -sfL -o '%s' '%s' 2>/dev/null",
                dest, url, dest, url);
            run(cmd);

            struct stat st;
            if (stat(dest, &st) == 0 && st.st_size > 32) {
                printf(C_GREEN "  ->" C_RESET
                       " [%s/] " C_BOLD "%s" C_RESET "\n",
                       folders2[f], dest);
                lpm_log("Fetched pkgbuild_%s from %s/%s",
                        argv[i], REPO_BASE2, folders2[f]);
                fetched = 1;
                break;
            }
            remove(dest);
        }

        if (!fetched) {
            fprintf(stderr, C_RED "error: " C_RESET
                    "pkgbuild_%s not found in base/, extra/, or lotus/\n", argv[i]);
        }
    }
    printf(C_CYAN "::" C_RESET " PKGBUILDs saved to " C_BOLD "%s" C_RESET "\n"
           "   Review then run: lpm -bi <package>\n", LPM_PKGBUILD_DIR);

#undef REPO_BASE2
#undef REPO_TRIES2
}
