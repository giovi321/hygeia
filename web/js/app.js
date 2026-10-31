// Start: find out whether this browser is paired, then show the page named by the URL.

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
