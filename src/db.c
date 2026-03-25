#include "lpm.h"

/* format: pkgname=ver-rel  (one per line) */

int db_is_installed(const char *pkgname) {
    FILE *f = fopen(LPM_DB, "r");
    if (!f) return 0;
    char line[MAX_STR];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        /* match "pkgname" or "pkgname=..." */
        size_t len = strlen(pkgname);
        if (strncmp(line, pkgname, len) == 0 &&
            (line[len] == '=' || line[len] == '\0')) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* returns malloc'd "ver-rel" string or NULL if not found / no version */
char *db_get_version(const char *pkgname) {
    FILE *f = fopen(LPM_DB, "r");
    if (!f) return NULL;
    char line[MAX_STR];
    char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        size_t len = strlen(pkgname);
        if (strncmp(line, pkgname, len) == 0 && line[len] == '=') {
            result = strdup(line + len + 1);
            break;
        }
    }
    fclose(f);
    return result;
}

void db_add(const char *pkgname, const char *ver, const char *rel) {
    /* remove old entry first */
    db_remove(pkgname);
    FILE *f = fopen(LPM_DB, "a");
    if (!f) die("cannot write to DB: %s", LPM_DB);
    fprintf(f, "%s=%s-%s\n", pkgname, ver, rel);
    fclose(f);
}

void db_remove(const char *pkgname) {
    FILE *f = fopen(LPM_DB, "r");
    if (!f) return;

    /* write to temp file in same dir as DB to avoid cross-filesystem rename */
    char tmp_path[MAX_STR];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", LPM_DB);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); return; }

    char line[MAX_STR];
    size_t len = strlen(pkgname);
    while (fgets(line, sizeof(line), f)) {
        char stripped[MAX_STR];
        strncpy(stripped, line, sizeof(stripped) - 1);
        stripped[strcspn(stripped, "\n")] = '\0';
        if (strncmp(stripped, pkgname, len) == 0 &&
            (stripped[len] == '=' || stripped[len] == '\0'))
            continue;   /* skip — this is the entry to remove */
        fputs(line, tmp);
    }
    fclose(f);
    fclose(tmp);
    rename(tmp_path, LPM_DB);
}
