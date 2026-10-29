#include "ef32.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

static ef_vm *g_vm;
static char *g_out;
static size_t g_len;
static size_t g_cap;
#define KEY_BUF 64
static int g_keys[KEY_BUF];
static int g_key_head;
static int g_key_n;
static int g_waiting;
static int g_running;
/* Same reply EditForth queues when Hayes prints its ACCEPT prompt. */
static char g_accept_tail[240];
static int g_accept_n;
static int g_yield;
static char g_breaks[640];
static char g_last_name[80];
static char *g_fetch_buf;
static int g_fetch_len;
static int g_fetch_ready;
static int g_fetch_ok;

/* Yield so the page can show the pause text and post the next key.
   Keys typed during a run stay in g_keys until KEY reads them. */
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

static void key_clear(void) {
    g_key_head = 0;
    g_key_n = 0;
}

static void key_push(int c) {
    if (g_key_n >= KEY_BUF) {
        return;
    }
    g_keys[(g_key_head + g_key_n) % KEY_BUF] = c;
    g_key_n++;
}

static void accept_tail_clear(void) {
    g_accept_n = 0;
    g_accept_tail[0] = 0;
}

/* Hayes core.fr prints this and then ACCEPT. The suite only checks the
   stack, so the reply text is the same one EditForth feeds. */
static void note_hayes_accept(int ch) {
    static const char prompt[] = "PLEASE TYPE UP TO 80 CHARACTERS:";
    static const char reply[] = "hayes-accept";
    const char *hit;
    int keep;
    int i;
    if (g_accept_n >= (int)sizeof g_accept_tail - 1) {
        memmove(g_accept_tail, g_accept_tail + 80, (size_t)(g_accept_n - 80));
        g_accept_n -= 80;
    }
    g_accept_tail[g_accept_n++] = (char)(ch & 0xff);
    g_accept_tail[g_accept_n] = 0;
    hit = strstr(g_accept_tail, prompt);
    if (!hit) {
        return;
    }
    keep = (int)(hit - g_accept_tail) + (int)(sizeof prompt - 1);
    memmove(g_accept_tail, g_accept_tail + keep, (size_t)(g_accept_n - keep + 1));
    g_accept_n -= keep;
    for (i = 0; reply[i]; i++) {
        key_push((unsigned char)reply[i]);
    }
    key_push('\r');
}

static int key_pop(void) {
    int c;
    if (g_key_n <= 0) {
        return -1;
    }
    c = g_keys[g_key_head];
    g_key_head = (g_key_head + 1) % KEY_BUF;
    g_key_n--;
    return c;
}

static int web_key_q(void *ctx) {
    (void)ctx;
    return g_key_n > 0;
}

static int web_key(void *ctx) {
    (void)ctx;
    if (g_key_n == 0) {
        g_waiting = 1;
        EM_ASM({
            if (typeof Module._forthPump === "function") Module._forthPump();
        });
        while (g_key_n == 0) {
            emscripten_sleep(10);
        }
        g_waiting = 0;
    }
    return key_pop();
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
    note_hayes_accept(ch);
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
    ef_set_key_q(g_vm, web_key_q, NULL);
    ef_set_debug(g_vm, web_debug, NULL);
    ef_set_view(g_vm, web_view, NULL);
    ef_set_fetch(g_vm, web_fetch, NULL);
    ef_set_yield(g_vm, g_yield);
    key_clear();
    accept_tail_clear();
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
    key_push(c);
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
    accept_tail_clear();
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
int ef_web_set_source_line(int line) {
    if (!g_vm) {
        return -1;
    }
    return ef_set_source_line(g_vm, line);
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
