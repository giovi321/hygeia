// Add or edit a medicine.
(function () {
  function dateToInt(s) {
    return s ? parseInt(s.replace(/-/g, ""), 10) : 0;
  }
  function intToDate(n) {
    if (!n) { return ""; }
    var s = String(n);
    return s.slice(0, 4) + "-" + s.slice(4, 6) + "-" + s.slice(6, 8);
  }

  function blank() {
    return {
      code: "", name: "", dose: "", color: 0,
      schedule: { type: "fixed", times: ["08:00"], weekdays: 127, min_interval_min: 0 },
      early_window_min: 60, late_after_min: 30, missed_after_min: 120,
      pill_count: { enabled: false, remaining: 0, alert: 0 },
      archived: false
    };
  }

  function render(params, view) {
    var token = HY.token;
    var isNew = params.id === "new";
    view.appendChild(HY.h("h1", null, isNew ? "Add medicine" : "Edit medicine"));
    var holder = HY.h("div");
    view.appendChild(holder);
    var jobs = [HY.api("GET", "/api/medicines")];
    if (!isNew) { jobs.push(HY.api("GET", "/api/medicines/" + encodeURIComponent(params.id))); }
    Promise.all(jobs).then(function (r) {
      if (!HY.alive(token)) { return; }
      build(holder, isNew ? blank() : r[1], r[0].meds || [], isNew, token);
    }, function (e) {
      if (HY.alive(token)) { holder.appendChild(HY.h("p", { class: "msg bad" }, HY.errText(e) || "Could not load the medicine")); }
    });
  }

  function build(holder, med, all, isNew, token) {
    var errs = {};
    var times = (med.schedule.times || []).slice();
    var type = med.schedule.type;
    var color = med.color;
    var days = med.schedule.weekdays;
    var top = HY.h("div");

    function field(name, label, input, hint) {
      var err = HY.h("div", { class: "ferr", "data-err": name, role: "alert" });
      errs[name] = err;
      return HY.h("div", { class: "field" }, HY.h("label", { for: "f-" + name }, label), input, hint ? HY.h("div", { class: "hint" }, hint) : null, err);
    }
    function text(name, value, extra) {
      var a = { id: "f-" + name, name: name, type: "text", value: value };
      for (var k in extra) { a[k] = extra[k]; }
      return HY.h("input", a);
    }
    function num(name, value, extra) {
      var a = { id: "f-" + name, name: name, type: "number", inputmode: "numeric", min: "0", step: "1", value: String(value) };
      for (var k in extra) { a[k] = extra[k]; }
      return HY.h("input", a);
    }

    var code = text("code", med.code, { maxlength: "2", autocomplete: "off", autocapitalize: "characters", class: "code-sm" });
    code.addEventListener("input", function () {
      code.value = code.value.toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 2);
    });
    var name = text("name", med.name, { maxlength: "32", autocomplete: "off" });
    var dose = text("dose", med.dose, { maxlength: "16", autocomplete: "off", placeholder: "1 tab" });

    // colour swatches, with the code of any active medicine already using the colour
    var swatches = HY.h("div", { class: "swatches", role: "radiogroup", "aria-label": "Colour" });
    function drawSwatches() {
      HY.clear(swatches);
      for (var i = 0; i < 10; i++) {
        (function (i) {
          var used = all.filter(function (m) { return !m.archived && m.color === i && m.id !== med.id; })
            .map(function (m) { return m.code; });
          var b = HY.h("button", { type: "button", class: "sw" + (color === i ? " on" : ""), role: "radio",
            "aria-checked": color === i ? "true" : "false",
            "aria-label": "Colour " + (i + 1) + (used.length ? ", used by " + used.join(", ") : ""),
            style: { "--c": HY.medColor(i) }, onclick: function () { color = i; drawSwatches(); } }, used.join(" "));
          swatches.appendChild(b);
        })(i);
      }
    }
    drawSwatches();

    // schedule type
    var typeBox = HY.h("div", { class: "seg", role: "radiogroup", "aria-label": "Schedule type" });
    var timesBox = HY.h("div", { class: "times" });
    var fixedBox = HY.h("div");
    var asNeededBox = HY.h("div");
    var daysBox = HY.h("div", { class: "days" });

    function drawType() {
      HY.clear(typeBox);
      [["fixed", "Fixed times"], ["as_needed", "As needed"]].forEach(function (t) {
        typeBox.appendChild(HY.h("button", { type: "button", class: "chip" + (type === t[0] ? " on" : ""), role: "radio",
          "aria-checked": type === t[0] ? "true" : "false",
          onclick: function () { type = t[0]; drawType(); } }, t[1]));
      });
      fixedBox.hidden = type !== "fixed";
      asNeededBox.hidden = type !== "as_needed";
    }

    function drawTimes() {
      HY.clear(timesBox);
      times.forEach(function (t, i) {
        var inp = HY.h("input", { type: "time", value: t, required: true, "aria-label": "Time " + (i + 1) });
        inp.addEventListener("input", function () { times[i] = inp.value; });
        timesBox.appendChild(HY.h("div", { class: "trow" }, inp,
          HY.h("button", { type: "button", class: "btn", "aria-label": "Remove time " + (i + 1),
            disabled: times.length <= 1, onclick: function () { times.splice(i, 1); drawTimes(); } }, "Remove")));
      });
      addTime.disabled = times.length >= 8;
    }
    var addTime = HY.h("button", { type: "button", class: "btn", onclick: function () {
      times.push("12:00"); drawTimes();
    } }, "Add time");

    function drawDays() {
      HY.clear(daysBox);
      HY.DAYS.forEach(function (d, i) {
        var on = (days & (1 << i)) !== 0;
        daysBox.appendChild(HY.h("button", { type: "button", class: "chip day" + (on ? " on" : ""),
          "aria-pressed": on ? "true" : "false", onclick: function () { days ^= (1 << i); drawDays(); } }, d));
      });
    }

    var interval = num("min_interval_min", med.schedule.min_interval_min || 0);
    var early = num("early_window_min", med.early_window_min);
    var late = num("late_after_min", med.late_after_min);
    var missed = num("missed_after_min", med.missed_after_min);
    var pillsOn = HY.h("input", { id: "f-pills", type: "checkbox", checked: med.pill_count.enabled ? true : null });
    var remaining = num("remaining", med.pill_count.remaining);
    var alertAt = num("alert", med.pill_count.alert);
    var from = HY.h("input", { id: "f-active_from", name: "active_from", type: "date", value: intToDate(med.active_from) });
    var until = HY.h("input", { id: "f-active_until", name: "active_until", type: "date", value: intToDate(med.active_until) });

    fixedBox.appendChild(HY.h("div", { class: "lbl" }, "Times"));
    fixedBox.appendChild(timesBox);
    fixedBox.appendChild(addTime);
    var te = HY.h("div", { class: "ferr", "data-err": "times", role: "alert" });
    errs.times = te;
    fixedBox.appendChild(te);
    fixedBox.appendChild(HY.h("div", { class: "lbl" }, "Days"));
    fixedBox.appendChild(daysBox);
    var we = HY.h("div", { class: "ferr", "data-err": "weekdays", role: "alert" });
    errs.weekdays = we;
    fixedBox.appendChild(we);
    asNeededBox.appendChild(field("min_interval_min", "Warn when logged sooner than (minutes)", interval));

    drawType(); drawTimes(); drawDays();

    function showErrors(list) {
      for (var k in errs) { errs[k].textContent = ""; }
      var first = null;
      list.forEach(function (e) {
        var el = errs[e.field] || null;
        if (el) { el.textContent = e.message; if (!first) { first = el; } }
        else { top.appendChild(HY.h("p", { class: "msg bad", role: "alert" }, e.message)); }
      });
      if (first) { first.scrollIntoView({ block: "center" }); }
    }

    function intOf(el, label) {
      var s = el.value.trim();
      if (!/^[0-9]{1,5}$/.test(s)) { return { field: el.name, message: label + " must be a whole number" }; }
      return parseInt(s, 10);
    }

    function gather() {
      var bad = [];
      var chk = function (el, label) {
        var v = intOf(el, label);
        if (typeof v === "object") { bad.push(v); return 0; }
        return v;
      };
      var body = {
        code: code.value.toUpperCase(),
        name: name.value.trim(),
        dose: dose.value.trim(),
        color: color,
        schedule: {
          type: type,
          times: type === "fixed" ? times.slice() : [],
          weekdays: days,
          min_interval_min: type === "as_needed" ? chk(interval, "The interval") : 0
        },
        early_window_min: chk(early, "The early window"),
        late_after_min: chk(late, "Late after"),
        missed_after_min: chk(missed, "Missed after"),
        pill_count: { enabled: pillsOn.checked, remaining: chk(remaining, "The remaining count"), alert: chk(alertAt, "The alert level") },
        active_from: dateToInt(from.value),
        active_until: dateToInt(until.value)
      };
      if (!body.code) { bad.push({ field: "code", message: "Enter a code of 1 or 2 letters or digits" }); }
      if (!body.name) { bad.push({ field: "name", message: "Enter a name" }); }
      if (type === "fixed") {
        if (times.some(function (t) { return !t; })) { bad.push({ field: "times", message: "Fill in every time" }); }
        if (!days) { bad.push({ field: "weekdays", message: "Pick at least one day" }); }
      }
      return { body: body, bad: bad };
    }

    var save = HY.h("button", { class: "btn p", type: "submit" }, "Save");
    var form = HY.h("form", { novalidate: true, class: "mform" });
    form.addEventListener("submit", function (ev) {
      ev.preventDefault();
      HY.clear(top);
      var g = gather();
      showErrors(g.bad);
      if (g.bad.length) { return; }
      save.disabled = true;
      var p = isNew ? HY.api("POST", "/api/medicines", g.body)
                    : HY.api("PUT", "/api/medicines/" + med.id, g.body, { "If-Match": med.etag });
      p.then(function () {
        HY.toast(isNew ? g.body.name + " added" : "Saved");
        if (HY.alive(token)) { HY.go("/meds"); }
      }, function (e) {
        save.disabled = false;
        if (!HY.alive(token)) { return; }
        if (e.status === 412) {
          top.appendChild(HY.h("div", { class: "banner bad", role: "alert" },
            HY.h("p", null, "This medicine was changed on the device. Reload to see the changes"),
            HY.h("div", { class: "actions" },
              HY.h("button", { class: "btn p", type: "button", onclick: function () { HY.navigate(); } }, "Reload"))));
          top.scrollIntoView({ block: "start" });
          return;
        }
        var t = HY.errText(e);
        if (!t) { return; }
        showErrors([{ field: e.field === "min_interval_min" || errs[e.field] ? e.field : "", message: t }]);
      });
    });

    var archiveBtn = null;
    if (!isNew && !med.archived) {
      archiveBtn = HY.h("button", { class: "btn", type: "button", onclick: function () {
        HY.confirm("Archive " + med.name + "?", "It leaves the home screen and stops reminding. Its history stays.", true, "Archive")
          .then(function (yes) {
            if (!yes) { return; }
            HY.api("POST", "/api/medicines/" + med.id + "/archive").then(function () {
              HY.toast(med.name + " archived");
              HY.go("/meds");
            }, function (e) { var t = HY.errText(e); if (t) { HY.toast(t); } });
          });
      } }, "Archive");
    }

    var left = HY.h("div", { class: "panel" },
      HY.h("div", { class: "row2" },
        field("code", "Code", code),
        field("name", "Name", name)),
      field("dose", "Dose", dose),
      HY.h("div", { class: "lbl" }, "Colour"), swatches,
      HY.h("div", { class: "lbl" }, "Schedule"), typeBox,
      fixedBox, asNeededBox);
    var right = HY.h("div", { class: "panel" },
      HY.h("div", { class: "lbl" }, "Windows, in minutes"),
      field("early_window_min", "Early window (before the time)", early),
      field("late_after_min", "Late after", late),
      field("missed_after_min", "Missed after", missed),
      HY.h("div", { class: "lbl" }, "Pill count"),
      HY.h("label", { class: "check", for: "f-pills" }, pillsOn, "Count pills"),
      field("remaining", "Remaining", remaining),
      field("alert", "Alert at", alertAt),
      HY.h("div", { class: "lbl" }, "Course of treatment"),
      field("active_from", "Active from", from),
      field("active_until", "Active until", until));
    form.appendChild(HY.h("div", { class: "cols" }, left, right));
    form.appendChild(HY.h("div", { class: "actions" }, save,
      HY.h("a", { class: "btn", href: "#/meds" }, "Cancel"), archiveBtn));

    holder.appendChild(top);
    holder.appendChild(form);
  }

  HY.route("/meds/:id", render, "meds");
})();
