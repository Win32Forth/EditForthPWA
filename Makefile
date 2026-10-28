# EditForthPWA — native smoke test and the browser page.
# There is no Xcode project. cc builds the terminal. emcc builds the page.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CPPFLAGS = -Ikernel -Ibuild

EMCC    ?= emcc
# gnu11: EM_ASM in host/web_main.c is rejected under -std=c11.
EMFLAGS  = -O2 -std=gnu11 -Wall -Wextra $(CPPFLAGS)
EMFLAGS += -s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createEditForth
EMFLAGS += -s EXPORTED_FUNCTIONS='["_ef_web_boot","_ef_web_eval","_ef_web_mount_clear","_ef_web_mount","_ef_web_set_source","_ef_web_output","_ef_web_depth","_ef_web_item","_ef_web_state","_ef_web_bye","_ef_web_error_line","_ef_web_error_name","_ef_web_boot_source","_ef_web_post_key","_ef_web_key_waiting","_malloc","_free"]'
EMFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","UTF8ToString"]'
EMFLAGS += -s ENVIRONMENT=web,node -s ALLOW_MEMORY_GROWTH=1 -s EXIT_RUNTIME=0
EMFLAGS += -s ASYNCIFY=1 -s ASYNCIFY_STACK_SIZE=65536
# cwrap copies each string argument onto the wasm stack. A source file can be 256KB.
EMFLAGS += -s STACK_SIZE=2097152

.PHONY: all test self-test wasm wasm-test serve clean icons

all: build/editforth

# Cold order: boot, kernel1, ef-bridge, kernel2, vocemit, app-output, ef-long, app-points, vocsys, xref, ef-hooks.
build/boot.inc: forth/boot.fth forth/kernel1.fth forth/ef-bridge.fth forth/kernel2.fth forth/vocemit.fth forth/app-output.fth forth/ef-long.fth forth/app-points.fth forth/vocsys.fth forth/xref.fth forth/ef-hooks.fth scripts/embed.py
	mkdir -p build
	python3 scripts/embed.py build/boot.inc forth/boot.fth forth/kernel1.fth forth/ef-bridge.fth forth/kernel2.fth forth/vocemit.fth forth/app-output.fth forth/ef-long.fth forth/app-points.fth forth/vocsys.fth forth/xref.fth forth/ef-hooks.fth

build/editforth: kernel/ef32.c kernel/ef32.h host/main.c build/boot.inc
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ kernel/ef32.c host/main.c

self-test: build/editforth
	./build/editforth --self-test

test: build/editforth
	./build/editforth --self-test
	./build/editforth --test test/smoke.fth

web/icon-192.png web/icon-512.png: scripts/make_icon.py
	python3 scripts/make_icon.py

icons: web/icon-192.png web/icon-512.png

wasm: build/boot.inc web/icon-192.png web/icon-512.png host/web_main.c
	$(EMCC) $(EMFLAGS) -o web/editforth.js kernel/ef32.c host/web_main.c

wasm-test: wasm
	node web/wasm-test.mjs

serve: wasm
	python3 -m http.server 8080 --directory web

clean:
	rm -rf build web/editforth.js web/editforth.wasm web/editforth.wasm.map
