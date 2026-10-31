// Log: entries newest first, filters, edit, delete, add a past dose.
(function () {
  var DAY = 86400;

  // Own dialog (HY.confirm has no inputs). Resolves with the result of build()'s read(), or null on cancel.
  function dialog(title, bodyEls, okLabel, onOk) {
    var prev = document.activeElement;
    var err = HY.h("div", { class: "ferr", role: "alert" });
    var back, ok;
    function close() {
      document.removeEventListener("keydown", onKey);
      if (back.parentNode) { back.parentNode.removeChild(back); }
      if (prev && prev.focus) { prev.focus(); }
    }
    function onKey(e) { if (e.key === "Escape") { close(); } }
    ok = HY.h("button", { class: "btn p", type: "button", onclick: function () {
      err.textContent = "";
      ok.disabled = true;
      onOk().then(function (res) {
        if (res === true) { close(); } else { ok.disabled = false; err.textContent = res || ""; }
      });
    } }, okLabel);
    back = HY.h("div", { class: "modal-back" },
      HY.h("div", { class: "modal", role: "dialog", "aria-modal": "true", "aria-label": title },
        HY.h("h2", null, title), bodyEls, err,
        HY.h("div", { class: "actions" },
          HY.h("button", { class: "btn", type: "button", onclick: close }, "Cancel"), ok)));
    document.body.appendChild(back);
    document.addEventListener("keydown", onKey);
    var first = back.querySelector("input, select");
    if (first) { first.focus(); }
  }

  function lab(text, id, input) {
    return HY.h("div", { class: "field" }, HY.h("label", { for: id }, text), input);
  }

  HY.route("/log", function (params, view) {
    var token = HY.token;
    var root = HY.h("div");
    var zone = "UTC";
    var meds = [];
    var rows = [];          // loaded entries, ascending by (ts, id)
    var lo = 0, hi = 0;     // loaded window [lo, hi)
    var f = { med: "", from: "", to: "" };
    var loading = false;
    var tableBox = HY.h("div");
    var moreBox = HY.h("div");
    var msg = HY.h("p", { class: "msg bad" });

    view.appendChild(HY.h("div", { class: "head" }, HY.h("h1", null, "Log"),
      HY.h("button", { class: "btn p", type: "button", onclick: addPast }, "Add a past dose")));
    view.appendChild(HY.histTabs("log"));
    view.appendChild(root);

    function medById(id) {
      for (var i = 0; i < meds.length; i++) { if (meds[i].id === id) { return meds[i]; } }
      return null;
    }

    // ---- loading: ascending pages by cursor, shown newest first ----
    function fetchRange(from, to) {
      var out = [];
      function page(after) {
        var q = "/api/log?from=" + from + "&to=" + to + "&limit=200" + (f.med ? "&med=" + f.med : "") +
          (after ? "&after=" + encodeURIComponent(after) : "");
        return HY.api("GET", q).then(function (r) {
          out = out.concat(r.log || []);
          if (r.next && out.length < 5000) { return page(r.next); }
          return out;
        });
      }
      return page(null);
    }

    function windowReset() {
      var now = Math.floor(Date.now() / 1000);
      hi = f.to ? HY.wallToEpoch(HY.addDays(f.to, 1), "00:00", zone) : now + 60;
      lo = f.from ? HY.wallToEpoch(f.from, "00:00", zone) : hi - 7 * DAY;
      if (lo >= hi) { lo = hi - DAY; }
    }

    function loadAll() {
      if (loading) { return Promise.resolve(); }
      loading = true;
      windowReset();
      return fetchRange(lo, hi).then(function (list) {
        loading = false;
        if (!HY.alive(token)) { return; }
        rows = list; msg.textContent = "";
        draw();
      }, function (e) {
        loading = false;
        if (HY.alive(token)) { msg.textContent = HY.errText(e); }
      });
    }

    function loadMore() {
      if (loading) { return; }
      loading = true;
      var newLo = lo - 30 * DAY;
      fetchRange(newLo, lo).then(function (list) {
        loading = false;
        if (!HY.alive(token)) { return; }
        rows = list.concat(rows); lo = newLo; msg.textContent = "";
        draw();
      }, function (e) {
        loading = false;
        if (HY.alive(token)) { msg.textContent = HY.errText(e); }
      });
    }

    // ---- rendering ----
    function cell(label, content, cls) { return HY.h("td", { "data-label": label, class: cls || "" }, content); }

    function rowEl(e) {
      var m = medById(e.med_id);
      return HY.h("tr", null,
        cell("When", e.local || HY.fmtTime(e.ts, zone), "c-when"),
        cell("Medicine", m ? [HY.h("b", { style: { color: HY.medColor(m.color) } }, m.code), " " + m.name] : "Medicine " + e.med_id, "c-name"),
        cell("Status", HY.statusText(e.status) + (e.edited ? " (edited)" : ""), "c-status"),
        cell("Note", e.note || "", "c-note"),
        cell("", HY.h("div", { class: "actions" },
          HY.h("button", { class: "btn", type: "button", onclick: function () { edit(e); } }, "Edit"),
          HY.h("button", { class: "btn danger", type: "button", onclick: function () { remove(e); } }, "Delete")), "c-act"));
    }

    function draw() {
      HY.clear(tableBox);
      HY.clear(moreBox);
      var list = rows.slice().reverse();
      if (!list.length) {
        tableBox.appendChild(HY.h("p", { class: "mut" }, "No entries in this period."));
      } else {
        tableBox.appendChild(HY.h("div", { class: "panel flush" }, HY.h("table", { class: "tbl" },
          HY.h("thead", null, HY.h("tr", null, HY.h("th", null, "When"), HY.h("th", null, "Medicine"),
            HY.h("th", null, "Status"), HY.h("th", null, "Note"), HY.h("th", null, ""))),
          HY.h("tbody", null, list.map(rowEl)))));
      }
      if (!f.from && lo > 1700000000) {
        moreBox.appendChild(HY.h("button", { class: "btn", type: "button", onclick: loadMore }, "Load more"));
      }
    }

    function filters() {
      var sel = HY.h("select", { id: "fl-med", onchange: function () { f.med = sel.value; loadAll(); } },
        HY.h("option", { value: "" }, "All medicines"));
      meds.forEach(function (m) {
        sel.appendChild(HY.h("option", { value: String(m.id) }, m.code + " " + m.name + (m.archived ? " (archived)" : "")));
      });
      sel.value = f.med;
      var from = HY.h("input", { id: "fl-from", type: "date", value: f.from, onchange: function () { f.from = from.value; loadAll(); } });
      var to = HY.h("input", { id: "fl-to", type: "date", value: f.to, onchange: function () { f.to = to.value; loadAll(); } });
      return HY.h("div", { class: "panel filters" },
        lab("Medicine", "fl-med", sel), lab("From", "fl-from", from), lab("To", "fl-to", to),
        HY.h("button", { class: "btn", type: "button", onclick: function () {
          f = { med: "", from: "", to: "" }; sel.value = ""; from.value = ""; to.value = ""; loadAll();
        } }, "Clear filters"));
    }

    // ---- edit ----
    function edit(e) {
      var dateIn = HY.h("input", { id: "le-date", type: "date", value: (e.local || "").slice(0, 10) });
      var timeIn = HY.h("input", { id: "le-time", type: "time", value: (e.local || "").slice(11, 16) });
      var st = HY.h("select", { id: "le-status" },
        ["taken", "skipped", "missed"].map(function (s) { return HY.h("option", { value: s }, HY.statusText(s)); }));
      st.value = e.status;
      var note = HY.h("input", { id: "le-note", type: "text", maxlength: "63", value: e.note || "", autocomplete: "off" });
      var m = medById(e.med_id);
      var origLocal = (e.local || "").slice(0, 16);
      dialog("Edit entry" + (m ? ", " + m.name : ""), [
        HY.h("div", { class: "row2" }, lab("Date", "le-date", dateIn), lab("Time", "le-time", timeIn)),
        lab("Status", "le-status", st), lab("Note", "le-note", note)], "Save", function () {
        var body = { orig_ts: e.ts, status: st.value, note: note.value };
        if (dateIn.value + " " + timeIn.value !== origLocal) {
          if (!dateIn.value || !timeIn.value) { return Promise.resolve("Enter a date and a time"); }
          body.ts = HY.wallToEpoch(dateIn.value, timeIn.value, zone);
        }
        return HY.api("PUT", "/api/log/" + e.id, body).then(function () {
          HY.toast("Entry saved");
          loadAll();
          return true;
        }, function (er) { return HY.errText(er) || false; });
      });
    }

    function remove(e) {
      var m = medById(e.med_id);
      HY.confirm("Delete this entry?", (m ? m.name + ", " : "") + (e.local || "") + ". It cannot be undone.", true, "Delete")
        .then(function (yes) {
          if (!yes) { return; }
          HY.api("DELETE", "/api/log/" + e.id + "?ts=" + e.ts).then(function () {
            HY.toast("Entry deleted");
            loadAll();
          }, function (er) {
            var t = HY.errText(er); if (t) { HY.toast(t); }
            loadAll();
          });
        });
    }

    // ---- add a past dose ----
    function addPast() {
      var active = meds.filter(function (m) { return !m.archived; });
      if (!active.length) { HY.toast("Add a medicine first"); return; }
      var sel = HY.h("select", { id: "la-med" }, active.map(function (m) {
        return HY.h("option", { value: String(m.id) }, m.code + " " + m.name);
      }));
      var now = Math.floor(Date.now() / 1000);
      var dateIn = HY.h("input", { id: "la-date", type: "date", value: HY.epochToDate(now, zone) });
      var timeIn = HY.h("input", { id: "la-time", type: "time", value: "" });
      var note = HY.h("input", { id: "la-note", type: "text", maxlength: "63", autocomplete: "off" });
      dialog("Add a past dose", [
        lab("Medicine", "la-med", sel),
        HY.h("div", { class: "row2" }, lab("Date", "la-date", dateIn), lab("Time", "la-time", timeIn)),
        lab("Note", "la-note", note)], "Add", function () {
        if (!dateIn.value || !timeIn.value) { return Promise.resolve("Enter a date and a time"); }
        var ts = HY.wallToEpoch(dateIn.value, timeIn.value, zone);
        if (ts > Math.floor(Date.now() / 1000) + 60) { return Promise.resolve("That time is in the future"); }
        var body = { med_id: parseInt(sel.value, 10), ts: ts };
        if (note.value) { body.note = note.value; }
        return HY.api("POST", "/api/log", body).then(function () {
          HY.toast("Dose added");
          loadAll();
          return true;
        }, function (er) { return HY.errText(er) || false; });
      });
    }

    root.appendChild(msg);
    Promise.all([HY.deviceZone(), HY.api("GET", "/api/medicines")]).then(function (r) {
      if (!HY.alive(token)) { return; }
      zone = r[0]; meds = r[1].meds || [];
      HY.clear(root);
      root.appendChild(msg);
      root.appendChild(filters());
      root.appendChild(tableBox);
      root.appendChild(moreBox);
      return loadAll();
    }, function (e) {
      if (HY.alive(token)) { msg.textContent = HY.errText(e) || "Could not load the log"; }
    });

    return function () { if (meds.length) { return loadAll(); } };
  }, "history");
})();
