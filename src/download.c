*** Begin Patch
*** Update File: src/download.c
@@
-Downloader dl_detect(const char *override) {
-    if (override && strcmp(override, "auto") != 0) {
-        if (!strcmp(override, "wget")) return DL_WGET;
-        if (!strcmp(override, "curl")) return DL_CURL;
-    }
-    if (system("command -v wget >/dev/null 2>&1") == 0) return DL_WGET;
-    if (system("command -v curl >/dev/null 2>&1") == 0) return DL_CURL;
-    return DL_NONE;
-}
+Downloader dl_detect(const char *override) {
+    /* Single-downloader policy: only curl. Respect explicit 'curl', otherwise auto-detect curl. */
+    if (override && strcmp(override, "auto") != 0) {
+        if (!strcmp(override, "curl")) return DL_CURL;
+        return DL_NONE;
+    }
+    if (system("command -v curl >/dev/null 2>&1") == 0) return DL_CURL;
+    return DL_NONE;
+}
@@
-static void *dl_worker(void *arg) {
+/* Helper: run curl and capture HTTP code. Returns shell exit code (WEXITSTATUS) or -1 on popen failure. */
+static int run_curl_capture_code(const char *url, const char *outpath, int connect_timeout, int max_time, char *http_code, size_t http_code_sz) {
+    char cmd[LPM_URL_MAX + 512];
+    /* -sS quiet but still fail on HTTP errors; -f to not output on 4xx/5xx. Use -w to emit HTTP code. */
+    snprintf(cmd, sizeof(cmd),
+        "curl -sS -f -w '%%{http_code}' --connect-timeout %d --max-time %d -o '%s' '%s' 2>/dev/null",
+        connect_timeout, max_time, outpath, url);
+    FILE *p = popen(cmd, "r");
+    if (!p) return -1;
+    if (http_code && http_code_sz) {
+        if (!fgets(http_code, (int)http_code_sz, p)) http_code[0] = '\0';
+        /* strip newline */
+        http_code[strcspn(http_code, "\n")] = '\0';
+    }
+    int rc = pclose(p);
+    if (rc == -1) return -1;
+    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
+    return -1;
+}
+
+static void *dl_worker(void *arg) {
     WorkerArg *wa  = (WorkerArg *)arg;
     FetchJob  *job = wa->job;
     int        sl  = wa->slot;
     free(wa);
 
     Downloader dl = dl_detect(g_cfg.downloader);
     if (dl == DL_NONE) {
         pthread_mutex_lock(&g_mtx);
         g_slots[sl].finished = -1;
         pthread_mutex_unlock(&g_mtx);
-        job->result = -1;
+        job->result = ERR_DOWNLOADER_MISSING;
         return NULL;
     }
-    DBG(2, "fetch [%s] %s -> %s", dl == DL_WGET ? "wget" : "curl",
-        job->url, job->dest);
+    DBG(2, "fetch [curl] %s -> %s", job->url, job->dest);
 
     /* use a temp file */
     char part[LPM_PATH_MAX + 8];
     snprintf(part, sizeof(part), "%s.part", job->dest);
     remove(part);
-
-    /* build command that prints progress to a temp file we poll */
-    char prog_file[LPM_PATH_MAX];
-    snprintf(prog_file, sizeof(prog_file), "/tmp/lpm_dl_%d.prog", sl);
-
-    char cmd[LPM_URL_MAX + LPM_PATH_MAX + 256];
-    if (dl == DL_WGET) {
-        /* wget writes progress to stderr; redirect to prog_file */
-        snprintf(cmd, sizeof(cmd),
-            "wget -q --show-progress --progress=dot:mega "
-            "--timeout=30 --tries=3 -O '%s' '%s' 2>'%s'",
-            part, job->url, prog_file);
-    } else {
-        /* curl: write progress to prog_file */
-        snprintf(cmd, sizeof(cmd),
-            "curl -L --retry 3 --connect-timeout 30 "
-            "--progress-bar -o '%s' '%s' 2>'%s'",
-            part, job->url, prog_file);
-    }
-
-    /* spawn download in background via fork+system trick:
-       we want to poll progress while it runs */
-    int rc = system(cmd);
-
-    struct stat st;
-    int ok = (rc == 0 && stat(part, &st) == 0 && st.st_size > 0);
-
-    if (ok) rename(part, job->dest);
-    else    remove(part);
+    /* perform download via curl and capture HTTP code */
+    char http_code[16] = "";
+    int curl_rc = run_curl_capture_code(job->url, part, 30, 300, http_code, sizeof(http_code));
+
+    struct stat st;
+    int ok = (curl_rc == 0 && stat(part, &st) == 0 && st.st_size > 0);
+
+    if (ok) {
+        rename(part, job->dest);
+    } else {
+        remove(part);
+    }
@@
-    /* verify checksum */
+    /* verify checksum */
     if (ok && job->cksum_type != CKSUM_SKIP && job->checksum[0]) {
         DBG(2, "verify checksum [%d] %s", job->cksum_type, job->dest);
         ok = (cksum_verify(job->dest, job->checksum, job->cksum_type) == 0);
-        DBG(2, "checksum %s: %s", ok ? "OK" : "MISMATCH", job->dest);
+        DBG(2, "checksum %s: %s", ok ? "OK" : "MISMATCH", job->dest);
+        if (!ok) job->result = ERR_REPO_CKSUM;
     }
-
-    remove(prog_file);
+
@@
-    if (ok) {
-        g_slots[sl].finished = 1;
-        /* fill bar to 100% */
-        if (g_slots[sl].total > 0)
-            g_slots[sl].done = g_slots[sl].total;
-        else {
-            struct stat fs;
-            if (stat(job->dest, &fs) == 0)
-                g_slots[sl].done = g_slots[sl].total = fs.st_size;
-        }
-    } else {
-        g_slots[sl].finished = -1;
-    }
+    if (ok) {
+        g_slots[sl].finished = 1;
+        if (g_slots[sl].total > 0) g_slots[sl].done = g_slots[sl].total;
+        else { struct stat fs; if (stat(job->dest, &fs) == 0) g_slots[sl].done = g_slots[sl].total = fs.st_size; }
+    } else {
+        g_slots[sl].finished = -1;
+        /* if job->result hasn't been set above (e.g. checksum), map curl_rc/http_code */
+        if (job->result == 0 || job->result == -1) {
+            if (curl_rc == 6) job->result = ERR_NET_DNS;
+            else if (curl_rc == 7) job->result = ERR_NET_CONNREFUSED;
+            else if (curl_rc == 28) job->result = ERR_NET_TIMEOUT;
+            else if (curl_rc == 35 || curl_rc == 51 || curl_rc == 60) job->result = ERR_NET_TLS;
+            else if (curl_rc == 52 || curl_rc == 56) job->result = ERR_NET_CONNRESET;
+            else if (curl_rc == 22) {
+                int h = http_code[0] ? atoi(http_code) : 0;
+                if (h == 404) job->result = ERR_HTTP_404;
+                else if (h == 403) job->result = ERR_HTTP_403;
+                else if (h == 429) job->result = ERR_HTTP_429;
+                else if (h == 500) job->result = ERR_HTTP_500;
+                else if (h == 502) job->result = ERR_HTTP_502;
+                else if (h == 503) job->result = ERR_HTTP_503;
+                else job->result = ERR_REPO_INVALID;
+            } else if (curl_rc == -1) job->result = ERR_DOWNLOADER_MISSING;
+            else job->result = ERR_NET_TIMEOUT;
+        }
+    }
@@
-    job->result = ok ? 0 : -1;
+    if (ok) job->result = 0;
     return NULL;
 }
*** End Patch
