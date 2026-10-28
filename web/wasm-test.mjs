import { readFileSync } from "node:fs";
import createEditForth from "./editforth.js";

const mod = await createEditForth();
const boot = mod.cwrap("ef_web_boot", "number", []);
const ev = mod.cwrap("ef_web_eval", "number", ["string"]);
const out = mod.cwrap("ef_web_output", "string", []);
const depth = mod.cwrap("ef_web_depth", "number", []);
const item = mod.cwrap("ef_web_item", "number", ["number"]);
const mountClear = mod.cwrap("ef_web_mount_clear", "number", []);
const mount = mod.cwrap("ef_web_mount", "number", ["string", "string"]);
const setSource = mod.cwrap("ef_web_set_source", "number", ["string"]);
const errLine = mod.cwrap("ef_web_error_line", "number", []);
const errName = mod.cwrap("ef_web_error_name", "string", []);

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
  const text = readFileSync(new URL("../Resources/Library/Debugger/" + name, import.meta.url), "utf8");
  if (mount("Resources/Library/Debugger/" + name, text) !== 0) {
    die("mount " + name);
  }
}
const fromLs = ev("FROMLIB LS");
const fromLsText = out();
if (fromLs !== 0 || !fromLsText.includes("Debugger") || !fromLsText.includes("<DIR>") || depth() !== before) {
  die("fromlib ls " + JSON.stringify(fromLsText));
}
const fromCh = ev("FROMLIB CHDIR Debugger");
if (fromCh !== 0 || !out().includes("Current directory: Resources/Library/Debugger") || depth() !== before) {
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
const overBefore = depth();
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
const wifing = evAsync("DEBUG WIF");
await steps(5, "wif");
const wifRc = await wifing;
if (wifRc !== 0 || depth() !== wifBefore + 1 || item(depth() - 1) !== 2) {
  die("wif rc=" + wifRc + " depth=" + depth() + " top=" + item(depth() - 1) + " out=" + JSON.stringify(out()));
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
console.log("wasm-test ok");
