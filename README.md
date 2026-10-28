# EditForthPWA

A 32-bit EditForth that runs in the browser. Cells are 4 bytes. The engine is C (`kernel/ef32.c`), compiled with `emcc` to WebAssembly. The page is the editor: a file tree, the source being edited, and a Forth console under it.

This folder is not an Xcode project. EditForth, EditForthWin, EditForthLin, and TZEditForth stay as they are. This tree does not write their document folders. Ask Forth is not part of this port.

## Run

The page needs a local server. `file://` will not load the module.

```
make serve
```

Open `http://127.0.0.1:8080/` in Chrome or Edge. The editor shows line numbers and one tab per open file. Startup leaves no file open. An error opens that file in view mode, selects the line, and marks its number. View mode is read-only, with a yellow bar and an Edit button. Typing in view mode asks whether to switch to Edit. Edit the current file, then press Include, or Cmd+Enter, or Ctrl+Enter. Include runs the text in the editor and does not write it. Save, or Cmd+S or Ctrl+S, writes the file. Closing a changed file asks Save, Don't Save, or Cancel. Switching tabs does not write the file. Restart boots a fresh image and clears the console. It does not include the open file. Shift with that shortcut, or the Restart button, does the fresh boot.

Open Folder picks a directory. The tree lists that folder, and the choice is remembered for this site. If a tab has unsaved changes, Open Folder asks Save, Don't Save, or Cancel first. The folder's first file opens. An empty folder opens `Untitled.fth`. Until a folder is open, files stay in the browser's private storage and the first visit creates `work.fth`. New opens an `Untitled.fth` tab. Save of that tab asks for a file name. Closing the last tab leaves the editor blank and not editable. The page does not delete files. `INCLUDE` reads the files in the tree. A name with no directory is looked up next to the file that is loading.

`CHDIR` prints `Current directory:`. `CHDIR documents` (also `desktop`, `downloads`, `music`, `pictures`, and `videos`) opens the folder picker when that name is not already a mounted directory. Cancel leaves the open folder as it is. `LS` and `DIR` list the current directory. `FROMLIB` makes the next `LS`, `DIR`, `CHDIR`, or `RESOLVE-KEY` start under `Resources/Library`.

When Chrome offers to install the page, Install adds a Dock window. Safari can run the editor on private storage, but it has no folder picker, so it cannot write edits back to a disk folder.

The terminal build is the same engine without the page:

```
make test
./build/editforth
./build/editforth some-file.fth
```

`make test` checks division by zero, an undefined word, and `test/smoke.fth`. `make wasm-test` checks the same engine under Node, including `INCLUDE` of mounted files. `make` from another directory has no rules. Run it in this folder. After a Forth source changes, if `make` keeps an old image, delete `build/boot.inc` and build again.

The page cache is named in `web/sw.js`. After a rebuild, reload the page so the new script and wasm are fetched.

## Editor

The window is white (`#ffffff`) with black text (`#111111`). Include is a black button. Errors are `#9b1c1c`.

The bar between the source and the console drags. Dragging up gives the console more room. The height is remembered (`editforth.consoleHeight`). The console stays under the source. There is no undock.

The console starts with `=== EditForthPWA 2.5.6 === Oct 26, 2026 3:26 PM ===`. `CLS` clears back to that line and the prompt. The Forth command line is the tail of the console text, on the line under the last output. The prompt is `ok(n)> `, with a trailing space. `n` is how many values are on the data stack. Return runs that tail. The output follows, and a new prompt is drawn in the same text. Up and Down recall earlier lines into that tail. An unknown name prints `undefined: NAME`, then `file:line`, then `throw -13`, in red, and the editor selects that line.

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

The last line of a normal boot is `Finished Loading: ef-hooks.fth`.

`kernel1.fth` and `kernel2.fth` are the 32-bit cold blobs. `vocemit.fth` builds `EMITTER`. `app-output.fth` builds `GRAPHICS`. `app-points.fth` adds `PLOT`, `LINE`, `1BIT`, `COLOR8`, and `TRUECOLOR`. `vocsys.fth` creates `SYSVOC` and moves host names into `GRAPHICS` or `SYSVOC`. `xref.fth` adds `REF`, `XREF`, `USEDIN`, `CALLS`, and `ANYWORDS`. The three `ef-*.fth` files supply host words those blobs call. `L@` and `L!` are 32-bit little-endian. `-ROT` is in `ef-hooks.fth`. Word names such as `(TZ-EOL)`, `(TZ-THREAD)`, `(TZ-UNLINK)`, and `(TZ-LINK)` keep that spelling.

Nothing under `Resources/` is loaded at boot.

## Layout

| Path | Role |
| --- | --- |
| `kernel/ef32.c` | The engine. Primitive ids, dictionary, files, locals. |
| `host/main.c` | Terminal. `BYE` quits. |
| `host/web_main.c` | Exports the engine to the page. |
| `forth/` | Cold sources embedded into the image. |
| `web/` | The editor page, service worker, and icons. |
| `Resources/` | Library, samples, debugger, and hyper text. |
| `test/smoke.fth` | Terminal smoke test. |
| `scripts/embed.py` | Packs `forth/` into `build/boot.inc`. |

`Resources/Library/Sources/` keeps the library copies of the cold `.fth` files. Boot does not read them. Where a file in `forth/` differs, that copy is the one the image runs. `forth.s`, the ARM boot-word includes, `kernel_api.h`, and `Library/TCOM` are not in this tree. This port does not assemble ARM64, and it does not run the old TCOM sources. `Library/Reference`, `Library/BigInteger`, `Library/PI`, and `Library/Emitter` are not included.

`DEBUG name` steps that word in the console. F6, Space, `o`, or Return steps over. F7 or `i` steps into. F8 steps out. `g` runs to the end. Esc or `q` aborts. `h` prints the key list again. That built-in pause is in the engine. `FROMLIB S" Debugger/debugger.fth" INCLUDED` loads the Forth pause UI from `Library/Debugger`. The page fetches those Debugger files from `web/Resources` (a link to `Resources/`) and mounts them, so the line works before Open Folder. A file of the same path in the open folder is the one that loads. `Library/Hyper` is present and is not loaded. The editor does not highlight the stepped word, and there is no debug status strip yet.

## Engine

The body of a word is at CFA+8. `IMM` is bit 31 of the flag cell at CFA−4. The layout words (`CELL+`, `>BODY`, `>LINK`, `>FLAGS`, `VIEW-LINE`, `VIEW-FILE#`) match this header.

`(APP-OPEN)` returns ior 0. `(APP-KEY)` returns 0 so `KEY` in `GRAPHICS` does not spin. `(APP-CBLIT)` and `(APP-PBLIT)` drop the pixel buffer. `ALSO GRAPHICS` or `ALSO SYSVOC` finds the names `vocsys.fth` moved. `(F-OP)` stays in `EMITTER`. `SOURCE` is the current input span. `REF IF` reports that the word is immediate. A name that is not defined throws −13. `KEY?` is false here, so the Space pause in `REF` does not wait. A float in `kernel2.fth` is still 8 bytes. `{: … :}` locals and `LOCALS|` are in this build. `2R@` copies the top two return-stack cells and leaves them there. `2!` stores the low cell at the address and the high cell in the next cell. `SAVE-INPUT` and `RESTORE-INPUT` save and restore `>IN` for the current source. `WORDS` prints `--- NAME (n) ---`, then the names in the first wordlist, in the current base. An optional filter is matched and not executed. `TEXTEDIT` and the boot-word table are stubs. Floats still throw −21.

`BRANCH` and `0BRANCH` store the destination minus the cell after the offset. `(EF-BRANCH)` and `(EF-0BRANCH)` use the other form: the instruction pointer becomes the offset cell plus the stored offset.

`/` `/MOD` `MOD` throw −9 when the divisor is zero. Division rounds toward zero. `2/` shifts toward negative infinity.

A missing `INCLUDE` file prints ` can't open: ` and throws −38. Paths with a space or a quote are rejected, so a few sample names in `Resources/` cannot be mounted. Files larger than 256KB are skipped. `Testing/HayesTest/src/HAYES-RESULTS.txt` is one of those.

## Not running yet

Hyper text, canvas graphics, `VIEW`, and `EMIT-APP` are not wired up. Ordinary words still absent include `WORDLIST`, `GET-CURRENT`, `SET-CURRENT`, `PREVIOUS`, `:NONAME`, `EVALUATE`, `>NUMBER`, `ACCEPT`, `S>D` and the rest of the doubles, `FM/MOD`, `SEARCH`, `ROLL`, `W/O`, `READ-LINE`, `WRITE-LINE`, `FILE-POSITION`, `DELETE-FILE`, `RENAME-FILE`, `MS`, and `TIME&DATE`.
