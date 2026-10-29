#include "ef32.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

static ef_vm *g_vm;
static char *g_out;
static size_t g_len;
static size_t g_cap;
static int g_posted;
static int g_has_key;
static int g_waiting;
static int g_running;
static int g_yield;
static char g_breaks[640];
static char g_last_name[80];
static char *g_fetch_buf;
static int g_fetch_len;
static int g_fetch_ready;
static int g_fetch_ok;

/* Yield so the page can show the pause text and post the next key.
   A key typed while nothing is waiting is ignored, so a slow eval
   cannot arm the next DEBUG by accident. */
static void web_view(void *ctx, const char *path, int line) {
    (void)ctx;
    EM_ASM({
        if (typeof Module._forthView === "function") {
            Module._forthView(UTF8ToString($0), $1);
        }
    }, path ? path : "", line);
}

static void web_debug(void *ctx, const char *path, int line, const char *name, int off, int len) {
    (void)ctx;
    EM_ASM({
        if (typeof Module._forthDebug === "function") {
            Module._forthDebug(Module.UTF8ToString($0), $1, Module.UTF8ToString($2), $3, $4);
        }
    }, path ? path : "", line, name ? name : "", off, len);
}

/* Ask the page for a file that is not already mounted. The page calls
   ef_web_fetch_done before this returns, or later after emscripten_sleep. */
static int web_fetch(void *ctx, const char *path, char **owned, int *len) {
    (void)ctx;
    free(g_fetch_buf);
    g_fetch_buf = NULL;
    g_fetch_len = 0;
    g_fetch_ok = 0;
    g_fetch_ready = 0;
    EM_ASM({
        var path = UTF8ToString($0);
        if (typeof Module._forthFetch === "function") {
            Module._forthFetch(path);
        } else {
            Module._ef_web_fetch_done(0, "");
        }
    }, path ? path : "");
    while (!g_fetch_ready) {
        emscripten_sleep(10);
    }
    if (!g_fetch_ok || !g_fetch_buf) {
        free(g_fetch_buf);
        g_fetch_buf = NULL;
        return -1;
    }
    *owned = g_fetch_buf;
    *len = g_fetch_len;
    g_fetch_buf = NULL;
    g_fetch_len = 0;
    return 0;
}

static int web_key(void *ctx) {
    (void)ctx;
    g_has_key = 0;
    g_waiting = 1;
    EM_ASM({
        if (typeof Module._forthPump === "function") Module._forthPump();
    });
    while (!g_has_key) {
        emscripten_sleep(10);
    }
    g_waiting = 0;
    g_has_key = 0;
    return g_posted;
}

static void out_clear(void) {
    g_len = 0;
    if (g_out) {
        g_out[0] = 0;
    }
}

static void web_emit(void *ctx, int ch) {
    (void)ctx;
    if (g_len + 2 > g_cap) {
        size_t ncap = g_cap ? g_cap * 2 : 256;
        char *nbuf = realloc(g_out, ncap);
        if (!nbuf) {
            return;
        }
        g_out = nbuf;
        g_cap = ncap;
    }
    g_out[g_len++] = (char)ch;
    g_out[g_len] = 0;
}

EMSCRIPTEN_KEEPALIVE
const char *ef_web_boot_source(void) {
    return ef_boot_source();
}

EMSCRIPTEN_KEEPALIVE
int ef_web_boot(void) {
    if (g_vm) {
        ef_destroy(g_vm);
        g_vm = NULL;
    }
    out_clear();
    g_vm = ef_create();
    if (!g_vm) {
        return -8;
    }
    ef_set_emit(g_vm, web_emit, NULL);
    ef_set_key(g_vm, web_key, NULL);
    ef_set_debug(g_vm, web_debug, NULL);
    ef_set_view(g_vm, web_view, NULL);
    ef_set_fetch(g_vm, web_fetch, NULL);
    ef_set_yield(g_vm, g_yield);
    g_has_key = 0;
    g_waiting = 0;
    g_running = 1;
    {
        int rc = ef_boot(g_vm);
        g_running = 0;
        return rc;
    }
}

EMSCRIPTEN_KEEPALIVE
void ef_web_post_key(int c) {
    if (!g_waiting) {
        return;
    }
    g_posted = c;
    g_has_key = 1;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_key_waiting(void) {
    return g_waiting;
}

EMSCRIPTEN_KEEPALIVE
void ef_web_fetch_done(int ok, const char *text) {
    int n;
    free(g_fetch_buf);
    g_fetch_buf = NULL;
    g_fetch_len = 0;
    g_fetch_ok = 0;
    if (ok && text) {
        n = (int)strlen(text);
        if (n >= 0 && n < 256 * 1024) {
            g_fetch_buf = malloc((size_t)n + 1);
            if (g_fetch_buf) {
                memcpy(g_fetch_buf, text, (size_t)n + 1);
                g_fetch_len = n;
                g_fetch_ok = 1;
            }
        }
    }
    g_fetch_ready = 1;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_eval(const char *text) {
    int rc;
    out_clear();
    if (!g_vm) {
        return -1;
    }
    g_running = 1;
    rc = ef_eval(g_vm, text ? text : "");
    g_running = 0;
    return rc;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_running(void) {
    return g_running;
}

EMSCRIPTEN_KEEPALIVE
void ef_web_set_yield(int on) {
    g_yield = on ? 1 : 0;
    if (g_vm) {
        ef_set_yield(g_vm, g_yield);
    }
}

EMSCRIPTEN_KEEPALIVE
int ef_web_break_toggle(const char *name) {
    return g_vm ? ef_break_toggle(g_vm, name ? name : "") : 0;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_break_clear(const char *name) {
    return g_vm ? ef_break_clear(g_vm, name ? name : "") : 0;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_break_enable(const char *name, int on) {
    return g_vm ? ef_break_set_enabled(g_vm, name ? name : "", on) : 0;
}

EMSCRIPTEN_KEEPALIVE
const char *ef_web_breaks(void) {
    g_breaks[0] = 0;
    if (g_vm) {
        ef_break_list(g_vm, g_breaks, (int)sizeof g_breaks);
    }
    return g_breaks;
}

EMSCRIPTEN_KEEPALIVE
void ef_web_bp_go(void) {
    if (g_vm) {
        ef_debug_bp_go(g_vm);
    }
}

/* 0 set, 1 nothing running, 2 already paused (no-op). */
EMSCRIPTEN_KEEPALIVE
int ef_web_break_asap(void) {
    if (!g_vm) {
        return 1;
    }
    if (g_waiting) {
        return 2;
    }
    if (!g_running && !ef_debug_armed(g_vm)) {
        return 1;
    }
    ef_debug_request_break(g_vm);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
const char *ef_web_last_name(void) {
    g_last_name[0] = 0;
    if (g_vm) {
        ef_last_name(g_vm, g_last_name, (int)sizeof g_last_name);
    }
    return g_last_name;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_mount_clear(void) {
    return g_vm ? ef_mount_clear(g_vm) : -1;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_mount(const char *path, const char *text) {
    if (!g_vm) {
        return -1;
    }
    return ef_mount_file(g_vm, path, text ? text : "");
}

EMSCRIPTEN_KEEPALIVE
int ef_web_set_source(const char *name) {
    if (!g_vm) {
        return -1;
    }
    return ef_set_source(g_vm, name ? name : "");
}

EMSCRIPTEN_KEEPALIVE
const char *ef_web_output(void) {
    return g_out ? g_out : "";
}

EMSCRIPTEN_KEEPALIVE
int ef_web_depth(void) {
    return g_vm ? ef_depth(g_vm) : 0;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_item(int from_bottom) {
    return g_vm ? (int)ef_stack_item(g_vm, from_bottom) : 0;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_state(void) {
    return g_vm ? ef_state(g_vm) : 0;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_bye(void) {
    return g_vm ? ef_bye_requested(g_vm) : 0;
}

EMSCRIPTEN_KEEPALIVE
int ef_web_error_line(void) {
    return g_vm ? ef_error_line(g_vm) : 0;
}

EMSCRIPTEN_KEEPALIVE
const char *ef_web_error_name(void) {
    return g_vm ? ef_error_name(g_vm) : "";
}

int main(void) {
    return 0;
}
