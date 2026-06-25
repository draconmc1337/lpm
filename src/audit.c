/*
 * audit.c — `lpm audit`: quick system health summary.
 *
 *   $ lpm audit
 *
 *   :: Auditing system...
 *
 *   Orphans: 2
 *   Updates: 5
 *
 *   :: Audit completed
 *
 * `lpm audit --log` dumps the raw audit log (package install/remove/
 * upgrade history) — a separate, lower-level view.
 */

#include "lpm.h"

void cmd_audit(int argc, char **argv) {
    init_dirs();

    /* ── --log: dump raw audit log (old behavior) ─────────────────────── */
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--log")) {
            FILE *f = fopen(LPM_AUDIT_LOG, "r");
            if (!f) {
                printf("No audit log found at %s\n", LPM_AUDIT_LOG);
                return;
            }
            char line[1024];
            while (fgets(line, sizeof(line), f))
                fputs(line, stdout);
            fclose(f);
            return;
        }
    }

    /* ── system audit summary ─────────────────────────────────────────── */
    printf(":: Auditing system...\n\n");

    int orphans = db_count_orphans();
    int updates = db_count_pending_updates();

    printf("Orphans: %d\n", orphans);
    if (updates < 0)
        printf("Updates: " C_GRAY "? (run `lpm update` first)" C_RESET "\n");
    else
        printf("Updates: %d\n", updates);

    printf("\n:: Audit completed\n");
}
