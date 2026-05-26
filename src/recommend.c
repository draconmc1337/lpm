/* recommend.c — optional/recommend dependency prompt
 *
 * Tính năng: sau khi install xong depends bắt buộc, hỏi user về
 * recommends (packages được khuyến nghị nhưng không bắt buộc).
 *
 * Luồng:
 *   depends=  → cài tự động (không hỏi, như hiện tại)
 *   recommends= → hỏi từng cái: [Y/n] Install <pkg>? (<description>)
 *
 * Caller (build.c / cmd_sync) gọi:
 *   lpm_prompt_recommends(pkgname, argc, argv)
 * sau khi install package chính xong.
 */

#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


/* Đọc description của một package từ PKGBUILD của nó.
 * Trả về static buffer — chỉ dùng ngay, không lưu lại. */
static const char *get_pkg_description(const char *pkgname) {
    static char desc[512];
    desc[0] = '\0';

    char pbfile[LPM_PATH_MAX];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, pkgname);

    FILE *f = fopen(pbfile, "r");
    if (!f) return desc;

    char line[LPM_PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "pkgdesc=", 8) == 0 ||
            strncmp(p, "description=", 12) == 0) {
            char *val = strchr(p, '=');
            if (!val) continue;
            val++;
            /* strip quotes */
            if (*val == '"' || *val == '\'') val++;
            val[strcspn(val, "\n\r\"'\"")] = '\0';
            strncpy(desc, val, sizeof(desc) - 1);
            break;
        }
    }
    fclose(f);
    return desc;
}

/* lpm_prompt_recommends — prompt user untuk tiap recommend yang belum
 * installed, lalu install jika user setuju.
 *
 * pkgname  = package yang baru saja diinstall
 * install_fn = function pointer ke install logic (biasanya cmd_sync intern)
 *              Signature: int fn(const char *pkg)
 *              Return 0 = success, non-zero = fail
 *
 * Return: jumlah package recommend yang diinstall.
 */
int lpm_prompt_recommends(const char *pkgname, const LpmFlags *flags,
                           int (*install_fn)(const char *pkg)) {
    char recommends[LPM_MAX_DEPS][MAX_STR];
    int n = dep_get_recommends(pkgname, recommends, LPM_MAX_DEPS);
    if (n == 0) return 0;

    printf("\n" C_CYAN "::" C_RESET
           " Optional packages for " C_BOLD "%s" C_RESET ":\n", pkgname);

    int installed_count = 0;
    for (int i = 0; i < n; i++) {
        const char *rec = recommends[i];
        const char *desc = get_pkg_description(rec);

        /* tampilkan nama + description jika ada */
        if (desc[0]) {
            printf("  " C_BOLD "%s" C_RESET
                   C_GRAY " — %s" C_RESET "\n", rec, desc);
        } else {
            printf("  " C_BOLD "%s" C_RESET "\n", rec);
        }

        /* skip jika g_cfg.default_yes → install semua tanpa tanya */
        int do_install = 0;
        if (flags && flags->no_confirm) {
            do_install = 1;
        } else {
            char prompt[256];
            snprintf(prompt, sizeof(prompt),
                C_CYAN "   Install %s?" C_RESET " [y/N] ", rec);
            do_install = confirm(prompt);
        }

        if (do_install) {
            printf(C_CYAN "::" C_RESET " Installing recommend: %s\n", rec);
            if (install_fn && install_fn(rec) == 0) {
                installed_count++;
            } else {
                fprintf(stderr,
                    C_YELLOW "warning:" C_RESET
                    " failed to install recommend %s — skipping\n", rec);
            }
        } else {
            printf(C_GRAY "   skipped %s\n" C_RESET, rec);
        }
    }

    if (installed_count > 0)
        printf(C_GREEN "::" C_RESET
               " Installed %d/%d optional package(s)\n",
               installed_count, n);
    return installed_count;
}

#pragma GCC diagnostic pop
