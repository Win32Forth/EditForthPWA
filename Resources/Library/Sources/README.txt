Library/Sources
===============

These .fth files are the library copies of the cold Forth sources. The
image that boots embeds the copies in forth/ at the top of this project,
not the files in this folder. A few of the forth/ copies differ where the
32-bit engine needed a change. Edit those when the running image should
change.

forth.s, the boot-word includes, and kernel_api.h are not here. This port
does not assemble the ARM64 kernel.

ef32.c and ef32.h are copies of kernel/ef32.c and kernel/ef32.h. `make`
refreshes them. `VIEW` of a primitive, such as `DUP`, opens ef32.c at the
function. Edit the kernel copies when the engine should change.
