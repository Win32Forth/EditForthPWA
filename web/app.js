import createEditForth from "./editforth.js";

const log = document.querySelector("#log");
const installBtn = document.querySelector("#install");
const editor = document.querySelector("#editor");
const listing = document.querySelector("#listing");
const where = document.querySelector("#where");
const nameForm = document.querySelector("#name-form");
const nameInput = document.querySelector("#name-input");

const SEED =
  "\\ Cmd+Enter or Ctrl+Enter loads this file.\n" +
  "\n" +
  ': HELLO ( -- ) ." Hello from the editor." CR ;\n' +
  "\n" +
  "HELLO\n";

const files = new Map();
const history = [];
let historyAt = 0;
const chunks = [];
let programmatic = false;
let currentPath = "";
let saveTimer = 0;
let ready = false;
let busy = false;
let opfs = null;
let diskFolder = false;
let saveError = "";
let skippedForth = 0;

const SKIP_EXT = new Set([
  "png", "jpg", "jpeg", "gif", "webp", "ico", "icns", "wasm",
  "o", "a", "obj", "exe", "dll", "dylib", "so", "zip", "gz",
  "pdf", "ttf", "woff", "woff2", "mp3", "mp4", "wav", "dmg",
]);

function protectedString() {
  let text = "";
  for (const chunk of chunks) {
    text += chunk.text;
  }
  return text;
}

function readLog() {
  let text = "";
  for (const node of log.childNodes) {
    if (node.nodeName === "BR") {
      text += "\n";
    } else if (node.nodeName === "DIV" || node.nodeName === "P") {
      if (text && !text.endsWith("\n")) {
        text += "\n";
      }
      text += node.textContent || "";
    } else {
      text += node.textContent || "";
    }
  }
  return text;
}

function currentTail() {
  const text = readLog();
  const prot = protectedString();
  return text.startsWith(prot) ? text.slice(prot.length) : "";
}

function placeCaretEnd() {
  const node = log.lastChild;
  if (!node) {
    return;
  }
  const range = document.createRange();
  if (node.nodeType === Node.TEXT_NODE) {
    range.setStart(node, node.textContent.length);
  } else {
    range.selectNodeContents(node);
    range.collapse(false);
  }
  range.collapse(true);
  const sel = window.getSelection();
  sel.removeAllRanges();
  sel.addRange(range);
}

function render(tail, moveCaret) {
  programmatic = true;
  const frag = document.createDocumentFragment();
  for (const chunk of chunks) {
    if (!chunk.text) {
      continue;
    }
    const span = document.createElement("span");
    if (chunk.cls) {
      span.className = chunk.cls;
    }
    span.textContent = chunk.text;
    frag.appendChild(span);
  }
  frag.appendChild(document.createTextNode(tail || ""));
  log.replaceChildren(frag);
  programmatic = false;
  if (moveCaret) {
    placeCaretEnd();
  }
  log.scrollTop = log.scrollHeight;
}

function write(text, cls) {
  if (!text) {
    return;
  }
  const tail = currentTail();
  const clsName = cls || "";
  const last = chunks[chunks.length - 1];
  const promptAtEnd = last && last.cls === "" && /^ok\(\d+\)> $/.test(last.text);
  if (promptAtEnd) {
    const before = chunks[chunks.length - 2];
    if (before && before.cls === clsName) {
      before.text += text;
    } else {
      chunks.splice(chunks.length - 1, 0, { text, cls: clsName });
    }
  } else if (last && last.cls === clsName && tail === "") {
    last.text += text;
  } else {
    chunks.push({ text, cls: clsName });
  }
  render(tail, false);
}

function setPrompt(focus) {
  const tail = currentTail();
  const prompt = "ok(" + api.depth() + ")> ";
  const last = chunks[chunks.length - 1];
  if (last && last.cls === "" && /^ok\(\d+\)> $/.test(last.text)) {
    last.text = prompt;
  } else {
    const joined = protectedString();
    if (joined.length && !joined.endsWith("\n")) {
      chunks.push({ text: "\n", cls: "" });
    }
    chunks.push({ text: prompt, cls: "" });
  }
  render(tail, Boolean(focus));
  if (focus) {
    log.focus();
  }
}

function boundaryOffset(container, offset) {
  const range = document.createRange();
  range.selectNodeContents(log);
  range.setEnd(container, offset);
  return range.toString().length;
}

function validPath(path) {
  if (!path || path.startsWith("/") || path.length > 500) {
    return false;
  }
  const parts = path.split("/");
  return parts.every((part) => part && part !== "." && part !== ".." && /^[^\s"'\\]+$/.test(part));
}

function parentDir(path) {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? "" : path.slice(0, slash);
}

function skipFile(name, size) {
  if (size > 256 * 1024 - 1) {
    return true;
  }
  const dot = name.lastIndexOf(".");
  if (dot <= 0) {
    return false;
  }
  return SKIP_EXT.has(name.slice(dot + 1).toLowerCase());
}

function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open("editforth-pwa", 1);
    req.onupgradeneeded = () => {
      req.result.createObjectStore("handles");
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function saveHandle(handle) {
  const db = await openDb();
  try {
    await new Promise((resolve, reject) => {
      const tx = db.transaction("handles", "readwrite");
      tx.objectStore("handles").put(handle, "folder");
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
    });
  } finally {
    db.close();
  }
}

async function loadHandle() {
  const db = await openDb();
  try {
    return await new Promise((resolve, reject) => {
      const tx = db.transaction("handles", "readonly");
      const req = tx.objectStore("handles").get("folder");
      req.onsuccess = () => resolve(req.result || null);
      req.onerror = () => reject(req.error);
    });
  } finally {
    db.close();
  }
}

function showFolder(handle) {
  const folder = document.querySelector("#folder");
  folder.textContent = diskFolder && handle && handle.name ? handle.name : "";
}

async function writeFile(path, text) {
  if (!opfs) {
    return;
  }
  try {
    const parts = path.split("/");
    let dir = opfs;
    for (let i = 0; i < parts.length - 1; i++) {
      dir = await dir.getDirectoryHandle(parts[i], { create: true });
    }
    const handle = await dir.getFileHandle(parts[parts.length - 1], { create: true });
    const writable = await handle.createWritable();
    await writable.write(text);
    await writable.close();
    saveError = "";
  } catch (err) {
    if (saveError !== path) {
      saveError = path;
      write("Could not save " + path + ".\n", "err");
    }
  }
}

async function removeFile(path) {
  if (!opfs) {
    return;
  }
  const parts = path.split("/");
  let dir = opfs;
  for (let i = 0; i < parts.length - 1; i++) {
    dir = await dir.getDirectoryHandle(parts[i]);
  }
  await dir.removeEntry(parts[parts.length - 1]);
}

async function walk(dir, prefix, into) {
  for await (const [name, handle] of dir.entries()) {
    if (name.startsWith(".") || name === "node_modules") {
      continue;
    }
    const path = prefix ? prefix + "/" + name : name;
    if (handle.kind === "directory") {
      await walk(handle, path, into);
    } else {
      const file = await handle.getFile();
      if (skipFile(name, file.size)) {
        if (file.size > 256 * 1024 - 1 && /\.(fth|f|4th|fs|forth)$/i.test(name)) {
          skippedForth += 1;
        }
        continue;
      }
      const text = await file.text();
      if (text.includes("\0")) {
        continue;
      }
      into.set(path, text);
    }
  }
}

async function readRoot() {
  const next = new Map();
  skippedForth = 0;
  if (opfs) {
    await walk(opfs, "", next);
  }
  if (skippedForth) {
    write("Skipped " + skippedForth + " Forth files larger than 256KB.\n", "err");
  }
  if (!diskFolder && next.size === 0) {
    next.set("work.fth", SEED);
    await writeFile("work.fth", SEED);
  }
  files.clear();
  for (const [path, text] of next) {
    files.set(path, text);
  }
}

async function privateRoot() {
  opfs = await navigator.storage.getDirectory();
  diskFolder = false;
  await readRoot();
  showFolder(null);
}

async function useFolder(handle) {
  await saveCurrent();
  clearTimeout(saveTimer);
  const oldRoot = opfs;
  const oldDisk = diskFolder;
  opfs = handle;
  diskFolder = true;
  saveError = "";
  try {
    await readRoot();
  } catch (err) {
    opfs = oldRoot;
    diskFolder = oldDisk;
    write("Cannot read that folder.\n", "err");
    return;
  }
  try {
    await saveHandle(handle);
  } catch (err) {
    write("This folder will not be remembered after a reload.\n", "err");
  }
  showFolder(handle);
  currentPath = "";
  editor.value = "";
  if (files.size === 0) {
    where.textContent = "";
    where.className = "";
    renderTree();
    editor.focus();
    return;
  }
  await openFile([...files.keys()].sort()[0]);
}

function renderTree() {
  const root = { dirs: new Map(), files: [] };
  function nodeFor(path) {
    const parts = path.split("/");
    let node = root;
    for (let i = 0; i < parts.length - 1; i++) {
      if (!node.dirs.has(parts[i])) {
        node.dirs.set(parts[i], { dirs: new Map(), files: [] });
      }
      node = node.dirs.get(parts[i]);
    }
    return { node, leaf: parts[parts.length - 1] };
  }
  for (const path of files.keys()) {
    nodeFor(path).node.files.push(nodeFor(path).leaf);
  }
  listing.replaceChildren();
  function add(node, prefix, depth) {
    const names = [...node.dirs.keys()].sort((a, b) => a.localeCompare(b));
    for (const name of names) {
      const row = document.createElement("div");
      row.className = "dir";
      row.style.paddingLeft = 8 + depth * 14 + "px";
      row.textContent = name + "/";
      listing.appendChild(row);
      add(node.dirs.get(name), prefix ? prefix + "/" + name : name, depth + 1);
    }
    const leaves = node.files.slice().sort((a, b) => a.localeCompare(b));
    for (const name of leaves) {
      const path = prefix ? prefix + "/" + name : name;
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.path = path;
      button.textContent = name;
      button.style.paddingLeft = 8 + depth * 14 + "px";
      if (path === currentPath) {
        button.className = "active";
      }
      button.addEventListener("click", () => openFile(path));
      listing.appendChild(button);
    }
  }
  add(root, "", 0);
}

async function saveCurrent() {
  if (!currentPath) {
    return;
  }
  clearTimeout(saveTimer);
  files.set(currentPath, editor.value);
  await writeFile(currentPath, editor.value);
}

async function openFile(path) {
  if (currentPath && currentPath !== path) {
    await saveCurrent();
  }
  currentPath = path;
  editor.value = files.get(path) ?? "";
  where.textContent = path;
  where.className = "";
  renderTree();
  editor.focus();
}

function selectLine(line) {
  const lines = editor.value.split("\n");
  const index = Math.max(0, Math.min(lines.length, line) - 1);
  let start = 0;
  for (let i = 0; i < index; i++) {
    start += lines[i].length + 1;
  }
  editor.focus();
  editor.setSelectionRange(start, start + lines[index].length);
}

async function revealError(name, line) {
  if (name && files.has(name) && name !== currentPath) {
    await openFile(name);
  }
  if (line && (!name || name === currentPath)) {
    selectLine(line);
  }
}

function syncMount() {
  api.mountClear();
  for (const [path, text] of files) {
    if (!validPath(path) || text.length > 256 * 1024 - 1) {
      continue;
    }
    api.mount(path, text);
  }
}

async function report(code) {
  const text = api.output();
  if (text) {
    write(text, code ? "err" : "");
    if (!text.endsWith("\n")) {
      write("\n");
    }
  }
  if (code) {
    const name = api.errorName();
    const line = api.errorLine();
    if (line) {
      write((name ? name + ":" + line : "line " + line) + "\n", "err");
      await revealError(name, line);
      where.textContent = name ? name + ":" + line : currentPath;
      where.className = "err";
    }
    write("throw " + code + "\n", "err");
  } else if (api.bye()) {
    write("bye\n");
  }
  setPrompt();
}

async function loadFile(fresh) {
  if (!ready || busy || !currentPath) {
    return;
  }
  busy = true;
  try {
    await saveCurrent();
    write((fresh ? "restart " : "load ") + currentPath + "\n");
    if (fresh) {
      const bootCode = api.boot();
      const bootText = api.output();
      if (bootText) {
        write(bootText, bootCode ? "err" : "");
      }
      if (bootCode) {
        write("boot throw " + bootCode + "\n", "err");
        setPrompt();
        return;
      }
    }
    syncMount();
    api.setSource(currentPath);
    await report(api.eval(files.get(currentPath) ?? ""));
  } catch (err) {
    write("load failed: " + (err && err.message ? err.message : String(err)) + "\n", "err");
    setPrompt();
  } finally {
    busy = false;
  }
}

let deferredPrompt = null;
window.addEventListener("beforeinstallprompt", (event) => {
  event.preventDefault();
  deferredPrompt = event;
  installBtn.hidden = false;
});
installBtn.addEventListener("click", async () => {
  if (!deferredPrompt) {
    return;
  }
  deferredPrompt.prompt();
  await deferredPrompt.userChoice;
  deferredPrompt = null;
  installBtn.hidden = true;
});
if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("sw.js").catch(() => {});
}

const Module = await createEditForth();
const api = {
  boot: Module.cwrap("ef_web_boot", "number", []),
  eval: Module.cwrap("ef_web_eval", "number", ["string"]),
  mountClear: Module.cwrap("ef_web_mount_clear", "number", []),
  mount: Module.cwrap("ef_web_mount", "number", ["string", "string"]),
  setSource: Module.cwrap("ef_web_set_source", "number", ["string"]),
  output: Module.cwrap("ef_web_output", "string", []),
  depth: Module.cwrap("ef_web_depth", "number", []),
  bye: Module.cwrap("ef_web_bye", "number", []),
  errorLine: Module.cwrap("ef_web_error_line", "number", []),
  errorName: Module.cwrap("ef_web_error_name", "string", []),
};

try {
  const saved = await loadHandle();
  if (saved && (await saved.queryPermission({ mode: "readwrite" })) === "granted") {
    opfs = saved;
    diskFolder = true;
    await readRoot();
    showFolder(opfs);
  } else {
    await privateRoot();
  }
} catch (err) {
  try {
    await privateRoot();
  } catch (err2) {
    write("File storage is unavailable in this window. Files last until you close it.\n", "err");
  }
}

const bootCode = api.boot();
if (api.output()) {
  write(api.output());
}
if (bootCode !== 0) {
  write("boot throw " + bootCode + "\n", "err");
} else {
  ready = true;
}
setPrompt();
if (files.size) {
  const first = !diskFolder && files.has("work.fth") ? "work.fth" : [...files.keys()].sort()[0];
  await openFile(first);
} else {
  renderTree();
  editor.focus();
}

document.querySelector("#open-folder").addEventListener("click", async () => {
  if (typeof window.showDirectoryPicker !== "function") {
    write("Open Folder needs Chrome or Edge.\n", "err");
    return;
  }
  let handle;
  try {
    handle = await window.showDirectoryPicker({
      id: "editforth",
      mode: "readwrite",
      startIn: "documents",
    });
  } catch (err) {
    if (err && err.name === "AbortError") {
      return;
    }
    write("Could not open that folder.\n", "err");
    return;
  }
  await useFolder(handle);
});
document.querySelector("#load").addEventListener("click", () => loadFile(false));
document.querySelector("#restart").addEventListener("click", () => loadFile(true));
document.querySelector("#new-file").addEventListener("click", () => {
  nameForm.hidden = false;
  nameInput.value = parentDir(currentPath);
  if (nameInput.value) {
    nameInput.value += "/";
  }
  nameInput.focus();
});
document.querySelector("#delete").addEventListener("click", async () => {
  if (!currentPath) {
    return;
  }
  if (!window.confirm("Delete " + currentPath + "?")) {
    return;
  }
  const path = currentPath;
  try {
    await removeFile(path);
  } catch (err) {
    write("Could not delete " + path + ".\n", "err");
    return;
  }
  files.delete(path);
  currentPath = "";
  editor.value = "";
  const rest = [...files.keys()].sort();
  if (rest.length) {
    await openFile(rest[0]);
  } else {
    where.textContent = "";
    renderTree();
  }
});
nameForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const path = nameInput.value.trim().replace(/^\/+/, "");
  nameForm.hidden = true;
  if (!validPath(path)) {
    where.textContent = "Use a name like work.fth or lib/work.fth";
    where.className = "err";
    return;
  }
  if (!files.has(path)) {
    const text = "\\ " + path + "\n";
    files.set(path, text);
    await writeFile(path, text);
  }
  await openFile(path);
});
editor.addEventListener("input", () => {
  if (!currentPath) {
    return;
  }
  files.set(currentPath, editor.value);
  where.textContent = currentPath;
  where.className = "";
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => writeFile(currentPath, editor.value), 300);
});
editor.addEventListener("keydown", (event) => {
  if (!(event.metaKey || event.ctrlKey) || event.key !== "Enter") {
    return;
  }
  event.preventDefault();
  loadFile(event.shiftKey);
});

function recallHistory(up) {
  if (history.length === 0) {
    return;
  }
  if (up) {
    historyAt = Math.max(0, historyAt - 1);
  } else {
    historyAt = Math.min(history.length, historyAt + 1);
  }
  render(historyAt === history.length ? "" : history[historyAt], true);
}

async function submitLine() {
  if (!ready || busy) {
    return;
  }
  const line = currentTail().trim();
  const shown = currentTail().endsWith("\n") ? currentTail() : currentTail() + "\n";
  chunks.push({ text: shown, cls: "" });
  render("", false);
  if (line !== "") {
    history.push(line);
    if (history.length > 50) {
      history.shift();
    }
  }
  historyAt = history.length;
  busy = true;
  try {
    await saveCurrent();
    syncMount();
    api.setSource("");
    await report(api.eval(line));
  } catch (err) {
    write("load failed: " + (err && err.message ? err.message : String(err)) + "\n", "err");
    setPrompt();
  } finally {
    busy = false;
    setPrompt(true);
  }
}

log.addEventListener("keydown", (event) => {
  if (event.isComposing) {
    return;
  }
  if (event.key === "ArrowUp" || event.key === "ArrowDown") {
    event.preventDefault();
    recallHistory(event.key === "ArrowUp");
    return;
  }
  if (event.key === "Enter" && !event.metaKey && !event.ctrlKey && !event.altKey) {
    event.preventDefault();
    submitLine();
  }
});
log.addEventListener("beforeinput", (event) => {
  if (programmatic) {
    return;
  }
  const sel = window.getSelection();
  if (!sel.rangeCount) {
    return;
  }
  const range = sel.getRangeAt(0);
  let start = boundaryOffset(range.startContainer, range.startOffset);
  let end = boundaryOffset(range.endContainer, range.endOffset);
  if (start > end) {
    const swap = start;
    start = end;
    end = swap;
  }
  const prot = protectedString().length;
  let delStart = start;
  if (event.inputType === "deleteContentBackward" && start === end) {
    delStart = start - 1;
  }
  if (delStart >= prot && end >= prot) {
    return;
  }
  event.preventDefault();
  if (event.inputType.startsWith("insert") && event.data) {
    render(currentTail() + event.data, true);
  }
});
log.addEventListener("input", () => {
  if (programmatic || busy) {
    return;
  }
  const text = readLog();
  const prot = protectedString();
  if (!text.startsWith(prot) || log.querySelector("div, p, br")) {
    const tail = text.startsWith(prot) ? text.slice(prot.length) : "";
    render(tail, true);
    if (!tail.includes("\n")) {
      return;
    }
  }
  if (currentTail().includes("\n")) {
    submitLine();
  }
});

installSplitter();

function installSplitter() {
  const split = document.querySelector("#split");
  const main = document.querySelector("#main");
  const consolePane = document.querySelector("#console");
  const key = "editforth.consoleHeight";
  const minConsole = 88;
  const minEditor = 120;
  const splitHeight = 18;

  function cap() {
    return Math.max(0, main.clientHeight - minEditor - splitHeight);
  }

  function apply(px, persist) {
    const limit = cap();
    const floor = Math.min(minConsole, limit);
    const next = Math.round(Math.min(Math.max(px, floor), limit));
    consolePane.style.height = next + "px";
    split.setAttribute("aria-valuemin", String(Math.round(floor)));
    split.setAttribute("aria-valuemax", String(Math.round(limit)));
    split.setAttribute("aria-valuenow", String(next));
    if (persist) {
      try {
        localStorage.setItem(key, String(next));
      } catch (err) {
        /* private mode can refuse storage */
      }
    }
    return next;
  }

  let stored = 190;
  try {
    const raw = Number(localStorage.getItem(key));
    if (Number.isFinite(raw) && raw > 0) {
      stored = raw;
    }
  } catch (err) {
    stored = 190;
  }
  apply(stored, false);

  let drag = null;
  split.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) {
      return;
    }
    drag = { y: event.clientY, h: consolePane.getBoundingClientRect().height };
    split.setPointerCapture(event.pointerId);
    document.body.classList.add("resizing");
    event.preventDefault();
  });
  split.addEventListener("pointermove", (event) => {
    if (!drag) {
      return;
    }
    apply(drag.h - (event.clientY - drag.y), false);
  });
  function endDrag() {
    if (!drag) {
      return;
    }
    drag = null;
    document.body.classList.remove("resizing");
    apply(consolePane.getBoundingClientRect().height, true);
  }
  split.addEventListener("pointerup", endDrag);
  split.addEventListener("pointercancel", endDrag);
  split.addEventListener("lostpointercapture", endDrag);

  split.addEventListener("keydown", (event) => {
    const step = event.shiftKey ? 40 : 16;
    const height = consolePane.getBoundingClientRect().height;
    if (event.key === "ArrowUp") {
      apply(height + step, true);
      event.preventDefault();
    } else if (event.key === "ArrowDown") {
      apply(height - step, true);
      event.preventDefault();
    }
  });

  new ResizeObserver(() => {
    const height = consolePane.getBoundingClientRect().height;
    if (height > cap() + 0.5) {
      apply(height, true);
    }
  }).observe(main);
}
