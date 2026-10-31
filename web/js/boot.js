// Placeholder page: shows what GET /api/info answers.
(function () {
  "use strict";
  var status = document.getElementById("status");
  fetch("/api/info", { cache: "no-store" })
    .then(function (r) {
      if (!r.ok) { throw new Error("HTTP " + r.status); }
      return r.json();
    })
    .then(function (info) {
      document.getElementById("name").textContent = info.name;
      document.getElementById("fw").textContent = info.fw;
      document.getElementById("paired").textContent = info.paired ? "yes" : "no";
      document.getElementById("info").hidden = false;
      status.textContent = "Connected.";
    })
    .catch(function (e) {
      status.textContent = "The device did not answer (" + e.message + ").";
    });
})();
