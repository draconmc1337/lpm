#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"

/*
 * pkgbuild.c — dependency-spec utilities (dep_satisfied, reverse_deps).
 *
 * lpm 2.0: this file used to also own pkgbuild_parse(), a second,
 * bash-based LPDF parser used as the primary parse call across ~20 call
 * sites (dep.c/search.c/verify.c/build.c/lpkg.c), with
 * pkgbuild_parse_fast() (pkgbuild_parser.c) treated as the "fast path"
 * used only by dep.c. In practice pkgbuild_parse() was the broken one:
 * it worked by bash-sourcing the LPDF file and reading back
 * ${varname[@]}, but bash treats "key = value" (spaces around '=') as a
 * command invocation, not an assignment — and LPDF v1 requires spaces
 * around '='. So real LPDF files silently produced empty results from
 * pkgbuild_parse(), papered over at ~7 call sites by pkg_patch_from_fast()
 * re-filling from the fast C parser afterward.
 *
 * There is now one parser: pkgbuild_parse_fast() (pkgbuild_parser.c),
 * a deterministic static-text C parser with no bash involved. Every
 * former pkgbuild_parse() call site now calls it directly, and
 * pkg_patch_from_fast() is gone — there's nothing left to patch.
 */

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

  if (!db_is_installed(pkgname))
    return 0;
  if (op[0] == '\0')
    return 1; /* no version constraint */

  char *installed = db_get_version(pkgname);
  if (!installed)
    return 1; /* old DB format, assume ok */

  /* installed is "ver-rel", strip "-rel" */
  char have[MAX_STR];
  strncpy(have, installed, MAX_STR - 1);
  free(installed);
  char *dash = strrchr(have, '-');
  if (dash)
    *dash = '\0';

  int ok = 0;
  if (strcmp(op, ">=") == 0)
    ok = version_gte(have, ver_need);
  else if (strcmp(op, "<=") == 0)
    ok = version_gte(ver_need, have);
  else if (strcmp(op, "=") == 0)
    ok = (strcmp(have, ver_need) == 0);

  return ok;
}

/* ── reverse_deps ────────────────────────────────────────────────────────── */
char *reverse_deps(const char *target) {
  static char result[LPM_NAME_MAX * 256];
  result[0] = '\0';

  FILE *db = fopen(LPM_DB, "r");
  if (!db)
    return result;

  char line[MAX_STR];
  while (fgets(line, sizeof(line), db)) {
    line[strcspn(line, "\n")] = '\0';
    char iname[MAX_STR];
    snprintf(iname, MAX_STR, "%s", line);
    char *eq = strchr(iname, '=');
    if (eq)
      *eq = '\0';

    if (strcmp(iname, target) == 0)
      continue;

    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, iname);

    Package dep_pkg;
    if (pkgbuild_parse_fast(pbfile, &dep_pkg) != 0)
      continue; /* unreadable/unparseable — no deps to check */

    for (int i = 0; i < dep_pkg.ndepends; i++) {
      char depname[LPM_NAME_MAX];
      strncpy(depname, dep_pkg.depends[i], sizeof(depname) - 1);
      depname[sizeof(depname) - 1] = '\0';
      /* strip operator */
      char *op = strpbrk(depname, "><=");
      if (op)
        *op = '\0';
      if (strcmp(depname, target) == 0) {
        size_t rlen = strlen(result);
        size_t rsz  = LPM_NAME_MAX * 256;
        if (result[0])
          snprintf(result + rlen, rsz - rlen, " %s", iname);
        else
          snprintf(result, rsz, "%s", iname);
        break;
      }
    }
  }
  fclose(db);
  return result;
}

#pragma GCC diagnostic pop
