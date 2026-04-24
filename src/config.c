#include "lpm.h"
#include <ctype.h>
#include <stdarg.h>

LpmConfig g_cfg;
int       g_verbose = 0;

void lpm_config_defaults(LpmConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->cflags,   "-O2 -pipe -fstack-protector-strong -D_FORTIFY_SOURCE=2", sizeof(cfg->cflags)-1);
    strncpy(cfg->cxxflags, "-O2 -pipe -fstack-protector-strong -D_FORTIFY_SOURCE=2", sizeof(cfg->cxxflags)-1);
    strncpy(cfg->ldflags,  "-Wl,-z,relro,-z,now", sizeof(cfg->ldflags)-1);
    strncpy(cfg->makeflags,"", sizeof(cfg->makeflags)-1);
    strncpy(cfg->cc,  "gcc", sizeof(cfg->cc)-1);
    strncpy(cfg->cxx, "g++", sizeof(cfg->cxx)-1);
    cfg->jobs = 0;
    strncpy(cfg->build_dir, "/var/cache/lpm", sizeof(cfg->build_dir)-1);
    strncpy(cfg->pkg_dest,  "/var/cache/lpm", sizeof(cfg->pkg_dest)-1);
    strncpy(cfg->src_dest,  "/var/cache/lpm", sizeof(cfg->src_dest)-1);
    cfg->color = 1; cfg->confirm = 1;
    cfg->keep_src = 0; cfg->keep_pkg = 0;
    cfg->check_space = 1; cfg->parallel_dl = 1;
    cfg->max_dl_threads = 4; cfg->verify_sig = 0;
    strncpy(cfg->downloader, "auto", sizeof(cfg->downloader)-1);
    /* compat fields for old API */
    cfg->default_yes    = 0;
    cfg->default_strict = 0;
    cfg->run_check      = 0;
    cfg->strict_build   = 0;
    cfg->n_critical     = 0;
    cfg->n_ignore       = 0;
    strncpy(cfg->log_dir,   LPM_LOG_DIR,   sizeof(cfg->log_dir)-1);
    strncpy(cfg->files_dir, LPM_FILES_DIR, sizeof(cfg->files_dir)-1);
}

static void strip_quotes(char *s) {
    size_t len = strlen(s);
    if (len < 2) return;
    if ((s[0]=='"'&&s[len-1]=='"')||(s[0]=='\''&&s[len-1]=='\'')) {
        memmove(s, s+1, len-2); s[len-2]='\0';
    }
}

static int parse_bool(const char *val) {
    if (!strcmp(val,"y")||!strcmp(val,"Y")||!strcmp(val,"1")||!strcmp(val,"yes")) return 1;
    return 0;
}

static void add_pkg_list(char list[][64], int *count, const char *val) {
    /* space/comma separated */
    char tmp[1024]; strncpy(tmp, val, sizeof(tmp)-1);
    char *tok = strtok(tmp, " ,");
    while (tok && *count < 256) {
        strncpy(list[(*count)++], tok, 63);
        tok = strtok(NULL, " ,");
    }
}

int lpm_config_load(const char *path, LpmConfig *cfg) {
    lpm_config_defaults(cfg);
    FILE *f = fopen(path ? path : LPM_CONF_FILE, "r");
    if (!f) return 0;  /* defaults are fine */

    char line[1024]; int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p=='#' || *p=='\0') continue;

        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq+1;
        char *kend = key+strlen(key)-1;
        while (kend>key && isspace((unsigned char)*kend)) *kend--='\0';
        while (isspace((unsigned char)*val)) val++;
        strip_quotes(val);

        if      (!strcmp(key,"CFLAGS"))         strncpy(cfg->cflags,    val, sizeof(cfg->cflags)-1);
        else if (!strcmp(key,"CXXFLAGS"))        strncpy(cfg->cxxflags,  val, sizeof(cfg->cxxflags)-1);
        else if (!strcmp(key,"LDFLAGS"))         strncpy(cfg->ldflags,   val, sizeof(cfg->ldflags)-1);
        else if (!strcmp(key,"MAKEFLAGS"))       strncpy(cfg->makeflags, val, sizeof(cfg->makeflags)-1);
        else if (!strcmp(key,"CC"))              strncpy(cfg->cc,        val, sizeof(cfg->cc)-1);
        else if (!strcmp(key,"CXX"))             strncpy(cfg->cxx,       val, sizeof(cfg->cxx)-1);
        else if (!strcmp(key,"JOBS"))            cfg->jobs            = atoi(val);
        else if (!strcmp(key,"BUILDDIR"))        strncpy(cfg->build_dir, val, sizeof(cfg->build_dir)-1);
        else if (!strcmp(key,"PKGDEST"))         strncpy(cfg->pkg_dest,  val, sizeof(cfg->pkg_dest)-1);
        else if (!strcmp(key,"SRCDEST"))         strncpy(cfg->src_dest,  val, sizeof(cfg->src_dest)-1);
        else if (!strcmp(key,"COLOR"))           cfg->color           = parse_bool(val);
        else if (!strcmp(key,"CONFIRM"))         cfg->confirm         = parse_bool(val);
        else if (!strcmp(key,"KEEP_SRC"))        cfg->keep_src        = parse_bool(val);
        else if (!strcmp(key,"KEEP_PKG"))        cfg->keep_pkg        = parse_bool(val);
        else if (!strcmp(key,"CHECK_SPACE"))     cfg->check_space     = parse_bool(val);
        else if (!strcmp(key,"PARALLEL_DL"))     cfg->parallel_dl     = parse_bool(val);
        else if (!strcmp(key,"MAX_DL_THREADS"))  cfg->max_dl_threads  = atoi(val);
        else if (!strcmp(key,"VERIFY_SIG"))      cfg->verify_sig      = parse_bool(val);
        else if (!strcmp(key,"DOWNLOADER"))      strncpy(cfg->downloader, val, sizeof(cfg->downloader)-1);
        else if (!strcmp(key,"DEFAULT_YES"))     cfg->default_yes     = parse_bool(val);
        else if (!strcmp(key,"DEFAULT_STRICT"))  cfg->default_strict  = parse_bool(val);
        else if (!strcmp(key,"RUN_CHECK"))       cfg->run_check       = parse_bool(val);
        else if (!strcmp(key,"STRICT_BUILD"))    cfg->strict_build    = parse_bool(val);
        else if (!strcmp(key,"CriticalPkg"))     add_pkg_list(cfg->critical_pkgs, &cfg->n_critical, val);
        else if (!strcmp(key,"IgnorePkg"))       add_pkg_list(cfg->ignore_pkgs,   &cfg->n_ignore,   val);
        else if (!strcmp(key,"LogDir"))          strncpy(cfg->log_dir,   val, sizeof(cfg->log_dir)-1);
        else if (!strcmp(key,"FilesDir"))        strncpy(cfg->files_dir, val, sizeof(cfg->files_dir)-1);
    }
    fclose(f);
    if (cfg->max_dl_threads < 1)  cfg->max_dl_threads = 1;
    if (cfg->max_dl_threads > 16) cfg->max_dl_threads = 16;
    if (cfg->jobs < 0) cfg->jobs = 0;
    return 0;
}

int lpm_config_is_critical(const LpmConfig *cfg, const char *pkgname) {
    for (int i = 0; i < cfg->n_critical; i++)
        if (!strcmp(cfg->critical_pkgs[i], pkgname)) return 1;
    return 0;
}

int lpm_config_is_ignored(const LpmConfig *cfg, const char *pkgname) {
    for (int i = 0; i < cfg->n_ignore; i++)
        if (!strcmp(cfg->ignore_pkgs[i], pkgname)) return 1;
    return 0;
}

void lpm_config_dump(const LpmConfig *cfg) {
    printf(C_PINK ":: lpm config\n" C_RESET);
    printf("  CFLAGS    = %s\n", cfg->cflags);
    printf("  CXXFLAGS  = %s\n", cfg->cxxflags);
    printf("  LDFLAGS   = %s\n", cfg->ldflags);
    printf("  MAKEFLAGS = %s\n", cfg->makeflags);
    printf("  CC/CXX    = %s / %s\n", cfg->cc, cfg->cxx);
    printf("  JOBS      = %d%s\n", cfg->jobs, cfg->jobs==0?" (auto)":"");
    printf("  BUILDDIR  = %s\n", cfg->build_dir);
    printf("  DOWNLOADER= %s\n", cfg->downloader);
    printf("  CriticalPkg(%d): ", cfg->n_critical);
    for (int i=0;i<cfg->n_critical;i++) printf("%s ", cfg->critical_pkgs[i]);
    printf("\n");
}
