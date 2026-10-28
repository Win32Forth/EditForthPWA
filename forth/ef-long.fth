\ ef-long.fth — 32-bit little-endian fetch and store.
\ Loaded after app-output.fth and before app-points.fth.
.( Loading: ef-long.fth) CR

: L@ ( addr -- u )
  DUP C@
  SWAP DUP 1+ C@ 8 LSHIFT ROT OR
  SWAP DUP 2 + C@ 16 LSHIFT ROT OR
  SWAP 3 + C@ 24 LSHIFT OR ;

: L! ( u addr -- )
  OVER 255 AND OVER C!
  OVER 8 RSHIFT 255 AND OVER 1+ C!
  OVER 16 RSHIFT 255 AND OVER 2 + C!
  SWAP 24 RSHIFT 255 AND SWAP 3 + C! ;
