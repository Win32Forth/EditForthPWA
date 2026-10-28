#include "ef32.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define CELL 4
#define EF_STACK 512
#define EF_RSTACK 512
#define EF_THREADS 16
#define EF_ORDER_MAX 8
#define EF_CF 64
#define DBG_FRAME_MAX 64
#define EF_SRC_MAX (256 * 1024)
#define EF_WORD_MAX 128
#define EF_IMAGE0 (1024 * 1024)
#define EF_IMAGE_MAX (32 * 1024 * 1024)

#define EF_DOCOL  (-1)
#define EF_DOVAR  (-2)
#define EF_DOCON  (-3)
#define EF_DODOES (-4)

#define EF_OK 0
#define EF_THROW 1

/* User area, source, WORD, PARSE, then a fixed PAD (pictured output),
   a view-path scratch, the WORDLISTS table, the block buffer, then FORTH heads.
   PAD is not allotted: <# writes below PAD+256 and HERE must not move.
   ALLOCATE grows up from HEAP_ORIGIN so INCLUDE can rewind HERE. */
#define UV_DP 0
#define UV_BASE 4
#define UV_STATE 8
#define UV_LAST 12
#define UV_CURRENT 16
#define UV_ORDERN 20
#define UV_ORDER 24
#define UV_IN 56
#define UV_WARN 60
#define UV_BLOCK_FILE 64
#define UV_BLOCK_UPD 68
#define UV_BLOCK_NR 72
#define UV_SCR 76
#define SRC_ADDR 256
#define WORD_ADDR (SRC_ADDR + EF_SRC_MAX)
#define EF_PARSE_MAX 1024
#define PARSE_ADDR (WORD_ADDR + EF_WORD_MAX)
#define EF_PAD 1024
#define EF_VIEWPATH 256
#define EF_WL 32
#define EF_BLOCK 1024
#define PAD_ADDR (((PARSE_ADDR + EF_PARSE_MAX) + 3) & ~3)
#define VIEWPATH_ADDR (PAD_ADDR + EF_PAD)
#define WL_ADDR (VIEWPATH_ADDR + EF_VIEWPATH)
#define BLOCK_BUF_ADDR (((WL_ADDR + EF_WL * CELL) + 3) & ~3)
#define FORTH_WID (((BLOCK_BUF_ADDR + EF_BLOCK) + 3) & ~3)
#define DICT_ORIGIN (FORTH_WID + EF_THREADS * CELL)
#define HEAP_ORIGIN (2 * 1024 * 1024)
#define EF_OPEN 8
#define EF_CWD 8
#define EF_REQ 64
#define EF_VIEWS 48
#define EF_LOCAL_MAX 32
#define EF_LOCAL_FRAMES 16

#define FFA1_IMM 0x80000000u

struct ef_vm {
    uint8_t *base;
    size_t count;

    int32_t stack[EF_STACK];
    int sp;
    int32_t rstack[EF_RSTACK];
    int rsp;
    int32_t ip;
    int in_inner;
    int throw_code;
    int bye;
    int line;
    int file_id;
    int next_file;
    int src_len;
    int defining;
    int32_t prev_last;
    int32_t define_origin;
    int32_t forth_wid;

    int32_t cf[EF_CF];
    int cf_n;
    int32_t begin_s[EF_CF];
    int begin_n;
    int32_t loop_s[EF_CF];
    int loop_n;

    int32_t xt_exit, xt_lit, xt_branch, xt_zbranch;
    int32_t xt_ef_branch, xt_ef_zbranch;
    int32_t xt_do, xt_qdo, xt_loop, xt_ploop, xt_unloop;
    int32_t xt_dotq, xt_squote, xt_dotparen, xt_does;
    int32_t xt_store;
    int32_t xt_linit, xt_lat, xt_lstore;

    int32_t heap_at;
    int32_t heap_last;
    int32_t wids[EF_WL];
    int nwid;
    char last_open[512];
    char cwd_stack[EF_CWD][512];
    int cwd_n;
    char *req[EF_REQ];
    int nreq;
    struct {
        int used;
        char *data;
        int len;
        int cap;
        int pos;
        char path[512];
    } files[EF_OPEN];
    struct {
        int id;
        char path[256];
    } views[EF_VIEWS];
    int nviews;

    ef_emit_fn emit;
    ef_key_fn key;
    ef_debug_fn debug_fn;
    void *emit_ctx;
    void *key_ctx;
    void *debug_ctx;

    struct ef_mounted *mounted;
    char include_dir[512];
    /* Empty means the process directory (native) or the mount root (page).
       Never absolute: INCLUDE joins this onto a relative name. */
    char logical_cwd[512];
    char src_name[256];
    char err_name[256];
    int err_line;
    int eval_depth;
    char doc[256];
    int doc_len;

    int32_t local_frame[EF_LOCAL_FRAMES][EF_LOCAL_MAX];
    int frame_rsp[EF_LOCAL_FRAMES];
    int frame_nloc[EF_LOCAL_FRAMES];
    int frame_n;
    char local_name[EF_LOCAL_MAX][32];
    int local_len[EF_LOCAL_MAX];
    int local_n;
    int local_init_n;
    int local_reverse;
    int local_declaring;

    /* rsp is a count that grows. ARM's RSP is a descending pointer, so every
       floor / over / out compare here is the other way around. over and out
       use -1 as unset: 0 is a real return-stack depth. */
    int dbg_armed;
    int dbg_busy;
    int dbg_abort;
    int dbg_bp_go;
    int dbg_break_asap;
    int dbg_floor_on;
    int dbg_fromlib;
    int dbg_need_intro;
    int dbg_need_stacks;
    int dbg_skip_nl;
    int dbg_midline;
    int dbg_help_shown;
    int dbg_line_col;
    int dbg_frame_n;
    int32_t dbg_floor;
    int32_t dbg_over;
    int32_t dbg_out;
    int32_t dbg_runto_ip;
    int32_t dbg_runto_off;
    int32_t dbg_runto_status;
    int32_t dbg_break_min;
    int32_t dbg_pause_rsp;
    int32_t dbg_xt;
    int32_t dbg_inline;
    int32_t dbg_ip;
    int32_t dbg_cfa;
    int32_t dbg_body;
    int32_t dbg_host_off;
    int32_t dbg_host_len;
    int32_t dbg_fr_rsp[DBG_FRAME_MAX];
    int32_t dbg_fr_cfa[DBG_FRAME_MAX];
    int32_t xt_dbg_off;
    int32_t xt_execute;
    int32_t xt_catch;
};

typedef struct ef_mounted {
    char *path;
    char *text;
    struct ef_mounted *next;
} ef_mounted;

typedef int (*ef_prim_fn)(ef_vm *vm);

static ef_prim_fn g_prims[512];
static int g_prim_count;

static void hook_set(ef_vm *vm, const char *name, int32_t v);
static int32_t hook_get(ef_vm *vm, const char *name, int32_t fallback);

/* While armed, a non-newline leaves DBG-MIDLINE set so the next pause
   starts on its own line. The Mac emitter does this on every byte. */
static void emit_ch(ef_vm *vm, int ch) {
    if (vm->emit) {
        vm->emit(vm->emit_ctx, ch);
    }
    if (!vm->dbg_armed) {
        return;
    }
    /* Forth may have stored these since the last byte. */
    vm->dbg_line_col = hook_get(vm, "DBG-LINE-COL", vm->dbg_line_col);
    vm->dbg_midline = hook_get(vm, "DBG-MIDLINE", vm->dbg_midline);
    if (ch == '\n') {
        vm->dbg_line_col = 0;
        vm->dbg_midline = 0;
    } else if (ch == '\b') {
        if (vm->dbg_line_col > 0) {
            vm->dbg_line_col--;
        }
        vm->dbg_midline = vm->dbg_line_col ? 1 : 0;
    } else if ((ch & 0xC0) != 0x80) {
        vm->dbg_line_col++;
        vm->dbg_midline = 1;
    } else {
        return;
    }
    hook_set(vm, "DBG-LINE-COL", vm->dbg_line_col);
    hook_set(vm, "DBG-MIDLINE", vm->dbg_midline);
}

static void emit_str(ef_vm *vm, const char *s) {
    while (*s) {
        emit_ch(vm, (unsigned char)*s++);
    }
}

static int fail(ef_vm *vm, int code, const char *msg) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    vm->throw_code = code;
    if (msg) {
        emit_str(vm, msg);
    }
    return EF_THROW;
}

static int32_t uv(ef_vm *vm, int addr) {
    int32_t v;
    memcpy(&v, vm->base + addr, CELL);
    return v;
}

static void uv_set(ef_vm *vm, int addr, int32_t v) {
    memcpy(vm->base + addr, &v, CELL);
}

static int ensure(ef_vm *vm, int32_t end) {
    size_t need;
    uint8_t *grown;
    if (end < 0) {
        return fail(vm, -16, "bad address\n");
    }
    need = (size_t)end;
    while (need > vm->count) {
        size_t n = vm->count ? vm->count * 2 : (size_t)EF_IMAGE0;
        if (n < need) {
            n = need;
        }
        if (n > (size_t)EF_IMAGE_MAX) {
            return fail(vm, -8, "dictionary full\n");
        }
        grown = realloc(vm->base, n);
        if (!grown) {
            return fail(vm, -8, "dictionary full\n");
        }
        memset(grown + vm->count, 0, n - vm->count);
        vm->base = grown;
        vm->count = n;
    }
    return EF_OK;
}

static int load_cell(ef_vm *vm, int32_t addr, int32_t *out) {
    if (addr < 0 || (addr & 3) || (size_t)addr + CELL > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    memcpy(out, vm->base + addr, CELL);
    return EF_OK;
}

static int store_cell(ef_vm *vm, int32_t addr, int32_t value) {
    if (ensure(vm, addr + CELL)) {
        return EF_THROW;
    }
    if (addr < 0 || (addr & 3)) {
        return fail(vm, -16, "bad address\n");
    }
    memcpy(vm->base + addr, &value, CELL);
    return EF_OK;
}

static int push(ef_vm *vm, int32_t v) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    if (vm->sp >= EF_STACK) {
        return fail(vm, -4, "stack overflow\n");
    }
    vm->stack[vm->sp++] = v;
    return EF_OK;
}

static int pop(ef_vm *vm, int32_t *out) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    if (vm->sp <= 0) {
        return fail(vm, -3, "stack underflow\n");
    }
    *out = vm->stack[--vm->sp];
    return EF_OK;
}

static int rpush(ef_vm *vm, int32_t v) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    if (vm->rsp >= EF_RSTACK) {
        return fail(vm, -6, "return stack overflow\n");
    }
    vm->rstack[vm->rsp++] = v;
    return EF_OK;
}

static int rpop(ef_vm *vm, int32_t *out) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    if (vm->rsp <= 0) {
        return fail(vm, -5, "return stack underflow\n");
    }
    *out = vm->rstack[--vm->rsp];
    return EF_OK;
}

static int comma(ef_vm *vm, int32_t v) {
    int32_t here = uv(vm, UV_DP);
    if (store_cell(vm, here, v)) {
        return EF_THROW;
    }
    uv_set(vm, UV_DP, here + CELL);
    return EF_OK;
}

static int align_dp(ef_vm *vm) {
    int32_t here = uv(vm, UV_DP);
    int32_t aligned = (here + 3) & ~3;
    if (aligned != here) {
        if (ensure(vm, aligned)) {
            return EF_THROW;
        }
        uv_set(vm, UV_DP, aligned);
    }
    return EF_OK;
}

static unsigned hash_name(const char *s, int n) {
    unsigned h = 0;
    int i;
    for (i = 0; i < n; i++) {
        h = h * 33u + (unsigned)toupper((unsigned char)s[i]);
    }
    return h % EF_THREADS;
}

static int name_eq(const uint8_t *nfa, const char *s, int n) {
    int len = nfa[0] & 0x7f;
    int i;
    if (len != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (toupper(nfa[1 + i]) != toupper((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

static int32_t nfa_of(ef_vm *vm, int32_t cfa) {
    int32_t flags = uv(vm, cfa - 2 * CELL);
    return cfa - (flags & 0xffff);
}

static int smudged(ef_vm *vm, int32_t cfa) {
    return (vm->base[nfa_of(vm, cfa)] & 0x80) != 0;
}

static void set_smudge(ef_vm *vm, int32_t cfa, int on) {
    uint8_t *count = vm->base + nfa_of(vm, cfa);
    if (on) {
        *count = (uint8_t)(*count | 0x80);
    } else {
        *count = (uint8_t)(*count & 0x7f);
    }
}

static int32_t find_word(ef_vm *vm, const char *name, int n, int *imm) {
    int slot;
    int order_n = uv(vm, UV_ORDERN);
    if (order_n < 0) {
        order_n = 0;
    }
    if (order_n > EF_ORDER_MAX) {
        order_n = EF_ORDER_MAX;
    }
    for (slot = 0; slot < order_n; slot++) {
        int32_t wid = uv(vm, UV_ORDER + slot * CELL);
        unsigned th = hash_name(name, n);
        int32_t cfa = uv(vm, wid + (int)th * CELL);
        while (cfa) {
            if (!smudged(vm, cfa) && name_eq(vm->base + nfa_of(vm, cfa), name, n)) {
                if (imm) {
                    uint32_t f1 = (uint32_t)uv(vm, cfa - CELL);
                    *imm = (f1 & FFA1_IMM) ? 1 : 0;
                }
                return cfa;
            }
            cfa = uv(vm, cfa - 3 * CELL);
        }
    }
    return 0;
}

static void copy_name(ef_vm *vm, int32_t cfa, char *dst, int dstmax) {
    int32_t nfa;
    int n;
    if (dstmax < 1) {
        return;
    }
    dst[0] = 0;
    if (cfa < 12 || (size_t)cfa + CELL > vm->count) {
        return;
    }
    nfa = nfa_of(vm, cfa);
    if (nfa < 0 || (size_t)nfa >= vm->count) {
        return;
    }
    n = vm->base[nfa] & 0x7f;
    int i;
    if (n >= dstmax) {
        n = dstmax - 1;
    }
    for (i = 0; i < n; i++) {
        dst[i] = (char)vm->base[nfa + 1 + i];
    }
    dst[n] = 0;
}

static void local_reset(ef_vm *vm) {
    vm->local_n = 0;
    vm->local_init_n = 0;
    vm->local_reverse = 0;
    vm->local_declaring = 0;
}

static void local_add_name(ef_vm *vm, const char *s, int n) {
    int i;
    char *d;
    if (vm->local_n >= EF_LOCAL_MAX) {
        return;
    }
    if (n > 31) {
        n = 31;
    }
    if (n < 0) {
        n = 0;
    }
    d = vm->local_name[vm->local_n];
    for (i = 0; i < n; i++) {
        d[i] = (char)toupper((unsigned char)s[i]);
    }
    d[n] = 0;
    vm->local_len[vm->local_n] = n;
    vm->local_n++;
}

static int local_lookup(ef_vm *vm, const char *s, int n) {
    int i, k;
    for (i = 0; i < vm->local_n; i++) {
        if (vm->local_len[i] != n) {
            continue;
        }
        for (k = 0; k < n; k++) {
            if (vm->local_name[i][k] != (char)toupper((unsigned char)s[k])) {
                break;
            }
        }
        if (k == n) {
            return i;
        }
    }
    return -1;
}

static int local_compile_init(ef_vm *vm) {
    if (!vm->xt_lit || !vm->xt_linit) {
        return fail(vm, -16, "locals are not ready\n");
    }
    if (comma(vm, vm->xt_lit) || comma(vm, vm->local_n) ||
        comma(vm, vm->xt_lit) || comma(vm, vm->local_init_n) ||
        comma(vm, vm->xt_lit) || comma(vm, vm->local_reverse) ||
        comma(vm, vm->xt_linit)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int local_compile_op(ef_vm *vm, int idx, int32_t xt) {
    if (!vm->xt_lit || !xt) {
        return fail(vm, -16, "locals are not ready\n");
    }
    if (comma(vm, vm->xt_lit) || comma(vm, idx) || comma(vm, xt)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int32_t header(ef_vm *vm, const char *name, int n, int32_t code) {
    int32_t hfa, nfa, lfa, cfa;
    unsigned th;
    int32_t wid, prev;
    uint32_t ffa0, ffa1;
    int i, padded;
    if (n < 1 || n > 127) {
        fail(vm, -16, "bad name\n");
        return 0;
    }
    if (align_dp(vm)) {
        return 0;
    }
    hfa = uv(vm, UV_DP);
    {
        int nhelp = vm->doc_len;
        int padded;
        if (nhelp < 0) {
            nhelp = 0;
        }
        if (nhelp > 255) {
            nhelp = 255;
        }
        padded = (1 + nhelp + 3) & ~3;
        if (ensure(vm, hfa + padded)) {
            return 0;
        }
        memset(vm->base + hfa, 0, (size_t)padded);
        vm->base[hfa] = (uint8_t)nhelp;
        if (nhelp) {
            memcpy(vm->base + hfa + 1, vm->doc, (size_t)nhelp);
        }
        uv_set(vm, UV_DP, hfa + padded);
        vm->doc_len = 0;
    }
    nfa = uv(vm, UV_DP);
    padded = (1 + n + 3) & ~3;
    if (ensure(vm, nfa + padded)) {
        return 0;
    }
    vm->base[nfa] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        vm->base[nfa + 1 + i] = (uint8_t)toupper((unsigned char)name[i]);
    }
    for (i = 1 + n; i < padded; i++) {
        vm->base[nfa + i] = 0;
    }
    uv_set(vm, UV_DP, nfa + padded);
    lfa = uv(vm, UV_DP);
    cfa = lfa + 3 * CELL;
    th = hash_name(name, n);
    wid = uv(vm, UV_CURRENT);
    prev = uv(vm, wid + (int)th * CELL);
    ffa0 = (uint32_t)(cfa - nfa) | ((uint32_t)(cfa - hfa) << 16);
    ffa1 = (uint32_t)(vm->line & 0xffff) | ((uint32_t)(vm->file_id & 0x1fff) << 16);
    if (comma(vm, prev) || comma(vm, (int32_t)ffa0) || comma(vm, (int32_t)ffa1) ||
        comma(vm, code) || comma(vm, 0)) {
        return 0;
    }
    uv_set(vm, wid + (int)th * CELL, cfa);
    vm->prev_last = uv(vm, UV_LAST);
    vm->define_origin = hfa;
    uv_set(vm, UV_LAST, cfa);
    return cfa;
}

static void unlink_defining(ef_vm *vm) {
    int32_t cfa = vm->defining;
    char name[128];
    unsigned th;
    int32_t wid, head;
    if (!cfa) {
        return;
    }
    copy_name(vm, cfa, name, sizeof name);
    th = hash_name(name, (int)strlen(name));
    wid = uv(vm, UV_CURRENT);
    head = uv(vm, wid + (int)th * CELL);
    if (head == cfa) {
        uv_set(vm, wid + (int)th * CELL, uv(vm, cfa - 3 * CELL));
    }
    uv_set(vm, UV_LAST, vm->prev_last);
    uv_set(vm, UV_DP, vm->define_origin);
    vm->defining = 0;
    uv_set(vm, UV_STATE, 0);
    vm->cf_n = 0;
    vm->begin_n = 0;
    vm->loop_n = 0;
}

static void recover(ef_vm *vm) {
    unlink_defining(vm);
    vm->sp = 0;
    vm->rsp = 0;
    vm->ip = 0;
    vm->in_inner = 0;
    uv_set(vm, UV_STATE, 0);
    vm->cf_n = 0;
    vm->begin_n = 0;
    vm->loop_n = 0;
    vm->doc_len = 0;
    vm->frame_n = 0;
    local_reset(vm);
}

static int is_white(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int next_name(ef_vm *vm, char *dst, int dstmax, int *outn) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = vm->base + SRC_ADDR;
    int start, n;
    while (in < len && is_white(s[in])) {
        if (s[in] == '\n') {
            vm->line++;
        }
        in++;
    }
    if (in >= len) {
        uv_set(vm, UV_IN, in);
        return 0;
    }
    start = in;
    while (in < len && !is_white(s[in])) {
        in++;
    }
    n = in - start;
    uv_set(vm, UV_IN, in);
    if (n > 2 && dstmax > 3 && s[start] == '.' && s[start + 1] == '"') {
        uv_set(vm, UV_IN, start + 2);
        dst[0] = '.';
        dst[1] = '"';
        dst[2] = 0;
        *outn = 2;
        return 1;
    }
    if (n > 2 && dstmax > 3 && toupper(s[start]) == 'S' && s[start + 1] == '"') {
        uv_set(vm, UV_IN, start + 2);
        dst[0] = 'S';
        dst[1] = '"';
        dst[2] = 0;
        *outn = 2;
        return 1;
    }
    if (n > 2 && dstmax > 4 && s[start] == '.' && s[start + 1] == '(') {
        uv_set(vm, UV_IN, start + 2);
        dst[0] = '.';
        dst[1] = '(';
        dst[2] = 0;
        *outn = 2;
        return 1;
    }
    if (n >= dstmax) {
        n = dstmax - 1;
    }
    memcpy(dst, s + start, (size_t)n);
    dst[n] = 0;
    *outn = n;
    return 1;
}

static int parse_until(ef_vm *vm, char end, char *dst, int dstmax, int *outn) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = vm->base + SRC_ADDR;
    int n = 0;
    while (in < len && s[in] != (uint8_t)end) {
        if (s[in] == '\n') {
            vm->line++;
        }
        if (n + 1 < dstmax && s[in] != '\r') {
            dst[n++] = (char)s[in];
        }
        in++;
    }
    if (in >= len || s[in] != (uint8_t)end) {
        uv_set(vm, UV_IN, in);
        return fail(vm, -16, "missing delimiter\n");
    }
    in++;
    uv_set(vm, UV_IN, in);
    dst[n] = 0;
    *outn = n;
    return EF_OK;
}

static int skip_line(ef_vm *vm) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = vm->base + SRC_ADDR;
    while (in < len && s[in] != '\n') {
        in++;
    }
    if (in < len && s[in] == '\n') {
        in++;
        vm->line++;
    }
    uv_set(vm, UV_IN, in);
    return EF_OK;
}

static int digit_val(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    return -1;
}

static int parse_number(const char *s, int n, int base, int32_t *out) {
    int i = 0;
    int sign = 1;
    int b = base;
    int64_t acc = 0;
    int64_t limit;
    if (n >= 3 && s[0] == '\'' && s[n - 1] == '\'' && n == 3) {
        *out = (unsigned char)s[1];
        return 1;
    }
    if (n > 0 && s[0] == '#') {
        b = 10;
        i++;
    } else if (n > 0 && s[0] == '$') {
        b = 16;
        i++;
    } else if (n > 0 && s[0] == '%') {
        b = 2;
        i++;
    }
    if (b < 2 || b > 36) {
        b = 10;
    }
    if (i < n && s[i] == '-') {
        sign = -1;
        i++;
    } else if (i < n && s[i] == '+') {
        i++;
    }
    if (i >= n) {
        return 0;
    }
    for (; i < n; i++) {
        int d = digit_val((unsigned char)s[i]);
        if (d < 0 || d >= b) {
            return 0;
        }
        acc = acc * b + d;
        limit = sign < 0 ? 2147483648LL : 4294967295LL;
        if (acc > limit) {
            return 0;
        }
    }
    if (sign < 0) {
        *out = (int32_t)(-acc);
    } else {
        *out = (int32_t)(uint32_t)acc;
    }
    return 1;
}

static int plant_string(ef_vm *vm, const char *s, int n, int32_t *char_addr) {
    int32_t here, end, i;
    if (n < 0 || n > 255) {
        return fail(vm, -16, "string too long\n");
    }
    here = uv(vm, UV_DP);
    end = (here + 1 + n + 3) & ~3;
    if (ensure(vm, end)) {
        return EF_THROW;
    }
    vm->base[here] = (uint8_t)n;
    memcpy(vm->base + here + 1, s, (size_t)n);
    for (i = here + 1 + n; i < end; i++) {
        vm->base[i] = 0;
    }
    uv_set(vm, UV_DP, end);
    if (char_addr) {
        *char_addr = here + 1;
    }
    return EF_OK;
}

static int32_t skip_counted(ef_vm *vm, int32_t ip) {
    int n;
    if (ip < 0 || (size_t)ip >= vm->count) {
        return ip;
    }
    n = vm->base[ip];
    return (ip + 1 + n + 3) & ~3;
}

static void emit_uint(ef_vm *vm, uint32_t u, int base) {
    char buf[40];
    int n = 0;
    if (base < 2 || base > 36) {
        base = 10;
    }
    do {
        uint32_t d = u % (uint32_t)base;
        buf[n++] = (char)(d < 10 ? '0' + d : 'A' + (int)d - 10);
        u /= (uint32_t)base;
    } while (u && n < (int)sizeof buf);
    while (n > 0) {
        emit_ch(vm, buf[--n]);
    }
}

static void emit_int(ef_vm *vm, int32_t value, int base, int is_signed) {
    uint32_t u;
    if (is_signed && value < 0) {
        emit_ch(vm, '-');
        u = (uint32_t)(-(int64_t)value);
    } else {
        u = (uint32_t)value;
    }
    emit_uint(vm, u, base);
    emit_ch(vm, ' ');
}

static int base_of(ef_vm *vm) {
    int b = uv(vm, UV_BASE);
    if (b < 2 || b > 36) {
        return 10;
    }
    return b;
}

/* Continue the DO/+LOOP while the step has not crossed the limit.
   The step stops once it has crossed the limit. */
static int loop_continue(int32_t index, int32_t limit, int32_t delta) {
    int32_t old_diff, new_diff;
    if (delta == 0) {
        return 1;
    }
    old_diff = (int32_t)((uint32_t)index - (uint32_t)limit);
    new_diff = (int32_t)((uint32_t)old_diff + (uint32_t)delta);
    return ((old_diff ^ new_diff) & (old_diff ^ delta)) >= 0;
}

static int divide(ef_vm *vm, int32_t a, int32_t b, int32_t *quot, int32_t *rem) {
    if (b == 0) {
        return fail(vm, -9, "division by zero\n");
    }
    if (b == -1 && a == INT32_MIN) {
        *quot = a;
        *rem = 0;
        return EF_OK;
    }
    *quot = a / b;
    *rem = a % b;
    return EF_OK;
}

static int exec_one(ef_vm *vm, int32_t cfa, int from_inner);
static void dbg_note_frame(ef_vm *vm, int32_t cfa);
static int dbg_consider(ef_vm *vm, int32_t xt);

static int inner(ef_vm *vm, int stop_rsp) {
    int guard = 0;
    int prev = vm->in_inner;
    vm->in_inner = 1;
    while (vm->ip != 0 && vm->rsp >= stop_rsp) {
        int32_t xt;
        if (++guard > 50000000) {
            vm->in_inner = prev;
            return fail(vm, -1, "step limit\n");
        }
        if (load_cell(vm, vm->ip, &xt)) {
            vm->in_inner = prev;
            return EF_THROW;
        }
        if (xt == 0) {
            vm->in_inner = prev;
            return fail(vm, -16, "null xt\n");
        }
        /* Pause before the IP moves so the inline cell is still the next one. */
        if (dbg_consider(vm, xt)) {
            vm->in_inner = prev;
            return EF_THROW;
        }
        vm->ip += CELL;
        if (exec_one(vm, xt, 1)) {
            vm->in_inner = prev;
            return EF_THROW;
        }
    }
    vm->in_inner = prev;
    return EF_OK;
}

static int exec_one(ef_vm *vm, int32_t cfa, int from_inner) {
    int32_t code;
    int stop;
    int32_t saved_ip;
    if (load_cell(vm, cfa, &code)) {
        return EF_THROW;
    }
    if (code == EF_DOCOL || code == EF_DODOES) {
        if (code == EF_DODOES && push(vm, cfa + 2 * CELL)) {
            return EF_THROW;
        }
        if (from_inner) {
            if (rpush(vm, vm->ip)) {
                return EF_THROW;
            }
            dbg_note_frame(vm, cfa);
            if (code == EF_DOCOL) {
                vm->ip = cfa + 2 * CELL;
            } else if (load_cell(vm, cfa + CELL, &vm->ip)) {
                return EF_THROW;
            }
            return EF_OK;
        }
        stop = vm->rsp;
        saved_ip = vm->ip;
        if (rpush(vm, 0)) {
            return EF_THROW;
        }
        dbg_note_frame(vm, cfa);
        if (code == EF_DOCOL) {
            vm->ip = cfa + 2 * CELL;
        } else if (load_cell(vm, cfa + CELL, &vm->ip)) {
            return EF_THROW;
        }
        if (inner(vm, stop)) {
            vm->ip = saved_ip;
            return EF_THROW;
        }
        vm->ip = saved_ip;
        return EF_OK;
    }
    if (code == EF_DOVAR) {
        return push(vm, cfa + 2 * CELL);
    }
    if (code == EF_DOCON) {
        int32_t v;
        if (load_cell(vm, cfa + 2 * CELL, &v)) {
            return EF_THROW;
        }
        return push(vm, v);
    }
    if (code < 0 || code >= g_prim_count || g_prims[code] == NULL) {
        return fail(vm, -16, "bad code field\n");
    }
    return g_prims[code](vm);
}

static int compile_only(ef_vm *vm, const char *what) {
    if (uv(vm, UV_STATE) == 0) {
        emit_str(vm, what);
        return fail(vm, -14, " compile only\n");
    }
    return EF_OK;
}

static int cf_push(ef_vm *vm, int32_t v) {
    if (vm->cf_n >= EF_CF) {
        return fail(vm, -16, "control stack overflow\n");
    }
    vm->cf[vm->cf_n++] = v;
    return EF_OK;
}

static int cf_pop(ef_vm *vm, int32_t *out, const char *msg) {
    if (vm->cf_n <= 0) {
        return fail(vm, -16, msg);
    }
    *out = vm->cf[--vm->cf_n];
    return EF_OK;
}

static int begin_push(ef_vm *vm, int32_t v) {
    if (vm->begin_n >= EF_CF) {
        return fail(vm, -16, "control stack overflow\n");
    }
    vm->begin_s[vm->begin_n++] = v;
    return EF_OK;
}

static int loop_push(ef_vm *vm, int32_t v) {
    if (vm->loop_n >= EF_CF) {
        return fail(vm, -16, "control stack overflow\n");
    }
    vm->loop_s[vm->loop_n++] = v;
    return EF_OK;
}

static int patch_forward(ef_vm *vm, int32_t ph) {
    int32_t here = uv(vm, UV_DP);
    return store_cell(vm, ph, here - (ph + CELL));
}

static int plant_back(ef_vm *vm, int32_t xt, int32_t dest) {
    int32_t here;
    if (comma(vm, xt)) {
        return EF_THROW;
    }
    here = uv(vm, UV_DP);
    return comma(vm, dest - (here + CELL));
}

/* ---- primitives ---- */

static int p_dup(ef_vm *vm) {
    if (vm->sp <= 0) {
        return fail(vm, -3, "stack underflow\n");
    }
    return push(vm, vm->stack[vm->sp - 1]);
}
static int p_drop(ef_vm *vm) {
    int32_t a;
    return pop(vm, &a);
}
static int p_swap(ef_vm *vm) {
    int32_t a, b;
    if (pop(vm, &b) || pop(vm, &a) || push(vm, b) || push(vm, a)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_over(ef_vm *vm) {
    int32_t a, b;
    if (pop(vm, &b) || pop(vm, &a) || push(vm, a) || push(vm, b) || push(vm, a)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_rot(ef_vm *vm) {
    int32_t a, b, c;
    if (pop(vm, &c) || pop(vm, &b) || pop(vm, &a) || push(vm, b) || push(vm, c) || push(vm, a)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_nip(ef_vm *vm) {
    int32_t a, b;
    if (pop(vm, &b) || pop(vm, &a) || push(vm, b)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_tuck(ef_vm *vm) {
    int32_t a, b;
    if (pop(vm, &b) || pop(vm, &a) || push(vm, b) || push(vm, a) || push(vm, b)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_pick(ef_vm *vm) {
    int32_t n;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    if (n < 0 || n >= vm->sp) {
        return fail(vm, -3, "stack underflow\n");
    }
    return push(vm, vm->stack[vm->sp - 1 - n]);
}
static int p_depth(ef_vm *vm) { return push(vm, vm->sp); }
static int p_to_r(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a) || rpush(vm, a)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_r_from(ef_vm *vm) {
    int32_t a;
    if (rpop(vm, &a) || push(vm, a)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_r_fetch(ef_vm *vm) {
    if (vm->rsp <= 0) {
        return fail(vm, -5, "return stack underflow\n");
    }
    return push(vm, vm->rstack[vm->rsp - 1]);
}

static int binop(ef_vm *vm, int32_t (*op)(int32_t, int32_t)) {
    int32_t a, b;
    if (pop(vm, &b) || pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, op(a, b));
}
static int32_t op_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t op_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t op_mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int32_t op_and(int32_t a, int32_t b) { return a & b; }
static int32_t op_or(int32_t a, int32_t b) { return a | b; }
static int32_t op_xor(int32_t a, int32_t b) { return a ^ b; }
static int p_add(ef_vm *vm) { return binop(vm, op_add); }
static int p_sub(ef_vm *vm) { return binop(vm, op_sub); }
static int p_mul(ef_vm *vm) { return binop(vm, op_mul); }
static int p_and(ef_vm *vm) { return binop(vm, op_and); }
static int p_or(ef_vm *vm) { return binop(vm, op_or); }
static int p_xor(ef_vm *vm) { return binop(vm, op_xor); }

static int unop(ef_vm *vm, int32_t (*op)(int32_t)) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, op(a));
}
static int32_t op_one_plus(int32_t a) { return (int32_t)((uint32_t)a + 1u); }
static int32_t op_one_minus(int32_t a) { return (int32_t)((uint32_t)a - 1u); }
static int32_t op_two_star(int32_t a) { return (int32_t)((uint32_t)a << 1); }
static int32_t op_two_slash(int32_t a) {
    uint32_t u = (uint32_t)a >> 1;
    if (a < 0) {
        u |= 0x80000000u;
    }
    return (int32_t)u;
}
static int32_t op_negate(int32_t a) { return (int32_t)(0u - (uint32_t)a); }
static int32_t op_abs(int32_t a) { return a < 0 ? op_negate(a) : a; }
static int32_t op_invert(int32_t a) { return ~a; }
static int p_one_plus(ef_vm *vm) { return unop(vm, op_one_plus); }
static int p_one_minus(ef_vm *vm) { return unop(vm, op_one_minus); }
static int p_two_star(ef_vm *vm) { return unop(vm, op_two_star); }
static int p_two_slash(ef_vm *vm) { return unop(vm, op_two_slash); }
static int p_negate(ef_vm *vm) { return unop(vm, op_negate); }
static int p_abs(ef_vm *vm) { return unop(vm, op_abs); }
static int p_invert(ef_vm *vm) { return unop(vm, op_invert); }

static int cmp2(ef_vm *vm, int (*pred)(int32_t, int32_t)) {
    int32_t a, b;
    if (pop(vm, &b) || pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, pred(a, b) ? -1 : 0);
}
static int c_eq(int32_t a, int32_t b) { return a == b; }
static int c_lt(int32_t a, int32_t b) { return a < b; }
static int c_gt(int32_t a, int32_t b) { return a > b; }
static int c_ne(int32_t a, int32_t b) { return a != b; }
static int c_ult(int32_t a, int32_t b) { return (uint32_t)a < (uint32_t)b; }
static int c_ugt(int32_t a, int32_t b) { return (uint32_t)a > (uint32_t)b; }
static int p_eq(ef_vm *vm) { return cmp2(vm, c_eq); }
static int p_lt(ef_vm *vm) { return cmp2(vm, c_lt); }
static int p_gt(ef_vm *vm) { return cmp2(vm, c_gt); }
static int p_ne(ef_vm *vm) { return cmp2(vm, c_ne); }
static int p_ult(ef_vm *vm) { return cmp2(vm, c_ult); }
static int p_ugt(ef_vm *vm) { return cmp2(vm, c_ugt); }

static int cmp0(ef_vm *vm, int (*pred)(int32_t)) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, pred(a) ? -1 : 0);
}
static int z_eq(int32_t a) { return a == 0; }
static int z_lt(int32_t a) { return a < 0; }
static int z_gt(int32_t a) { return a > 0; }
static int z_ne(int32_t a) { return a != 0; }
static int p_zero_eq(ef_vm *vm) { return cmp0(vm, z_eq); }
static int p_zero_lt(ef_vm *vm) { return cmp0(vm, z_lt); }
static int p_zero_gt(ef_vm *vm) { return cmp0(vm, z_gt); }
static int p_zero_ne(ef_vm *vm) { return cmp0(vm, z_ne); }

static int p_slash(ef_vm *vm) {
    int32_t a, b, q, r;
    if (pop(vm, &b) || pop(vm, &a) || divide(vm, a, b, &q, &r)) {
        return EF_THROW;
    }
    return push(vm, q);
}
static int p_mod(ef_vm *vm) {
    int32_t a, b, q, r;
    if (pop(vm, &b) || pop(vm, &a) || divide(vm, a, b, &q, &r)) {
        return EF_THROW;
    }
    return push(vm, r);
}
static int p_slash_mod(ef_vm *vm) {
    int32_t a, b, q, r;
    if (pop(vm, &b) || pop(vm, &a) || divide(vm, a, b, &q, &r) || push(vm, r) || push(vm, q)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_lshift(ef_vm *vm) {
    int32_t a, n;
    if (pop(vm, &n) || pop(vm, &a)) {
        return EF_THROW;
    }
    if (n < 0 || n >= 32) {
        return push(vm, 0);
    }
    return push(vm, (int32_t)((uint32_t)a << (uint32_t)n));
}
static int p_rshift(ef_vm *vm) {
    int32_t a, n;
    if (pop(vm, &n) || pop(vm, &a)) {
        return EF_THROW;
    }
    if (n < 0 || n >= 32) {
        return push(vm, 0);
    }
    return push(vm, (int32_t)((uint32_t)a >> (uint32_t)n));
}

static int p_fetch(ef_vm *vm) {
    int32_t a, v;
    if (pop(vm, &a) || load_cell(vm, a, &v) || push(vm, v)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_store(ef_vm *vm) {
    int32_t a, v;
    if (pop(vm, &a) || pop(vm, &v) || store_cell(vm, a, v)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_plus_store(ef_vm *vm) {
    int32_t a, v, cur;
    if (pop(vm, &a) || pop(vm, &v) || load_cell(vm, a, &cur)) {
        return EF_THROW;
    }
    return store_cell(vm, a, op_add(cur, v));
}
static int p_cfetch(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    if (a < 0 || (size_t)a >= vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    return push(vm, vm->base[a]);
}
static int p_cstore(ef_vm *vm) {
    int32_t a, v;
    if (pop(vm, &a) || pop(vm, &v)) {
        return EF_THROW;
    }
    if (ensure(vm, a + 1) || a < 0) {
        return fail(vm, -16, "bad address\n");
    }
    vm->base[a] = (uint8_t)v;
    return EF_OK;
}
static int p_comma(ef_vm *vm) {
    int32_t v;
    if (pop(vm, &v)) {
        return EF_THROW;
    }
    return comma(vm, v);
}
static int p_ccomma(ef_vm *vm) {
    int32_t v;
    int32_t here;
    if (pop(vm, &v)) {
        return EF_THROW;
    }
    here = uv(vm, UV_DP);
    if (ensure(vm, here + 1)) {
        return EF_THROW;
    }
    vm->base[here] = (uint8_t)v;
    uv_set(vm, UV_DP, here + 1);
    return EF_OK;
}
static int p_allot(ef_vm *vm) {
    int32_t n, here;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    here = uv(vm, UV_DP);
    if (n > 0 && ensure(vm, here + n)) {
        return EF_THROW;
    }
    if ((int64_t)here + n < DICT_ORIGIN) {
        return fail(vm, -8, "dictionary underflow\n");
    }
    uv_set(vm, UV_DP, here + n);
    return EF_OK;
}
static int p_here(ef_vm *vm) { return push(vm, uv(vm, UV_DP)); }
static int p_dp(ef_vm *vm) { return push(vm, UV_DP); }
static int p_cell(ef_vm *vm) { return push(vm, CELL); }
static int p_base(ef_vm *vm) { return push(vm, UV_BASE); }
static int p_state(ef_vm *vm) { return push(vm, UV_STATE); }
static int p_true(ef_vm *vm) { return push(vm, -1); }
static int p_false(ef_vm *vm) { return push(vm, 0); }
static int p_bl(ef_vm *vm) { return push(vm, ' '); }

static int p_fill(ef_vm *vm) {
    int32_t addr, n, ch;
    int32_t i;
    if (pop(vm, &ch) || pop(vm, &n) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (n < 0) {
        return fail(vm, -16, "bad count\n");
    }
    if (n == 0) {
        return EF_OK;
    }
    if (ensure(vm, addr + n)) {
        return EF_THROW;
    }
    for (i = 0; i < n; i++) {
        vm->base[addr + i] = (uint8_t)ch;
    }
    return EF_OK;
}
static int p_move(ef_vm *vm) {
    int32_t src, dst, n;
    if (pop(vm, &n) || pop(vm, &dst) || pop(vm, &src)) {
        return EF_THROW;
    }
    if (n < 0) {
        return fail(vm, -16, "bad count\n");
    }
    if (n == 0) {
        return EF_OK;
    }
    if (ensure(vm, dst + n) || src < 0 || (size_t)src + (size_t)n > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    memmove(vm->base + dst, vm->base + src, (size_t)n);
    return EF_OK;
}

static int p_to_body(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, a + 2 * CELL);
}
static int p_to_link(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, a - 3 * CELL);
}
static int p_to_flags(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    return push(vm, a - 2 * CELL);
}
static int p_to_code(ef_vm *vm) {
    (void)vm;
    return EF_OK;
}

static int p_emit(ef_vm *vm) {
    int32_t c;
    if (pop(vm, &c)) {
        return EF_THROW;
    }
    emit_ch(vm, c & 0xff);
    return EF_OK;
}
static int p_cr(ef_vm *vm) {
    emit_ch(vm, '\n');
    return EF_OK;
}
static int p_space(ef_vm *vm) {
    emit_ch(vm, ' ');
    return EF_OK;
}
static int p_type(ef_vm *vm) {
    int32_t addr, n, i;
    if (pop(vm, &n) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (n < 0 || addr < 0 || (size_t)addr + (size_t)n > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    for (i = 0; i < n; i++) {
        emit_ch(vm, vm->base[addr + i]);
    }
    return EF_OK;
}
static int p_dot(ef_vm *vm) {
    int32_t v;
    if (pop(vm, &v)) {
        return EF_THROW;
    }
    emit_int(vm, v, base_of(vm), 1);
    return EF_OK;
}
static int p_udot(ef_vm *vm) {
    int32_t v;
    if (pop(vm, &v)) {
        return EF_THROW;
    }
    emit_int(vm, v, base_of(vm), 0);
    return EF_OK;
}
static int p_dot_s(ef_vm *vm) {
    int i;
    for (i = 0; i < vm->sp; i++) {
        emit_int(vm, vm->stack[i], base_of(vm), 1);
    }
    return EF_OK;
}
static int p_key(ef_vm *vm) {
    int c;
    if (!vm->key) {
        return fail(vm, -21, "KEY is not connected\n");
    }
    c = vm->key(vm->key_ctx);
    if (c < 0) {
        return fail(vm, -21, "KEY end of input\n");
    }
    return push(vm, c & 0xff);
}

/* Full cell. KEY masks to a byte, so a tagged function key has to come through here. */
static int p_ekey(ef_vm *vm) {
    int c;
    if (!vm->key) {
        return fail(vm, -21, "KEY is not connected\n");
    }
    c = vm->key(vm->key_ctx);
    if (c < 0) {
        return fail(vm, -21, "KEY end of input\n");
    }
    return push(vm, c);
}

static int p_bye(ef_vm *vm) {
    vm->bye = 1;
    return EF_OK;
}
static int p_abort(ef_vm *vm) { return fail(vm, -1, NULL); }

static int p_exit(ef_vm *vm) {
    if (vm->frame_n > 0 && vm->frame_rsp[vm->frame_n - 1] == vm->rsp) {
        vm->frame_n--;
    }
    if (rpop(vm, &vm->ip)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_lit(ef_vm *vm) {
    int32_t v;
    if (load_cell(vm, vm->ip, &v)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    return push(vm, v);
}
static int p_branch(ef_vm *vm) {
    int32_t offset;
    if (load_cell(vm, vm->ip, &offset)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    vm->ip += offset;
    return EF_OK;
}
static int p_zbranch(ef_vm *vm) {
    int32_t offset, flag;
    if (load_cell(vm, vm->ip, &offset)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    if (pop(vm, &flag)) {
        return EF_THROW;
    }
    if (flag == 0) {
        vm->ip += offset;
    }
    return EF_OK;
}
static int p_ef_branch(ef_vm *vm) {
    int32_t addr = vm->ip;
    int32_t offset;
    if (load_cell(vm, addr, &offset)) {
        return EF_THROW;
    }
    vm->ip = addr + offset;
    return EF_OK;
}
static int p_ef_zbranch(ef_vm *vm) {
    int32_t addr = vm->ip;
    int32_t offset, flag;
    if (pop(vm, &flag)) {
        return EF_THROW;
    }
    if (flag == 0) {
        if (load_cell(vm, addr, &offset)) {
            return EF_THROW;
        }
        vm->ip = addr + offset;
    } else {
        vm->ip = addr + CELL;
    }
    return EF_OK;
}

static int p_do_rt(ef_vm *vm) {
    int32_t start, limit;
    if (pop(vm, &start) || pop(vm, &limit) || rpush(vm, limit) || rpush(vm, start)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_qdo_rt(ef_vm *vm) {
    int32_t start, limit, offset;
    if (pop(vm, &start) || pop(vm, &limit)) {
        return EF_THROW;
    }
    if (start == limit) {
        if (load_cell(vm, vm->ip, &offset)) {
            return EF_THROW;
        }
        vm->ip += CELL;
        vm->ip += offset;
        return EF_OK;
    }
    vm->ip += CELL;
    if (rpush(vm, limit) || rpush(vm, start)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_loop_rt(ef_vm *vm) {
    int32_t offset, index, limit, neu;
    if (load_cell(vm, vm->ip, &offset)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    if (rpop(vm, &index) || rpop(vm, &limit)) {
        return EF_THROW;
    }
    neu = op_one_plus(index);
    if (loop_continue(index, limit, 1)) {
        if (rpush(vm, limit) || rpush(vm, neu)) {
            return EF_THROW;
        }
        vm->ip += offset;
    }
    return EF_OK;
}
static int p_ploop_rt(ef_vm *vm) {
    int32_t delta, offset, index, limit, neu;
    if (pop(vm, &delta) || load_cell(vm, vm->ip, &offset)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    if (rpop(vm, &index) || rpop(vm, &limit)) {
        return EF_THROW;
    }
    neu = op_add(index, delta);
    if (loop_continue(index, limit, delta)) {
        if (rpush(vm, limit) || rpush(vm, neu)) {
            return EF_THROW;
        }
        vm->ip += offset;
    }
    return EF_OK;
}
static int p_i(ef_vm *vm) {
    if (vm->rsp < 1) {
        return fail(vm, -5, "return stack underflow\n");
    }
    return push(vm, vm->rstack[vm->rsp - 1]);
}
static int p_j(ef_vm *vm) {
    if (vm->rsp < 4) {
        return fail(vm, -5, "return stack underflow\n");
    }
    return push(vm, vm->rstack[vm->rsp - 3]);
}
static int p_unloop(ef_vm *vm) {
    int32_t a, b;
    if (rpop(vm, &a) || rpop(vm, &b)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_does_rt(ef_vm *vm) {
    int32_t last = uv(vm, UV_LAST);
    if (!last) {
        return fail(vm, -16, "DOES> without CREATE\n");
    }
    if (store_cell(vm, last, EF_DODOES) || store_cell(vm, last + CELL, vm->ip)) {
        return EF_THROW;
    }
    if (rpop(vm, &vm->ip)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_dot_quote_rt(ef_vm *vm) {
    int n, i;
    if (vm->ip < 0 || (size_t)vm->ip >= vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    n = vm->base[vm->ip];
    for (i = 0; i < n; i++) {
        emit_ch(vm, vm->base[vm->ip + 1 + i]);
    }
    vm->ip = skip_counted(vm, vm->ip);
    return EF_OK;
}
static int p_s_quote_rt(ef_vm *vm) {
    int n;
    int32_t addr;
    if (vm->ip < 0 || (size_t)vm->ip >= vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    n = vm->base[vm->ip];
    addr = vm->ip + 1;
    vm->ip = skip_counted(vm, vm->ip);
    if (push(vm, addr) || push(vm, n)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_colon(ef_vm *vm) {
    char name[128];
    int n;
    int32_t cfa;
    if (uv(vm, UV_STATE) != 0) {
        return fail(vm, -14, "already compiling\n");
    }
    if (!next_name(vm, name, sizeof name, &n)) {
        return fail(vm, -16, ": needs a name\n");
    }
    vm->cf_n = 0;
    vm->begin_n = 0;
    vm->loop_n = 0;
    cfa = header(vm, name, n, EF_DOCOL);
    if (!cfa) {
        return EF_THROW;
    }
    set_smudge(vm, cfa, 1);
    vm->defining = cfa;
    uv_set(vm, UV_STATE, -1);
    return EF_OK;
}
static int p_semicolon(ef_vm *vm) {
    if (compile_only(vm, ";")) {
        return EF_THROW;
    }
    if (vm->cf_n || vm->begin_n || vm->loop_n) {
        return fail(vm, -22, "control flow not finished\n");
    }
    if (comma(vm, vm->xt_exit)) {
        return EF_THROW;
    }
    if (vm->defining) {
        set_smudge(vm, vm->defining, 0);
    }
    vm->defining = 0;
    uv_set(vm, UV_STATE, 0);
    local_reset(vm);
    return EF_OK;
}
static int p_immediate(ef_vm *vm) {
    int32_t cfa = uv(vm, UV_LAST);
    int32_t f;
    if (!cfa) {
        return fail(vm, -16, "IMMEDIATE without a definition\n");
    }
    f = uv(vm, cfa - CELL);
    uv_set(vm, cfa - CELL, (int32_t)((uint32_t)f | FFA1_IMM));
    return EF_OK;
}
static int p_lbrack(ef_vm *vm) {
    uv_set(vm, UV_STATE, 0);
    return EF_OK;
}
static int p_rbrack(ef_vm *vm) {
    uv_set(vm, UV_STATE, -1);
    return EF_OK;
}
static int p_literal(ef_vm *vm) {
    int32_t v;
    if (compile_only(vm, "LITERAL")) {
        return EF_THROW;
    }
    if (pop(vm, &v) || comma(vm, vm->xt_lit) || comma(vm, v)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_recurse(ef_vm *vm) {
    int32_t cfa;
    if (compile_only(vm, "RECURSE")) {
        return EF_THROW;
    }
    cfa = uv(vm, UV_LAST);
    if (!cfa) {
        return fail(vm, -16, "RECURSE without a definition\n");
    }
    return comma(vm, cfa);
}

static int p_create(ef_vm *vm) {
    char name[128];
    int n;
    if (!next_name(vm, name, sizeof name, &n)) {
        return fail(vm, -16, "CREATE needs a name\n");
    }
    if (!header(vm, name, n, EF_DOVAR)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_variable(ef_vm *vm) {
    if (p_create(vm) || comma(vm, 0)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_constant(ef_vm *vm) {
    int32_t v;
    char name[128];
    int n;
    if (pop(vm, &v)) {
        return EF_THROW;
    }
    if (!next_name(vm, name, sizeof name, &n)) {
        return fail(vm, -16, "CONSTANT needs a name\n");
    }
    if (!header(vm, name, n, EF_DOCON) || comma(vm, v)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_does(ef_vm *vm) {
    if (compile_only(vm, "DOES>")) {
        return EF_THROW;
    }
    return comma(vm, vm->xt_does);
}

static int p_begin(ef_vm *vm) {
    if (compile_only(vm, "BEGIN")) {
        return EF_THROW;
    }
    return begin_push(vm, uv(vm, UV_DP));
}
static int p_until(ef_vm *vm) {
    int32_t dest;
    if (compile_only(vm, "UNTIL")) {
        return EF_THROW;
    }
    if (vm->begin_n <= 0) {
        return fail(vm, -22, "UNTIL without BEGIN\n");
    }
    dest = vm->begin_s[--vm->begin_n];
    return plant_back(vm, vm->xt_zbranch, dest);
}
static int p_again(ef_vm *vm) {
    int32_t dest;
    if (compile_only(vm, "AGAIN")) {
        return EF_THROW;
    }
    if (vm->begin_n <= 0) {
        return fail(vm, -22, "AGAIN without BEGIN\n");
    }
    dest = vm->begin_s[--vm->begin_n];
    return plant_back(vm, vm->xt_branch, dest);
}
static int p_while(ef_vm *vm) {
    int32_t ph;
    if (compile_only(vm, "WHILE")) {
        return EF_THROW;
    }
    if (vm->begin_n <= 0) {
        return fail(vm, -22, "WHILE without BEGIN\n");
    }
    if (comma(vm, vm->xt_zbranch)) {
        return EF_THROW;
    }
    ph = uv(vm, UV_DP);
    if (comma(vm, 0) || cf_push(vm, ph)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_repeat(ef_vm *vm) {
    int32_t ph, dest;
    if (compile_only(vm, "REPEAT")) {
        return EF_THROW;
    }
    if (cf_pop(vm, &ph, "REPEAT without WHILE\n")) {
        return EF_THROW;
    }
    if (vm->begin_n <= 0) {
        return fail(vm, -22, "REPEAT without BEGIN\n");
    }
    dest = vm->begin_s[--vm->begin_n];
    if (plant_back(vm, vm->xt_branch, dest) || patch_forward(vm, ph)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_ahead(ef_vm *vm) {
    int32_t ph;
    if (compile_only(vm, "AHEAD")) {
        return EF_THROW;
    }
    if (comma(vm, vm->xt_branch)) {
        return EF_THROW;
    }
    ph = uv(vm, UV_DP);
    if (comma(vm, 0) || cf_push(vm, ph)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_if(ef_vm *vm) {
    int32_t ph;
    if (compile_only(vm, "IF")) {
        return EF_THROW;
    }
    if (comma(vm, vm->xt_zbranch)) {
        return EF_THROW;
    }
    ph = uv(vm, UV_DP);
    if (comma(vm, 0) || cf_push(vm, ph)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_else(ef_vm *vm) {
    int32_t ph, neu;
    if (compile_only(vm, "ELSE")) {
        return EF_THROW;
    }
    if (cf_pop(vm, &ph, "ELSE without IF\n")) {
        return EF_THROW;
    }
    if (comma(vm, vm->xt_branch)) {
        return EF_THROW;
    }
    neu = uv(vm, UV_DP);
    if (comma(vm, 0) || patch_forward(vm, ph) || cf_push(vm, neu)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_then(ef_vm *vm) {
    int32_t ph;
    if (compile_only(vm, "THEN")) {
        return EF_THROW;
    }
    if (cf_pop(vm, &ph, "THEN without IF\n")) {
        return EF_THROW;
    }
    return patch_forward(vm, ph);
}

static int finish_loop(ef_vm *vm, int32_t xt, const char *what) {
    int32_t dest = 0;
    int32_t phs[EF_CF];
    int np = 0;
    int32_t after;
    int i;
    if (compile_only(vm, what)) {
        return EF_THROW;
    }
    while (vm->loop_n > 0) {
        int32_t x = vm->loop_s[--vm->loop_n];
        if (x == 0) {
            break;
        }
        phs[np++] = x;
    }
    if (vm->loop_n <= 0) {
        return fail(vm, -22, "LOOP without DO\n");
    }
    dest = vm->loop_s[--vm->loop_n];
    if (plant_back(vm, xt, dest)) {
        return EF_THROW;
    }
    after = uv(vm, UV_DP);
    for (i = 0; i < np; i++) {
        if (store_cell(vm, phs[i], after - (phs[i] + CELL))) {
            return EF_THROW;
        }
    }
    return EF_OK;
}
static int p_do(ef_vm *vm) {
    if (compile_only(vm, "DO")) {
        return EF_THROW;
    }
    if (comma(vm, vm->xt_do) || loop_push(vm, uv(vm, UV_DP)) || loop_push(vm, 0)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_qdo(ef_vm *vm) {
    int32_t ph;
    if (compile_only(vm, "?DO")) {
        return EF_THROW;
    }
    if (comma(vm, vm->xt_qdo)) {
        return EF_THROW;
    }
    ph = uv(vm, UV_DP);
    if (comma(vm, 0) || loop_push(vm, uv(vm, UV_DP)) || loop_push(vm, 0) || loop_push(vm, ph)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_loop(ef_vm *vm) { return finish_loop(vm, vm->xt_loop, "LOOP"); }
static int p_ploop(ef_vm *vm) { return finish_loop(vm, vm->xt_ploop, "+LOOP"); }
static int p_leave(ef_vm *vm) {
    int32_t ph;
    if (compile_only(vm, "LEAVE")) {
        return EF_THROW;
    }
    if (comma(vm, vm->xt_unloop) || comma(vm, vm->xt_branch)) {
        return EF_THROW;
    }
    ph = uv(vm, UV_DP);
    if (comma(vm, 0) || loop_push(vm, ph)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_backslash(ef_vm *vm) { return skip_line(vm); }
static int p_paren(ef_vm *vm) {
    char buf[4];
    int n;
    return parse_until(vm, ')', buf, 0, &n);
}
static int compile_or_type_string(ef_vm *vm, int32_t rt, int do_type, int do_push) {
    char buf[4096];
    int n;
    int32_t addr;
    int compiling = uv(vm, UV_STATE) != 0;
    if (parse_until(vm, do_type && rt == vm->xt_dotparen ? ')' : '"', buf, sizeof buf, &n)) {
        return EF_THROW;
    }
    if (!compiling) {
        if (do_type) {
            int i;
            for (i = 0; i < n; i++) {
                emit_ch(vm, buf[i]);
            }
        }
        if (do_push) {
            if (plant_string(vm, buf, n, &addr) || push(vm, addr) || push(vm, n)) {
                return EF_THROW;
            }
        }
        return EF_OK;
    }
    if (comma(vm, rt) || plant_string(vm, buf, n, NULL)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_dot_quote(ef_vm *vm) { return compile_or_type_string(vm, vm->xt_dotq, 1, 0); }
static int p_s_quote(ef_vm *vm) { return compile_or_type_string(vm, vm->xt_squote, 0, 1); }
static int p_dot_paren(ef_vm *vm) {
    char buf[4096];
    int n, i;
    /* .( types immediately, including while a colon definition is being compiled. */
    if (parse_until(vm, ')', buf, (int)sizeof buf, &n)) {
        return EF_THROW;
    }
    for (i = 0; i < n; i++) {
        emit_ch(vm, buf[i]);
    }
    return EF_OK;
}

static int p_tick(ef_vm *vm) {
    char name[128];
    int n;
    int32_t cfa;
    if (!next_name(vm, name, sizeof name, &n)) {
        return fail(vm, -16, "' needs a name\n");
    }
    cfa = find_word(vm, name, n, NULL);
    if (!cfa) {
        emit_str(vm, "undefined: ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        vm->throw_code = -13;
        return EF_THROW;
    }
    return push(vm, cfa);
}
static int p_bracket_tick(ef_vm *vm) {
    int32_t cfa;
    if (compile_only(vm, "[']")) {
        return EF_THROW;
    }
    if (p_tick(vm)) {
        return EF_THROW;
    }
    if (pop(vm, &cfa) || comma(vm, vm->xt_lit) || comma(vm, cfa)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_execute(ef_vm *vm) {
    int32_t cfa;
    if (pop(vm, &cfa)) {
        return EF_THROW;
    }
    return exec_one(vm, cfa, vm->in_inner);
}
static int p_char(ef_vm *vm) {
    char name[128];
    int n;
    if (!next_name(vm, name, sizeof name, &n) || n < 1) {
        return fail(vm, -16, "CHAR needs a name\n");
    }
    return push(vm, (unsigned char)name[0]);
}
static int p_bracket_char(ef_vm *vm) {
    int32_t c;
    if (compile_only(vm, "[CHAR]")) {
        return EF_THROW;
    }
    if (p_char(vm) || pop(vm, &c) || comma(vm, vm->xt_lit) || comma(vm, c)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_find(ef_vm *vm) {
    int32_t addr;
    int n;
    int imm = 0;
    int32_t cfa;
    char tmp[128];
    if (pop(vm, &addr)) {
        return EF_THROW;
    }
    if (addr < 0 || (size_t)addr >= vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    n = vm->base[addr] & 0x7f;
    if (n >= (int)sizeof tmp) {
        n = (int)sizeof tmp - 1;
    }
    memcpy(tmp, vm->base + addr + 1, (size_t)n);
    tmp[n] = 0;
    cfa = find_word(vm, tmp, n, &imm);
    if (!cfa) {
        return push(vm, addr) || push(vm, 0);
    }
    if (push(vm, cfa) || push(vm, imm ? 1 : -1)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_word(ef_vm *vm) {
    int32_t delim;
    int in, len, n, start;
    uint8_t *s;
    if (pop(vm, &delim)) {
        return EF_THROW;
    }
    in = uv(vm, UV_IN);
    len = vm->src_len;
    s = vm->base + SRC_ADDR;
    if (delim == ' ') {
        while (in < len && (s[in] == ' ' || s[in] == '\t' || s[in] == '\n' || s[in] == '\r')) {
            if (s[in] == '\n') {
                vm->line++;
            }
            in++;
        }
    }
    start = in;
    while (in < len && s[in] != (uint8_t)delim && !(delim == ' ' && is_white(s[in]))) {
        in++;
    }
    n = in - start;
    if (n > 127) {
        n = 127;
    }
    if (in < len && s[in] == (uint8_t)delim) {
        in++;
    }
    uv_set(vm, UV_IN, in);
    if (ensure(vm, WORD_ADDR + 1 + n + 1)) {
        return EF_THROW;
    }
    vm->base[WORD_ADDR] = (uint8_t)n;
    memcpy(vm->base + WORD_ADDR + 1, s + start, (size_t)n);
    return push(vm, WORD_ADDR);
}

/* Case-insensitive substring. An empty filter matches every name. */
static int contains_ci(const char *name, const char *filt, int flen) {
    int nlen, i, j;
    if (flen <= 0) {
        return 1;
    }
    nlen = (int)strlen(name);
    if (flen > nlen) {
        return 0;
    }
    for (i = 0; i <= nlen - flen; i++) {
        for (j = 0; j < flen; j++) {
            if (toupper((unsigned char)name[i + j]) != toupper((unsigned char)filt[j])) {
                break;
            }
        }
        if (j == flen) {
            return 1;
        }
    }
    return 0;
}

/* A listed name: not smudged, non-empty, and containing the filter. */
static int word_listed(ef_vm *vm, int32_t cfa, const char *filt, int flen, char *name, int namemax) {
    if (smudged(vm, cfa)) {
        return 0;
    }
    copy_name(vm, cfa, name, namemax);
    return name[0] && contains_ci(name, filt, flen);
}

/* FORTH, the VOCABULARY whose body is this wid, or "wid". */
static void emit_wid_name(ef_vm *vm, int32_t wid) {
    int th;
    if (wid == vm->forth_wid) {
        emit_str(vm, "FORTH");
        return;
    }
    for (th = 0; th < EF_THREADS; th++) {
        int32_t cfa = uv(vm, vm->forth_wid + th * CELL);
        int guard = 0;
        while (cfa && guard++ < 1000000) {
            if (uv(vm, cfa) == EF_DODOES && cfa + 2 * CELL == wid) {
                char name[128];
                copy_name(vm, cfa, name, sizeof name);
                if (name[0]) {
                    emit_str(vm, name);
                    return;
                }
            }
            cfa = uv(vm, cfa - 3 * CELL);
        }
    }
    emit_str(vm, "wid");
}

/* WORDS [filter] — names in the first search-order wordlist.
   Header is "--- NAME (n) ---" with n in the current base.
   The next word is the filter, not a command. */
static int p_words(ef_vm *vm) {
    char filt[128];
    int flen = 0;
    int th;
    int32_t wid;
    int norder;
    int count = 0;
    int first = 1;
    norder = uv(vm, UV_ORDERN);
    wid = norder > 0 ? uv(vm, UV_ORDER) : vm->forth_wid;
    if (!next_name(vm, filt, (int)sizeof filt, &flen)) {
        flen = 0;
    }
    for (th = 0; th < EF_THREADS; th++) {
        int32_t cfa = uv(vm, wid + th * CELL);
        int guard = 0;
        while (cfa && guard++ < 1000000) {
            char name[128];
            if (word_listed(vm, cfa, filt, flen, name, sizeof name)) {
                count++;
            }
            cfa = uv(vm, cfa - 3 * CELL);
        }
    }
    emit_str(vm, "--- ");
    emit_wid_name(vm, wid);
    emit_str(vm, " (");
    emit_uint(vm, (uint32_t)count, base_of(vm));
    emit_str(vm, ") ---\n");
    for (th = 0; th < EF_THREADS; th++) {
        int32_t cfa = uv(vm, wid + th * CELL);
        int guard = 0;
        while (cfa && guard++ < 1000000) {
            char name[128];
            if (word_listed(vm, cfa, filt, flen, name, sizeof name)) {
                if (!first) {
                    emit_ch(vm, ' ');
                }
                first = 0;
                emit_str(vm, name);
            }
            cfa = uv(vm, cfa - 3 * CELL);
        }
    }
    emit_ch(vm, '\n');
    return EF_OK;
}

static int p_see(ef_vm *vm) {
    char name[128];
    int n;
    int32_t cfa, code, ip;
    int guard;
    if (!next_name(vm, name, sizeof name, &n)) {
        return fail(vm, -16, "SEE needs a name\n");
    }
    cfa = find_word(vm, name, n, NULL);
    if (!cfa) {
        emit_str(vm, "undefined: ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        vm->throw_code = -13;
        return EF_THROW;
    }
    copy_name(vm, cfa, name, sizeof name);
    code = uv(vm, cfa);
    if (code == EF_DOCON) {
        int32_t v = uv(vm, cfa + 2 * CELL);
        emit_int(vm, v, base_of(vm), 1);
        emit_str(vm, "CONSTANT ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        return EF_OK;
    }
    if (code == EF_DOVAR) {
        emit_str(vm, "VARIABLE ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        return EF_OK;
    }
    if (code != EF_DOCOL && code != EF_DODOES) {
        emit_str(vm, "CODE ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        return EF_OK;
    }
    emit_str(vm, ": ");
    emit_str(vm, name);
    emit_ch(vm, ' ');
    ip = (code == EF_DODOES) ? uv(vm, cfa + CELL) : cfa + 2 * CELL;
    for (guard = 0; guard < 10000; guard++) {
        int32_t xt;
        char wname[128];
        if (load_cell(vm, ip, &xt)) {
            return EF_THROW;
        }
        ip += CELL;
        if (xt == vm->xt_exit) {
            emit_str(vm, ";");
            break;
        }
        if (xt == vm->xt_lit) {
            int32_t v;
            if (load_cell(vm, ip, &v)) {
                return EF_THROW;
            }
            ip += CELL;
            emit_str(vm, "LIT ");
            emit_int(vm, v, base_of(vm), 1);
            continue;
        }
        if (xt == vm->xt_dotq || xt == vm->xt_squote) {
            int sn = vm->base[ip];
            int i;
            emit_str(vm, xt == vm->xt_dotq ? ".\" " : "S\" ");
            for (i = 0; i < sn; i++) {
                emit_ch(vm, vm->base[ip + 1 + i]);
            }
            emit_str(vm, "\" ");
            ip = skip_counted(vm, ip);
            continue;
        }
        copy_name(vm, xt, wname, sizeof wname);
        emit_str(vm, wname);
        emit_ch(vm, ' ');
        if (xt == vm->xt_branch || xt == vm->xt_zbranch || xt == vm->xt_loop ||
            xt == vm->xt_ploop || xt == vm->xt_qdo || xt == vm->xt_ef_branch ||
            xt == vm->xt_ef_zbranch) {
            int32_t offset;
            if (load_cell(vm, ip, &offset)) {
                return EF_THROW;
            }
            ip += CELL;
            emit_int(vm, offset, 10, 1);
        }
    }
    emit_ch(vm, '\n');
    return EF_OK;
}

static int p_forth(ef_vm *vm) {
    uv_set(vm, UV_ORDERN, 1);
    uv_set(vm, UV_ORDER, vm->forth_wid);
    return EF_OK;
}
static int p_definitions(ef_vm *vm) {
    uv_set(vm, UV_CURRENT, uv(vm, UV_ORDER));
    return EF_OK;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n + 1);
    }
    return d;
}

static int path_ok(const char *path) {
    const char *p;
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\') {
        return 0;
    }
    if (strlen(path) > 500) {
        return 0;
    }
    p = path;
    while (*p) {
        const char *start = p;
        size_t n;
        while (*p && *p != '/' && *p != '\\') {
            if (isspace((unsigned char)*p) || *p == '"' || *p == '\'') {
                return 0;
            }
            p++;
        }
        n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.') || (n == 2 && start[0] == '.' && start[1] == '.')) {
            return 0;
        }
        if (*p) {
            p++;
        }
    }
    return 1;
}

static void slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

static void dir_of(const char *path, char *dst, size_t n) {
    const char *slash;
    size_t len;
    if (n == 0) {
        return;
    }
    dst[0] = 0;
    if (!path) {
        return;
    }
    slash = strrchr(path, '/');
    if (!slash) {
        return;
    }
    len = (size_t)(slash - path);
    if (len >= n) {
        len = n - 1;
    }
    memcpy(dst, path, len);
    dst[len] = 0;
}

int ef_mount_clear(ef_vm *vm) {
    ef_mounted *m;
    if (!vm) {
        return -1;
    }
    m = vm->mounted;
    while (m) {
        ef_mounted *next = m->next;
        free(m->path);
        free(m->text);
        free(m);
        m = next;
    }
    vm->mounted = NULL;
    return 0;
}

int ef_mount_file(ef_vm *vm, const char *path, const char *text) {
    char norm[512];
    ef_mounted *m;
    char *copy;
    if (!vm || !path_ok(path)) {
        return -16;
    }
    snprintf(norm, sizeof norm, "%s", path);
    slashes(norm);
    if (!text) {
        text = "";
    }
    if (strlen(text) > (size_t)EF_SRC_MAX - 1) {
        return -16;
    }
    copy = dup_str(text);
    if (!copy) {
        return -8;
    }
    for (m = vm->mounted; m; m = m->next) {
        if (strcmp(m->path, norm) == 0) {
            free(m->text);
            m->text = copy;
            return 0;
        }
    }
    m = calloc(1, sizeof *m);
    if (!m) {
        free(copy);
        return -8;
    }
    m->path = dup_str(norm);
    if (!m->path) {
        free(copy);
        free(m);
        return -8;
    }
    m->text = copy;
    m->next = vm->mounted;
    vm->mounted = m;
    return 0;
}

int ef_set_source(ef_vm *vm, const char *name) {
    if (!vm) {
        return -1;
    }
    if (!name) {
        name = "";
    }
    if (name[0] && !path_ok(name)) {
        return -16;
    }
    snprintf(vm->src_name, sizeof vm->src_name, "%s", name);
    slashes(vm->src_name);
    dir_of(vm->src_name, vm->include_dir, sizeof vm->include_dir);
    return 0;
}

static void remember_view(ef_vm *vm, int id, const char *path) {
    int i;
    if (!path || !path[0] || id <= 0) {
        return;
    }
    for (i = 0; i < vm->nviews; i++) {
        if (vm->views[i].id == id) {
            snprintf(vm->views[i].path, sizeof vm->views[i].path, "%s", path);
            return;
        }
    }
    if (vm->nviews < EF_VIEWS) {
        i = vm->nviews++;
    } else {
        i = id % EF_VIEWS;
    }
    vm->views[i].id = id;
    snprintf(vm->views[i].path, sizeof vm->views[i].path, "%s", path);
}

static int include_text(ef_vm *vm, const char *resolved, const char *text) {
    char saved_dir[512];
    char saved_name[256];
    int saved_file = vm->file_id;
    int rc;
    snprintf(saved_dir, sizeof saved_dir, "%s", vm->include_dir);
    snprintf(saved_name, sizeof saved_name, "%s", vm->src_name);
    dir_of(resolved, vm->include_dir, sizeof vm->include_dir);
    snprintf(vm->src_name, sizeof vm->src_name, "%s", resolved);
    vm->file_id = vm->next_file++;
    if (vm->next_file > 0x1fff) {
        vm->next_file = 2;
    }
    remember_view(vm, vm->file_id, resolved);
    rc = ef_eval(vm, text);
    vm->file_id = saved_file;
    snprintf(vm->include_dir, sizeof vm->include_dir, "%s", saved_dir);
    snprintf(vm->src_name, sizeof vm->src_name, "%s", saved_name);
    if (rc) {
        vm->throw_code = rc;
        return EF_THROW;
    }
    return EF_OK;
}

#ifndef __EMSCRIPTEN__
static int read_disk(const char *name, char **out) {
    FILE *fp;
    long sz;
    char *buf;
    *out = NULL;
    fp = fopen(name, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    sz = ftell(fp);
    if (sz < 0 || sz > EF_SRC_MAX - 1) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buf[sz] = 0;
    *out = buf;
    return 0;
}
#endif

static int copy_owned(const char *text, int len, char **out) {
    char *b;
    if (len < 0 || len > EF_SRC_MAX - 1) {
        return -1;
    }
    b = malloc((size_t)len + 1);
    if (!b) {
        return -1;
    }
    if (len > 0) {
        memcpy(b, text, (size_t)len);
    }
    b[len] = 0;
    *out = b;
    return 0;
}

static int mounted_copy(ef_vm *vm, const char *path, char **out, int *len) {
    ef_mounted *m;
    for (m = vm->mounted; m; m = m->next) {
        if (strcmp(m->path, path) == 0) {
            int n = (int)strlen(m->text);
            if (copy_owned(m->text, n, out)) {
                return 0;
            }
            *len = n;
            return 1;
        }
    }
    return 0;
}

static void add_cand(char cands[][512], int *nc, const char *s) {
    char tmp[512];
    int i;
    if (!s || !s[0]) {
        return;
    }
    snprintf(tmp, sizeof tmp, "%s", s);
    slashes(tmp);
    if (!path_ok(tmp)) {
        return;
    }
    for (i = 0; i < *nc; i++) {
        if (strcmp(cands[i], tmp) == 0) {
            return;
        }
    }
    if (*nc >= 5) {
        return;
    }
    snprintf(cands[*nc], 512, "%s", tmp);
    (*nc)++;
}

/* Cold words are stamped view file "boot" (build/boot.inc). */
static int open_embedded_boot(char **owned, int *len);

/* Mounted paths first, then disk. include_dir and the directory saved by
   BEGIN-LOAD-CWD are both tried: BEGIN runs before OPEN-FILE. */
static int resolve_file(ef_vm *vm, const char *name, char *resolved, size_t rmax,
                        char **owned, int *len) {
    char cands[5][512];
    char joined[512];
    int nc = 0;
    int i;
    static const char libprefix[] = "Resources/Library/";
    *owned = NULL;
    *len = 0;
    resolved[0] = 0;
    add_cand(cands, &nc, name);
    /* FROMLIB plants Resources/Library/name. An open Library folder mounts
       the same file as name, with no Resources/Library prefix. */
    if (strncmp(name, libprefix, sizeof libprefix - 1) == 0) {
        add_cand(cands, &nc, name + (sizeof libprefix - 1));
    }
    if (vm->include_dir[0]) {
        snprintf(joined, sizeof joined, "%s/%s", vm->include_dir, name);
        add_cand(cands, &nc, joined);
    }
    if (vm->cwd_n > 0 && vm->cwd_stack[vm->cwd_n - 1][0]) {
        snprintf(joined, sizeof joined, "%s/%s", vm->cwd_stack[vm->cwd_n - 1], name);
        add_cand(cands, &nc, joined);
    }
    /* Session directory from CHDIR. Tried after the include stack so a nested
       load still follows the file that is open. */
    if (vm->logical_cwd[0]) {
        snprintf(joined, sizeof joined, "%s/%s", vm->logical_cwd, name);
        add_cand(cands, &nc, joined);
    }
    for (i = 0; i < nc; i++) {
        if (mounted_copy(vm, cands[i], owned, len)) {
            snprintf(resolved, rmax, "%s", cands[i]);
            return 1;
        }
    }
#ifndef __EMSCRIPTEN__
    for (i = 0; i < nc; i++) {
        char *buf = NULL;
        if (read_disk(cands[i], &buf) == 0) {
            *owned = buf;
            *len = (int)strlen(buf);
            snprintf(resolved, rmax, "%s", cands[i]);
            return 1;
        }
    }
#endif
    /* A mounted or on-disk "boot" already won. Page mountClear cannot drop
       this copy: it is the concatenated cold source whose line numbers match
       VIEW-LINE (kernel1 2OVER is line 67). */
    if (name && strcmp(name, "boot") == 0 && open_embedded_boot(owned, len) == 0) {
        snprintf(resolved, rmax, "boot");
        return 1;
    }
    return 0;
}

static int include_by_name(ef_vm *vm, const char *name) {
    char resolved[512];
    char *owned = NULL;
    int len = 0;
    int rc;
    if (!resolve_file(vm, name, resolved, sizeof resolved, &owned, &len)) {
        emit_str(vm, "cannot open ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        return fail(vm, -38, NULL);
    }
    rc = include_text(vm, resolved, owned);
    free(owned);
    return rc;
}

static int p_include(ef_vm *vm) {
    char name[512];
    int n;
    if (!next_name(vm, name, sizeof name, &n)) {
        return fail(vm, -16, "INCLUDE needs a name\n");
    }
    name[n] = 0;
    return include_by_name(vm, name);
}

static int interpret(ef_vm *vm) {
    char name[EF_WORD_MAX];
    int n;
    vm->line = 1;
    while (!vm->throw_code && next_name(vm, name, sizeof name, &n)) {
        int imm = 0;
        int32_t cfa;
        int compiling = uv(vm, UV_STATE) != 0;
        if (compiling) {
            int idx = local_lookup(vm, name, n);
            if (idx >= 0) {
                if (local_compile_op(vm, idx, vm->xt_lat)) {
                    return EF_THROW;
                }
                if (vm->bye) {
                    return EF_OK;
                }
                continue;
            }
        }
        cfa = find_word(vm, name, n, &imm);
        if (cfa) {
            if (compiling && !imm) {
                if (comma(vm, cfa)) {
                    return EF_THROW;
                }
            } else if (exec_one(vm, cfa, 0)) {
                return EF_THROW;
            }
        } else {
            int32_t num;
            if (!parse_number(name, n, base_of(vm), &num)) {
                emit_str(vm, "undefined: ");
                emit_str(vm, name);
                emit_ch(vm, '\n');
                vm->throw_code = -13;
                return EF_THROW;
            }
            if (compiling) {
                if (comma(vm, vm->xt_lit) || comma(vm, num)) {
                    return EF_THROW;
                }
            } else if (push(vm, num)) {
                return EF_THROW;
            }
        }
        if (vm->bye) {
            return EF_OK;
        }
    }
    return vm->throw_code ? EF_THROW : EF_OK;
}

typedef struct {
    char *text;
    int len;
    int in;
    int line;
    int file_id;
    int defining;
} src_frame;

static int save_source(ef_vm *vm, src_frame *f) {
    f->len = vm->src_len;
    f->in = uv(vm, UV_IN);
    f->line = vm->line;
    f->file_id = vm->file_id;
    f->defining = vm->defining;
    f->text = NULL;
    if (f->len > 0) {
        f->text = malloc((size_t)f->len);
        if (!f->text) {
            return -1;
        }
        memcpy(f->text, vm->base + SRC_ADDR, (size_t)f->len);
    }
    return 0;
}

static void restore_source(ef_vm *vm, src_frame *f) {
    if (f->text && f->len > 0) {
        if (ensure(vm, SRC_ADDR + f->len) == EF_OK) {
            memcpy(vm->base + SRC_ADDR, f->text, (size_t)f->len);
        }
    }
    vm->src_len = f->len;
    uv_set(vm, UV_IN, f->in);
    vm->line = f->line;
    vm->file_id = f->file_id;
    free(f->text);
}

static int p_throw(ef_vm *vm) {
    int32_t code;
    if (pop(vm, &code)) {
        return EF_THROW;
    }
    if (code == 0) {
        return EF_OK;
    }
    vm->throw_code = code;
    return EF_THROW;
}
static int p_last(ef_vm *vm) {
    return push(vm, uv(vm, UV_LAST));
}
static int p_parse(ef_vm *vm) {
    int32_t delim;
    char buf[EF_PARSE_MAX];
    int n;
    if (pop(vm, &delim)) {
        return EF_THROW;
    }
    if (parse_until(vm, (char)(delim & 0xff), buf, (int)sizeof buf, &n)) {
        return EF_THROW;
    }
    if (ensure(vm, PARSE_ADDR + n + 1)) {
        return EF_THROW;
    }
    memcpy(vm->base + PARSE_ADDR, buf, (size_t)n);
    vm->base[PARSE_ADDR + n] = 0;
    if (push(vm, PARSE_ADDR) || push(vm, n)) {
        return EF_THROW;
    }
    return EF_OK;
}
static int p_setdoc(ef_vm *vm) {
    int32_t u, addr;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u < 0 || addr < 0 || (size_t)addr + (size_t)u > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    if (u > 255) {
        u = 255;
    }
    memcpy(vm->doc, vm->base + addr, (size_t)u);
    vm->doc_len = u;
    return EF_OK;
}
static int p_get_order(ef_vm *vm) {
    int n = uv(vm, UV_ORDERN);
    int i;
    if (n < 0) {
        n = 0;
    }
    if (n > EF_ORDER_MAX) {
        n = EF_ORDER_MAX;
    }
    for (i = n - 1; i >= 0; i--) {
        if (push(vm, uv(vm, UV_ORDER + i * CELL))) {
            return EF_THROW;
        }
    }
    return push(vm, n);
}
static int p_set_order(ef_vm *vm) {
    int32_t n, wid;
    int i;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    if (n < 0 || n > EF_ORDER_MAX) {
        return fail(vm, -16, "bad search order\n");
    }
    for (i = 0; i < n; i++) {
        if (pop(vm, &wid)) {
            return EF_THROW;
        }
        uv_set(vm, UV_ORDER + i * CELL, wid);
    }
    uv_set(vm, UV_ORDERN, n);
    return EF_OK;
}
static int p_forth_wid(ef_vm *vm) {
    return push(vm, vm->forth_wid);
}
static int p_dict_threads(ef_vm *vm) {
    return push(vm, EF_THREADS);
}

static int push2(ef_vm *vm, int32_t a, int32_t b) {
    if (push(vm, a)) {
        return EF_THROW;
    }
    return push(vm, b);
}

static uint64_t pack_d(int32_t lo, int32_t hi) {
    return ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
}

static int push_d(ef_vm *vm, uint64_t d) {
    if (push(vm, (int32_t)(uint32_t)d)) {
        return EF_THROW;
    }
    return push(vm, (int32_t)(uint32_t)(d >> 32));
}

static int span_ok(ef_vm *vm, int32_t addr, int32_t n) {
    if (n == 0) {
        return 1;
    }
    if (n < 0 || addr < 0) {
        return 0;
    }
    return (size_t)addr + (size_t)n <= vm->count;
}

static int pop_text(ef_vm *vm, char *dst, int dstmax) {
    int32_t u, addr;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u < 0 || u >= dstmax || !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    if (u > 0) {
        memcpy(dst, vm->base + addr, (size_t)u);
    }
    dst[u] = 0;
    return EF_OK;
}

static int plant_parse(ef_vm *vm, const char *s, int n) {
    if (n < 0) {
        n = 0;
    }
    if (n >= EF_PARSE_MAX) {
        n = EF_PARSE_MAX - 1;
    }
    if (ensure(vm, PARSE_ADDR + n + 1)) {
        return EF_THROW;
    }
    if (n > 0) {
        memcpy(vm->base + PARSE_ADDR, s, (size_t)n);
    }
    vm->base[PARSE_ADDR + n] = 0;
    return push2(vm, PARSE_ADDR, n);
}

static int skip_white_src(ef_vm *vm) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = vm->base + SRC_ADDR;
    while (in < len && is_white(s[in])) {
        if (s[in] == '\n') {
            vm->line++;
        }
        in++;
    }
    uv_set(vm, UV_IN, in);
    return in;
}

static int32_t hfa_of(ef_vm *vm, int32_t cfa) {
    uint32_t ffa0;
    if (cfa < 12 || (size_t)cfa + CELL > vm->count) {
        return cfa;
    }
    ffa0 = (uint32_t)uv(vm, cfa - 2 * CELL);
    return cfa - (int32_t)(ffa0 >> 16);
}

static void prune_wid(ef_vm *vm, int32_t wid, int32_t cut) {
    int th;
    if (wid < 0 || (size_t)wid + (size_t)EF_THREADS * CELL > vm->count) {
        return;
    }
    for (th = 0; th < EF_THREADS; th++) {
        int32_t link = wid + th * CELL;
        int guard = 0;
        while (guard++ < 1000000) {
            int32_t cfa = uv(vm, link);
            if (!cfa || hfa_of(vm, cfa) < cut) {
                break;
            }
            uv_set(vm, link, uv(vm, cfa - 3 * CELL));
        }
    }
}

static void prune_order(ef_vm *vm, int32_t cut) {
    int n = uv(vm, UV_ORDERN);
    int i, w = 0;
    int32_t keep[EF_ORDER_MAX];
    if (n < 0) {
        n = 0;
    }
    if (n > EF_ORDER_MAX) {
        n = EF_ORDER_MAX;
    }
    for (i = 0; i < n; i++) {
        int32_t wid = uv(vm, UV_ORDER + i * CELL);
        if (wid < cut) {
            keep[w++] = wid;
        }
    }
    if (w == 0) {
        uv_set(vm, UV_ORDERN, 1);
        uv_set(vm, UV_ORDER, vm->forth_wid);
        return;
    }
    for (i = 0; i < w; i++) {
        uv_set(vm, UV_ORDER + i * CELL, keep[i]);
    }
    uv_set(vm, UV_ORDERN, w);
}

static int slot_of(ef_vm *vm, int32_t fid) {
    if (fid < 1 || fid > EF_OPEN || !vm->files[fid - 1].used) {
        return -1;
    }
    return (int)fid - 1;
}

static int alloc_slot(ef_vm *vm) {
    int i;
    for (i = 0; i < EF_OPEN; i++) {
        if (!vm->files[i].used) {
            return i;
        }
    }
    return -1;
}

static int file_grow(ef_vm *vm, int s, int need) {
    int cap;
    char *p;
    int old;
    if (need <= vm->files[s].cap) {
        return 0;
    }
    if (need > EF_SRC_MAX) {
        return -1;
    }
    old = vm->files[s].cap;
    cap = old > 0 ? old : 64;
    while (cap < need) {
        if (cap > EF_SRC_MAX / 2) {
            cap = EF_SRC_MAX;
            break;
        }
        cap *= 2;
    }
    if (cap < need) {
        return -1;
    }
    p = realloc(vm->files[s].data, (size_t)cap);
    if (!p) {
        return -1;
    }
    if (cap > old) {
        memset(p + old, 0, (size_t)(cap - old));
    }
    vm->files[s].data = p;
    vm->files[s].cap = cap;
    return 0;
}

static int p_pad(ef_vm *vm) { return push(vm, PAD_ADDR); }
static int p_unused(ef_vm *vm) { return push(vm, HEAP_ORIGIN - uv(vm, UV_DP)); }
static int p_to_in(ef_vm *vm) { return push(vm, UV_IN); }
static int p_source(ef_vm *vm) { return push2(vm, SRC_ADDR, vm->src_len); }
static int p_refill(ef_vm *vm) { return push(vm, 0); }

/* No graphics window on this page. The names exist so app-output.fth can load.
   (APP-KEY) returns 0: KEY loops while the result is negative. */
static int p_app_open(ef_vm *vm) {
    int32_t cols, rows;
    if (pop(vm, &rows) || pop(vm, &cols)) {
        return EF_THROW;
    }
    (void)cols;
    (void)rows;
    return push(vm, 0);
}
static int p_app_close(ef_vm *vm) {
    (void)vm;
    return EF_OK;
}
static int p_app_blit(ef_vm *vm) {
    int32_t addr, n;
    if (pop(vm, &n) || pop(vm, &addr)) {
        return EF_THROW;
    }
    (void)addr;
    (void)n;
    return EF_OK;
}
/* ( c-addr u depth -- ) and ( c-addr u -- ). No pixel window on this page. */
static int p_app_cblit(ef_vm *vm) {
    int32_t addr, n, depth;
    if (pop(vm, &depth) || pop(vm, &n) || pop(vm, &addr)) {
        return EF_THROW;
    }
    (void)addr;
    (void)n;
    (void)depth;
    return EF_OK;
}
static int p_app_pblit(ef_vm *vm) {
    int32_t addr, n;
    if (pop(vm, &n) || pop(vm, &addr)) {
        return EF_THROW;
    }
    (void)addr;
    (void)n;
    return EF_OK;
}
static int p_app_key_q(ef_vm *vm) { return push(vm, 0); }
static int p_app_key(ef_vm *vm) { return push(vm, 0); }
static int p_app_name(ef_vm *vm) {
    int32_t addr, n;
    if (pop(vm, &n) || pop(vm, &addr)) {
        return EF_THROW;
    }
    (void)addr;
    (void)n;
    return EF_OK;
}
static int p_app_tone(ef_vm *vm) {
    int32_t freq, dur;
    if (pop(vm, &dur) || pop(vm, &freq)) {
        return EF_THROW;
    }
    (void)freq;
    (void)dur;
    return EF_OK;
}
static int p_app_pump(ef_vm *vm) {
    (void)vm;
    return EF_OK;
}
static int p_app_mouse(ef_vm *vm) {
    if (push(vm, 0) || push(vm, 0)) {
        return EF_THROW;
    }
    return push(vm, 0);
}
/* ( -- cols rows flag ). Flag 0 so ?WINDOW-RESIZE takes the 2DROP path. */
static int p_app_size(ef_vm *vm) {
    if (push(vm, 0) || push(vm, 0)) {
        return EF_THROW;
    }
    return push(vm, 0);
}
static int p_key_q(ef_vm *vm) { return push(vm, 0); }
static int p_latest(ef_vm *vm) { return push(vm, uv(vm, UV_CURRENT)); }
static int p_redef(ef_vm *vm) { return push(vm, UV_WARN); }
static int p_block_file(ef_vm *vm) { return push(vm, UV_BLOCK_FILE); }
static int p_block_upd(ef_vm *vm) { return push(vm, UV_BLOCK_UPD); }
static int p_block_nr(ef_vm *vm) { return push(vm, UV_BLOCK_NR); }
static int p_scr(ef_vm *vm) { return push(vm, UV_SCR); }
static int p_block_buf(ef_vm *vm) { return push(vm, BLOCK_BUF_ADDR); }
static int p_ro(ef_vm *vm) { return push(vm, 1); }
static int p_rw(ef_vm *vm) { return push(vm, 3); }
static int p_bin(ef_vm *vm) { (void)vm; return EF_OK; }
static int p_load_enter(ef_vm *vm) { (void)vm; return EF_OK; }

static int load_quiet(ef_vm *vm, int32_t addr, int32_t *out) {
    if (addr < 0 || (addr & 3) || (size_t)addr + (size_t)CELL > vm->count) {
        *out = 0;
        return -1;
    }
    memcpy(out, vm->base + addr, CELL);
    return 0;
}

static void hook_set(ef_vm *vm, const char *name, int32_t v) {
    int32_t cfa = find_word(vm, name, (int)strlen(name), NULL);
    if (!cfa) {
        return;
    }
    (void)store_cell(vm, cfa + 2 * CELL, v);
}

static int32_t hook_get(ef_vm *vm, const char *name, int32_t fallback) {
    int32_t cfa = find_word(vm, name, (int)strlen(name), NULL);
    int32_t v = 0;
    if (!cfa || load_quiet(vm, cfa + 2 * CELL, &v)) {
        return fallback;
    }
    return v;
}

static void dbg_note_frame(ef_vm *vm, int32_t cfa) {
    int n = 0;
    /* The pause UI runs nested while dbg_busy. Its colon frames sit at the
       same return depth >R produces, and the next pause would treat the
       pause word as the word being stepped. */
    if (vm->dbg_busy) {
        return;
    }
    while (n < vm->dbg_frame_n && vm->dbg_fr_rsp[n] < vm->rsp) {
        n++;
    }
    vm->dbg_frame_n = n;
    if (vm->dbg_frame_n >= DBG_FRAME_MAX) {
        memmove(vm->dbg_fr_rsp, vm->dbg_fr_rsp + 1,
                (size_t)(DBG_FRAME_MAX - 1) * sizeof vm->dbg_fr_rsp[0]);
        memmove(vm->dbg_fr_cfa, vm->dbg_fr_cfa + 1,
                (size_t)(DBG_FRAME_MAX - 1) * sizeof vm->dbg_fr_cfa[0]);
        vm->dbg_frame_n = DBG_FRAME_MAX - 1;
    }
    vm->dbg_fr_rsp[vm->dbg_frame_n] = vm->rsp;
    vm->dbg_fr_cfa[vm->dbg_frame_n] = cfa;
    vm->dbg_frame_n++;
}

/* VIEW path and 1-based line for a stamped xt. 0 if the header has no file. */
static int view_stamp(ef_vm *vm, int32_t xt, char *path, int cap, int *line) {
    int32_t ffa1 = 0;
    int file_id;
    int i;
    int n;
    const char *found = NULL;
    if (!xt || (xt & 3) || xt < CELL || cap < 2) {
        return 0;
    }
    if (load_quiet(vm, xt - CELL, &ffa1)) {
        return 0;
    }
    *line = (int)(ffa1 & 0xffff);
    file_id = (int)(((uint32_t)ffa1 >> 16) & 0x1fff);
    if (!file_id || !*line) {
        return 0;
    }
    for (i = 0; i < vm->nviews; i++) {
        if (vm->views[i].id == file_id) {
            found = vm->views[i].path;
            break;
        }
    }
    if (!found || !found[0]) {
        return 0;
    }
    n = (int)strlen(found);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(path, found, (size_t)n);
    path[n] = 0;
    return 1;
}

/* Enclosing colon first, then the peeked token. Same order as kernel_debug_location. */
static void dbg_location(ef_vm *vm, char *path, int cap, int *line) {
    path[0] = 0;
    *line = 0;
    if (view_stamp(vm, vm->dbg_cfa, path, cap, line)) {
        return;
    }
    if (vm->dbg_xt != vm->dbg_cfa) {
        view_stamp(vm, vm->dbg_xt, path, cap, line);
    }
}

static void dbg_notify(ef_vm *vm, int active) {
    char path[256];
    char name[80];
    int line = 0;
    int off = 0;
    int len = 0;
    if (!vm->debug_fn) {
        return;
    }
    if (!active) {
        vm->dbg_host_off = 0;
        vm->dbg_host_len = 0;
        vm->debug_fn(vm->debug_ctx, "", 0, "", 0, 0);
        return;
    }
    dbg_location(vm, path, (int)sizeof path, &line);
    copy_name(vm, vm->dbg_xt, name, (int)sizeof name);
    off = vm->dbg_host_off;
    len = vm->dbg_host_len;
    if (len <= 0 || off < 0) {
        off = 0;
        len = 0;
    }
    /* One paint consumes the span, so the next pause cannot reuse it. */
    vm->dbg_host_off = 0;
    vm->dbg_host_len = 0;
    vm->debug_fn(vm->debug_ctx, path, line, name, off, len);
}

static void dbg_disarm(ef_vm *vm) {
    int was = vm->dbg_armed;
    vm->dbg_armed = 0;
    vm->dbg_floor_on = 0;
    vm->dbg_bp_go = 0;
    vm->dbg_runto_ip = 0;
    vm->dbg_break_asap = 0;
    vm->dbg_over = -1;
    vm->dbg_out = -1;
    if (was) {
        dbg_notify(vm, 0);
    }
}

static void dbg_sync_flags(ef_vm *vm) {
    hook_set(vm, "DBG-NEED-INTRO", vm->dbg_need_intro);
    hook_set(vm, "DBG-NEED-STACKS", vm->dbg_need_stacks);
    hook_set(vm, "DBG-SKIP-NL", vm->dbg_skip_nl);
    hook_set(vm, "DBG-MIDLINE", vm->dbg_midline);
    hook_set(vm, "DBG-HELP-SHOWN", vm->dbg_help_shown);
    hook_set(vm, "DBG-LINE-COL", vm->dbg_line_col);
}

static void dbg_read_flags(ef_vm *vm) {
    vm->dbg_need_intro = hook_get(vm, "DBG-NEED-INTRO", vm->dbg_need_intro);
    vm->dbg_need_stacks = hook_get(vm, "DBG-NEED-STACKS", vm->dbg_need_stacks);
    vm->dbg_skip_nl = hook_get(vm, "DBG-SKIP-NL", vm->dbg_skip_nl);
    vm->dbg_midline = hook_get(vm, "DBG-MIDLINE", vm->dbg_midline);
    vm->dbg_help_shown = hook_get(vm, "DBG-HELP-SHOWN", vm->dbg_help_shown);
    vm->dbg_line_col = hook_get(vm, "DBG-LINE-COL", vm->dbg_line_col);
}

static int dbg_bp_match(ef_vm *vm, int32_t xt) {
    int32_t table = find_word(vm, "BREAK-TABLE", 11, NULL);
    int32_t enables = find_word(vm, "BREAK-ENABLES", 13, NULL);
    int i;
    if (!table || !enables || !xt) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        int32_t slot = 0;
        int32_t en = 0;
        if (load_quiet(vm, table + 2 * CELL + i * CELL, &slot) ||
            load_quiet(vm, enables + 2 * CELL + i * CELL, &en)) {
            return 0;
        }
        if (slot && en && slot == xt) {
            return 1;
        }
    }
    return 0;
}

static void dbg_resolve_enclosing(ef_vm *vm) {
    int i;
    int best = -1;
    int32_t cfa = 0;
    for (i = 0; i < vm->dbg_frame_n; i++) {
        if (vm->dbg_fr_rsp[i] <= vm->rsp) {
            best = i;
        }
    }
    if (best >= 0) {
        cfa = vm->dbg_fr_cfa[best];
    }
    vm->dbg_cfa = cfa;
    vm->dbg_body = cfa ? (vm->dbg_ip - (cfa + 2 * CELL)) / CELL : 0;
}

static void dbg_print_name(ef_vm *vm) {
    char buf[80];
    copy_name(vm, vm->dbg_xt, buf, (int)sizeof buf);
    emit_str(vm, buf[0] ? buf : "?");
}

static int dbg_name_ok(ef_vm *vm, int32_t xt) {
    int32_t nfa;
    int n;
    int i;
    int32_t here;
    if (!xt || (xt & 3) || xt < DICT_ORIGIN) {
        return 0;
    }
    here = uv(vm, UV_DP);
    if (xt >= here || (size_t)xt + (size_t)CELL > vm->count) {
        return 0;
    }
    nfa = nfa_of(vm, xt);
    if (nfa < DICT_ORIGIN || nfa >= xt || (size_t)nfa >= vm->count) {
        return 0;
    }
    n = vm->base[nfa] & 0x7f;
    if (n < 1 || n > 63) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = vm->base[nfa + 1 + i];
        if (c < 32 || c > 126) {
            return 0;
        }
    }
    return 1;
}

static void dbg_print_inline(ef_vm *vm) {
    int32_t xt = vm->dbg_xt;
    int32_t inl = vm->dbg_inline;
    if (xt == vm->xt_lit) {
        emit_ch(vm, ' ');
        if (dbg_name_ok(vm, inl)) {
            char buf[80];
            copy_name(vm, inl, buf, (int)sizeof buf);
            emit_str(vm, buf[0] ? buf : "?");
        } else {
            emit_int(vm, inl, base_of(vm), 1);
        }
        return;
    }
    if (xt == vm->xt_branch || xt == vm->xt_zbranch || xt == vm->xt_ef_branch ||
        xt == vm->xt_ef_zbranch || xt == vm->xt_loop || xt == vm->xt_ploop) {
        int32_t cells = inl / CELL;
        emit_ch(vm, ' ');
        if (cells >= 0) {
            emit_ch(vm, '+');
        }
        emit_int(vm, cells, 10, 1);
        emit_str(vm, "CELLS");
    }
}

static int dbg_intoable(ef_vm *vm, int32_t xt) {
    int32_t code = 0;
    if (!xt || load_quiet(vm, xt, &code)) {
        return 0;
    }
    if (code == EF_DOCOL || code == EF_DODOES) {
        return 1;
    }
    return xt == vm->xt_execute || xt == vm->xt_catch;
}

/* Data: TOS on the right, at most 6, with ... when deeper.
   Return: nearest on the left, at most 4, clipped to the DBG-ON floor.
   While the pause UI is nested, show the debuggee return stack. */
static void dbg_print_sr(ef_vm *vm) {
    int top = vm->dbg_busy ? vm->dbg_pause_rsp : vm->rsp;
    int bot = 0;
    int depth;
    int show;
    int start;
    int i;
    if (vm->dbg_floor_on && vm->dbg_floor > 0 && vm->dbg_floor < top) {
        bot = vm->dbg_floor;
    }
    depth = top - bot;
    if (depth < 0) {
        depth = 0;
    }
    emit_ch(vm, 'S');
    emit_ch(vm, '(');
    emit_uint(vm, (uint32_t)vm->sp, base_of(vm));
    emit_str(vm, "): ");
    if (vm->sp > 6) {
        emit_str(vm, "... ");
    }
    show = vm->sp > 6 ? 6 : vm->sp;
    start = vm->sp - show;
    for (i = start; i < vm->sp; i++) {
        emit_int(vm, vm->stack[i], base_of(vm), 1);
    }
    emit_str(vm, "R(");
    emit_uint(vm, (uint32_t)depth, base_of(vm));
    emit_str(vm, "): ");
    show = depth > 4 ? 4 : depth;
    for (i = 0; i < show; i++) {
        int idx = top - 1 - i;
        if (idx < 0 || idx >= EF_RSTACK) {
            break;
        }
        emit_int(vm, vm->rstack[idx], base_of(vm), 1);
    }
    if (depth > 4) {
        emit_str(vm, "...");
    }
}

static void dbg_help(ef_vm *vm) {
    emit_str(vm,
        "[F6/Space/o/Return=over F7/i=into(I>>) F8=out Esc/q=abort Cmd-Shift-Y/g=go h=help]\n");
}

static void dbg_step_over(ef_vm *vm) {
    int32_t code = 0;
    dbg_read_flags(vm);
    vm->dbg_need_stacks = 1;
    vm->dbg_out = -1;
    vm->dbg_over = -1;
    if (vm->dbg_xt && !load_quiet(vm, vm->dbg_xt, &code) && code == EF_DOCOL) {
        vm->dbg_over = vm->dbg_pause_rsp;
    }
    dbg_sync_flags(vm);
}

static void dbg_step_into(ef_vm *vm) {
    dbg_read_flags(vm);
    vm->dbg_need_stacks = 1;
    vm->dbg_out = -1;
    vm->dbg_over = -1;
    dbg_sync_flags(vm);
}

static void dbg_step_out(ef_vm *vm) {
    dbg_read_flags(vm);
    vm->dbg_need_stacks = 1;
    vm->dbg_over = -1;
    vm->dbg_out = vm->dbg_pause_rsp;
    dbg_sync_flags(vm);
}

static void dbg_go(ef_vm *vm) {
    dbg_read_flags(vm);
    vm->dbg_need_stacks = 0;
    vm->dbg_need_intro = 0;
    vm->dbg_help_shown = 0;
    vm->dbg_over = -1;
    vm->dbg_out = -1;
    if (vm->dbg_midline) {
        emit_ch(vm, '\n');
    }
    vm->dbg_midline = 0;
    dbg_sync_flags(vm);
    if (!vm->dbg_bp_go && !vm->dbg_runto_ip) {
        dbg_disarm(vm);
    }
}

static void dbg_abort_session(ef_vm *vm) {
    dbg_read_flags(vm);
    vm->dbg_need_stacks = 0;
    vm->dbg_need_intro = 0;
    vm->dbg_help_shown = 0;
    if (vm->dbg_midline) {
        emit_ch(vm, '\n');
    }
    vm->dbg_midline = 0;
    emit_str(vm, "DEBUG aborted\n");
    vm->dbg_abort = -1;
    vm->dbg_runto_off = 0;
    vm->dbg_runto_status = 0;
    dbg_disarm(vm);
    dbg_sync_flags(vm);
}

static int dbg_mode_of(ef_vm *vm, int c) {
    dbg_read_flags(vm);
    if (vm->dbg_skip_nl) {
        if (c == 10 || c == 13) {
            return 0;
        }
        vm->dbg_skip_nl = 0;
        dbg_sync_flags(vm);
    }
    if (c == 'h' || c == 'H') {
        return 7;
    }
    if (c == 135) {
        return 8;
    }
    if (c == 134 || c == 'g' || c == 'G') {
        return 4;
    }
    if (c == 27 || c == 'q' || c == 'Q') {
        return 5;
    }
    if (c == ' ' || c == 13 || c == 'o' || c == 'O') {
        return 1;
    }
    if (c == 'i' || c == 'I') {
        return 2;
    }
    if (c == 0 || c == 3 || c == 7) {
        return 6;
    }
    if (((unsigned)c & 0xff000000u) == 0x02000000u) {
        int k = c & 0xffffff;
        if (k == 16) {
            return 1;
        }
        if (k == 17) {
            return 2;
        }
        if (k == 18) {
            return 3;
        }
    }
    return 0;
}

static void dbg_apply(ef_vm *vm, int mode) {
    if (mode == 1) {
        dbg_step_over(vm);
    } else if (mode == 2) {
        dbg_step_into(vm);
    } else if (mode == 3) {
        dbg_step_out(vm);
    } else if (mode == 4) {
        dbg_go(vm);
    } else if (mode == 5) {
        dbg_abort_session(vm);
    }
}

static int dbg_fallback(ef_vm *vm) {
    dbg_read_flags(vm);
    if (vm->dbg_need_intro) {
        vm->dbg_need_intro = 0;
        if (vm->dbg_midline) {
            vm->dbg_midline = 0;
            emit_ch(vm, '\n');
        }
        dbg_sync_flags(vm);
        dbg_help(vm);
        dbg_print_sr(vm);
        emit_ch(vm, '\n');
    } else if (vm->dbg_need_stacks) {
        vm->dbg_need_stacks = 0;
        if (vm->dbg_help_shown) {
            vm->dbg_help_shown = 0;
            emit_ch(vm, '\n');
        }
        dbg_sync_flags(vm);
        dbg_print_sr(vm);
        emit_ch(vm, '\n');
    }
    emit_str(vm, dbg_intoable(vm, vm->dbg_xt) ? "I>> " : ">> ");
    dbg_print_name(vm);
    dbg_print_inline(vm);
    emit_ch(vm, '\n');
    dbg_notify(vm, 1);
    for (;;) {
        int c;
        int mode;
        if (!vm->key) {
            dbg_abort_session(vm);
            return EF_OK;
        }
        c = vm->key(vm->key_ctx);
        if (c < 0) {
            dbg_abort_session(vm);
            return EF_OK;
        }
        mode = dbg_mode_of(vm, c);
        if (mode == 0 || mode == 6 || mode == 8) {
            continue;
        }
        if (mode == 7) {
            if (!vm->dbg_help_shown) {
                dbg_help(vm);
                vm->dbg_help_shown = -1;
                dbg_sync_flags(vm);
            }
            continue;
        }
        dbg_apply(vm, mode);
        return EF_OK;
    }
}

static int dbg_want_pause(ef_vm *vm, int32_t xt) {
    if (vm->dbg_busy || !vm->dbg_armed) {
        return 0;
    }
    if (vm->dbg_floor_on) {
        if (vm->rsp == vm->dbg_floor) {
            return 0;
        }
        if (vm->rsp < vm->dbg_floor) {
            dbg_disarm(vm);
            return 0;
        }
    }
    if (vm->dbg_out >= 0 && vm->rsp >= vm->dbg_out) {
        return 0;
    }
    if (vm->dbg_over >= 0 && vm->rsp > vm->dbg_over) {
        return 0;
    }
    if (vm->dbg_runto_ip) {
        if (vm->ip == vm->dbg_runto_ip) {
            vm->dbg_runto_ip = 0;
            vm->dbg_bp_go = 0;
        } else if (vm->dbg_bp_go) {
            if (!dbg_bp_match(vm, xt)) {
                return 0;
            }
            vm->dbg_bp_go = 0;
            vm->dbg_runto_ip = 0;
        } else {
            return 0;
        }
    } else if (vm->dbg_bp_go) {
        if (!dbg_bp_match(vm, xt)) {
            return 0;
        }
        vm->dbg_bp_go = 0;
    }
    if (xt == vm->xt_dbg_off) {
        return 0;
    }
    vm->dbg_over = -1;
    vm->dbg_out = -1;
    return 1;
}

static void dbg_snapshot(ef_vm *vm, int32_t xt) {
    int32_t inl = 0;
    vm->dbg_pause_rsp = vm->rsp;
    vm->dbg_xt = xt;
    vm->dbg_ip = vm->ip;
    if (load_quiet(vm, vm->ip + CELL, &inl)) {
        inl = 0;
    }
    vm->dbg_inline = inl;
    dbg_resolve_enclosing(vm);
}

static int dbg_consider(ef_vm *vm, int32_t xt) {
    int32_t pause_xt;
    int rc;
    if (!dbg_want_pause(vm, xt)) {
        return EF_OK;
    }
    dbg_snapshot(vm, xt);
    vm->dbg_busy = 1;
    pause_xt = hook_get(vm, "DBG-PAUSE-XT", 0);
    if (pause_xt) {
        rc = exec_one(vm, pause_xt, 0);
    } else {
        rc = dbg_fallback(vm);
    }
    vm->dbg_busy = 0;
    if (rc) {
        return rc;
    }
    if (vm->dbg_abort) {
        vm->dbg_abort = 0;
        return fail(vm, -1, NULL);
    }
    return EF_OK;
}

static int p_dbg_on(ef_vm *vm) {
    vm->dbg_floor = vm->rsp;
    vm->dbg_floor_on = 1;
    vm->dbg_armed = 1;
    vm->dbg_bp_go = 0;
    vm->dbg_runto_ip = 0;
    vm->dbg_over = -1;
    vm->dbg_out = -1;
    vm->dbg_abort = 0;
    vm->dbg_break_asap = 0;
    vm->dbg_frame_n = 0;
    vm->dbg_xt = 0;
    vm->dbg_ip = 0;
    vm->dbg_cfa = 0;
    vm->dbg_body = 0;
    vm->dbg_need_intro = 1;
    vm->dbg_need_stacks = 0;
    vm->dbg_skip_nl = 1;
    vm->dbg_midline = 0;
    vm->dbg_help_shown = 0;
    vm->dbg_line_col = 0;
    dbg_sync_flags(vm);
    return EF_OK;
}

static int p_dbg_off(ef_vm *vm) {
    vm->dbg_need_intro = 0;
    vm->dbg_need_stacks = 0;
    vm->dbg_skip_nl = 0;
    vm->dbg_midline = 0;
    vm->dbg_help_shown = 0;
    vm->dbg_line_col = 0;
    vm->dbg_abort = 0;
    dbg_disarm(vm);
    dbg_sync_flags(vm);
    return EF_OK;
}

static int p_dbg_break_min(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    vm->dbg_break_min = a;
    return EF_OK;
}

static int p_dbg_xt_at(ef_vm *vm) { return push(vm, vm->dbg_xt); }
static int p_dbg_ip_at(ef_vm *vm) { return push(vm, vm->dbg_ip); }
static int p_dbg_cfa_at(ef_vm *vm) { return push(vm, vm->dbg_cfa); }
static int p_dbg_body_n(ef_vm *vm) { return push(vm, vm->dbg_body); }
static int p_dbg_dot_sr(ef_vm *vm) { dbg_print_sr(vm); return EF_OK; }
static int p_dbg_print_inline(ef_vm *vm) { dbg_print_inline(vm); return EF_OK; }
static int p_dbg_print_name(ef_vm *vm) { dbg_print_name(vm); return EF_OK; }
static int p_dbg_intoable(ef_vm *vm) {
    return push(vm, dbg_intoable(vm, vm->dbg_xt) ? -1 : 0);
}
static int p_dbg_host_paint(ef_vm *vm) {
    dbg_notify(vm, 1);
    return EF_OK;
}
static int p_dbg_view_update(ef_vm *vm) { (void)vm; return EF_OK; }

static int p_dbg_host_span(ef_vm *vm) {
    int32_t len;
    int32_t off;
    if (pop(vm, &len) || pop(vm, &off)) {
        return EF_THROW;
    }
    vm->dbg_host_off = off;
    vm->dbg_host_len = len;
    return EF_OK;
}

static int p_dbg_host_span_at(ef_vm *vm) {
    return push2(vm, vm->dbg_host_off, vm->dbg_host_len);
}

static int p_dbg_step_over(ef_vm *vm) { dbg_step_over(vm); return EF_OK; }
static int p_dbg_step_into(ef_vm *vm) { dbg_step_into(vm); return EF_OK; }
static int p_dbg_step_out(ef_vm *vm) { dbg_step_out(vm); return EF_OK; }
static int p_dbg_go(ef_vm *vm) { dbg_go(vm); return EF_OK; }
static int p_dbg_abort(ef_vm *vm) { dbg_abort_session(vm); return EF_OK; }
static int p_bp_go(ef_vm *vm) { vm->dbg_bp_go = 1; return EF_OK; }

static int p_runto_ip(ef_vm *vm) {
    int32_t a;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    vm->dbg_runto_ip = a;
    return EF_OK;
}

static int p_runto_off_fetch(ef_vm *vm) { return push(vm, vm->dbg_runto_off); }

static int p_runto_status(ef_vm *vm) {
    int32_t n;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    vm->dbg_runto_status = n;
    return EF_OK;
}

static int p_fromlib(ef_vm *vm) { vm->dbg_fromlib = 1; return EF_OK; }
static int p_fromlib_q(ef_vm *vm) { return push(vm, vm->dbg_fromlib ? -1 : 0); }
static int p_fromlib_off(ef_vm *vm) { vm->dbg_fromlib = 0; return EF_OK; }

/* One-shot prefix for the next INCLUDED. Interpret S" keeps the blank after the quote. */
static int p_resolve_key(ef_vm *vm) {
    char raw[EF_PARSE_MAX];
    char path[EF_PARSE_MAX];
    const char *s;
    const char *prefix = "Resources/Library/";
    size_t plen;
    int n;
    if (!vm->dbg_fromlib) {
        return EF_OK;
    }
    vm->dbg_fromlib = 0;
    if (pop_text(vm, raw, (int)sizeof raw)) {
        return EF_THROW;
    }
    s = raw;
    n = (int)strlen(s);
    if (n > 0 && s[0] == ' ') {
        s++;
        n--;
    }
    plen = strlen(prefix);
    if (n >= (int)plen && strncmp(s, prefix, plen) == 0) {
        return plant_parse(vm, s, n);
    }
    if ((size_t)n + plen >= sizeof path) {
        return fail(vm, -16, "path too long\n");
    }
    memcpy(path, prefix, plen);
    if (n > 0) {
        memcpy(path + plen, s, (size_t)n);
    }
    path[plen + (size_t)n] = 0;
    return plant_parse(vm, path, n + (int)plen);
}
static int p_boot0(ef_vm *vm) { return push(vm, 0); }

static int p_ms_at(ef_vm *vm) {
    struct timeval tv;
    int64_t ms;
    gettimeofday(&tv, NULL);
    ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
    return push(vm, (int32_t)ms);
}

static int p_um_star(ef_vm *vm) {
    int32_t a, b;
    uint64_t p;
    if (pop(vm, &b) || pop(vm, &a)) {
        return EF_THROW;
    }
    p = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    return push_d(vm, p);
}

static int p_m_star(ef_vm *vm) {
    int32_t a, b;
    int64_t p;
    if (pop(vm, &b) || pop(vm, &a)) {
        return EF_THROW;
    }
    p = (int64_t)a * (int64_t)b;
    return push_d(vm, (uint64_t)p);
}

static int p_um_mod(ef_vm *vm) {
    int32_t u, hi, lo;
    uint64_t ud, q;
    uint32_t div;
    if (pop(vm, &u) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    if (u == 0) {
        return fail(vm, -9, "division by zero\n");
    }
    div = (uint32_t)u;
    ud = pack_d(lo, hi);
    q = ud / div;
    if (push(vm, (int32_t)(uint32_t)(ud % div))) {
        return EF_THROW;
    }
    return push(vm, (int32_t)(uint32_t)q);
}

static int p_sm_rem(ef_vm *vm) {
    int32_t n, hi, lo, quot, rem;
    int64_t d;
    if (pop(vm, &n) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    if (n == 0) {
        return fail(vm, -9, "division by zero\n");
    }
    d = (int64_t)pack_d(lo, hi);
    if (n == -1 && d == INT64_MIN) {
        rem = 0;
        quot = (int32_t)0x80000000;
    } else {
        quot = (int32_t)(d / n);
        rem = (int32_t)(d % n);
    }
    return push2(vm, rem, quot);
}

static int p_d_plus(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    return push_d(vm, pack_d(lo1, hi1) + pack_d(lo2, hi2));
}

static int p_d_negate(ef_vm *vm) {
    int32_t hi, lo;
    uint64_t d;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    d = pack_d(lo, hi);
    return push_d(vm, (uint64_t)(0 - d));
}

static int p_d_abs(ef_vm *vm) {
    int32_t hi, lo;
    uint64_t d;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    d = pack_d(lo, hi);
    if (hi < 0) {
        d = (uint64_t)(0 - d);
    }
    return push_d(vm, d);
}

static int p_d_zero_lt(ef_vm *vm) {
    int32_t hi, lo;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    (void)lo;
    return push(vm, hi < 0 ? -1 : 0);
}

static int p_two_fetch(ef_vm *vm) {
    int32_t a, lo, hi;
    if (pop(vm, &a) || load_cell(vm, a, &lo) || load_cell(vm, a + CELL, &hi)) {
        return EF_THROW;
    }
    return push2(vm, lo, hi);
}

static int p_two_store(ef_vm *vm) {
    int32_t a, lo, hi;
    if (pop(vm, &a) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    if (store_cell(vm, a, lo) || store_cell(vm, a + CELL, hi)) {
        return EF_THROW;
    }
    return EF_OK;
}

/* One source level: >IN and a count. RESTORE-INPUT returns 0 on success. */
static int p_save_input(ef_vm *vm) {
    return push2(vm, uv(vm, UV_IN), 1);
}

static int p_restore_input(ef_vm *vm) {
    int32_t n, in = 0;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    if (n != 1 || pop(vm, &in) || in < 0 || in > vm->src_len) {
        return push(vm, -1);
    }
    uv_set(vm, UV_IN, in);
    return push(vm, 0);
}

static int p_two_to_r(ef_vm *vm) {
    int32_t x1, x2;
    if (pop(vm, &x2) || pop(vm, &x1)) {
        return EF_THROW;
    }
    if (rpush(vm, x1) || rpush(vm, x2)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_two_r_from(ef_vm *vm) {
    int32_t x1, x2;
    if (rpop(vm, &x2) || rpop(vm, &x1)) {
        return EF_THROW;
    }
    return push2(vm, x1, x2);
}

/* Copy the top two return cells. 2>R leaves x2 on top, so the copy is x1 x2. */
static int p_two_r_fetch(ef_vm *vm) {
    int32_t x1, x2;
    if (vm->rsp < 2) {
        return fail(vm, -5, "return stack underflow\n");
    }
    x1 = vm->rstack[vm->rsp - 2];
    x2 = vm->rstack[vm->rsp - 1];
    return push2(vm, x1, x2);
}

static int p_within(ef_vm *vm) {
    int32_t n1, n2, n3;
    if (pop(vm, &n3) || pop(vm, &n2) || pop(vm, &n1)) {
        return EF_THROW;
    }
    return push(vm, (uint32_t)(n1 - n2) < (uint32_t)(n3 - n2) ? -1 : 0);
}

static int p_compare(ef_vm *vm) {
    int32_t a1, u1, a2, u2;
    int n, cmp;
    if (pop(vm, &u2) || pop(vm, &a2) || pop(vm, &u1) || pop(vm, &a1)) {
        return EF_THROW;
    }
    if (!span_ok(vm, a1, u1) || !span_ok(vm, a2, u2)) {
        return fail(vm, -16, "bad address\n");
    }
    n = u1 < u2 ? u1 : u2;
    cmp = n ? memcmp(vm->base + a1, vm->base + a2, (size_t)n) : 0;
    if (cmp < 0) {
        return push(vm, -1);
    }
    if (cmp > 0) {
        return push(vm, 1);
    }
    if (u1 == u2) {
        return push(vm, 0);
    }
    return push(vm, u1 < u2 ? -1 : 1);
}

static int p_parse_name(ef_vm *vm) {
    int in = skip_white_src(vm);
    int len = vm->src_len;
    uint8_t *s = vm->base + SRC_ADDR;
    int start, n;
    char buf[EF_PARSE_MAX];
    if (in >= len) {
        return push2(vm, 0, 0);
    }
    start = in;
    while (in < len && !is_white(s[in])) {
        in++;
    }
    n = in - start;
    uv_set(vm, UV_IN, in);
    if (n >= EF_PARSE_MAX) {
        n = EF_PARSE_MAX - 1;
    }
    memcpy(buf, s + start, (size_t)n);
    buf[n] = 0;
    return plant_parse(vm, buf, n);
}

static int p_parse_filespec(ef_vm *vm) {
    int in = skip_white_src(vm);
    int len = vm->src_len;
    uint8_t *s = vm->base + SRC_ADDR;
    char buf[EF_PARSE_MAX];
    int n;
    if (in >= len) {
        return push2(vm, 0, 0);
    }
    if (s[in] != '"') {
        return p_parse_name(vm);
    }
    uv_set(vm, UV_IN, in + 1);
    if (parse_until(vm, '"', buf, (int)sizeof buf, &n)) {
        return EF_THROW;
    }
    return plant_parse(vm, buf, n);
}

static int p_also(ef_vm *vm) {
    int n = uv(vm, UV_ORDERN);
    int i;
    int32_t top;
    if (n <= 0 || n >= EF_ORDER_MAX) {
        return EF_OK;
    }
    top = uv(vm, UV_ORDER);
    for (i = n - 1; i >= 0; i--) {
        uv_set(vm, UV_ORDER + (i + 1) * CELL, uv(vm, UV_ORDER + i * CELL));
    }
    uv_set(vm, UV_ORDER, top);
    uv_set(vm, UV_ORDERN, n + 1);
    return EF_OK;
}

/* Drop the first-searched wordlist. One entry is left alone. */
static int p_previous(ef_vm *vm) {
    int n = uv(vm, UV_ORDERN);
    int i;
    if (n <= 1) {
        return EF_OK;
    }
    for (i = 0; i < n - 1; i++) {
        uv_set(vm, UV_ORDER + i * CELL, uv(vm, UV_ORDER + (i + 1) * CELL));
    }
    uv_set(vm, UV_ORDERN, n - 1);
    return EF_OK;
}

static int p_to(ef_vm *vm) {
    char name[EF_WORD_MAX];
    int n;
    int imm = 0;
    int32_t cfa, body, x;
    if (!next_name(vm, name, (int)sizeof name, &n)) {
        return fail(vm, -13, "undefined:\n");
    }
    if (uv(vm, UV_STATE)) {
        int idx = local_lookup(vm, name, n);
        if (idx >= 0) {
            return local_compile_op(vm, idx, vm->xt_lstore);
        }
    }
    cfa = find_word(vm, name, n, &imm);
    if (!cfa) {
        emit_str(vm, "undefined: ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        return fail(vm, -13, NULL);
    }
    body = cfa + 2 * CELL;
    if (uv(vm, UV_STATE)) {
        if (comma(vm, vm->xt_lit) || comma(vm, body) || comma(vm, vm->xt_store)) {
            return EF_THROW;
        }
        return EF_OK;
    }
    if (pop(vm, &x) || store_cell(vm, body, x)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_catch(ef_vm *vm) {
    int32_t xt;
    int saved_sp, saved_rsp, saved_inner, saved_frames;
    int32_t saved_ip;
    int rc;
    if (pop(vm, &xt)) {
        return EF_THROW;
    }
    saved_sp = vm->sp;
    saved_rsp = vm->rsp;
    saved_ip = vm->ip;
    saved_inner = vm->in_inner;
    saved_frames = vm->frame_n;
    rc = exec_one(vm, xt, 0);
    if (rc != EF_OK || vm->throw_code) {
        int code = vm->throw_code ? vm->throw_code : -1;
        vm->sp = saved_sp;
        vm->rsp = saved_rsp;
        vm->ip = saved_ip;
        vm->in_inner = saved_inner;
        vm->frame_n = saved_frames;
        vm->throw_code = 0;
        return push(vm, code);
    }
    return push(vm, 0);
}

static int p_forget(ef_vm *vm) {
    char name[EF_WORD_MAX];
    int n;
    int imm = 0;
    int32_t cfa, cut;
    int i, j;
    if (!next_name(vm, name, (int)sizeof name, &n)) {
        return fail(vm, -13, "undefined:\n");
    }
    cfa = find_word(vm, name, n, &imm);
    if (!cfa) {
        emit_str(vm, "undefined: ");
        emit_str(vm, name);
        emit_ch(vm, '\n');
        return fail(vm, -13, NULL);
    }
    cut = hfa_of(vm, cfa);
    uv_set(vm, UV_DP, cut);
    prune_wid(vm, vm->forth_wid, cut);
    j = 0;
    for (i = 0; i < vm->nwid; i++) {
        if (vm->wids[i] < cut) {
            prune_wid(vm, vm->wids[i], cut);
            vm->wids[j++] = vm->wids[i];
        }
    }
    vm->nwid = j;
    prune_order(vm, cut);
    if (uv(vm, UV_CURRENT) >= cut) {
        uv_set(vm, UV_CURRENT, vm->forth_wid);
    }
    if (uv(vm, UV_LAST) >= cut) {
        uv_set(vm, UV_LAST, 0);
    }
    return EF_OK;
}

static int p_traverse(ef_vm *vm) {
    int32_t xt, wid, cfa;
    int th;
    if (pop(vm, &wid) || pop(vm, &xt)) {
        return EF_THROW;
    }
    if (wid < 0 || (size_t)wid + (size_t)EF_THREADS * CELL > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    for (th = 0; th < EF_THREADS; th++) {
        int guard = 0;
        cfa = uv(vm, wid + th * CELL);
        while (cfa && guard++ < 1000000) {
            int32_t next = uv(vm, cfa - 3 * CELL);
            int32_t flag;
            if (!smudged(vm, cfa)) {
                if (push(vm, cfa) || exec_one(vm, xt, 0) || pop(vm, &flag)) {
                    return EF_THROW;
                }
                if (flag == 0) {
                    return EF_OK;
                }
            }
            cfa = next;
        }
    }
    return EF_OK;
}

static int p_wordlists(ef_vm *vm) {
    int i;
    if (ensure(vm, WL_ADDR + vm->nwid * CELL)) {
        return EF_THROW;
    }
    for (i = 0; i < vm->nwid; i++) {
        if (store_cell(vm, WL_ADDR + i * CELL, vm->wids[i])) {
            return EF_THROW;
        }
    }
    return push2(vm, WL_ADDR, vm->nwid);
}

static int p_register_wid(ef_vm *vm) {
    int32_t wid;
    int i;
    if (pop(vm, &wid)) {
        return EF_THROW;
    }
    for (i = 0; i < vm->nwid; i++) {
        if (vm->wids[i] == wid) {
            return EF_OK;
        }
    }
    if (vm->nwid < EF_WL) {
        vm->wids[vm->nwid++] = wid;
    }
    return EF_OK;
}

static int p_view_path(ef_vm *vm) {
    int32_t id;
    const char *path = NULL;
    int i, n;
    if (pop(vm, &id)) {
        return EF_THROW;
    }
    if (ensure(vm, VIEWPATH_ADDR + EF_VIEWPATH)) {
        return EF_THROW;
    }
    for (i = 0; i < vm->nviews; i++) {
        if (vm->views[i].id == id) {
            path = vm->views[i].path;
            break;
        }
    }
    if (!path) {
        vm->base[VIEWPATH_ADDR] = 0;
        return push2(vm, VIEWPATH_ADDR, 0);
    }
    n = (int)strlen(path);
    if (n > EF_VIEWPATH - 1) {
        n = EF_VIEWPATH - 1;
    }
    memcpy(vm->base + VIEWPATH_ADDR, path, (size_t)n);
    vm->base[VIEWPATH_ADDR + n] = 0;
    return push2(vm, VIEWPATH_ADDR, n);
}

static int p_local_init(ef_vm *vm) {
    int32_t rev, ninit, nlocals;
    int i;
    if (pop(vm, &rev) || pop(vm, &ninit) || pop(vm, &nlocals)) {
        return EF_THROW;
    }
    if (nlocals < 0) {
        nlocals = 0;
    }
    if (nlocals > EF_LOCAL_MAX) {
        nlocals = EF_LOCAL_MAX;
    }
    if (ninit < 0) {
        ninit = 0;
    }
    if (ninit > nlocals) {
        ninit = nlocals;
    }
    if (vm->frame_n >= EF_LOCAL_FRAMES) {
        for (i = 0; i < ninit; i++) {
            int32_t junk;
            if (pop(vm, &junk)) {
                return EF_THROW;
            }
        }
        return EF_OK;
    }
    for (i = 0; i < nlocals; i++) {
        vm->local_frame[vm->frame_n][i] = 0;
    }
    if (rev) {
        for (i = ninit; i > 0; i--) {
            int32_t v;
            if (pop(vm, &v)) {
                return EF_THROW;
            }
            vm->local_frame[vm->frame_n][i - 1] = v;
        }
    } else {
        for (i = 0; i < ninit; i++) {
            int32_t v;
            if (pop(vm, &v)) {
                return EF_THROW;
            }
            vm->local_frame[vm->frame_n][i] = v;
        }
    }
    vm->frame_rsp[vm->frame_n] = vm->rsp;
    vm->frame_nloc[vm->frame_n] = nlocals;
    vm->frame_n++;
    return EF_OK;
}

static int p_local_at(ef_vm *vm) {
    int32_t idx;
    int top;
    if (pop(vm, &idx)) {
        return EF_THROW;
    }
    top = vm->frame_n - 1;
    if (top < 0 || idx < 0 || idx >= vm->frame_nloc[top]) {
        return push(vm, 0);
    }
    return push(vm, vm->local_frame[top][idx]);
}

static int p_local_store(ef_vm *vm) {
    int32_t idx, v;
    int top;
    if (pop(vm, &idx) || pop(vm, &v)) {
        return EF_THROW;
    }
    top = vm->frame_n - 1;
    if (top >= 0 && idx >= 0 && idx < vm->frame_nloc[top]) {
        vm->local_frame[top][idx] = v;
    }
    return EF_OK;
}

static int p_local(ef_vm *vm) {
    int32_t addr, u;
    char name[32];
    int n, i;
    if (uv(vm, UV_STATE) == 0) {
        return EF_OK;
    }
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u == 0) {
        if (!vm->local_declaring) {
            local_reset(vm);
        }
        if (local_compile_init(vm)) {
            return EF_THROW;
        }
        vm->local_declaring = 0;
        return EF_OK;
    }
    if (u < 0 || !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    if (!vm->local_declaring) {
        local_reset(vm);
        vm->local_declaring = 1;
    }
    n = u > 31 ? 31 : u;
    for (i = 0; i < n; i++) {
        name[i] = (char)vm->base[addr + i];
    }
    name[n] = 0;
    local_add_name(vm, name, n);
    vm->local_init_n++;
    return EF_OK;
}

static int p_brace(ef_vm *vm) {
    char name[EF_WORD_MAX];
    int n;
    int phase = 0;
    if (compile_only(vm, "{:")) {
        return EF_THROW;
    }
    local_reset(vm);
    vm->local_reverse = 1;
    while (next_name(vm, name, (int)sizeof name, &n)) {
        if (n == 2 && name[0] == ':' && name[1] == '}') {
            break;
        }
        if (n == 1 && name[0] == '|') {
            phase = 1;
            continue;
        }
        if (n == 2 && name[0] == '-' && name[1] == '-') {
            phase = 2;
            continue;
        }
        if (phase == 2) {
            continue;
        }
        local_add_name(vm, name, n);
        if (phase == 0) {
            vm->local_init_n++;
        }
    }
    return local_compile_init(vm);
}

static int p_search_wl(ef_vm *vm) {
    int32_t wid, u, addr;
    char name[128];
    unsigned th;
    int32_t cfa;
    if (pop(vm, &wid) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (wid <= 0 || u <= 0 || u > 127) {
        return push(vm, 0);
    }
    if (!span_ok(vm, addr, u) ||
        (size_t)wid + (size_t)EF_THREADS * CELL > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    memcpy(name, vm->base + addr, (size_t)u);
    name[u] = 0;
    th = hash_name(name, u);
    cfa = uv(vm, wid + (int)th * CELL);
    while (cfa) {
        if (!smudged(vm, cfa) && name_eq(vm->base + nfa_of(vm, cfa), name, u)) {
            uint32_t f1 = (uint32_t)uv(vm, cfa - CELL);
            int flag = (f1 & FFA1_IMM) ? 1 : -1;
            if (push(vm, cfa)) {
                return EF_THROW;
            }
            return push(vm, flag);
        }
        cfa = uv(vm, cfa - 3 * CELL);
    }
    return push(vm, 0);
}

static int p_fop(ef_vm *vm) {
    int32_t op;
    if (pop(vm, &op)) {
        return EF_THROW;
    }
    (void)op;
    return fail(vm, -21, "floats are not in this build\n");
}

static int p_flit(ef_vm *vm) {
    return fail(vm, -21, "floats are not in this build\n");
}

static int p_textedit(ef_vm *vm) {
    emit_str(vm, "TEXTEDIT is not on this page\n");
    return EF_OK;
}

static int p_open_file(ef_vm *vm) {
    int32_t fam;
    char name[512];
    char resolved[512];
    char *owned = NULL;
    int len = 0;
    int slot;
    if (pop(vm, &fam)) {
        return EF_THROW;
    }
    (void)fam;
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    slot = -1;
    if (resolve_file(vm, name, resolved, sizeof resolved, &owned, &len)) {
        slot = alloc_slot(vm);
    }
    if (slot < 0) {
        free(owned);
        return push2(vm, 0, 1);
    }
    vm->files[slot].used = 1;
    vm->files[slot].data = owned;
    vm->files[slot].len = len;
    vm->files[slot].cap = owned ? len + 1 : 0;
    vm->files[slot].pos = 0;
    snprintf(vm->files[slot].path, sizeof vm->files[slot].path, "%s", resolved);
    snprintf(vm->last_open, sizeof vm->last_open, "%s", resolved);
    return push2(vm, slot + 1, 0);
}

static int p_create_file(ef_vm *vm) {
    int32_t fam;
    char name[512];
    int slot;
    if (pop(vm, &fam)) {
        return EF_THROW;
    }
    (void)fam;
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    slashes(name);
    slot = path_ok(name) ? alloc_slot(vm) : -1;
    if (slot < 0) {
        return push2(vm, 0, 1);
    }
    vm->files[slot].used = 1;
    vm->files[slot].data = NULL;
    vm->files[slot].len = 0;
    vm->files[slot].cap = 0;
    vm->files[slot].pos = 0;
    snprintf(vm->files[slot].path, sizeof vm->files[slot].path, "%s", name);
    snprintf(vm->last_open, sizeof vm->last_open, "%s", name);
    return push2(vm, slot + 1, 0);
}

static int p_close_file(ef_vm *vm) {
    int32_t fid;
    int s;
    if (pop(vm, &fid)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0) {
        return push(vm, 1);
    }
    free(vm->files[s].data);
    memset(&vm->files[s], 0, sizeof vm->files[s]);
    return push(vm, 0);
}

static int p_file_size(ef_vm *vm) {
    int32_t fid;
    int s;
    if (pop(vm, &fid)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0) {
        if (push(vm, 0) || push(vm, 0)) {
            return EF_THROW;
        }
        return push(vm, 1);
    }
    if (push(vm, vm->files[s].len) || push(vm, 0)) {
        return EF_THROW;
    }
    return push(vm, 0);
}

static int p_read_file(ef_vm *vm) {
    int32_t fid, u, addr;
    int s, n;
    if (pop(vm, &fid) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0 || u < 0) {
        return push2(vm, 0, 1);
    }
    n = vm->files[s].len - vm->files[s].pos;
    if (n < 0) {
        n = 0;
    }
    if (n > u) {
        n = u;
    }
    if (n > 0) {
        if (addr < 0 || ensure(vm, addr + n)) {
            return EF_THROW;
        }
        if (!vm->files[s].data) {
            return push2(vm, 0, 1);
        }
        memcpy(vm->base + addr, vm->files[s].data + vm->files[s].pos, (size_t)n);
        vm->files[s].pos += n;
    }
    return push2(vm, n, 0);
}

static int p_write_file(ef_vm *vm) {
    int32_t fid, u, addr;
    int s, need;
    if (pop(vm, &fid) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0 || u < 0 || !span_ok(vm, addr, u)) {
        return push(vm, 1);
    }
    need = vm->files[s].pos + u;
    if (file_grow(vm, s, need)) {
        return push(vm, 1);
    }
    if (u > 0) {
        memcpy(vm->files[s].data + vm->files[s].pos, vm->base + addr, (size_t)u);
        vm->files[s].pos += u;
        if (vm->files[s].pos > vm->files[s].len) {
            vm->files[s].len = vm->files[s].pos;
        }
    }
    return push(vm, 0);
}

static int p_reposition(ef_vm *vm) {
    int32_t fid, hi, lo;
    int s;
    if (pop(vm, &fid) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    (void)hi;
    s = slot_of(vm, fid);
    if (s < 0 || lo < 0 || lo > EF_SRC_MAX) {
        return push(vm, 1);
    }
    vm->files[s].pos = lo;
    return push(vm, 0);
}

static int p_flush_file(ef_vm *vm) {
    int32_t fid;
    if (pop(vm, &fid)) {
        return EF_THROW;
    }
    return push(vm, slot_of(vm, fid) < 0 ? 1 : 0);
}

static int p_allocate(ef_vm *vm) {
    int32_t u, header, payload, next, take;
    if (pop(vm, &u)) {
        return EF_THROW;
    }
    if (u < 0) {
        return push2(vm, 0, 1);
    }
    header = (vm->heap_at + 3) & ~3;
    payload = header + CELL;
    take = (u + 3) & ~3;
    next = payload + take;
    if (ensure(vm, next)) {
        return EF_THROW;
    }
    if (store_cell(vm, header, u)) {
        return EF_THROW;
    }
    vm->heap_last = payload;
    vm->heap_at = next;
    return push2(vm, payload, 0);
}

static int p_free(ef_vm *vm) {
    int32_t addr;
    if (pop(vm, &addr)) {
        return EF_THROW;
    }
    if (addr == vm->heap_last && addr >= CELL) {
        vm->heap_at = addr - CELL;
        vm->heap_last = 0;
    }
    return push(vm, 0);
}

static int p_line_source(ef_vm *vm) {
    int32_t addr, u;
    char *copy = NULL;
    char saved_name[256];
    int saved_file;
    int rc;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u < 0 || u > EF_SRC_MAX - 1 || !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    copy = malloc((size_t)u + 1);
    if (!copy) {
        return fail(vm, -8, "out of memory\n");
    }
    if (u > 0) {
        memcpy(copy, vm->base + addr, (size_t)u);
    }
    copy[u] = 0;
    saved_file = vm->file_id;
    snprintf(saved_name, sizeof saved_name, "%s", vm->src_name);
    if (vm->last_open[0]) {
        snprintf(vm->src_name, sizeof vm->src_name, "%s", vm->last_open);
    }
    vm->file_id = vm->next_file++;
    if (vm->next_file > 0x1fff) {
        vm->next_file = 2;
    }
    remember_view(vm, vm->file_id, vm->src_name);
    rc = ef_eval(vm, copy);
    free(copy);
    vm->file_id = saved_file;
    snprintf(vm->src_name, sizeof vm->src_name, "%s", saved_name);
    if (rc) {
        vm->throw_code = rc;
        return EF_THROW;
    }
    return EF_OK;
}

/* Optional filespec for CHDIR / LS / DIR. 1 = a name, 0 = bare, -1 = throw.
   The name stays on this line. A bare CHDIR must not swallow the next line. */
static int take_spec(ef_vm *vm, char *dst, int dstmax) {
    int in;
    int len;
    uint8_t *s;
    int n = 0;
    dst[0] = 0;
    if (dstmax < 2) {
        return 0;
    }
    in = uv(vm, UV_IN);
    len = vm->src_len;
    s = vm->base + SRC_ADDR;
    while (in < len && (s[in] == ' ' || s[in] == '\t' || s[in] == '\r')) {
        in++;
    }
    uv_set(vm, UV_IN, in);
    if (in >= len || s[in] == '\n') {
        return 0;
    }
    if (s[in] == '"') {
        uv_set(vm, UV_IN, in + 1);
        if (parse_until(vm, '"', dst, dstmax, &n)) {
            return -1;
        }
        return 1;
    }
    while (in < len && !is_white(s[in])) {
        if (n + 1 < dstmax) {
            dst[n++] = (char)s[in];
        }
        in++;
    }
    dst[n] = 0;
    uv_set(vm, UV_IN, in);
    return 1;
}

static int seg_ok(const char *s, size_t n) {
    size_t i;
    if (n == 0 || n >= 128) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c) || c == '"' || c == '\'' || c == '\\' || c == '/') {
            return 0;
        }
    }
    return 1;
}

static int spec_abs(const char *spec) {
    if (!spec || !spec[0]) {
        return 0;
    }
    if (spec[0] == '/' || spec[0] == '\\' || spec[0] == '~') {
        return 1;
    }
    if (isalpha((unsigned char)spec[0]) && spec[1] == ':') {
        return 1;
    }
    return 0;
}

/* Fold base and spec into a relative path. `..` may not climb above the root. */
static int join_rel(const char *base, const char *spec, char *out, size_t outmax) {
    char parts[40][128];
    const char *srcs[2];
    int np = 0;
    int si;
    size_t used = 0;
    srcs[0] = base ? base : "";
    srcs[1] = spec ? spec : "";
    for (si = 0; si < 2; si++) {
        const char *p = srcs[si];
        if (si == 1 && (p[0] == '/' || p[0] == '\\')) {
            return -1;
        }
        while (*p) {
            const char *start;
            size_t n;
            while (*p == '/' || *p == '\\') {
                p++;
            }
            if (!*p) {
                break;
            }
            start = p;
            while (*p && *p != '/' && *p != '\\') {
                p++;
            }
            n = (size_t)(p - start);
            if (n == 1 && start[0] == '.') {
                continue;
            }
            if (n == 2 && start[0] == '.' && start[1] == '.') {
                if (np <= 0) {
                    return -1;
                }
                np--;
                continue;
            }
            if (!seg_ok(start, n) || np >= 40) {
                return -1;
            }
            memcpy(parts[np], start, n);
            parts[np][n] = 0;
            np++;
        }
    }
    out[0] = 0;
    for (si = 0; si < np; si++) {
        size_t n = strlen(parts[si]);
        if (used + n + (used ? 1 : 0) + 1 > outmax) {
            return -1;
        }
        if (used) {
            out[used++] = '/';
        }
        memcpy(out + used, parts[si], n);
        used += n;
        out[used] = 0;
    }
    return 0;
}

/* Root is always visible. A relative directory is a real folder, or a prefix
   of a mounted path (the page has no process directory). */
static int dir_visible(ef_vm *vm, const char *rel) {
    ef_mounted *m;
    size_t n;
    if (!rel || !rel[0]) {
        return 1;
    }
#ifndef __EMSCRIPTEN__
    {
        DIR *d = opendir(rel);
        if (d) {
            closedir(d);
            return 1;
        }
    }
#endif
    n = strlen(rel);
    for (m = vm->mounted; m; m = m->next) {
        if (strncmp(m->path, rel, n) == 0 && m->path[n] == '/') {
            return 1;
        }
    }
    return 0;
}

static void emit_dir_label(ef_vm *vm, const char *dir) {
    char cwd[512];
    if (dir && dir[0]) {
        emit_str(vm, dir);
        return;
    }
#ifndef __EMSCRIPTEN__
    if (getcwd(cwd, sizeof cwd)) {
        emit_str(vm, cwd);
        return;
    }
#else
    (void)cwd;
#endif
    emit_str(vm, ".");
}

static void emit_cwd_line(ef_vm *vm) {
    emit_str(vm, "Current directory: ");
    emit_dir_label(vm, vm->logical_cwd);
    emit_ch(vm, '\n');
}

static void emit_pad(ef_vm *vm, const char *s, int width) {
    int n = (int)strlen(s);
    int i;
    emit_str(vm, s);
    for (i = n; i < width; i++) {
        emit_ch(vm, ' ');
    }
}

typedef struct {
    char name[160];
    int is_dir;
    long size;
} ef_dent;

static int dent_cmp(const void *a, const void *b) {
    return strcasecmp(((const ef_dent *)a)->name, ((const ef_dent *)b)->name);
}

static void dent_add(ef_dent *ents, int *n, int cap, const char *name, int is_dir, long size) {
    int i;
    size_t len;
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    len = strlen(name);
    if (len >= sizeof ents[0].name) {
        return;
    }
    for (i = 0; i < *n; i++) {
        if (strcasecmp(ents[i].name, name) == 0) {
            if (is_dir) {
                ents[i].is_dir = 1;
            } else if (!ents[i].is_dir) {
                ents[i].size = size;
            }
            return;
        }
    }
    if (*n >= cap) {
        return;
    }
    memcpy(ents[*n].name, name, len + 1);
    ents[*n].is_dir = is_dir;
    ents[*n].size = size;
    (*n)++;
}

static int wild_eq(const char *pat, const char *s) {
    while (*pat && *s) {
        if (*pat == '*') {
            while (*pat == '*') {
                pat++;
            }
            if (!*pat) {
                return 1;
            }
            for (; *s; s++) {
                if (wild_eq(pat, s)) {
                    return 1;
                }
            }
            return 0;
        }
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) {
            return 0;
        }
        pat++;
        s++;
    }
    while (*pat == '*') {
        pat++;
    }
    return *pat == 0 && *s == 0;
}

static void list_mounts(ef_vm *vm, const char *dir, ef_dent *ents, int *n, int cap) {
    ef_mounted *m;
    size_t dlen = dir ? strlen(dir) : 0;
    for (m = vm->mounted; m; m = m->next) {
        const char *rest = m->path;
        const char *slash;
        char name[160];
        size_t ln;
        if (dlen) {
            if (strncmp(m->path, dir, dlen) != 0 || m->path[dlen] != '/') {
                continue;
            }
            rest = m->path + dlen + 1;
        }
        if (!rest[0]) {
            continue;
        }
        slash = strchr(rest, '/');
        if (slash) {
            ln = (size_t)(slash - rest);
            if (ln == 0 || ln >= sizeof name) {
                continue;
            }
            memcpy(name, rest, ln);
            name[ln] = 0;
            dent_add(ents, n, cap, name, 1, 0);
        } else {
            dent_add(ents, n, cap, rest, 0, (long)strlen(m->text ? m->text : ""));
        }
    }
}

#ifndef __EMSCRIPTEN__
static void list_disk(const char *dir, ef_dent *ents, int *n, int cap) {
    DIR *d = opendir(dir && dir[0] ? dir : ".");
    struct dirent *de;
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char path[800];
        struct stat st;
        int is_dir = 0;
        long size = 0;
        if (dir && dir[0]) {
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        } else {
            snprintf(path, sizeof path, "%s", de->d_name);
        }
        if (stat(path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            size = (long)st.st_size;
        }
        dent_add(ents, n, cap, de->d_name, is_dir, size);
    }
    closedir(d);
}
#endif

static int list_at(ef_vm *vm, const char *base, const char *spec) {
    char dir[512];
    char filter[256];
    char rel[512];
    ef_dent *ents;
    int n = 0;
    int i;
    int shown = 0;
    const int cap = 512;
    const char *slash;
    filter[0] = 0;
    dir[0] = 0;
    if (spec_abs(spec)) {
        emit_str(vm, "DIR error: bad path\n");
        return EF_OK;
    }
    if (spec && spec[0] && (strchr(spec, '*') || strchr(spec, '?'))) {
        slash = strrchr(spec, '/');
        if (slash) {
            char head[512];
            size_t hn = (size_t)(slash - spec);
            if (hn >= sizeof head) {
                emit_str(vm, "DIR error: bad path\n");
                return EF_OK;
            }
            memcpy(head, spec, hn);
            head[hn] = 0;
            snprintf(filter, sizeof filter, "%s", slash + 1);
            if (head[0]) {
                if (join_rel(base, head, rel, sizeof rel) || (rel[0] && !path_ok(rel))) {
                    emit_str(vm, "DIR error: bad path\n");
                    return EF_OK;
                }
                snprintf(dir, sizeof dir, "%s", rel);
            } else {
                snprintf(dir, sizeof dir, "%s", base ? base : "");
            }
        } else {
            snprintf(filter, sizeof filter, "%s", spec);
            snprintf(dir, sizeof dir, "%s", base ? base : "");
        }
    } else if (spec && spec[0]) {
        if (join_rel(base, spec, rel, sizeof rel) || (rel[0] && !path_ok(rel))) {
            emit_str(vm, "DIR error: bad path\n");
            return EF_OK;
        }
        if (dir_visible(vm, rel)) {
            snprintf(dir, sizeof dir, "%s", rel);
        } else {
            snprintf(filter, sizeof filter, "%s", spec);
            snprintf(dir, sizeof dir, "%s", base ? base : "");
        }
    } else {
        snprintf(dir, sizeof dir, "%s", base ? base : "");
    }
    if (!dir_visible(vm, dir)) {
        emit_str(vm, "DIR error: Cannot read directory '");
        emit_dir_label(vm, dir);
        emit_str(vm, "'\n");
        return EF_OK;
    }
    ents = calloc((size_t)cap, sizeof *ents);
    if (!ents) {
        emit_str(vm, "DIR error: out of memory\n");
        return EF_OK;
    }
#ifndef __EMSCRIPTEN__
    list_disk(dir, ents, &n, cap);
#endif
    list_mounts(vm, dir, ents, &n, cap);
    qsort(ents, (size_t)n, sizeof *ents, dent_cmp);
    emit_ch(vm, '\n');
    emit_str(vm, "Directory of ");
    emit_dir_label(vm, dir);
    emit_str(vm, "\n\n");
    for (i = 0; i < n; i++) {
        char num[32];
        if (filter[0] && !wild_eq(filter, ents[i].name)) {
            continue;
        }
        emit_ch(vm, ' ');
        emit_pad(vm, ents[i].name, 30);
        if (ents[i].is_dir) {
            emit_str(vm, " <DIR>\n");
        } else {
            snprintf(num, sizeof num, "%ld", ents[i].size);
            emit_ch(vm, ' ');
            emit_pad(vm, num, 12);
            emit_ch(vm, '\n');
        }
        shown++;
    }
    free(ents);
    emit_str(vm, "\n ");
    emit_int(vm, shown, 10, 1);
    emit_str(vm, "file(s)\n\n");
    return EF_OK;
}

/* Form feed. The page and a terminal treat it as clear-the-console. */
static int p_cls(ef_vm *vm) {
    emit_ch(vm, '\f');
    return EF_OK;
}

static int p_chdir(ef_vm *vm) {
    char spec[512];
    char base[512];
    char dest[512];
    int got;
    int fromlib = vm->dbg_fromlib;
    vm->dbg_fromlib = 0;
    got = take_spec(vm, spec, (int)sizeof spec);
    if (got < 0) {
        return EF_THROW;
    }
    slashes(spec);
    if (fromlib) {
        snprintf(base, sizeof base, "Resources/Library");
    } else {
        snprintf(base, sizeof base, "%s", vm->logical_cwd);
    }
    if (!got || !spec[0]) {
        if (fromlib) {
            if (!dir_visible(vm, base)) {
                emit_str(vm, "CHDIR error: Cannot open directory '");
                emit_str(vm, base);
                emit_str(vm, "'\n");
                return EF_OK;
            }
            snprintf(vm->logical_cwd, sizeof vm->logical_cwd, "%s", base);
        }
        emit_cwd_line(vm);
        return EF_OK;
    }
    if (spec_abs(spec) || join_rel(base, spec, dest, sizeof dest) ||
        (dest[0] && !path_ok(dest))) {
        emit_str(vm, "CHDIR error: bad path\n");
        return EF_OK;
    }
    if (!dir_visible(vm, dest)) {
        emit_str(vm, "CHDIR error: Cannot open directory '");
        emit_str(vm, dest[0] ? dest : ".");
        emit_str(vm, "'\n");
        return EF_OK;
    }
    snprintf(vm->logical_cwd, sizeof vm->logical_cwd, "%s", dest);
    emit_cwd_line(vm);
    return EF_OK;
}

static int p_ls(ef_vm *vm) {
    char spec[512];
    char base[512];
    int got;
    int fromlib = vm->dbg_fromlib;
    vm->dbg_fromlib = 0;
    got = take_spec(vm, spec, (int)sizeof spec);
    if (got < 0) {
        return EF_THROW;
    }
    slashes(spec);
    if (fromlib) {
        snprintf(base, sizeof base, "Resources/Library");
    } else {
        snprintf(base, sizeof base, "%s", vm->logical_cwd);
    }
    return list_at(vm, base, got ? spec : "");
}

static int p_begin_cwd(ef_vm *vm) {
    char name[512];
    char parent[512];
    char joined[512];
    const char *old;
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    if (vm->cwd_n >= EF_CWD) {
        return fail(vm, -16, "too many includes\n");
    }
    snprintf(vm->cwd_stack[vm->cwd_n], sizeof vm->cwd_stack[vm->cwd_n], "%s", vm->include_dir);
    vm->cwd_n++;
    old = vm->cwd_stack[vm->cwd_n - 1];
    dir_of(name, parent, sizeof parent);
    if (parent[0]) {
        if (old[0]) {
            snprintf(joined, sizeof joined, "%s/%s", old, parent);
        } else if (vm->logical_cwd[0]) {
            snprintf(joined, sizeof joined, "%s/%s", vm->logical_cwd, parent);
        } else {
            snprintf(joined, sizeof joined, "%s", parent);
        }
        slashes(joined);
        if (path_ok(joined)) {
            snprintf(vm->include_dir, sizeof vm->include_dir, "%s", joined);
        }
    }
    return EF_OK;
}

static int p_end_cwd(ef_vm *vm) {
    if (vm->cwd_n > 0) {
        vm->cwd_n--;
        snprintf(vm->include_dir, sizeof vm->include_dir, "%s", vm->cwd_stack[vm->cwd_n]);
    }
    return EF_OK;
}

static int req_has(ef_vm *vm, const char *name) {
    int i;
    for (i = 0; i < vm->nreq; i++) {
        if (vm->req[i] && strcmp(vm->req[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void req_add(ef_vm *vm, const char *name) {
    char *copy;
    if (!name[0] || vm->nreq >= EF_REQ || req_has(vm, name)) {
        return;
    }
    copy = dup_str(name);
    if (copy) {
        vm->req[vm->nreq++] = copy;
    }
}

static int p_register_inc(ef_vm *vm) {
    char name[256];
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    req_add(vm, name);
    return EF_OK;
}

static int p_required(ef_vm *vm) {
    char name[512];
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    if (req_has(vm, name)) {
        return EF_OK;
    }
    if (include_by_name(vm, name)) {
        return EF_THROW;
    }
    req_add(vm, name);
    return EF_OK;
}

static int p_include_dialog(ef_vm *vm) {
    emit_str(vm, "INCLUDE needs a name\n");
    return fail(vm, -38, NULL);
}

static int p_xc_fetch(ef_vm *vm) {
    int32_t a;
    unsigned lead, cp;
    int need, i;
    if (pop(vm, &a)) {
        return EF_THROW;
    }
    if (a < 0 || (size_t)a >= vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    lead = vm->base[a];
    if (lead < 0x80u) {
        return push2(vm, a + 1, (int32_t)lead);
    }
    if (lead < 0xE0u) {
        need = 2;
        cp = lead & 0x1Fu;
    } else if (lead < 0xF0u) {
        need = 3;
        cp = lead & 0x0Fu;
    } else {
        need = 4;
        cp = lead & 0x07u;
    }
    if ((size_t)a + (size_t)need > vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    for (i = 1; i < need; i++) {
        cp = (cp << 6) | (vm->base[a + i] & 0x3Fu);
    }
    if (cp > 0x10FFFFu) {
        cp = 0x10FFFFu;
    }
    return push2(vm, a + need, (int32_t)cp);
}

static int p_xc_store(ef_vm *vm) {
    int32_t a, x;
    uint32_t cp;
    unsigned char b[4];
    int n;
    if (pop(vm, &a) || pop(vm, &x)) {
        return EF_THROW;
    }
    cp = (uint32_t)x;
    if (cp > 0x10FFFFu) {
        cp = 0x10FFFFu;
    }
    if (cp <= 0x7Fu) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FFu) {
        b[0] = (unsigned char)(0xC0u | (cp >> 6));
        b[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp <= 0xFFFFu) {
        b[0] = (unsigned char)(0xE0u | (cp >> 12));
        b[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0u | (cp >> 18));
        b[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    if (a < 0 || ensure(vm, a + n)) {
        return fail(vm, -16, "bad address\n");
    }
    memcpy(vm->base + a, b, (size_t)n);
    return push(vm, a + n);
}

static const struct {
    const char *name;
    ef_prim_fn fn;
    int imm;
} k_prims[] = {
    { "DUP", p_dup, 0 },
    { "DROP", p_drop, 0 },
    { "SWAP", p_swap, 0 },
    { "OVER", p_over, 0 },
    { "ROT", p_rot, 0 },
    { "NIP", p_nip, 0 },
    { "TUCK", p_tuck, 0 },
    { "PICK", p_pick, 0 },
    { "DEPTH", p_depth, 0 },
    { ">R", p_to_r, 0 },
    { "R>", p_r_from, 0 },
    { "R@", p_r_fetch, 0 },
    { "+", p_add, 0 },
    { "-", p_sub, 0 },
    { "*", p_mul, 0 },
    { "/", p_slash, 0 },
    { "MOD", p_mod, 0 },
    { "/MOD", p_slash_mod, 0 },
    { "1+", p_one_plus, 0 },
    { "1-", p_one_minus, 0 },
    { "2*", p_two_star, 0 },
    { "2/", p_two_slash, 0 },
    { "NEGATE", p_negate, 0 },
    { "ABS", p_abs, 0 },
    { "AND", p_and, 0 },
    { "OR", p_or, 0 },
    { "XOR", p_xor, 0 },
    { "INVERT", p_invert, 0 },
    { "LSHIFT", p_lshift, 0 },
    { "RSHIFT", p_rshift, 0 },
    { "=", p_eq, 0 },
    { "<", p_lt, 0 },
    { ">", p_gt, 0 },
    { "<>", p_ne, 0 },
    { "U<", p_ult, 0 },
    { "U>", p_ugt, 0 },
    { "0=", p_zero_eq, 0 },
    { "0<", p_zero_lt, 0 },
    { "0>", p_zero_gt, 0 },
    { "0<>", p_zero_ne, 0 },
    { "@", p_fetch, 0 },
    { "!", p_store, 0 },
    { "+!", p_plus_store, 0 },
    { "C@", p_cfetch, 0 },
    { "C!", p_cstore, 0 },
    { ",", p_comma, 0 },
    { "C,", p_ccomma, 0 },
    { "ALLOT", p_allot, 0 },
    { "HERE", p_here, 0 },
    { "DP", p_dp, 0 },
    { "CELL", p_cell, 0 },
    { "BASE", p_base, 0 },
    { "STATE", p_state, 0 },
    { "TRUE", p_true, 0 },
    { "FALSE", p_false, 0 },
    { "BL", p_bl, 0 },
    { "FILL", p_fill, 0 },
    { "MOVE", p_move, 0 },
    { ">BODY", p_to_body, 0 },
    { ">LINK", p_to_link, 0 },
    { ">FLAGS", p_to_flags, 0 },
    { ">CODE", p_to_code, 0 },
    { "EMIT", p_emit, 0 },
    { "CR", p_cr, 0 },
    { "SPACE", p_space, 0 },
    { "TYPE", p_type, 0 },
    { ".", p_dot, 0 },
    { "U.", p_udot, 0 },
    { ".S", p_dot_s, 0 },
    { "KEY", p_key, 0 },
    { "EKEY", p_ekey, 0 },
    { "BYE", p_bye, 0 },
    { "ABORT", p_abort, 0 },
    { "EXIT", p_exit, 0 },
    { "LIT", p_lit, 0 },
    { "BRANCH", p_branch, 0 },
    { "0BRANCH", p_zbranch, 0 },
    { "(EF-BRANCH)", p_ef_branch, 0 },
    { "(EF-0BRANCH)", p_ef_zbranch, 0 },
    { "(DO)", p_do_rt, 0 },
    { "(?DO)", p_qdo_rt, 0 },
    { "(LOOP)", p_loop_rt, 0 },
    { "(+LOOP)", p_ploop_rt, 0 },
    { "I", p_i, 0 },
    { "J", p_j, 0 },
    { "UNLOOP", p_unloop, 0 },
    { "(DOES>)", p_does_rt, 0 },
    { "(.\")", p_dot_quote_rt, 0 },
    { "(S\")", p_s_quote_rt, 0 },
    { ":", p_colon, 0 },
    { ";", p_semicolon, 1 },
    { "IMMEDIATE", p_immediate, 0 },
    { "[", p_lbrack, 1 },
    { "]", p_rbrack, 0 },
    { "LITERAL", p_literal, 1 },
    { "RECURSE", p_recurse, 1 },
    { "CREATE", p_create, 0 },
    { "VARIABLE", p_variable, 0 },
    { "CONSTANT", p_constant, 0 },
    { "DOES>", p_does, 1 },
    { "BEGIN", p_begin, 1 },
    { "UNTIL", p_until, 1 },
    { "AGAIN", p_again, 1 },
    { "WHILE", p_while, 1 },
    { "REPEAT", p_repeat, 1 },
    { "IF", p_if, 1 },
    { "ELSE", p_else, 1 },
    { "THEN", p_then, 1 },
    { "AHEAD", p_ahead, 1 },
    { "DO", p_do, 1 },
    { "?DO", p_qdo, 1 },
    { "LOOP", p_loop, 1 },
    { "+LOOP", p_ploop, 1 },
    { "LEAVE", p_leave, 1 },
    { "\\", p_backslash, 1 },
    { "(", p_paren, 1 },
    { ".\"", p_dot_quote, 1 },
    { "S\"", p_s_quote, 1 },
    { ".(", p_dot_paren, 1 },
    { "'", p_tick, 0 },
    { "[']", p_bracket_tick, 1 },
    { "EXECUTE", p_execute, 0 },
    { "CHAR", p_char, 0 },
    { "[CHAR]", p_bracket_char, 1 },
    { "FIND", p_find, 0 },
    { "WORD", p_word, 0 },
    { "WORDS", p_words, 0 },
    { "SEE", p_see, 0 },
    { "FORTH", p_forth, 0 },
    { "DEFINITIONS", p_definitions, 0 },
    { "INCLUDE", p_include, 0 },
    { "CLS", p_cls, 0 },
    { "CHDIR", p_chdir, 0 },
    { "LS", p_ls, 0 },
    { "DIR", p_ls, 0 },
    { "THROW", p_throw, 0 },
    { "LAST", p_last, 0 },
    { "PARSE", p_parse, 0 },
    { "SETDOC", p_setdoc, 0 },
    { "GET-ORDER", p_get_order, 0 },
    { "SET-ORDER", p_set_order, 0 },
    { "FORTH-WORDLIST", p_forth_wid, 0 },
    { "DICT-THREADS", p_dict_threads, 0 },
    { "PAD", p_pad, 0 },
    { "UNUSED", p_unused, 0 },
    { ">IN", p_to_in, 0 },
    { "SOURCE", p_source, 0 },
    { "REFILL", p_refill, 0 },
    { "(APP-OPEN)", p_app_open, 0 },
    { "(APP-CLOSE)", p_app_close, 0 },
    { "(APP-BLIT)", p_app_blit, 0 },
    { "(APP-CBLIT)", p_app_cblit, 0 },
    { "(APP-PBLIT)", p_app_pblit, 0 },
    { "(APP-KEY?)", p_app_key_q, 0 },
    { "(APP-KEY)", p_app_key, 0 },
    { "(APP-NAME)", p_app_name, 0 },
    { "(APP-TONE)", p_app_tone, 0 },
    { "(APP-PUMP)", p_app_pump, 0 },
    { "(APP-MOUSE)", p_app_mouse, 0 },
    { "(APP-SIZE?)", p_app_size, 0 },
    { "KEY?", p_key_q, 0 },
    { "LATEST", p_latest, 0 },
    { "REDEF-WARNING", p_redef, 0 },
    { "BLOCK-FILE", p_block_file, 0 },
    { "(BLOCK-UPD)", p_block_upd, 0 },
    { "(BLOCK-NR)", p_block_nr, 0 },
    { "SCR", p_scr, 0 },
    { "(BLOCK-BUF)", p_block_buf, 0 },
    { "R/O", p_ro, 0 },
    { "R/W", p_rw, 0 },
    { "BIN", p_bin, 0 },
    { "DBG-ON", p_dbg_on, 0 },
    { "DBG-OFF", p_dbg_off, 0 },
    { "DBG-BREAK-MIN!", p_dbg_break_min, 0 },
    { "DBG-XT@", p_dbg_xt_at, 0 },
    { "DBG-IP@", p_dbg_ip_at, 0 },
    { "DBG-CFA@", p_dbg_cfa_at, 0 },
    { "DBG-BODY#", p_dbg_body_n, 0 },
    { "DBG-.SR", p_dbg_dot_sr, 0 },
    { "DBG-PRINT-INLINE", p_dbg_print_inline, 0 },
    { "DBG-PRINT-NAME", p_dbg_print_name, 0 },
    { "DBG-XT-INTOABLE?", p_dbg_intoable, 0 },
    { "DBG-HOST-PAINT", p_dbg_host_paint, 0 },
    { "DBG-HOST-SPAN", p_dbg_host_span, 0 },
    { "DBG-HOST-SPAN@", p_dbg_host_span_at, 0 },
    { "DBG-VIEW-UPDATE", p_dbg_view_update, 0 },
    { "DBG-STEP-OVER", p_dbg_step_over, 0 },
    { "DBG-STEP-INTO", p_dbg_step_into, 0 },
    { "DBG-STEP-OUT", p_dbg_step_out, 0 },
    { "DBG-GO", p_dbg_go, 0 },
    { "DBG-ABORT-SESSION", p_dbg_abort, 0 },
    { "(BP-GO)", p_bp_go, 0 },
    { "(RUNTO-IP)", p_runto_ip, 0 },
    { "RUNTO-OFF@", p_runto_off_fetch, 0 },
    { "RUNTO-STATUS!", p_runto_status, 0 },
    { "FROMLIB", p_fromlib, 0 },
    { "FROMLIB?", p_fromlib_q, 0 },
    { "FROMLIB-OFF", p_fromlib_off, 0 },
    { "(LOAD-ENTER)", p_load_enter, 0 },
    { "RESOLVE-KEY", p_resolve_key, 0 },
    { "BOOT-WORD-TABLE", p_boot0, 0 },
    { "BOOT-WORD-TABLE-END", p_boot0, 0 },
    { "MS@", p_ms_at, 0 },
    { "UM*", p_um_star, 0 },
    { "M*", p_m_star, 0 },
    { "UM/MOD", p_um_mod, 0 },
    { "SM/REM", p_sm_rem, 0 },
    { "D+", p_d_plus, 0 },
    { "DNEGATE", p_d_negate, 0 },
    { "DABS", p_d_abs, 0 },
    { "D0<", p_d_zero_lt, 0 },
    { "2@", p_two_fetch, 0 },
    { "2!", p_two_store, 0 },
    { "SAVE-INPUT", p_save_input, 0 },
    { "RESTORE-INPUT", p_restore_input, 0 },
    { "2>R", p_two_to_r, 0 },
    { "2R>", p_two_r_from, 0 },
    { "2R@", p_two_r_fetch, 0 },
    { "WITHIN", p_within, 0 },
    { "COMPARE", p_compare, 0 },
    { "PARSE-NAME", p_parse_name, 0 },
    { "PARSE-FILESPEC", p_parse_filespec, 0 },
    { "ALSO", p_also, 0 },
    { "PREVIOUS", p_previous, 0 },
    { "TO", p_to, 1 },
    { "CATCH", p_catch, 0 },
    { "FORGET", p_forget, 0 },
    { "TRAVERSE-WORDLIST", p_traverse, 0 },
    { "WORDLISTS", p_wordlists, 0 },
    { "(REGISTER-WID)", p_register_wid, 0 },
    { "VIEW-PATH", p_view_path, 0 },
    { "SEARCH-WORDLIST", p_search_wl, 0 },
    { "(LOCAL)", p_local, 0 },
    { "LOCAL-INIT", p_local_init, 0 },
    { "(LOCAL@)", p_local_at, 0 },
    { "(LOCAL!)", p_local_store, 0 },
    { "{:", p_brace, 1 },
    { "(F-OP)", p_fop, 0 },
    { "FLIT", p_flit, 0 },
    { "TEXTEDIT", p_textedit, 0 },
    { "OPEN-FILE", p_open_file, 0 },
    { "CREATE-FILE", p_create_file, 0 },
    { "CLOSE-FILE", p_close_file, 0 },
    { "FILE-SIZE", p_file_size, 0 },
    { "READ-FILE", p_read_file, 0 },
    { "WRITE-FILE", p_write_file, 0 },
    { "REPOSITION-FILE", p_reposition, 0 },
    { "FLUSH-FILE", p_flush_file, 0 },
    { "ALLOCATE", p_allocate, 0 },
    { "FREE", p_free, 0 },
    { "(LINE-SOURCE)", p_line_source, 0 },
    { "(LOAD-RUN)", p_line_source, 0 },
    { "BEGIN-LOAD-CWD", p_begin_cwd, 0 },
    { "END-LOAD-CWD", p_end_cwd, 0 },
    { "REGISTER-INCLUDED-STR", p_register_inc, 0 },
    { "REQUIRED", p_required, 0 },
    { "(INCLUDE)", p_include_dialog, 0 },
    { "XC@+", p_xc_fetch, 0 },
    { "XC!+", p_xc_store, 0 },
};

static void note_special(ef_vm *vm, const char *name, int32_t cfa) {
    if (strcmp(name, "EXIT") == 0) vm->xt_exit = cfa;
    else if (strcmp(name, "LIT") == 0) vm->xt_lit = cfa;
    else if (strcmp(name, "BRANCH") == 0) vm->xt_branch = cfa;
    else if (strcmp(name, "0BRANCH") == 0) vm->xt_zbranch = cfa;
    else if (strcmp(name, "(EF-BRANCH)") == 0) vm->xt_ef_branch = cfa;
    else if (strcmp(name, "(EF-0BRANCH)") == 0) vm->xt_ef_zbranch = cfa;
    else if (strcmp(name, "(DO)") == 0) vm->xt_do = cfa;
    else if (strcmp(name, "(?DO)") == 0) vm->xt_qdo = cfa;
    else if (strcmp(name, "(LOOP)") == 0) vm->xt_loop = cfa;
    else if (strcmp(name, "(+LOOP)") == 0) vm->xt_ploop = cfa;
    else if (strcmp(name, "UNLOOP") == 0) vm->xt_unloop = cfa;
    else if (strcmp(name, "(.\")") == 0) vm->xt_dotq = cfa;
    else if (strcmp(name, "(S\")") == 0) vm->xt_squote = cfa;
    else if (strcmp(name, "(DOES>)") == 0) vm->xt_does = cfa;
    else if (strcmp(name, ".(") == 0) vm->xt_dotparen = cfa;
    else if (strcmp(name, "!") == 0) vm->xt_store = cfa;
    else if (strcmp(name, "LOCAL-INIT") == 0) vm->xt_linit = cfa;
    else if (strcmp(name, "(LOCAL@)") == 0) vm->xt_lat = cfa;
    else if (strcmp(name, "(LOCAL!)") == 0) vm->xt_lstore = cfa;
    else if (strcmp(name, "DBG-OFF") == 0) vm->xt_dbg_off = cfa;
    else if (strcmp(name, "EXECUTE") == 0) vm->xt_execute = cfa;
    else if (strcmp(name, "CATCH") == 0) vm->xt_catch = cfa;
}

static void register_all(ef_vm *vm) {
    size_t i;
    g_prim_count = 0;
    memset(g_prims, 0, sizeof g_prims);
    for (i = 0; i < sizeof k_prims / sizeof k_prims[0]; i++) {
        int32_t id;
        if (g_prim_count >= (int)(sizeof g_prims / sizeof g_prims[0])) {
            fail(vm, -8, "too many primitives\n");
            return;
        }
        id = g_prim_count++;
        int32_t cfa;
        g_prims[id] = k_prims[i].fn;
        cfa = header(vm, k_prims[i].name, (int)strlen(k_prims[i].name), id);
        if (!cfa) {
            return;
        }
        if (k_prims[i].imm) {
            int32_t f = uv(vm, cfa - CELL);
            uv_set(vm, cfa - CELL, (int32_t)((uint32_t)f | FFA1_IMM));
        }
        note_special(vm, k_prims[i].name, cfa);
    }
}

static void default_emit(void *ctx, int ch) {
    (void)ctx;
    fputc(ch, stderr);
}

#include "boot.inc"

static int open_embedded_boot(char **owned, int *len) {
    int n = (int)strlen(boot_fth);
    if (copy_owned(boot_fth, n, owned)) {
        return -1;
    }
    *len = n;
    return 0;
}

ef_vm *ef_create(void) {
    ef_vm *vm = calloc(1, sizeof *vm);
    if (!vm) {
        return NULL;
    }
    vm->count = EF_IMAGE0;
    vm->base = calloc(1, vm->count);
    if (!vm->base) {
        free(vm);
        return NULL;
    }
    vm->emit = default_emit;
    vm->forth_wid = FORTH_WID;
    vm->next_file = 2;
    uv_set(vm, UV_DP, DICT_ORIGIN);
    uv_set(vm, UV_BASE, 10);
    uv_set(vm, UV_STATE, 0);
    uv_set(vm, UV_CURRENT, FORTH_WID);
    uv_set(vm, UV_ORDERN, 1);
    uv_set(vm, UV_ORDER, FORTH_WID);
    uv_set(vm, UV_BLOCK_NR, -1);
    vm->heap_at = HEAP_ORIGIN;
    vm->dbg_over = -1;
    vm->dbg_out = -1;
    register_all(vm);
    if (vm->throw_code) {
        ef_destroy(vm);
        return NULL;
    }
    return vm;
}

void ef_destroy(ef_vm *vm) {
    int i;
    if (!vm) {
        return;
    }
    for (i = 0; i < EF_OPEN; i++) {
        free(vm->files[i].data);
    }
    for (i = 0; i < vm->nreq; i++) {
        free(vm->req[i]);
    }
    ef_mount_clear(vm);
    free(vm->base);
    free(vm);
}

void ef_set_emit(ef_vm *vm, ef_emit_fn fn, void *ctx) {
    vm->emit = fn ? fn : default_emit;
    vm->emit_ctx = ctx;
}

void ef_set_debug(ef_vm *vm, ef_debug_fn fn, void *ctx) {
    if (!vm) {
        return;
    }
    vm->debug_fn = fn;
    vm->debug_ctx = ctx;
}

void ef_set_key(ef_vm *vm, ef_key_fn fn, void *ctx) {
    vm->key = fn;
    vm->key_ctx = ctx;
}

int ef_eval(ef_vm *vm, const char *text) {
    src_frame saved;
    size_t n;
    int rc;
    int top;
    int saved_inner;
    if (!text) {
        text = "";
    }
    saved_inner = vm->in_inner;
    top = vm->eval_depth == 0;
    if (top) {
        vm->err_line = 0;
        vm->err_name[0] = 0;
    }
    vm->eval_depth++;
    if (save_source(vm, &saved)) {
        vm->eval_depth--;
        vm->in_inner = saved_inner;
        return fail(vm, -8, "out of memory\n"), vm->throw_code;
    }
    n = strlen(text);
    if (n > EF_SRC_MAX - 1) {
        restore_source(vm, &saved);
        vm->eval_depth--;
        vm->in_inner = saved_inner;
        return fail(vm, -16, "input too long\n"), vm->throw_code;
    }
    if (ensure(vm, SRC_ADDR + (int32_t)n + 1)) {
        restore_source(vm, &saved);
        recover(vm);
        rc = vm->throw_code;
        vm->throw_code = 0;
        vm->in_inner = saved_inner;
        vm->eval_depth--;
        return rc;
    }
    memcpy(vm->base + SRC_ADDR, text, n);
    vm->base[SRC_ADDR + n] = 0;
    vm->src_len = (int)n;
    uv_set(vm, UV_IN, 0);
    vm->throw_code = 0;
    vm->bye = 0;
    rc = interpret(vm);
    if (rc != EF_OK && vm->err_line == 0) {
        vm->err_line = vm->line > 0 ? vm->line : 1;
        snprintf(vm->err_name, sizeof vm->err_name, "%s", vm->src_name);
    }
    restore_source(vm, &saved);
    vm->eval_depth--;
    if (rc != EF_OK) {
        int code = vm->throw_code ? vm->throw_code : -1;
        recover(vm);
        vm->throw_code = 0;
        vm->in_inner = saved_inner;
        return code;
    }
    vm->in_inner = saved_inner;
    return 0;
}

const char *ef_boot_source(void) {
    return boot_fth;
}

int ef_boot(ef_vm *vm) {
    int saved = vm->file_id;
    int rc;
    remember_view(vm, 1, "boot");
    vm->file_id = 1;
    rc = ef_eval(vm, boot_fth);
    vm->file_id = saved;
    return rc;
}

int ef_depth(const ef_vm *vm) { return vm->sp; }

int ef_state(const ef_vm *vm) { return uv((ef_vm *)vm, UV_STATE); }

int32_t ef_stack_item(const ef_vm *vm, int from_bottom) {
    if (from_bottom < 0 || from_bottom >= vm->sp) {
        return 0;
    }
    return vm->stack[from_bottom];
}

int ef_throw_code(const ef_vm *vm) { return vm->throw_code; }

int ef_bye_requested(const ef_vm *vm) { return vm->bye; }

int ef_error_line(const ef_vm *vm) { return vm->err_line; }

const char *ef_error_name(const ef_vm *vm) { return vm->err_name; }
