# EditForthPWA

A 32-bit EditForth that runs in the browser. This release is 0.7.0. Cells are 4 bytes. The engine is C (`kernel/ef32.c`), compiled with `emcc` to WebAssembly. The page is the editor: a file tree, the source being edited, and a Forth console under it.

This folder is not an Xcode project. EditForth, EditForthWin, EditForthLin, and TZEditForth stay as they are. This tree does not write their document folders.

## Run

The page needs a local server. `file://` will not load the module.

```
make serve
```

Open `http://127.0.0.1:8080/` in Chrome or Edge. The editor shows line numbers and one tab per open file. Startup leaves no file open. An error opens that file in view mode, selects the line, and marks its number. View mode is read-only, with a yellow bar and an Edit button. Typing in view mode asks whether to switch to Edit. Edit the current file, then press Include, or F4, or Cmd+Enter, or Ctrl+Enter. Include runs the text in the editor and does not write it. Save, or Cmd+S or Ctrl+S, writes the file. Closing a changed file asks Save, Don't Save, or Cancel. Switching tabs does not write the file. Restart boots a fresh image and clears the console. It does not include the open file. Shift with Cmd+Enter or Ctrl+Enter, or the Restart button, does the fresh boot. Include, Restart, RUN, and Pause sit on the strip between the editor and the console. RUN (F5) puts the word under the caret on the console line, or LAST when the caret is not on a word. Command-click fills `DEBUG`, and Command-Shift-click fills `BPGO`. Return runs that line. Pause stops at the next word defined after the debugger. Emit is not on the strip. Breakpoints lists the eight BREAK slots: the button toggles the caret word, a checkbox keeps a slot but turns it off, and Arm runs until an enabled break.

The menu to the left of the title is three lines. Open Folder is the first item. Show boot messages is the next item. It prints the `Loading:` and `Finished Loading:` lines from the latest boot. Those lines are kept off the console at startup and on Restart. Open Folder picks a directory. The tree lists that folder, and the choice is remembered for this site. If a tab has unsaved changes, Open Folder asks Save, Don't Save, or Cancel first. The folder's first file opens. An empty folder opens `Untitled.fth`. Until a folder is open, files stay in the browser's private storage and the first visit creates `work.fth`. The file list switches between Private and Resources. Resources is selected when the page opens. Private is that storage, or the folder from Open Folder. Resources is marked read only and lists the shared `Resources` folder served with the page. Those files open read only. Edit, or Make Editable in the menu, asks to copy the file to Private and then edits that copy. The shared file is not changed, including when Open Folder is pointed at the library. Read Only in the menu makes the current file read only again. A browser that cannot open its private file store, such as an iPad using the Mac's address, keeps the copy in the browser's own database. Share sends the open file through the system share sheet when that browser can share a file. Chrome and Edge otherwise ask where to save it. A browser with neither downloads the file. Cancelling either sheet does nothing. New opens an `Untitled.fth` tab. Save of that tab asks for a file name. Closing the last tab leaves the editor blank and not editable. The page does not delete files. `INCLUDE` reads the files in the tree. A name with no directory is looked up next to the file that is loading.

`CHDIR` prints `Current directory:`. `CHDIR documents` (also `desktop`, `downloads`, `music`, `pictures`, and `videos`) opens the folder picker when that name is not already a mounted directory. Cancel leaves the open folder as it is. `LS` and `DIR` list the current directory. `FROMLIB` makes the next `LS`, `DIR`, `CHDIR`, or `RESOLVE-KEY` start under `Resources/Library`.

When Chrome offers to install the page, Install adds a Dock window. Safari can run the editor on private storage, but it has no folder picker, so it cannot write edits back to a disk folder.

The terminal build is the same engine without the page:

```
make test
./build/editforth
./build/editforth some-file.fth
```

`make test` checks division by zero, an undefined word, and `test/smoke.fth`. `make wasm-test` checks the same engine under Node, including `INCLUDE` of mounted files. `make` from another directory has no rules. Run it in this folder. After a Forth source changes, if `make` keeps an old image, delete `build/boot.inc` and build again.

GitHub Pages serves this release at `https://win32forth.github.io/EditForthPWA/`. A push to `main` publishes the `web` folder. The publish step copies `Resources` as real files and writes a listing page in each folder, which is what the Resources button reads. Pages does not follow the `web/Resources` shortcut or list folders on its own.

`web/editforth.js` and `web/editforth.wasm` are the engine the page loads. Both are in the repository. `make wasm` rebuilds them from `kernel/ef32.c`, `host/web_main.c`, and the cold files in `forth/`. When that build changes, commit the new pair with the sources. `web/editforth.wasm.map` is not committed. `make clean` deletes the pair. Run `make wasm` to put it back. `build/` stays out of the repository. The terminal binary `build/editforth` is not committed.

The page cache is named in `web/sw.js`. After a rebuild, reload the page so the new script and wasm are fetched.

## Editor

The window is white (`#ffffff`) with black text (`#111111`). Include is a black button. Errors are `#9b1c1c`.

The bar between the source and the console drags. Dragging up gives the console more room. The height is remembered (`editforth.consoleHeight`). The console stays under the source. There is no undock. The bar on the right of the file list drags sideways. The list starts at 220px, and the width is remembered (`editforth.treeWidth`). Save sits with New and Share. Close is the tab’s own button.

The console starts with `=== EditForthPWA 0.7.0 === Oct 28, 2026 10:56 PM ===`. `CLS` clears back to that line and the prompt. The Forth command line is the tail of the console text, on the line under the last output. The prompt is `ok(n)> `, with a trailing space. `n` is how many values are on the data stack. Return runs that tail. The output follows, and a new prompt is drawn in the same text. Up and Down recall earlier lines into that tail. An unknown name prints `undefined: NAME`, then `file:line`, then `throw -13`, in red, and the editor selects that line.

`KEY` and `EKEY` wait for the next key typed in the console. `KEY` returns the low 8 bits. `EKEY` returns the whole cell, so F6, F7, and F8 keep their tags. There is no canvas yet, so `GRAPHICS` words do not open a window.

## What loads

Boot embeds the files in `forth/`, in this order:

1. `boot.fth`
2. `kernel1.fth`
3. `ef-bridge.fth`
4. `kernel2.fth`
5. `vocemit.fth`
6. `app-output.fth`
7. `ef-long.fth`
8. `app-points.fth`
9. `vocsys.fth`
10. `xref.fth`
11. `ef-hooks.fth`

`ef-hooks.fth` then includes `Resources/Library/Debugger/debugger.fth`. That file loads `debug-bp.fth`, `dbg-pause.fth`, `dbg-ed.fth`, `dbg-map.fth`, and `debug-runto.fth`. The engine's last boot line is `Finished Loading: debugger.fth`. The page holds that line, and the other `Loading:` and `Finished Loading:` lines, until Show boot messages.

`kernel1.fth` and `kernel2.fth` are the 32-bit cold blobs. `vocemit.fth` builds `EMITTER`. `app-output.fth` builds `GRAPHICS`. `app-points.fth` adds `PLOT`, `LINE`, `1BIT`, `COLOR8`, and `TRUECOLOR`. `vocsys.fth` creates `SYSVOC` and moves host names into `GRAPHICS` or `SYSVOC`. `xref.fth` adds `REF`, `XREF`, `USEDIN`, `CALLS`, and `ANYWORDS`. The three `ef-*.fth` files supply host words those blobs call. `L@` and `L!` are 32-bit little-endian. `-ROT` is in `ef-hooks.fth`. Word names such as `(TZ-EOL)`, `(TZ-THREAD)`, `(TZ-UNLINK)`, and `(TZ-LINK)` keep that spelling.

`Resources/Library/Debugger/` is loaded at boot, as above. The rest of `Resources/` is not.

## Layout

| Path | Role |
| --- | --- |
| `kernel/ef32.c` | The engine. Primitive ids, dictionary, files, locals. |
| `host/main.c` | Terminal. `BYE` quits. |
| `host/web_main.c` | Exports the engine to the page. |
| `forth/` | Cold sources embedded into the image. |
| `web/` | The editor page, service worker, icons, and the committed `editforth.js` / `editforth.wasm` build. |
| `Resources/` | Library, samples, and the debugger. |
| `test/smoke.fth` | Terminal smoke test. |
| `scripts/embed.py` | Packs `forth/` into `build/boot.inc`. |
| `scripts/prim_lines.py` | Stamps each primitive with its line in `kernel/ef32.c` for `VIEW`. |

`Resources/Library/Sources/` keeps the library copies of the cold `.fth` files. Boot does not read them. Where a file in `forth/` differs, that copy is the one the image runs. `forth.s`, the ARM boot-word includes, `kernel_api.h`, and `Library/TCOM` are not in this tree. This port does not assemble ARM64, and it does not run the old TCOM sources. `Library/Reference`, `Library/BigInteger`, `Library/PI`, `Library/Emitter`, `Library/Assembler`, `Library/Hyper`, `AutoLoad`, and `Config` are not included.

`DEBUG name` steps that word in the console. F6, Space, `o`, or Return steps over. F7 or `i` steps into. F8 steps out. `g` runs to the end. Esc or `q` aborts. `h` prints the key list again. Boot includes the Forth debugger, so the pause UI, breakpoints, and token map are already there. After a step, the data stack starts at column 23 on that word's line, and the return stack starts 45 columns later. Each pause washes the current token in green and opens that file in view mode. A cold word such as `2OVER` opens the embedded `boot` image in a view-mode tab. That image is not listed in the file tree. The wash uses the debugger's file span. When there is no span, it finds the word near the definition. The wash clears when the session ends. The page fetches `Resources/Library/Debugger/` while booting. A file of the same path in the open folder is the one that loads. While a pause is waiting, a strip between the editor and the console shows the current word and Step Over, Step Into, Step Out, Continue, and Stop. Those buttons send the same keys as F6, F7, F8, `g`, and Esc. The strip hides when the session ends.

## Engine

The body of a word is at CFA+8. `IMM` is bit 31 of the flag cell at CFA−4. The layout words (`CELL+`, `>BODY`, `>LINK`, `>FLAGS`, `VIEW-LINE`, `VIEW-FILE#`) match this header.

`(APP-OPEN)` returns ior 0. `(APP-KEY)` returns 0 so `KEY` in `GRAPHICS` does not spin. `(APP-CBLIT)` and `(APP-PBLIT)` drop the pixel buffer. `ALSO GRAPHICS` or `ALSO SYSVOC` finds the names `vocsys.fth` moved. `(F-OP)` stays in `EMITTER`. `SOURCE` is the current input span. `REF IF` reports that the word is immediate. A name that is not defined throws −13. `KEY?` is false here, so the Space pause in `REF` does not wait. A float in `kernel2.fth` is still 8 bytes. `{: … :}` locals and `LOCALS|` are in this build. `2R@` copies the top two return-stack cells and leaves them there. `2!` stores the low cell at the address and the high cell in the next cell. `SAVE-INPUT` and `RESTORE-INPUT` save the place in the current source. On a file that is the line. On a block it includes the block number. Inside `EVALUATE` it is only `>IN`. `WORDS` prints `--- NAME (n) ---`, then the names in the first wordlist, in the current base. An optional filter is matched and not executed. `TEXTEDIT` and the boot-word table are stubs. Float words are in `FLOATING` and run on a separate stack of 16 IEEE doubles. `ENVIRONMENT?` reports `FLOATING` and `FLOAT-EXT` as true, `FLOATING-STACK` as 16, and `MAX-FLOAT` as the largest finite double. A decimal token that contains `e`, `E`, `d`, or `D` is a float. In hex, `0e` is still an integer.

`BRANCH` and `0BRANCH` store the destination minus the cell after the offset. `(EF-BRANCH)` and `(EF-0BRANCH)` use the other form: the instruction pointer becomes the offset cell plus the stored offset.

`/` `/MOD` `MOD` throw −9 when the divisor is zero. Division rounds toward zero. `2/` shifts toward negative infinity.

`\S` stops the rest of the current file. A missing `INCLUDE` file prints `can't open: ` and throws −38. A nested `FLOAD` of `Resources/Library/…` that is not already mounted is fetched from the page’s Resources tree. `INCLUDE` rejects a space, a quote, or a leading slash. File words such as `OPEN-FILE` accept a space, so a session file under `Application Support` can be named. A relative include may contain `..`. That name is folded against the directory of the file that is loading, and it cannot climb above that directory. Files larger than 256KB are skipped. `Testing/HayesTest/src/HAYES-RESULTS.txt` is one of those. `S"` drops the first blank after the quote and keeps any blanks after that.

`VIEW name` prints the stamped `path:line` and opens that line in view mode. Command-click does the same for the word under the pointer, in the editor or in the console. A name printed by `WORDS` is one of those words. Off a Mac, Ctrl-click is the same gesture. A C primitive such as `DUP` opens `Resources/Library/Sources/ef32.c` at its function. `make` copies `kernel/ef32.c` and `kernel/ef32.h` into that folder so the file tree and `VIEW` use the same files. A name with no file prints `(no source)` and leaves the editor alone. An unknown name prints `undefined: name`.

`FROMLIB FLOAD Testing/HayesTest/HayesTest.fth` runs the suite. After the core `ACCEPT` line, every section counter is 0 and the run prints `HAYES: ALL COUNTS ZERO — PASS`. `LOAD` reads a block as 16 lines of 64 characters, and one buffer is enough for that suite. The floating-point section runs in decimal. `FROMLIB FLOAD Testing/ANSValidate/ANS-VALIDATE.fth` finishes with no error.

## Not running yet

Canvas graphics and `EMIT-APP` are not wired up.
