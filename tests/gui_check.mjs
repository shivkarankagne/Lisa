// SPDX-License-Identifier: BUSL-1.1
// gui_check.mjs — drives the LISA GUI in headless Chrome over the DevTools
// protocol (no npm packages; Node >= 22 for the built-in WebSocket).
// A test tool only: nothing here ships in lisa.
//
// Usage: node gui_check.mjs <url-with-#token> <docs-dir> <shots-dir> [--models]
// Env:   CHROME=/path/to/chrome (default: Google Chrome on macOS)
// Exit:  0 all checks passed, 1 a check failed, 77 Chrome not found (skip).

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [url, docsDir, shotsDir] = process.argv.slice(2);
const withModels = process.argv.includes("--models");
/* Any Chromium browser will do: they all speak the DevTools protocol. */
const CHROMIUMS = [
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
  "/usr/bin/chromium", "/usr/bin/google-chrome",
];
const chrome = process.env.CHROME || CHROMIUMS.find((p) => existsSync(p));
if (!url || !docsDir || !shotsDir) {
  console.error("usage: node gui_check.mjs <url> <docs-dir> <shots-dir> [--models]");
  process.exit(2);
}
if (!chrome || !existsSync(chrome)) {
  console.log("SKIP: no Chromium browser found (set CHROME=/path/to/browser)");
  process.exit(77);
}

let pass = 0;
let fail = 0;
const check = (name, ok, detail) => {
  if (ok) { pass++; console.log("  PASS: " + name); }
  else { fail++; console.log("  FAIL: " + name + (detail ? ": " + detail : "")); }
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/* Indexing and answering run on the CPU on a CI machine, where they take
 * tens of minutes rather than seconds. */
const MODEL_MS = Number(process.env.LISA_GUI_MODEL_MS || 900000);

// ---- start Chrome and connect ---------------------------------------------------
const profile = mkdtempSync(join(tmpdir(), "lisa-gui-chrome-"));
const proc = spawn(chrome, [
  "--headless=new", "--remote-debugging-port=0", "--user-data-dir=" + profile,
  "--no-first-run", "--no-default-browser-check", "--disable-extensions",
  "--window-size=1280,900", "about:blank",
], { stdio: "ignore" });

async function cleanup() {
  proc.kill("SIGKILL");
  await sleep(200);
  try { rmSync(profile, { recursive: true, force: true }); } catch (_) {}
}

let port = 0;
for (let i = 0; i < 100 && !port; i++) {
  await sleep(100);
  try { port = parseInt(readFileSync(join(profile, "DevToolsActivePort"), "utf8").split("\n")[0], 10); } catch (_) {}
}
if (!port) { console.log("FAIL: Chrome did not start"); await cleanup(); process.exit(1); }

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = targets.find((t) => t.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });

let seq = 0;
const pending = new Map();
const consoleErrors = [];
const requests = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) {
    const { resolve, reject } = pending.get(m.id);
    pending.delete(m.id);
    if (m.error) reject(new Error(m.error.message)); else resolve(m.result);
  } else if (m.method === "Runtime.exceptionThrown") {
    consoleErrors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  } else if (m.method === "Log.entryAdded" && m.params.entry.level === "error") {
    consoleErrors.push(m.params.entry.text);
  } else if (m.method === "Network.requestWillBeSent") {
    requests.push(m.params.request.url);
  }
};
const send = (method, params = {}) => new Promise((resolve, reject) => {
  const id = ++seq;
  pending.set(id, { resolve, reject });
  ws.send(JSON.stringify({ id, method, params }));
});
const js = async (expr) => {
  const r = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || "evaluation failed");
  return r.result.value;
};
/* Errors are retried, not raised: during a navigation the expression can
 * still run against the old document, where the elements do not exist. */
const waitFor = async (expr, ms) => {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try { if (await js(expr)) return true; } catch (_) {}
    await sleep(200);
  }
  return false;
};
const shot = async (name) => {
  const r = await send("Page.captureScreenshot", { format: "png" });
  writeFileSync(join(shotsDir, name + ".png"), Buffer.from(r.data, "base64"));
};

await send("Runtime.enable");
await send("Log.enable");
await send("Network.enable");
await send("Page.enable");

try {
  // ---- load --------------------------------------------------------------------------
  await send("Page.navigate", { url });
  await waitFor("document.readyState === 'complete' && !!document.getElementById('setup')", 10000);
  /* A fresh data directory shows the first-run folder panel. */
  check("first run offers folders to watch", await waitFor("!document.getElementById('setup').hidden", 10000));
  check("suggested folders listed",
        (await js("document.querySelectorAll('#setup-folders li').length")) > 0);
  await js(`document.getElementById('setup-extra').value = ${JSON.stringify(docsDir)};
            document.getElementById('setup-add').click(); true`);
  check("another folder can be added",
        await js(`[...document.querySelectorAll('#setup-folders label')].some(l => l.textContent === ${JSON.stringify(docsDir)})`));
  await js("document.getElementById('setup-skip').click(); true");
  check("\"Not now\" goes to the ask view",
        await waitFor("document.getElementById('setup').hidden && !document.getElementById('view-ask').hidden", 3000));

  check("page loads and shows collections",
        await waitFor("document.querySelectorAll('#collections li').length > 0 || !document.getElementById('coll-empty').hidden", 10000));
  check("token removed from the address bar", (await js("location.hash")) === "");
  check("token kept for the session", (await js("sessionStorage.getItem('lisa.token') || ''")).length >= 16);
  check("settings view hidden on the ask view",
        await js("getComputedStyle(document.getElementById('view-settings')).display === 'none'"));
  check("no error banner", await js("document.getElementById('banner').hidden"),
        await js("document.getElementById('banner').textContent"));

  // Every control has an accessible name.
  const unnamed = await js(`[...document.querySelectorAll('button, input, textarea')].filter(el => {
      if (el.closest('[hidden]')) return false;
      const lbl = el.id && document.querySelector('label[for="' + el.id + '"]');
      return !(lbl || el.getAttribute('aria-label') || el.textContent.trim());
    }).map(el => el.id || el.outerHTML.slice(0, 40))`);
  check("every visible control has a name", unnamed.length === 0, unnamed.join(", "));

  // Keyboard: Tab reaches the question box.
  let reached = false;
  for (let i = 0; i < 40 && !reached; i++) {
    await send("Input.dispatchKeyEvent", { type: "keyDown", key: "Tab", code: "Tab", windowsVirtualKeyCode: 9 });
    await send("Input.dispatchKeyEvent", { type: "keyUp", key: "Tab", code: "Tab", windowsVirtualKeyCode: 9 });
    reached = await js("document.activeElement && document.activeElement.id === 'question'");
  }
  check("Tab reaches the question box", reached);
  await send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-color-scheme", value: "light" }] });
  check("light theme follows the system",
        (await js("getComputedStyle(document.body).backgroundColor")) === "rgb(247, 247, 245)");
  await shot("1-light");

  if (withModels) {
    // ---- add documents -----------------------------------------------------------------
    await js(`document.getElementById('add-coll').value = 'guitest';
              document.getElementById('add-path').value = ${JSON.stringify(docsDir)};
              document.getElementById('add-form').requestSubmit(); true`);
    const done = await waitFor("document.getElementById('job-text').textContent.startsWith('Done')", MODEL_MS);
    check("adding a folder finishes with a summary", done, await js("document.getElementById('job-text').textContent"));
    /* The list is reloaded after the job summary appears, so this waits. */
    check("new collection listed and selected",
          await waitFor("!!document.querySelector('#collections button[data-name=\"guitest\"][aria-current=\"true\"]')", 10000));

    // ---- ask ----------------------------------------------------------------------------
    await js(`document.getElementById('question').value = 'Why did pump P-7 fail?';
              document.getElementById('ask-form').requestSubmit(); true`);
    const answered = await waitFor("document.querySelectorAll('#sources li').length > 0 || document.getElementById('answer-text').classList.contains('notfound')", MODEL_MS);
    check("answer arrives", answered, await js("document.getElementById('answer-meta').textContent"));
    const text = await js("document.getElementById('answer-text').textContent");
    check("answer is about the bearing", /bearing/i.test(text), text);
    check("answer has a clickable citation", await js("document.querySelectorAll('#answer-text button.cite').length > 0"));
    await js("document.querySelector('#answer-text button.cite').click(); true");
    check("citation opens the source passage",
          await waitFor("!document.getElementById('passage').hidden && /bearing/i.test(document.getElementById('passage-quote').textContent)", 3000));
    check("passage takes focus", await js("document.activeElement.id === 'passage'"));
    await shot("2-answer");

    await js(`document.getElementById('question').value = 'Who won the cricket world cup in 2011?';
              document.getElementById('ask-form').requestSubmit(); true`);
    check("off-topic question says not found",
          await waitFor("document.getElementById('answer-text').classList.contains('notfound')", 120000),
          await js("document.getElementById('answer-text').textContent"));
  }

  // ---- settings, theme ------------------------------------------------------------------
  await js("document.getElementById('nav-settings').click(); true");
  check("settings show the data directory",
        await waitFor("document.getElementById('data-dir').textContent.startsWith('/')", 5000));
  check("ask view hidden on the settings view",
        await js("getComputedStyle(document.getElementById('view-ask')).display === 'none'"));
  check("settings show the version", /^LISA \d+\.\d+\.\d+/.test(await js("document.getElementById('version').textContent")));
  await js("document.getElementById('theme').click(); document.getElementById('theme').click(); true");
  check("theme switches to dark", (await js("document.documentElement.dataset.theme")) === "dark");
  await shot("3-settings-dark");
  await js("document.getElementById('nav-ask').click(); true");

  // ---- 200% zoom: a 1280-px window at 2x is 640 CSS px wide --------------------------------
  await send("Emulation.setDeviceMetricsOverride", { width: 640, height: 900, deviceScaleFactor: 2, mobile: false });
  await sleep(300);
  check("question box comes first at 200% zoom",
        await js("document.getElementById('question').getBoundingClientRect().top < document.getElementById('collections').getBoundingClientRect().top"));
  check("no horizontal scrolling at 200% zoom",
        await js("document.documentElement.scrollWidth <= window.innerWidth + 1"),
        await js("document.documentElement.scrollWidth + ' > ' + window.innerWidth"));
  await shot("4-zoom200-dark");

  // ---- nothing leaves this computer -------------------------------------------------------
  const foreign = requests.filter((u) => !u.startsWith("http://127.0.0.1:") && !u.startsWith("data:") && u !== "about:blank");
  check("every request goes to the local server", foreign.length === 0, foreign.join(", "));
  check("no script errors", consoleErrors.length === 0, consoleErrors.join(" | "));
} catch (e) {
  check("run", false, e.message);
}

console.log(`GUI: ${pass} passed, ${fail} failed`);
ws.close();
await cleanup();
process.exit(fail === 0 ? 0 : 1);
