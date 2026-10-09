// SPDX-License-Identifier: GPL-3.0-or-later
// Pure table engine for the web status viewer: sort, filter, search, tree, export, URL state.
// No DOM and no globals, so it runs unchanged under `node --test`.

const PRIORITY_ORDER = ["max", "high", "mid", "low", "for-later"];
const STATE_ORDER = { done: 0, open: 1, pending: 1, "in-flight": 2, active: 2, blocked: 3 };
const NONE_LABEL = "(none)";
const RANGE_NUMBER = "-?\\d+(?:\\.\\d+)?";
const RANGE_COMPARE = new RegExp(`^(>=|<=|>|<|=)\\s*(${RANGE_NUMBER})$`);
const RANGE_SPAN = new RegExp(`^(${RANGE_NUMBER})\\s*\\.\\.\\s*(${RANGE_NUMBER})$`);
const REGEX_FILTER = /^\/(.+)\/([a-z]*)$/s;
const ID_PARTS = /^([A-Za-z]*)(\d+)/;
const SORT_DIRS = ["asc", "desc"];

function isMissing(type, value) {
  if (type === "percent") return value === null || value === undefined || value.percent === null || value.percent === undefined;
  return value === null || value === undefined || (type === "priority" && value === "");
}

function progressFraction(value) {
  return value.total > 0 ? value.done / value.total : 0;
}

function compareIds(a, b) {
  const left = ID_PARTS.exec(String(a));
  const right = ID_PARTS.exec(String(b));
  if (!left || !right) return compareText(a, b);
  return Number(left[2]) - Number(right[2]) || compareText(left[1], right[1]);
}

function compareText(a, b) {
  const left = String(a).toLowerCase();
  const right = String(b).toLowerCase();
  return left < right ? -1 : left > right ? 1 : 0;
}

function rankOf(order, value) {
  const index = order.indexOf(value);
  return index < 0 ? order.length : index;
}

/** Ascending comparison of two non-missing values of a column type; missing values sort last. */
export function compareValues(type, a, b) {
  const missingA = isMissing(type, a);
  const missingB = isMissing(type, b);
  if (missingA || missingB) return Number(missingA) - Number(missingB);
  switch (type) {
    case "priority":
      return rankOf(PRIORITY_ORDER, a) - rankOf(PRIORITY_ORDER, b);
    case "state":
      return (STATE_ORDER[a] ?? 4) - (STATE_ORDER[b] ?? 4);
    case "id":
      return compareIds(a, b);
    case "number":
    case "age":
      return a - b;
    case "progress":
      return progressFraction(a) - progressFraction(b);
    case "percent":
      return a.percent - b.percent;
    case "bool":
      return Number(a) - Number(b);
    default:
      return compareText(a, b);
  }
}

function formatAge(seconds) {
  const whole = Math.max(0, Math.floor(seconds));
  if (whole < 60) return `${whole}s`;
  if (whole < 3600) return `${Math.floor(whole / 60)}m`;
  return `${Math.floor(whole / 3600)}h`;
}

/** The text a cell shows, also the text that filters, search and CSV see. */
export function cellText(column, value) {
  if (value === null || value === undefined) return "";
  switch (column.type) {
    case "progress":
      return `${value.done}/${value.total}`;
    case "age":
      return formatAge(value);
    case "percent":
      if (value.no_report) return "no report";
      if (value.percent === null || value.percent === undefined) return "";
      return `${value.kind === "assumed" ? "~" : ""}${value.percent}%`;
    case "bool":
      return value ? "yes" : "no";
    default:
      return String(value);
  }
}

/** Lowercased search terms, `"quoted phrases"` kept whole. */
export function searchTerms(query) {
  const terms = [];
  for (const match of String(query ?? "").matchAll(/"([^"]*)"|([^\s"]+)/g)) {
    const term = (match[1] ?? match[2]).toLowerCase();
    if (term) terms.push(term);
  }
  return terms;
}

/** Stable multi-key sort by `[{key, dir}]`, a new array, missing values last in both directions. */
export function sortRows(rows, columns, sorts) {
  const keys = (sorts ?? []).flatMap((sort) => {
    const column = columns.find((candidate) => candidate.key === sort.key);
    return column ? [{ key: sort.key, type: column.type, sign: sort.dir === "desc" ? -1 : 1 }] : [];
  });
  return rows
    .map((row, index) => ({ row, index }))
    .sort((left, right) => {
      for (const { key, type, sign } of keys) {
        const a = left.row[key];
        const b = right.row[key];
        const missingA = isMissing(type, a);
        const missingB = isMissing(type, b);
        if (missingA || missingB) {
          if (missingA !== missingB) return missingA ? 1 : -1;
          continue;
        }
        const order = compareValues(type, a, b);
        if (order) return sign * order;
      }
      return left.index - right.index;
    })
    .map(({ row }) => row);
}

function numericValue(column, value) {
  if (value === null || value === undefined) return null;
  if (column.type === "progress") return progressFraction(value) * 100;
  if (column.type === "percent") return value.percent ?? null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function parseRange(text) {
  const trimmed = text.trim();
  const compare = RANGE_COMPARE.exec(trimmed);
  if (compare) {
    const limit = Number(compare[2]);
    const tests = {
      ">": (n) => n > limit,
      ">=": (n) => n >= limit,
      "<": (n) => n < limit,
      "<=": (n) => n <= limit,
      "=": (n) => n === limit,
    };
    return tests[compare[1]];
  }
  const span = RANGE_SPAN.exec(trimmed);
  if (span) {
    const low = Number(span[1]);
    const high = Number(span[2]);
    return (n) => n >= low && n <= high;
  }
  return null;
}

function parseTextFilter(text) {
  const negated = text.startsWith("!");
  const pattern = negated ? text.slice(1) : text;
  if (!pattern) return { empty: true };
  const wrapped = REGEX_FILTER.exec(pattern);
  if (!wrapped) {
    const needle = pattern.toLowerCase();
    return { negated, test: (cell) => cell.toLowerCase().includes(needle) };
  }
  try {
    const regex = new RegExp(wrapped[1], wrapped[2].replace(/[gy]/g, ""));
    return { negated, test: (cell) => regex.test(cell) };
  } catch {
    return { invalid: true };
  }
}

/** Selected values of an enum filter: an array as is, or the hash form `a,b` (`(none)` is the empty value). */
export function enumSelection(filter) {
  if (Array.isArray(filter)) return filter;
  if (!filter) return [];
  return String(filter)
    .split(",")
    .map((item) => item.replace(/%2C/g, ","))
    .map((item) => (item === NONE_LABEL ? "" : item));
}

/** Inverse of enumSelection for the hash form. */
export function enumJoin(values) {
  return values.map((value) => (value === "" ? NONE_LABEL : value.replace(/,/g, "%2C"))).join(",");
}

/** False when a non-empty filter text cannot be parsed (an invalid regex or range), so the UI can flag it. */
export function isFilterValid(column, text) {
  if (!text || column.filter === "enum") return true;
  if (column.filter === "range") return parseRange(text) !== null;
  return !parseTextFilter(text).invalid;
}

function rowPasses(row, column, filter) {
  const value = row[column.key];
  if (column.filter === "enum") {
    const selected = enumSelection(filter);
    return selected.length === 0 || selected.includes(cellText(column, value));
  }
  const text = String(filter ?? "");
  if (!text.trim()) return true;
  if (column.filter === "range") {
    const test = parseRange(text);
    const number = numericValue(column, value);
    return test !== null && number !== null && test(number);
  }
  const parsed = parseTextFilter(text);
  if (parsed.empty) return true;
  if (parsed.invalid) return false;
  return parsed.test(cellText(column, value)) !== parsed.negated;
}

/** Rows passing every column filter and every search term. Search reads `searchColumns`, default `columns`. */
export function filterRows(rows, columns, filters, search, searchColumns = columns) {
  const active = columns.filter((column) => filters && column.key in filters);
  const terms = searchTerms(search);
  return rows.filter((row) => {
    if (!active.every((column) => rowPasses(row, column, filters[column.key]))) return false;
    if (terms.length === 0) return true;
    const haystack = searchColumns
      .map((column) => cellText(column, row[column.key]))
      .join("\n")
      .toLowerCase();
    return terms.every((term) => haystack.includes(term));
  });
}

/** Text split into `{text, match}` segments where a search term occurs, for highlighting. */
export function highlightSegments(text, terms) {
  const lower = text.toLowerCase();
  if (lower.length !== text.length || terms.length === 0) return [{ text, match: false }];
  const marked = new Array(text.length).fill(false);
  for (const term of terms) {
    for (let at = lower.indexOf(term); term && at >= 0; at = lower.indexOf(term, at + 1)) {
      marked.fill(true, at, at + term.length);
    }
  }
  const segments = [];
  for (let index = 0; index < text.length; index += 1) {
    const last = segments[segments.length - 1];
    if (last && last.match === marked[index]) last.text += text[index];
    else segments.push({ text: text[index], match: marked[index] });
  }
  return segments.length ? segments : [{ text, match: false }];
}

const LINK_AT = /^\[([^\]]*)\]\(([^)\s]*)\)/;

function pushSegment(out, kind, text, target) {
  const last = out[out.length - 1];
  if (last && last.kind === kind && (kind === "text" || kind === "strong")) last.text += text;
  else out.push(target === undefined ? { kind, text } : { kind, text, target });
}

function scanInline(source, plain, out) {
  let at = 0;
  while (at < source.length) {
    const char = source[at];
    const closeCode = char === "`" ? source.indexOf("`", at + 1) : -1;
    const closeStrong = source.startsWith("**", at) ? source.indexOf("**", at + 2) : -1;
    const link = char === "[" ? LINK_AT.exec(source.slice(at)) : null;
    if (closeCode > at + 1) {
      pushSegment(out, "code", source.slice(at + 1, closeCode));
      at = closeCode + 1;
    } else if (closeStrong > at + 2) {
      scanInline(source.slice(at + 2, closeStrong), "strong", out);
      at = closeStrong + 2;
    } else if (link && link[1]) {
      pushSegment(out, "link", link[1], link[2]);
      at += link[0].length;
    } else {
      pushSegment(out, plain, char);
      at += 1;
    }
  }
}

/**
 * Inline markdown of a note as `[{kind: "text"|"strong"|"code"|"link", text, target?}]`.
 * Only `**strong**`, `` `code` `` and `[text](target)` are recognised, everything else (including HTML) stays text.
 * Markup that is unterminated or empty stays literal. A link target is returned verbatim and is never to be followed.
 */
export function inlineSegments(text) {
  const out = [];
  scanInline(String(text ?? ""), "text", out);
  return out;
}

/** Nested rows for a tree table: `[{row, depth, hasChildren, collapsed, dim}]` in display order. */
export function visibleTree(rows, columns, options = {}) {
  const { sorts = [], filters = {}, search = "", collapsed = new Set(), searchColumns = columns } = options;
  const byKey = new Map(rows.map((row) => [row._key, row]));
  const hasValidParent = (row) => {
    const seen = new Set([row._key]);
    for (let at = byKey.get(row._parent); at; at = byKey.get(at._parent)) {
      if (seen.has(at._key)) return false;
      seen.add(at._key);
    }
    return byKey.has(row._parent);
  };
  const children = new Map();
  const roots = [];
  for (const row of rows) {
    if (!hasValidParent(row)) roots.push(row);
    else if (children.has(row._parent)) children.get(row._parent).push(row);
    else children.set(row._parent, [row]);
  }
  const matched = new Set(filterRows(rows, columns, filters, search, searchColumns).map((row) => row._key));
  const kept = new Set(matched);
  for (const key of matched) {
    for (let row = byKey.get(key); hasValidParent(row) && !kept.has(row._parent); row = byKey.get(row._parent)) {
      kept.add(row._parent);
    }
  }
  const out = [];
  const walk = (siblings, depth) => {
    for (const row of sortRows(siblings, columns, sorts)) {
      if (!kept.has(row._key)) continue;
      const shown = (children.get(row._key) ?? []).filter((child) => kept.has(child._key));
      const isCollapsed = shown.length > 0 && collapsed.has(row._key);
      out.push({ row, depth, hasChildren: shown.length > 0, collapsed: isCollapsed, dim: !matched.has(row._key) });
      if (!isCollapsed) walk(shown, depth + 1);
    }
  };
  walk(roots, 0);
  return out;
}

function csvField(text) {
  return /[",\r\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

/** RFC 4180 CSV (CRLF) of the given rows and columns as displayed. */
export function toCsv(rows, columns) {
  const lines = [columns.map((column) => csvField(column.label ?? column.key))];
  for (const row of rows) lines.push(columns.map((column) => csvField(cellText(column, row[column.key]))));
  return lines.map((line) => `${line.join(",")}\r\n`).join("");
}

/** JSON array of the given rows restricted to the given columns, raw typed values. */
export function toJson(rows, columns) {
  return JSON.stringify(
    rows.map((row) => Object.fromEntries(columns.map((column) => [column.key, row[column.key] ?? null]))),
    null,
    2,
  );
}

const encodeList = (items) => items.map(encodeURIComponent).join(",");

function decodeText(raw) {
  try {
    return decodeURIComponent(raw);
  } catch {
    return raw;
  }
}

const decodeList = (raw) => raw.split(",").filter(Boolean).map(decodeText);

/** URL query text (or legacy hash text) for `{tab, tables: {id: {sorts, q, filters, cols, collapsed}}}`, "" when empty. */
export function encodeState(state) {
  const parts = [];
  if (state.tab) parts.push(`tab=${encodeURIComponent(state.tab)}`);
  for (const [id, table] of Object.entries(state.tables ?? {})) {
    const prefix = `t.${id}.`;
    if (table.sorts?.length) {
      parts.push(`${prefix}sort=${table.sorts.map((sort) => `${encodeURIComponent(sort.key)}:${sort.dir}`).join(",")}`);
    }
    if (table.q) parts.push(`${prefix}q=${encodeURIComponent(table.q)}`);
    for (const [key, filter] of Object.entries(table.filters ?? {})) {
      if (filter) parts.push(`${prefix}f.${key}=${encodeURIComponent(filter).replace(/%2C/g, ",")}`);
    }
    if (table.cols?.length) parts.push(`${prefix}cols=${encodeList(table.cols)}`);
    if (table.collapsed?.length) parts.push(`${prefix}collapsed=${encodeList(table.collapsed)}`);
  }
  return parts.join("&");
}

/** Inverse of encodeState, for a query string, a hash or bare text. Unknown keys are ignored. */
export function decodeState(text) {
  const state = { tab: "", tables: {} };
  for (const part of String(text ?? "").replace(/^[#?]/, "").split("&")) {
    const at = part.indexOf("=");
    if (at < 0) continue;
    const name = part.slice(0, at);
    const raw = part.slice(at + 1);
    if (name === "tab") {
      state.tab = decodeText(raw);
      continue;
    }
    const match = /^t\.([^.]+)\.(sort|q|cols|collapsed|f\..+)$/.exec(name);
    if (!match) continue;
    // A table id comes from a link: it must never name an inherited property such as __proto__.
    if (match[1] === "__proto__") continue;
    if (!Object.hasOwn(state.tables, match[1])) state.tables[match[1]] = { sorts: [], q: "", filters: {}, cols: [], collapsed: [] };
    const table = state.tables[match[1]];
    const field = match[2];
    if (field === "sort") {
      table.sorts = decodeList(raw).flatMap((item) => {
        const [key, dir] = item.split(":");
        return key && SORT_DIRS.includes(dir) ? [{ key, dir }] : [];
      });
    } else if (field === "q") table.q = decodeText(raw);
    else if (field === "cols") table.cols = decodeList(raw);
    else if (field === "collapsed") table.collapsed = decodeList(raw);
    else table.filters[field.slice(2)] = decodeText(raw);
  }
  return state;
}

export const VIEW_IDS = ["dashboard", "milestones", "tasks", "sessions", "workers", "messages", "progress"];
const TASK_ROUTE_ID = /^T\d+$/;
const MILESTONE_ROUTE_ID = /^M\d+$/;
const SESSION_ROUTE_ID = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
// Message recipients that are not one session.
const NOT_A_SESSION = new Set(["", "all", "*"]);

/** The view a pathname shows: `{view, id?}`, view `notfound` for a path the server does not serve. */
export function parseRoute(pathname) {
  const parts = String(pathname ?? "").split("/");
  if (parts[0] !== "") return { view: "notfound" };
  if (parts.length === 2 && parts[1] === "") return { view: "dashboard" };
  if (parts.length === 2 && VIEW_IDS.includes(parts[1])) return { view: parts[1] };
  if (parts.length === 3 && parts[1] === "tasks" && TASK_ROUTE_ID.test(parts[2])) return { view: "task", id: parts[2] };
  if (parts.length === 3 && parts[1] === "milestones" && MILESTONE_ROUTE_ID.test(parts[2])) return { view: "milestone", id: parts[2] };
  if (parts.length === 3 && parts[1] === "sessions" && SESSION_ROUTE_ID.test(parts[2])) return { view: "session", id: parts[2] };
  return { view: "notfound" };
}

/** Inverse of parseRoute. A route that is not found has no page, so it maps to the dashboard. */
export function routePath(route) {
  if (route.view === "task") return `/tasks/${route.id}`;
  if (route.view === "milestone") return `/milestones/${route.id}`;
  if (route.view === "session") return `/sessions/${route.id}`;
  return VIEW_IDS.includes(route.view) ? `/${route.view}` : "/dashboard";
}

/** The Blocked by line of a task page: the server text, plus `(blocker landed, reopen)` on a [!] task whose blockers all landed. "" when the task names no blocker. */
export function blockedByText(blocked, state) {
  if (!blocked || !blocked.text) return "";
  return blocked.landed && state === "blocked" ? `${blocked.text} (blocker landed, reopen)` : blocked.text;
}

/** `badge state-no-report` for the value "no report": a space in the value must not split the CSS class. */
export function badgeClass(kind, value) {
  return `badge ${kind}-${String(value).trim().replace(/\s+/g, "-")}`;
}

const plural = (count, word) => `${count} ${word}${count === 1 ? "" : "s"}`;

/** The Audit line of a task page from `task.audit`: owners ever, claims, time held, handoffs, blocks, progress reports. "" without events. */
export function auditText(audit) {
  if (!audit) return "";
  const owners = audit.owners ?? [];
  const blocks = audit.blocks ?? { count: 0, open: false };
  const parts = [
    `${plural(owners.length, "owner")} ever${owners.length ? ` (${owners.join(", ")})` : ""}`,
    plural(audit.claims ?? 0, "claim"),
    `held ${formatAge(audit.held_seconds ?? 0)}`,
    plural(audit.handoffs ?? 0, "handoff"),
    `${plural(blocks.count, "block")}${blocks.open ? " (one open)" : ""}`,
    plural(audit.progress?.updates ?? 0, "progress report"),
  ];
  return parts.join(", ");
}

/** Page link for a task id (`T12`) or a milestone id (`M3`), "" for any other text. */
export function entityHref(id) {
  if (TASK_ROUTE_ID.test(id ?? "")) return routePath({ view: "task", id });
  if (MILESTONE_ROUTE_ID.test(id ?? "")) return routePath({ view: "milestone", id });
  return "";
}

/** Page link for a session owner (`claude-main`), "" for an empty name, `all`, `*` or a name the server would refuse. */
export function sessionHref(owner) {
  const name = owner ?? "";
  if (NOT_A_SESSION.has(name) || !SESSION_ROUTE_ID.test(name)) return "";
  return routePath({ view: "session", id: name });
}

/** A pathname plus the query of the given table states, no `?` when every state is default. */
export function stateHref(pathname, tables) {
  const text = encodeState({ tables });
  return text ? `${pathname}?${text}` : pathname;
}

/** The link that results from changing one table's state (`patch` is merged over it). */
export function tableHref(pathname, tables, tableId, patch) {
  return stateHref(pathname, { ...tables, [tableId]: { ...(tables[tableId] ?? {}), ...patch } });
}

/** Sort keys after a click on a column: asc, desc, off. `additive` (shift) edits the other keys' list instead of replacing it. */
export function nextSorts(sorts, key, additive) {
  const list = sorts.map((sort) => ({ ...sort }));
  const current = list.find((sort) => sort.key === key);
  const dir = !current ? "asc" : current.dir === "asc" ? "desc" : null;
  if (!additive) return dir ? [{ key, dir }] : [];
  if (!current) return [...list, { key, dir: "asc" }];
  if (dir) {
    current.dir = dir;
    return list;
  }
  return list.filter((sort) => sort !== current);
}

/** The link a sort header points at: the current path with the query that clicking it would give. */
export function sortHref(pathname, tables, tableId, key, additive = false) {
  return tableHref(pathname, tables, tableId, { sorts: nextSorts(tables[tableId]?.sorts ?? [], key, additive) });
}

/** The URL a legacy `#tab=tasks&t.tasks.sort=...` link stands for (tab to path, the rest to query), null when the hash holds no state. */
export function legacyHashToUrl(hash, pathname = "/dashboard") {
  const state = decodeState(hash);
  if (!state.tab && Object.keys(state.tables).length === 0) return null;
  return stateHref(VIEW_IDS.includes(state.tab) ? `/${state.tab}` : pathname, state.tables);
}
