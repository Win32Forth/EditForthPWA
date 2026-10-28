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
console.log("wasm-test ok");
