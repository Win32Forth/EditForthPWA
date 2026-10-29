#include "ef32.h"

#include <ctype.h>
#include <dirent.h>
#include <float.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CELL 4
#define EF_STACK 512
#define EF_RSTACK 512
/* ANSValidate ENV-FSTK expects this depth from FLOATING-STACK. */
#define EF_FSTACK 16
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
/* File-id 2 is Resources/System/Sources/ef32.c. Includes start at 3. */
#define EF_VIEW_C 2
#define EF_LOCAL_MAX 32
#define EF_LOCAL_FRAMES 16

#define FFA1_IMM 0x80000000u
/* "UTF-8" for ENVIRONMENT? XCHAR-ENCODING. Below the source buffer. */
#define EF_ENV_UTF8 128

typedef struct ef_blob {
    char path[512];
    char *data;
    int len;
    int cap;
    struct ef_blob *next;
} ef_blob;

struct ef_vm {
    uint8_t *base;
    size_t count;

    int32_t stack[EF_STACK];
    int sp;
    /* Separate IEEE double stack. Cells stay 32-bit; a float is 8 bytes. */
    double fstack[EF_FSTACK];
    int fsp;
    int fprec;
    int32_t rstack[EF_RSTACK];
    int rsp;
    int32_t ip;
    int in_inner;
    int throw_code;
    int bye;
    int line;
    int file_id;
    int next_file;
    /* One eval can start at this 1-based line of src_name. hold_line keeps
       interpret from resetting the line. Console input uses both so a later
       pause can mark the token in the accumulated source. */
    int src_line;
    int src_line_set;
    int hold_line;
    int src_len;
    /* Address SOURCE returns. SRC_ADDR for a console line or an included
       line. EVALUATE points it at the string it was given. */
    int32_t src_at;
    /* FILE-ECHO body. file_echo_pos is the next source byte not yet echoed.
       src_echo is 1 for an INCLUDE/FLOAD buffer. echo_file is consumed by
       the next ef_eval. Console and the cold image stay quiet. */
    int32_t file_echo_addr;
    int file_echo_pos;
    int src_echo;
    int echo_file;
    /* ANS: 0 user input, -1 EVALUATE, >0 a file being included. */
    int source_id;
    /* High-level INCLUDE keeps the file here and shows one line as SOURCE.
       load_on is 0 for the console, EVALUATE, and the cold image. \S sets
       load_stop so the rest of that file is not read. */
    char *load_text;
    int load_len;
    int load_pos;
    int load_stop;
    int load_line;
    int load_line_at;
    int load_gen;
    int load_on;
    /* -1 while this load is a file. A block load stores the block number.
       REFILL of a block loads the next block, not the next 64-character line. */
    int load_blk;
    int quit_req;
    int32_t noname_xt;
    int32_t blk_addr;
    int fac_col;
    int fac_row;
    ef_blob *blobs;
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

    int32_t xt_exit, xt_lit, xt_flit, xt_branch, xt_zbranch;
    int32_t xt_ef_branch, xt_ef_zbranch;
    int32_t xt_do, xt_qdo, xt_loop, xt_ploop, xt_unloop;
    int32_t xt_dotq, xt_squote, xt_cquote, xt_dotparen, xt_does;
    int32_t xt_store, xt_two_fetch, xt_two_store;
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
    ef_keyq_fn key_q;
    ef_debug_fn debug_fn;
    ef_view_fn view_fn;
    ef_fetch_fn fetch_fn;
    void *emit_ctx;
    void *key_ctx;
    void *key_q_ctx;
    void *debug_ctx;
    void *view_ctx;
    void *fetch_ctx;

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
    int dbg_yield;
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
    /* Next interpret S" pocket. Three slots sit above the <# hold area. */
    int s_pocket;
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
static ef_blob *blob_find(ef_vm *vm, const char *path);
static int span_ok(ef_vm *vm, int32_t addr, int32_t n);
static uint64_t pack_d(int32_t lo, int32_t hi);
static int push_d(ef_vm *vm, uint64_t d);

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

static int fpush(ef_vm *vm, double v) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    if (vm->fsp >= EF_FSTACK) {
        return fail(vm, -44, "float stack overflow\n");
    }
    vm->fstack[vm->fsp++] = v;
    return EF_OK;
}

static int fpop(ef_vm *vm, double *out) {
    if (vm->throw_code) {
        return EF_THROW;
    }
    if (vm->fsp <= 0) {
        return fail(vm, -45, "float stack underflow\n");
    }
    *out = vm->fstack[--vm->fsp];
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
    /* n == 0 is :NONAME. The header is not linked, so FIND and WORDS skip it. */
    if (n < 0 || n > 127) {
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
    wid = uv(vm, UV_CURRENT);
    prev = 0;
    th = 0;
    if (n > 0) {
        th = hash_name(name, n);
        prev = uv(vm, wid + (int)th * CELL);
    }
    ffa0 = (uint32_t)(cfa - nfa) | ((uint32_t)(cfa - hfa) << 16);
    ffa1 = (uint32_t)(vm->line & 0xffff) | ((uint32_t)(vm->file_id & 0x1fff) << 16);
    if (comma(vm, prev) || comma(vm, (int32_t)ffa0) || comma(vm, (int32_t)ffa1) ||
        comma(vm, code) || comma(vm, 0)) {
        return 0;
    }
    if (n > 0) {
        uv_set(vm, wid + (int)th * CELL, cfa);
    }
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
    vm->fsp = 0;
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

static uint8_t *src_ptr(const ef_vm *vm) {
    return vm->base + vm->src_at;
}

/* S" C" ." .( ( and \ keep the character after the name. Every other
   word consumes one trailing blank, matching WORD, so 1 >IN +! skips
   the next character (the Hayes xSOURCE trick). */
static int keeps_following_text(const char *s, int n) {
    if (n == 1 && (s[0] == '(' || s[0] == '\\')) {
        return 1;
    }
    if (n == 2 && s[1] == '"' && (s[0] == 'S' || s[0] == 's' || s[0] == 'C' || s[0] == 'c' || s[0] == '.')) {
        return 1;
    }
    if (n == 2 && s[0] == '.' && s[1] == '(') {
        return 1;
    }
    return 0;
}

static int next_name(ef_vm *vm, char *dst, int dstmax, int *outn) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = src_ptr(vm);
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
    /* Glue S"hi" and ."hello" onto the string word. The next character
       is the closing delimiter for a name such as S"", which stays whole. */
    if (n > 2 && s[start + 2] != '"' && dstmax > 3 && s[start] == '.' && s[start + 1] == '"') {
        uv_set(vm, UV_IN, start + 2);
        dst[0] = '.';
        dst[1] = '"';
        dst[2] = 0;
        *outn = 2;
        return 1;
    }
    if (n > 2 && s[start + 2] != '"' && dstmax > 3 && toupper(s[start]) == 'S' && s[start + 1] == '"') {
        uv_set(vm, UV_IN, start + 2);
        dst[0] = 'S';
        dst[1] = '"';
        dst[2] = 0;
        *outn = 2;
        return 1;
    }
    if (n > 2 && s[start + 2] != ')' && dstmax > 4 && s[start] == '.' && s[start + 1] == '(') {
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
    if (!keeps_following_text(dst, n) && in < len && s[in] == ' ') {
        uv_set(vm, UV_IN, in + 1);
    }
    return 1;
}

static int parse_until(ef_vm *vm, char end, char *dst, int dstmax, int *outn) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = src_ptr(vm);
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
    uint8_t *s = src_ptr(vm);
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

/* 1 = single in *lo. 2 = double, *lo under *hi. A trailing '.' is a double.
   The dot is not a digit. A single still rejects values outside one cell. */
static int parse_literal(const char *s, int n, int base, int32_t *lo, int32_t *hi) {
    int i = 0;
    int sign = 1;
    int b = base;
    int dbl = 0;
    int digits = 0;
    uint64_t acc = 0;
    if (n >= 3 && s[0] == '\'' && s[n - 1] == '\'' && n == 3) {
        *lo = (unsigned char)s[1];
        return 1;
    }
    if (n > 1 && s[n - 1] == '.') {
        dbl = 1;
        n--;
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
        uint64_t lim;
        if (d < 0 || d >= b) {
            return 0;
        }
        if (acc > (UINT64_MAX - (uint64_t)d) / (uint64_t)b) {
            return 0;
        }
        acc = acc * (uint64_t)b + (uint64_t)d;
        digits++;
        if (!dbl) {
            lim = sign < 0 ? 2147483648ULL : 4294967295ULL;
            if (acc > lim) {
                return 0;
            }
        }
    }
    if (!digits) {
        return 0;
    }
    if (sign < 0) {
        acc = ~acc + 1u;
    }
    *lo = (int32_t)(uint32_t)acc;
    if (!dbl) {
        return 1;
    }
    *hi = (int32_t)(uint32_t)(acc >> 32);
    return 2;
}

/* Forth-94 >FLOAT. Empty or all blanks is +0. Any other space fails.
   A bare + or - after the significand is an exponent (1+1 is 10).
   Conversion is decimal, whatever BASE is. */
static int is_exp_mark(int c) {
    return c == 'e' || c == 'E' || c == 'd' || c == 'D';
}

static int parse_float(const char *s, int n, double *out) {
    int i = 0;
    int sign = 0;
    int digits = 0;
    int k;
    int exp_at = -1;
    int bare = 0;
    char buf[768];
    char *end = NULL;
    double v;
    if (n < 0 || n > 700) {
        return 0;
    }
    if (n == 0) {
        *out = 0.0;
        return 1;
    }
    for (k = 0; k < n; k++) {
        if (!is_white((unsigned char)s[k]) && s[k] != ' ') {
            break;
        }
    }
    if (k == n) {
        *out = 0.0;
        return 1;
    }
    for (k = 0; k < n; k++) {
        if (s[k] == ' ' || s[k] == '\t' || s[k] == '\n' || s[k] == '\r') {
            return 0;
        }
    }
    if (s[i] == '+' || s[i] == '-') {
        sign = (s[i] == '-') ? -1 : 1;
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        digits++;
        i++;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            digits++;
            i++;
        }
    }
    if (!digits) {
        return 0;
    }
    if (i < n && is_exp_mark((unsigned char)s[i])) {
        exp_at = i;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
        if (i != n) {
            return 0;
        }
    } else if (i < n && (s[i] == '+' || s[i] == '-')) {
        int ed = 0;
        bare = 1;
        exp_at = i;
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            ed++;
            i++;
        }
        if (!ed || i != n) {
            return 0;
        }
    } else if (i != n) {
        return 0;
    }
    k = 0;
    if (bare) {
        memcpy(buf, s, (size_t)exp_at);
        k = exp_at;
        buf[k++] = 'e';
        memcpy(buf + k, s + exp_at, (size_t)(n - exp_at));
        k += n - exp_at;
        buf[k] = 0;
    } else if (exp_at >= 0) {
        int j;
        for (j = 0; j < n; j++) {
            char c = s[j];
            if (c == 'd' || c == 'D' || c == 'E') {
                c = 'e';
            }
            buf[k++] = c;
        }
        if (buf[k - 1] == 'e' || buf[k - 1] == '+' || buf[k - 1] == '-') {
            buf[k++] = '0';
        }
        buf[k] = 0;
    } else {
        memcpy(buf, s, (size_t)n);
        buf[n] = 0;
    }
    errno = 0;
    v = strtod(buf, &end);
    if (!end || end == buf || *end) {
        return 0;
    }
    if (v == 0.0 && sign < 0) {
        v = copysign(0.0, -1.0);
    }
    *out = v;
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
    while (vm->ip != 0 && vm->rsp >= stop_rsp && !vm->quit_req) {
        int32_t xt;
        /* One eval. Yield every 2048 steps keeps Pause alive, so the
           browser does not treat a long loop as a hung tab. */
        if (++guard > 500000000) {
            vm->in_inner = prev;
            return fail(vm, -1, "step limit\n");
        }
#ifdef __EMSCRIPTEN__
        /* Let the page set Break Now while a long eval is still inside NEXT.
           The pause KEY wait already sleeps, so skip that nested case. */
        if (vm->dbg_yield && !vm->dbg_busy && (guard & 2047) == 0) {
            emscripten_sleep(1);
        }
#endif
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
/* ( xu ... x0 u -- xu-1 ... x0 xu ). u=0 is a no-op. */
static int p_roll(ef_vm *vm) {
    int32_t u, xu, i;
    if (pop(vm, &u)) {
        return EF_THROW;
    }
    if (u == 0) {
        return EF_OK;
    }
    if (u < 0 || u >= vm->sp) {
        return fail(vm, -3, "stack underflow\n");
    }
    xu = vm->stack[vm->sp - 1 - u];
    for (i = vm->sp - u; i < vm->sp; i++) {
        vm->stack[i - 1] = vm->stack[i];
    }
    vm->stack[vm->sp - 1] = xu;
    return EF_OK;
}
/* Orig and dest live on cf, one cell each. u comes from the data stack. */
static int p_cs_pick(ef_vm *vm) {
    int32_t n;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    if (n < 0 || n >= vm->cf_n) {
        return fail(vm, -3, "stack underflow\n");
    }
    return cf_push(vm, vm->cf[vm->cf_n - 1 - n]);
}
/* ( C: xu ... x0 u -- xu-1 ... x0 xu ). u=0 is a no-op. */
static int p_cs_roll(ef_vm *vm) {
    int32_t u, xu, i;
    if (pop(vm, &u)) {
        return EF_THROW;
    }
    if (u == 0) {
        return EF_OK;
    }
    if (u < 0 || u >= vm->cf_n) {
        return fail(vm, -3, "stack underflow\n");
    }
    xu = vm->cf[vm->cf_n - 1 - u];
    for (i = vm->cf_n - u; i < vm->cf_n; i++) {
        vm->cf[i - 1] = vm->cf[i];
    }
    vm->cf[vm->cf_n - 1] = xu;
    return EF_OK;
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

/* ( c-addr +n1 -- +n2 ). Stops at CR or LF. The delimiter is not stored. */
static int p_accept(ef_vm *vm) {
    int32_t n1, addr, n2, i;
    int c;
    if (pop(vm, &n1) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (n1 <= 0) {
        return push(vm, 0);
    }
    if (!vm->key) {
        return fail(vm, -21, "KEY is not connected\n");
    }
    if (!span_ok(vm, addr, n1)) {
        return fail(vm, -16, "bad address\n");
    }
    n2 = 0;
    for (i = 0; i < n1; i++) {
        c = vm->key(vm->key_ctx);
        if (c < 0) {
            break;
        }
        c &= 0xff;
        if (c == '\n' || c == '\r') {
            break;
        }
        vm->base[addr + i] = (uint8_t)c;
        n2++;
    }
    return push(vm, n2);
}

/* ( ud1 c-addr1 u1 -- ud2 c-addr2 u2 ). Stops on the first non-digit. */
static int p_to_number(ef_vm *vm) {
    int32_t u, addr, hi, lo;
    int base, i;
    uint64_t ud;
    if (pop(vm, &u) || pop(vm, &addr) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    if (u < 0 || !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    base = base_of(vm);
    ud = pack_d(lo, hi);
    i = 0;
    while (i < u) {
        int ch = vm->base[addr + i];
        int dig;
        if (ch >= '0' && ch <= '9') {
            dig = ch - '0';
        } else if (ch >= 'A' && ch <= 'Z') {
            dig = ch - 'A' + 10;
        } else if (ch >= 'a' && ch <= 'z') {
            dig = ch - 'a' + 10;
        } else {
            break;
        }
        if (dig >= base) {
            break;
        }
        ud = ud * (uint64_t)base + (uint64_t)dig;
        i++;
    }
    if (push_d(vm, ud) || push(vm, addr + i) || push(vm, u - i)) {
        return EF_THROW;
    }
    return EF_OK;
}

/* ( c-addr1 u1 c-addr2 u2 -- c-addr3 u3 flag ). An empty needle matches. */
static int p_search(ef_vm *vm) {
    int32_t u2, a2, u1, a1;
    int i, j;
    if (pop(vm, &u2) || pop(vm, &a2) || pop(vm, &u1) || pop(vm, &a1)) {
        return EF_THROW;
    }
    if (u1 < 0 || u2 < 0 || !span_ok(vm, a1, u1) || !span_ok(vm, a2, u2)) {
        return fail(vm, -16, "bad address\n");
    }
    if (u2 == 0) {
        return push(vm, a1) || push(vm, u1) || push(vm, -1) ? EF_THROW : EF_OK;
    }
    for (i = 0; i <= u1 - u2; i++) {
        for (j = 0; j < u2; j++) {
            if (vm->base[a1 + i + j] != vm->base[a2 + j]) {
                break;
            }
        }
        if (j == u2) {
            if (push(vm, a1 + i) || push(vm, u1 - i) || push(vm, -1)) {
                return EF_THROW;
            }
            return EF_OK;
        }
    }
    if (push(vm, a1) || push(vm, u1) || push(vm, 0)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_environment(ef_vm *vm) {
    static const struct {
        const char *name;
        int kind;
        int32_t value;
    } tab[] = {
        { "/COUNTED-STRING", 1, 255 },
        { "ADDRESS-UNIT-BITS", 1, 8 },
        { "CORE", 1, -1 },
        { "CORE-EXT", 1, -1 },
        { "FLOORED", 1, 0 },
        { "MAX-CHAR", 1, 255 },
        { "MAX-N", 1, 2147483647 },
        { "MAX-U", 1, -1 },
        { "RETURN-STACK-CELLS", 1, EF_RSTACK },
        { "STACK-CELLS", 1, EF_STACK },
        { "STRING", 1, -1 },
        { "FACILITY", 1, -1 },
        { "FACILITY-EXT", 1, -1 },
        { "LOCALS", 1, -1 },
        { "#LOCALS", 1, EF_LOCAL_MAX },
        { "EXTENDED-CHARACTER", 1, -1 },
        { "XCHAR-ENCODING", 2, 0 },
        { "MAX-XCHAR", 1, 0x10FFFF },
        { "XCHAR-MAXMEM", 1, 4 },
        { "DOUBLE", 1, -1 },
        { "EXCEPTION", 1, -1 },
        { "MEMORY-ALLOCATION", 1, -1 },
        { "SEARCH-ORDER", 1, -1 },
        { "WORDLISTS", 1, EF_ORDER_MAX },
        { "BLOCK", 1, -1 },
        { "FILE", 1, -1 },
        { "FILE-EXT", 1, -1 },
        { "TOOLS-EXT", 1, -1 },
    };
    int32_t u, addr;
    char q[64];
    size_t i;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u < 0 || u >= (int)sizeof q || !span_ok(vm, addr, u)) {
        return push(vm, 0);
    }
    if (u > 0) {
        memcpy(q, vm->base + addr, (size_t)u);
    }
    q[u] = 0;
    /* FLOATING and FLOAT-EXT are flags. FLOATING-STACK is the depth.
       MAX-FLOAT pushes the value on the float stack, then the found flag. */
    if (u == 8 && memcmp(q, "FLOATING", 8) == 0) {
        return push(vm, -1) || push(vm, -1) ? EF_THROW : EF_OK;
    }
    if (u == 9 && memcmp(q, "FLOAT-EXT", 9) == 0) {
        return push(vm, -1) || push(vm, -1) ? EF_THROW : EF_OK;
    }
    if (u == 14 && memcmp(q, "FLOATING-STACK", 14) == 0) {
        return push(vm, EF_FSTACK) || push(vm, -1) ? EF_THROW : EF_OK;
    }
    if (u == 9 && memcmp(q, "MAX-FLOAT", 9) == 0) {
        if (fpush(vm, DBL_MAX)) {
            return EF_THROW;
        }
        return push(vm, -1);
    }
    for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if ((int)strlen(tab[i].name) != u || memcmp(tab[i].name, q, (size_t)u) != 0) {
            continue;
        }
        if (tab[i].kind == 2) {
            if (push(vm, EF_ENV_UTF8) || push(vm, 5) || push(vm, -1)) {
                return EF_THROW;
            }
            return EF_OK;
        }
        if (push(vm, tab[i].value) || push(vm, -1)) {
            return EF_THROW;
        }
        return EF_OK;
    }
    return push(vm, 0);
}

/* Paint pending console output and return. One millisecond lets the browser draw the line. */
static int p_pause(ef_vm *vm) {
    (void)vm;
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof Module._forthPump === "function") Module._forthPump();
    });
    emscripten_sleep(1);
#else
    fflush(stdout);
#endif
    return EF_OK;
}

static int p_ms(ef_vm *vm) {
    int32_t ms;
    if (pop(vm, &ms)) {
        return EF_THROW;
    }
    if (ms <= 0) {
        return EF_OK;
    }
#ifdef __EMSCRIPTEN__
    emscripten_sleep((unsigned)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) != 0) {
            if (errno != EINTR) {
                break;
            }
        }
    }
#endif
    return EF_OK;
}

/* ( -- sec min hour day month year ). Local wall time. */
static int p_time_date(ef_vm *vm) {
    time_t now;
    struct tm tm;
    int sec = 0, min = 0, hour = 0, day = 1, month = 1, year = 1970;
    now = time(NULL);
    if (now != (time_t)-1 && localtime_r(&now, &tm)) {
        sec = tm.tm_sec;
        min = tm.tm_min;
        hour = tm.tm_hour;
        day = tm.tm_mday;
        month = tm.tm_mon + 1;
        year = tm.tm_year + 1900;
    }
    if (push(vm, sec) || push(vm, min) || push(vm, hour) || push(vm, day) ||
        push(vm, month) || push(vm, year)) {
        return EF_THROW;
    }
    return EF_OK;
}

/* The page console is a transcript. PAGE clears it. AT-XY records the cell. */
static int p_page(ef_vm *vm) {
    vm->fac_col = 0;
    vm->fac_row = 0;
    emit_ch(vm, '\f');
    return EF_OK;
}

static int p_at_xy(ef_vm *vm) {
    int32_t col, row;
    if (pop(vm, &row) || pop(vm, &col)) {
        return EF_THROW;
    }
    if (col < 0) {
        col = 0;
    }
    if (row < 0) {
        row = 0;
    }
    vm->fac_col = col;
    vm->fac_row = row;
    return EF_OK;
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
/* (C") ( -- c-addr ) c-addr is the length byte of the inline counted string. */
static int p_c_quote_rt(ef_vm *vm) {
    int32_t addr;
    if (vm->ip < 0 || (size_t)vm->ip >= vm->count) {
        return fail(vm, -16, "bad address\n");
    }
    addr = vm->ip;
    vm->ip = skip_counted(vm, vm->ip);
    return push(vm, addr);
}

static int p_colon(ef_vm *vm) {
    char name[128];
    int n;
    int32_t cfa;
    if (uv(vm, UV_STATE) != 0) {
        return fail(vm, -14, "already compiling\n");
    }
    vm->noname_xt = 0;
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
/* ( -- xt ) once ; finishes the definition. The name is empty and unlinked. */
static int p_noname(ef_vm *vm) {
    int32_t cfa;
    if (uv(vm, UV_STATE) != 0) {
        return fail(vm, -14, "already compiling\n");
    }
    vm->cf_n = 0;
    vm->begin_n = 0;
    vm->loop_n = 0;
    cfa = header(vm, "", 0, EF_DOCOL);
    if (!cfa) {
        return EF_THROW;
    }
    set_smudge(vm, cfa, 1);
    vm->defining = cfa;
    vm->noname_xt = cfa;
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
    if (vm->noname_xt) {
        int32_t xt = vm->noname_xt;
        vm->noname_xt = 0;
        return push(vm, xt);
    }
    return EF_OK;
}
/* Empty the return stack and stop this eval. The data stack stays. */
static int p_quit(ef_vm *vm) {
    unlink_defining(vm);
    uv_set(vm, UV_STATE, 0);
    vm->cf_n = 0;
    vm->begin_n = 0;
    vm->loop_n = 0;
    vm->rsp = 0;
    vm->ip = 0;
    vm->frame_n = 0;
    vm->noname_xt = 0;
    vm->source_id = 0;
    vm->quit_req = 1;
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
    return cf_push(vm, uv(vm, UV_DP));
}
static int p_until(ef_vm *vm) {
    int32_t dest;
    if (compile_only(vm, "UNTIL")) {
        return EF_THROW;
    }
    if (vm->cf_n <= 0) {
        return fail(vm, -22, "UNTIL without BEGIN\n");
    }
    dest = vm->cf[--vm->cf_n];
    return plant_back(vm, vm->xt_zbranch, dest);
}
static int p_again(ef_vm *vm) {
    int32_t dest;
    if (compile_only(vm, "AGAIN")) {
        return EF_THROW;
    }
    if (vm->cf_n <= 0) {
        return fail(vm, -22, "AGAIN without BEGIN\n");
    }
    dest = vm->cf[--vm->cf_n];
    return plant_back(vm, vm->xt_branch, dest);
}
/* ( dest -- orig dest ). The dest stays so several WHILEs can share it. */
static int p_while(ef_vm *vm) {
    int32_t dest, ph;
    if (compile_only(vm, "WHILE")) {
        return EF_THROW;
    }
    if (vm->cf_n <= 0) {
        return fail(vm, -22, "WHILE without BEGIN\n");
    }
    if (vm->cf_n >= EF_CF) {
        return fail(vm, -16, "control stack overflow\n");
    }
    dest = vm->cf[--vm->cf_n];
    if (comma(vm, vm->xt_zbranch)) {
        cf_push(vm, dest);
        return EF_THROW;
    }
    ph = uv(vm, UV_DP);
    if (comma(vm, 0) || cf_push(vm, ph) || cf_push(vm, dest)) {
        cf_push(vm, dest);
        return EF_THROW;
    }
    return EF_OK;
}
/* ( orig dest -- ). */
static int p_repeat(ef_vm *vm) {
    int32_t ph, dest;
    if (compile_only(vm, "REPEAT")) {
        return EF_THROW;
    }
    if (vm->cf_n < 2) {
        if (vm->cf_n == 0) {
            return fail(vm, -22, "REPEAT without BEGIN\n");
        }
        return fail(vm, -16, "REPEAT without WHILE\n");
    }
    dest = vm->cf[--vm->cf_n];
    ph = vm->cf[--vm->cf_n];
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

/* Next line of a high-level INCLUDE. 1 = a line is in SOURCE, 0 = no more,
   -1 = throw. A console, EVALUATE, or cold-image buffer is not a line load. */
static int load_next_line(ef_vm *vm) {
    const char *t;
    int pos, len, start, n, line;
    if (!vm->load_on || !vm->load_text || vm->load_stop) {
        return 0;
    }
    t = vm->load_text;
    len = vm->load_len;
    pos = vm->load_pos;
    if (pos < 0) {
        pos = 0;
    }
    if (pos >= len) {
        return 0;
    }
    line = vm->load_line > 0 ? vm->load_line : 1;
    start = pos;
    while (pos < len && t[pos] != '\n' && t[pos] != '\r') {
        pos++;
    }
    n = pos - start;
    if (pos < len && t[pos] == '\r') {
        pos++;
        if (pos < len && t[pos] == '\n') {
            pos++;
        }
    } else if (pos < len && t[pos] == '\n') {
        pos++;
    }
    vm->load_pos = pos;
    vm->load_line_at = start;
    vm->load_line = line + 1;
    if (n > EF_SRC_MAX - 1) {
        return fail(vm, -16, "input too long\n"), -1;
    }
    if (ensure(vm, SRC_ADDR + n + 1)) {
        return -1;
    }
    if (n > 0) {
        memcpy(vm->base + SRC_ADDR, t + start, (size_t)n);
    }
    vm->base[SRC_ADDR + n] = 0;
    vm->src_at = SRC_ADDR;
    vm->src_len = n;
    uv_set(vm, UV_IN, 0);
    vm->line = line;
    vm->file_echo_pos = 0;
    return 1;
}

/* Stop the rest of this load. On one line of SOURCE, end-of-buffer would
   only skip that line and REFILL would keep going. */
static int p_backslash_s(ef_vm *vm) {
    uv_set(vm, UV_IN, vm->src_len);
    if (vm->load_on) {
        vm->load_stop = 1;
    }
    return EF_OK;
}
static int p_paren(ef_vm *vm) {
    for (;;) {
        int in = uv(vm, UV_IN);
        int len = vm->src_len;
        uint8_t *s = src_ptr(vm);
        int got;
        while (in < len && s[in] != ')') {
            if (s[in] == '\n') {
                vm->line++;
            }
            in++;
        }
        if (in < len && s[in] == ')') {
            uv_set(vm, UV_IN, in + 1);
            return EF_OK;
        }
        uv_set(vm, UV_IN, in);
        if (!vm->load_on) {
            return fail(vm, -16, "missing delimiter\n");
        }
        got = load_next_line(vm);
        if (got < 0) {
            return EF_THROW;
        }
        if (!got) {
            return fail(vm, -16, "missing delimiter\n");
        }
    }
}
/* ." S" and C" drop one leading blank. Further blanks stay in the string. */
static void drop_first_blank(char *buf, int *n) {
    if (*n > 0 && buf[0] == ' ') {
        memmove(buf, buf + 1, (size_t)(*n));
        *n -= 1;
    }
}

/* Interpreting S" must not allot at HERE. [ S" hi" ] runs while a colon
   body is still the dictionary pointer, and planting there becomes an xt.
   <# writes downward from PAD+256, so the three pockets start there.
   The next S" uses the next pocket, which keeps two strings live. */
#define S_POCKET_N 3
#define S_POCKET_LEN 256
static int32_t s_pocket_addr(ef_vm *vm) {
    int slot = vm->s_pocket % S_POCKET_N;
    vm->s_pocket++;
    return PAD_ADDR + 256 + slot * S_POCKET_LEN;
}

static int compile_or_type_string(ef_vm *vm, int32_t rt, int do_type, int do_push) {
    char buf[4096];
    int n;
    int32_t addr;
    int compiling = uv(vm, UV_STATE) != 0;
    if (parse_until(vm, do_type && rt == vm->xt_dotparen ? ')' : '"', buf, sizeof buf, &n)) {
        return EF_THROW;
    }
    drop_first_blank(buf, &n);
    if (!compiling) {
        if (do_type) {
            int i;
            for (i = 0; i < n; i++) {
                emit_ch(vm, buf[i]);
            }
        }
        if (do_push) {
            int i;
            if (n < 0 || n > 255) {
                return fail(vm, -16, "string too long\n");
            }
            addr = s_pocket_addr(vm);
            if (ensure(vm, addr + n)) {
                return EF_THROW;
            }
            for (i = 0; i < n; i++) {
                vm->base[addr + i] = (uint8_t)buf[i];
            }
            if (push(vm, addr) || push(vm, n)) {
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

/* S\"  (the name is S, backslash, quote). One trailing blank is already
   consumed, so the first character after that is part of the string.
   \x takes one or two hex digits. \m is CR LF. \n is LF. */
static int scan_escaped(ef_vm *vm, char *buf, int *outn) {
    int in = uv(vm, UV_IN);
    int len = vm->src_len;
    uint8_t *s = src_ptr(vm);
    int n = 0;
    while (in < len) {
        unsigned char c = s[in++];
        int ch;
        if (c == '"') {
            uv_set(vm, UV_IN, in);
            *outn = n;
            return EF_OK;
        }
        if (c == '\n') {
            vm->line++;
        }
        if (c != '\\') {
            if (n >= 255) {
                return fail(vm, -16, "string too long\n");
            }
            buf[n++] = (char)c;
            continue;
        }
        if (in >= len) {
            uv_set(vm, UV_IN, in);
            return fail(vm, -16, "missing delimiter\n");
        }
        c = s[in++];
        if (c == 'm') {
            if (n + 2 > 255) {
                return fail(vm, -16, "string too long\n");
            }
            buf[n++] = 13;
            buf[n++] = 10;
            continue;
        }
        if (c == 'x' || c == 'X') {
            int val = 0;
            int digits = 0;
            int d;
            while (digits < 2 && in < len && (d = digit_val(s[in])) >= 0 && d < 16) {
                val = (val << 4) + d;
                in++;
                digits++;
            }
            if (n >= 255) {
                return fail(vm, -16, "string too long\n");
            }
            buf[n++] = (char)(digits ? val : 0);
            continue;
        }
        switch (c) {
        case 'a': ch = 7; break;
        case 'b': ch = 8; break;
        case 'e': ch = 27; break;
        case 'f': ch = 12; break;
        case 'l': ch = 10; break;
        case 'n': ch = 10; break;
        case 'q': ch = 34; break;
        case 'r': ch = 13; break;
        case 't': ch = 9; break;
        case 'v': ch = 11; break;
        case 'z': ch = 0; break;
        case '"': ch = 34; break;
        case '\\': ch = 92; break;
        default: ch = (int)c; break;
        }
        if (n >= 255) {
            return fail(vm, -16, "string too long\n");
        }
        buf[n++] = (char)ch;
    }
    uv_set(vm, UV_IN, in);
    return fail(vm, -16, "missing delimiter\n");
}
static int p_s_escape(ef_vm *vm) {
    char buf[256];
    int n, i;
    int32_t addr;
    int compiling = uv(vm, UV_STATE) != 0;
    if (scan_escaped(vm, buf, &n)) {
        return EF_THROW;
    }
    if (!compiling) {
        addr = s_pocket_addr(vm);
        if (ensure(vm, addr + n)) {
            return EF_THROW;
        }
        for (i = 0; i < n; i++) {
            vm->base[addr + i] = (uint8_t)buf[i];
        }
        if (push(vm, addr) || push(vm, n)) {
            return EF_THROW;
        }
        return EF_OK;
    }
    if (!vm->xt_squote) {
        return fail(vm, -21, "S\\\" is not ready\n");
    }
    if (comma(vm, vm->xt_squote) || plant_string(vm, buf, n, NULL)) {
        return EF_THROW;
    }
    return EF_OK;
}
/* C" ( -- c-addr ) counted string. One leading blank is dropped, as with S". */
static int p_c_quote(ef_vm *vm) {
    char buf[4096];
    int n, i;
    if (parse_until(vm, '"', buf, (int)sizeof buf, &n)) {
        return EF_THROW;
    }
    drop_first_blank(buf, &n);
    if (n > 255) {
        n = 255;
    }
    if (uv(vm, UV_STATE) == 0) {
        if (ensure(vm, PAD_ADDR + 1 + n)) {
            return EF_THROW;
        }
        vm->base[PAD_ADDR] = (uint8_t)n;
        for (i = 0; i < n; i++) {
            vm->base[PAD_ADDR + 1 + i] = (uint8_t)buf[i];
        }
        return push(vm, PAD_ADDR);
    }
    if (!vm->xt_cquote) {
        return fail(vm, -21, "C\" is not ready\n");
    }
    if (comma(vm, vm->xt_cquote) || plant_string(vm, buf, n, NULL)) {
        return EF_THROW;
    }
    return EF_OK;
}
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
    /* Run to completion. A nested INCLUDE is already inside an outer colon,
       so vm->in_inner is 1, but this ip belongs to that colon. Nesting the
       xt there would run it after the file returns. */
    return exec_one(vm, cfa, 0);
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
    s = src_ptr(vm);
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
        if (xt == vm->xt_flit) {
            ip += 2 * CELL;
            emit_str(vm, "FLIT ");
            continue;
        }
        if (xt == vm->xt_dotq || xt == vm->xt_squote || xt == vm->xt_cquote) {
            int sn = vm->base[ip];
            int i;
            emit_str(vm, xt == vm->xt_dotq ? ".\" " : xt == vm->xt_cquote ? "C\" " : "S\" ");
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

/* Replace the first-searched wordlist. The depth stays, unless it was empty. */
static int p_forth(ef_vm *vm) {
    int n = uv(vm, UV_ORDERN);
    if (n <= 0) {
        uv_set(vm, UV_ORDERN, 1);
    }
    uv_set(vm, UV_ORDER, vm->forth_wid);
    return EF_OK;
}
static int p_definitions(ef_vm *vm) {
    uv_set(vm, UV_CURRENT, uv(vm, UV_ORDER));
    return EF_OK;
}
/* Print the search order, first-searched first, then the compilation wordlist. */
static int p_order(ef_vm *vm) {
    int n = uv(vm, UV_ORDERN);
    int i;
    if (n < 0) {
        n = 0;
    }
    if (n > EF_ORDER_MAX) {
        n = EF_ORDER_MAX;
    }
    emit_str(vm, "Search order: ");
    for (i = 0; i < n; i++) {
        if (i) {
            emit_ch(vm, ' ');
        }
        emit_wid_name(vm, uv(vm, UV_ORDER + i * CELL));
    }
    emit_ch(vm, '\n');
    emit_str(vm, "Compilation wordlist: ");
    emit_wid_name(vm, uv(vm, UV_CURRENT));
    emit_ch(vm, '\n');
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

/* File words may name /tmp/.... and a volume path that contains spaces
   (~/Library/Application Support/...). INCLUDE uses path_ok and still
   rejects spaces, quotes, and a leading slash. */
static int file_path_ok(const char *path) {
    const char *p;
    int absolute;
    if (!path || !path[0] || strlen(path) > 500) {
        return 0;
    }
    absolute = path[0] == '/';
    if (!absolute && path[0] == '\\') {
        return 0;
    }
    p = path;
    if (absolute) {
        while (*p) {
            const char *start;
            size_t n;
            if (*p == '/') {
                p++;
                continue;
            }
            start = p;
            while (*p && *p != '/') {
                if (*p == '"' || *p == '\'') {
                    return 0;
                }
                p++;
            }
            n = (size_t)(p - start);
            if (n == 1 && start[0] == '.') {
                return 0;
            }
            if (n == 2 && start[0] == '.' && start[1] == '.') {
                return 0;
            }
        }
        return 1;
    }
    while (*p) {
        const char *start = p;
        size_t n;
        while (*p && *p != '/' && *p != '\\') {
            if (*p == '"' || *p == '\'') {
                return 0;
            }
            p++;
        }
        n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.')) {
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

int ef_set_source_line(ef_vm *vm, int line) {
    if (!vm) {
        return -1;
    }
    vm->src_line = line > 0 ? line : 1;
    vm->src_line_set = 1;
    return 0;
}

static void remember_view(ef_vm *vm, int id, const char *path);

/* Reuse the view id already stored for this path. A console transcript and
   an editor buffer stay one file across many evals, so Pause can map a token
   back to the line that compiled it. */
static int view_id_named(const ef_vm *vm, const char *path) {
    int i;
    if (!path || !path[0]) {
        return 0;
    }
    for (i = 0; i < vm->nviews; i++) {
        if (strcmp(vm->views[i].path, path) == 0) {
            return vm->views[i].id;
        }
    }
    return 0;
}

static void stamp_source(ef_vm *vm) {
    int id;
    if (!vm->src_name[0]) {
        return;
    }
    id = view_id_named(vm, vm->src_name);
    if (id) {
        vm->file_id = id;
        return;
    }
    vm->file_id = vm->next_file++;
    if (vm->next_file > 0x1fff) {
        vm->next_file = EF_VIEW_C + 1;
    }
    remember_view(vm, vm->file_id, vm->src_name);
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
        vm->next_file = EF_VIEW_C + 1;
    }
    remember_view(vm, vm->file_id, resolved);
    vm->echo_file = 1;
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
    if (*nc >= 8) {
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
    char cands[8][512];
    char joined[512];
    int nc = 0;
    int i;
    static const char libprefix[] = "Resources/Library/";
    static const char sysprefix[] = "Resources/System/";
    *owned = NULL;
    *len = 0;
    resolved[0] = 0;
    add_cand(cands, &nc, name);
    /* FROMLIB plants Resources/Library/name or Resources/System/name.
       An open folder mounts the same file as name, with no prefix.
       Library/ and System/ are the same roots without Resources/. */
    if (strncmp(name, libprefix, sizeof libprefix - 1) == 0) {
        add_cand(cands, &nc, name + (sizeof libprefix - 1));
    }
    if (strncmp(name, sysprefix, sizeof sysprefix - 1) == 0) {
        add_cand(cands, &nc, name + (sizeof sysprefix - 1));
    }
    if (strncmp(name, "Library/", 8) == 0 || strncmp(name, "System/", 7) == 0) {
        snprintf(joined, sizeof joined, "Resources/%s", name);
        add_cand(cands, &nc, joined);
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
    if (vm->fetch_fn) {
        for (i = 0; i < nc; i++) {
            if (vm->fetch_fn(vm->fetch_ctx, cands[i], owned, len) == 0 && *owned) {
                snprintf(resolved, rmax, "%s", cands[i]);
                return 1;
            }
            free(*owned);
            *owned = NULL;
            *len = 0;
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
    ef_blob *b = blob_find(vm, name);
    /* CREATE-FILE / WRITE-FILE keep a session copy. REQUIRED must see it,
       including a /tmp name. resolve_file still rejects a leading slash,
       so INCLUDE of an absolute path is unchanged. */
    if (b) {
        return include_text(vm, name, b->data ? b->data : "");
    }
    if (!resolve_file(vm, name, resolved, sizeof resolved, &owned, &len)) {
#ifndef __EMSCRIPTEN__
        if (name && name[0] == '/' && file_path_ok(name) && read_disk(name, &owned) == 0) {
            rc = include_text(vm, name, owned ? owned : "");
            free(owned);
            return rc;
        }
#endif
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

/* 5-wide right-aligned line number, then "| ". Wider numbers print in full. */
static void emit_lineno(ef_vm *vm, int line) {
    char buf[16];
    int i;
    if (line < 0) {
        line = 0;
    }
    snprintf(buf, sizeof buf, "%5d| ", line);
    for (i = 0; buf[i]; i++) {
        emit_ch(vm, buf[i]);
    }
}

/* Echo INCLUDE/FLOAD lines through the line that holds the next token.
   Blank lines the parser skipped are included. Off, or a console / cold
   SOURCE, prints nothing. Does not move >IN. */
static void file_echo_upto(ef_vm *vm) {
    int32_t flag = 0;
    int end, cursor, target, eol, pos;
    uint8_t *s;
    if (!vm->src_echo || !vm->file_echo_addr) {
        return;
    }
    if (load_cell(vm, vm->file_echo_addr, &flag) || flag == 0) {
        return;
    }
    end = vm->src_len;
    if (end < 0) {
        return;
    }
    /* A blank included line has no bytes and no newline to scan. */
    if (vm->load_on && end == 0) {
        if (vm->file_echo_pos != 0) {
            return;
        }
        emit_lineno(vm, vm->line > 0 ? vm->line : 1);
        emit_ch(vm, '\n');
        vm->file_echo_pos = 1;
        return;
    }
    s = src_ptr(vm);
    cursor = uv(vm, UV_IN);
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor > end) {
        cursor = end;
    }
    target = cursor;
    while (target < end && s[target] && is_white(s[target])) {
        target++;
    }
    eol = target;
    while (eol < end && s[eol] && s[eol] != '\n' && s[eol] != '\r') {
        eol++;
    }
    pos = vm->file_echo_pos;
    if (pos < 0) {
        pos = 0;
    }
    if (pos > end) {
        pos = end;
    }
    while (pos < eol) {
        int at_break = pos == 0;
        int this_eol, next, i, line;
        if (!at_break) {
            uint8_t prev = s[pos - 1];
            at_break = prev == '\n' || prev == '\r';
        }
        if (at_break) {
            if (vm->load_on) {
                line = vm->line > 0 ? vm->line : 1;
            } else {
                line = 1;
                for (i = 0; i < pos; i++) {
                    if (s[i] == '\n') {
                        line++;
                    }
                }
            }
            emit_lineno(vm, line);
        }
        this_eol = pos;
        while (this_eol < end && s[this_eol] && s[this_eol] != '\n' && s[this_eol] != '\r') {
            this_eol++;
        }
        for (i = pos; i < this_eol; i++) {
            emit_ch(vm, s[i]);
        }
        next = this_eol;
        if (this_eol < end) {
            if (s[this_eol] == '\n') {
                next = this_eol + 1;
            } else if (s[this_eol] == '\r') {
                next = this_eol + 1;
                if (next < end && s[next] == '\n') {
                    next++;
                }
            }
        }
        pos = next;
        vm->file_echo_pos = pos;
        emit_ch(vm, '\n');
    }
}

static int interpret(ef_vm *vm) {
    char name[EF_WORD_MAX];
    int n;
    /* A line load already stored the file line. A whole buffer starts at 1
       unless this eval named a line in an accumulated source file. */
    if (!vm->load_on) {
        if (!vm->hold_line) {
            vm->line = 1;
        }
        vm->hold_line = 0;
    }
    while (!vm->throw_code) {
        if (vm->quit_req) {
            return EF_OK;
        }
        file_echo_upto(vm);
        if (!next_name(vm, name, sizeof name, &n)) {
            if (vm->load_on && !vm->load_stop) {
                int got = load_next_line(vm);
                if (got < 0) {
                    return EF_THROW;
                }
                if (got) {
                    continue;
                }
            }
            break;
        }
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
            int32_t num, hi = 0;
            int kind = parse_literal(name, n, base_of(vm), &num, &hi);
            double fval = 0.0;
            int is_float = 0;
            if (!kind) {
                int ei;
                for (ei = 0; ei < n; ei++) {
                    if (is_exp_mark((unsigned char)name[ei])) {
                        is_float = parse_float(name, n, &fval);
                        break;
                    }
                }
            }
            if (!kind && !is_float) {
                emit_str(vm, "undefined: ");
                emit_str(vm, name);
                emit_ch(vm, '\n');
                vm->throw_code = -13;
                return EF_THROW;
            }
            if (is_float) {
                if (compiling) {
                    uint64_t bits = 0;
                    memcpy(&bits, &fval, 8);
                    if (!vm->xt_flit ||
                        comma(vm, vm->xt_flit) ||
                        comma(vm, (int32_t)(uint32_t)(bits >> 32)) ||
                        comma(vm, (int32_t)(uint32_t)bits)) {
                        return EF_THROW;
                    }
                } else if (fpush(vm, fval)) {
                    return EF_THROW;
                }
            } else
            if (kind == 2) {
                if (compiling) {
                    if (comma(vm, vm->xt_lit) || comma(vm, num) ||
                        comma(vm, vm->xt_lit) || comma(vm, hi)) {
                        return EF_THROW;
                    }
                } else if (push(vm, num) || push(vm, hi)) {
                    return EF_THROW;
                }
            } else if (compiling) {
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
    int echo_pos;
    int src_echo;
    int source_id;
    int32_t src_at;
} src_frame;

static int save_source(ef_vm *vm, src_frame *f) {
    f->len = vm->src_len;
    f->in = uv(vm, UV_IN);
    f->line = vm->line;
    f->file_id = vm->file_id;
    f->defining = vm->defining;
    f->echo_pos = vm->file_echo_pos;
    f->src_echo = vm->src_echo;
    f->source_id = vm->source_id;
    f->src_at = vm->src_at;
    f->text = NULL;
    if (f->len > 0) {
        f->text = malloc((size_t)f->len);
        if (!f->text) {
            return -1;
        }
        memcpy(f->text, vm->base + f->src_at, (size_t)f->len);
    }
    return 0;
}

static void restore_source(ef_vm *vm, src_frame *f) {
    if (f->text && f->len > 0) {
        if (ensure(vm, f->src_at + f->len) == EF_OK) {
            memcpy(vm->base + f->src_at, f->text, (size_t)f->len);
        }
    }
    vm->src_at = f->src_at;
    vm->src_len = f->len;
    uv_set(vm, UV_IN, f->in);
    vm->line = f->line;
    vm->file_id = f->file_id;
    vm->file_echo_pos = f->echo_pos;
    vm->src_echo = f->src_echo;
    vm->source_id = f->source_id;
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
/* PARSE returns the address in the input buffer. A missing delimiter is
   the rest of the parse area, not an error. S" still requires its quote. */
static int p_parse(ef_vm *vm) {
    int32_t delim;
    int in, len, start, n;
    uint8_t *s;
    unsigned char end;
    if (pop(vm, &delim)) {
        return EF_THROW;
    }
    end = (unsigned char)(delim & 0xff);
    in = uv(vm, UV_IN);
    len = vm->src_len;
    s = src_ptr(vm);
    start = in;
    while (in < len && s[in] != end) {
        if (s[in] == '\n') {
            vm->line++;
        }
        in++;
    }
    n = in - start;
    if (in < len && s[in] == end) {
        in++;
    }
    uv_set(vm, UV_IN, in);
    if (push(vm, vm->src_at + start) || push(vm, n)) {
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
    /* -1 is the minimum search order: FORTH-WORDLIST alone, same as ONLY. */
    if (n == -1) {
        uv_set(vm, UV_ORDER, vm->forth_wid);
        uv_set(vm, UV_ORDERN, 1);
        return EF_OK;
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
    uint8_t *s = src_ptr(vm);
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

/* Session copies so CLOSE then OPEN still sees CREATE-FILE / WRITE-LINE.
   DELETE and RENAME change this list. They do not unlink a host file. */
static ef_blob *blob_find(ef_vm *vm, const char *path) {
    ef_blob *b;
    for (b = vm->blobs; b; b = b->next) {
        if (strcmp(b->path, path) == 0) {
            return b;
        }
    }
    return NULL;
}

static ef_blob *blob_touch(ef_vm *vm, const char *path) {
    ef_blob *b = blob_find(vm, path);
    if (b) {
        return b;
    }
    b = calloc(1, sizeof *b);
    if (!b) {
        return NULL;
    }
    snprintf(b->path, sizeof b->path, "%s", path);
    b->next = vm->blobs;
    vm->blobs = b;
    return b;
}

static int blob_store(ef_vm *vm, const char *path, const char *data, int len) {
    ef_blob *b;
    if (!path || !path[0] || len < 0) {
        return -1;
    }
    b = blob_touch(vm, path);
    if (!b) {
        return -1;
    }
    if (len + 1 > b->cap) {
        char *p = realloc(b->data, (size_t)len + 1);
        if (!p) {
            return -1;
        }
        b->data = p;
        b->cap = len + 1;
    }
    if (len > 0 && data) {
        memcpy(b->data, data, (size_t)len);
    }
    if (b->data) {
        b->data[len] = 0;
    }
    b->len = len;
    return 0;
}

static void blob_drop(ef_vm *vm, const char *path) {
    ef_blob **pp = &vm->blobs;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0) {
            ef_blob *b = *pp;
            *pp = b->next;
            free(b->data);
            free(b);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void sync_slot(ef_vm *vm, int s) {
    if (s < 0 || !vm->files[s].used || !vm->files[s].path[0]) {
        return;
    }
    blob_store(vm, vm->files[s].path, vm->files[s].data, vm->files[s].len);
}

static int slot_load(ef_vm *vm, const char *path, const char *data, int len) {
    int slot;
    char *owned = NULL;
    if (len < 0 || copy_owned(data ? data : "", len, &owned)) {
        return -1;
    }
    slot = alloc_slot(vm);
    if (slot < 0) {
        free(owned);
        return -1;
    }
    vm->files[slot].used = 1;
    vm->files[slot].data = owned;
    vm->files[slot].len = len;
    vm->files[slot].cap = owned ? len + 1 : 0;
    vm->files[slot].pos = 0;
    snprintf(vm->files[slot].path, sizeof vm->files[slot].path, "%s", path);
    snprintf(vm->last_open, sizeof vm->last_open, "%s", path);
    return slot;
}

static int file_known(ef_vm *vm, const char *name, int *len) {
    ef_blob *b = blob_find(vm, name);
    char resolved[512];
    char *owned = NULL;
    int n = 0;
    if (b) {
        *len = b->len;
        return 1;
    }
    if (path_ok(name) && resolve_file(vm, name, resolved, sizeof resolved, &owned, &n)) {
        *len = n;
        free(owned);
        return 1;
    }
#ifndef __EMSCRIPTEN__
    {
        struct stat st;
        if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_size > 0x7fffffff) {
                *len = 0x7fffffff;
            } else {
                *len = (int)st.st_size;
            }
            return 1;
        }
    }
#endif
    return 0;
}

static int file_put(ef_vm *vm, int s, const char *bytes, int n) {
    int need;
    if (n < 0) {
        return 1;
    }
    need = vm->files[s].pos + n;
    if (n > 0 && file_grow(vm, s, need)) {
        return 1;
    }
    if (n > 0) {
        memcpy(vm->files[s].data + vm->files[s].pos, bytes, (size_t)n);
        vm->files[s].pos += n;
        if (vm->files[s].pos > vm->files[s].len) {
            vm->files[s].len = vm->files[s].pos;
        }
    }
    return 0;
}

static int p_pad(ef_vm *vm) { return push(vm, PAD_ADDR); }
static int p_unused(ef_vm *vm) { return push(vm, HEAP_ORIGIN - uv(vm, UV_DP)); }
static int p_to_in(ef_vm *vm) { return push(vm, UV_IN); }
static int p_source(ef_vm *vm) { return push2(vm, vm->src_at, vm->src_len); }
static int p_source_id(ef_vm *vm) { return push(vm, vm->source_id); }
static int refill_block(ef_vm *vm);
static int reload_block(ef_vm *vm, int32_t blk);
static int p_refill(ef_vm *vm) {
    int got;
    if (vm->load_on && vm->load_blk >= 0) {
        return refill_block(vm);
    }
    if (!vm->load_on) {
        return push(vm, 0);
    }
    got = load_next_line(vm);
    if (got < 0) {
        return EF_THROW;
    }
    return push(vm, got ? -1 : 0);
}

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
static int p_key_q(ef_vm *vm) {
    int flag = 0;
    static int calls;
    /* PAUSE itself still yields every time. KEY? yields on every 10th call. */
    if (++calls >= 10) {
        calls = 0;
        if (p_pause(vm)) {
            return EF_THROW;
        }
    }
    if (vm->key_q && vm->key_q(vm->key_q_ctx)) {
        flag = -1;
    }
    return push(vm, flag);
}
static int p_latest(ef_vm *vm) { return push(vm, uv(vm, UV_CURRENT)); }
static int p_evaluate(ef_vm *vm) {
    src_frame saved;
    int32_t u, addr, saved_blk = 0;
    int rc;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    /* SOURCE inside EVALUATE is this string, not a copy. Hayes GS1
       compares the addresses. */
    if (u < 0 || u >= EF_SRC_MAX || !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    if (save_source(vm, &saved)) {
        return fail(vm, -8, "out of memory\n");
    }
    vm->src_at = addr;
    vm->src_len = (int)u;
    uv_set(vm, UV_IN, 0);
    vm->line = 1;
    vm->file_echo_pos = 0;
    vm->src_echo = 0;
    vm->source_id = -1;
    if (vm->blk_addr) {
        load_cell(vm, vm->blk_addr, &saved_blk);
        store_cell(vm, vm->blk_addr, 0);
    }
    {
        int saved_load = vm->load_on;
        vm->load_on = 0;
        rc = interpret(vm);
        vm->load_on = saved_load;
    }
    restore_source(vm, &saved);
    if (vm->blk_addr) {
        store_cell(vm, vm->blk_addr, saved_blk);
    }
    if (rc != EF_OK) {
        if (!vm->throw_code) {
            vm->throw_code = -1;
        }
        return EF_THROW;
    }
    return EF_OK;
}

static int p_get_current(ef_vm *vm) { return push(vm, uv(vm, UV_CURRENT)); }
static int p_set_current(ef_vm *vm) {
    int32_t wid;
    if (pop(vm, &wid)) {
        return EF_THROW;
    }
    if (wid < 0 || (wid & 3) || (size_t)wid + (size_t)EF_THREADS * CELL > vm->count) {
        return fail(vm, -16, "bad wordlist\n");
    }
    uv_set(vm, UV_CURRENT, wid);
    return EF_OK;
}
static int p_redef(ef_vm *vm) { return push(vm, UV_WARN); }
static int p_block_file(ef_vm *vm) { return push(vm, UV_BLOCK_FILE); }
static int p_block_upd(ef_vm *vm) { return push(vm, UV_BLOCK_UPD); }
static int p_block_nr(ef_vm *vm) { return push(vm, UV_BLOCK_NR); }
static int p_scr(ef_vm *vm) { return push(vm, UV_SCR); }
static int p_block_buf(ef_vm *vm) { return push(vm, BLOCK_BUF_ADDR); }
static int p_ro(ef_vm *vm) { return push(vm, 1); }
static int p_wo(ef_vm *vm) { return push(vm, 2); }
static int p_rw(ef_vm *vm) { return push(vm, 3); }
static int p_bin(ef_vm *vm) {
    int32_t fam;
    if (pop(vm, &fam)) {
        return EF_THROW;
    }
    return push(vm, fam | 8);
}
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
    if (xt == vm->xt_flit) {
        emit_str(vm, " FLIT");
        return;
    }
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

/* S starts at column 23 (after ">> " and a 20-column name). The S field is
   45 columns, then R. A line already past a column gets one space instead.
   Same columns as EditForth DEBUG_STACK_COL / the 45-wide S field. */
enum { DBG_STACK_COL = 23, DBG_S_WIDTH = 45 };

static void dbg_spaces(ef_vm *vm, int n) {
    while (n-- > 0) {
        emit_ch(vm, ' ');
    }
}

static void dbg_pad_column(ef_vm *vm, int col) {
    int at = hook_get(vm, "DBG-LINE-COL", vm->dbg_line_col);
    if (at < col) {
        dbg_spaces(vm, col - at);
    } else if (at > col) {
        emit_ch(vm, ' ');
    }
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
    int scol;
    int slen = 0;
    if (vm->dbg_floor_on && vm->dbg_floor > 0 && vm->dbg_floor < top) {
        bot = vm->dbg_floor;
    }
    depth = top - bot;
    if (depth < 0) {
        depth = 0;
    }
    dbg_pad_column(vm, DBG_STACK_COL);
    scol = hook_get(vm, "DBG-LINE-COL", vm->dbg_line_col);
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
    slen = hook_get(vm, "DBG-LINE-COL", vm->dbg_line_col) - scol;
    if (slen < DBG_S_WIDTH) {
        dbg_spaces(vm, DBG_S_WIDTH - slen);
    } else if (slen > DBG_S_WIDTH) {
        emit_ch(vm, ' ');
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
            if (vm->dbg_midline) {
                emit_ch(vm, '\n');
            }
        }
        dbg_sync_flags(vm);
        dbg_print_sr(vm);
        emit_ch(vm, '\n');
    }
    emit_str(vm, dbg_intoable(vm, vm->dbg_xt) ? "I>> " : ">> ");
    dbg_print_name(vm);
    dbg_print_inline(vm);
    /* Leave this line open. The next pause pads S/R onto it. */
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
                if (vm->dbg_midline) {
                    emit_ch(vm, '\n');
                }
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

/* Break Now. Arm if this eval was free-running, drop go/step modes, and
   keep executing until the enclosing colon is at or above DEBUGGER-END.
   The flag stays set across those skipped tokens. On the hit, the caller
   frame is the floor: this word still pauses, and returning only as far as
   its caller does not end the session. */
static void dbg_seek_break(ef_vm *vm) {
    int was;
    int32_t min;
    if (!vm->dbg_break_asap || vm->dbg_busy) {
        return;
    }
    was = vm->dbg_armed;
    vm->dbg_armed = 1;
    if (!was) {
        vm->dbg_need_intro = 1;
        vm->dbg_need_stacks = 0;
        vm->dbg_skip_nl = 1;
        vm->dbg_midline = 0;
        vm->dbg_help_shown = 0;
        vm->dbg_line_col = 0;
        dbg_sync_flags(vm);
    }
    vm->dbg_floor_on = 0;
    vm->dbg_floor = 0;
    vm->dbg_bp_go = 0;
    vm->dbg_runto_ip = 0;
    vm->dbg_over = -1;
    vm->dbg_out = -1;
    min = vm->dbg_break_min;
    if (min) {
        dbg_resolve_enclosing(vm);
        if (!vm->dbg_cfa || vm->dbg_cfa < min) {
            return;
        }
    }
    vm->dbg_break_asap = 0;
    vm->dbg_floor = vm->rsp > 0 ? vm->rsp - 1 : 0;
    vm->dbg_floor_on = 1;
}

static int dbg_want_pause(ef_vm *vm, int32_t xt) {
    if (vm->dbg_busy || !vm->dbg_armed || vm->dbg_break_asap) {
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
    dbg_seek_break(vm);
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
    vm->dbg_line_col = hook_get(vm, "DBG-LINE-COL", vm->dbg_line_col);
    vm->dbg_midline = hook_get(vm, "DBG-MIDLINE", vm->dbg_midline);
    if (vm->dbg_line_col > 0) {
        emit_ch(vm, '\n');
    }
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

/* EditForth keeps these folders under Resources/System. FROMLIB still
   accepts the short name: FROMLIB FLOAD Testing/… and Debugger/…. */
static int fromlib_is_system(const char *spec) {
    const char *s = spec ? spec : "";
    char folder[32];
    size_t n = 0;
    static const char *names[] = {
        "assembler", "debugger", "emitter", "hyper",
        "reference", "sources", "testing", NULL
    };
    int i;
    while (*s == '/') {
        s++;
    }
    while (s[n] && s[n] != '/' && n + 1 < sizeof folder) {
        folder[n] = (char)tolower((unsigned char)s[n]);
        n++;
    }
    if (s[n] && s[n] != '/') {
        return 0;
    }
    folder[n] = 0;
    for (i = 0; names[i]; i++) {
        if (strcmp(folder, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *fromlib_prefix(const char *spec) {
    return fromlib_is_system(spec) ? "Resources/System/" : "Resources/Library/";
}

static void fromlib_base_path(const char *spec, char *base, size_t n) {
    snprintf(base, n, "%s", fromlib_is_system(spec) ? "Resources/System" : "Resources/Library");
}

/* One-shot prefix for the next INCLUDED. S" already dropped one leading blank. */
static int p_resolve_key(ef_vm *vm) {
    char raw[EF_PARSE_MAX];
    char path[EF_PARSE_MAX];
    const char *s;
    const char *prefix;
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
    prefix = fromlib_prefix(s);
    plen = strlen(prefix);
    if ((strncmp(s, "Resources/Library/", 18) == 0 || strncmp(s, "Resources/System/", 17) == 0)) {
        return plant_parse(vm, s, n);
    }
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

/* Floored. Divisor 0 leaves remainder 0 and quotient -1, matching EditForth. */
static int p_fm_mod(ef_vm *vm) {
    int32_t n, hi, lo, quot, rem;
    int64_t d;
    if (pop(vm, &n) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    if (n == 0) {
        return push2(vm, 0, -1);
    }
    d = (int64_t)pack_d(lo, hi);
    if (n == -1 && d == INT64_MIN) {
        rem = 0;
        quot = (int32_t)0x80000000;
    } else {
        quot = (int32_t)(d / n);
        rem = (int32_t)(d % n);
        if (rem != 0 && ((rem < 0) != (n < 0))) {
            quot -= 1;
            rem += n;
        }
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

static int p_s_to_d(ef_vm *vm) {
    int32_t n;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    return push2(vm, n, n < 0 ? -1 : 0);
}

static int p_d_minus(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    return push_d(vm, pack_d(lo1, hi1) - pack_d(lo2, hi2));
}

static int p_d_two_star(ef_vm *vm) {
    int32_t hi, lo;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    return push_d(vm, pack_d(lo, hi) << 1);
}

static int p_d_two_slash(ef_vm *vm) {
    int32_t hi, lo;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    return push_d(vm, (uint64_t)((int64_t)pack_d(lo, hi) >> 1));
}

static int p_d_zero_eq(ef_vm *vm) {
    int32_t hi, lo;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    return push(vm, (lo | hi) == 0 ? -1 : 0);
}

static int p_d_eq(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    return push(vm, (lo1 == lo2 && hi1 == hi2) ? -1 : 0);
}

static int d_less(uint64_t a, uint64_t b, int uns) {
    if (uns) {
        return a < b;
    }
    return (int64_t)a < (int64_t)b;
}

static int p_d_lt(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    return push(vm, d_less(pack_d(lo1, hi1), pack_d(lo2, hi2), 0) ? -1 : 0);
}

static int p_du_lt(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    return push(vm, d_less(pack_d(lo1, hi1), pack_d(lo2, hi2), 1) ? -1 : 0);
}

static int p_d_min(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    uint64_t a, b;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    a = pack_d(lo1, hi1);
    b = pack_d(lo2, hi2);
    return push_d(vm, d_less(b, a, 0) ? b : a);
}

static int p_d_max(ef_vm *vm) {
    int32_t hi2, lo2, hi1, lo1;
    uint64_t a, b;
    if (pop(vm, &hi2) || pop(vm, &lo2) || pop(vm, &hi1) || pop(vm, &lo1)) {
        return EF_THROW;
    }
    a = pack_d(lo1, hi1);
    b = pack_d(lo2, hi2);
    return push_d(vm, d_less(a, b, 0) ? b : a);
}

static int p_d_to_s(ef_vm *vm) {
    int32_t hi, lo;
    if (pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    (void)hi;
    return push(vm, lo);
}

static int p_m_plus(ef_vm *vm) {
    int32_t n, hi, lo;
    if (pop(vm, &n) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    return push_d(vm, (uint64_t)((int64_t)pack_d(lo, hi) + (int64_t)n));
}

static int p_two_rot(ef_vm *vm) {
    int32_t x1, x2, x3, x4, x5, x6;
    if (pop(vm, &x6) || pop(vm, &x5) || pop(vm, &x4) || pop(vm, &x3) ||
        pop(vm, &x2) || pop(vm, &x1)) {
        return EF_THROW;
    }
    if (push(vm, x3) || push(vm, x4) || push(vm, x5) || push(vm, x6) ||
        push(vm, x1) || push(vm, x2)) {
        return EF_THROW;
    }
    return EF_OK;
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

/* ANS: x2 lives at a-addr and x1 at the next cell. Same as
   DUP CELL+ @ SWAP @  and  SWAP OVER ! CELL+ ! */
static int p_two_fetch(ef_vm *vm) {
    int32_t a, x1, x2;
    if (pop(vm, &a) || load_cell(vm, a, &x2) || load_cell(vm, a + CELL, &x1)) {
        return EF_THROW;
    }
    return push2(vm, x1, x2);
}

static int p_two_store(ef_vm *vm) {
    int32_t a, x1, x2;
    if (pop(vm, &a) || pop(vm, &x2) || pop(vm, &x1)) {
        return EF_THROW;
    }
    if (store_cell(vm, a, x2) || store_cell(vm, a + CELL, x1)) {
        return EF_THROW;
    }
    return EF_OK;
}

/* Drop n saved items and report that RESTORE-INPUT could not rewind. */
static int input_fail(ef_vm *vm, int32_t n) {
    int32_t junk;
    int i;
    if (n < 0) {
        n = 0;
    }
    if (n > 64) {
        n = 64;
    }
    for (i = 0; i < n; i++) {
        if (pop(vm, &junk)) {
            break;
        }
    }
    return push(vm, -1);
}

/* EVALUATE saves >IN and a count of 1. A file line also saves the offset
   of that line so RESTORE-INPUT can put it back after REFILL.
   A block load adds the block number: REFILL switches blocks, and
   RESTORE-INPUT has to load that block again. */
static int p_save_input(ef_vm *vm) {
    if (vm->load_on && vm->load_text && vm->load_blk >= 0) {
        if (push(vm, uv(vm, UV_IN)) || push(vm, vm->load_line_at) ||
            push(vm, vm->line) || push(vm, vm->load_stop) ||
            push(vm, vm->load_blk) || push(vm, vm->load_gen) ||
            push(vm, 6)) {
            return EF_THROW;
        }
        return EF_OK;
    }
    if (vm->load_on && vm->load_text) {
        if (push(vm, uv(vm, UV_IN)) || push(vm, vm->load_line_at) ||
            push(vm, vm->line) || push(vm, vm->load_stop) ||
            push(vm, vm->load_gen) || push(vm, 5)) {
            return EF_THROW;
        }
        return EF_OK;
    }
    return push2(vm, uv(vm, UV_IN), 1);
}

static int p_restore_input(ef_vm *vm) {
    int32_t n, in = 0;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    if (n == 6 && vm->load_on && vm->load_text && vm->load_blk >= 0) {
        int32_t gen, blk, stop, line, at;
        if (pop(vm, &gen) || pop(vm, &blk) || pop(vm, &stop) || pop(vm, &line) ||
            pop(vm, &at) || pop(vm, &in)) {
            return EF_THROW;
        }
        if (gen != vm->load_gen || at < 0) {
            return push(vm, -1);
        }
        if (blk != vm->load_blk) {
            if (reload_block(vm, blk)) {
                return EF_THROW;
            }
        }
        if (at > vm->load_len) {
            return push(vm, -1);
        }
        vm->load_pos = at;
        vm->load_line = line > 0 ? line : 1;
        vm->load_stop = 0;
        if (load_next_line(vm) != 1 || in < 0 || in > vm->src_len) {
            return push(vm, -1);
        }
        uv_set(vm, UV_IN, in);
        vm->load_stop = stop ? 1 : 0;
        if (vm->blk_addr) {
            store_cell(vm, vm->blk_addr, blk);
        }
        return push(vm, 0);
    }
    if (n == 5 && vm->load_on && vm->load_text) {
        int32_t gen, stop, line, at;
        if (pop(vm, &gen) || pop(vm, &stop) || pop(vm, &line) || pop(vm, &at) ||
            pop(vm, &in)) {
            return EF_THROW;
        }
        if (gen != vm->load_gen || at < 0 || at > vm->load_len) {
            return push(vm, -1);
        }
        vm->load_pos = at;
        vm->load_line = line > 0 ? line : 1;
        vm->load_stop = 0;
        if (load_next_line(vm) != 1 || in < 0 || in > vm->src_len) {
            return push(vm, -1);
        }
        uv_set(vm, UV_IN, in);
        vm->load_stop = stop ? 1 : 0;
        return push(vm, 0);
    }
    if (n != 1 || pop(vm, &in) || in < 0 || in > vm->src_len) {
        if (n != 1) {
            return input_fail(vm, n);
        }
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

/* The address is in the input buffer, so two PARSE-NAME results on one
   line stay distinct. A copy would overwrite the first name. */
static int p_parse_name(ef_vm *vm) {
    int in = skip_white_src(vm);
    int len = vm->src_len;
    uint8_t *s = src_ptr(vm);
    int start, n;
    if (in >= len) {
        return push2(vm, 0, 0);
    }
    start = in;
    while (in < len && !is_white(s[in])) {
        in++;
    }
    n = in - start;
    uv_set(vm, UV_IN, in);
    if (push(vm, vm->src_at + start) || push(vm, n)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int p_parse_filespec(ef_vm *vm) {
    int in = skip_white_src(vm);
    int len = vm->src_len;
    uint8_t *s = src_ptr(vm);
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

/* 1 when DOES> begins with 2@ (2VALUE). 0 for a one-cell VALUE. -1 on a bad header. */
static int to_two(ef_vm *vm, int32_t cfa) {
    int32_t code, ip, xt;
    if (load_cell(vm, cfa, &code)) {
        return -1;
    }
    if (code != EF_DODOES || !vm->xt_two_fetch) {
        return 0;
    }
    if (load_cell(vm, cfa + CELL, &ip) || load_cell(vm, ip, &xt)) {
        return -1;
    }
    return xt == vm->xt_two_fetch;
}

static int p_to(ef_vm *vm) {
    char name[EF_WORD_MAX];
    int n, two;
    int imm = 0;
    int32_t cfa, body, x, hi, lo;
    int32_t store_xt;
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
    two = to_two(vm, cfa);
    if (two < 0) {
        return EF_THROW;
    }
    store_xt = two ? vm->xt_two_store : vm->xt_store;
    if (uv(vm, UV_STATE)) {
        if (comma(vm, vm->xt_lit) || comma(vm, body) || comma(vm, store_xt)) {
            return EF_THROW;
        }
        return EF_OK;
    }
    if (two) {
        if (pop(vm, &hi) || pop(vm, &lo) ||
            store_cell(vm, body, hi) || store_cell(vm, body + CELL, lo)) {
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
    int saved_cf, saved_begin, saved_loop;
    int32_t saved_ip, saved_state, saved_defining, saved_noname;
    int rc;
    if (pop(vm, &xt)) {
        return EF_THROW;
    }
    saved_sp = vm->sp;
    saved_rsp = vm->rsp;
    saved_ip = vm->ip;
    saved_inner = vm->in_inner;
    saved_frames = vm->frame_n;
    saved_cf = vm->cf_n;
    saved_begin = vm->begin_n;
    saved_loop = vm->loop_n;
    saved_state = uv(vm, UV_STATE);
    saved_defining = vm->defining;
    saved_noname = vm->noname_xt;
    rc = exec_one(vm, xt, 0);
    if (rc != EF_OK || vm->throw_code) {
        int code = vm->throw_code ? vm->throw_code : -1;
        vm->sp = saved_sp;
        vm->rsp = saved_rsp;
        vm->ip = saved_ip;
        vm->in_inner = saved_inner;
        vm->frame_n = saved_frames;
        vm->cf_n = saved_cf;
        vm->begin_n = saved_begin;
        vm->loop_n = saved_loop;
        vm->defining = saved_defining;
        vm->noname_xt = saved_noname;
        uv_set(vm, UV_STATE, saved_state);
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

/* nt is the CFA. Interpretation semantics are that execution token. */
static int p_name_interpret(ef_vm *vm) {
    (void)vm;
    return EF_OK;
}

/* ( nt -- x xt ). EXECUTE of xt performs the compilation semantics.
   Immediate words use EXECUTE so the word runs. Other words use COMPILE,
   and x is the nt that COMPILE, consumes. */
static int p_name_compile(ef_vm *vm) {
    int32_t nt, flags, xt;
    const char *doer;
    int n;
    if (pop(vm, &nt)) {
        return EF_THROW;
    }
    if (nt < 4 * CELL || load_cell(vm, nt - CELL, &flags)) {
        return EF_THROW;
    }
    doer = flags < 0 ? "EXECUTE" : "COMPILE,";
    n = (int)strlen(doer);
    xt = find_word(vm, doer, n, NULL);
    if (!xt) {
        return fail(vm, -13, NULL);
    }
    if (push(vm, nt) || push(vm, xt)) {
        return EF_THROW;
    }
    return EF_OK;
}

/* ( xn .. x1 n -- ) ( R: -- x1 .. xn n ). NR> restores the same order. */
static int p_n_to_r(ef_vm *vm) {
    int32_t n, v;
    int i;
    if (pop(vm, &n)) {
        return EF_THROW;
    }
    if (n < 0) {
        return fail(vm, -3, "stack underflow\n");
    }
    for (i = 0; i < n; i++) {
        if (pop(vm, &v) || rpush(vm, v)) {
            return EF_THROW;
        }
    }
    return rpush(vm, n);
}

static int p_n_r_from(ef_vm *vm) {
    int32_t n, v;
    int i;
    if (rpop(vm, &n)) {
        return EF_THROW;
    }
    if (n < 0) {
        return fail(vm, -5, "return stack underflow\n");
    }
    for (i = 0; i < n; i++) {
        if (rpop(vm, &v) || push(vm, v)) {
            return EF_THROW;
        }
    }
    return push(vm, n);
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

/* ( c-addr u line -- ) Tell the host to show this source. No host: do nothing. */
static int p_host_view(ef_vm *vm) {
    int32_t line, u, addr;
    char path[512];
    if (pop(vm, &line) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u < 0 || u >= (int)sizeof path || !span_ok(vm, addr, u > 0 ? u : 0)) {
        return fail(vm, -16, "bad address\n");
    }
    if (u > 0) {
        memcpy(path, vm->base + addr, (size_t)u);
    }
    path[u > 0 ? u : 0] = 0;
    if (vm->view_fn && path[0]) {
        vm->view_fn(vm->view_ctx, path, (int)line);
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

/* REPRESENT: 0.digits * 10^n. Zero has n 0. NaN and Inf fail (flag2 false). */
static int fp_represent(double r, char *dst, int u, int *n_out, int *neg_out) {
    char buf[96];
    char *p;
    int exp = 0;
    int nd = 0;
    int prec;
    *neg_out = signbit(r) ? 1 : 0;
    if (u < 1) {
        *n_out = 0;
        return 0;
    }
    if (!isfinite(r)) {
        *n_out = 0;
        return 0;
    }
    if (r == 0.0) {
        memset(dst, '0', (size_t)u);
        *n_out = 0;
        return 1;
    }
    r = fabs(r);
    prec = u - 1;
    if (prec > 40) {
        prec = 40;
    }
    snprintf(buf, sizeof buf, "%.*e", prec, r);
    p = buf;
    if (*p == '-') {
        p++;
    }
    if (p[0] < '0' || p[0] > '9' || p[1] != '.') {
        *n_out = 0;
        return 0;
    }
    dst[nd++] = p[0];
    p += 2;
    while (*p && *p != 'e' && *p != 'E' && nd < u) {
        if (*p >= '0' && *p <= '9') {
            dst[nd++] = *p;
        }
        p++;
    }
    while (*p && *p != 'e' && *p != 'E') {
        p++;
    }
    if (*p == 'e' || *p == 'E') {
        exp = atoi(p + 1);
    }
    while (nd < u) {
        dst[nd++] = '0';
    }
    *n_out = exp + 1;
    return 1;
}

static int fp_prec(const ef_vm *vm) {
    int p = vm->fprec;
    if (p < 1) {
        return 1;
    }
    if (p > 64) {
        return 64;
    }
    return p;
}

static void emit_zeros(ef_vm *vm, int n) {
    while (n-- > 0) {
        emit_ch(vm, '0');
    }
}

static void emit_dec_sp(ef_vm *vm, int v) {
    char b[16];
    int n = 0;
    unsigned int u;
    if (v < 0) {
        emit_ch(vm, '-');
        u = (unsigned int)(-v);
    } else {
        u = (unsigned int)v;
    }
    do {
        b[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && n < (int)sizeof b);
    while (n > 0) {
        emit_ch(vm, b[--n]);
    }
    emit_ch(vm, ' ');
}

static void emit_fbad(ef_vm *vm, double r) {
    if (signbit(r)) {
        emit_ch(vm, '-');
    }
    emit_str(vm, isnan(r) ? "nan " : "inf ");
}

static int fp_digits(ef_vm *vm, double r, char *digs, int *n, int *neg) {
    int prec = fp_prec(vm);
    if (!fp_represent(r, digs, prec, n, neg)) {
        emit_fbad(vm, r);
        return 0;
    }
    return prec;
}

/* Gforth f. / fs. / fe. : significand digits, then one trailing space. */
static void emit_fdot(ef_vm *vm, double r) {
    char digs[80];
    int n = 0;
    int neg = 0;
    int prec;
    int i;
    int skip;
    int end;
    prec = fp_digits(vm, r, digs, &n, &neg);
    if (!prec) {
        return;
    }
    if (neg) {
        emit_ch(vm, '-');
    }
    if (n <= 0) {
        emit_ch(vm, '0');
    } else {
        int take = n < prec ? n : prec;
        for (i = 0; i < take; i++) {
            emit_ch(vm, digs[i]);
        }
        emit_zeros(vm, n - prec);
    }
    emit_ch(vm, '.');
    if (n < 0) {
        emit_zeros(vm, -n);
    }
    skip = n > 0 ? n : 0;
    end = prec;
    while (end > skip && digs[end - 1] == '0') {
        end--;
    }
    for (i = skip; i < end; i++) {
        emit_ch(vm, digs[i]);
    }
    emit_ch(vm, ' ');
}

static void emit_fs(ef_vm *vm, double r) {
    char digs[80];
    int n = 0;
    int neg = 0;
    int prec;
    int i;
    prec = fp_digits(vm, r, digs, &n, &neg);
    if (!prec) {
        return;
    }
    if (neg) {
        emit_ch(vm, '-');
    }
    emit_ch(vm, digs[0]);
    emit_ch(vm, '.');
    for (i = 1; i < prec; i++) {
        emit_ch(vm, digs[i]);
    }
    emit_ch(vm, 'E');
    emit_dec_sp(vm, n - 1);
}

static void emit_fe(ef_vm *vm, double r) {
    char digs[80];
    int n = 0;
    int neg = 0;
    int prec;
    int exp;
    int quot;
    int rem;
    int before;
    int typed;
    int i;
    prec = fp_digits(vm, r, digs, &n, &neg);
    if (!prec) {
        return;
    }
    exp = n - 1;
    quot = exp / 3;
    rem = exp % 3;
    if (rem < 0) {
        rem += 3;
        quot--;
    }
    before = rem + 1;
    if (neg) {
        emit_ch(vm, '-');
    }
    typed = before < prec ? before : prec;
    for (i = 0; i < typed; i++) {
        emit_ch(vm, digs[i]);
    }
    emit_zeros(vm, before - prec);
    emit_ch(vm, '.');
    if (before < prec) {
        for (i = before; i < prec; i++) {
            emit_ch(vm, digs[i]);
        }
    }
    emit_ch(vm, 'E');
    emit_dec_sp(vm, quot * 3);
}

/* Nearest integer, ties toward +infinity. A zero result keeps the sign of r. */
static double f_round_neg(double r) {
    double q;
    if (!isfinite(r)) {
        return r;
    }
    q = floor(r + 0.5);
    if (q == 0.0 && signbit(r)) {
        return -0.0;
    }
    return q;
}

static int f_trunc_i64(double r, int64_t *out) {
    double q;
    if (!isfinite(r)) {
        return 0;
    }
    q = trunc(r);
    if (q >= 9223372036854775808.0 || q < -9223372036854775808.0) {
        return 0;
    }
    *out = (int64_t)q;
    return 1;
}

static int f_flag(ef_vm *vm, int cond) {
    return push(vm, cond ? -1 : 0);
}

static int fp_fetch(ef_vm *vm, int32_t addr, int n) {
    double d;
    if (!span_ok(vm, addr, n)) {
        return fail(vm, -16, "bad address\n");
    }
    if (n == 4) {
        float f;
        memcpy(&f, vm->base + addr, 4);
        d = (double)f;
    } else {
        memcpy(&d, vm->base + addr, 8);
    }
    return fpush(vm, d);
}

static int fp_store(ef_vm *vm, int32_t addr, int n) {
    double d;
    if (fpop(vm, &d)) {
        return EF_THROW;
    }
    if (!span_ok(vm, addr, n)) {
        return fail(vm, -16, "bad address\n");
    }
    if (n == 4) {
        float f = (float)d;
        memcpy(vm->base + addr, &f, 4);
    } else {
        memcpy(vm->base + addr, &d, 8);
    }
    return EF_OK;
}

static int32_t align_up(int32_t addr, int32_t m) {
    uint32_t u = (uint32_t)addr;
    uint32_t mask = (uint32_t)m - 1u;
    return (int32_t)((u + mask) & ~mask);
}

/* r3>0 absolute, r3=0 same bits (+0 and -0 differ), r3<0 relative. A NaN r3 is false. */
static int f_same_bits(double a, double b) {
    uint64_t ua, ub;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

static int f_prox(double r1, double r2, double r3) {
    if (r3 > 0.0) {
        return !isnan(r1) && !isnan(r2) && fabs(r1 - r2) < r3;
    }
    if (r3 < 0.0) {
        return !isnan(r1) && !isnan(r2) &&
               fabs(r1 - r2) < fabs(r3) * (fabs(r1) + fabs(r2));
    }
    if (r3 == 0.0) {
        return f_same_bits(r1, r2);
    }
    return 0;
}

static int p_fop(ef_vm *vm) {
    int32_t op;
    double a, b, c;
    if (pop(vm, &op)) {
        return EF_THROW;
    }
    switch (op) {
    case 1:
        return push(vm, vm->fsp);
    case 2:
        return fpop(vm, &a);
    case 3:
        if (fpop(vm, &a) || fpush(vm, a)) {
            return EF_THROW;
        }
        return fpush(vm, a);
    case 4:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, b) || fpush(vm, a) ? EF_THROW : EF_OK;
    case 5:
        if (vm->fsp < 2) {
            return fail(vm, -45, "float stack underflow\n");
        }
        return fpush(vm, vm->fstack[vm->fsp - 2]);
    case 6:
        if (fpop(vm, &c) || fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, b) || fpush(vm, c) || fpush(vm, a) ? EF_THROW : EF_OK;
    case 7:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, a + b);
    case 8:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, a - b);
    case 9:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, a * b);
    case 10:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, a / b);
    case 11:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, -a);
    case 12:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, fabs(a));
    case 13:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, fmax(a, b));
    case 14:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, fmin(a, b));
    case 15:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, a == 0.0);
    case 16:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, a < 0.0);
    case 17:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, a < b);
    case 18:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, a > b);
    case 19:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, a == b);
    case 20:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, a != b);
    case 21:
        if (fpop(vm, &c) || fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return f_flag(vm, f_prox(a, b, c));
    case 22:
    case 26: {
        int32_t addr;
        if (pop(vm, &addr)) {
            return EF_THROW;
        }
        return fp_fetch(vm, addr, 8);
    }
    case 23:
    case 27: {
        int32_t addr;
        if (pop(vm, &addr)) {
            return EF_THROW;
        }
        return fp_store(vm, addr, 8);
    }
    case 24: {
        int32_t addr;
        if (pop(vm, &addr)) {
            return EF_THROW;
        }
        return fp_fetch(vm, addr, 4);
    }
    case 25: {
        int32_t addr;
        if (pop(vm, &addr)) {
            return EF_THROW;
        }
        return fp_store(vm, addr, 4);
    }
    case 28: {
        int32_t n;
        if (pop(vm, &n)) {
            return EF_THROW;
        }
        return fpush(vm, (double)n);
    }
    case 29: {
        int64_t q;
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        if (!f_trunc_i64(a, &q) || q > 2147483647LL || q < -2147483648LL) {
            return fail(vm, -44, "float out of range\n");
        }
        return push(vm, (int32_t)q);
    }
    case 30: {
        int32_t hi, lo;
        if (pop(vm, &hi) || pop(vm, &lo)) {
            return EF_THROW;
        }
        return fpush(vm, (double)(int64_t)pack_d(lo, hi));
    }
    case 31: {
        int64_t q;
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        if (!f_trunc_i64(a, &q)) {
            return fail(vm, -44, "float out of range\n");
        }
        return push_d(vm, (uint64_t)q);
    }
    case 32: {
        int32_t u, addr;
        double v = 0.0;
        char tmp[768];
        if (pop(vm, &u) || pop(vm, &addr)) {
            return EF_THROW;
        }
        if (u < 0 || u >= (int)sizeof tmp || !span_ok(vm, addr, u > 0 ? u : 0)) {
            return fail(vm, -16, "bad address\n");
        }
        if (u > 0) {
            memcpy(tmp, vm->base + addr, (size_t)u);
        }
        if (!parse_float(tmp, u, &v)) {
            return push(vm, 0);
        }
        if (fpush(vm, v)) {
            return EF_THROW;
        }
        return push(vm, -1);
    }
    case 33:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        emit_fdot(vm, a);
        return EF_OK;
    case 34:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        emit_fs(vm, a);
        return EF_OK;
    case 35:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        emit_fe(vm, a);
        return EF_OK;
    case 36:
        return push(vm, vm->fprec > 0 ? vm->fprec : 15);
    case 37: {
        int32_t u;
        if (pop(vm, &u)) {
            return EF_THROW;
        }
        if (u < 1) {
            u = 1;
        }
        if (u > 64) {
            u = 64;
        }
        vm->fprec = u;
        return EF_OK;
    }
    case 38: {
        int32_t u, addr;
        int n = 0;
        int neg = 0;
        int ok;
        char digs[256];
        if (pop(vm, &u) || pop(vm, &addr)) {
            return EF_THROW;
        }
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        if (u < 1 || !span_ok(vm, addr, u)) {
            return fail(vm, -16, "bad address\n");
        }
        if (u <= (int)sizeof digs) {
            ok = fp_represent(a, digs, u, &n, &neg);
            if (ok) {
                memcpy(vm->base + addr, digs, (size_t)u);
            }
        } else {
            ok = fp_represent(a, digs, (int)sizeof digs, &n, &neg);
            if (ok) {
                memcpy(vm->base + addr, digs, sizeof digs);
                memset(vm->base + addr + sizeof digs, '0', (size_t)u - sizeof digs);
            }
        }
        if (!ok) {
            n = 0;
        }
        return push(vm, n) || push(vm, neg ? -1 : 0) || push(vm, ok ? -1 : 0)
                   ? EF_THROW
                   : EF_OK;
    }
    case 39:
    case 43: {
        int32_t n;
        if (pop(vm, &n)) {
            return EF_THROW;
        }
        return push(vm, n * 8);
    }
    case 40:
    case 44: {
        int32_t n;
        if (pop(vm, &n)) {
            return EF_THROW;
        }
        return push(vm, n + 8);
    }
    case 41: {
        int32_t n;
        if (pop(vm, &n)) {
            return EF_THROW;
        }
        return push(vm, n * 4);
    }
    case 42: {
        int32_t n;
        if (pop(vm, &n)) {
            return EF_THROW;
        }
        return push(vm, n + 4);
    }
    case 45:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, sqrt(a));
    case 46:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, pow(a, b));
    case 47:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, exp(a));
    case 48:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, expm1(a));
    case 49:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, log(a));
    case 50:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, log1p(a));
    case 51:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, log10(a));
    case 52:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, pow(10.0, a));
    case 53:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, sin(a));
    case 54:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, cos(a));
    case 55:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, tan(a));
    case 56:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, asin(a));
    case 57:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, acos(a));
    case 58:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, atan(a));
    case 59:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, atan2(a, b));
    case 60:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, sin(a)) || fpush(vm, cos(a)) ? EF_THROW : EF_OK;
    case 61:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, sinh(a));
    case 62:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, cosh(a));
    case 63:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, tanh(a));
    case 64:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, asinh(a));
    case 65:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, acosh(a));
    case 66:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, atanh(a));
    case 67:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, floor(a));
    case 68:
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, f_round_neg(a));
    case 69:
        if (fpop(vm, &b) || fpop(vm, &a)) {
            return EF_THROW;
        }
        return fpush(vm, a - b * floor(a / b));
    case 73:
    case 75: {
        int32_t addr;
        if (pop(vm, &addr)) {
            return EF_THROW;
        }
        return push(vm, align_up(addr, 8));
    }
    case 74: {
        int32_t addr;
        if (pop(vm, &addr)) {
            return EF_THROW;
        }
        return push(vm, align_up(addr, 4));
    }
    case 102: {
        uint64_t bits = 0;
        if (fpop(vm, &a)) {
            return EF_THROW;
        }
        memcpy(&bits, &a, 8);
        if (push(vm, (int32_t)(uint32_t)bits)) {
            return EF_THROW;
        }
        return push(vm, (int32_t)(uint32_t)(bits >> 32));
    }
    default:
        return fail(vm, -21, "unsupported float op\n");
    }
}

static int p_flit(ef_vm *vm) {
    int32_t hi, lo;
    uint64_t bits;
    double d;
    if (load_cell(vm, vm->ip, &hi)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    if (load_cell(vm, vm->ip, &lo)) {
        return EF_THROW;
    }
    vm->ip += CELL;
    bits = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
    memcpy(&d, &bits, 8);
    return fpush(vm, d);
}

static int p_textedit(ef_vm *vm) {
    emit_str(vm, "TEXTEDIT is not on this page\n");
    return EF_OK;
}

/* Defined after join_rel. A name that already passes file_path_ok is unchanged. */
static int collapse_include(ef_vm *vm, char *name, size_t namemax);

static int p_open_file(ef_vm *vm) {
    int32_t fam;
    char name[512];
    char resolved[512];
    char *owned = NULL;
    int len = 0;
    int slot = -1;
    ef_blob *b;
    if (pop(vm, &fam)) {
        return EF_THROW;
    }
    (void)fam;
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    slashes(name);
    /* INCLUDE writes ../fp/<file> against the Harness directory. file_path_ok
       rejects "..", so fold it here. CREATE-FILE stays strict. */
    if (!collapse_include(vm, name, sizeof name)) {
        return push2(vm, 0, 1);
    }
    b = blob_find(vm, name);
    if (b) {
        slot = slot_load(vm, name, b->data, b->len);
        return push2(vm, slot < 0 ? 0 : slot + 1, slot < 0 ? 1 : 0);
    }
    if (resolve_file(vm, name, resolved, sizeof resolved, &owned, &len)) {
        slot = slot_load(vm, resolved, owned, len);
        free(owned);
    }
#ifndef __EMSCRIPTEN__
    else if (name[0] == '/') {
        char *disk = NULL;
        if (read_disk(name, &disk) == 0 && disk) {
            slot = slot_load(vm, name, disk, (int)strlen(disk));
            free(disk);
        }
    }
#endif
    if (slot < 0) {
        return push2(vm, 0, 1);
    }
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
    if (!file_path_ok(name)) {
        return push2(vm, 0, 1);
    }
    slot = slot_load(vm, name, "", 0);
    if (slot < 0 || blob_store(vm, name, "", 0)) {
        if (slot >= 0) {
            free(vm->files[slot].data);
            memset(&vm->files[slot], 0, sizeof vm->files[slot]);
        }
        return push2(vm, 0, 1);
    }
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
    sync_slot(vm, s);
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
    int s;
    if (pop(vm, &fid) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0 || u < 0 || !span_ok(vm, addr, u)) {
        return push(vm, 1);
    }
    if (file_put(vm, s, u ? (const char *)(vm->base + addr) : "", u)) {
        return push(vm, 1);
    }
    sync_slot(vm, s);
    return push(vm, 0);
}

static int p_read_line(ef_vm *vm) {
    int32_t fid, u, addr;
    int s, pos, len, n;
    char *d;
    if (pop(vm, &fid) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0 || u < 0) {
        if (push(vm, 0) || push(vm, 0)) {
            return EF_THROW;
        }
        return push(vm, 1);
    }
    pos = vm->files[s].pos;
    len = vm->files[s].len;
    d = vm->files[s].data;
    if (pos >= len || !d) {
        if (push(vm, 0) || push(vm, 0)) {
            return EF_THROW;
        }
        return push(vm, 0);
    }
    if (u > 0 && !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    n = 0;
    while (pos < len && n < u) {
        unsigned char c = (unsigned char)d[pos];
        if (c == '\n' || c == '\r') {
            break;
        }
        vm->base[addr + n] = c;
        n++;
        pos++;
    }
    if (pos < len && n < u) {
        if (d[pos] == '\r') {
            pos++;
            if (pos < len && d[pos] == '\n') {
                pos++;
            }
        } else if (d[pos] == '\n') {
            pos++;
        }
    }
    vm->files[s].pos = pos;
    if (push(vm, n) || push(vm, -1)) {
        return EF_THROW;
    }
    return push(vm, 0);
}

static int p_write_line(ef_vm *vm) {
    int32_t fid, u, addr;
    int s;
    char nl = '\n';
    if (pop(vm, &fid) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0 || u < 0 || !span_ok(vm, addr, u)) {
        return push(vm, 1);
    }
    if (file_put(vm, s, u ? (const char *)(vm->base + addr) : "", u) ||
        file_put(vm, s, &nl, 1)) {
        return push(vm, 1);
    }
    sync_slot(vm, s);
    return push(vm, 0);
}

static int p_file_position(ef_vm *vm) {
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
    if (push(vm, vm->files[s].pos) || push(vm, 0)) {
        return EF_THROW;
    }
    return push(vm, 0);
}

static int p_resize_file(ef_vm *vm) {
    int32_t fid, hi, lo;
    int s, old;
    if (pop(vm, &fid) || pop(vm, &hi) || pop(vm, &lo)) {
        return EF_THROW;
    }
    s = slot_of(vm, fid);
    if (s < 0 || hi != 0 || lo < 0 || lo > EF_SRC_MAX) {
        return push(vm, 1);
    }
    old = vm->files[s].len;
    if (lo > old && file_grow(vm, s, lo)) {
        return push(vm, 1);
    }
    if (lo > old && vm->files[s].data) {
        memset(vm->files[s].data + old, 0, (size_t)(lo - old));
    }
    vm->files[s].len = lo;
    if (vm->files[s].pos > lo) {
        vm->files[s].pos = lo;
    }
    sync_slot(vm, s);
    return push(vm, 0);
}

static int p_delete_file(ef_vm *vm) {
    char name[512];
    int i;
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    slashes(name);
    if (!file_path_ok(name) || !blob_find(vm, name)) {
        return push(vm, 1);
    }
    for (i = 0; i < EF_OPEN; i++) {
        if (vm->files[i].used && strcmp(vm->files[i].path, name) == 0) {
            free(vm->files[i].data);
            memset(&vm->files[i], 0, sizeof vm->files[i]);
        }
    }
    blob_drop(vm, name);
    return push(vm, 0);
}

static int p_rename_file(ef_vm *vm) {
    char to[512], from[512];
    ef_blob *b;
    int i;
    if (pop_text(vm, to, (int)sizeof to) || pop_text(vm, from, (int)sizeof from)) {
        return EF_THROW;
    }
    slashes(to);
    slashes(from);
    if (!file_path_ok(from) || !file_path_ok(to)) {
        return push(vm, 1);
    }
    b = blob_find(vm, from);
    if (!b) {
        return push(vm, 1);
    }
    if (strcmp(from, to) != 0) {
        blob_drop(vm, to);
        b = blob_find(vm, from);
        if (!b) {
            return push(vm, 1);
        }
        snprintf(b->path, sizeof b->path, "%s", to);
    }
    for (i = 0; i < EF_OPEN; i++) {
        if (vm->files[i].used && strcmp(vm->files[i].path, from) == 0) {
            snprintf(vm->files[i].path, sizeof vm->files[i].path, "%s", to);
        }
    }
    return push(vm, 0);
}

static int p_file_status(ef_vm *vm) {
    char name[512];
    int len = 0;
    if (pop_text(vm, name, (int)sizeof name)) {
        return EF_THROW;
    }
    slashes(name);
    if (!file_path_ok(name) || !file_known(vm, name, &len)) {
        if (push(vm, 0)) {
            return EF_THROW;
        }
        return push(vm, 1);
    }
    if (push(vm, len)) {
        return EF_THROW;
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
    if (slot_of(vm, fid) < 0) {
        return push(vm, 1);
    }
    sync_slot(vm, slot_of(vm, fid));
    return push(vm, 0);
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

/* ( a-addr1 u -- a-addr2 ior ). The size cell sits just before the payload.
   The newest block grows or shrinks in place. An older block that must grow
   is copied to a new block. A negative size leaves the address and ior 1. */
static int p_resize(ef_vm *vm) {
    int32_t addr, u, old, take, next, copy_n;
    int32_t nh, np, nt;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    if (u < 0 || addr < HEAP_ORIGIN || (addr & 3)) {
        return push2(vm, addr, 1);
    }
    if (load_cell(vm, addr - CELL, &old)) {
        return EF_THROW;
    }
    if (old < 0 || (size_t)addr + (size_t)old > vm->count) {
        return push2(vm, addr, 1);
    }
    if ((uint32_t)u > 0x7FFFFFFCu) {
        return push2(vm, addr, 1);
    }
    take = (int32_t)(((uint32_t)u + 3u) & ~3u);
    if (addr == vm->heap_last) {
        if ((uint32_t)take > (uint32_t)(INT32_MAX - addr)) {
            return push2(vm, addr, 1);
        }
        next = addr + take;
        if (ensure(vm, next)) {
            return EF_THROW;
        }
        if (store_cell(vm, addr - CELL, u)) {
            return EF_THROW;
        }
        vm->heap_at = next;
        return push2(vm, addr, 0);
    }
    if (u <= old) {
        if (store_cell(vm, addr - CELL, u)) {
            return EF_THROW;
        }
        return push2(vm, addr, 0);
    }
    nh = (vm->heap_at + 3) & ~3;
    np = nh + CELL;
    if ((uint32_t)take > (uint32_t)(INT32_MAX - np)) {
        return push2(vm, addr, 1);
    }
    nt = np + take;
    if (ensure(vm, nt)) {
        return EF_THROW;
    }
    if (store_cell(vm, nh, u)) {
        return EF_THROW;
    }
    copy_n = old;
    if (copy_n > 0) {
        memcpy(vm->base + np, vm->base + addr, (size_t)copy_n);
    }
    vm->heap_last = np;
    vm->heap_at = nt;
    return push2(vm, np, 0);
}

/* A block is 1024 bytes, 16 lines of 64. A newline or a NUL in the buffer
   would end a line early, so those bytes are blanks. The newline after
   each line is only a separator for the line loader. */
static int block_to_lines(const uint8_t *src, int n, char **out, int *outlen) {
    int i, o, lines;
    char *d;
    if (n < 0) {
        n = 0;
    }
    if (n > 1024) {
        n = 1024;
    }
    lines = n == 0 ? 0 : (n + 63) / 64;
    d = malloc((size_t)lines * 65 + 1);
    if (!d) {
        return -1;
    }
    o = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = src[i];
        if (c == '\n' || c == '\r' || c == 0) {
            c = ' ';
        }
        d[o++] = (char)c;
        if ((i & 63) == 63) {
            d[o++] = '\n';
        }
    }
    if (n > 0 && (n & 63) != 0) {
        d[o++] = '\n';
    }
    d[o] = 0;
    *out = d;
    *outlen = o;
    return 0;
}

/* Copy block blk through the Forth BLOCK word so the buffer cache stays
   the one UPDATE and FLUSH already use. */
static int fetch_block_text(ef_vm *vm, int32_t blk, char **text, int *len) {
    int32_t cfa, addr;
    cfa = find_word(vm, "BLOCK", 5, NULL);
    if (!cfa) {
        return fail(vm, -13, "undefined: BLOCK\n");
    }
    if (push(vm, blk)) {
        return EF_THROW;
    }
    if (exec_one(vm, cfa, 0)) {
        return EF_THROW;
    }
    if (pop(vm, &addr)) {
        return EF_THROW;
    }
    if (!span_ok(vm, addr, 1024)) {
        return fail(vm, -16, "bad address\n");
    }
    if (block_to_lines(vm->base + addr, 1024, text, len)) {
        return fail(vm, -8, "out of memory\n");
    }
    return EF_OK;
}

static int reload_block(ef_vm *vm, int32_t blk) {
    char *text = NULL;
    int len = 0;
    int rc = fetch_block_text(vm, blk, &text, &len);
    if (rc) {
        return rc;
    }
    free(vm->load_text);
    vm->load_text = text;
    vm->load_len = len;
    vm->load_blk = blk;
    if (vm->blk_addr && store_cell(vm, vm->blk_addr, blk)) {
        return EF_THROW;
    }
    return EF_OK;
}

static int refill_block(ef_vm *vm) {
    char *text = NULL;
    int len = 0;
    int got;
    int32_t next = vm->load_blk + 1;
    if (fetch_block_text(vm, next, &text, &len)) {
        return EF_THROW;
    }
    free(vm->load_text);
    vm->load_text = text;
    vm->load_len = len;
    vm->load_pos = 0;
    vm->load_stop = 0;
    vm->load_line = 1;
    vm->load_line_at = 0;
    vm->load_blk = next;
    if (vm->blk_addr) {
        store_cell(vm, vm->blk_addr, next);
    }
    got = load_next_line(vm);
    if (got < 0) {
        return EF_THROW;
    }
    return push(vm, got ? -1 : 0);
}

static int load_source(ef_vm *vm, int32_t addr, int32_t u, int as_block, int32_t blk_n) {
    char *copy = NULL;
    char saved_name[256];
    src_frame saved;
    char *old_text;
    int old_len, old_pos, old_stop, old_line, old_at, old_gen, old_on, old_blk;
    int text_len = 0;
    int saved_file;
    int rc, got, code;
    int32_t saved_blk_cell = 0;
    if (u < 0 || u > EF_SRC_MAX - 1 || !span_ok(vm, addr, u)) {
        return fail(vm, -16, "bad address\n");
    }
    if (as_block) {
        int n = u > 1024 ? 1024 : (int)u;
        if (block_to_lines(vm->base + addr, n, &copy, &text_len)) {
            return fail(vm, -8, "out of memory\n");
        }
    } else {
        copy = malloc((size_t)u + 1);
        if (!copy) {
            return fail(vm, -8, "out of memory\n");
        }
        if (u > 0) {
            memcpy(copy, vm->base + addr, (size_t)u);
        }
        copy[u] = 0;
        text_len = (int)u;
    }
    if (save_source(vm, &saved)) {
        free(copy);
        return fail(vm, -8, "out of memory\n");
    }
    old_text = vm->load_text;
    old_len = vm->load_len;
    old_pos = vm->load_pos;
    old_stop = vm->load_stop;
    old_line = vm->load_line;
    old_at = vm->load_line_at;
    old_gen = vm->load_gen;
    old_on = vm->load_on;
    old_blk = vm->load_blk;
    if (as_block && vm->blk_addr) {
        load_cell(vm, vm->blk_addr, &saved_blk_cell);
    }
    vm->load_text = copy;
    vm->load_len = text_len;
    vm->load_pos = 0;
    vm->load_stop = 0;
    vm->load_line = 1;
    vm->load_line_at = 0;
    vm->load_gen = old_gen + 1;
    vm->load_on = 1;
    vm->load_blk = as_block ? blk_n : -1;
    if (as_block && vm->blk_addr) {
        store_cell(vm, vm->blk_addr, blk_n);
    }
    saved_file = vm->file_id;
    snprintf(saved_name, sizeof saved_name, "%s", vm->src_name);
    if (vm->last_open[0]) {
        snprintf(vm->src_name, sizeof vm->src_name, "%s", vm->last_open);
    }
    vm->file_id = vm->next_file++;
    if (vm->next_file > 0x1fff) {
        vm->next_file = EF_VIEW_C + 1;
    }
    remember_view(vm, vm->file_id, vm->src_name);
    /* A block's SOURCE-ID is 0. Do not echo the block lines. */
    vm->src_echo = as_block ? 0 : 1;
    vm->source_id = as_block ? 0 : 1;
    vm->throw_code = 0;
    got = load_next_line(vm);
    if (got < 0) {
        rc = EF_THROW;
    } else if (!got) {
        rc = EF_OK;
    } else {
        rc = interpret(vm);
    }
    code = 0;
    if (rc != EF_OK) {
        code = vm->throw_code ? vm->throw_code : -1;
        if (vm->err_line == 0) {
            vm->err_line = vm->line > 0 ? vm->line : 1;
            snprintf(vm->err_name, sizeof vm->err_name, "%s", vm->src_name);
        }
    }
    restore_source(vm, &saved);
    free(vm->load_text);
    vm->load_text = old_text;
    vm->load_len = old_len;
    vm->load_pos = old_pos;
    vm->load_stop = old_stop;
    vm->load_line = old_line;
    vm->load_line_at = old_at;
    vm->load_gen = old_gen;
    vm->load_on = old_on;
    vm->load_blk = old_blk;
    if (as_block && vm->blk_addr) {
        store_cell(vm, vm->blk_addr, saved_blk_cell);
    }
    vm->file_id = saved_file;
    snprintf(vm->src_name, sizeof vm->src_name, "%s", saved_name);
    if (rc != EF_OK) {
        recover(vm);
        vm->throw_code = code;
        return EF_THROW;
    }
    return EF_OK;
}

static int p_line_source(ef_vm *vm) {
    int32_t addr, u;
    if (pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    return load_source(vm, addr, u, 0, 0);
}

static int p_blk_load(ef_vm *vm) {
    int32_t addr, u, blk;
    if (pop(vm, &blk) || pop(vm, &u) || pop(vm, &addr)) {
        return EF_THROW;
    }
    return load_source(vm, addr, u, 1, blk);
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
    s = src_ptr(vm);
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

/* A relative name that file_path_ok rejects (a "." or ".." segment) is folded
   against the directory that name was written against. BEGIN-LOAD-CWD has
   already moved include_dir, so the directory saved on cwd_stack is first.
   A name that already passes is left alone. ".." may not climb above that
   directory. An empty result stays a failed open. */
static int collapse_include(ef_vm *vm, char *name, size_t namemax) {
    char joined[512];
    const char *bases[3];
    int nbase = 0;
    int i;
    if (file_path_ok(name)) {
        return 1;
    }
    if (vm->cwd_n > 0 && vm->cwd_stack[vm->cwd_n - 1][0]) {
        bases[nbase++] = vm->cwd_stack[vm->cwd_n - 1];
    }
    if (vm->include_dir[0]) {
        bases[nbase++] = vm->include_dir;
    }
    if (vm->logical_cwd[0]) {
        bases[nbase++] = vm->logical_cwd;
    }
    if (nbase == 0) {
        bases[nbase++] = "";
    }
    for (i = 0; i < nbase; i++) {
        size_t n;
        if (join_rel(bases[i], name, joined, sizeof joined)) {
            continue;
        }
        if (!joined[0] || !file_path_ok(joined)) {
            continue;
        }
        n = strlen(joined);
        if (n + 1 > namemax) {
            continue;
        }
        memcpy(name, joined, n + 1);
        return 1;
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
        fromlib_base_path(got ? spec : "", base, sizeof base);
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
        fromlib_base_path(got ? spec : "", base, sizeof base);
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
        const char *base = old[0] ? old : (vm->logical_cwd[0] ? vm->logical_cwd : "");
        int ok = 0;
        if (base[0]) {
            /* Harness/../fp becomes .../src/fp. A climb above the root leaves
               include_dir where it was. */
            ok = !join_rel(base, parent, joined, sizeof joined) &&
                 joined[0] && path_ok(joined);
        } else {
            snprintf(joined, sizeof joined, "%s", parent);
            slashes(joined);
            ok = path_ok(joined);
        }
        if (ok) {
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
    { "ROLL", p_roll, 0 },
    { "CS-PICK", p_cs_pick, 0 },
    { "CS-ROLL", p_cs_roll, 0 },
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
    { "ACCEPT", p_accept, 0 },
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
    { "(C\")", p_c_quote_rt, 0 },
    { "C\"", p_c_quote, 1 },
    { ":", p_colon, 0 },
    { ":NONAME", p_noname, 0 },
    { ";", p_semicolon, 1 },
    { "QUIT", p_quit, 0 },
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
    { "\\S", p_backslash_s, 1 },
    { "(", p_paren, 1 },
    { ".\"", p_dot_quote, 1 },
    { "S\"", p_s_quote, 1 },
    { "S\\\"", p_s_escape, 1 },
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
    { "ORDER", p_order, 0 },
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
    { "SOURCE-ID", p_source_id, 0 },
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
    { "PAUSE", p_pause, 0 },
    { "LATEST", p_latest, 0 },
    { "EVALUATE", p_evaluate, 0 },
    { "GET-CURRENT", p_get_current, 0 },
    { "SET-CURRENT", p_set_current, 0 },
    { "REDEF-WARNING", p_redef, 0 },
    { "BLOCK-FILE", p_block_file, 0 },
    { "(BLOCK-UPD)", p_block_upd, 0 },
    { "(BLOCK-NR)", p_block_nr, 0 },
    { "SCR", p_scr, 0 },
    { "(BLOCK-BUF)", p_block_buf, 0 },
    { "R/O", p_ro, 0 },
    { "W/O", p_wo, 0 },
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
    { "FM/MOD", p_fm_mod, 0 },
    { "D+", p_d_plus, 0 },
    { "D-", p_d_minus, 0 },
    { "D0=", p_d_zero_eq, 0 },
    { "D2*", p_d_two_star, 0 },
    { "D2/", p_d_two_slash, 0 },
    { "D<", p_d_lt, 0 },
    { "D=", p_d_eq, 0 },
    { "D>S", p_d_to_s, 0 },
    { "DMAX", p_d_max, 0 },
    { "DMIN", p_d_min, 0 },
    { "DU<", p_du_lt, 0 },
    { "S>D", p_s_to_d, 0 },
    { "M+", p_m_plus, 0 },
    { "2ROT", p_two_rot, 0 },
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
    { "SEARCH", p_search, 0 },
    { ">NUMBER", p_to_number, 0 },
    { "ENVIRONMENT?", p_environment, 0 },
    { "MS", p_ms, 0 },
    { "TIME&DATE", p_time_date, 0 },
    { "AT-XY", p_at_xy, 0 },
    { "PAGE", p_page, 0 },
    { "PARSE-NAME", p_parse_name, 0 },
    { "PARSE-FILESPEC", p_parse_filespec, 0 },
    { "ALSO", p_also, 0 },
    { "PREVIOUS", p_previous, 0 },
    { "TO", p_to, 1 },
    { "CATCH", p_catch, 0 },
    { "FORGET", p_forget, 0 },
    { "NAME>INTERPRET", p_name_interpret, 0 },
    { "NAME>COMPILE", p_name_compile, 0 },
    { "N>R", p_n_to_r, 0 },
    { "NR>", p_n_r_from, 0 },
    { "TRAVERSE-WORDLIST", p_traverse, 0 },
    { "WORDLISTS", p_wordlists, 0 },
    { "(REGISTER-WID)", p_register_wid, 0 },
    { "VIEW-PATH", p_view_path, 0 },
    { "(HOST-VIEW)", p_host_view, 0 },
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
    { "READ-LINE", p_read_line, 0 },
    { "WRITE-FILE", p_write_file, 0 },
    { "WRITE-LINE", p_write_line, 0 },
    { "FILE-POSITION", p_file_position, 0 },
    { "DELETE-FILE", p_delete_file, 0 },
    { "RENAME-FILE", p_rename_file, 0 },
    { "RESIZE-FILE", p_resize_file, 0 },
    { "FILE-STATUS", p_file_status, 0 },
    { "REPOSITION-FILE", p_reposition, 0 },
    { "FLUSH-FILE", p_flush_file, 0 },
    { "ALLOCATE", p_allocate, 0 },
    { "FREE", p_free, 0 },
    { "RESIZE", p_resize, 0 },
    { "(LINE-SOURCE)", p_line_source, 0 },
    { "(LOAD-RUN)", p_line_source, 0 },
    { "(BLK-LOAD)", p_blk_load, 0 },
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
    else if (strcmp(name, "FLIT") == 0) vm->xt_flit = cfa;
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
    else if (strcmp(name, "(C\")") == 0) vm->xt_cquote = cfa;
    else if (strcmp(name, "(DOES>)") == 0) vm->xt_does = cfa;
    else if (strcmp(name, ".(") == 0) vm->xt_dotparen = cfa;
    else if (strcmp(name, "!") == 0) vm->xt_store = cfa;
    else if (strcmp(name, "2@") == 0) vm->xt_two_fetch = cfa;
    else if (strcmp(name, "2!") == 0) vm->xt_two_store = cfa;
    else if (strcmp(name, "LOCAL-INIT") == 0) vm->xt_linit = cfa;
    else if (strcmp(name, "(LOCAL@)") == 0) vm->xt_lat = cfa;
    else if (strcmp(name, "(LOCAL!)") == 0) vm->xt_lstore = cfa;
    else if (strcmp(name, "DBG-OFF") == 0) vm->xt_dbg_off = cfa;
    else if (strcmp(name, "EXECUTE") == 0) vm->xt_execute = cfa;
    else if (strcmp(name, "CATCH") == 0) vm->xt_catch = cfa;
}

static const struct {
    const char *name;
    int line;
} prim_c_lines[] = {
#include "prim_lines.inc"
};

static int prim_c_line(const char *name) {
    size_t i;
    for (i = 0; i < sizeof prim_c_lines / sizeof prim_c_lines[0]; i++) {
        if (strcmp(prim_c_lines[i].name, name) == 0) {
            return prim_c_lines[i].line;
        }
    }
    return 0;
}

static void register_all(ef_vm *vm) {
    size_t i;
    int saved_line = vm->line;
    int saved_file = vm->file_id;
    g_prim_count = 0;
    memset(g_prims, 0, sizeof g_prims);
    remember_view(vm, EF_VIEW_C, "Resources/System/Sources/ef32.c");
    vm->file_id = EF_VIEW_C;
    for (i = 0; i < sizeof k_prims / sizeof k_prims[0]; i++) {
        int32_t id;
        if (g_prim_count >= (int)(sizeof g_prims / sizeof g_prims[0])) {
            fail(vm, -8, "too many primitives\n");
            vm->line = saved_line;
            vm->file_id = saved_file;
            return;
        }
        id = g_prim_count++;
        int32_t cfa;
        g_prims[id] = k_prims[i].fn;
        vm->line = prim_c_line(k_prims[i].name);
        cfa = header(vm, k_prims[i].name, (int)strlen(k_prims[i].name), id);
        if (!cfa) {
            vm->line = saved_line;
            vm->file_id = saved_file;
            return;
        }
        if (k_prims[i].imm) {
            int32_t f = uv(vm, cfa - CELL);
            uv_set(vm, cfa - CELL, (int32_t)((uint32_t)f | FFA1_IMM));
        }
        note_special(vm, k_prims[i].name, cfa);
    }
    {
        static const char doc[] =
            "FILE-ECHO ( -- addr ) echo INCLUDE/FLOAD lines when nonzero";
        int32_t cfa;
        memcpy(vm->doc, doc, sizeof doc - 1);
        vm->doc_len = (int)(sizeof doc - 1);
        vm->line = 0;
        cfa = header(vm, "FILE-ECHO", 9, EF_DOVAR);
        if (!cfa || comma(vm, 0)) {
            vm->line = saved_line;
            vm->file_id = saved_file;
            return;
        }
        vm->file_echo_addr = cfa + 2 * CELL;
    }
    {
        static const char doc[] =
            "BLK ( -- addr ) block number variable (0 = not from a block)";
        int32_t cfa;
        memcpy(vm->doc, doc, sizeof doc - 1);
        vm->doc_len = (int)(sizeof doc - 1);
        vm->line = 0;
        cfa = header(vm, "BLK", 3, EF_DOVAR);
        if (!cfa || comma(vm, 0)) {
            vm->line = saved_line;
            vm->file_id = saved_file;
            return;
        }
        vm->blk_addr = cfa + 2 * CELL;
    }
    vm->line = saved_line;
    vm->file_id = saved_file;
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
    vm->src_at = SRC_ADDR;
    memcpy(vm->base + EF_ENV_UTF8, "UTF-8", 5);
    vm->forth_wid = FORTH_WID;
    vm->next_file = EF_VIEW_C + 1;
    uv_set(vm, UV_DP, DICT_ORIGIN);
    uv_set(vm, UV_BASE, 10);
    uv_set(vm, UV_STATE, 0);
    uv_set(vm, UV_CURRENT, FORTH_WID);
    uv_set(vm, UV_ORDERN, 1);
    uv_set(vm, UV_ORDER, FORTH_WID);
    uv_set(vm, UV_BLOCK_NR, -1);
    vm->load_blk = -1;
    vm->fprec = 15;
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
    while (vm->blobs) {
        ef_blob *next = vm->blobs->next;
        free(vm->blobs->data);
        free(vm->blobs);
        vm->blobs = next;
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

void ef_set_view(ef_vm *vm, ef_view_fn fn, void *ctx) {
    if (!vm) {
        return;
    }
    vm->view_fn = fn;
    vm->view_ctx = ctx;
}

void ef_set_fetch(ef_vm *vm, ef_fetch_fn fn, void *ctx) {
    if (!vm) {
        return;
    }
    vm->fetch_fn = fn;
    vm->fetch_ctx = ctx;
}

void ef_set_key(ef_vm *vm, ef_key_fn fn, void *ctx) {
    vm->key = fn;
    vm->key_ctx = ctx;
}

void ef_set_key_q(ef_vm *vm, ef_keyq_fn fn, void *ctx) {
    if (!vm) {
        return;
    }
    vm->key_q = fn;
    vm->key_q_ctx = ctx;
}

int ef_eval(ef_vm *vm, const char *text) {
    src_frame saved;
    size_t n;
    int rc;
    int top;
    int saved_inner;
    int hold_line;
    int start_line;
    if (!text) {
        text = "";
    }
    hold_line = vm->src_line_set;
    start_line = vm->src_line > 0 ? vm->src_line : 1;
    vm->src_line_set = 0;
    saved_inner = vm->in_inner;
    top = vm->eval_depth == 0;
    if (top) {
        vm->err_line = 0;
        vm->err_name[0] = 0;
        vm->quit_req = 0;
    }
    vm->eval_depth++;
    if (save_source(vm, &saved)) {
        vm->echo_file = 0;
        vm->eval_depth--;
        vm->in_inner = saved_inner;
        return fail(vm, -8, "out of memory\n"), vm->throw_code;
    }
    n = strlen(text);
    if (n > EF_SRC_MAX - 1) {
        vm->echo_file = 0;
        restore_source(vm, &saved);
        vm->eval_depth--;
        vm->in_inner = saved_inner;
        return fail(vm, -16, "input too long\n"), vm->throw_code;
    }
    if (ensure(vm, SRC_ADDR + (int32_t)n + 1)) {
        vm->echo_file = 0;
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
    vm->src_at = SRC_ADDR;
    vm->src_len = (int)n;
    uv_set(vm, UV_IN, 0);
    vm->file_echo_pos = 0;
    vm->source_id = vm->echo_file ? 1 : 0;
    vm->src_echo = vm->echo_file ? 1 : 0;
    vm->echo_file = 0;
    vm->throw_code = 0;
    vm->bye = 0;
    {
        int saved_load = vm->load_on;
        vm->load_on = 0;
        stamp_source(vm);
        if (hold_line) {
            vm->line = start_line;
            vm->hold_line = 1;
        }
        rc = interpret(vm);
        vm->hold_line = 0;
        vm->load_on = saved_load;
    }
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
        if (vm->eval_depth == 0 && vm->dbg_armed && !vm->dbg_busy) {
            dbg_disarm(vm);
        }
        return code;
    }
    vm->in_inner = saved_inner;
    /* Break Now arms without a matching DBG-OFF. A finished outer eval is
       back at the prompt, so the session does not leak into the next line. */
    if (vm->eval_depth == 0 && vm->dbg_armed && !vm->dbg_busy) {
        dbg_disarm(vm);
    }
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

static int break_bodies(ef_vm *vm, int32_t *table, int32_t *enables) {
    int32_t t = find_word(vm, "BREAK-TABLE", 11, NULL);
    int32_t e = find_word(vm, "BREAK-ENABLES", 13, NULL);
    if (!t || !e) {
        return -1;
    }
    *table = t + 2 * CELL;
    *enables = e + 2 * CELL;
    return 0;
}

static int break_name_ok(const char *name, int *n) {
    int i;
    if (!name || !name[0]) {
        return 0;
    }
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 32 || c > 126) {
            return 0;
        }
    }
    if (i > 63) {
        return 0;
    }
    *n = i;
    return 1;
}

int ef_break_toggle(ef_vm *vm, const char *name) {
    int n = 0;
    int i;
    int empty = -1;
    int32_t xt;
    int32_t table;
    int32_t enables;
    int32_t slot = 0;
    if (!break_name_ok(name, &n)) {
        return 0;
    }
    xt = find_word(vm, name, n, NULL);
    if (!xt || break_bodies(vm, &table, &enables)) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        if (load_quiet(vm, table + i * CELL, &slot)) {
            return 0;
        }
        if (slot == xt) {
            if (store_cell(vm, table + i * CELL, 0) ||
                store_cell(vm, enables + i * CELL, 0)) {
                return 0;
            }
            return 2;
        }
        if (!slot && empty < 0) {
            empty = i;
        }
    }
    if (empty < 0) {
        return -1;
    }
    if (store_cell(vm, table + empty * CELL, xt) ||
        store_cell(vm, enables + empty * CELL, -1)) {
        return 0;
    }
    return 1;
}

int ef_break_clear(ef_vm *vm, const char *name) {
    int n = 0;
    int i;
    int32_t xt;
    int32_t table;
    int32_t enables;
    int32_t slot = 0;
    if (!break_name_ok(name, &n)) {
        return 0;
    }
    xt = find_word(vm, name, n, NULL);
    if (!xt || break_bodies(vm, &table, &enables)) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        if (load_quiet(vm, table + i * CELL, &slot)) {
            return 0;
        }
        if (slot == xt) {
            if (store_cell(vm, table + i * CELL, 0) ||
                store_cell(vm, enables + i * CELL, 0)) {
                return 0;
            }
            return 1;
        }
    }
    return 0;
}

int ef_break_set_enabled(ef_vm *vm, const char *name, int enabled) {
    int n = 0;
    int i;
    int32_t xt;
    int32_t table;
    int32_t enables;
    int32_t slot = 0;
    if (!break_name_ok(name, &n)) {
        return 0;
    }
    xt = find_word(vm, name, n, NULL);
    if (!xt || break_bodies(vm, &table, &enables)) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        if (load_quiet(vm, table + i * CELL, &slot)) {
            return 0;
        }
        if (slot == xt) {
            if (store_cell(vm, enables + i * CELL, enabled ? -1 : 0)) {
                return 0;
            }
            return 1;
        }
    }
    return 0;
}

void ef_break_list(ef_vm *vm, char *dst, int dstmax) {
    int32_t table;
    int32_t enables;
    int i;
    int used = 0;
    if (!dst || dstmax < 1) {
        return;
    }
    dst[0] = 0;
    if (break_bodies(vm, &table, &enables)) {
        return;
    }
    for (i = 0; i < 8; i++) {
        int32_t slot = 0;
        int32_t en = 0;
        char name[80];
        int n;
        int k;
        if (load_quiet(vm, table + i * CELL, &slot) ||
            load_quiet(vm, enables + i * CELL, &en) || !slot) {
            continue;
        }
        copy_name(vm, slot, name, (int)sizeof name);
        if (!name[0]) {
            continue;
        }
        n = (int)strlen(name);
        if (used + n + 3 >= dstmax) {
            break;
        }
        for (k = 0; k < n; k++) {
            dst[used++] = name[k];
        }
        dst[used++] = '\t';
        dst[used++] = en ? '1' : '0';
        dst[used++] = '\n';
        dst[used] = 0;
    }
}

void ef_debug_bp_go(ef_vm *vm) {
    vm->dbg_bp_go = 1;
}

int ef_debug_armed(const ef_vm *vm) {
    return vm->dbg_armed;
}

void ef_debug_request_break(ef_vm *vm) {
    vm->dbg_break_asap = 1;
}

void ef_set_yield(ef_vm *vm, int on) {
    vm->dbg_yield = on ? 1 : 0;
}

void ef_last_name(ef_vm *vm, char *dst, int dstmax) {
    if (!dst || dstmax < 1) {
        return;
    }
    copy_name(vm, uv(vm, UV_LAST), dst, dstmax);
}
