#include "lpm.h"

#define MAX_QUEUE 256

typedef struct {
    char name[MAX_STR];
    char ver[MAX_STR];
    int  installed;
    int  has_src;
    int  depth;
} DepNode;

static DepNode resolved[MAX_QUEUE];
static int     nresolved = 0;
static int     build_order[MAX_QUEUE];
static int     nbuild = 0;
static int     visited[MAX_QUEUE];
static int     in_stack[MAX_QUEUE];

static int already_seen(const char *name) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, name) == 0) return 1;
    return 0;
}

static int index_of(const char *name) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, name) == 0) return i;
    return -1;
}

static void dep_name_only(const char *spec, char *out) {
    strncpy(out, spec, MAX_STR - 1);
    out[MAX_STR - 1] = '\0';
    char *op = strpbrk(out, "><= ");
    if (op) *op = '\0';
}

static void collect(const char *pkgname, int depth) {
    if (already_seen(pkgname)) return;
    if (nresolved >= MAX_QUEUE) return;

    DepNode node;
    memset(&node, 0, sizeof(node));
    strncpy(node.name, pkgname, MAX_STR - 1);
    node.depth     = depth;
    node.installed = db_is_installed(pkgname);

    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, pkgname);

    struct stat st;
    node.has_src = (stat(pbfile, &st) == 0);

    if (node.has_src) {
        Pkg pkg;
        pkgbuild_parse(pbfile, &pkg);
        strncpy(node.ver, pkg.pkgver, MAX_STR - 1);
        resolved[nresolved++] = node;
        for (int i = 0; i < pkg.ndepends; i++) {
            char depname[MAX_STR];
            dep_name_only(pkg.depends[i], depname);
            collect(depname, depth + 1);
        }
    } else {
        strncpy(node.ver, "?", MAX_STR - 1);
        resolved[nresolved++] = node;
    }
}

static void topo_visit(int idx) {
    if (visited[idx]) return;
    if (in_stack[idx]) {
        warn("circular dependency: %s", resolved[idx].name);
        return;
    }
    in_stack[idx] = 1;
    if (resolved[idx].has_src) {
        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, resolved[idx].name);
        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) == 0) {
            for (int d = 0; d < pkg.ndepends; d++) {
                char depname[MAX_STR];
                dep_name_only(pkg.depends[d], depname);
                int di = index_of(depname);
                if (di >= 0) topo_visit(di);
            }
        }
    }
    in_stack[idx] = 0;
    visited[idx]  = 1;
    build_order[nbuild++] = idx;
}

static void toposort(void) {
    memset(visited,  0, sizeof(visited));
    memset(in_stack, 0, sizeof(in_stack));
    nbuild = 0;
    for (int i = 0; i < nresolved; i++)
        topo_visit(i);
}

int dep_resolve_queue(const char *pkgname,
                      char out[][MAX_STR], int maxout) {
    nresolved = 0;
    collect(pkgname, 0);
    toposort();
    int n = 0;
    for (int i = 0; i < nbuild && n < maxout; i++) {
        DepNode *node = &resolved[build_order[i]];
        if (node->has_src && !node->installed)
            strncpy(out[n++], node->name, MAX_STR - 1);
    }
    return n;
}

void cmd_deptree(int argc, char **argv) {
    if (argc == 0) die("No package specified.\nUsage: lpm -D <package>");

    for (int a = 0; a < argc; a++) {
        nresolved = 0;
        collect(argv[a], 0);

        if (nresolved == 0) {
            fprintf(stderr, C_RED "error: " C_RESET
                    "No PKGBUILD found for '%s'\n", argv[a]);
            continue;
        }

        toposort();

        printf("\n");
        printf(C_BOLD "  %-4s  %-28s  %s\n" C_RESET,
               "src", "package", "version");
        printf("  ────  ────────────────────────────  ─────────────\n");

        for (int i = 0; i < nresolved; i++) {
            DepNode *n = &resolved[i];
            const char *src_col = n->has_src
                ? C_GREEN "Y  " C_RESET : C_YELLOW "N  " C_RESET;
            const char *inst = n->installed
                ? " " C_CYAN "[installed]" C_RESET : "";
            char indent[64] = "";
            for (int d = 0; d < n->depth; d++)
                strncat(indent, "  ", sizeof(indent) - strlen(indent) - 1);
            char namebuf[64];
            snprintf(namebuf, sizeof(namebuf), "%s%s", indent, n->name);
            printf("  %s  " C_BOLD "%-28s" C_RESET "  %s%s\n",
                   src_col, namebuf,
                   n->ver[0] ? n->ver : "?", inst);
        }

        printf("  ────  ────────────────────────────  ─────────────\n");

        int inst_count = 0, to_build = 0, missing = 0;
        for (int i = 0; i < nresolved; i++) {
            if (!resolved[i].has_src)  missing++;
            if (resolved[i].installed) inst_count++;
            if (resolved[i].has_src && !resolved[i].installed) to_build++;
        }
        printf("  Total: " C_BOLD "%d" C_RESET
               "  installed: " C_CYAN "%d" C_RESET
               "  to build: " C_YELLOW "%d" C_RESET,
               nresolved, inst_count, to_build);
        if (missing) printf("  no PKGBUILD: " C_RED "%d" C_RESET, missing);
        printf("\n");

        if (to_build > 0) {
            printf("\n  Build order:\n");
            int order = 1;
            /* use resolved[] display order not build_order[] to avoid dups */
            for (int i = 0; i < nresolved; i++) {
                DepNode *n = &resolved[i];
                if (n->has_src && !n->installed)
                    printf("    " C_CYAN "%d." C_RESET " %s\n",
                           order++, n->name);
            }
        }
        printf("\n");
    }
}
