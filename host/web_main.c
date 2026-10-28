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

/* Yield so the page can show the pause text and post the next key.
   A key typed while nothing is waiting is ignored, so a slow eval
   cannot arm the next DEBUG by accident. */
static void web_debug(void *ctx, const char *path, int line, const char *name, int off, int len) {
    (void)ctx;
    EM_ASM({
        if (typeof Module._forthDebug === "function") {
            Module._forthDebug(Module.UTF8ToString($0), $1, Module.UTF8ToString($2), $3, $4);
        }
    }, path ? path : "", line, name ? name : "", off, len);
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
    g_has_key = 0;
    g_waiting = 0;
    return ef_boot(g_vm);
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
int ef_web_eval(const char *text) {
    out_clear();
    if (!g_vm) {
        return -1;
    }
    return ef_eval(g_vm, text ? text : "");
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
