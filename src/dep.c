#include "lpm.h"

#define MAX_QUEUE 256

/* repo folders to search — same order as cmd_sync.
 * REPO_BASE itself now comes from lpm.h (single source of truth). */

typedef struct {
    char name[MAX_STR];
    char ver[MAX_STR];
    char inst_ver[MAX_STR];       /* currently installed version, "" if none */
    char required_by[MAX_STR];    /* parent package that requires this dep */
    char constraint[64];          /* e.g. ">=2.3" — empty means any version */
    char folder[16];              /* base / extra / lotus / ? */
    int  installed;
    int  ver_too_old;             /* 1 = installed but version constraint fails */
    int  has_src;
    int  is_binary;               /* 1 = pkgtype=binary, will sync .lpkg */
    int  depth;
} DepNode;

typedef enum {
    LLPM_DEP_ANY = 0,
    LLPM_DEP_EQ,
    LLPM_DEP_GE,
    LLPM_DEP_LE,
    LLPM_DEP_GT,
    LLPM_DEP_LT
} llpm_depmod_t;

typedef struct {
    char name[MAX_STR];
    char ver[MAX_STR];
    llpm_depmod_t op;
} DepSpec;

static void dep_parse(const char *spec, DepSpec *out);
static int dep_constraint_satisfied(const DepSpec *dep, const char *installed_ver);

static DepNode resolved[MAX_QUEUE];
static int     nresolved = 0;
static int     build_order[MAX_QUEUE];
static int     nbuild = 0;
static int     visited[MAX_QUEUE];
static int     in_stack[MAX_QUEUE];
static Package  pkg_cache[MAX_QUEUE];
static int     pkg_cached[MAX_QUEUE];

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

static int parse_cached(const char *pkgname, Package *out) {
    int idx = index_of(pkgname);
    if (idx >= 0 && pkg_cached[idx]) {
        *out = pkg_cache[idx];
        return 0;
    }
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgname);
    if (pkgbuild_parse_fast(pbfile, out) != 0) return -1;
    if (idx >= 0) {
        pkg_cache[idx] = *out;
        pkg_cached[idx] = 1;
    }
    return 0;
}

static int parse_int_segment(const char *s, int *consumed) {
    int value = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        value = (value * 10) + (s[i] - '0');
        i++;
    }
    *consumed = i;
    return value;
}

static int stage_rank(const char *s, int *consumed) {
    if (strncmp(s, "alpha", 5) == 0) { *consumed = 5; return 0; }
    if (strncmp(s, "beta", 4) == 0)  { *consumed = 4; return 1; }
    if (strncmp(s, "rc", 2) == 0)    { *consumed = 2; return 2; }
    *consumed = 0;
    return 3;
}

static int llpm_vercmp(const char *a, const char *b) {
    const char *pa = a;
    const char *pb = b;
    int eca = 0;
    int ecb = 0;
    int epoch_a = 0;
    int epoch_b = 0;

    if (!a || !b) return 0;

    epoch_a = parse_int_segment(pa, &eca);
    if (eca > 0 && pa[eca] == ':') pa += (eca + 1);
    else epoch_a = 0;

    epoch_b = parse_int_segment(pb, &ecb);
    if (ecb > 0 && pb[ecb] == ':') pb += (ecb + 1);
    else epoch_b = 0;

    if (epoch_a != epoch_b) return (epoch_a > epoch_b) ? 1 : -1;

    while (*pa || *pb) {
        int ca = 0, cb = 0;
        int has_num_a = (*pa >= '0' && *pa <= '9');
        int has_num_b = (*pb >= '0' && *pb <= '9');
        int sa = has_num_a ? parse_int_segment(pa, &ca) : 0;
        int sb = has_num_b ? parse_int_segment(pb, &cb) : 0;

        if (sa != sb) return (sa > sb) ? 1 : -1;
        pa += ca;
        pb += cb;

        if (*pa == '.' || *pa == '-') pa++;
        if (*pb == '.' || *pb == '-') pb++;

        if (!*pa && *pb) {
            int z = 0, c = 0;
            if (*pb >= '0' && *pb <= '9') {
                z = parse_int_segment(pb, &c);
                if (z != 0) return -1;
                pb += c;
                if (*pb == '.' || *pb == '-') pb++;
                continue;
            }
        }
        if (!*pb && *pa) {
            int z = 0, c = 0;
            if (*pa >= '0' && *pa <= '9') {
                z = parse_int_segment(pa, &c);
                if (z != 0) return 1;
                pa += c;
                if (*pa == '.' || *pa == '-') pa++;
                continue;
            }
        }

        if ((*pa < '0' || *pa > '9') && (*pb < '0' || *pb > '9')) {
            int ta = 0, tb = 0;
            int ra = stage_rank(pa, &ta);
            int rb = stage_rank(pb, &tb);
            if (ra != rb) return (ra > rb) ? 1 : -1;
            pa += ta;
            pb += tb;
            while (*pa == '.' || *pa == '-') pa++;
            while (*pb == '.' || *pb == '-') pb++;
        }
    }
    return 0;
}

static void dep_parse(const char *spec, DepSpec *out) {
    const char *p = spec;
    memset(out, 0, sizeof(*out));
    out->op = LLPM_DEP_ANY;

    while (*p == ' ') p++;
    size_t ni = 0;
    while (*p && *p != ' ' && *p != '<' && *p != '>' && *p != '=') {
        if (ni + 1 < sizeof(out->name)) out->name[ni++] = *p;
        p++;
    }
    out->name[ni] = '\0';
    while (*p == ' ') p++;

    if (p[0] == '>' && p[1] == '=') { out->op = LLPM_DEP_GE; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { out->op = LLPM_DEP_LE; p += 2; }
    else if (p[0] == '>') { out->op = LLPM_DEP_GT; p += 1; }
    else if (p[0] == '<') { out->op = LLPM_DEP_LT; p += 1; }
    else if (p[0] == '=') { out->op = LLPM_DEP_EQ; p += 1; }

    while (*p == ' ') p++;
    snprintf(out->ver, sizeof(out->ver), "%s", p);
}

static int dep_constraint_satisfied(const DepSpec *dep, const char *installed_ver) {
    int cmp;
    if (!dep || dep->op == LLPM_DEP_ANY) return 1;
    if (!installed_ver || !installed_ver[0] || !dep->ver[0]) return 0;
    cmp = llpm_vercmp(installed_ver, dep->ver);
    switch (dep->op) {
        case LLPM_DEP_EQ: return cmp == 0;
        case LLPM_DEP_GE: return cmp >= 0;
        case LLPM_DEP_LE: return cmp <= 0;
        case LLPM_DEP_GT: return cmp > 0;
        case LLPM_DEP_LT: return cmp < 0;
        case LLPM_DEP_ANY:
        default: return 1;
    }
}


static void collect(const char *pkgname, int depth) {
    if (already_seen(pkgname)) return;
    if (nresolved >= MAX_QUEUE) return;

    DepNode node;
    memset(&node, 0, sizeof(node));
    strncpy(node.name, pkgname, MAX_STR - 1);
    node.depth     = depth;
    node.installed = db_is_installed(pkgname);
    strncpy(node.folder, "?", sizeof(node.folder) - 1);

    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, pkgname);

    struct stat st;
    node.has_src = (stat(pbfile, &st) == 0);

    if (node.has_src) {
        Package pkg;
        if (parse_cached(pkgname, &pkg) != 0) {
            strncpy(node.ver, "?", MAX_STR - 1);
            resolved[nresolved++] = node;
            return;
        }
        node.is_binary = (pkg.type == PKG_TYPE_BINARY);
        snprintf(node.ver, MAX_STR, "%s", pkg.version);
        resolved[nresolved++] = node;
        for (int i = 0; i < pkg.ndepends; i++) {
            DepSpec dep;
            dep_parse(pkg.depends[i], &dep);
            char *iv = db_get_version(dep.name);
            int ok = dep_constraint_satisfied(&dep, iv);

            if (!ok) {
                collect(dep.name, depth + 1);
                /* after collect, find the node and annotate it */
                int nidx = index_of(dep.name);
                if (nidx >= 0) {
                    if (iv && iv[0])
                        snprintf(resolved[nidx].inst_ver,
                                 MAX_STR, "%s", iv);
                    snprintf(resolved[nidx].required_by,
                             MAX_STR, "%s", pkgname);
                    /* build constraint string e.g. ">=2.3" */
                    const char *opstr =
                        dep.op == LLPM_DEP_GE ? ">=" :
                        dep.op == LLPM_DEP_GT ? ">"  :
                        dep.op == LLPM_DEP_LE ? "<=" :
                        dep.op == LLPM_DEP_LT ? "<"  :
                        dep.op == LLPM_DEP_EQ ? "="  : "";
                    {
                        char *_dst = resolved[nidx].constraint;
                        /* write opstr (max 2 chars) */
                        int _ol = 0;
                        while (opstr[_ol] && _ol < 2) { _dst[_ol] = opstr[_ol]; _ol++; }
                        /* append version, truncate to 61 chars */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
                        strncpy(_dst + _ol, dep.ver, 63 - _ol);
#pragma GCC diagnostic pop
                        _dst[63] = (char)0;
                    }
                    /* mark as version-too-old if was installed */
                    if (iv && iv[0])
                        resolved[nidx].ver_too_old = 1;
                }
            }
            free(iv);
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
        Package pkg;
        if (parse_cached(resolved[idx].name, &pkg) == 0) {
            for (int d = 0; d < pkg.ndepends; d++) {
                DepSpec dep;
                dep_parse(pkg.depends[d], &dep);
                int di = index_of(dep.name);
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
                      char out[][MAX_STR], int maxout, int build_all) {
    nresolved = 0;
    memset(pkg_cached, 0, sizeof(pkg_cached));
    collect(pkgname, 0);
    toposort();
    int n = 0;
    for (int i = 0; i < nbuild && n < maxout; i++) {
        DepNode *node = &resolved[build_order[i]];
        if (node->has_src && (build_all || !node->installed))
            strncpy(out[n++], node->name, MAX_STR - 1);
    }
    return n;
}

/* ── cmd_deptree ─────────────────────────────────────────────────────────── */

/* Recursively print a package's dependency tree with box-drawing glyphs.
 * Walks the FULL depends graph (not just unsatisfied deps), so it matches
 * the `lpm deps` contract. `path`/`pathlen` is the current root-to-node
 * chain, used to break cycles (a dep already on this branch is printed but
 * not re-expanded). Prints `name` and then its subtree. */
static void print_dep_tree(const char *name, const char *prefix,
                           int is_last, char path[][MAX_STR], int pathlen) {
    printf("%s%s%s\n", prefix, is_last ? "└─ " : "├─ ", name);

    /* cycle guard: already on this branch → don't expand */
    for (int i = 0; i < pathlen; i++)
        if (!strcmp(path[i], name)) return;
    if (pathlen >= MAX_QUEUE) return;
    snprintf(path[pathlen], MAX_STR, "%s", name);

    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, name);
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) return;

    char child_prefix[MAX_STR];
    snprintf(child_prefix, sizeof(child_prefix), "%s%s",
             prefix, is_last ? "   " : "│  ");
    for (int i = 0; i < pkg.ndepends; i++) {
        DepSpec dep;
        dep_parse(pkg.depends[i], &dep);
        int dup = 0;
        for (int k = 0; k < i; k++) {
            DepSpec d2; dep_parse(pkg.depends[k], &d2);
            if (!strcmp(d2.name, dep.name)) { dup = 1; break; }
        }
        if (dup) continue;
        print_dep_tree(dep.name, child_prefix, i == pkg.ndepends - 1,
                       path, pathlen + 1);
    }
}

/* Expand only the ROOT's children (root itself is printed by the caller,
 * without a glyph, matching the contract's `firefox\n├─ gtk3` shape). */
static void print_dep_roots_children(const char *name,
                                     char path[][MAX_STR], int pathlen) {
    snprintf(path[pathlen], MAX_STR, "%s", name);
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, name);
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) return;
    for (int i = 0; i < pkg.ndepends; i++) {
        DepSpec dep;
        dep_parse(pkg.depends[i], &dep);
        int dup = 0;
        for (int k = 0; k < i; k++) {
            DepSpec d2; dep_parse(pkg.depends[k], &d2);
            if (!strcmp(d2.name, dep.name)) { dup = 1; break; }
        }
        if (dup) continue;
        print_dep_tree(dep.name, "", i == pkg.ndepends - 1, path, pathlen + 1);
    }
}

void cmd_deptree(int argc, char **argv) {
    if (argc == 0) die("No package specified.\nUsage: lpm deps <package>");

    for (int a = 0; a < argc; a++) {
        char path[MAX_QUEUE][MAX_STR];
        printf("%s\n", argv[a]);           /* root: name only, no glyph */
        print_dep_roots_children(argv[a], path, 0);
        if (a + 1 < argc) printf("\n");
    }
}

/* called from cmd_sync — set folder info on resolved nodes */
void dep_set_folder(const char *pkgname, const char *folder) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, pkgname) == 0) {
            strncpy(resolved[i].folder, folder, sizeof(resolved[i].folder) - 1);
            return;
        }
}

/* ── dep_resolve_queue_multi ─────────────────────────────────────────────── */
/* Collect N packages + toposort once — replaces loop of dep_resolve_queue()×N */
int dep_resolve_queue_multi(char **pkgnames, int npkgs,
                             char out[][MAX_STR], int maxout, int build_all) {
    nresolved = 0;
    memset(pkg_cached, 0, sizeof(pkg_cached));
    for (int i = 0; i < npkgs; i++)
        collect(pkgnames[i], 0);
    toposort();
    int n = 0;
    for (int i = 0; i < nbuild && n < maxout; i++) {
        DepNode *node = &resolved[build_order[i]];
        if (node->has_src && (build_all || !node->installed))
            strncpy(out[n++], node->name, MAX_STR - 1);
    }
    return n;
}

/* ── dep_meta_cache_invalidate ───────────────────────────────────────────── */
/* Invalidate in-process parse cache (call after lpm update) */
void dep_meta_cache_invalidate(void) {
    memset(pkg_cached, 0, sizeof(pkg_cached));
}

/* ── dep_get_recommends ──────────────────────────────────────────────────── */
int dep_get_recommends(const char *pkgname, char out[][MAX_STR], int maxout) {
    Package pkg;
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgname);
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) return 0;

    int n = pkg.nrecommends;
    if (n > maxout) n = maxout;
    for (int i = 0; i < n; i++)
        strncpy(out[i], pkg.recommends[i], MAX_STR - 1);
    return n;
}
