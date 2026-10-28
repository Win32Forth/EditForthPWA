#include "ef32.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

static ef_vm *g_vm;
static char *g_out;
static size_t g_len;
static size_t g_cap;

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
    return ef_boot(g_vm);
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
