\ facility.fth — ANS Facility + Facility Ext spot-checks
\
\ Requires: tester.fth already loaded
\ Structures (BEGIN-STRUCTURE / +FIELD / FIELD: / CFIELD:) — Hayes facilitytest subset.
\ Facility Ext: K-* constants, EKEY>FKEY / EKEY>CHAR (synthetic events).
\ Light MS / TIME&DATE smoke. Skip PAGE/AT-XY buffer capture (host UI).
\
\ NOT a formal ANS certificate. Prefer Hayes facilitytest.fth.

DECIMAL
ONLY FORTH DEFINITIONS

CR .( === Facility ===) CR

\ --- empty structure ---
BEGIN-STRUCTURE FA-S1
END-STRUCTURE
FA-S1 0 = S" empty-struct" EXPECT

\ --- +FIELD: 1 char, then an aligned cell ---
\ CELL is 4. +FIELD does not align. ALIGNED lifts offset 1 to 4, then + 4 = 8.
\ The cell store needs that aligned address.
BEGIN-STRUCTURE FA-S2
  1 CHARS +FIELD FA-A
  ALIGNED 1 CELLS +FIELD FA-B
END-STRUCTURE
FA-S2 8 = S" struct-size" EXPECT

CREATE FA-INST FA-S2 ALLOT
77 FA-INST FA-A C!
FA-INST FA-A C@ 77 = S" field-C!" EXPECT

99 FA-INST FA-B !
FA-INST FA-B @ 99 = S" field-!" EXPECT

\ --- FIELD: (cell fields) ---
BEGIN-STRUCTURE FA-S3
  FIELD: FA-X
  FIELD: FA-Y
END-STRUCTURE
0 FA-Y 4 = S" FIELD:-off" EXPECT
FA-S3 8 = S" FIELD:-size" EXPECT

\ --- CFIELD: ---
BEGIN-STRUCTURE FA-S5
  CFIELD: FA-C1
  CFIELD: FA-C2
END-STRUCTURE
0 FA-C2 1 = S" CFIELD:-off" EXPECT
FA-S5 2 = S" CFIELD:-size" EXPECT

\ --- nested structures ---
\ FA-S2 is 8 and already aligned; FA-S3 is 8 → total 16
BEGIN-STRUCTURE FA-S4
  FA-S2 +FIELD FA-N
  ALIGNED
  FA-S3 +FIELD FA-M
END-STRUCTURE
FA-S4 16 = S" nested-size" EXPECT

\ --- ENVIRONMENT? FACILITY / FACILITY-EXT ---
S" FACILITY" ENVIRONMENT? NIP S" ENV-FACILITY" EXPECT
S" FACILITY-EXT" ENVIRONMENT? NIP S" ENV-FACILITY-EXT" EXPECT

\ --- MS (0 is fine) ---
0 MS
TRUE S" MS0" EXPECT

\ --- TIME&DATE leaves 6 items ---
: (FA-CLEAR)  BEGIN DEPTH WHILE DROP REPEAT ;
(FA-CLEAR)
TIME&DATE
DEPTH 6 = S" TIME&DATE-n" EXPECT
(FA-CLEAR)

\ --- Facility Ext: K-* + EKEY>FKEY / EKEY>CHAR (synthetic events) ---
\ Encoding: (2<<24)|id = function key; plain 0..255 = character
K-LEFT 1 = S" K-LEFT" EXPECT
K-F12 22 = S" K-F12" EXPECT
K-SHIFT-MASK $2000 = S" K-SHIFT" EXPECT

\ build fkey event without interpret IF: 2 24 LSHIFT K-LEFT OR
2 24 LSHIFT K-LEFT OR
EKEY>FKEY
SWAP K-LEFT = AND S" EKEY>FKEY" EXPECT

\ non-fkey leaves false
65 EKEY>FKEY 0= S" EKEY>FKEY-miss" EXPECT

65 EKEY>CHAR SWAP 65 = AND S" EKEY>CHAR" EXPECT

\ char event tag 1<<24 | 'A'
1 24 LSHIFT 65 OR
EKEY>CHAR
SWAP 65 = AND S" EKEY>CHAR-tag" EXPECT

.( --- Facility batch done ---) .STACK-DEPTH CR
