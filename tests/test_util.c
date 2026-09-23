/* Child failures must never be reported as successful filesystem work. */
#include "lpm.h"
#include <assert.h>

int main(void) {
    assert(util_run("exit 0") == 0);
    assert(util_run("exit 7") == 7);
    /* Only the spawned shell is signalled; no host filesystem operations. */
    int rc = util_run("kill -TERM $$");
    if (rc == 0) {
        fprintf(stderr, "FAIL: util_run reported a SIGTERM-killed child as success\n");
        return 1;
    }
    assert(util_run("kill -KILL $$") != 0);
    puts("test_util: normal and signal exit status checks passed");
    return 0;
}
