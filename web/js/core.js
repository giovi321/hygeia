// Shared helpers: element builder, router, toast, confirm dialog, time formatting.
// Every file adds to the one global HY object; there are no modules.
var HY = window.HY = {
  routes: [],
  cleanups: [],
  token: 0,
  info: null,
  state: null,
  refreshFn: null,
  colors: ["#4FD1C5", "#F6C453", "#A78BFA", "#7CE38B", "#F38BA8",
           "#60A5FA", "#FB923C", "#E879F9", "#A3E635", "#F87171"]
};

// h("div", {class: "x", onclick: fn}, "text", child, [more]) builds an element.
// Strings become text nodes, so device data can never inject markup.
HY.h = function (tag, attrs) {
  var el = document.createElement(tag);
  var k, v;
  if (attrs) {
    for (k in attrs) {
      v = attrs[k];
      if (v === null || v === undefined || v === false) { continue; }
      if (k === "class") { el.className = v; }
      else if (k === "text") { el.textContent = v; }
      else if (k === "style") { for (var s in v) { el.style.setProperty(s, v[s]); } }
      else if (k.indexOf("on") === 0 && typeof v === "function") { el.addEventListener(k.slice(2), v); }
      else if (v === true) { el.setAttribute(k, ""); }
      else if (k === "value") { el.value = v; }
      else { el.setAttribute(k, String(v)); }
    }
  }
  for (var i = 2; i < arguments.length; i++) { HY._add(el, arguments[i]); }
  return el;
};

HY._add = function (el, c) {
  if (c === null || c === undefined || c === false) { return; }
  if (Array.isArray(c)) { for (var i = 0; i < c.length; i++) { HY._add(el, c[i]); } }
  else if (c.nodeType) { el.appendChild(c); }
  else { el.appendChild(document.createTextNode(String(c))); }
};

HY.clear = function (el) { while (el.firstChild) { el.removeChild(el.firstChild); } };

// ---- routing -------------------------------------------------------------

// HY.route("/meds/:id", function (params, view) { ...; return refreshFn; }, "meds")
// The last argument names the menu entry to highlight.
HY.route = function (pattern, render, nav) {
  HY.routes.push({ parts: pattern.split("/").slice(1), render: render, nav: nav || "" });
};

HY.onLeave = function (fn) { HY.cleanups.push(fn); };

HY.path = function () {
  var p = (location.hash || "#/").slice(1).split("?")[0];
  return p === "" ? "/" : p;
};

HY.go = function (path) {
  if (location.hash === "#" + path) { HY.navigate(); } else { location.hash = "#" + path; }
};

HY.navigate = function () {
  var path = HY.path();
  if (HY.info && !HY.info.paired && path !== "/pair") { location.hash = "#/pair"; return; }
  var segs = path.split("/").slice(1);
  var hit = null, params = {};
  for (var i = 0; i < HY.routes.length && !hit; i++) {
    var r = HY.routes[i];
    if (r.parts.length !== segs.length) { continue; }
    var p = {}, ok = true;
    for (var j = 0; j < segs.length; j++) {
      if (r.parts[j].charAt(0) === ":") { p[r.parts[j].slice(1)] = decodeURIComponent(segs[j]); }
      else if (r.parts[j] !== segs[j]) { ok = false; break; }
    }
    if (ok) { hit = r; params = p; }
  }
  HY.token++;
  var c = HY.cleanups; HY.cleanups = [];
  c.forEach(function (f) { try { f(); } catch (e) { /* ignore */ } });
  HY.refreshFn = null;
  var view = document.getElementById("view");
  HY.clear(view);
  document.body.classList.toggle("unpaired", !!(HY.info && !HY.info.paired));
  var nav = hit ? hit.nav : "";
  var links = document.querySelectorAll("#menu a");
  for (var n = 0; n < links.length; n++) {
    links[n].classList.toggle("on", links[n].getAttribute("data-nav") === nav);
  }
  window.scrollTo(0, 0);
  if (!hit) {
    view.appendChild(HY.h("h1", null, "Not found"));
    view.appendChild(HY.h("p", { class: "mut" }, "This page does not exist."));
    return;
  }
  var res = hit.render(params, view);
  if (typeof res === "function") { HY.refreshFn = res; }
};

// True while the page that started an async step is still the one on screen.
HY.alive = function (token) { return token === HY.token; };

// Refetch the data of the current page (WebSocket events, failed writes).
HY.refresh = function () { if (HY.refreshFn) { HY.refreshFn(); } };

// ---- toast and confirm ---------------------------------------------------

HY.toast = function (text, undoFn, ms) {
  var box = document.getElementById("toasts");
  var t;
  var gone = function () { if (t.parentNode) { t.parentNode.removeChild(t); } };
  t = HY.h("div", { class: "toast", role: "status" }, HY.h("span", null, text));
  if (undoFn) {
    t.appendChild(HY.h("button", { class: "btn", type: "button", onclick: function () {
      gone();
      undoFn();
    } }, "Undo"));
  }
  box.appendChild(t);
  setTimeout(gone, ms || (undoFn ? 5000 : 4000));
};

// Resolves true when the person confirms, false on cancel.
HY.confirm = function (title, body, danger, okLabel) {
  return new Promise(function (resolve) {
    var prev = document.activeElement;
    var done = function (v) {
      document.removeEventListener("keydown", onKey);
      if (back.parentNode) { back.parentNode.removeChild(back); }
      if (prev && prev.focus) { prev.focus(); }
      resolve(v);
    };
    var onKey = function (e) { if (e.key === "Escape") { done(false); } };
    var cancel = HY.h("button", { class: "btn", type: "button", onclick: function () { done(false); } }, "Cancel");
    var ok = HY.h("button", { class: "btn " + (danger ? "danger" : "p"), type: "button",
                              onclick: function () { done(true); } }, okLabel || (danger ? "Confirm" : "OK"));
    var back = HY.h("div", { class: "modal-back" },
      HY.h("div", { class: "modal", role: "dialog", "aria-modal": "true", "aria-label": title },
        HY.h("h2", null, title),
        body ? HY.h("p", null, body) : null,
        HY.h("div", { class: "actions" }, cancel, ok)));
    document.body.appendChild(back);
    document.addEventListener("keydown", onKey);
    cancel.focus();
  });
};

// ---- time ----------------------------------------------------------------

HY._fmt = {};
HY._formatter = function (zone, opts, key) {
  var k = key + "|" + (zone || "");
  if (!HY._fmt[k]) {
    var o = {}, x;
    for (x in opts) { o[x] = opts[x]; }
    if (zone) { o.timeZone = zone; }
    try { HY._fmt[k] = new Intl.DateTimeFormat("en-GB", o); }
    catch (e) { delete o.timeZone; HY._fmt[k] = new Intl.DateTimeFormat("en-GB", o); }
  }
  return HY._fmt[k];
};

// Epoch seconds as local time in the device's zone: "08:30", or "Wed 08:30" on another day.
HY.fmtTime = function (utc, zone) {
  if (!utc) { return ""; }
  var d = new Date(utc * 1000);
  var hm = HY._formatter(zone, { hour: "2-digit", minute: "2-digit", hour12: false }, "hm").format(d);
  var day = HY._formatter(zone, { year: "numeric", month: "2-digit", day: "2-digit" }, "d");
  if (day.format(d) === day.format(new Date())) { return hm; }
  return HY._formatter(zone, { weekday: "short" }, "w").format(d) + " " + hm;
};

HY.fmtAgo = function (utc) {
  var s = Math.floor(Date.now() / 1000) - utc;
  if (s < 90) { return "just now"; }
  if (s < 3600) { return Math.round(s / 60) + " min ago"; }
  if (s < 86400) { return Math.floor(s / 3600) + " h " + Math.round((s % 3600) / 60) + " min ago"; }
  return Math.floor(s / 86400) + " d ago";
};

HY.medColor = function (i) { return HY.colors[(i | 0) % 10] || HY.colors[0]; };
