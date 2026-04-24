#include "lpm.h"

int cksum_verify(const char *path, const char *expected, CksumType type) {
    if (type == CKSUM_SKIP || !expected || !expected[0]) return 0;

    char cmd[LPM_PATH_MAX + 256];
    char actual[129] = "";

    if (type == CKSUM_SHA256)
        snprintf(cmd, sizeof(cmd), "sha256sum '%s' | cut -d' ' -f1", path);
    else if (type == CKSUM_SHA512)
        snprintf(cmd, sizeof(cmd), "sha512sum '%s' | cut -d' ' -f1", path);
    else if (type == CKSUM_MD5)
        snprintf(cmd, sizeof(cmd), "md5sum '%s' | cut -d' ' -f1", path);
    else return 0;

    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    fgets(actual, sizeof(actual), p);
    pclose(p);
    actual[strcspn(actual, "\n")] = '\0';

    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, C_RED "error:" C_RESET
            " checksum mismatch for %s\n"
            "  expected: %s\n"
            "  got:      %s\n", path, expected, actual);
        return -1;
    }
    return 0;
}
