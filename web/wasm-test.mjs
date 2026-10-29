import { readFileSync } from "node:fs";
import createEditForth from "./editforth.js";

const mod = await createEditForth();
const paints = [];
mod._forthDebug = (path, line, name, off, len) => {
  paints.push({ path, line, name, off, len });
};
const boot = mod.cwrap("ef_web_boot", "number", []);
const fetchDone = mod.cwrap("ef_web_fetch_done", null, ["number", "string"]);
mod._forthFetch = (path) => {
  try {
    const text = readFileSync(new URL("../" + path, import.meta.url), "utf8");
    if (text.length > 256 * 1024 - 1) {
      fetchDone(0, "");
      return;
    }
    fetchDone(1, text);
  } catch (err) {
    fetchDone(0, "");
  }
};
const ev = mod.cwrap("ef_web_eval", "number", ["string"]);
const out = mod.cwrap("ef_web_output", "string", []);
const depth = mod.cwrap("ef_web_depth", "number", []);
const item = mod.cwrap("ef_web_item", "number", ["number"]);
const mountClear = mod.cwrap("ef_web_mount_clear", "number", []);
const mount = mod.cwrap("ef_web_mount", "number", ["string", "string"]);
const setSource = mod.cwrap("ef_web_set_source", "number", ["string"]);
const errLine = mod.cwrap("ef_web_error_line", "number", []);
const errName = mod.cwrap("ef_web_error_name", "string", []);
const bootSource = mod.cwrap("ef_web_boot_source", "string", []);

function die(msg) {
  console.error(msg);
  process.exit(1);
}

const bootCode = boot();
if (bootCode !== 0) {
  die("boot " + bootCode + " " + out());
}
const add = ev("2 3 +");
if (add !== 0 || depth() !== 1 || item(0) !== 5) {
  die("add rc=" + add + " depth=" + depth() + " item=" + item(0) + " out=" + out());
}
const two = ev("CREATE Z2 2 CELLS ALLOT 11 22 Z2 2! Z2 2@");
if (two !== 0 || depth() !== 3 || item(1) !== 11 || item(2) !== 22) {
  die("2! rc=" + two + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
ev("2DROP DROP");
const saved = ev(": GRAB SAVE-INPUT PARSE-NAME 2DROP RESTORE-INPUT THROW PARSE-NAME NIP ; GRAB HELLO");
if (saved !== 0 || depth() !== 1 || item(0) !== 5) {
  die("save-input rc=" + saved + " depth=" + depth() + " item=" + item(0) + " out=" + JSON.stringify(out()));
}
ev("DROP");
const hi = ev(': HI ."hello" ; HI');
const text = out();
if (hi !== 0 || !text.includes("hello")) {
  die("dotq rc=" + hi + " out=" + JSON.stringify(text));
}
const div = ev("1 0 /");
if (div !== -9 || !out().includes("division by zero")) {
  die("div0 rc=" + div + " out=" + out());
}
if (mountClear() !== 0 || mount("lib/a.fth", "INCLUDE b.fth\n") !== 0 || mount("lib/b.fth", "41 1 +\n") !== 0) {
  die("mount failed");
}
if (setSource("lib/a.fth") !== 0) {
  die("set source failed");
}
const inc = ev("INCLUDE b.fth");
if (inc !== 0 || item(depth() - 1) !== 42) {
  die("include rc=" + inc + " depth=" + depth() + " top=" + item(depth() - 1) + " out=" + out());
}
const fm = ev("10 0 3 FM/MOD");
if (fm !== 0 || item(depth() - 1) !== 3 || item(depth() - 2) !== 1) {
  die("fm/mod rc=" + fm + " depth=" + depth() + " out=" + out());
}
ev("2DROP");
const floor = ev("-10 -1 3 FM/MOD");
if (floor !== 0 || item(depth() - 1) !== -4 || item(depth() - 2) !== 2) {
  die("fm/mod floor rc=" + floor + " rem=" + item(depth() - 2) + " quot=" + item(depth() - 1));
}
ev("2DROP");
const sd = ev("123 S>D");
if (sd !== 0 || item(depth() - 1) !== 0 || item(depth() - 2) !== 123) {
  die("s>d rc=" + sd + " hi=" + item(depth() - 1) + " lo=" + item(depth() - 2));
}
ev("2DROP");
const noname = ev(":NONAME 6 7 + ; EXECUTE");
if (noname !== 0 || item(depth() - 1) !== 13) {
  die("noname rc=" + noname + " top=" + item(depth() - 1) + " out=" + out());
}
ev("DROP");
const env = ev('S" FLOORED" ENVIRONMENT?');
if (env !== 0 || item(depth() - 1) !== -1 || item(depth() - 2) !== 0) {
  die("env rc=" + env + " flag=" + item(depth() - 1) + " val=" + item(depth() - 2));
}
ev("2DROP");
const made = ev('S" wa.txt" W/O CREATE-FILE');
if (made !== 0 || item(depth() - 1) !== 0) {
  die("create rc=" + made + " ior=" + item(depth() - 1) + " out=" + out());
}
const fid = item(depth() - 2);
ev("2DROP");
const wrote = ev("PAD 0 " + fid + " WRITE-LINE");
if (wrote !== 0 || item(depth() - 1) !== 0) {
  die("write-line rc=" + wrote + " ior=" + item(depth() - 1));
}
ev("DROP");
ev(fid + " CLOSE-FILE DROP");
const st = ev('S" wa.txt" FILE-STATUS');
if (st !== 0 || item(depth() - 1) !== 0) {
  die("status rc=" + st + " ior=" + item(depth() - 1));
}
ev("2DROP");
ev('S" wa.txt" DELETE-FILE DROP');
const quit = ev("1 2 QUIT 99");
if (quit !== 0 || depth() < 2 || item(depth() - 1) !== 2) {
  die("quit rc=" + quit + " depth=" + depth() + " top=" + item(depth() - 1));
}
ev("2DROP");
if (mount("echo.fth", "1 2 +\n\n3\n") !== 0) {
  die("echo mount failed");
}
const quiet = ev("FILE-ECHO ON\n1 2 .\n");
if (quiet !== 0 || out().includes("|") || !out().includes("2 ")) {
  die("file-echo console out=" + JSON.stringify(out()));
}
const echoed = ev("INCLUDE echo.fth\nFILE-ECHO OFF\n");
const echoText = out();
if (echoed !== 0 || !echoText.includes("    1| 1 2 +") || !echoText.includes("    2| ") ||
    !echoText.includes("    3| 3")) {
  die("file-echo include rc=" + echoed + " out=" + JSON.stringify(echoText));
}
const bad = ev("1 1 +\nnope\n");
if (bad !== -13 || errLine() !== 2 || errName() !== "lib/a.fth") {
  die("loc rc=" + bad + " line=" + errLine() + " name=" + errName() + " out=" + out());
}
const dirDepth = depth();
if (ev("' CLS DROP") !== 0 || ev("' CHDIR DROP") !== 0 || ev("' LS DROP") !== 0 || ev("' DIR DROP") !== 0 || depth() !== dirDepth) {
  die("dir words rc depth=" + depth() + " out=" + JSON.stringify(out()));
}
const cls = ev("CLS");
if (cls !== 0 || !out().includes("\f") || depth() !== dirDepth) {
  die("cls rc=" + cls + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const lsRoot = ev("LS");
const lsRootText = out();
if (lsRoot !== 0 || !lsRootText.includes("Directory of .") || !lsRootText.includes(" lib") || !lsRootText.includes("<DIR>") || depth() !== dirDepth) {
  die("ls root " + JSON.stringify(lsRootText));
}
const chLib = ev("CHDIR lib");
if (chLib !== 0 || !out().includes("Current directory: lib") || depth() !== dirDepth) {
  die("chdir lib " + JSON.stringify(out()));
}
const lsLib = ev("DIR");
const lsLibText = out();
if (lsLib !== 0 || !lsLibText.includes("a.fth") || !lsLibText.includes("b.fth") || depth() !== dirDepth) {
  die("ls lib " + JSON.stringify(lsLibText));
}
const chUp = ev("CHDIR ..");
if (chUp !== 0 || !out().includes("Current directory: .") || depth() !== dirDepth) {
  die("chdir up " + JSON.stringify(out()));
}
const chAbs = ev("CHDIR /tmp");
if (chAbs !== 0 || !out().includes("CHDIR error") || depth() !== dirDepth) {
  die("chdir abs " + JSON.stringify(out()));
}
const chStay = ev("CHDIR");
if (chStay !== 0 || !out().includes("Current directory: .") || depth() !== dirDepth) {
  die("chdir stay " + JSON.stringify(out()));
}
const chLine = ev("CHDIR\n99");
if (chLine !== 0 || !out().includes("Current directory: .") || depth() !== dirDepth + 1 || item(dirDepth) !== 99) {
  die("chdir newline rc=" + chLine + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
ev("DROP");
if (mount("Classes/note.fth", "77\n") !== 0) {
  die("mount note");
}
const fromShort = ev('FROMLIB S" Classes/note.fth" INCLUDED');
if (fromShort !== 0 || depth() !== dirDepth + 1 || item(dirDepth) !== 77) {
  die("fromlib short rc=" + fromShort + " depth=" + depth() + " top=" + item(depth() - 1) + " out=" + JSON.stringify(out()));
}
ev("DROP");
const views = [];
mod._forthView = (path, line) => {
  views.push({ path, line });
};
const viewRc = ev("VIEW CELL+");
if (viewRc !== 0 || !out().includes("boot:") || views.length !== 1 || views[0].path !== "boot" || views[0].line < 1 || depth() !== dirDepth) {
  die("view cell+ rc=" + viewRc + " depth=" + depth() + " views=" + JSON.stringify(views) + " out=" + JSON.stringify(out()));
}
const viewMiss = ev("VIEW NO-SUCH-VIEWWORD");
if (viewMiss !== 0 || !out().includes("undefined: NO-SUCH-VIEWWORD") || views.length !== 1 || depth() !== dirDepth) {
  die("view miss rc=" + viewMiss + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const viewDup = ev("VIEW DUP");
const dupLine = views.length === 2 ? views[1].line : 0;
const csrc = readFileSync(new URL("../Resources/System/Sources/ef32.c", import.meta.url), "utf8").split("\n");
const dupText = dupLine > 0 && dupLine <= csrc.length ? csrc[dupLine - 1] : "";
if (viewDup !== 0 || !out().includes("Resources/System/Sources/ef32.c:") || views.length !== 2 || views[1].path !== "Resources/System/Sources/ef32.c" || !dupText.includes("static int p_dup") || depth() !== dirDepth) {
  die("view dup rc=" + viewDup + " depth=" + depth() + " views=" + JSON.stringify(views) + " line=" + JSON.stringify(dupText) + " out=" + JSON.stringify(out()));
}
const sq = ev('S" AB" NIP');
if (sq !== 0 || depth() !== dirDepth + 1 || item(dirDepth) !== 2) {
  die("s-quote one blank rc=" + sq + " depth=" + depth() + " top=" + item(depth() - 1));
}
ev("DROP");
const sq2 = ev('S"  AB" DROP C@');
if (sq2 !== 0 || depth() !== dirDepth + 1 || item(dirDepth) !== 32) {
  die("s-quote two blanks rc=" + sq2 + " depth=" + depth() + " top=" + item(depth() - 1));
}
ev("DROP");
mod._forthFetch = (path) => {
  if (path === "Resources/Library/Classes/probe.fth") {
    fetchDone(1, "88\n");
    return;
  }
  fetchDone(0, "");
};
const fetched = ev('FROMLIB S" Classes/probe.fth" INCLUDED');
if (fetched !== 0 || depth() !== dirDepth + 1 || item(dirDepth) !== 88) {
  die("fetch probe rc=" + fetched + " depth=" + depth() + " top=" + item(depth() - 1) + " out=" + JSON.stringify(out()));
}
ev("DROP");
mod._forthFetch = (path) => fetchDone(0, "");
const evAsync = mod.cwrap("ef_web_eval", "number", ["string"], { async: true });
const postKey = mod.cwrap("ef_web_post_key", null, ["number"]);
const keyWaiting = mod.cwrap("ef_web_key_waiting", "number", []);

function waitFor(pred) {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    const tick = () => {
      try {
        if (pred()) {
          resolve();
          return;
        }
      } catch (err) {
        reject(err);
        return;
      }
      if (Date.now() - start > 4000) {
        reject(new Error("timed out waiting for a debug key"));
        return;
      }
      setTimeout(tick, 5);
    };
    tick();
  });
}

const before = depth();
const stepping = evAsync(": T 10 32 + . ; DEBUG T");
for (let i = 0; i < 5; i++) {
  await waitFor(() => keyWaiting());
  if (i === 0 && !out().includes(">> LIT")) {
    die("no pause text " + JSON.stringify(out()));
  }
  if (i === 1) {
    const lit = out().split("\n").find((line) => line.includes(">> LIT"));
    const sAt = lit ? lit.indexOf("S(") : -1;
    const rAt = lit ? lit.indexOf("R(") : -1;
    if (sAt !== 23 || rAt !== sAt + 45) {
      die("stack cols s=" + sAt + " r=" + rAt + " line=" + JSON.stringify(lit));
    }
  }
  postKey("o".charCodeAt(0));
  await new Promise((r) => setTimeout(r, 40));
}
const stepRc = await stepping;
if (stepRc !== 0 || !out().includes("42") || depth() !== before) {
  die("step rc=" + stepRc + " depth=" + depth() + " before=" + before + " out=" + JSON.stringify(out()));
}
const aborting = evAsync(": T2 99 . ; DEBUG T2");
await waitFor(() => keyWaiting());
postKey("q".charCodeAt(0));
const abortRc = await aborting;
if (abortRc !== 0 || !out().includes("DEBUG aborted") || out().includes(">> .")) {
  die("abort rc=" + abortRc + " out=" + JSON.stringify(out()));
}
const intoing = evAsync(": NEST 7 . ; : U NEST ; DEBUG U");
await waitFor(() => keyWaiting());
if (!out().includes("I>> NEST")) {
  die("no into " + JSON.stringify(out()));
}
postKey("i".charCodeAt(0));
await new Promise((r) => setTimeout(r, 40));
for (let i = 0; i < 4; i++) {
  await waitFor(() => keyWaiting());
  postKey("o".charCodeAt(0));
  await new Promise((r) => setTimeout(r, 40));
}
const intoRc = await intoing;
if (intoRc !== 0 || !out().includes(">> .") || depth() !== before) {
  die("into rc=" + intoRc + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const keying = evAsync("KEY .");
await waitFor(() => keyWaiting());
postKey(65);
const keyRc = await keying;
if (keyRc !== 0 || !out().includes("65") || depth() !== before) {
  die("key rc=" + keyRc + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
for (const name of [
  "debugger.fth",
  "debug-bp.fth",
  "dbg-pause.fth",
  "dbg-ed.fth",
  "dbg-map.fth",
  "debug-runto.fth",
]) {
  const text = readFileSync(new URL("../Resources/System/Debugger/" + name, import.meta.url), "utf8");
  if (mount("Resources/System/Debugger/" + name, text) !== 0) {
    die("mount " + name);
  }
}
const fromLs = ev("FROMLIB LS Debugger");
const fromLsText = out();
if (fromLs !== 0 || !fromLsText.includes("debugger.fth") || depth() !== before) {
  die("fromlib ls " + JSON.stringify(fromLsText));
}
const fromCh = ev("FROMLIB CHDIR Debugger");
if (fromCh !== 0 || !out().includes("Current directory: Resources/System/Debugger") || depth() !== before) {
  die("fromlib chdir " + JSON.stringify(out()));
}
const star = ev("LS *.fth");
if (star !== 0 || !out().includes("debugger.fth") || depth() !== before) {
  die("ls star " + JSON.stringify(out()));
}
if (ev("CHDIR ..") !== 0 || ev("CHDIR ..") !== 0 || ev("CHDIR ..") !== 0 || !out().includes("Current directory: .") || depth() !== before) {
  die("chdir home " + JSON.stringify(out()));
}
const hub = ev('FROMLIB S" Debugger/debugger.fth" INCLUDED');
if (hub !== 0 || depth() !== before) {
  die("hub rc=" + hub + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const breaks = ev("' .BREAKS DROP");
if (breaks !== 0 || depth() !== before) {
  die("breaks rc=" + breaks + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const bootOpen = ev("' 2OVER VIEW-FILE# VIEW-PATH R/O BIN OPEN-FILE THROW DUP FILE-SIZE THROW DROP SWAP CLOSE-FILE THROW");
const bootSz = item(depth() - 1);
if (bootOpen !== 0 || bootSz < 80000 || depth() !== before + 1) {
  die("boot open rc=" + bootOpen + " size=" + bootSz + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
if (ev("DROP") !== 0 || depth() !== before) {
  die("boot drop depth=" + depth());
}
if (ev("ALSO DEBUGGER 1 DBG-SPAN-TRACE ! PREVIOUS") !== 0 || depth() !== before) {
  die("span trace " + JSON.stringify(out()));
}
const cold = bootSource();
const coldBytes = new TextEncoder().encode(cold);
if (!cold.includes(": 2OVER") || coldBytes.length < 80000) {
  die("boot source " + coldBytes.length);
}
const overBefore = depth();
const overAt = paints.length;
const overing = evAsync("1 2 3 4 DEBUG 2OVER");
for (let i = 0; i < 7; i++) {
  await waitFor(() => keyWaiting());
  const text = out();
  if (text.includes("bad address") || text.includes("can't open")) {
    die("2over pause " + JSON.stringify(text));
  }
  if (i === 0 && (!text.includes(">> >R") || !text.includes("body#=0") || !text.includes("map#=0") || !text.includes("len="))) {
    die("no 2over " + JSON.stringify(text));
  }
  if (i === 1 && (!text.includes("body#=1") || !text.includes("map#=1"))) {
    die("cell map " + JSON.stringify(text));
  }
  postKey("o".charCodeAt(0));
  await new Promise((r) => setTimeout(r, 40));
}
const overRc = await overing;
const top = [];
for (let i = depth() - 6; i < depth(); i++) {
  top.push(item(i));
}
if (overRc !== 0 || depth() !== overBefore + 6 || top.join(",") !== "1,2,3,4,1,2") {
  die("2over rc=" + overRc + " depth=" + depth() + " top=" + top.join(",") + " out=" + JSON.stringify(out()));
}
const overText = paints.slice(overAt)
  .filter((p) => p.path === "boot" && p.line > 0 && p.len > 0 && p.off >= 0 && p.off + p.len <= coldBytes.length)
  .map((p) => new TextDecoder().decode(coldBytes.subarray(p.off, p.off + p.len)));
if (!overText.includes(">R")) {
  die("boot highlight " + JSON.stringify(paints.slice(overAt)) + " text " + JSON.stringify(overText));
}
if (mount("walk.fth", ": WIF 1 IF 2 ELSE 3 THEN ;\n: WSTR S\" hi\" 2DROP ;\n: WEX IF EXIT THEN 1 ;\n") !== 0) {
  die("mount walk");
}
const walkRc = ev("INCLUDE walk.fth");
if (walkRc !== 0 || depth() !== overBefore + 6) {
  die("walk include rc=" + walkRc + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
async function steps(n, label) {
  for (let i = 0; i < n; i++) {
    await waitFor(() => keyWaiting());
    const text = out();
    if (text.includes("bad address") || text.includes("can't open")) {
      die(label + " pause " + JSON.stringify(text));
    }
    postKey("o".charCodeAt(0));
    await new Promise((r) => setTimeout(r, 40));
  }
}
const wifBefore = depth();
const paintAt = paints.length;
const wifing = evAsync("DEBUG WIF");
await steps(5, "wif");
const wifRc = await wifing;
if (wifRc !== 0 || depth() !== wifBefore + 1 || item(depth() - 1) !== 2) {
  die("wif rc=" + wifRc + " depth=" + depth() + " top=" + item(depth() - 1) + " out=" + JSON.stringify(out()));
}
const walkSrc = ": WIF 1 IF 2 ELSE 3 THEN ;\n: WSTR S\" hi\" 2DROP ;\n: WEX IF EXIT THEN 1 ;\n";
const walkBytes = new TextEncoder().encode(walkSrc);
const walkPaints = paints.slice(paintAt).filter((p) => p.path === "walk.fth");
const walkHits = walkPaints.filter((p) => p.len > 0 && p.line > 0);
const walkText = walkHits.map((p) => new TextDecoder().decode(walkBytes.subarray(p.off, p.off + p.len)));
if (!walkText.includes("1") || !walkPaints.some((p) => p.line > 0 && p.name)) {
  die("highlight " + JSON.stringify(walkPaints) + " text " + JSON.stringify(walkText));
}
if (!paints.slice(paintAt).some((p) => p.path === "")) {
  die("highlight stayed up " + JSON.stringify(paints.slice(paintAt)));
}
const wstrBefore = depth();
const wstring = evAsync("DEBUG WSTR");
await steps(3, "wstr");
const wstrRc = await wstring;
if (wstrRc !== 0 || depth() !== wstrBefore) {
  die("wstr rc=" + wstrRc + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const wexBefore = depth();
const wexing = evAsync("0 DEBUG WEX");
await steps(3, "wex");
const wexRc = await wexing;
if (wexRc !== 0 || depth() !== wexBefore + 1 || item(depth() - 1) !== 1) {
  die("wex rc=" + wexRc + " depth=" + depth() + " top=" + item(depth() - 1) + " out=" + JSON.stringify(out()));
}
const dotSrc = ': HDOT ." hi" ;\n';
if (mount("dot.fth", dotSrc) !== 0) {
  die("mount dot");
}
const dotInc = ev("INCLUDE dot.fth");
if (dotInc !== 0 || depth() !== wexBefore + 1) {
  die("dot include rc=" + dotInc + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
const dotAt = paints.length;
const doting = evAsync("DEBUG HDOT");
await steps(2, "hdot");
const dotRc = await doting;
const dotBytes = new TextEncoder().encode(dotSrc);
const dotText = paints.slice(dotAt)
  .filter((p) => p.path === "dot.fth" && p.len > 0)
  .map((p) => new TextDecoder().decode(dotBytes.subarray(p.off, p.off + p.len)));
if (dotRc !== 0 || !out().includes("hi") || !dotText.includes('."')) {
  die("hdot rc=" + dotRc + " text=" + JSON.stringify(dotText) + " out=" + JSON.stringify(out()));
}
const breakToggle = mod.cwrap("ef_web_break_toggle", "number", ["string"]);
const breakClear = mod.cwrap("ef_web_break_clear", "number", ["string"]);
const breakEnable = mod.cwrap("ef_web_break_enable", "number", ["string", "number"]);
const breakList = mod.cwrap("ef_web_breaks", "string", []);
const breakAsap = mod.cwrap("ef_web_break_asap", "number", []);
const setSourceLine = mod.cwrap("ef_web_set_source_line", "number", ["number"]);
const running = mod.cwrap("ef_web_running", "number", []);
const setYield = mod.cwrap("ef_web_set_yield", null, ["number"]);
setYield(1);
if (breakAsap() !== 1) {
  die("pause idle " + breakAsap());
}
if (breakToggle("DUP") !== 1 || !breakList().startsWith("DUP\t1\n")) {
  die("toggle add " + JSON.stringify(breakList()));
}
if (breakEnable("DUP", 0) !== 1 || breakList() !== "DUP\t0\n") {
  die("toggle off " + JSON.stringify(breakList()));
}
if (breakEnable("DUP", 1) !== 1 || breakToggle("NOSUCHWORD") !== 0) {
  die("toggle miss " + JSON.stringify(breakList()));
}
const bpAt = paints.length;
const bping = evAsync(": TBP 3 DUP DROP ; BPGO TBP");
await waitFor(() => keyWaiting());
if (!out().includes("DUP") || paints.slice(bpAt).every((p) => p.name !== "DUP")) {
  die("bpgo " + JSON.stringify(out()) + " " + JSON.stringify(paints.slice(bpAt)));
}
postKey(27);
const bpRc = await bping;
if (bpRc === 0 || breakToggle("DUP") !== 2 || breakList() !== "") {
  die("bpgo end rc=" + bpRc + " list=" + JSON.stringify(breakList()) + " out=" + JSON.stringify(out()));
}
if (breakToggle("SWAP") !== 1 || breakClear("SWAP") !== 1 || breakList() !== "") {
  die("clear " + JSON.stringify(breakList()));
}
const spinAt = paints.length;
const spinning = evAsync(": SPIN 0 BEGIN 1+ DUP 40000 > UNTIL DROP ; SPIN");
await waitFor(() => running());
if (breakAsap() !== 0) {
  die("pause set while spinning");
}
await waitFor(() => keyWaiting());
const spinPaint = paints.slice(spinAt);
if (!spinPaint.some((p) => p.name)) {
  die("pause now " + JSON.stringify(out()) + " " + JSON.stringify(spinPaint));
}
postKey(27);
const spinRc = await spinning;
if (running() !== 0 || keyWaiting() !== 0) {
  die("pause leftover rc=" + spinRc + " out=" + JSON.stringify(out()));
}
const pauseDepth = depth();
if (ev("1 2 +") !== 0 || depth() !== pauseDepth + 1 || item(depth() - 1) !== 3) {
  die("after pause rc depth=" + depth() + " pauseDepth=" + pauseDepth + " out=" + JSON.stringify(out()));
}
ev("DROP");
const conSrc = ": CSPIN 0 begin 1+ dup 40000 > until drop ;\n";
if (mount("console.fth", conSrc) !== 0 || setSource("console.fth") !== 0 || setSourceLine(1) !== 0) {
  die("console source");
}
if (ev(conSrc) !== 0) {
  die("console def " + JSON.stringify(out()));
}
const conAt = paints.length;
const conning = evAsync("CSPIN");
await waitFor(() => running());
if (breakAsap() !== 0) {
  die("console pause set");
}
await waitFor(() => keyWaiting());
const conBytes = new TextEncoder().encode(conSrc);
const conText = paints.slice(conAt)
  .filter((p) => p.path === "console.fth" && p.len > 0 && p.off >= 0 && p.off + p.len <= conBytes.length)
  .map((p) => new TextDecoder().decode(conBytes.subarray(p.off, p.off + p.len)));
if (!conText.some((word) => conSrc.includes(word))) {
  die("console highlight " + JSON.stringify(paints.slice(conAt)) + " text " + JSON.stringify(conText));
}
postKey(27);
await conning;
setSource("");
let pumps = 0;
mod._forthPump = () => {
  pumps += 1;
};
const paused = await evAsync('." pausemark" cr PAUSE');
if (paused !== 0 || pumps < 1 || !out().includes("pausemark")) {
  die("pause rc=" + paused + " pumps=" + pumps + " out=" + JSON.stringify(out()));
}
const keyDepth = depth();
if (await evAsync("KEY?") !== 0 || depth() !== keyDepth + 1 || item(keyDepth) !== 0) {
  die("key? empty depth=" + depth() + " top=" + item(depth() - 1));
}
ev("DROP");
postKey(65);
postKey(66);
if (await evAsync("KEY?") !== 0 || depth() !== keyDepth + 1 || item(keyDepth) !== -1) {
  die("key? buffered depth=" + depth() + " top=" + item(depth() - 1));
}
ev("DROP");
if (await evAsync("KEY?") !== 0 || item(depth() - 1) !== -1) {
  die("key? kept the key");
}
ev("DROP");
if (ev("KEY") !== 0 || depth() !== keyDepth + 1 || item(keyDepth) !== 65) {
  die("key first top=" + item(depth() - 1));
}
ev("DROP");
if (ev("KEY") !== 0 || item(depth() - 1) !== 66) {
  die("key second top=" + item(depth() - 1));
}
ev("DROP");
if (await evAsync("KEY?") !== 0 || item(depth() - 1) !== 0) {
  die("key? drained top=" + item(depth() - 1));
}
ev("DROP");
const acceptRc = ev(': ATEST CR ." PLEASE TYPE UP TO 80 CHARACTERS:" CR PAD 40 ACCEPT PAD SWAP TYPE ; ATEST');
if (acceptRc !== 0 || !out().includes("hayes-accept") || depth() !== keyDepth) {
  die("hayes accept rc=" + acceptRc + " depth=" + depth() + " out=" + JSON.stringify(out()));
}
console.log("wasm-test ok");
