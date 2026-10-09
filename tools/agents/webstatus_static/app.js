// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only status page: the list views render the snapshot of /api/state, the task, milestone and session pages
// render /api/task/T<n>, /api/milestone/M<n> and /api/session/<owner>, all with the table engine in table-core.js.
// The path says which view, the query string carries the table state, navigation never reloads the page.
// All ledger text reaches the DOM through textContent or text nodes, never innerHTML.
import {
  auditText, badgeClass, blockedByText, cellText, compareValues, decodeState, encodeState, entityHref, enumJoin, enumSelection, filterRows, highlightSegments,
  inlineSegments, isFilterValid, legacyHashToUrl, nextSorts, parseRoute, routePath, searchTerms, sessionHref, sortHref, sortRows, tableHref,
  toCsv, toJson, VIEW_IDS, visibleTree,
} from "/static/table-core.js";

const DEFAULT_INTERVAL = 5;
const STALE_FLOOR_SECONDS = 30;
// The text column that takes the spare width, the first of these a table has.
const PRIMARY_KEYS = ["now", "title", "text", "note", "assigned", "tasks"];
const INDENT_REM = 1.25;
// Which snapshot views show each gather source's error (mirrors the TUI error_lines).
const SOURCE_VIEWS = {
  roadmap: ["dashboard", "milestones"],
  claims: ["dashboard", "milestones", "tasks", "sessions"],
  tasks: ["dashboard", "tasks", "progress"],
  metrics: ["dashboard", "progress"],
  messages: ["dashboard", "messages"],
  fleet: ["dashboard", "workers"],
};
const HINTS = {
  text: "text, /regex/, !not",
  range: ">5  <=3  =7  5..10",
};

const app = {
  route: { view: "notfound" },
  path: "",
  snapshot: null,
  version: "",
  interval: DEFAULT_INTERVAL,
  entity: null,
  tables: new Map(),
  tableStates: {},
  paused: false,
  lastOkAt: 0,
  receivedAt: 0,
  baseElapsed: 0,
  stale: false,
  unreachable: false,
  pollTimer: null,
  generation: 0,
};

const byId = (id) => document.getElementById(id);
const isEntity = (route) => route.view === "task" || route.view === "milestone" || route.view === "session";

function h(tag, props = {}, ...kids) {
  const element = document.createElement(tag);
  for (const [name, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (name === "class") element.className = value;
    else if (name.startsWith("on")) element.addEventListener(name.slice(2), value);
    else element.setAttribute(name, value === true ? "" : String(value));
  }
  element.append(...kids.flat().filter((kid) => kid !== null && kid !== undefined && kid !== false));
  return element;
}

function primaryColumnKey(columns) {
  const wanted = PRIMARY_KEYS.find((key) => columns.some((column) => column.key === key && column.type === "text"));
  return wanted ?? "";
}

function ageOffset() {
  return app.baseElapsed + (performance.now() - app.receivedAt) / 1000;
}

function tableState(id) {
  app.tableStates[id] ??= { sorts: [], q: "", filters: {}, cols: [], collapsed: [] };
  return app.tableStates[id];
}

/** A click the browser should handle itself (new tab, new window, download): any modifier or a non-primary button. */
function modifiedClick(event, allowShift = false) {
  return event.button !== 0 || event.ctrlKey || event.metaKey || event.altKey || (event.shiftKey && !allowShift);
}

/** Put the table state of the page into the query string, replacing the entry so history is not spammed. */
function syncUrl() {
  const text = encodeState({ tables: app.tableStates });
  history.replaceState(null, "", location.pathname + (text ? `?${text}` : ""));
  for (const table of app.tables.values()) table.updateLinks();
}

function sourceErrors(view, table) {
  // An entity page carries only the errors of its own sources.
  if (view.errors) return Object.entries(view.errors).map(([name, message]) => `ERROR ${name}: ${message}`);
  const errors = app.snapshot?.errors ?? {};
  return Object.entries(errors)
    .filter(([name]) => name === table.id || name === view.id || SOURCE_VIEWS[name]?.includes(view.id))
    .map(([name, message]) => `ERROR ${name}: ${message}`);
}

class TableView {
  constructor(view, spec) {
    this.view = view;
    this.spec = spec;
    this.state = tableState(spec.id);
    this.signature = "";
    this.nodes = [];
    this.popover = null;
    this.build();
    this.update(spec);
  }

  get columns() {
    return this.spec.columns;
  }

  isVisible(column) {
    const { cols } = this.state;
    if (cols.includes(`-${column.key}`)) return false;
    if (cols.includes(`+${column.key}`)) return true;
    return !column.hidden;
  }

  visibleColumns() {
    return this.columns.filter((column) => this.isVisible(column));
  }

  rows() {
    const offset = Math.floor(ageOffset());
    const ageKeys = this.columns.filter((column) => column.type === "age").map((column) => column.key);
    if (ageKeys.length === 0) return this.spec.rows;
    return this.spec.rows.map((row) => {
      const copy = { ...row };
      for (const key of ageKeys) if (typeof copy[key] === "number") copy[key] += offset;
      return copy;
    });
  }

  build() {
    this.search = h("input", {
      type: "search", class: "search", "aria-label": `Search ${this.spec.title}`, placeholder: "search  (/ to focus)",
      value: this.state.q,
    });
    this.search.value = this.state.q;
    this.search.addEventListener("input", () => {
      this.state.q = this.search.value;
      this.changed();
    });
    this.search.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      event.stopPropagation();
      this.clearSearch();
      this.search.blur();
    });
    this.searchClear = h("a", { class: "search-clear", "aria-label": "Clear search", title: "Clear search", hidden: true }, "×");
    this.searchClear.addEventListener("click", (event) => this.linkClick(event, () => this.clearSearch()));
    this.count = h("span", { class: "count", role: "status" });
    this.clearLink = h("a", { class: "clear button-link" }, "Clear filters");
    this.clearLink.addEventListener("click", (event) => this.linkClick(event, () => this.clearFilters()));
    this.columnsButton = h("button", { type: "button", class: "columns-button", "aria-haspopup": "true" }, "Columns");
    this.columnsButton.addEventListener("click", () => this.openColumnsMenu());
    const csvButton = h("button", { type: "button", class: "export-csv", onclick: () => this.download("csv") }, "CSV");
    const jsonButton = h("button", { type: "button", class: "export-json", onclick: () => this.download("json") }, "JSON");
    this.errorBox = h("div", { class: "error", role: "alert", hidden: true });
    this.headRow = h("tr", { class: "head-row" });
    this.filterRow = h("tr", { class: "filter-row" });
    this.body = h("tbody");
    this.table = h("table", {}, h("thead", {}, this.headRow, this.filterRow), this.body);
    this.wrap = h("div", { class: "scroll" }, this.table);
    this.element = h(
      "section",
      { class: "card", "data-table": this.spec.id },
      h("div", { class: "toolbar" }, h("h2", {}, this.spec.title), h("span", { class: "search-box" }, this.search, this.searchClear), this.count, this.clearLink, this.columnsButton, csvButton, jsonButton),
      this.errorBox,
      this.wrap,
    );
  }

  /** A real link whose plain click does `action` in place, a modified click is left to the browser (new tab). */
  linkClick(event, action, allowShift = false) {
    if (modifiedClick(event, allowShift) || !event.currentTarget.hasAttribute("href")) return;
    event.preventDefault();
    action();
  }

  /** Apply a new spec from a poll: rebuild the header only when the columns changed, always redraw rows. */
  update(spec) {
    this.spec = spec;
    const signature = JSON.stringify(spec.columns.map((column) => [column.key, column.label, column.type, column.filter, column.hidden]));
    if (signature !== this.signature) {
      this.signature = signature;
      this.buildHead();
    }
    this.renderBody();
  }

  /** Take the state of the page again after the query string changed under the table. */
  restoreState() {
    this.state = tableState(this.spec.id);
    this.search.value = this.state.q;
    this.signature = "";
    this.update(this.spec);
  }

  buildHead() {
    const columns = this.visibleColumns();
    this.primaryKey = primaryColumnKey(columns);
    this.headRow.replaceChildren(...columns.map((column) => this.headerCell(column)));
    this.filterRow.replaceChildren(...columns.map((column) => this.filterCell(column)));
  }

  wrapKind(column) {
    return column.key === this.primaryKey ? "primary" : undefined;
  }

  headerCell(column) {
    const sorts = this.state.sorts;
    const index = sorts.findIndex((sort) => sort.key === column.key);
    const dir = index < 0 ? "none" : sorts[index].dir === "asc" ? "ascending" : "descending";
    const arrow = index < 0 ? "" : sorts[index].dir === "asc" ? " ▲" : " ▼";
    const rank = index >= 0 && sorts.length > 1 ? h("sup", { class: "rank" }, String(index + 1)) : "";
    const link = h("a", {
      class: "sort", "data-key": column.key, href: sortHref(location.pathname, app.tableStates, this.spec.id, column.key),
      title: "click: sort, shift-click: add a sort key",
    }, column.label, arrow, rank);
    link.addEventListener("click", (event) => this.linkClick(event, () => this.cycleSort(column.key, event.shiftKey), true));
    return h("th", { "data-col": column.key, "data-type": column.type, "data-wrap": this.wrapKind(column), "aria-sort": dir, scope: "col" }, link);
  }

  filterCell(column) {
    const cell = h("th", { "data-col": column.key, "data-type": column.type, "data-wrap": this.wrapKind(column), class: "filter" });
    if (column.filter === "enum") {
      const button = h("button", { type: "button", class: "enum-button", "data-filter": column.key, "aria-haspopup": "true" });
      button.textContent = this.enumLabel(column);
      button.addEventListener("click", () => this.openEnumMenu(column, button));
      cell.append(button);
    } else {
      const input = h("input", {
        type: "text", "data-filter": column.key, "aria-label": `Filter ${column.label}`, placeholder: HINTS[column.filter] ?? HINTS.text,
      });
      input.value = String(this.state.filters[column.key] ?? "");
      this.flagFilter(input, column);
      input.addEventListener("input", () => {
        this.setFilter(column.key, input.value);
        this.flagFilter(input, column);
        this.changed();
      });
      cell.append(input);
    }
    return cell;
  }

  flagFilter(input, column) {
    const valid = isFilterValid(column, input.value);
    input.classList.toggle("invalid", !valid);
    input.setAttribute("aria-invalid", String(!valid));
  }

  enumLabel(column) {
    const selected = enumSelection(this.state.filters[column.key]);
    return selected.length === 0 ? "all" : `${selected.length} selected`;
  }

  setFilter(key, text) {
    if (text) this.state.filters[key] = text;
    else delete this.state.filters[key];
  }

  clearSearch() {
    this.state.q = "";
    this.search.value = "";
    this.changed();
  }

  clearFilters() {
    this.state.filters = {};
    this.state.q = "";
    this.search.value = "";
    this.buildHead();
    this.changed();
  }

  cycleSort(key, additive) {
    this.state.sorts = nextSorts(this.state.sorts, key, additive);
    this.buildHead();
    this.changed();
  }

  changed() {
    this.closePopover();
    this.refresh();
  }

  refresh() {
    this.renderBody();
    syncUrl();
  }

  hasFilters() {
    return Boolean(this.state.q) || Object.keys(this.state.filters).length > 0;
  }

  /** Point the sort headers, "Clear filters" and the search clear at the URL their click leads to. */
  updateLinks() {
    const { pathname } = location;
    const id = this.spec.id;
    for (const link of this.headRow.querySelectorAll("a.sort")) {
      link.setAttribute("href", sortHref(pathname, app.tableStates, id, link.getAttribute("data-key")));
    }
    const filtered = this.hasFilters();
    this.clearLink.setAttribute("aria-disabled", String(!filtered));
    if (filtered) this.clearLink.setAttribute("href", tableHref(pathname, app.tableStates, id, { q: "", filters: {} }));
    else this.clearLink.removeAttribute("href");
    this.searchClear.hidden = !this.state.q;
    this.searchClear.setAttribute("href", tableHref(pathname, app.tableStates, id, { q: "" }));
  }

  /** Rows in display order after filter, search and sort, with the tree collapse state applied. */
  displayed(collapsed) {
    const rows = this.rows();
    const columns = this.columns;
    const options = { sorts: this.state.sorts, filters: this.state.filters, search: this.state.q, searchColumns: this.visibleColumns() };
    if (this.spec.tree) return visibleTree(rows, columns, { ...options, collapsed });
    const kept = sortRows(filterRows(rows, columns, options.filters, options.search, options.searchColumns), columns, options.sorts);
    return kept.map((row) => ({ row, depth: 0, hasChildren: false, collapsed: false, dim: false }));
  }

  renderBody() {
    const { top, left } = { top: this.wrap.scrollTop, left: this.wrap.scrollLeft };
    const columns = this.visibleColumns();
    const terms = searchTerms(this.state.q);
    this.nodes = this.displayed(new Set(this.state.collapsed));
    const total = this.spec.rows.length;
    const shown = this.spec.tree ? this.matchCount() : this.nodes.length;
    this.count.textContent = `${shown} of ${total} rows`;
    this.updateLinks();
    const messages = sourceErrors(this.view, this.spec);
    this.errorBox.hidden = messages.length === 0;
    this.errorBox.replaceChildren(...messages.map((message) => h("div", {}, message)));
    const rows = this.nodes.map((node) => this.rowElement(node, columns, terms));
    if (rows.length === 0) {
      const text = messages.length ? "no data, the source failed" : total ? "no rows match" : "no rows";
      rows.push(h("tr", { class: "empty" }, h("td", { colspan: Math.max(1, columns.length) }, text)));
    }
    this.body.replaceChildren(...rows);
    this.wrap.scrollTop = top;
    this.wrap.scrollLeft = left;
    this.ageCells = [...this.body.querySelectorAll("td[data-age]")];
  }

  matchCount() {
    const { filters, q } = this.state;
    return filterRows(this.rows(), this.columns, filters, q, this.visibleColumns()).length;
  }

  rowElement(node, columns, terms) {
    const { row } = node;
    const tr = h("tr", { "data-key": row._key, class: node.dim ? "dim" : "" });
    columns.forEach((column, index) => {
      const td = h("td", { "data-col": column.key, "data-type": column.type, "data-wrap": this.wrapKind(column), class: column.type === "number" || column.type === "age" ? "num" : "" });
      if (column.type === "age") td.setAttribute("data-age", column.key);
      if (this.spec.tree && index === 0) td.append(this.treePrefix(node));
      td.append(this.cellContent(column, row, terms));
      const full = cellText(column, row[column.key]);
      if (full.length > 24) td.setAttribute("title", full);
      tr.append(td);
    });
    return tr;
  }

  treePrefix(node) {
    const indent = h("span", { class: "indent" });
    indent.style.paddingLeft = `${node.depth * INDENT_REM}rem`;
    if (!node.hasChildren) return h("span", { class: "tree-prefix" }, indent, h("span", { class: "toggle spacer" }));
    const toggle = h("button", {
      type: "button", class: "toggle", "aria-expanded": String(!node.collapsed),
      "aria-label": `${node.collapsed ? "Expand" : "Collapse"} ${node.row._key}`,
    }, node.collapsed ? "▸" : "▾");
    toggle.addEventListener("click", () => this.toggleNode(node.row._key));
    return h("span", { class: "tree-prefix" }, indent, toggle);
  }

  toggleNode(key) {
    const set = new Set(this.state.collapsed);
    if (!set.delete(key)) set.add(key);
    this.state.collapsed = [...set];
    this.changed();
  }

  cellContent(column, row, terms) {
    const value = row[column.key];
    const text = cellText(column, value);
    switch (column.type) {
      case "percent":
        return percentCell(value, text, row.claim_state === "stale");
      case "state":
      case "priority":
        return text ? h("span", { class: badgeClass(column.type, text) }, text) : h("span", { class: "muted" }, "—");
      case "progress": {
        if (value === null || value === undefined) return "";
        return h("span", { class: "progress" }, h("progress", { max: Math.max(1, value.total), value: value.done, "aria-label": text }), h("span", { class: "progress-text" }, text));
      }
      case "bool":
        return value === null || value === undefined ? "" : h("span", { class: `bool bool-${value ? "yes" : "no"}` }, text);
      case "id": {
        // Only a task id (T12) or a milestone id (M3) is a link, any other text in an id column stays text.
        const href = entityHref(text);
        return href ? h("a", { class: "id-link", href }, this.highlighted(text, terms)) : this.highlighted(text, terms);
      }
      case "session": {
        // A real owner name links to its page, an empty cell or a broadcast (`all`, `*`) stays text.
        const href = sessionHref(text);
        return href ? h("a", { class: "id-link session-link", href }, this.highlighted(text, terms)) : this.highlighted(text, terms);
      }
      default:
        return this.highlighted(text, terms);
    }
  }

  highlighted(text, terms) {
    if (terms.length === 0) return h("span", { class: "text" }, text);
    return h("span", { class: "text" }, highlightSegments(text, terms).map((part) => (part.match ? h("mark", {}, part.text) : part.text)));
  }

  /** Refresh the age cells in place so a ticking clock does not redraw the table. */
  tickAges() {
    const ageKeys = new Set(this.columns.filter((column) => column.type === "age").map((column) => column.key));
    if (ageKeys.size === 0) return;
    if (Object.keys(this.state.filters).some((key) => ageKeys.has(key))) {
      this.renderBody();
      return;
    }
    const offset = Math.floor(ageOffset());
    const rowsByKey = new Map(this.spec.rows.map((row) => [row._key, row]));
    for (const cell of this.ageCells ?? []) {
      const row = rowsByKey.get(cell.parentElement.getAttribute("data-key"));
      const base = row?.[cell.getAttribute("data-age")];
      if (typeof base !== "number") continue;
      const column = this.columns.find((candidate) => candidate.key === cell.getAttribute("data-age"));
      const text = cellText(column, base + offset);
      if (cell.textContent !== text) cell.textContent = text;
    }
  }

  download(kind) {
    const rows = this.displayed(new Set()).map((node) => node.row);
    const columns = this.visibleColumns();
    const body = kind === "csv" ? toCsv(rows, columns) : toJson(rows, columns);
    const type = kind === "csv" ? "text/csv" : "application/json";
    const url = URL.createObjectURL(new Blob([body], { type }));
    const link = h("a", { href: url, download: `${this.spec.id}.${kind}` });
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  closePopover() {
    if (this.popover) this.popover.remove();
    this.popover = null;
  }

  showPopover(anchor, content) {
    closeAllPopovers();
    const box = anchor.getBoundingClientRect();
    const popover = h("div", { class: "popover", role: "dialog" }, content);
    popover.style.left = `${Math.max(4, Math.min(box.left, window.innerWidth - 260))}px`;
    popover.style.top = `${box.bottom + 2}px`;
    document.body.append(popover);
    // The page itself never scrolls, so keep the menu inside the window.
    const overflow = box.bottom + 2 + popover.offsetHeight - window.innerHeight + 4;
    if (overflow > 0) popover.style.top = `${Math.max(4, box.bottom + 2 - overflow)}px`;
    this.popover = popover;
  }

  openEnumMenu(column, button) {
    if (this.popover && this.popover.getAttribute("data-for") === column.key) {
      this.closePopover();
      return;
    }
    const values = [...new Set(this.rows().map((row) => cellText(column, row[column.key])))]
      .sort((a, b) => compareValues(column.type, a === "" ? null : a, b === "" ? null : b) || (a < b ? -1 : a > b ? 1 : 0));
    const selected = new Set(enumSelection(this.state.filters[column.key]));
    const items = values.map((value) => {
      const box = h("input", { type: "checkbox", "data-value": value });
      box.checked = selected.has(value);
      box.addEventListener("change", () => {
        if (box.checked) selected.add(value);
        else selected.delete(value);
        const ordered = values.filter((item) => selected.has(item));
        this.setFilter(column.key, ordered.length ? enumJoin(ordered) : "");
        button.textContent = this.enumLabel(column);
        this.refresh();
      });
      return h("label", { class: "check" }, box, h("span", {}, value === "" ? "(none)" : value));
    });
    this.showPopover(button, h("div", {}, h("div", { class: "popover-title" }, column.label), ...items));
    this.popover.setAttribute("data-for", column.key);
  }

  openColumnsMenu() {
    if (this.popover && this.popover.getAttribute("data-for") === "*columns") {
      this.closePopover();
      return;
    }
    const items = this.columns.map((column) => {
      const box = h("input", { type: "checkbox", "data-column": column.key });
      box.checked = this.isVisible(column);
      box.addEventListener("change", () => this.setColumnVisible(column, box.checked));
      const filtered = !box.checked && column.key in this.state.filters ? " (filtered)" : "";
      return h("label", { class: "check" }, box, h("span", {}, `${column.label}${filtered}`));
    });
    this.showPopover(this.columnsButton, h("div", {}, h("div", { class: "popover-title" }, "Columns"), ...items));
    this.popover.setAttribute("data-for", "*columns");
  }

  setColumnVisible(column, visible) {
    const rest = this.state.cols.filter((token) => token.slice(1) !== column.key);
    if (visible === !column.hidden) this.state.cols = rest;
    else this.state.cols = [...rest, `${visible ? "+" : "-"}${column.key}`];
    this.buildHead();
    this.refresh();
  }
}

/** Inline markdown as DOM nodes. A link is plain text with its target in `title`, never an anchor: ledger text is untrusted. */
function inlineNodes(text) {
  return inlineSegments(text).map((segment) => {
    switch (segment.kind) {
      case "strong": return h("strong", {}, segment.text);
      case "code": return h("code", {}, segment.text);
      case "link": return h("span", { class: "note-link", title: segment.target }, segment.text);
      default: return segment.text;
    }
  });
}

function notesElement(view) {
  return view.notes.map((note) => h(
    "section",
    { class: "card notes" },
    h("h2", {}, note.title),
    h("ul", {}, note.lines.map((line) => h("li", {}, inlineNodes(line)))),
  ));
}

function viewElement(view) {
  const tables = view.tables.map((spec) => {
    const existing = app.tables.get(spec.id);
    if (existing) {
      existing.view = view;
      existing.update(spec);
      return existing;
    }
    const created = new TableView(view, spec);
    app.tables.set(spec.id, created);
    return created;
  });
  return { notes: notesElement(view), tables };
}

/** Swap the tables of the page only when the set changed: re-attaching would blur a focused filter and reset scroll. */
function mountTables(tables, extra = []) {
  const box = byId("tables");
  const wanted = [...tables.map((table) => table.element), ...extra];
  const current = [...box.children];
  if (wanted.length !== current.length || wanted.some((element, index) => element !== current[index])) box.replaceChildren(...wanted);
}

function showList() {
  const view = app.snapshot.views.find((candidate) => candidate.id === app.route.view);
  if (!view) {
    byId("notes").replaceChildren(h("p", { class: "muted" }, "unknown view"));
    mountTables([]);
    return;
  }
  const { notes, tables } = viewElement(view);
  byId("notes").replaceChildren(...notes);
  mountTables(tables);
}

function applySnapshot(snapshot) {
  const view = byId("view");
  const keepY = view.scrollTop;
  app.snapshot = snapshot;
  app.version = snapshot.version;
  app.interval = snapshot.interval ?? DEFAULT_INTERVAL;
  showList();
  view.scrollTop = keepY;
}

// Entity pages: the task, milestone and session views.

const badge = (kind, value) => (value ? h("span", { class: badgeClass(kind, value) }, value) : h("span", { class: "muted" }, "—"));
const none = (text = "none") => h("span", { class: "muted" }, text);

function entityLink(id) {
  const href = entityHref(id);
  return href ? h("a", { class: "id-link", href }, id) : h("span", {}, id);
}

function sessionLink(owner) {
  const href = sessionHref(owner);
  return href ? h("a", { class: "id-link session-link", href }, owner) : h("span", {}, owner ?? "");
}

const claimBadge = (state) => (state ? h("span", { class: `badge claim-${state}` }, state) : h("span", { class: "muted" }, "—"));

function fact(label, ...value) {
  return [h("dt", {}, label), h("dd", {}, ...value)];
}

function breadcrumb(items) {
  return h("nav", { class: "breadcrumb", "aria-label": "Breadcrumb" }, h("ol", {}, items.map(([label, href]) => (
    href ? h("li", {}, h("a", { href }, label)) : h("li", { "aria-current": "page" }, label)
  ))));
}

function entityHeader(id, title, ...badges) {
  return [h("h2", { class: "entity-title" }, h("span", { class: "entity-id" }, id), " ", title), h("div", { class: "badges" }, ...badges)];
}

/** A bar for a percent cell: solid when measured, hatched when assumed, always with its text label. */
function percentBar(value, text) {
  const fill = h("span", { class: "pbar-fill" });
  fill.style.width = `${Math.min(100, Math.max(0, value.percent))}%`;
  return h("span", { class: `pbar ${value.kind === "assumed" ? "pbar-assumed" : "pbar-measured"}`, role: "progressbar", "aria-valuemin": "0", "aria-valuemax": "100", "aria-valuenow": String(value.percent), "aria-label": text }, fill);
}

function percentCell(value, text, stale) {
  if (value === null || value === undefined) return "";
  if (value.no_report) return h("span", { class: "muted", title: "no progress report yet" }, text);
  if (value.percent === null || value.percent === undefined) return value.status ? h("span", { class: "muted" }, value.status) : "";
  return h("span", { class: `percent${stale ? " percent-stale" : ""}`, title: value.status || null }, percentBar(value, text), h("span", { class: "progress-text" }, text));
}

function progressElement(claim) {
  if (!claim) return none("no claim");
  const hasPercent = claim.percent !== null && claim.percent !== undefined;
  if (!hasPercent && !claim.status_text) return none("no report yet");
  const text = hasPercent ? `${claim.percent_kind === "assumed" ? "~" : ""}${claim.percent}%` : "";
  const age = typeof claim.progress_age === "number"
    ? h("span", {}, "last report ", h("span", { "data-progress-age": "" }, cellText({ type: "age" }, claim.progress_age + Math.floor(ageOffset()))), " ago")
    : "";
  return h(
    "span", { class: `claim-progress${claim.state === "stale" ? " percent-stale" : ""}` },
    hasPercent ? h("span", { class: "percent percent-large" }, percentBar({ percent: claim.percent, kind: claim.percent_kind }, text), h("span", { class: "progress-text" }, text)) : "",
    hasPercent ? h("span", { class: "muted" }, claim.percent_kind === "assumed" ? " assumed" : " measured") : "",
    claim.status_text ? h("span", { class: "progress-status" }, claim.status_text) : "",
    age,
  );
}

function claimElement(claim) {
  if (!claim) return none("unclaimed");
  const age = h("span", { "data-claim-age": "" }, cellText({ type: "age" }, claim.age + Math.floor(ageOffset())));
  return h("span", { class: "claim" }, sessionLink(claim.owner), `, slot ${claim.slot}, held `, age, ", ", claimBadge(claim.state), claim.note ? `, ${claim.note}` : "");
}

/** Blocked by and Audit rows of the task card, each only when the server has something to say. */
function taskAuditFacts(task) {
  const blocked = blockedByText(task.blocked_by, task.state);
  const audit = auditText(task.audit);
  return [...(blocked ? [fact("Blocked by", h("span", { class: "blocked-by" }, blocked))] : []), ...(audit ? [fact("Audit", audit)] : [])];
}

function taskElements(task) {
  const titles = task.ancestor_titles ?? {};
  const crumbs = [["Tasks", "/tasks"], ...(task.ancestors ?? []).map((id) => [`${id} ${titles[id] ?? ""}`.trim(), entityHref(id) || "/tasks"]), [task.id, ""]];
  const milestone = task.milestone
    ? [entityLink(task.milestone), " ", task.milestone_title, " ", task.milestone_priority ? badge("priority", task.milestone_priority) : ""]
    : none();
  const parent = task.parent ? [entityLink(task.parent), " ", titles[task.parent] ?? ""] : none();
  return [
    breadcrumb(crumbs),
    h(
      "section", { class: "card entity" },
      ...entityHeader(task.id, task.title, badge("state", task.state), task.priority ? badge("priority", task.priority) : ""),
      h("dl", { class: "facts" }, fact("Milestone", milestone), fact("Parent", parent), fact("Claim", claimElement(task.claim)), fact("Progress", progressElement(task.claim)), ...taskAuditFacts(task)),
    ),
    ...(task.children_table.rows.length ? [] : [h("section", { class: "card" }, h("h2", {}, "Children (0)"), h("p", { class: "muted" }, "No children"))]),
    ...(task.progress_table.rows.length ? [] : [h("section", { class: "card" }, h("h2", {}, "Progress reports (0)"), h("p", { class: "muted" }, "No progress reports"))]),
  ];
}

/** The entry card lives in `#tables`, below the children table, and is kept so a refresh does not re-attach the table. */
const entryCard = h("section", { class: "card" });

function entryElement(task) {
  entryCard.replaceChildren(
    h("h2", {}, "Entry"),
    h("pre", { class: "entry" }, (task.body ?? []).flatMap((line) => [...inlineNodes(line), "\n"])),
  );
  return entryCard;
}

function textFact(label, text) {
  return fact(label, text ? inlineNodes(text) : none());
}

function milestoneElements(milestone) {
  const { done, total } = milestone.progress;
  const percent = total > 0 ? Math.round((100 * done) / total) : 0;
  const meter = h(
    "span", { class: "meter" },
    h("progress", { max: Math.max(1, total), value: done, "aria-label": `${done} of ${total} done` }),
    h("span", { class: "progress-text" }, `${done}/${total} done, ${percent}%`),
  );
  return [
    breadcrumb([["Milestones", "/milestones"], [milestone.id, ""]]),
    h(
      "section", { class: "card entity" },
      ...entityHeader(milestone.id, milestone.title, milestone.priority ? badge("priority", milestone.priority) : "", badge("state", milestone.state)),
      h(
        "dl", { class: "facts" },
        fact("Progress", meter),
        fact("Ready", `${milestone.ready} ready task${milestone.ready === 1 ? "" : "s"}`),
        fact("Sessions", milestone.sessions?.length ? milestone.sessions.join(", ") : none()),
        textFact("Goal", milestone.goal),
        textFact("Now", milestone.now),
        textFact("Exit", milestone.exit),
      ),
    ),
  ];
}

function sessionElements(session) {
  const noClaims = session.live_claims + session.stale_claims === 0;
  const freshest = session.freshest_age === null || session.freshest_age === undefined
    ? none()
    : h("span", { "data-freshest-age": "" }, cellText({ type: "age" }, session.freshest_age + Math.floor(ageOffset())));
  const worker = session.worker
    ? h("span", {}, `${session.worker.backend}, generation ${session.worker.generation ?? "?"}, assigned `, session.worker.assigned.length ? session.worker.assigned.map((id, index) => [index ? ", " : "", entityLink(id)]) : none("nothing"))
    : none("not a fleet worker");
  return [
    breadcrumb([["Sessions", "/sessions"], [session.owner, ""]]),
    h(
      "section", { class: "card entity" },
      ...entityHeader("Session", session.owner, h("span", { class: `badge claim-${session.alive ? "live" : noClaims ? "none" : "stale"}` }, session.alive ? "live" : noClaims ? "no claims" : "stale")),
      h(
        "dl", { class: "facts" },
        fact("Family", session.family),
        fact("Live claims", String(session.live_claims)),
        fact("Stale claims", String(session.stale_claims)),
        fact("Last beat", freshest),
        fact("Worker", worker),
      ),
    ),
  ];
}

/** Show the tables of an entity page, keeping the state and scroll of the ones already there. */
function showEntityTables(id, errors, specs, extra = []) {
  const view = { id, errors: errors ?? {} };
  const tables = specs.map((spec) => {
    const existing = app.tables.get(spec.id);
    if (existing) {
      existing.view = view;
      existing.update(spec);
      return existing;
    }
    const created = new TableView(view, spec);
    app.tables.set(spec.id, created);
    return created;
  });
  mountTables(tables, extra);
}

function showEntity(payload) {
  const view = byId("view");
  const keepY = view.scrollTop;
  app.entity = { version: payload.version, payload };
  if (app.route.view === "task") {
    byId("entity").replaceChildren(...taskElements(payload));
    const specs = [payload.children_table, payload.progress_table].filter((spec) => spec.rows.length);
    for (const spec of [payload.children_table, payload.progress_table]) if (!spec.rows.length) app.tables.delete(spec.id);
    showEntityTables("task", payload.errors, specs, [entryElement(payload)]);
  } else if (app.route.view === "milestone") {
    byId("entity").replaceChildren(...milestoneElements(payload));
    showEntityTables("milestone", payload.errors, [payload.table]);
  } else {
    byId("entity").replaceChildren(...sessionElements(payload));
    showEntityTables("session", payload.errors, [payload.claims, payload.messages]);
  }
  document.title = app.route.view === "session" ? `${payload.owner}, Agent status` : `${payload.id} ${payload.title}, Agent status`;
  view.scrollTop = keepY;
}

function showNotFound(message) {
  app.tables = new Map();
  app.entity = null;
  byId("notes").replaceChildren();
  byId("tables").replaceChildren();
  byId("entity").replaceChildren(h(
    "section", { class: "card notfound" },
    h("h2", {}, "Not found"),
    h("p", {}, message),
    h("p", {}, h("a", { href: "/dashboard" }, "Back to the dashboard")),
  ));
  document.title = "Not found, Agent status";
}

// Tabs, header and routing.

const tabLabel = (id) => id[0].toUpperCase() + id.slice(1);
const SECTIONS = { task: "tasks", milestone: "milestones", session: "sessions" };
const sectionOf = (route) => SECTIONS[route.view] ?? route.view;

function buildTabs() {
  byId("tabs").replaceChildren(...VIEW_IDS.map((id, index) => h(
    "a",
    { class: "tab", id: `tab-${id}`, "data-view": id, href: routePath({ view: id }), title: `key ${index + 1}` },
    h("span", { class: "tab-key" }, String(index + 1)),
    tabLabel(id),
  )));
}

function updateTabs() {
  const section = sectionOf(app.route);
  for (const link of byId("tabs").children) {
    const current = link.getAttribute("data-view") === section;
    if (current) link.setAttribute("aria-current", app.route.view === section ? "page" : "true");
    else link.removeAttribute("aria-current");
  }
  byId("view").setAttribute("aria-labelledby", `tab-${section}`);
}

function closeAllPopovers() {
  for (const table of app.tables.values()) table.closePopover();
}

const NOT_FOUND_TEXT = {
  task: (id) => `${id} is not in the ledger.`,
  milestone: (id) => `${id} is not in the roadmap.`,
  session: (id) => `${id} is not a known session: it holds no claim and sent or received no message.`,
};
const sourceKey = (route) => (isEntity(route) ? routePath(route) : route.view === "notfound" ? "" : "state");

/** Show the page of the path: swap the tables, and restart polling when the data source changes. */
function showRoute(route) {
  const sourceChanged = sourceKey(app.route) !== sourceKey(route);
  app.route = route;
  closeAllPopovers();
  app.tables = new Map();
  app.tableStates = decodeState(location.search).tables;
  for (const id of ["notes", "entity", "tables"]) byId(id).replaceChildren();
  byId("view").scrollTop = 0;
  updateTabs();
  document.title = route.view === "notfound" || isEntity(route) ? "Agent status" : `${tabLabel(route.view)}, Agent status`;
  if (route.view === "notfound") {
    stopPolling();
    showNotFound(`There is no page at ${location.pathname}.`);
    updateHeader();
    return;
  }
  if (sourceChanged) {
    app.snapshot = null;
    app.version = "";
    app.entity = null;
    app.lastOkAt = 0;
    app.stale = false;
    if (isEntity(route)) byId("entity").replaceChildren(h("p", { class: "muted" }, "loading"));
    restartPolling();
  } else if (app.snapshot) {
    showList();
  }
  updateHeader();
}

/** Another query string on the page that is showing: tell every table, nothing is refetched. */
function restoreTables() {
  closeAllPopovers();
  app.tableStates = decodeState(location.search).tables;
  for (const table of app.tables.values()) table.restoreState();
}

function applyLocation() {
  if (location.pathname === app.path) {
    restoreTables();
    return;
  }
  app.path = location.pathname;
  showRoute(parseRoute(location.pathname));
}

/** Go to a same-origin URL without reloading. A query-only change replaces the entry, a new path adds one. */
function navigate(target) {
  const url = new URL(target, location.href);
  const samePath = url.pathname === location.pathname;
  history[samePath ? "replaceState" : "pushState"](null, "", url.pathname + url.search);
  applyLocation();
}

function updateHeader() {
  const seconds = Math.max(0, Math.floor((Date.now() - app.lastOkAt) / 1000));
  byId("updated").textContent = app.route.view === "notfound" ? "" : app.lastOkAt ? `last updated ${seconds}s ago` : "loading";
  const banner = byId("banner");
  let message = "";
  if (app.unreachable) message = app.lastOkAt ? `Server unreachable, showing data from ${seconds}s ago` : "Server unreachable";
  else if (app.stale) message = "Snapshot is stale, the server has not rebuilt it recently";
  banner.hidden = message === "";
  banner.textContent = message;
  const pause = byId("pause");
  pause.textContent = app.paused ? "Resume" : "Pause";
  pause.setAttribute("aria-pressed", String(app.paused));
}

function tick() {
  updateHeader();
  for (const table of app.tables.values()) table.tickAges();
  const payload = app.entity?.payload;
  tickAge("[data-claim-age]", payload?.claim?.age);
  tickAge("[data-progress-age]", payload?.claim?.progress_age);
  tickAge("[data-freshest-age]", payload?.freshest_age);
}

function tickAge(selector, base) {
  const cell = document.querySelector(selector);
  if (typeof base !== "number" || !cell) return;
  const text = cellText({ type: "age" }, base + Math.floor(ageOffset()));
  if (cell.textContent !== text) cell.textContent = text;
}

/** One conditional GET: unchanged (304), missing (404 or 400) or the new payload with the clock it arrived on. */
async function fetchVersioned(url, version) {
  const query = version ? `?since=${encodeURIComponent(version)}` : "";
  const response = await fetch(`${url}${query}`, { cache: "no-store" });
  if (response.status === 304) return { status: "unchanged" };
  if (response.status === 404 || response.status === 400) return { status: "missing" };
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const serverDate = Date.parse(response.headers.get("date") ?? "");
  const payload = await response.json();
  const serverNow = Number.isNaN(serverDate) ? Date.now() : serverDate;
  return { status: "ok", payload, baseElapsed: Math.max(0, serverNow / 1000 - payload.generated_at), receivedAt: performance.now() };
}

function fetchRoute(route) {
  if (route.view === "task") return fetchVersioned(`/api/task/${route.id}`, app.entity?.version ?? "");
  if (route.view === "milestone") return fetchVersioned(`/api/milestone/${route.id}`, app.entity?.version ?? "");
  if (route.view === "session") return fetchVersioned(`/api/session/${route.id}`, app.entity?.version ?? "");
  return fetchVersioned("/api/state", app.version);
}

function stopPolling() {
  app.generation += 1;
  clearTimeout(app.pollTimer);
}

/** Drop any poll in flight or scheduled and fetch now, once even when paused so a new page is not left empty. */
function restartPolling() {
  stopPolling();
  poll(true);
}

async function poll(force = false) {
  clearTimeout(app.pollTimer);
  if ((app.paused && !force) || app.route.view === "notfound") return;
  const { generation, route } = app;
  try {
    const result = await fetchRoute(route);
    if (generation !== app.generation) return;
    if (result.status === "ok") {
      app.baseElapsed = result.baseElapsed;
      app.receivedAt = result.receivedAt;
      if (isEntity(route)) showEntity(result.payload);
      else applySnapshot(result.payload);
      app.stale = app.baseElapsed > Math.max(STALE_FLOOR_SECONDS, 6 * app.interval);
    } else if (result.status === "missing") {
      if (!isEntity(route)) throw new Error("HTTP 404");
      showNotFound(NOT_FOUND_TEXT[route.view](route.id));
    }
    app.lastOkAt = Date.now();
    app.unreachable = false;
  } catch (error) {
    if (generation !== app.generation) return;
    app.unreachable = true;
    console.error("status poll failed", error);
  }
  updateHeader();
  if (!app.paused) app.pollTimer = setTimeout(poll, app.interval * 1000);
}

/** An entity page opened cold has not seen the snapshot yet: read the server's poll interval from it once. */
async function loadInterval() {
  try {
    const response = await fetch("/api/state", { cache: "no-store" });
    if (response.ok) app.interval = (await response.json()).interval ?? DEFAULT_INTERVAL;
  } catch (error) {
    console.error("interval lookup failed", error);
  }
}

function togglePause() {
  app.paused = !app.paused;
  updateHeader();
  stopPolling();
  if (!app.paused) poll();
}

function typingTarget(target) {
  return target instanceof HTMLElement && (target.matches("input, textarea, select") || target.isContentEditable);
}

function onKey(event) {
  if (event.ctrlKey || event.metaKey || event.altKey) return;
  if (event.key === "Escape") {
    const open = [...app.tables.values()].find((table) => table.popover);
    if (open) open.closePopover();
    return;
  }
  if (typingTarget(event.target)) return;
  if (event.key === "/") {
    const search = document.querySelector("#view input.search");
    if (search) {
      event.preventDefault();
      search.focus();
      search.select();
    }
    return;
  }
  const view = VIEW_IDS[Number(event.key) - 1];
  if (/^[1-9]$/.test(event.key) && view) navigate(routePath({ view }));
}

/** A plain left click on a same-origin page link navigates in place, everything else is the browser's. */
function followLink(event) {
  if (event.defaultPrevented || modifiedClick(event)) return;
  const link = event.target instanceof Element ? event.target.closest("a[href]") : null;
  if (!link || link.target === "_blank" || link.hasAttribute("download")) return;
  const url = new URL(link.href, location.href);
  if (url.origin !== location.origin || url.pathname.startsWith("/api/") || url.pathname.startsWith("/static/")) return;
  event.preventDefault();
  navigate(url.pathname + url.search);
}

function onDocumentClick(event) {
  followLink(event);
  const open = [...app.tables.values()].find((table) => table.popover);
  if (!open) return;
  const inside = open.popover.contains(event.target) || event.target.closest?.(".enum-button, .columns-button");
  if (!inside) open.closePopover();
}

function start() {
  // A legacy link keeps its state in the hash: `#tab=tasks&t.tasks.sort=...` becomes `/tasks?t.tasks.sort=...`.
  const legacy = legacyHashToUrl(location.hash, location.pathname);
  if (legacy) history.replaceState(null, "", legacy);
  else if (location.pathname === "/") history.replaceState(null, "", `/dashboard${location.search}`);
  buildTabs();
  byId("pause").addEventListener("click", togglePause);
  document.addEventListener("keydown", onKey);
  document.addEventListener("click", onDocumentClick);
  window.addEventListener("popstate", applyLocation);
  setInterval(tick, 1000);
  applyLocation();
  if (isEntity(app.route)) loadInterval();
}

start();
