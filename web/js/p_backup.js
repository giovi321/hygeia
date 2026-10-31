// Backup: export the log as CSV, export everything as JSON, import a JSON backup (merge or replace) after a preview.
HY.settingsLinks = HY.settingsLinks || [];
HY.settingsLinks.push({ href: "#/settings/backup", text: "Backup", note: "Export CSV or JSON, import a backup" });

(function () {
  var MAX_BYTES = 4 * 1024 * 1024;
  var REPLACE_WARNING = "Everything on the device is replaced. The current data is kept in a backup folder until the next import";
  var MERGE_HINT = "Adds medicines and log entries that the device does not have. Nothing on the device is removed.";
  var MODES = [["merge", "Merge"], ["replace", "Replace"]];

  // The counts of a preview or of a finished import as lines of text.
  function summary(r) {
    var lines = [];
    var m = r.meds || {}, l = r.log || {}, g = r.gaps || {};
    lines.push("Medicines: " + (m.add | 0) + " new, " + (m.change | 0) + " changed, " + (m.same | 0) + " unchanged");
    lines.push("Log entries: " + (l.add | 0) + " new, " + (l.dup | 0) + " already on the device" +
      (l.skip ? ", " + (l.skip | 0) + " skipped because their medicine is not in the file" : ""));
    if (g.add) { lines.push("Untracked periods: " + (g.add | 0) + " new"); }
    if (r.settings) { lines.push("The file holds settings" + (r.secrets ? " and Wi-Fi passwords" : "")); }
    return lines;
  }

  HY.route("/settings/backup", function (params, view) {
    var token = HY.token;
    var mode = "merge";
    var file = null;
    var preview = null;     // the answer of the last preview, for the current file and mode
    var busy = false;

    view.appendChild(HY.h("div", { class: "head" }, HY.h("h1", null, "Backup"),
      HY.h("a", { class: "btn", href: "#/settings" }, "Settings")));

    // ---- export ----
    var pw = HY.h("input", { id: "b-secrets", type: "checkbox" });
    var jsonLink = HY.h("a", { class: "btn", href: "/api/export.json", download: "" }, "Download JSON");
    pw.addEventListener("change", function () {
      jsonLink.setAttribute("href", pw.checked ? "/api/export.json?secrets=1" : "/api/export.json");
    });
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Export"),
      HY.h("p", { class: "mut" }, "CSV holds the log and opens in a spreadsheet. JSON holds the medicines, the log and the settings and can be imported again."),
      HY.h("div", { class: "actions" },
        HY.h("a", { class: "btn", href: "/api/export.csv", download: "" }, "Download CSV"), jsonLink),
      HY.h("label", { class: "check", for: "b-secrets" }, pw, "Include Wi-Fi passwords"),
      HY.h("div", { class: "hint" }, "Off by default. When ticked, the JSON file holds the saved Wi-Fi passwords and the access point password in plain text: keep the file private.")));

    // ---- import ----
    var pick = HY.h("input", { id: "b-file", type: "file", accept: ".json,application/json" });
    var fileErr = HY.h("div", { class: "ferr", role: "alert" });
    var chips = HY.h("div", { class: "seg", role: "group", "aria-label": "Import mode" });
    var modeHint = HY.h("div", { class: "banner warn", hidden: true, id: "b-replace-warning" });
    var modeNote = HY.h("div", { class: "hint" });
    var previewBtn = HY.h("button", { class: "btn", type: "button", disabled: true, onclick: runPreview }, "Preview");
    var importBtn = HY.h("button", { class: "btn p", type: "button", hidden: true, onclick: runImport }, "Import");
    var useSettings = HY.h("input", { id: "b-settings", type: "checkbox" });
    var settingsRow = HY.h("label", { class: "check", for: "b-settings", hidden: true }, useSettings,
      "Also apply the settings and Wi-Fi networks in the file");
    var result = HY.h("div", { class: "msg", role: "status" });
    var sumBox = HY.h("div", { hidden: true });

    function drawChips() {
      HY.clear(chips);
      MODES.forEach(function (m) {
        chips.appendChild(HY.h("button", { class: "chip" + (mode === m[0] ? " on" : ""), type: "button",
          "aria-pressed": mode === m[0] ? "true" : "false", disabled: busy,
          onclick: function () { if (mode !== m[0]) { mode = m[0]; resetPreview(); drawChips(); } } }, m[1]));
      });
      modeHint.hidden = mode !== "replace";
      modeHint.textContent = mode === "replace" ? REPLACE_WARNING : "";
      modeNote.textContent = mode === "merge" ? MERGE_HINT : "";
      importBtn.className = "btn " + (mode === "replace" ? "danger" : "p");
      importBtn.textContent = mode === "replace" ? "Replace everything" : "Import";
    }

    function resetPreview() {
      preview = null;
      sumBox.hidden = true; HY.clear(sumBox);
      importBtn.hidden = true;
      settingsRow.hidden = true; useSettings.checked = false;
      show("");
    }

    function lock(on) {
      busy = on;
      previewBtn.disabled = on || !file;
      importBtn.disabled = on;
      pick.disabled = on;
      drawChips();
    }

    function show(text, bad) { result.textContent = text; result.className = "msg" + (bad ? " bad" : ""); }

    function fail(e) {
      if (!HY.alive(token)) { return; }
      var t = HY.errText(e);
      if (!t && e && e.handled) { t = "The device was busy. Check the result before trying again"; }
      show((e && e.field ? "(" + e.field + ") " : "") + (t || "The import failed") +
        (e && e.error === "changed" ? ". Run the preview and import again" : ""), true);
    }

    function url(isPreview) {
      return "/api/import?mode=" + mode + (isPreview ? "&preview=1" : (useSettings.checked ? "&settings=1" : ""));
    }

    function runPreview() {
      if (!file || busy) { return; }
      resetPreview();
      show("Reading the file on the device");
      lock(true);
      HY.api("POST", url(true), file).then(function (r) {
        if (!HY.alive(token)) { return; }
        lock(false);
        preview = r;
        show("");
        HY.clear(sumBox);
        sumBox.appendChild(HY.h("h3", null, "Preview: nothing has changed yet"));
        summary(r).forEach(function (t) { sumBox.appendChild(HY.h("p", null, t)); });
        sumBox.hidden = false;
        settingsRow.hidden = !(r.settings || r.secrets);
        importBtn.hidden = false;
      }, function (e) { if (HY.alive(token)) { lock(false); } fail(e); });
    }

    function commit() {
      show("Importing. Keep this page open");
      lock(true);
      HY.api("POST", url(false), file).then(function (r) {
        if (!HY.alive(token)) { return; }
        lock(false);
        var lines = summary(r);
        if (r.settings_applied) { lines.push("Settings applied"); }
        if (r.secrets_applied) { lines.push("Wi-Fi networks and passwords applied"); }
        resetPreview();
        sumBox.appendChild(HY.h("h3", null, "Imported"));
        lines.forEach(function (t) { sumBox.appendChild(HY.h("p", null, t)); });
        sumBox.hidden = false;
        HY.toast("Import done");
      }, function (e) { if (HY.alive(token)) { lock(false); } fail(e); });
    }

    function runImport() {
      if (!file || !preview || busy) { return; }
      if (mode === "replace") {
        HY.confirm("Replace everything?", REPLACE_WARNING, true, "Replace").then(function (yes) { if (yes) { commit(); } });
      } else {
        commit();
      }
    }

    pick.addEventListener("change", function () {
      fileErr.textContent = "";
      resetPreview();
      file = pick.files && pick.files[0] ? pick.files[0] : null;
      if (file && file.size > MAX_BYTES) {
        fileErr.textContent = "The file is larger than 4 MB, which the device does not accept";
        file = null;
      } else if (file && file.size === 0) {
        fileErr.textContent = "The file is empty";
        file = null;
      }
      previewBtn.disabled = !file;
    });

    drawChips();
    view.appendChild(HY.h("section", { class: "panel" }, HY.h("h2", null, "Import"),
      HY.h("p", { class: "mut" }, "Choose a JSON file made by Export. The device shows what the import would change before it changes anything."),
      HY.h("div", { class: "field" }, HY.h("label", { for: "b-file" }, "Backup file (JSON)"), pick, fileErr),
      HY.h("div", { class: "lbl" }, "Mode"), chips, modeNote, modeHint,
      HY.h("div", { class: "actions" }, previewBtn),
      sumBox, settingsRow,
      HY.h("div", { class: "actions" }, importBtn),
      result));
  }, "settings");
})();
