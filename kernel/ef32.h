/*
 * EditForthPWA — 32-bit indirect-threaded engine.
 *
 * Cells are int32. The code field holds a primitive id (or a DOCOL / DOVAR /
 * DOCON / DODOES marker). Function pointers stay outside the dictionary.
 *
 * Header, low to high, CELL = 4:
 *   HFA    counted help, padded to a cell (empty help is a 0 count byte)
 *   NFA    counted UPPERCASE name, padded to a cell
 *          count bits 0–6 = length (max 127), bit 7 = SMUDGE
 *   LFA    previous CFA in this hash thread          @ CFA - 3 CELLS
 *   FFA0   NFA offset (0–15), HFA offset (16–31)     @ CFA - 2 CELLS
 *   FFA1   VIEW line (0–15), file-id (16–28),
 *          INLINE 29, EMM 30, IMM 31                 @ CFA - 1 CELL
 *   CFA    code id                                   the xt
 *   INFO   DOES> fragment, or 0                      @ CFA + 1 CELL
 *   BODY   colon tokens or CREATE data               @ CFA + 2 CELLS
 *
 * IMM is bit 31 of FFA1.
 *
 * BRANCH / 0BRANCH offsets are destination minus the cell after the offset.
 * (EF-BRANCH) / (EF-0BRANCH) use the other form: ip = offset-cell + offset.
 */
#ifndef EF32_H
#define EF32_H

#include <stdint.h>

typedef struct ef_vm ef_vm;

typedef void (*ef_emit_fn)(void *ctx, int ch);
/* Return the next input byte, or -1 if none. */
typedef int (*ef_key_fn)(void *ctx);

ef_vm *ef_create(void);
void ef_destroy(ef_vm *vm);

void ef_set_emit(ef_vm *vm, ef_emit_fn fn, void *ctx);
void ef_set_key(ef_vm *vm, ef_key_fn fn, void *ctx);

/* Interpret text. Newlines are whitespace. '\' comments end at newline.
   Returns 0, or a THROW code. After a THROW the VM is usable again:
   stacks cleared, STATE interpret, a smudged colon definition reclaimed. */
int ef_eval(ef_vm *vm, const char *text);

/* Run the embedded boot sources: forth/boot.fth, forth/kernel1.fth,
   forth/ef-bridge.fth, forth/kernel2.fth, forth/vocemit.fth,
   forth/app-output.fth, forth/ef-long.fth, forth/app-points.fth,
   forth/vocsys.fth, forth/xref.fth, then forth/ef-hooks.fth. */
int ef_boot(ef_vm *vm);

/* Page file tree. INCLUDE looks here before the real disk.
   A path is slash-separated, with no "." or ".." segment. */
int ef_mount_clear(ef_vm *vm);
int ef_mount_file(ef_vm *vm, const char *path, const char *text);

/* Name used for error locations, and as the directory for a relative INCLUDE.
   An empty name clears it. */
int ef_set_source(ef_vm *vm, const char *name);
int ef_error_line(const ef_vm *vm);
const char *ef_error_name(const ef_vm *vm);

int ef_depth(const ef_vm *vm);
/* from_bottom 0 is the oldest item. Returns 0 if out of range. */
int32_t ef_stack_item(const ef_vm *vm, int from_bottom);
/* Non-zero while a colon definition is being compiled. */
int ef_state(const ef_vm *vm);
int ef_throw_code(const ef_vm *vm);
int ef_bye_requested(const ef_vm *vm);

#endif
