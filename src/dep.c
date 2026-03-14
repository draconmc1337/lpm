#include "lpm.h"

#define MAX_QUEUE 256

typedef struct {
    char name[MAX_STR];
    char ver[MAX_STR];
    int  installed;   /* in DB */
    int  has_src;     /* PKGBUILD exists locally */
    int  depth;
} DepNode;

static DepNode resolved[MAX_QUEUE];
static int     nresolved = 0;

/* check if already in resolved list */
static int already_seen(const char *name) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, name) == 0) return 1;
    return 0;
}

/* strip version operator from dep spec */
static void dep_name_only(const char *spec, char *out) {
    strncpy(out, spec, MAX_STR - 1);
    char *op = strpbrk(out, "><=");
    if (op) *op = '\0';
}

/* recursive resolver */
static void resolve(const char *pkgname, int depth) {
    if (already_seen(pkgname)) return;
    if (nresolved >= MAX_QUEUE) return;

    DepNode node;
    memset(&node, 0, sizeof(node));
    strncpy(node.name, pkgname, MAX_STR - 1);
    node.depth     = depth;
    node.installed = db_is_installed(pkgname);

    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgname);

    struct stat st;
    node.has_src = (stat(pbfile, &st) == 0);

    if (node.has_src) {
        Pkg pkg;
        pkgbuild_parse(pbfile, &pkg);
        strncpy(node.ver, pkg.pkgver, MAX_STR - 1);

        /* add self first */
        resolved[nresolved++] = node;

        /* recurse into depends */
        for (int i = 0; i < pkg.ndepends; i++) {
            char depname[MAX_STR];
            dep_name_only(pkg.depends[i], depname);
            resolve(depname, depth + 1);
        }
    } else {
        /* no PKGBUILD — leaf node, unknown version */
        strncpy(node.ver, "?", MAX_STR - 1);
        resolved[nresolved++] = node;
    }
}

/* ── cmd_deptree ─────────────────────────────────────────────────────────── */
void cmd_deptree(int argc, char **argv) {
    if (argc == 0) die("No package specified.\nUsage: lpm -D <package>");

    for (int a = 0; a < argc; a++) {
        nresolved = 0;
        resolve(argv[a], 0);

        if (nresolved == 0) {
            fprintf(stderr, C_RED "error: " C_RESET
                    "No PKGBUILD found for '%s'\n", argv[a]);
            continue;
        }

        /* header */
        printf("\n");
        printf(C_BOLD "  %-4s  %-28s  %s\n" C_RESET,
               "src", "package", "version");
        printf("  ────  ────────────────────────────  ─────────────\n");

        for (int i = 0; i < nresolved; i++) {
            DepNode *n = &resolved[i];

            /* src column: Y/N */
            const char *src_col;
            if (n->has_src)
                src_col = C_GREEN "Y  " C_RESET;
            else
                src_col = C_YELLOW "N  " C_RESET;

            /* installed marker */
            const char *inst = n->installed
                ? " " C_CYAN "[installed]" C_RESET : "";

            /* indent by depth */
            char indent[64] = "";
            for (int d = 0; d < n->depth; d++)
                strncat(indent, "  ", sizeof(indent) - strlen(indent) - 1);

            printf("  %s  %s" C_BOLD "%-*s" C_RESET "  %s%s\n",
                   src_col,
                   indent,
                   (int)(28 - n->depth * 2),
                   n->name,
                   n->ver[0] ? n->ver : "?",
                   inst);
        }

        /* summary */
        int total    = nresolved;
        int missing  = 0;
        int installed = 0;
        for (int i = 0; i < nresolved; i++) {
            if (!resolved[i].has_src) missing++;
            if (resolved[i].installed) installed++;
        }

        printf("  ────  ────────────────────────────  ─────────────\n");
        printf("  Total: " C_BOLD "%d" C_RESET
               "  installed: " C_CYAN "%d" C_RESET,
               total, installed);
        if (missing)
            printf("  no PKGBUILD: " C_YELLOW "%d" C_RESET, missing);
        printf("\n\n");
    }
}
