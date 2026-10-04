// Crimson Weather community admin (browser script).
//
// Security note: preset titles, descriptions and INI text come from players.
// They are only ever inserted with textContent (via el()), never as HTML.

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
const $ = (selector, root = document) => root.querySelector(selector);

// el("div", { class: "x", text: "hi", onClick: fn, data: { id: 1 }, attrs: { href } }, child, ...)
function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === "class") node.className = value;
    else if (key === "text") node.textContent = String(value);
    else if (key === "data") Object.assign(node.dataset, value);
    else if (key === "attrs") for (const [a, v] of Object.entries(value)) node.setAttribute(a, v);
    else if (key.startsWith("on")) node.addEventListener(key.slice(2).toLowerCase(), value);
    else node[key] = value;
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

async function api(method, path, body, { raw = false } = {}) {
  const init = { method, headers: {} };
  if (body instanceof ArrayBuffer || body instanceof Blob) {
    init.body = body;
    init.headers["content-type"] = "application/octet-stream";
  } else if (body !== undefined) {
    init.body = JSON.stringify(body);
    init.headers["content-type"] = "application/json";
  }
  const res = await fetch(path, init);
  if (raw) return res;
  const data = await res.json().catch(() => ({ ok: false, error: `HTTP ${res.status}` }));
  if (!res.ok || data.ok === false) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

function toast(message, isError = false) {
  const node = el("div", { class: isError ? "toast error" : "toast", text: message });
  $("#toasts").append(node);
  setTimeout(() => node.remove(), isError ? 7000 : 3500);
}

// Runs an action, reporting success/failure; returns true on success.
async function attempt(fn, success) {
  try {
    await fn();
    if (success) toast(success);
    return true;
  } catch (error) {
    toast(error.message || String(error), true);
    return false;
  }
}

const fmt = {
  num: (n) => Number(n || 0).toLocaleString(),
  bytes: (n) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n || 0} B`),
  date: (iso) => (iso ? new Date(iso).toLocaleString() : ""),
  ago(iso) {
    if (!iso) return "";
    const s = Math.round((Date.now() - new Date(iso).getTime()) / 1000);
    const future = s < 0;
    const a = Math.abs(s);
    const text = a < 60 ? `${a}s` : a < 3600 ? `${Math.round(a / 60)}m` : a < 86400 ? `${Math.round(a / 3600)}h` : `${Math.round(a / 86400)}d`;
    return future ? `in ${text}` : `${text} ago`;
  },
};

const isTyping = (event) => /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName);
const debounce = (fn, ms) => {
  let timer;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
};

// ---------------------------------------------------------------------------
// INI rendering and diff
// ---------------------------------------------------------------------------
function iniLine(text) {
  const trimmed = text.trim();
  if (trimmed.startsWith(";") || trimmed.startsWith("#")) return el("span", { class: "cmt", text });
  if (trimmed.startsWith("[") && trimmed.endsWith("]")) return el("span", { class: "sec", text });
  const eq = text.indexOf("=");
  if (eq < 0) return el("span", { text });
  return el("span", {}, el("span", { class: "key", text: text.slice(0, eq) }), "=", el("span", { class: "val", text: text.slice(eq + 1) }));
}

const splitLines = (text) => String(text || "").replace(/\r\n?/g, "\n").split("\n");

function renderIni(text) {
  return el("div", { class: "code" },
    splitLines(text).map((line, i) => el("div", { class: "ln" }, el("span", { class: "no", text: i + 1 }), el("span", { class: "src" }, iniLine(line)))));
}

// Above this many cells (lines before x lines after, once the common start and
// end are set aside) the diff is not computed: a crafted file could otherwise
// make the page allocate gigabytes. Real preset edits stay far below it.
const DIFF_CELL_BUDGET = 2_000_000;

// Line diff (longest common subsequence) of the part between the common first
// and last lines. Returns null when that part is too big to compare.
function diffLines(before, after) {
  const fullA = splitLines(before);
  const fullB = splitLines(after);
  let head = 0;
  while (head < fullA.length && head < fullB.length && fullA[head] === fullB[head]) head++;
  let tail = 0;
  while (tail < fullA.length - head && tail < fullB.length - head &&
         fullA[fullA.length - 1 - tail] === fullB[fullB.length - 1 - tail]) tail++;
  const a = fullA.slice(head, fullA.length - tail);
  const b = fullB.slice(head, fullB.length - tail);
  if ((a.length + 1) * (b.length + 1) > DIFF_CELL_BUDGET) return null;
  const same = (text, no) => ({ type: "same", text, no });
  const ops = fullB.slice(0, head).map((text, k) => same(text, k + 1));

  const dp = Array.from({ length: a.length + 1 }, () => new Uint16Array(b.length + 1));
  for (let i = a.length - 1; i >= 0; i--) {
    for (let j = b.length - 1; j >= 0; j--) {
      dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
    }
  }
  let i = 0;
  let j = 0;
  while (i < a.length || j < b.length) {
    if (i < a.length && j < b.length && a[i] === b[j]) {
      ops.push(same(b[j], head + j + 1));
      i++;
      j++;
    } else if (i < a.length && (j >= b.length || dp[i + 1][j] >= dp[i][j + 1])) {
      ops.push({ type: "del", text: a[i], no: "" }); // removed lines first, like a normal diff
      i++;
    } else {
      ops.push({ type: "add", text: b[j], no: head + j + 1 });
      j++;
    }
  }
  fullB.slice(fullB.length - tail).forEach((text, k) => ops.push(same(text, fullB.length - tail + k + 1)));
  return ops;
}

function renderDiff(before, after) {
  const ops = diffLines(before, after);
  if (!ops) {
    return el("div", {},
      el("div", { class: "muted", text: "Too many changed lines to compare. The full new version is shown instead." }),
      renderIni(after));
  }
  const changed = ops.filter((op) => op.type !== "same").length;
  return el("div", {},
    el("div", { class: "muted", text: changed ? `${changed} changed line(s) compared with the live version` : "No changes to the preset content" }),
    el("div", { class: "code" }, ops.map((op) =>
      el("div", { class: op.type === "same" ? "ln" : `ln ${op.type}` }, el("span", { class: "no", text: op.no }), el("span", { class: "src" }, iniLine(op.text))))));
}

// ---------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------
const state = { counts: {}, actor: "" };

function statusBadge(p) {
  if (p.deleted_at) return el("span", { class: "badge bad", text: "in trash" });
  if (p.status === "approved") return el("span", { class: "badge ok", text: "live" });
  if (p.status === "rejected") return el("span", { class: "badge bad", text: "rejected" });
  return el("span", { class: "badge warn", text: p.update_of ? "update pending" : "pending" });
}

async function refreshCounts() {
  try {
    const { counts } = await api("GET", "/api/admin/overview");
    state.counts = counts;
    $("#count-pending").textContent = counts.pending ? String(counts.pending) : "";
    $("#count-approved").textContent = String(counts.approved);
    $("#count-trash").textContent = counts.trash ? String(counts.trash) : "";
    $("#count-trusted").textContent = counts.trusted ? String(counts.trusted) : "";
    $("#stat-downloads").textContent = fmt.num(counts.downloads);
    $("#stat-likes").textContent = fmt.num(counts.likes);
  } catch (error) {
    toast(error.message, true);
  }
}

// Performs a moderation action on one preset.
async function presetAction(id, action, body, message) {
  const ok = await attempt(() => api("POST", `/api/admin/presets/${encodeURIComponent(id)}/${action}`, body), message);
  if (ok) refreshCounts();
  return ok;
}

// Full detail panel for one preset with the actions its state allows.
// `onDone` is called after an action succeeds.
function renderDetail(detail, onDone) {
  const p = detail.preset;
  const root = el("div", { class: "card" });
  const rejectBox = el("div", { class: "reject-box", hidden: true },
    el("textarea", { rows: 2, placeholder: "Note for the log (optional)", attrs: { "aria-label": "Reject note" } }),
    el("div", { class: "actions" },
      el("button", { class: "btn danger", text: "Reject", onClick: async () => {
        if (await presetAction(p.id, "reject", { note: $("textarea", rejectBox).value }, `Rejected "${p.title}"`)) onDone();
      } }),
      el("button", { class: "btn", text: "Cancel", onClick: () => { rejectBox.hidden = true; } })));

  const actions = el("div", { class: "actions" });
  const add = (label, cls, handler, key) => actions.append(el("button", { class: `btn ${cls}`, onClick: handler }, label, key ? el("kbd", { text: key }) : null));
  const run = (action, body, message) => async () => {
    if (await presetAction(p.id, action, body, message)) onDone();
  };

  if (p.deleted_at) {
    add("Restore", "primary", run("restore", {}, `Restored "${p.title}"`));
    add("Delete permanently", "danger", async () => {
      if (confirm(`Permanently delete "${p.title}"? This removes it and its file now.`)) await run("purge", {}, `Purged "${p.title}"`)();
    });
  } else if (p.status === "pending") {
    // The hash ties the approval to exactly the content shown here.
    const approveBody = { sha256: p.sha256 };
    add(p.update_of ? "Approve update " : "Approve ", "primary", run("approve", approveBody, `Approved "${p.title}"`), "A");
    add("Reject ", "danger", () => { rejectBox.hidden = false; $("textarea", rejectBox).focus(); }, "R");
    if (!p.trusted_label) {
      add("Trust uploader & approve", "", async () => {
        if (await presetAction(p.id, "trust", {}, "Uploader trusted: their future uploads are approved automatically")) {
          await run("approve", approveBody, `Approved "${p.title}"`)();
        }
      });
    }
    add("Delete", "danger", run("delete", { reason: "admin" }, `Moved "${p.title}" to trash`));
  } else {
    if (p.status === "approved" && !p.update_of && !p.trusted_label) {
      add("Trust uploader", "", run("trust", {}, "Uploader trusted"));
    }
    add(p.status === "approved" ? "Hide (move to trash)" : "Delete", "danger", async () => {
      if (confirm(`Move "${p.title}" to trash? Players stop seeing it now; it is deleted after 7 days.`)) {
        await run("delete", { reason: "admin" }, `Moved "${p.title}" to trash`)();
      }
    });
  }

  const chips = el("div", { class: "chips" },
    statusBadge(p),
    p.update_of ? el("span", { class: "badge info", text: `update to "${p.target_title || p.update_of}"` }) : null,
    p.trusted_label ? el("span", { class: "badge ok", text: `trusted: ${p.trusted_label}` }) : null,
    el("span", { class: "badge", text: `format v${p.format_version}` }),
    el("span", { class: "badge", text: fmt.bytes(p.size_bytes) }),
    el("span", { class: "badge", text: `addon ${p.min_addon_version}` }),
    el("span", { class: "badge", text: `${fmt.num(p.downloads)} downloads` }),
    el("span", { class: "badge", text: `${fmt.num(p.likes)} likes` }),
    el("span", { class: "badge mono", text: `uploader ${p.client_fingerprint || "unknown"}`, title: p.submitter_hash }));

  const scan = detail.scan && !detail.scan.ok
    ? el("ul", { class: "scan-errors" }, detail.scan.errors.map((e) => el("li", { text: e })))
    : null;

  // Content: for updates, a diff against the live version, plus the full text.
  const content = el("div");
  const showFull = () => content.replaceChildren(renderIni(detail.iniText));
  const showDiff = () => content.replaceChildren(renderDiff(detail.current?.iniText || "", detail.iniText));
  let tabs = null;
  if (detail.current) {
    const diffBtn = el("button", { class: "btn small active", text: "Changes" });
    const fullBtn = el("button", { class: "btn small", text: "Full preset" });
    diffBtn.addEventListener("click", () => { showDiff(); diffBtn.classList.add("active"); fullBtn.classList.remove("active"); });
    fullBtn.addEventListener("click", () => { showFull(); fullBtn.classList.add("active"); diffBtn.classList.remove("active"); });
    tabs = el("div", { class: "tabs" }, diffBtn, fullBtn);
    showDiff();
  } else {
    showFull();
  }

  const parts = [
    el("div", { class: "detail-head" },
      el("div", {},
        el("h1", { text: p.title }),
        el("div", { class: "muted" }, `by ${p.author_name} - submitted ${fmt.ago(p.created_at)}`,
          p.deleted_at ? ` - deleted ${fmt.ago(p.deleted_at)} (${p.delete_reason || "no reason"}), purged ${fmt.ago(p.delete_after)}` : "")),
      el("span", { class: "muted mono", text: p.id })),
    chips,
    p.description ? el("p", { class: "description", text: p.description }) : null,
    scan,
    actions,
    rejectBox,
    tabs,
    content,
  ];
  root.append(...parts.filter(Boolean)); // absent sections are null
  root.approve = actions.querySelector(".btn.primary");
  root.reject = () => { if (p.status === "pending" && !p.deleted_at) { rejectBox.hidden = false; $("textarea", rejectBox).focus(); } };
  return root;
}

function openDrawer(id, onChange) {
  const backdrop = el("div", { class: "drawer-backdrop" });
  const drawer = el("aside", { class: "drawer", attrs: { role: "dialog", "aria-modal": "true" } }, el("div", { class: "muted", text: "Loading..." }));
  const close = () => { backdrop.remove(); drawer.remove(); document.removeEventListener("keydown", onKey); };
  const onKey = (e) => { if (e.key === "Escape") close(); };
  backdrop.addEventListener("click", close);
  document.addEventListener("keydown", onKey);
  document.body.append(backdrop, drawer);
  api("GET", `/api/admin/presets/${encodeURIComponent(id)}`).then((detail) => {
    drawer.replaceChildren(
      el("div", { class: "toolbar" }, el("span", { class: "grow" }), el("button", { class: "btn small", text: "Close", onClick: close })),
      renderDetail(detail, () => { close(); onChange(); }));
  }).catch((error) => { close(); toast(error.message, true); });
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
const views = {};
let cleanup = null; // per-view teardown (keyboard handlers)
let routeNumber = 0; // changes on navigation; a view that finishes loading after it was left does nothing

// Review queue: pending submissions and owner updates, oldest first.
views.review = async (main) => {
  const opened = routeNumber;
  const { presets } = await api("GET", "/api/admin/presets?status=pending&sort=created&limit=300");
  if (opened !== routeNumber) return;
  const selected = new Set();
  let current = presets[0]?.id || null;
  let detailNode = null;

  const list = el("div", { class: "queue" });
  const detailPane = el("div");
  const bulkbar = el("div", { class: "bulkbar", hidden: true });

  // Re-renders the view; this instance's keyboard handler is removed first.
  const reload = async () => {
    if (cleanup) cleanup();
    cleanup = null;
    await refreshCounts();
    await views.review(main);
  };

  function drawList() {
    list.replaceChildren(...(presets.length ? presets.map((p) => {
      const box = el("input", { type: "checkbox", checked: selected.has(p.id), attrs: { "aria-label": `Select ${p.title}` },
        onClick: (e) => { e.stopPropagation(); e.target.checked ? selected.add(p.id) : selected.delete(p.id); drawBulk(); } });
      return el("div", { class: p.id === current ? "queue-item selected" : "queue-item", onClick: () => select(p.id) },
        box,
        el("div", {},
          el("div", { class: "title", text: p.title }),
          el("div", { class: "meta" },
            p.update_of ? el("span", { class: "badge info", text: "update" }) : el("span", { class: "badge", text: "new" }),
            p.trusted_label ? el("span", { class: "badge ok", text: "trusted" }) : null,
            el("span", { text: p.author_name }),
            el("span", { text: fmt.ago(p.created_at) }))));
    }) : [el("div", { class: "empty", text: "Nothing to review. Nice." })]));
  }

  function drawBulk() {
    bulkbar.hidden = selected.size === 0;
    const bulk = (action, label) => el("button", { class: action === "approve" ? "btn primary" : "btn danger", text: label, onClick: async () => {
      if (action !== "approve" && !confirm(`${label} ${selected.size} preset(s)?`)) return;
      const ids = [...selected];
      const hashes = Object.fromEntries(presets.filter((p) => selected.has(p.id)).map((p) => [p.id, p.sha256]));
      const { results } = await api("POST", "/api/admin/presets/bulk", { action, ids, hashes, note: "bulk", reason: "admin" });
      const failed = results.filter((r) => !r.ok);
      toast(failed.length ? `${results.length - failed.length} done, ${failed.length} failed: ${failed[0].error}` : `${results.length} preset(s): ${action} done`, failed.length > 0);
      await reload();
    } });
    bulkbar.replaceChildren(el("span", { class: "grow", text: `${selected.size} selected` }), bulk("approve", "Approve"), bulk("reject", "Reject"), bulk("delete", "Delete"));
  }

  async function select(id) {
    current = id;
    detailNode = null; // shortcuts must not act on the previous preset while this one loads
    drawList();
    if (!id) {
      detailPane.replaceChildren();
      return;
    }
    detailPane.replaceChildren(el("div", { class: "muted", text: "Loading..." }));
    try {
      const detail = await api("GET", `/api/admin/presets/${encodeURIComponent(id)}`);
      if (current !== id) return;
      detailNode = renderDetail(detail, reload);
      detailPane.replaceChildren(detailNode);
    } catch (error) {
      detailPane.replaceChildren(el("div", { class: "card", text: error.message }));
    }
  }

  function move(delta) {
    const index = presets.findIndex((p) => p.id === current);
    const next = presets[Math.min(presets.length - 1, Math.max(0, index + delta))];
    if (next) select(next.id);
  }

  const onKey = (e) => {
    if (isTyping(e) || e.ctrlKey || e.metaKey || e.altKey || document.querySelector(".drawer")) return;
    if (e.key === "j" || e.key === "ArrowDown") { e.preventDefault(); move(1); }
    else if (e.key === "k" || e.key === "ArrowUp") { e.preventDefault(); move(-1); }
    else if (e.key === "a" && detailNode?.approve) detailNode.approve.click();
    else if (e.key === "r" && detailNode) { e.preventDefault(); detailNode.reject(); }
    else if (e.key === "x" && current) { selected.has(current) ? selected.delete(current) : selected.add(current); drawList(); drawBulk(); }
  };
  document.addEventListener("keydown", onKey);
  cleanup = () => document.removeEventListener("keydown", onKey);

  main.replaceChildren(
    el("h1", { text: "Review" }),
    el("p", { class: "subtitle" }, `${presets.length} waiting, oldest first. `,
      el("kbd", { text: "J" }), " ", el("kbd", { text: "K" }), " move, ", el("kbd", { text: "A" }), " approve, ",
      el("kbd", { text: "R" }), " reject, ", el("kbd", { text: "X" }), " select"),
    el("div", { class: "split" }, el("div", {}, list, bulkbar), detailPane));
  drawList();
  drawBulk();
  await select(current);
};

// All presets with search, status filter and sort.
views.presets = async (main) => {
  const q = el("input", { type: "search", placeholder: "Search title, author, id or uploader hash", class: "grow", attrs: { "aria-label": "Search" } });
  const status = el("select", { attrs: { "aria-label": "Status" } },
    ["approved:Live", "pending:Pending", "rejected:Rejected", ":All"].map((o) => { const [v, l] = o.split(":"); return el("option", { value: v, text: l }); }));
  const sort = el("select", { attrs: { "aria-label": "Sort" } },
    ["updated:Recently changed", "downloads:Most downloaded", "likes:Most liked", "created:Oldest first"].map((o) => { const [v, l] = o.split(":"); return el("option", { value: v, text: l }); }));
  const body = el("tbody");

  async function load() {
    const params = new URLSearchParams({ q: q.value, status: status.value, sort: sort.value, limit: "300" });
    const { presets } = await api("GET", `/api/admin/presets?${params}`);
    body.replaceChildren(...(presets.length ? presets.map((p) => el("tr", { class: "clickable", onClick: () => openDrawer(p.id, load) },
      el("td", {}, el("div", { text: p.title }), el("div", { class: "muted mono", text: p.id })),
      el("td", { text: p.author_name }),
      el("td", {}, statusBadge(p), p.trusted_label ? " " : null, p.trusted_label ? el("span", { class: "badge ok", text: "trusted" }) : null),
      el("td", { class: "num", text: fmt.num(p.downloads) }),
      el("td", { class: "num", text: fmt.num(p.likes) }),
      el("td", { class: "muted", text: fmt.ago(p.updated_at), title: fmt.date(p.updated_at) }))) : [el("tr", {}, el("td", { colSpan: 6, class: "empty", text: "No presets match." }))]));
  }

  q.addEventListener("input", debounce(load, 250));
  status.addEventListener("change", load);
  sort.addEventListener("change", load);
  main.replaceChildren(
    el("h1", { text: "Presets" }),
    el("p", { class: "subtitle", text: "Everything players uploaded. Click a preset for details and actions." }),
    el("div", { class: "toolbar" }, q, status, sort),
    el("table", {}, el("thead", {}, el("tr", {}, ["Preset", "Author", "Status", "Downloads", "Likes", "Changed"].map((h) => el("th", { text: h })))), body));
  await load();
};

// Soft-deleted presets, restorable until they are purged.
views.trash = async (main) => {
  const { presets } = await api("GET", "/api/admin/presets?view=trash&sort=expiry&limit=300");
  const reload = async () => { await refreshCounts(); await views.trash(main); };
  main.replaceChildren(
    el("h1", { text: "Trash" }),
    el("p", { class: "subtitle", text: "Deleted presets stay here for 7 days, then are removed automatically. Players cannot see them." }),
    el("table", {},
      el("thead", {}, el("tr", {}, ["Preset", "Deleted", "Reason", "Purged", ""].map((h) => el("th", { text: h })))),
      el("tbody", {}, presets.length ? presets.map((p) => el("tr", {},
        el("td", {}, el("div", { text: p.title }), el("div", { class: "muted", text: `by ${p.author_name}` })),
        el("td", { text: fmt.ago(p.deleted_at), title: `${fmt.date(p.deleted_at)} by ${p.deleted_by}` }),
        el("td", { text: p.delete_reason || "" }),
        el("td", { text: fmt.ago(p.delete_after), title: fmt.date(p.delete_after) }),
        el("td", { class: "actions" },
          el("button", { class: "btn small", text: "View", onClick: () => openDrawer(p.id, reload) }),
          el("button", { class: "btn small", text: "Restore", onClick: async () => { if (await presetAction(p.id, "restore", {}, `Restored "${p.title}"`)) reload(); } })))) :
        el("tr", {}, el("td", { colSpan: 5, class: "empty", text: "Trash is empty." })))));
};

// Uploaders whose presets are approved automatically.
views.trusted = async (main) => {
  const { trusted } = await api("GET", "/api/admin/trusted");
  const reload = async () => { await refreshCounts(); await views.trusted(main); };
  const idInput = el("input", { type: "text", class: "grow", placeholder: "Uploader hash (64 hex) or client id", attrs: { "aria-label": "Uploader" } });
  const labelInput = el("input", { type: "text", placeholder: "Label (e.g. their name)", attrs: { "aria-label": "Label" } });
  const add = el("button", { class: "btn primary", text: "Trust", onClick: async () => {
    const value = idInput.value.trim();
    const body = /^[a-f0-9]{64}$/i.test(value) ? { submitterHash: value, label: labelInput.value } : { clientId: value, label: labelInput.value };
    if (value && await attempt(() => api("POST", "/api/admin/trusted", body), "Uploader trusted")) reload();
  } });
  main.replaceChildren(
    el("h1", { text: "Trusted uploaders" }),
    el("p", { class: "subtitle", text: "Uploads from these players go live without review. Trust someone from a preset's detail panel, or add them here." }),
    el("div", { class: "toolbar" }, idInput, labelInput, add),
    el("table", {},
      el("thead", {}, el("tr", {}, ["Uploader", "Presets", "Trusted since", ""].map((h) => el("th", { text: h })))),
      el("tbody", {}, trusted.length ? trusted.map((t) => el("tr", {},
        el("td", {}, el("div", { text: t.label || "(no label)" }), el("div", { class: "muted mono", text: t.client_fingerprint, title: t.submitter_hash }),
          t.note ? el("div", { class: "muted", text: t.note }) : null),
        el("td", { class: "num", text: fmt.num(t.presets) }),
        el("td", { text: fmt.ago(t.created_at), title: fmt.date(t.created_at) }),
        el("td", { class: "actions" }, el("button", { class: "btn small danger", text: "Remove", onClick: async () => {
          if (confirm(`Stop trusting ${t.label || t.client_fingerprint}? Their future uploads will need review.`) &&
            await attempt(() => api("DELETE", `/api/admin/trusted/${t.submitter_hash}`), "Trust removed")) reload();
        } })))) :
        el("tr", {}, el("td", { colSpan: 4, class: "empty", text: "No trusted uploaders yet." })))));
};

// What the in-game updater offers.
views.release = async (main) => {
  const { release: r } = await api("GET", "/api/admin/release");
  const version = el("input", { type: "text", value: r.latestVersion, attrs: { "aria-label": "Version", pattern: "\\d+(\\.\\d+){1,3}" } });
  const url = el("input", { type: "url", value: r.downloadPageUrl, attrs: { "aria-label": "Download page" } });
  const published = el("input", { type: "text", value: r.publishedAt, placeholder: "e.g. 2026-09-20", attrs: { "aria-label": "Published" } });
  const critical = el("input", { type: "checkbox", checked: r.critical });
  const changelog = el("textarea", { rows: 14, value: r.changelog, attrs: { "aria-label": "Changelog" } });
  const file = el("input", { type: "file", accept: ".addon64", attrs: { "aria-label": "Addon file" } });

  const save = el("button", { class: "btn primary", text: "Save update notice", onClick: async () => {
    if (version.value.trim() !== r.latestVersion && r.addonR2Key &&
      !confirm(`Changing the version to ${version.value} removes the ${r.latestVersion} addon file from the updater until you upload the new one. Continue?`)) return;
    if (await attempt(() => api("PUT", "/api/admin/release", {
      latestVersion: version.value, downloadPageUrl: url.value, publishedAt: published.value, critical: critical.checked, changelog: changelog.value,
    }), "Update notice saved")) views.release(main);
  } });
  const upload = el("button", { class: "btn", text: "Upload addon file", onClick: async () => {
    const f = file.files[0];
    if (!f) return toast("Choose the .addon64 file first.", true);
    if (!confirm(`Players on older versions will be offered ${f.name} as version ${version.value}. Upload?`)) return;
    if (await attempt(() => api("PUT", `/api/admin/release/artifact?version=${encodeURIComponent(version.value)}`, f), "Addon file uploaded")) views.release(main);
  } });

  main.replaceChildren(
    el("h1", { text: "Release" }),
    el("p", { class: "subtitle", text: "What the in-game updater shows. Players on an older version see this notice; with a file uploaded they can update in game." }),
    el("div", { class: "grid-2" },
      el("div", { class: "card" },
        el("h2", { text: "Live now" }),
        el("dl", { class: "kv" },
          el("dt", { text: "Version" }), el("dd", { text: r.latestVersion }),
          el("dt", { text: "Download page" }), el("dd", { text: r.downloadPageUrl }),
          el("dt", { text: "Published" }), el("dd", { text: r.publishedAt || "-" }),
          el("dt", { text: "Critical" }), el("dd", { text: r.critical ? "yes" : "no" }),
          el("dt", { text: "Addon file" }), el("dd", { class: "mono", text: r.addonR2Key ? `${fmt.bytes(r.addonSizeBytes)} - sha256 ${r.addonSha256}` : "none (players get the download page)" })),
        el("h2", { text: "Changelog" }),
        el("div", { class: "changelog", text: r.changelog })),
      el("div", { class: "card" },
        el("h2", { text: "Publish" }),
        el("label", { class: "field" }, el("span", { text: "Version" }), version),
        el("label", { class: "field" }, el("span", { text: "Download page (https)" }), url),
        el("label", { class: "field" }, el("span", { text: "Published (shown to players)" }), published),
        el("label", { class: "field" }, el("span", { text: "Changelog (newest first)" }), changelog),
        el("label", { class: "actions" }, critical, el("span", { text: "Mark as critical" })),
        el("div", { class: "actions", style: "margin-top:12px" }, save),
        el("h2", { text: "Addon file for in-game update" }),
        el("div", { class: "actions" }, file, upload))));
};

// Audit log.
views.activity = async (main) => {
  const { audit } = await api("GET", "/api/admin/audit?limit=500");
  const filter = el("input", { type: "search", class: "grow", placeholder: "Filter by action, preset, person or note", attrs: { "aria-label": "Filter" } });
  const body = el("tbody");
  const draw = () => {
    const f = filter.value.toLowerCase();
    const rows = audit.filter((a) => !f || [a.action, a.preset_id, a.actor, a.note].some((v) => String(v || "").toLowerCase().includes(f)));
    body.replaceChildren(...rows.map((a) => el("tr", {},
      el("td", { text: fmt.ago(a.created_at), title: fmt.date(a.created_at) }),
      el("td", { text: a.actor }),
      el("td", {}, el("span", { class: "badge", text: a.action })),
      el("td", { class: "mono", text: a.preset_id || "" }),
      el("td", { text: a.note || "" }))));
  };
  filter.addEventListener("input", draw);
  main.replaceChildren(
    el("h1", { text: "Activity" }),
    el("p", { class: "subtitle", text: "Every moderation and release action, newest first." }),
    el("div", { class: "toolbar" }, filter),
    el("table", {}, el("thead", {}, el("tr", {}, ["When", "Who", "Action", "Preset", "Note"].map((h) => el("th", { text: h })))), body));
  draw();
};

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------
async function route() {
  const name = (location.hash || "#review").slice(1);
  const view = views[name] ? name : "review";
  for (const link of document.querySelectorAll(".sidebar a")) link.classList.toggle("active", link.dataset.view === view);
  routeNumber++;
  if (cleanup) cleanup();
  cleanup = null;
  document.querySelectorAll(".drawer, .drawer-backdrop").forEach((node) => node.remove());
  const main = $("#main");
  main.replaceChildren(el("div", { class: "muted", text: "Loading..." }));
  refreshCounts();
  try {
    await views[view](main);
  } catch (error) {
    main.replaceChildren(el("div", { class: "card" }, el("h1", { text: "Something went wrong" }), el("p", { text: error.message })));
  }
}

async function start() {
  if (/-dev\b|localhost/.test(location.hostname)) $("#env-badge").hidden = false;
  try {
    const me = await api("GET", "/api/admin/me");
    $("#whoami").textContent = me.actor.replace(/^admin:/, "");
  } catch (error) {
    $("#whoami").textContent = "unknown";
  }
  window.addEventListener("hashchange", route);
  await route();
}

start();
