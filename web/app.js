import createEditForth from "./editforth.js?v=38";

const log = document.querySelector("#log");
const installBtn = document.querySelector("#install");
const editor = document.querySelector("#editor");
const editorPane = document.querySelector("#editor-pane");
const hl = document.querySelector("#hl");
const tabsEl = document.querySelector("#tabs");
const banner = document.querySelector("#banner");
const saveBtn = document.querySelector("#save");
const gutterInner = document.querySelector("#gutter-inner");
const gutterNums = document.querySelector("#gutter-nums");
const gutterMark = document.querySelector("#gutter-mark");
const listing = document.querySelector("#listing");
const where = document.querySelector("#where");
const nameForm = document.querySelector("#name-form");
const nameInput = document.querySelector("#name-input");

const BANNER = "=== EditForthPWA 2.5.6 === Oct 26, 2026 3:26 PM ===\n";

const SEED =
  "\\ Cmd+Enter or Ctrl+Enter loads this file.\n" +
  "\n" +
  ': HELLO ( -- ) ." Hello from the editor." CR ;\n' +
  "\n" +
  "HELLO\n";

const files = new Map();
const shipped = new Map();
const SHIPPED_LIB = [
  "Resources/Library/Debugger/debugger.fth",
  "Resources/Library/Debugger/debug-bp.fth",
  "Resources/Library/Debugger/dbg-pause.fth",
  "Resources/Library/Debugger/dbg-ed.fth",
  "Resources/Library/Debugger/dbg-map.fth",
  "Resources/Library/Debugger/debug-runto.fth",
];
const history = [];
let historyAt = 0;
const chunks = [];
let programmatic = false;
let currentPath = "";
let currentId = 0;
let nextTabId = 1;
const tabs = [];
let saveAsWait = null;
let askingEdit = false;
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

function resetConsole() {
  chunks.length = 0;
  chunks.push({ text: BANNER, cls: "" });
  render("", false);
}

let bootLog = "";

function peelBoot(text) {
  const parts = text.split("\n");
  const show = [];
  const boot = [];
  const endsNL = text.endsWith("\n");
  for (let i = 0; i < parts.length; i++) {
    const line = parts[i];
    const incomplete = i === parts.length - 1 && !endsNL;
    if (!incomplete && /^ (?:Loading|Finished Loading): \S+$/.test(line)) {
      boot.push(line);
    } else {
      show.push(line);
    }
  }
  bootLog = boot.length ? boot.join("\n") + "\n" : "";
  return show.join("\n");
}

function writeBoot(text, cls) {
  const shown = peelBoot(text);
  if (shown) {
    write(shown, cls);
  }
}

function write(text, cls) {
  if (!text) {
    return;
  }
  const cut = text.indexOf("\f");
  if (cut >= 0) {
    const tail = currentTail();
    if (cut > 0) {
      write(text.slice(0, cut), cls);
    }
    chunks.length = 0;
    chunks.push({ text: BANNER, cls: "" });
    render(tail, false);
    write(text.slice(cut + 1), cls);
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
    return false;
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
    return true;
  } catch (err) {
    if (saveError !== path) {
      saveError = path;
      write("Could not save " + path + ".\n", "err");
    }
    return false;
  }
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
  rememberEditor();
  const dirty = tabs.filter((tab) => bufferOf(tab) !== tab.saved);
  if (dirty.length) {
    const choice = await askSave("Save changes before opening this folder?");
    if (choice === "cancel") {
      return false;
    }
    if (choice === "save") {
      for (const tab of dirty) {
        if (!(await saveDirty(tab))) {
          return false;
        }
      }
    }
  }
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
    return false;
  }
  try {
    await saveHandle(handle);
  } catch (err) {
    write("This folder will not be remembered after a reload.\n", "err");
  }
  showFolder(handle);
  tabs.length = 0;
  currentId = 0;
  currentPath = "";
  if (files.size === 0) {
    newUntitled();
    return true;
  }
  await openFile([...files.keys()].sort()[0]);
  return true;
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

function currentTab() {
  return tabs.find((tab) => tab.id === currentId) || null;
}

function bufferOf(tab) {
  if (!tab) {
    return "";
  }
  if (tab.id === currentId) {
    return editor.value;
  }
  if (tab.path && files.has(tab.path)) {
    return files.get(tab.path);
  }
  if (tab.text) {
    return tab.text;
  }
  if (tab.path && shipped.has(tab.path)) {
    return shipped.get(tab.path);
  }
  return "";
}

function rememberEditor() {
  const tab = currentTab();
  if (!tab) {
    return;
  }
  if (tab.path && files.has(tab.path)) {
    files.set(tab.path, editor.value);
  } else {
    tab.text = editor.value;
  }
}

function askSave(message) {
  const box = document.querySelector("#save-ask");
  document.querySelector("#save-ask-text").textContent = message;
  box.hidden = false;
  return new Promise((resolve) => {
    const buttons = [...box.querySelectorAll("button")];
    const finish = (choice) => {
      buttons.forEach((button) => button.removeEventListener("click", onClick));
      box.hidden = true;
      resolve(choice);
    };
    const onClick = (event) => finish(event.currentTarget.dataset.choice);
    buttons.forEach((button) => button.addEventListener("click", onClick));
  });
}

async function saveTab(tab) {
  if (!tab || !tab.path) {
    return false;
  }
  rememberEditor();
  const text = files.get(tab.path) ?? "";
  if (!(await writeFile(tab.path, text))) {
    return false;
  }
  tab.saved = text;
  updateSaveButton();
  renderTabs();
  return true;
}

function saveAs(tab) {
  nameForm.hidden = false;
  nameInput.value = tab.path === "boot" ? "boot.fth" : (tab.title || "Untitled.fth");
  nameInput.focus();
  nameInput.select();
  return new Promise((resolve) => {
    saveAsWait = { tab, resolve };
  });
}

async function finishSaveAs(path) {
  const pending = saveAsWait;
  if (!pending) {
    return;
  }
  const tab = pending.tab;
  if (path === "boot") {
    where.textContent = "boot is the cold image";
    where.className = "err";
    nameForm.hidden = false;
    return;
  }
  if (!validPath(path)) {
    where.textContent = "Use a name like work.fth or lib/work.fth";
    where.className = "err";
    nameForm.hidden = false;
    return;
  }
  if (files.has(path) || tabs.some((item) => item !== tab && item.path === path)) {
    where.textContent = path + " already exists";
    where.className = "err";
    nameForm.hidden = false;
    return;
  }
  const text = bufferOf(tab);
  if (!(await writeFile(path, text))) {
    saveAsWait = null;
    nameForm.hidden = true;
    pending.resolve(false);
    return;
  }
  files.set(path, text);
  tab.path = path;
  tab.title = path.split("/").pop();
  tab.saved = text;
  tab.text = "";
  saveAsWait = null;
  nameForm.hidden = true;
  if (currentId === tab.id) {
    currentPath = path;
    where.textContent = path;
    where.className = "";
  }
  updateSaveButton();
  renderTabs();
  renderTree();
  pending.resolve(true);
}

function cancelSaveAs() {
  if (!saveAsWait) {
    nameForm.hidden = true;
    return;
  }
  const pending = saveAsWait;
  saveAsWait = null;
  nameForm.hidden = true;
  pending.resolve(false);
}

async function saveDirty(tab) {
  if (!tab) {
    return false;
  }
  if (!tab.path || !files.has(tab.path)) {
    return saveAs(tab);
  }
  return saveTab(tab);
}

function updateSaveButton() {
  const tab = currentTab();
  saveBtn.disabled = !tab || bufferOf(tab) === tab.saved;
}

function renderTabs() {
  tabsEl.replaceChildren();
  for (const tab of tabs) {
    const leaf = tab.path ? tab.path.split("/").pop() : tab.title;
    const dup = tab.path && tabs.filter((item) => item.path && item.path.split("/").pop() === leaf).length > 1;
    const chip = document.createElement("div");
    chip.className = "tab" + (tab.id === currentId ? " active" : "");
    const select = document.createElement("button");
    select.type = "button";
    select.className = "tab-name";
    const dirty = bufferOf(tab) !== tab.saved;
    select.textContent = (dirty ? "• " : "") + (dup ? tab.path : leaf);
    select.title = tab.path || tab.title;
    select.addEventListener("click", () => {
      if (tab.path) {
        openFile(tab.path);
      } else {
        rememberEditor();
        showTab(tab);
      }
    });
    const close = document.createElement("button");
    close.type = "button";
    close.className = "tab-close";
    close.textContent = "×";
    close.title = "Close";
    close.addEventListener("click", (event) => {
      event.stopPropagation();
      closeTab(tab.id);
    });
    chip.append(select, close);
    tabsEl.appendChild(chip);
  }
}

function leaveView() {
  const tab = currentTab();
  if (!tab) {
    return;
  }
  tab.view = false;
  editor.readOnly = false;
  banner.hidden = true;
  editor.focus();
}

function showTab(tab, opts) {
  if (opts && opts.view === true) {
    tab.view = true;
  }
  currentId = tab.id;
  currentPath = tab.path;
  clearHighlight();
  if (!tab.path) {
    editor.value = tab.text ?? "";
  } else if (files.has(tab.path)) {
    editor.value = files.get(tab.path);
  } else {
    editor.value = tab.text || shipped.get(tab.path) || "";
  }
  editor.readOnly = tab.view;
  banner.hidden = !tab.view;
  where.textContent = tab.path || tab.title;
  where.className = "";
  paintLines();
  renderTabs();
  renderTree();
  updateSaveButton();
  editor.focus();
}

function showNone() {
  currentId = 0;
  currentPath = "";
  clearHighlight();
  editor.value = "";
  editor.readOnly = true;
  banner.hidden = true;
  where.textContent = "";
  where.className = "";
  paintLines();
  renderTabs();
  renderTree();
  updateSaveButton();
}

function newUntitled() {
  rememberEditor();
  let title = "Untitled.fth";
  let n = 1;
  const taken = (name) => files.has(name) || tabs.some((tab) => tab.path === name || (!tab.path && tab.title === name));
  while (taken(title)) {
    n += 1;
    title = "Untitled" + n + ".fth";
  }
  const tab = { id: nextTabId++, path: "", title, saved: "", view: false, text: "" };
  tabs.push(tab);
  showTab(tab);
}

async function openFile(path, opts) {
  rememberEditor();
  let tab = tabs.find((item) => item.path === path);
  if (!tab) {
    tab = { id: nextTabId++, path, title: path.split("/").pop(), saved: files.get(path) ?? "", view: false, text: "" };
    tabs.push(tab);
  }
  showTab(tab, opts);
}

async function closeTab(id) {
  rememberEditor();
  const tab = tabs.find((item) => item.id === id);
  if (!tab) {
    return;
  }
  const label = tab.path || tab.title;
  if (bufferOf(tab) !== tab.saved) {
    const choice = await askSave("Save changes to " + label + " before closing?");
    if (choice === "cancel") {
      return;
    }
    if (choice === "save") {
      if (!(await saveDirty(tab))) {
        return;
      }
    } else if (tab.path && files.has(tab.path)) {
      files.set(tab.path, tab.saved);
    }
  }
  const index = tabs.indexOf(tab);
  tabs.splice(index, 1);
  if (currentId !== id) {
    renderTabs();
    renderTree();
    updateSaveButton();
    return;
  }
  const next = tabs[index] || tabs[index - 1];
  currentId = 0;
  currentPath = "";
  if (next) {
    showTab(next);
    return;
  }
  showNone();
}

function lineCount() {
  const text = editor.value;
  if (text.length === 0) {
    return 1;
  }
  let n = 1;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) === 10) {
      n++;
    }
  }
  return n;
}

function paintLines() {
  const n = lineCount();
  const parts = new Array(n);
  for (let i = 0; i < n; i++) {
    parts[i] = String(i + 1);
  }
  gutterNums.textContent = parts.join("\n");
  gutterNums.style.minWidth = String(n).length + "ch";
  gutterMark.hidden = true;
  syncGutter();
}

function syncGutter() {
  gutterInner.style.transform = "translateY(" + (-editor.scrollTop) + "px)";
  if (editorPane.classList.contains("debugging")) {
    hl.scrollTop = editor.scrollTop;
    hl.scrollLeft = editor.scrollLeft;
  }
}

function clearHighlight() {
  hl.replaceChildren();
  editorPane.classList.remove("debugging");
}

function isForthSpace(ch) {
  return ch === " " || ch === "\t" || ch === "\n" || ch === "\r";
}

function firstWholeWord(text, needle, from, to, ignoreCase) {
  const hay = ignoreCase ? text.toLowerCase() : text;
  const want = ignoreCase ? needle.toLowerCase() : needle;
  let at = from;
  while (at < to) {
    const hit = hay.indexOf(want, at);
    if (hit < 0 || hit >= to) {
      return -1;
    }
    const end = hit + want.length;
    const before = hit === 0 || isForthSpace(text[hit - 1]);
    const after = end >= text.length || isForthSpace(text[end]);
    if (end <= to && before && after) {
      return hit;
    }
    at = hit + 1;
  }
  return -1;
}

function lineStart(text, line) {
  if (line <= 1) {
    return 0;
  }
  let n = 1;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) === 10) {
      n++;
      if (n === line) {
        return i + 1;
      }
    }
  }
  return text.length;
}

function definitionEnd(text, from) {
  const semi = firstWholeWord(text, ";", from, text.length, false);
  if (semi >= 0) {
    return semi + 1;
  }
  let i = from;
  while (i < text.length && text[i] !== "\n") {
    i++;
  }
  if (i < text.length) {
    i++;
  }
  while (i < text.length) {
    let j = i;
    while (text[j] === " " || text[j] === "\t") {
      j++;
    }
    if (text[j] === ":" && (j + 1 >= text.length || isForthSpace(text[j + 1]))) {
      return i;
    }
    while (i < text.length && text[i] !== "\n") {
      i++;
    }
    if (i < text.length) {
      i++;
    }
  }
  return text.length;
}

function highlightNeedles(name) {
  const key = name.toUpperCase();
  if (key === "EXIT") {
    return [[";", false], ["EXIT", true]];
  }
  if (key === '(S")') {
    return [['S"', false], ['."', false]];
  }
  if (key === '(.")') {
    return [['."', false], ['S"', false]];
  }
  if (key === '(C")') {
    return [['C"', false]];
  }
  if (key === "0BRANCH") {
    return [["IF", true], ["WHILE", true], ["UNTIL", true]];
  }
  if (key === "BRANCH") {
    return [["ELSE", true], ["AGAIN", true], ["REPEAT", true]];
  }
  if (key === "(DO)") {
    return [["DO", true]];
  }
  if (key === "(?DO)") {
    return [["?DO", false]];
  }
  if (key === "(LOOP)") {
    return [["LOOP", true]];
  }
  if (key === "(+LOOP)") {
    return [["+LOOP", false]];
  }
  if (key === "LIT") {
    return [["[']", false], ["[CHAR]", true], ["LIT", true]];
  }
  return [[name, true]];
}

function findWordRange(text, name, line, startAt) {
  const from = lineStart(text, line || 1);
  const to = definitionEnd(text, from);
  const begin = startAt == null ? from : Math.max(from, startAt);
  if (begin >= to) {
    return null;
  }
  for (const [needle, ignoreCase] of highlightNeedles(name)) {
    const hit = firstWholeWord(text, needle, begin, to, ignoreCase);
    if (hit >= 0) {
      return [hit, hit + needle.length];
    }
  }
  return null;
}

function utf8Range(text, off, len) {
  if (off < 0 || len <= 0) {
    return null;
  }
  const bytes = new TextEncoder().encode(text);
  if (off + len > bytes.length) {
    return null;
  }
  const dec = new TextDecoder();
  const start = dec.decode(bytes.subarray(0, off)).length;
  const count = dec.decode(bytes.subarray(off, off + len)).length;
  return [start, start + count];
}

function paintHighlight(text, start, end) {
  hl.replaceChildren();
  hl.append(document.createTextNode(text.slice(0, start)));
  const mark = document.createElement("mark");
  mark.textContent = text.slice(start, end);
  hl.append(mark);
  hl.append(document.createTextNode(text.slice(end)));
  editorPane.classList.add("debugging");
  hl.scrollTop = editor.scrollTop;
  hl.scrollLeft = editor.scrollLeft;
}

function scrollIndex(index) {
  const line = editor.value.slice(0, index).split("\n").length;
  const cs = getComputedStyle(editor);
  const lh = parseFloat(cs.lineHeight) || 21;
  const pad = parseFloat(cs.paddingTop) || 0;
  const y = pad + (line - 1) * lh;
  if (y < editor.scrollTop || y + lh > editor.scrollTop + editor.clientHeight) {
    editor.scrollTop = Math.max(0, y - editor.clientHeight / 3);
  }
  syncGutter();
}

let lastWash = null;

function showDebug(path, line, name, off, len) {
  if (!path) {
    lastWash = null;
    clearHighlight();
    return;
  }
  const known = files.has(path) ? files.get(path) : (shipped.get(path) ?? null);
  if (known == null) {
    return;
  }
  let tab = tabs.find((item) => item.path === path);
  if (!tab) {
    if (files.has(path)) {
      openFile(path, { view: true });
    } else {
      rememberEditor();
      tab = {
        id: nextTabId++,
        path,
        title: path.split("/").pop(),
        saved: known,
        view: true,
        text: known,
      };
      tabs.push(tab);
      showTab(tab, { view: true });
    }
  } else if (tab.id !== currentId) {
    rememberEditor();
    showTab(tab, { view: true });
  } else if (!tab.view) {
    tab.view = true;
    editor.readOnly = true;
    banner.hidden = false;
  }
  const text = editor.value;
  let range = utf8Range(text, off, len);
  if (!range && name) {
    const after = lastWash && lastWash.path === path && lastWash.name === name ? lastWash.end : null;
    range = findWordRange(text, name, line, after);
  }
  /* A repeated token with the same span, or a name search that starts over,
     would wash the first copy again. Take the next copy in the definition. */
  if (range && lastWash && lastWash.path === path && lastWash.name === name && range[0] === lastWash.start) {
    const next = findWordRange(text, name, line, lastWash.end);
    if (next) {
      range = next;
    }
  }
  if (!range) {
    lastWash = null;
    clearHighlight();
    if (line > 0) {
      scrollIndex(lineStart(text, line));
    }
    return;
  }
  lastWash = { path, name, start: range[0], end: range[1] };
  paintHighlight(text, range[0], range[1]);
  editor.setSelectionRange(range[0], range[0]);
  scrollIndex(range[0]);
}

function markLine(line) {
  const n = lineCount();
  const shown = Math.max(1, Math.min(n, line));
  const pad = parseFloat(getComputedStyle(gutterInner).paddingTop) || 0;
  const lh = parseFloat(getComputedStyle(gutterNums).lineHeight) || 21;
  gutterMark.hidden = false;
  gutterMark.style.top = (pad + (shown - 1) * lh) + "px";
}

function selectLine(line) {
  const lines = editor.value.split("\n");
  const index = Math.max(0, Math.min(lines.length, line) - 1);
  let start = 0;
  for (let i = 0; i < index; i++) {
    start += lines[i].length + 1;
  }
  const cs = getComputedStyle(editor);
  const lh = parseFloat(cs.lineHeight) || 21;
  const pad = parseFloat(cs.paddingTop) || 0;
  const y = pad + index * lh;
  if (y < editor.scrollTop || y + lh > editor.scrollTop + editor.clientHeight) {
    editor.scrollTop = Math.max(0, y - editor.clientHeight / 3);
  }
  syncGutter();
  markLine(index + 1);
  editor.focus();
  editor.setSelectionRange(start, start + lines[index].length);
}

async function revealError(name, line) {
  if (name && files.has(name)) {
    await openFile(name, { view: true });
  } else if (name) {
    const tab = tabs.find((item) => item.path === name || (!item.path && item.title === name));
    if (tab) {
      rememberEditor();
      showTab(tab, { view: true });
    }
  }
  const shown = currentTab();
  const shownName = shown ? (shown.path || shown.title) : "";
  if (line && (!name || name === shownName)) {
    selectLine(line);
  }
}

function mountOne(path, text, seen) {
  if (seen.has(path) || !validPath(path) || text.length > 256 * 1024 - 1) {
    return;
  }
  api.mount(path, text);
  seen.add(path);
}

function syncMount() {
  api.mountClear();
  const seen = new Set();
  for (const [path, text] of files) {
    mountOne(path, text, seen);
  }
  for (const [path, text] of shipped) {
    /* "boot" stays the embedded image. A mount of the same name would hide it. */
    if (path === "boot") {
      continue;
    }
    mountOne(path, text, seen);
  }
}

async function loadShipped() {
  let missing = 0;
  for (const path of SHIPPED_LIB) {
    try {
      const res = await fetch(path);
      if (!res.ok) {
        missing += 1;
        continue;
      }
      shipped.set(path, await res.text());
    } catch (err) {
      missing += 1;
    }
  }
  if (missing && !files.has(SHIPPED_LIB[0])) {
    write("Debugger library files were not found next to this page.\n", "err");
  }
}

function debugCode(event) {
  if (event.metaKey || event.ctrlKey || event.altKey) {
    return null;
  }
  if (event.key === "F6") {
    return (2 << 24) | 16;
  }
  if (event.key === "F7") {
    return (2 << 24) | 17;
  }
  if (event.key === "F8") {
    return (2 << 24) | 18;
  }
  if (event.key === "Escape") {
    return 27;
  }
  if (event.key === "Enter") {
    return 13;
  }
  if (event.key === " ") {
    return 32;
  }
  if (event.key.length === 1) {
    return event.key.charCodeAt(0);
  }
  return null;
}

async function runEval(source) {
  shownOut = 0;
  const code = await api.eval(source);
  const text = api.output() || "";
  const rest = text.slice(shownOut);
  const visible = text.replaceAll("\f", "");
  if (shownOut === 0) {
    if (text) {
      write(labelRoot(text), code ? "err" : "");
      if (visible && !visible.endsWith("\n")) {
        write("\n");
      }
    }
  } else if (rest) {
    write(rest, code ? "err" : "");
    if (visible && !visible.endsWith("\n")) {
      write("\n");
    }
  } else if (visible && !visible.endsWith("\n")) {
    write("\n");
  }
  if (code) {
    const name = api.errorName();
    const line = api.errorLine();
    if (line) {
      write((name ? name + ":" + line : "line " + line) + "\n", "err");
      await revealError(name, line);
      where.textContent = name ? name + ":" + line : (currentTab() ? (currentTab().path || currentTab().title) : "");
      where.className = "err";
    }
    write("throw " + code + "\n", "err");
  } else if (api.bye()) {
    write("bye\n");
  }
  setPrompt();
}

async function loadFile(fresh) {
  const tab = currentTab();
  if (!ready || busy || (!tab && !fresh)) {
    return;
  }
  busy = true;
  try {
    rememberEditor();
    if (fresh) {
      resetConsole();
      const bootCode = api.boot();
      const bootText = api.output();
      if (bootText) {
        writeBoot(bootText, bootCode ? "err" : "");
      }
      syncMount();
      api.setSource("");
      if (bootCode) {
        write("boot throw " + bootCode + "\n", "err");
      }
      setPrompt();
      return;
    }
    const name = tab.path || tab.title;
    write("include " + name + "\n");
    syncMount();
    api.setSource(name);
    await runEval(bufferOf(tab));
  } catch (err) {
    write("include failed: " + (err && err.message ? err.message : String(err)) + "\n", "err");
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

const menuBtn = document.querySelector("#menu-btn");
const menuPanel = document.querySelector("#menu-panel");
function setMenu(open) {
  menuPanel.hidden = !open;
  menuBtn.setAttribute("aria-expanded", open ? "true" : "false");
}
menuBtn.addEventListener("click", (event) => {
  event.stopPropagation();
  setMenu(menuPanel.hidden);
});
menuPanel.addEventListener("click", (event) => {
  event.stopPropagation();
});
document.addEventListener("click", () => setMenu(false));
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && !menuPanel.hidden) {
    setMenu(false);
    menuBtn.focus();
  }
});

document.querySelector("#show-boot").addEventListener("click", () => {
  setMenu(false);
  if (bootLog) {
    write(bootLog);
  }
});

document.querySelector("#open-folder").addEventListener("click", async () => {
  setMenu(false);
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

const Module = await createEditForth({
  locateFile(path, prefix) {
    return (prefix || "") + path + "?v=38";
  },
});
Module._forthDebug = (path, line, name, off, len) => {
  showDebug(path, line, name, off, len);
};
const api = {
  boot: Module.cwrap("ef_web_boot", "number", []),
  eval: Module.cwrap("ef_web_eval", "number", ["string"], { async: true }),
  mountClear: Module.cwrap("ef_web_mount_clear", "number", []),
  mount: Module.cwrap("ef_web_mount", "number", ["string", "string"]),
  setSource: Module.cwrap("ef_web_set_source", "number", ["string"]),
  output: Module.cwrap("ef_web_output", "string", []),
  depth: Module.cwrap("ef_web_depth", "number", []),
  bye: Module.cwrap("ef_web_bye", "number", []),
  errorLine: Module.cwrap("ef_web_error_line", "number", []),
  errorName: Module.cwrap("ef_web_error_name", "string", []),
  bootSource: Module.cwrap("ef_web_boot_source", "string", []),
  postKey: Module.cwrap("ef_web_post_key", null, ["number"]),
  keyWaiting: Module.cwrap("ef_web_key_waiting", "number", []),
};

const cold = api.bootSource();
if (cold) {
  shipped.set("boot", cold);
}

let shownOut = 0;
function pumpDebug() {
  const text = api.output() || "";
  if (text.length <= shownOut) {
    return;
  }
  write(text.slice(shownOut));
  shownOut = text.length;
}
Module._forthPump = pumpDebug;

write(BANNER);
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
  writeBoot(api.output());
}
if (bootCode !== 0) {
  write("boot throw " + bootCode + "\n", "err");
} else {
  await loadShipped();
  ready = true;
}
setPrompt();
showNone();

document.querySelector("#load").addEventListener("click", () => loadFile(false));
document.querySelector("#restart").addEventListener("click", () => loadFile(true));
document.querySelector("#close-file").addEventListener("click", () => {
  if (currentTab()) {
    closeTab(currentId);
  }
});
document.querySelector("#save").addEventListener("click", () => {
  const tab = currentTab();
  if (tab && bufferOf(tab) !== tab.saved) {
    saveDirty(tab);
  }
});
document.querySelector("#leave-view").addEventListener("click", leaveView);
document.addEventListener("keydown", (event) => {
  if ((event.metaKey || event.ctrlKey) && !event.altKey && event.key.toLowerCase() === "s") {
    event.preventDefault();
    const tab = currentTab();
    if (tab && bufferOf(tab) !== tab.saved) {
      saveDirty(tab);
    }
  }
});
document.querySelector("#new-file").addEventListener("click", () => {
  newUntitled();
});
nameForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const path = nameInput.value.trim().replace(/^\/+/, "");
  finishSaveAs(path);
});
nameInput.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    event.preventDefault();
    cancelSaveAs();
  }
});
editor.addEventListener("scroll", syncGutter);
editor.addEventListener("input", () => {
  clearHighlight();
  paintLines();
  const tab = currentTab();
  if (!tab) {
    return;
  }
  if (tab.path && files.has(tab.path)) {
    files.set(tab.path, editor.value);
  } else {
    tab.text = editor.value;
  }
  where.textContent = tab.path || tab.title;
  where.className = "";
  updateSaveButton();
  renderTabs();
});
function isTypingKey(event) {
  if (event.metaKey || event.ctrlKey || event.altKey) {
    return false;
  }
  return event.key === "Backspace" || event.key === "Delete" || event.key === "Enter" || event.key === "Tab" || event.key.length === 1;
}
editor.addEventListener("keydown", (event) => {
  if (editor.readOnly && isTypingKey(event)) {
    event.preventDefault();
    if (!currentTab()) {
      return;
    }
    if (!askingEdit) {
      askingEdit = true;
      const yes = window.confirm("This file is in view mode. Switch to Edit?");
      askingEdit = false;
      if (yes) {
        leaveView();
      }
    }
    return;
  }
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

/* Browser folder ids. CHDIR of one of these, when it is not already in the
   mount, opens the folder picker there. The call has to start in the key handler. */
const FOLDER_IDS = ["desktop", "documents", "downloads", "music", "pictures", "videos"];

function chdirFolderId(line) {
  const match = /^\s*chdir\s+(?:"([^"]*)"|(\S+))\s*$/i.exec(line);
  if (!match) {
    return "";
  }
  const raw = match[1] != null ? match[1] : match[2];
  const name = raw.toLowerCase();
  return FOLDER_IDS.includes(name) ? name : "";
}

function mountedTopDir(name) {
  const want = name.toLowerCase();
  const paths = [...files.keys(), ...shipped.keys()];
  for (const path of paths) {
    const seg = path.split("/")[0];
    if (path.includes("/") && seg.toLowerCase() === want) {
      return seg;
    }
  }
  return "";
}

function labelRoot(text) {
  const name = diskFolder && opfs && opfs.name ? opfs.name : "";
  if (!name || !text) {
    return text;
  }
  return text.replaceAll("Directory of .", "Directory of " + name).replaceAll("Current directory: .", "Current directory: " + name);
}

async function cwdToRoot() {
  for (let i = 0; i < 40; i++) {
    await api.eval("CHDIR ..");
    const text = api.output() || "";
    if (text.includes("CHDIR error")) {
      return;
    }
  }
}

async function submitLine(folderPick, lineOverride) {
  if (!ready || busy) {
    return;
  }
  const line = lineOverride != null ? lineOverride : currentTail().trim();
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
    if (folderPick) {
      let handle;
      try {
        handle = await folderPick;
      } catch (err) {
        if (!(err && err.name === "AbortError")) {
          write("Could not open that folder.\n", "err");
        }
        return;
      }
      const opened = await useFolder(handle);
      if (!opened) {
        return;
      }
      await cwdToRoot();
      write("Current directory: " + (handle.name || "folder") + "\n");
      return;
    }
    rememberEditor();
    syncMount();
    api.setSource("");
    await runEval(line);
  } catch (err) {
    write("include failed: " + (err && err.message ? err.message : String(err)) + "\n", "err");
    setPrompt();
  } finally {
    busy = false;
    setPrompt(true);
  }
}

document.addEventListener("keydown", (event) => {
  if (!api.keyWaiting() || event.isComposing) {
    return;
  }
  const code = debugCode(event);
  if (code == null) {
    return;
  }
  event.preventDefault();
  event.stopPropagation();
  if (!event.repeat) {
    api.postKey(code);
  }
}, true);

log.addEventListener("keydown", (event) => {
  if (event.isComposing) {
    return;
  }
  if (api.keyWaiting()) {
    event.preventDefault();
    return;
  }
  if (event.key === "ArrowUp" || event.key === "ArrowDown") {
    event.preventDefault();
    recallHistory(event.key === "ArrowUp");
    return;
  }
  if (event.key === "Enter" && !event.metaKey && !event.ctrlKey && !event.altKey) {
    event.preventDefault();
    const typed = currentTail().trim();
    const folderId = chdirFolderId(typed);
    const mounted = folderId ? mountedTopDir(folderId) : "";
    if (folderId && !mounted && typeof window.showDirectoryPicker === "function") {
      let pick;
      try {
        pick = window.showDirectoryPicker({
          id: "editforth-" + folderId,
          mode: "readwrite",
          startIn: folderId,
        });
      } catch (err) {
        pick = Promise.reject(err);
      }
      submitLine(pick);
      return;
    }
    if (mounted) {
      submitLine(null, "chdir " + mounted);
      return;
    }
    submitLine();
  }
});
log.addEventListener("beforeinput", (event) => {
  if (api.keyWaiting()) {
    event.preventDefault();
    return;
  }
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
installTreeSplitter();

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

function installTreeSplitter() {
  const split = document.querySelector("#tree-split");
  const workspace = document.querySelector("#workspace");
  const tree = document.querySelector("#tree");
  const key = "editforth.treeWidth";
  const defaultWidth = 220;
  const minTree = 140;
  const minEditor = 240;
  const splitWidth = 18;
  const narrow = window.matchMedia("(max-width: 700px)");

  function cap() {
    return Math.max(0, workspace.clientWidth - minEditor - splitWidth);
  }

  function apply(px, persist) {
    if (narrow.matches) {
      tree.style.width = "";
      return defaultWidth;
    }
    const limit = cap();
    const floor = Math.min(minTree, limit);
    const next = Math.round(Math.min(Math.max(px, floor), limit));
    tree.style.width = next + "px";
    split.setAttribute("aria-valuemin", String(Math.round(floor)));
    split.setAttribute("aria-valuemax", String(Math.round(limit)));
    split.setAttribute("aria-valuenow", String(next));
    if (persist) {
      stored = next;
      try {
        localStorage.setItem(key, String(next));
      } catch (err) {
        /* private mode can refuse storage */
      }
    }
    return next;
  }

  let stored = defaultWidth;
  try {
    const raw = Number(localStorage.getItem(key));
    if (Number.isFinite(raw) && raw > 0) {
      stored = raw;
    }
  } catch (err) {
    stored = defaultWidth;
  }
  apply(stored, false);

  let drag = null;
  split.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || narrow.matches) {
      return;
    }
    drag = { x: event.clientX, w: tree.getBoundingClientRect().width };
    split.setPointerCapture(event.pointerId);
    document.body.classList.add("resizing-x");
    event.preventDefault();
  });
  split.addEventListener("pointermove", (event) => {
    if (!drag) {
      return;
    }
    apply(drag.w + (event.clientX - drag.x), false);
  });
  function endDrag() {
    if (!drag) {
      return;
    }
    drag = null;
    document.body.classList.remove("resizing-x");
    apply(tree.getBoundingClientRect().width, true);
  }
  split.addEventListener("pointerup", endDrag);
  split.addEventListener("pointercancel", endDrag);
  split.addEventListener("lostpointercapture", endDrag);

  split.addEventListener("keydown", (event) => {
    if (narrow.matches) {
      return;
    }
    const step = event.shiftKey ? 40 : 16;
    const width = tree.getBoundingClientRect().width;
    if (event.key === "ArrowRight") {
      apply(width + step, true);
      event.preventDefault();
    } else if (event.key === "ArrowLeft") {
      apply(width - step, true);
      event.preventDefault();
    }
  });

  function refit() {
    if (narrow.matches) {
      tree.style.width = "";
      return;
    }
    apply(stored, false);
  }
  narrow.addEventListener("change", refit);
  new ResizeObserver(() => {
    const width = tree.getBoundingClientRect().width;
    if (!narrow.matches && width > cap() + 0.5) {
      apply(width, true);
    }
  }).observe(workspace);
}
