/*
 * audit.c — `lpm audit`: transaction history view.
 *
 *   $ lpm audit
 *
 *   Audit log
 *
 *   2026-08-31 17:42  install  foo-1.2.3
 *   2026-08-31 17:43  upgrade  bar  1.0.0 → 1.1.0
 *   2026-08-31 17:50  remove   baz-2.0.0
 *
 * `lpm audit --raw` dumps the unformatted log; `lpm audit --health` shows
 * the quick system-health summary (orphans + pending updates).
 */

#include "lpm.h"

/* Map a raw audit-log action token to the contract's verb. */
static const char *audit_verb(const char *tok) {
    if (!strcmp(tok, "install") || !strcmp(tok, "lpkg-install")) return "install";
    if (!strcmp(tok, "remove"))   return "remove";
    if (!strcmp(tok, "upgrade"))  return "upgrade";
    return tok;
}

void cmd_audit(int argc, char **argv) {
    init_dirs();

    int raw = 0, health = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw"))  raw = 1;
        if (!strcmp(argv[i], "--log"))  raw = 1;   /* back-compat alias */
        if (!strcmp(argv[i], "--health")) health = 1;
    }

    if (health) {
        printf(":: Auditing system...\n\n");
        int orphans = db_count_orphans();
        int updates = db_count_pending_updates();
        printf("Orphans: %d\n", orphans);
        if (updates < 0)
            printf("Updates: " C_GRAY "? (run `lpm update` first)" C_RESET "\n");
        else
            printf("Updates: %d\n", updates);
        printf("\n:: Audit completed\n");
        return;
    }

    FILE *f = fopen(LPM_AUDIT_LOG, "r");
    if (!f) {
        printf("Audit log\n\nNo audit log found at %s\n", LPM_AUDIT_LOG);
        return;
    }

    if (raw) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) fputs(line, stdout);
        fclose(f);
        return;
    }

    printf("Audit log\n\n");
    char line[1024];
    int shown = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        /* format: [YYYY-MM-DD HH:MM:SS] action: payload */
        if (line[0] != '[') continue;
        char *close = strchr(line, ']');
        if (!close) continue;
        *close = '\0';
        const char *ts = line + 1;                 /* "YYYY-MM-DD HH:MM:SS" */
        /* trim to minute: find the HH:MM part */
        char date[32] = "", clock_part[16] = "";
        sscanf(ts, "%31s %15s", date, clock_part);
        if (strlen(clock_part) >= 5) clock_part[5] = '\0';  /* HH:MM */

        const char *rest = close + 1;
        while (*rest == ' ') rest++;
        /* action token up to ':' */
        char action[64] = "";
        const char *colon = strchr(rest, ':');
        const char *payload = rest;
        if (colon) {
            size_t al = (size_t)(colon - rest);
            if (al >= sizeof(action)) al = sizeof(action) - 1;
            memcpy(action, rest, al); action[al] = '\0';
            payload = colon + 1;
            while (*payload == ' ') payload++;
        }
        const char *verb = action[0] ? audit_verb(action) : "?";

        /* tidy payload: "name ver-rel" → "name-ver-rel"; strip trailing " from FILE" */
        char name[256] = "", tail[512] = "";
        int nf = sscanf(payload, "%255s %511[^\n]", name, tail);
        printf("%s %s  %-8s  ", date, clock_part, verb);
        if (nf >= 2 && tail[0])
            printf("%s-%s\n", name, tail);
        else
            printf("%s\n", name[0] ? name : payload);
        shown++;
    }
    if (!shown)
        printf("(no transactions recorded)\n");
    fclose(f);
}
