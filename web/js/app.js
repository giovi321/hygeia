// Start: find out whether this browser is paired, then show the page named by the URL.

// Pages that other parts of the UI add later register their own routes before this file runs.
["/history", "/log", "/schedule", "/settings"].forEach(function (p) {
  HY.route(p, function (params, view) {
    view.appendChild(HY.h("h1", null, p.charAt(1).toUpperCase() + p.slice(2)));
    view.appendChild(HY.h("p", { class: "mut" }, "This page is not available in this version."));
  }, p.slice(1) === "log" ? "history" : p.slice(1));
});

(function () {
  window.addEventListener("hashchange", HY.navigate);
  HY.api("GET", "/api/info").then(function (info) {
    HY.info = info;
    if (info.paired) { HY.ws.start(); }
    HY.navigate();
    if (info.paired) { HY.timesync.run().then(function (changed) { if (changed) { HY.refresh(); } }); }
  }, function (e) {
    HY.info = { paired: true };
    var v = document.getElementById("view");
    v.appendChild(HY.h("h1", null, "Hygeia"));
    v.appendChild(HY.h("p", { class: "msg bad" }, "The device did not answer. " + (e.message || "")));
    v.appendChild(HY.h("button", { class: "btn", type: "button", onclick: function () { location.reload(); } }, "Try again"));
  });
})();
