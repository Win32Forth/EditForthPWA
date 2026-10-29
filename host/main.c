#include "ef32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char cap[65536];
static size_t cap_n;
static int capturing;

static const char *accept_keys;

static int accept_key(void *ctx) {
    const char **p = ctx;
    if (!p || !*p || !**p) {
        return -1;
    }
    return *(*p)++;
}

static int stdin_key(void *ctx) {
    FILE *fp = ctx ? ctx : stdin;
    int c;
    fflush(stdout);
    c = fgetc(fp);
    return c == EOF ? -1 : c;
}

static void emit_out(void *ctx, int ch) {
    FILE *fp = ctx ? ctx : stdout;
    if (ch == '\f' && isatty(fileno(fp))) {
        fputs("\033[2J\033[H", fp);
    } else {
        fputc(ch, fp);
    }
    if (capturing && cap_n + 1 < sizeof cap) {
        cap[cap_n++] = (char)ch;
        cap[cap_n] = 0;
    }
}

static int fail_msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    return 1;
}

static int expect_throw(ef_vm *vm, const char *src, int code) {
    int rc = ef_eval(vm, src);
    if (rc != code) {
        fprintf(stderr, "expected throw %d from [%s], got %d\n", code, src, rc);
        return 1;
    }
    return 0;
}

static int self_test(void) {
    ef_vm *vm = ef_create();
    int rc;
    int fails = 0;
    if (!vm) {
        return fail_msg("ef_create failed");
    }
    ef_set_emit(vm, emit_out, stdout);
    rc = ef_boot(vm);
    if (rc) {
        fprintf(stderr, "boot failed %d\n", rc);
        ef_destroy(vm);
        return 1;
    }
    fails += expect_throw(vm, "1 0 /", -9);
    fails += expect_throw(vm, "no-such-word", -13);
    fails += expect_throw(vm, "DROP", -3);
    rc = ef_eval(vm, "2 3 +");
    if (rc || ef_depth(vm) != 1 || ef_stack_item(vm, 0) != 5) {
        fprintf(stderr, "add failed rc=%d depth=%d item=%d\n", rc, ef_depth(vm),
                (int)ef_stack_item(vm, 0));
        fails++;
    }
    rc = ef_eval(vm, "DROP");
    if (rc || ef_depth(vm) != 0) {
        fprintf(stderr, "drop failed rc=%d depth=%d\n", rc, ef_depth(vm));
        fails++;
    }
    rc = ef_eval(vm, ": SQ");
    if (rc || ef_state(vm) == 0) {
        fprintf(stderr, "open colon failed rc=%d state=%d\n", rc, ef_state(vm));
        fails++;
    }
    rc = ef_eval(vm, "DUP * ;");
    if (rc || ef_state(vm) != 0) {
        fprintf(stderr, "close colon failed rc=%d state=%d\n", rc, ef_state(vm));
        fails++;
    }
    rc = ef_eval(vm, "6 SQ");
    if (rc || ef_depth(vm) != 1 || ef_stack_item(vm, 0) != 36) {
        fprintf(stderr, "SQ failed rc=%d depth=%d item=%d\n", rc, ef_depth(vm),
                (int)ef_stack_item(vm, 0));
        fails++;
    }
    capturing = 1;
    cap_n = 0;
    cap[0] = 0;
    rc = ef_eval(vm, ": HI .\"hello\" ; HI");
    capturing = 0;
    if (rc || strstr(cap, "hello") == NULL) {
        fprintf(stderr, "dot-quote failed rc=%d out=[%s]\n", rc, cap);
        fails++;
    }
    if (ef_mount_file(vm, "lib/a.fth", "INCLUDE b.fth\n") ||
        ef_mount_file(vm, "lib/b.fth", "20 22 +\n")) {
        fails += fail_msg("mount failed");
    }
    ef_set_source(vm, "lib/a.fth");
    rc = ef_eval(vm, "INCLUDE b.fth");
    if (rc || ef_depth(vm) < 1 || ef_stack_item(vm, ef_depth(vm) - 1) != 42) {
        fprintf(stderr, "mount include failed rc=%d depth=%d top=%d\n", rc, ef_depth(vm),
                ef_depth(vm) ? (int)ef_stack_item(vm, ef_depth(vm) - 1) : 0);
        fails++;
    }
    ef_set_source(vm, "lib/a.fth");
    rc = ef_eval(vm, "1 1 +\n1 0 /\n");
    if (rc != -9 || ef_error_line(vm) != 2 || strcmp(ef_error_name(vm), "lib/a.fth") != 0) {
        fprintf(stderr, "error loc rc=%d line=%d name=%s\n", rc, ef_error_line(vm),
                ef_error_name(vm));
        fails++;
    }
    rc = ef_eval(vm, "10 0 3 FM/MOD");
    if (rc || ef_depth(vm) < 2 || ef_stack_item(vm, ef_depth(vm) - 1) != 3 ||
        ef_stack_item(vm, ef_depth(vm) - 2) != 1) {
        fprintf(stderr, "fm/mod rc=%d depth=%d\n", rc, ef_depth(vm));
        fails++;
    } else {
        ef_eval(vm, "2DROP");
    }
    rc = ef_eval(vm, "-10 -1 3 FM/MOD");
    if (rc || ef_depth(vm) < 2 || ef_stack_item(vm, ef_depth(vm) - 1) != -4 ||
        ef_stack_item(vm, ef_depth(vm) - 2) != 2) {
        fprintf(stderr, "fm/mod floor rc=%d top=%d rem=%d\n", rc,
                ef_depth(vm) ? (int)ef_stack_item(vm, ef_depth(vm) - 1) : 0,
                ef_depth(vm) > 1 ? (int)ef_stack_item(vm, ef_depth(vm) - 2) : 0);
        fails++;
    } else {
        ef_eval(vm, "2DROP");
    }
    if (ef_mount_file(vm, "echo.fth", "1 2 +\n\n3\n")) {
        fails += fail_msg("echo mount failed");
    }
    capturing = 1;
    cap_n = 0;
    cap[0] = 0;
    rc = ef_eval(vm, "FILE-ECHO ON\n1 2 .\n");
    capturing = 0;
    if (rc || strstr(cap, "|") != NULL || strstr(cap, "2 ") == NULL) {
        fprintf(stderr, "file-echo console rc=%d out=[%s]\n", rc, cap);
        fails++;
    }
    capturing = 1;
    cap_n = 0;
    cap[0] = 0;
    rc = ef_eval(vm, "INCLUDE echo.fth\nFILE-ECHO OFF\n");
    capturing = 0;
    if (rc || strstr(cap, "    1| 1 2 +") == NULL || strstr(cap, "    2| ") == NULL ||
        strstr(cap, "    3| 3") == NULL) {
        fprintf(stderr, "file-echo include rc=%d out=[%s]\n", rc, cap);
        fails++;
    }
    {
        int before = ef_depth(vm);
        rc = ef_eval(vm, "1 2 QUIT 99");
        if (rc || ef_depth(vm) != before + 2 ||
            ef_stack_item(vm, ef_depth(vm) - 1) != 2 ||
            ef_stack_item(vm, ef_depth(vm) - 2) != 1) {
            fprintf(stderr, "quit rc=%d depth=%d top=%d\n", rc, ef_depth(vm),
                    ef_depth(vm) ? (int)ef_stack_item(vm, ef_depth(vm) - 1) : 0);
            fails++;
        } else {
            ef_eval(vm, "2DROP");
        }
    }
    accept_keys = "Hi\n";
    ef_set_key(vm, accept_key, &accept_keys);
    rc = ef_eval(vm, "PAD 8 ACCEPT");
    if (rc || ef_depth(vm) < 1 || ef_stack_item(vm, ef_depth(vm) - 1) != 2) {
        fprintf(stderr, "accept rc=%d top=%d\n", rc,
                ef_depth(vm) ? (int)ef_stack_item(vm, ef_depth(vm) - 1) : 0);
        fails++;
    } else {
        ef_eval(vm, "DROP");
    }
    ef_set_key(vm, NULL, NULL);
    if (ef_mount_file(vm, "sid.fth", "SOURCE-ID\n")) {
        fails += fail_msg("source-id mount failed");
    }
    rc = ef_eval(vm, "INCLUDE sid.fth");
    if (rc || ef_depth(vm) < 1 || ef_stack_item(vm, ef_depth(vm) - 1) != 1) {
        fprintf(stderr, "source-id rc=%d top=%d\n", rc,
                ef_depth(vm) ? (int)ef_stack_item(vm, ef_depth(vm) - 1) : 0);
        fails++;
    } else {
        ef_eval(vm, "DROP");
    }
    rc = ef_eval(vm, "INCLUDE no-such-editforth-file.fth");
    if (rc != -38) {
        fprintf(stderr, "missing include rc=%d\n", rc);
        fails++;
    }
    rc = ef_eval(vm, "BYE");
    if (rc || !ef_bye_requested(vm)) {
        fprintf(stderr, "BYE failed rc=%d bye=%d\n", rc, ef_bye_requested(vm));
        fails++;
    }
    ef_destroy(vm);
    if (fails) {
        return fail_msg("self-test failed");
    }
    printf("\nself-test ok\n");
    return 0;
}

static int run_path(ef_vm *vm, const char *path) {
    char *cmd;
    size_t n = strlen(path);
    int rc;
    cmd = malloc(n + 16);
    if (!cmd) {
        return fail_msg("out of memory");
    }
    memcpy(cmd, "INCLUDE ", 8);
    memcpy(cmd + 8, path, n + 1);
    rc = ef_eval(vm, cmd);
    free(cmd);
    if (rc) {
        fprintf(stderr, "throw %d\n", rc);
        return 1;
    }
    return 0;
}

static void repl(ef_vm *vm) {
    char line[4096];
    puts("EditForthPWA 32-bit. Empty line is ok. BYE quits.");
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        int rc = ef_eval(vm, line);
        if (ef_bye_requested(vm)) {
            break;
        }
        if (rc) {
            if (ef_error_name(vm)[0]) {
                printf("throw %d at %s:%d\n", rc, ef_error_name(vm), ef_error_line(vm));
            } else {
                printf("throw %d\n", rc);
            }
        } else {
            fputs(" ok\n", stdout);
        }
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    ef_vm *vm;
    int rc;
    if (argc >= 2 && strcmp(argv[1], "--self-test") == 0) {
        return self_test();
    }
    vm = ef_create();
    if (!vm) {
        return fail_msg("ef_create failed");
    }
    ef_set_emit(vm, emit_out, stdout);
    ef_set_key(vm, stdin_key, stdin);
    rc = ef_boot(vm);
    if (rc) {
        fprintf(stderr, "boot failed %d\n", rc);
        ef_destroy(vm);
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "--test") == 0) {
        capturing = 1;
        rc = run_path(vm, argv[2]);
        ef_destroy(vm);
        if (rc) {
            return 1;
        }
        if (strstr(cap, "ALL PASS") == NULL) {
            return fail_msg("smoke did not print ALL PASS");
        }
        return 0;
    }
    if (argc >= 2) {
        rc = run_path(vm, argv[1]);
        ef_destroy(vm);
        return rc ? 1 : 0;
    }
    repl(vm);
    ef_destroy(vm);
    return 0;
}
