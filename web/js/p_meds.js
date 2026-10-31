// Medicines: list, reorder with buttons, archive, unarchive, delete.
HY.DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];

HY.schedText = function (m) {
  var s = m.schedule || {};
  if (s.type === "as_needed") {
    return "As needed" + (s.min_interval_min ? ", at least " + HY.fmtMinutes(s.min_interval_min) + " apart" : "");
  }
  var days = [];
  for (var i = 0; i < 7; i++) { if (s.weekdays & (1 << i)) { days.push(HY.DAYS[i]); } }
  var d = days.length === 7 ? "every day" : days.join(", ");
  return (s.times || []).join(", ") + (d ? ", " + d : "");
};

HY.fmtMinutes = function (n) {
  if (n >= 60 && n % 60 === 0) { return (n / 60) + " h"; }
  return n + " min";
};

HY.route("/meds", function (params, view) {
  var token = HY.token;
  var root = HY.h("div");
  var meds = [];       // all medicines from the device
  var order = null;    // ids of active medicines in the edited order, null when not edited
  var busy = false;
  view.appendChild(HY.h("div", { class: "head" },
    HY.h("h1", null, "Medicines"),
    HY.h("a", { class: "btn p", href: "#/meds/new" }, "Add medicine")));
  view.appendChild(root);

  function load() {
    if (order) { return Promise.resolve(); }   // keep an unsaved order on screen
    return HY.api("GET", "/api/medicines").then(function (r) {
      if (!HY.alive(token)) { return; }
      meds = r.meds || [];
      draw();
    }, function (e) {
      if (HY.alive(token)) { root.textContent = HY.errText(e); }
    });
  }

  function byId(id) { for (var i = 0; i < meds.length; i++) { if (meds[i].id === id) { return meds[i]; } } return null; }

  function activeList() {
    var act = meds.filter(function (m) { return !m.archived; });
    act.sort(function (a, b) { return a.order - b.order; });
    if (order) { return order.map(byId).filter(Boolean); }
    return act;
  }

  function move(i, d) {
    var ids = activeList().map(function (m) { return m.id; });
    var j = i + d;
    if (j < 0 || j >= ids.length) { return; }
    var t = ids[i]; ids[i] = ids[j]; ids[j] = t;
    order = ids;
    draw();
  }

  function saveOrder() {
    busy = true;
    HY.api("PUT", "/api/medicines/order", { ids: order }).then(function () {
      busy = false; order = null;
      HY.toast("Order saved");
      load();
    }, function (e) {
      busy = false;
      var t = HY.errText(e); if (t) { HY.toast(t); }
    });
  }

  function post(m, what, text) {
    if (busy) { return; }
    busy = true;
    HY.api("POST", "/api/medicines/" + m.id + "/" + what).then(function () {
      busy = false; HY.toast(text); order = null; load();
    }, function (e) {
      busy = false;
      var t = HY.errText(e); if (t) { HY.toast(t); }
    });
  }

  function archive(m) {
    HY.confirm("Archive " + m.name + "?",
      HY.ARCHIVE_BODY, true, "Archive")
      .then(function (yes) { if (yes) { post(m, "archive", m.name + " archived"); } });
  }

  function unarchive(m) {
    HY.confirm("Unarchive " + m.name + "?", "It returns to the home screen and reminds again", false, "Unarchive")
      .then(function (yes) { if (yes) { post(m, "unarchive", m.name + " is active again"); } });
  }

  function remove(m) {
    var done = function () { busy = false; HY.toast(m.name + " deleted"); load(); };
    var fail = function (e) { busy = false; var t = HY.errText(e); if (t) { HY.toast(t); } load(); };
    if (busy) { return; }
    busy = true;
    HY.api("DELETE", "/api/medicines/" + m.id).then(done, function (e) {
      if (e.status === 409 && e.data && typeof e.data.log_entries === "number") {
        busy = false;
        var n = e.data.log_entries;
        HY.confirm("Delete " + m.name + "?",
          "This also deletes " + n + (n === 1 ? " log entry" : " log entries") + ". It cannot be undone.",
          true, "Delete").then(function (yes) {
            if (!yes) { return; }
            busy = true;
            HY.api("DELETE", "/api/medicines/" + m.id + "?confirm=1").then(done, fail);
          });
        return;
      }
      fail(e);
    });
  }

  function cell(label, content, cls) {
    return HY.h("td", { "data-label": label, class: cls || "" }, content);
  }

  function row(m, i, n, canMove) {
    var color = HY.medColor(m.color);
    var mv = canMove ? HY.h("div", { class: "mv" },
      HY.h("button", { class: "btn sq", type: "button", "aria-label": "Move " + m.name + " up",
        disabled: i === 0, onclick: function () { move(i, -1); } }, "▲"),
      HY.h("button", { class: "btn sq", type: "button", "aria-label": "Move " + m.name + " down",
        disabled: i === n - 1, onclick: function () { move(i, 1); } }, "▼")) : null;
    var acts = m.archived ?
      HY.h("div", { class: "actions" },
        HY.h("button", { class: "btn", type: "button", onclick: function () { unarchive(m); } }, "Unarchive"),
        HY.h("button", { class: "btn danger", type: "button", onclick: function () { remove(m); } }, "Delete")) :
      HY.h("div", { class: "actions" },
        HY.h("a", { class: "btn", href: "#/meds/" + m.id }, "Edit"),
        HY.h("button", { class: "btn", type: "button", onclick: function () { archive(m); } }, "Archive"));
    return HY.h("tr", null,
      canMove ? cell("Order", mv, "c-order") : null,
      cell("Code", HY.h("b", { style: { color: color } }, m.code), "c-code"),
      cell("Name", m.name, "c-name"),
      cell("Dose", m.dose || "", "c-dose"),
      cell("Schedule", HY.schedText(m), "c-sched"),
      cell("", acts, "c-act"));
  }

  function table(list, canMove) {
    var head = HY.h("tr", null, canMove ? HY.h("th", null, "Order") : null,
      HY.h("th", null, "Code"), HY.h("th", null, "Name"), HY.h("th", null, "Dose"),
      HY.h("th", null, "Schedule"), HY.h("th", null, ""));
    var rows = list.map(function (m, i) { return row(m, i, list.length, canMove); });
    return HY.h("table", { class: "tbl" }, HY.h("thead", null, head), HY.h("tbody", null, rows));
  }

  function draw() {
    HY.clear(root);
    var act = activeList();
    var arch = meds.filter(function (m) { return m.archived; });
    if (order) {
      root.appendChild(HY.h("div", { class: "banner" },
        HY.h("p", null, "The new order is not saved yet."),
        HY.h("div", { class: "actions" },
          HY.h("button", { class: "btn p", type: "button", onclick: saveOrder }, "Save order"),
          HY.h("button", { class: "btn", type: "button", onclick: function () { order = null; load(); } }, "Discard"))));
    }
    if (act.length) {
      root.appendChild(HY.h("div", { class: "panel flush" }, table(act, true)));
    } else {
      root.appendChild(HY.h("p", { class: "mut" }, "No active medicines yet."));
    }
    if (arch.length) {
      root.appendChild(HY.h("details", { class: "panel arch" },
        HY.h("summary", null, "Archived (" + arch.length + ")"),
        table(arch, false)));
    }
  }

  load();
  return load;
}, "meds");
