/*
 * updatecheck.c — GAP-107, port of the macOS UpdateChecker (9670c16).
 *
 * Networking is curl, spawned async. The previous implementation used
 * `curl -fsSL` with stderr silenced, which is precisely what made every
 * failure look identical: -f makes curl exit non-zero with an EMPTY body
 * on any 4xx/5xx, so the status code never reached the app and a rate
 * limit, a proxy block, a 404 and a dead server all reported "check your
 * internet connection".
 *
 * Here curl keeps the body, writes the response headers to a file (-D)
 * and appends the status code to stdout (-w), so we can tell those cases
 * apart — and send If-None-Match, which is the fix that matters: GitHub
 * does not bill a 304 against the 60-per-hour-per-IP unauthenticated
 * limit, and that ceiling is shared by everyone behind the same NAT.
 */
#include "updatecheck.h"
#include "prefs.h"
#include "paths.h"
#include "branding.h"
#include "i18n.h"

#include <gio/gio.h>
#include <glib/gstdio.h>   /* g_unlink */
#include <stdlib.h>
#include <string.h>

/* Same endpoint macOS uses: the public macOS repo is the release
 * source-of-truth and, unlike the private GTK4 repo, its releases API is
 * reachable without authentication. */
static const char *kReleasesAPI =
    "https://api.github.com/repos/nextpad-plus-plus/"
    "nextpad-plus-plus-macos/releases/latest";
static const char *kDownloadPage =
    "https://github.com/nextpad-plus-plus/nextpad-plus-plus-macos/releases/latest";

#define CHECK_INTERVAL   (24 * 60 * 60)   /* daily                     */
#define RETRY_FLOOR      (10 * 60)        /* after a failure           */
#define REQUEST_TIMEOUT  "15"             /* seconds, curl --max-time  */

/* Persisted state. Kept OUT of config.xml: that file mirrors the macOS
 * <GUIConfig> schema, and none of this is user-facing settings. */
static char   *s_latest;        /* last known release version   */
static char   *s_etag;          /* for If-None-Match            */
static char   *s_seen;          /* version dismissed with Close */
static gint64  s_last_check;    /* unix seconds, 0 = never      */

/* Session-only. */
static char   *s_failure;
static gint64  s_last_attempt;
static gboolean s_in_flight;

static gchar *state_path(void) { return npp_user_file(NULL, "updatecheck.ini"); }

void updatecheck_init(void)
{
    gchar *path = state_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        s_latest     = g_key_file_get_string(kf, "state", "latest", NULL);
        s_etag       = g_key_file_get_string(kf, "state", "etag",   NULL);
        s_seen       = g_key_file_get_string(kf, "state", "seen",   NULL);
        s_last_check = g_key_file_get_int64 (kf, "state", "last_check", NULL);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void state_save(void)
{
    GKeyFile *kf = g_key_file_new();
    if (s_latest) g_key_file_set_string(kf, "state", "latest", s_latest);
    if (s_etag)   g_key_file_set_string(kf, "state", "etag",   s_etag);
    if (s_seen)   g_key_file_set_string(kf, "state", "seen",   s_seen);
    g_key_file_set_int64(kf, "state", "last_check", s_last_check);
    gchar *path = state_path();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

/* ── Pure helpers ──────────────────────────────────────────────────── */

/* Numeric compare, so 1.10.0 sorts above 1.9.0 (macOS NSNumericSearch). */
static int version_cmp(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    sscanf(a ? a : "", "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b ? b : "", "%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) return a1 - b1;
    if (a2 != b2) return a2 - b2;
    return a3 - b3;
}

static char *json_string_value(const char *json, const char *key)
{
    if (!json) return NULL;
    char *pat = g_strdup_printf("\"%s\"", key);
    const char *p = strstr(json, pat);
    g_free(pat);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    while (*p && *p != '"') p++;
    if (!*p) return NULL;
    const char *start = ++p;
    while (*p && *p != '"') p++;
    return g_strndup(start, (gsize)(p - start));
}

/* Case-insensitive header lookup over a curl -D dump. Caller frees. */
static char *header_value(const char *headers, const char *name)
{
    if (!headers || !name) return NULL;
    gchar **lines = g_strsplit(headers, "\n", -1);
    char *found = NULL;
    for (int i = 0; lines[i] && !found; i++) {
        const char *colon = strchr(lines[i], ':');
        if (!colon) continue;
        gchar *k = g_strstrip(g_strndup(lines[i], (gsize)(colon - lines[i])));
        if (g_ascii_strcasecmp(k, name) == 0)
            found = g_strstrip(g_strdup(colon + 1));
        g_free(k);
    }
    g_strfreev(lines);
    return found;
}

/* macOS messageForHTTPStatus: — the reason, not a guess. */
static char *message_for_status(long status, const char *headers)
{
    /* GitHub answers an exhausted rate limit with 403 (or 429) and a
     * zeroed x-ratelimit-remaining. This is the common real-world
     * failure: the ceiling is per IP, so office/VPN/CGNAT users share
     * it with everyone else on their network. */
    if (status == 403 || status == 429) {
        char *remaining = header_value(headers, "x-ratelimit-remaining");
        gboolean exhausted = remaining && atoi(remaining) == 0;
        g_free(remaining);
        if (exhausted) {
            char *reset = header_value(headers, "x-ratelimit-reset");
            gint64 when = reset ? g_ascii_strtoll(reset, NULL, 10) : 0;
            g_free(reset);
            if (when > 0) {
                GDateTime *dt = g_date_time_new_from_unix_local(when);
                char *hhmm = g_date_time_format(dt, "%X");
                char *msg = g_strdup_printf("%s %s",
                    i18n_translate("Too many update checks from your network "
                                   "(this limit is shared by everyone on it). "
                                   "Try again after"), hhmm);
                g_free(hhmm);
                g_date_time_unref(dt);
                return msg;
            }
            return g_strdup(i18n_translate(
                "Too many update checks from your network (this limit is "
                "shared by everyone on it). Please try again later."));
        }
        return g_strdup(i18n_translate(
            "The update server refused the request. A proxy, firewall or "
            "VPN may be blocking access to GitHub."));
    }
    if (status == 404)
        return g_strdup(i18n_translate("No published release was found."));
    if (status >= 500)
        return g_strdup(i18n_translate("The update server is temporarily "
                                       "unavailable. Please try again later."));
    return g_strdup_printf("%s %ld.",
        i18n_translate("The update server returned status"), status);
}

/* ── State queries ─────────────────────────────────────────────────── */

const char *updatecheck_current_version(void) { return APP_VERSION; }
const char *updatecheck_latest_version(void)  { return s_latest; }
const char *updatecheck_failure_message(void) { return s_failure; }
gint64      updatecheck_last_check(void)      { return s_last_check; }
const char *updatecheck_download_url(void)    { return kDownloadPage; }

NppUpdateStatus updatecheck_status(void)
{
    if (s_failure && *s_failure) return NPP_UPDATE_FAILED;
    if (s_latest && *s_latest &&
        version_cmp(s_latest, APP_VERSION) > 0) return NPP_UPDATE_AVAILABLE;
    if (s_last_check > 0) return NPP_UPDATE_UP_TO_DATE;
    return NPP_UPDATE_UNKNOWN;
}

gboolean updatecheck_is_due(void)
{
    if (!g_prefs.auto_check_updates) return FALSE;
    if (s_in_flight) return FALSE;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (s_last_attempt && now - s_last_attempt < RETRY_FLOOR) return FALSE;
    if (s_last_check <= 0) return TRUE;
    return now - s_last_check >= CHECK_INTERVAL;
}

gboolean updatecheck_should_present_card(void)
{
    if (!g_prefs.auto_check_updates) return FALSE;
    if (updatecheck_status() != NPP_UPDATE_AVAILABLE) return FALSE;
    return !(s_seen && *s_seen && s_latest && !strcmp(s_seen, s_latest));
}

void updatecheck_mark_seen(void)
{
    if (!s_latest || !*s_latest) return;
    g_free(s_seen);
    s_seen = g_strdup(s_latest);
    state_save();
}

void updatecheck_disable_auto(void)
{
    g_prefs.auto_check_updates = FALSE;
    prefs_save();
}

/* ── The check ─────────────────────────────────────────────────────── */

typedef struct {
    void (*done)(gpointer);
    gpointer user_data;
    char *body_path;
    char *hdr_path;
} CheckCtx;

static void finish(CheckCtx *ctx)
{
    s_in_flight = FALSE;
    if (ctx->body_path) { g_unlink(ctx->body_path); g_free(ctx->body_path); }
    if (ctx->hdr_path)  { g_unlink(ctx->hdr_path);  g_free(ctx->hdr_path);  }
    if (ctx->done) ctx->done(ctx->user_data);
    g_free(ctx);
}

static void set_failure(const char *msg)
{
    g_free(s_failure);
    s_failure = g_strdup(msg);
}

static void on_curl_done(GObject *src, GAsyncResult *res, gpointer u)
{
    CheckCtx *ctx = u;
    char *out = NULL;
    GError *err = NULL;

    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res,
                                              &out, NULL, &err)) {
        set_failure(err && err->message ? err->message
                    : i18n_translate("No internet connection."));
        if (err) g_error_free(err);
        g_free(out);
        finish(ctx);
        return;
    }

    /* -w appended the status code after the body. Split it back off. */
    long status = 0;
    char *body = NULL;
    if (out) {
        const char *nl = strrchr(out, '\n');
        if (nl) {
            status = strtol(nl + 1, NULL, 10);
            body   = g_strndup(out, (gsize)(nl - out));
        } else {
            status = strtol(out, NULL, 10);
            body   = g_strdup("");
        }
    }

    /* curl exiting non-zero with no status = it never got a response. */
    if (status == 0) {
        set_failure(i18n_translate("No internet connection."));
        g_free(body); g_free(out);
        finish(ctx);
        return;
    }

    char *headers = NULL;
    if (ctx->hdr_path)
        g_file_get_contents(ctx->hdr_path, &headers, NULL, NULL);

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    if (status == 304) {
        /* Nothing changed since the cached ETag — a success, and free of
         * rate-limit quota. Keep the cached version, refresh the clock. */
        set_failure(NULL);
        s_last_check = now;
        state_save();
    } else if (status != 200) {
        char *msg = message_for_status(status, headers);
        set_failure(msg);
        g_free(msg);
    } else {
        char *tag = json_string_value(body, "tag_name");
        if (!tag || !*tag) {
            set_failure(i18n_translate("No published release was found."));
        } else {
            const char *v = (tag[0] == 'v') ? tag + 1 : tag;
            set_failure(NULL);
            g_free(s_latest);
            s_latest = g_strdup(v);
            s_last_check = now;
            char *etag = header_value(headers, "ETag");
            if (etag) { g_free(s_etag); s_etag = etag; }
            state_save();
        }
        g_free(tag);
    }

    g_free(headers);
    g_free(body);
    g_free(out);
    finish(ctx);
}

/* QA short-circuit — see updatecheck.h. Returns TRUE when simulating. */
static gboolean simulate(CheckCtx *ctx)
{
    const char *sim_latest = g_getenv("NPP_UPDATE_SIMULATE_LATEST");
    const char *sim_status = g_getenv("NPP_UPDATE_SIMULATE_STATUS");
    long status = sim_status ? strtol(sim_status, NULL, 10) : 0;
    if ((!sim_latest || !*sim_latest) && status <= 0) return FALSE;

    /* Loudly on the record: if a user ever reports a phantom update,
     * this line in the terminal explains it immediately. */
    g_message("[updatecheck] SIMULATION ACTIVE — latest=%s status=%ld "
              "(clear with: unset NPP_UPDATE_SIMULATE_LATEST "
              "NPP_UPDATE_SIMULATE_STATUS)",
              sim_latest && *sim_latest ? sim_latest : "(none)", status);

    if (status > 0 && status != 200) {
        char *msg = message_for_status(status, NULL);
        set_failure(msg);
        g_free(msg);
    } else {
        set_failure(NULL);
        g_free(s_latest);
        s_latest = g_strdup(sim_latest ? sim_latest : "");
        s_last_check = g_get_real_time() / G_USEC_PER_SEC;
        state_save();
    }
    finish(ctx);
    return TRUE;
}

void updatecheck_run(void (*done)(gpointer), gpointer user_data)
{
    if (s_in_flight) { if (done) done(user_data); return; }
    s_in_flight   = TRUE;
    s_last_attempt = g_get_real_time() / G_USEC_PER_SEC;

    CheckCtx *ctx = g_new0(CheckCtx, 1);
    ctx->done = done;
    ctx->user_data = user_data;

    if (simulate(ctx)) return;

    ctx->hdr_path  = g_build_filename(g_get_user_runtime_dir(),
                                      "npp-update-hdr.txt", NULL);

    char *ua = g_strdup_printf("Nextpad++/%s (Linux)", APP_VERSION);
    char *inm = s_etag && *s_etag
                ? g_strdup_printf("If-None-Match: %s", s_etag) : NULL;

    /* NOTE: no -f. It would discard the body AND hide the status code,
     * which is the whole reason failures were indistinguishable. */
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, (gpointer)"curl");
    g_ptr_array_add(a, (gpointer)"-sS");
    g_ptr_array_add(a, (gpointer)"--max-time");
    g_ptr_array_add(a, (gpointer)REQUEST_TIMEOUT);
    g_ptr_array_add(a, (gpointer)"-w");
    g_ptr_array_add(a, (gpointer)"\n%{http_code}");
    g_ptr_array_add(a, (gpointer)"-D");
    g_ptr_array_add(a, (gpointer)ctx->hdr_path);
    g_ptr_array_add(a, (gpointer)"-A");
    g_ptr_array_add(a, (gpointer)ua);
    g_ptr_array_add(a, (gpointer)"-H");
    g_ptr_array_add(a, (gpointer)"Accept: application/vnd.github+json");
    if (inm) {
        g_ptr_array_add(a, (gpointer)"-H");
        g_ptr_array_add(a, (gpointer)inm);
    }
    g_ptr_array_add(a, (gpointer)kReleasesAPI);
    g_ptr_array_add(a, NULL);

    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv((const gchar *const *)a->pdata,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &err);
    g_ptr_array_free(a, TRUE);
    g_free(ua);
    g_free(inm);

    if (!proc) {
        set_failure(err && err->message ? err->message
                    : i18n_translate("The 'curl' command is required to "
                                     "check for updates."));
        if (err) g_error_free(err);
        finish(ctx);
        return;
    }
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_curl_done, ctx);
    g_object_unref(proc);
}

/* ── Menu tooltip ──────────────────────────────────────────────────── */

char *updatecheck_menu_tooltip(void)
{
    GString *s = g_string_new(NULL);

    switch (updatecheck_status()) {
    case NPP_UPDATE_AVAILABLE:
        g_string_append_printf(s, "Nextpad++ v%s %s",
            s_latest ? s_latest : "", i18n_translate("is available"));
        break;
    case NPP_UPDATE_UP_TO_DATE:
        g_string_append_printf(s, "Nextpad++ %s %s", APP_VERSION,
            i18n_translate("is the latest version."));
        break;
    case NPP_UPDATE_FAILED:
        g_string_append(s, s_failure ? s_failure
                        : i18n_translate("Unable to Check for Updates"));
        break;
    case NPP_UPDATE_UNKNOWN:
        if (!g_prefs.auto_check_updates)
            g_string_append(s, i18n_translate("Automatic update checks are off."));
        break;
    }

    if (s_last_check > 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(s_last_check);
        char *when = g_date_time_format(dt, "%x %X");
        if (s->len) g_string_append_c(s, '\n');
        g_string_append_printf(s, "%s %s",
                               i18n_translate("Last checked:"), when);
        g_free(when);
        g_date_time_unref(dt);
    }
    return g_string_free(s, FALSE);
}
