// LISA GUI. Plain JavaScript, no build step. Talks only to the local
// /v1 API (docs/http-api.md). Text from documents is only ever inserted
// with textContent, never as HTML.
"use strict";

const $ = (id) => document.getElementById(id);

// ---- token -----------------------------------------------------------------
// The server hands the session token over in the URL fragment, which the
// browser never sends anywhere. Keep it for this tab, then hide it.
let token = "";
(function takeToken() {
  const m = /[#&]token=([A-Za-z0-9_\-]+)/.exec(location.hash);
  try {
    if (m) sessionStorage.setItem("lisa.token", m[1]);
    token = (m && m[1]) || sessionStorage.getItem("lisa.token") || "";
  } catch (_) {
    token = (m && m[1]) || "";
  }
  if (m) history.replaceState(null, "", location.pathname);
})();

// ---- API -------------------------------------------------------------------
class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

async function api(method, path, body) {
  const opts = { method, headers: { Authorization: "Bearer " + token } };
  if (body !== undefined) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  let res;
  try {
    res = await fetch(path, opts);
  } catch (e) {
    throw new ApiError(0, "offline", "LISA is not running. Start it again with `lisa gui`.");
  }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    const err = data.error || {};
    throw new ApiError(res.status, err.code || "error", err.message || res.statusText);
  }
  return data;
}

function showBanner(text) {
  const b = $("banner");
  b.textContent = text;
  b.hidden = !text;
}

function friendly(e) {
  if (e.code === "unauthorized") return "This page has no valid session. Open LISA again with `lisa gui`.";
  if (e.code === "no_model") return "The models are not loaded. Check Settings, then restart LISA.";
  return e.message;
}

// ---- theme -----------------------------------------------------------------
const THEMES = ["auto", "light", "dark"];
function applyTheme(t) {
  if (t === "auto") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = t;
  const btn = $("theme");
  btn.textContent = "Theme: " + t;
  btn.setAttribute("aria-label", "Theme: " + (t === "auto" ? "automatic" : t) + ". Change theme.");
}
function loadTheme() {
  let t = "auto";
  try { t = localStorage.getItem("lisa.theme") || "auto"; } catch (_) {}
  applyTheme(THEMES.includes(t) ? t : "auto");
}
$("theme").addEventListener("click", () => {
  const cur = document.documentElement.dataset.theme || "auto";
  const next = THEMES[(THEMES.indexOf(cur) + 1) % THEMES.length];
  try { localStorage.setItem("lisa.theme", next); } catch (_) {}
  applyTheme(next);
});

// ---- views -----------------------------------------------------------------
function showView(name) {
  const ask = name === "ask";
  $("view-ask").hidden = !ask;
  $("view-settings").hidden = ask;
  $("nav-ask").setAttribute("aria-pressed", String(ask));
  $("nav-settings").setAttribute("aria-pressed", String(!ask));
  if (!ask) loadSettings();
}
$("nav-ask").addEventListener("click", () => showView("ask"));
$("nav-settings").addEventListener("click", () => showView("settings"));

// ---- collections -------------------------------------------------------------
let current = "";
let collections = [];

function selectCollection(name) {
  current = name;
  $("ask-in").textContent = name ? "in “" + name + "”" : "";
  $("add-coll").value = name || $("add-coll").value;
  for (const b of document.querySelectorAll("#collections button")) {
    b.setAttribute("aria-current", String(b.dataset.name === name));
  }
  $("ask-go").disabled = !name;
}

async function loadCollections(select) {
  const data = await api("GET", "/v1/collections");
  collections = data.collections || [];
  const ul = $("collections");
  ul.replaceChildren();
  for (const c of collections) {
    const li = document.createElement("li");
    const b = document.createElement("button");
    b.type = "button";
    b.dataset.name = c.name;
    const n = document.createElement("span");
    n.textContent = c.name;
    const count = document.createElement("span");
    count.className = "count";
    count.textContent = c.chunks === undefined ? "unavailable" : c.chunks + " passages";
    b.append(n, count);
    b.addEventListener("click", () => {
      selectCollection(c.name);
      $("question").focus();
    });
    li.append(b);
    ul.append(li);
  }
  $("coll-empty").hidden = collections.length > 0;
  const want = select || current || (collections[0] && collections[0].name) || "";
  selectCollection(collections.some((c) => c.name === want) ? want : "");
}

// ---- add documents -------------------------------------------------------------
let polling = null;

function jobText(j) {
  const s = [];
  if (j.state === "queued") return "Waiting to start…";
  s.push(j.files_seen + " files checked");
  if (j.files_added) s.push(j.files_added + " added");
  if (j.files_updated) s.push(j.files_updated + " updated");
  if (j.files_unchanged) s.push(j.files_unchanged + " unchanged");
  if (j.files_removed) s.push(j.files_removed + " removed");
  if (j.files_no_text) s.push(j.files_no_text + " without text (scanned?)");
  if (j.files_failed) s.push(j.files_failed + " could not be read");
  if (j.files_skipped) s.push(j.files_skipped + " not supported");
  return s.join(", ") + ".";
}

async function addDocuments(name, paths) {
  $("add-go").disabled = true;
  $("job").hidden = false;
  $("job-bar").removeAttribute("value");
  $("job-text").textContent = "Starting…";
  try {
    const { job } = await api("POST", "/v1/collections/" + encodeURIComponent(name) + "/ingest", { paths });
    await new Promise((resolve, reject) => {
      polling = setInterval(async () => {
        try {
          const { job: j } = await api("GET", "/v1/jobs/" + job.id);
          if (j.state === "queued" || j.state === "running") {
            $("job-text").textContent = "Indexing: " + jobText(j);
            return;
          }
          clearInterval(polling);
          $("job-bar").value = 1;
          $("job-bar").max = 1;
          if (j.state === "succeeded") {
            const readable = j.files_added + j.files_updated + j.files_unchanged;
            const unreadable = j.files_no_text + j.files_failed + j.files_skipped;
            $("job-text").textContent = "Done in " + j.elapsed_seconds.toFixed(1) + " s: " + jobText(j);
            if (readable === 0 && unreadable > 0) {
              showBanner("Nothing could be read from what you added. " +
                         "Scanned documents need text recognition, and only PDF, text and Markdown files are supported.");
            }
            resolve();
          } else {
            reject(new ApiError(0, j.state, "Indexing " + j.state + (j.error ? ": " + j.error : "")));
          }
        } catch (e) {
          clearInterval(polling);
          reject(e);
        }
      }, 500);
    });
    await loadCollections(name);
  } catch (e) {
    $("job-text").textContent = friendly(e);
  } finally {
    $("add-go").disabled = false;
  }
}

$("add-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const name = $("add-coll").value.trim();
  const path = $("add-path").value.trim();
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(name)) {
    $("job").hidden = false;
    $("job-text").textContent = "Collection names use letters, digits, - and _ (up to 64).";
    $("add-coll").focus();
    return;
  }
  if (!path.startsWith("/")) {
    $("job").hidden = false;
    $("job-text").textContent = "Give the full path, starting with /.";
    $("add-path").focus();
    return;
  }
  addDocuments(name, [path]);
});

// Native window only: a folder picker, and paths of folders dropped on the window.
if (typeof window.lisaChooseFolder === "function") {
  const choose = $("choose");
  choose.hidden = false;
  choose.addEventListener("click", async () => {
    const p = await window.lisaChooseFolder();
    if (p) $("add-path").value = p;
  });
}
window.lisaDropped = (paths) => {
  if (!Array.isArray(paths) || !paths.length) return;
  $("add-path").value = paths[0];
  const name = $("add-coll").value.trim() || current;
  if (/^[A-Za-z0-9_-]{1,64}$/.test(name)) {
    $("add-coll").value = name;
    addDocuments(name, paths);
  } else {
    $("add-coll").focus();
  }
};
// In a browser, dropping files gives no path; say so instead of failing silently.
document.addEventListener("dragover", (ev) => { ev.preventDefault(); document.body.classList.add("drop-active"); });
document.addEventListener("dragleave", () => document.body.classList.remove("drop-active"));
document.addEventListener("drop", (ev) => {
  ev.preventDefault();
  document.body.classList.remove("drop-active");
  if (typeof window.lisaChooseFolder !== "function") {
    $("job").hidden = false;
    $("job-text").textContent = "Browsers do not reveal where a dropped folder is. Type or paste its path instead.";
  }
});

// ---- ask -------------------------------------------------------------------------
let lastCitations = [];

function renderAnswerText(el, text, citations) {
  el.replaceChildren();
  const byNum = new Map(citations.map((c) => [c.number, c]));
  const re = /\[(\d+(?:\s*,\s*\d+)*)\]/g;
  let last = 0;
  let m;
  while ((m = re.exec(text)) !== null) {
    el.append(document.createTextNode(text.slice(last, m.index)));
    for (const n of m[1].split(",").map((x) => parseInt(x, 10))) {
      const c = byNum.get(n);
      if (!c) {
        el.append(document.createTextNode("[" + n + "]"));
        continue;
      }
      const b = document.createElement("button");
      b.type = "button";
      b.className = "cite";
      b.textContent = "[" + n + "]";
      b.setAttribute("aria-label", "Source " + n + ": " + sourceName(c));
      b.addEventListener("click", () => showPassage(c));
      el.append(b);
    }
    last = re.lastIndex;
  }
  el.append(document.createTextNode(text.slice(last)));
}

function sourceName(c) {
  const file = c.path.split("/").pop();
  const name = c.title ? c.title + " (" + file + ")" : file;
  return c.page > 0 ? name + ", page " + c.page : name;
}

function showPassage(c) {
  $("passage-where").textContent = sourceName(c) + " — " + c.path;
  $("passage-quote").textContent = c.quote;
  const p = $("passage");
  p.hidden = false;
  p.focus();
}
$("passage-close").addEventListener("click", () => {
  $("passage").hidden = true;
  $("question").focus();
});
document.addEventListener("keydown", (ev) => {
  if (ev.key === "Escape" && !$("passage").hidden) {
    $("passage").hidden = true;
    $("question").focus();
  }
});

function renderAnswer(a) {
  const t = $("answer-text");
  t.classList.toggle("notfound", !a.found);
  renderAnswerText(t, a.text, a.citations || []);
  t.setAttribute("aria-busy", "false");
  lastCitations = a.citations || [];
  const src = $("sources");
  src.replaceChildren();
  for (const c of lastCitations) {
    const li = document.createElement("li");
    li.value = c.number;
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = sourceName(c);
    b.addEventListener("click", () => showPassage(c));
    li.append(b);
    src.append(li);
  }
  $("sources-heading").hidden = lastCitations.length === 0;
  const meta = [];
  if (a.found) meta.push(a.passages_used + " passage" + (a.passages_used === 1 ? "" : "s") + " used");
  meta.push(a.total_seconds.toFixed(1) + " s");
  if (!a.complete) meta.push("stopped early");
  $("answer-meta").textContent = meta.join(" · ");
}

// Read a Server-Sent Events response from fetch (EventSource cannot POST).
async function readEvents(res, onEvent) {
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let i;
    while ((i = buf.indexOf("\n\n")) >= 0) {
      const block = buf.slice(0, i);
      buf = buf.slice(i + 2);
      let event = "message";
      let data = "";
      for (const line of block.split("\n")) {
        if (line.startsWith("event: ")) event = line.slice(7);
        else if (line.startsWith("data: ")) data += line.slice(6);
      }
      onEvent(event, data ? JSON.parse(data) : null);
    }
  }
}

function emptyNotice(name) {
  return "\u201c" + name + "\u201d has no readable text yet, so there is nothing to answer from. " +
         "Add documents on the left. Scanned pages and unsupported files are skipped.";
}

async function ask(question) {
  const coll = collections.find((c) => c.name === current);
  if (coll && coll.chunks === 0) {
    $("answer").hidden = false;
    $("answer-text").classList.add("notfound");
    $("answer-text").textContent = emptyNotice(current);
    $("answer-meta").textContent = "";
    $("sources").replaceChildren();
    $("sources-heading").hidden = true;
    return;
  }
  $("ask-go").disabled = true;
  $("passage").hidden = true;
  $("answer").hidden = false;
  const t = $("answer-text");
  t.classList.remove("notfound");
  t.textContent = "";
  t.setAttribute("aria-busy", "true");
  $("answer-meta").textContent = "Searching your documents…";
  $("sources").replaceChildren();
  $("sources-heading").hidden = true;
  let text = "";
  try {
    const res = await fetch("/v1/collections/" + encodeURIComponent(current) + "/ask", {
      method: "POST",
      headers: { Authorization: "Bearer " + token, "Content-Type": "application/json" },
      body: JSON.stringify({ stream: true, messages: [{ role: "user", content: question }] }),
    });
    if (!res.ok) {
      const data = await res.json().catch(() => ({}));
      const err = data.error || {};
      throw new ApiError(res.status, err.code || "error", err.message || res.statusText);
    }
    let finished = false;
    await readEvents(res, (event, data) => {
      if (event === "token") {
        text += data.text;
        t.textContent = text;
        $("answer-meta").textContent = "Writing…";
      } else if (event === "answer") {
        finished = true;
        renderAnswer(data);
      } else if (event === "error") {
        throw new ApiError(0, "error", data.message);
      }
    });
    if (!finished) throw new ApiError(0, "error", "The answer was cut off. Try again.");
  } catch (e) {
    t.setAttribute("aria-busy", "false");
    $("answer-meta").textContent = friendly(e);
  } finally {
    $("ask-go").disabled = !current;
  }
}

$("ask-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const q = $("question").value.trim();
  if (q && current) ask(q);
});
$("question").addEventListener("keydown", (ev) => {
  if (ev.key === "Enter" && !ev.shiftKey && !ev.isComposing) {
    ev.preventDefault();
    $("ask-form").requestSubmit();
  }
});

// ---- first run: which folders to keep indexed -----------------------------------------
const WATCH_COLLECTION = "my-documents";
let setupFolders = [];   // { path, checked }

function renderSetupFolders() {
  const ul = $("setup-folders");
  ul.replaceChildren();
  setupFolders.forEach((f, i) => {
    const li = document.createElement("li");
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = f.checked;
    box.id = "wf" + i;
    box.addEventListener("change", () => { f.checked = box.checked; });
    const label = document.createElement("label");
    label.htmlFor = box.id;
    label.textContent = f.path;
    li.append(box, label);
    ul.append(li);
  });
}

function addSetupFolder(path) {
  path = (path || "").trim();
  if (!path.startsWith("/")) {
    $("setup-msg").textContent = "Give the full path of a folder, starting with /.";
    return;
  }
  if (!setupFolders.some((f) => f.path === path)) setupFolders.push({ path, checked: true });
  $("setup-extra").value = "";
  $("setup-msg").textContent = "";
  renderSetupFolders();
}

$("setup-add").addEventListener("click", () => addSetupFolder($("setup-extra").value));
$("setup-extra").addEventListener("keydown", (ev) => {
  if (ev.key === "Enter") { ev.preventDefault(); addSetupFolder($("setup-extra").value); }
});
if (typeof window.lisaChooseFolder === "function") {
  const b = $("setup-browse");
  b.hidden = false;
  b.addEventListener("click", async () => {
    const p = await window.lisaChooseFolder();
    if (p) addSetupFolder(p);
  });
}
$("setup-skip").addEventListener("click", () => {
  $("setup").hidden = true;
  showView("ask");
});
$("setup-go").addEventListener("click", async () => {
  const chosen = setupFolders.filter((f) => f.checked).map((f) => f.path);
  if (chosen.length === 0) {
    $("setup-msg").textContent = "Tick at least one folder, or choose Not now.";
    return;
  }
  $("setup-go").disabled = true;
  $("setup-msg").textContent = "Starting\u2026";
  try {
    await api("POST", "/v1/settings", { watch: { collection: WATCH_COLLECTION, folders: chosen } });
    $("setup").hidden = true;
    showView("ask");
    $("job").hidden = false;
    $("job-bar").removeAttribute("value");
    $("job-text").textContent = "Reading your folders\u2026 you can ask questions as they arrive.";
    watchProgress();
    await loadCollections(WATCH_COLLECTION);
  } catch (e) {
    $("setup-msg").textContent = friendly(e);
  } finally {
    $("setup-go").disabled = false;
  }
});

/* While the watcher indexes, show what it has done and refresh the list. */
async function watchProgress() {
  for (let i = 0; i < 600; i++) {
    await new Promise((r) => setTimeout(r, 2000));
    let list;
    try {
      list = await api("GET", "/v1/jobs");
    } catch (_) {
      return;
    }
    const j = (list.jobs || [])[0];
    if (!j) return;
    if (j.state === "running" || j.state === "queued") {
      $("job-text").textContent = "Indexing: " + jobText(j);
    } else {
      $("job-text").textContent = "Ready: " + jobText(j);
      $("job-bar").value = 1;
      $("job-bar").max = 1;
      await loadCollections(current || WATCH_COLLECTION);
      return;
    }
  }
}

// ---- settings -----------------------------------------------------------------------
function modelStatus(m) {
  if (!m.path) return "No file found. Enter the path of a model file.";
  return (m.loaded ? "Loaded" : "Not loaded") + (m.source === "config" ? " · set in settings" : " · found automatically");
}

async function loadSettings() {
  try {
    const s = await api("GET", "/v1/settings");
    $("data-dir").textContent = s.data_dir;
    $("chat-model").value = s.models.chat.path || "";
    $("embed-model").value = s.models.embedding.path || "";
    $("chat-status").textContent = modelStatus(s.models.chat);
    $("embed-status").textContent = modelStatus(s.models.embedding);
    $("version").textContent = "LISA " + s.version;
  } catch (e) {
    showBanner(friendly(e));
  }
}

$("models-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const body = {};
  const c = $("chat-model").value.trim();
  const e = $("embed-model").value.trim();
  if (c) body.chat_model = c;
  if (e) body.embedding_model = e;
  try {
    await api("POST", "/v1/settings", body);
    $("models-msg").textContent = "Saved. Restart LISA to use the new models.";
  } catch (err) {
    $("models-msg").textContent = friendly(err);
  }
});

// ---- start -------------------------------------------------------------------------
async function start() {
  loadTheme();
  selectCollection("");
  try {
    const h = await api("GET", "/v1/health");
    if (!h.models.chat || !h.models.embedding) {
      showBanner("A model is missing, so asking questions will not work yet. Open Settings to choose model files.");
    }
    await loadCollections();

    /* Nothing watched and nothing indexed yet: offer the folders once. */
    const s = await api("GET", "/v1/settings");
    if (!s.watch.collection && collections.length === 0) {
      setupFolders = (s.watch.suggested || []).map((path) => ({ path, checked: true }));
      renderSetupFolders();
      $("setup").hidden = false;
      $("view-ask").hidden = true;
    } else if (s.watch.collection) {
      watchProgress();
    }
  } catch (e) {
    showBanner(friendly(e));
  }
}
start();
