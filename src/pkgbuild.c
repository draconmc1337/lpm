#include "lpm.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* run bash snippet, collect stdout lines into out[] array, return count */
static int bash_array(const char *pbfile, const char *varname,
                      char out[][MAX_STR], int maxn) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "bash -c 'source \"%s\" 2>/dev/null; "
        "for _x in \"${%s[@]}\"; do printf \"%%s\\n\" \"$_x\"; done'",
        pbfile, varname);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    int n = 0;
    while (n < maxn && fgets(out[n], MAX_STR, p)) {
        out[n][strcspn(out[n], "\n")] = '\0';
        if (out[n][0]) n++;
    }
    pclose(p);
    return n;
}

static void bash_scalar(const char *pbfile, const char *varname,
                        char *out, size_t outsz) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "bash -c 'source \"%s\" 2>/dev/null; printf \"%%s\" \"${%s}\"'",
        pbfile, varname);
    FILE *p = popen(cmd, "r");
    out[0] = '\0';
    if (!p) return;
    fgets(out, (int)outsz, p);
    out[strcspn(out, "\n")] = '\0';
    pclose(p);
}

static int bash_func_exists(const char *pbfile, const char *fname) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "bash -c 'source \"%s\" 2>/dev/null; declare -f %s &>/dev/null'",
        pbfile, fname);
    return (system(cmd) == 0);
}

/* ── pkgbuild_parse ──────────────────────────────────────────────────────── */
int pkgbuild_parse(const char *pbfile, Pkg *pkg) {
    struct stat st;
    if (stat(pbfile, &st) != 0) return -1;

    memset(pkg, 0, sizeof(*pkg));
    strncpy(pkg->pbfile, pbfile, MAX_STR - 1);

    bash_scalar(pbfile, "pkgname", pkg->pkgname, MAX_STR);
    bash_scalar(pbfile, "pkgver",  pkg->pkgver,  MAX_STR);
    bash_scalar(pbfile, "pkgrel",  pkg->pkgrel,  MAX_STR);

    pkg->ndepends    = bash_array(pbfile, "depends",     pkg->depends,     MAX_DEPS);
    pkg->nrecommends = bash_array(pbfile, "recommends",  pkg->recommends,  MAX_DEPS);
    pkg->nmakedepends= bash_array(pbfile, "makedepends", pkg->makedepends, MAX_DEPS);

    /* source is a scalar string, not array */
    bash_scalar(pbfile, "source", pkg->source[0], MAX_STR);
    pkg->nsources = pkg->source[0][0] ? 1 : 0;

    /* source2, source3... */
    for (int i = 2; i <= MAX_SRCS && pkg->nsources < MAX_SRCS; i++) {
        char varname[16];
        snprintf(varname, sizeof(varname), "source%d", i);
        bash_scalar(pbfile, varname, pkg->source[pkg->nsources], MAX_STR);
        if (pkg->source[pkg->nsources][0]) pkg->nsources++;
    }

    /* sha256sums — scalar for single source, sha256sums2/3 for extra */
    bash_scalar(pbfile, "sha256sums", pkg->sha256sums[0], MAX_STR);
    for (int i = 2; i <= MAX_SRCS; i++) {
        char varname[32];
        snprintf(varname, sizeof(varname), "sha256sums%d", i);
        bash_scalar(pbfile, varname, pkg->sha256sums[i-1], MAX_STR);
    }

    pkg->has_check     = bash_func_exists(pbfile, "check");
    pkg->has_uninstall = bash_func_exists(pbfile, "uninstall");

    return 0;
}

/* ── dep_satisfied ───────────────────────────────────────────────────────── */
int dep_satisfied(const char *spec) {
    char pkgname[MAX_STR];
    char op[4] = "";
    char ver_need[MAX_STR] = "";

    /* parse "name>=ver" / "name<=ver" / "name=ver" / "name" */
    const char *p = spec;
    int ni = 0;
    while (*p && *p != '>' && *p != '<' && *p != '=')
        pkgname[ni++] = *p++;
    pkgname[ni] = '\0';

    if (*p) {
        int oi = 0;
        while (*p == '>' || *p == '<' || *p == '=')
            op[oi++] = *p++;
        op[oi] = '\0';
        strncpy(ver_need, p, MAX_STR - 1);
    }

    if (!db_is_installed(pkgname)) return 0;
    if (op[0] == '\0') return 1;   /* no version constraint */

    char *installed = db_get_version(pkgname);
    if (!installed) return 1;      /* old DB format, assume ok */

    /* installed is "ver-rel", strip "-rel" */
    char have[MAX_STR];
    strncpy(have, installed, MAX_STR - 1);
    free(installed);
    char *dash = strrchr(have, '-');
    if (dash) *dash = '\0';

    int ok = 0;
    if (strcmp(op, ">=") == 0)      ok = version_gte(have, ver_need);
    else if (strcmp(op, "<=") == 0) ok = version_gte(ver_need, have);
    else if (strcmp(op, "=")  == 0) ok = (strcmp(have, ver_need) == 0);

    return ok;
}

/* ── reverse_deps ────────────────────────────────────────────────────────── */
char *reverse_deps(const char *target) {
    static char result[4096];
    result[0] = '\0';

    FILE *db = fopen(LPM_DB, "r");
    if (!db) return result;

    char line[MAX_STR];
    while (fgets(line, sizeof(line), db)) {
        line[strcspn(line, "\n")] = '\0';
        char iname[MAX_STR];
        strncpy(iname, line, MAX_STR - 1);
        char *eq = strchr(iname, '=');
        if (eq) *eq = '\0';

        if (strcmp(iname, target) == 0) continue;

        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, iname);

        char deps[MAX_DEPS][MAX_STR];
        int n = bash_array(pbfile, "depends", deps, MAX_DEPS);
        for (int i = 0; i < n; i++) {
            char depname[MAX_STR];
            strncpy(depname, deps[i], MAX_STR - 1);
            /* strip operator */
            char *op = strpbrk(depname, "><=");
            if (op) *op = '\0';
            if (strcmp(depname, target) == 0) {
                if (result[0]) strncat(result, " ", sizeof(result) - strlen(result) - 1);
                strncat(result, iname, sizeof(result) - strlen(result) - 1);
                break;
            }
        }
    }
    fclose(db);
    return result;
}
