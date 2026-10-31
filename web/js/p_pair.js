// Pair this browser: the device shows a 6-digit code, the person types it here.
HY.route("/pair", function (params, view) {
  var token = HY.token;
  var left = 5;
  var msg = HY.h("p", { class: "msg", role: "alert" });
  var box = HY.h("div", { class: "panel narrow" });
  view.appendChild(HY.h("h1", null, "Pair this browser"));
  view.appendChild(box);

  function say(text, bad) {
    msg.textContent = text || "";
    msg.className = "msg" + (bad ? " bad" : "");
  }

  function stepStart() {
    HY.clear(box);
    var btn = HY.h("button", { class: "btn p", type: "button", onclick: function () {
      btn.disabled = true;
      HY.api("POST", "/api/pair/start").then(function () {
        if (HY.alive(token)) { left = 5; say(""); stepCode(); }
      }, function (e) {
        btn.disabled = false;
        if (!HY.alive(token)) { return; }
        say(e.error === "blocked" ? "Too many tries. Wait a minute and ask for a new code" : HY.errText(e), true);
      });
    } }, "Show a code on the device");
    box.appendChild(HY.h("p", null,
      "This browser is not paired with the device yet. Pairing proves you are next to it."));
    box.appendChild(btn);
    box.appendChild(msg);
  }

  function stepCode() {
    HY.clear(box);
    var input = HY.h("input", { id: "code", name: "code", type: "text", inputmode: "numeric",
      autocomplete: "one-time-code", maxlength: "6", pattern: "[0-9]*", "aria-label": "Pairing code",
      class: "code-in", placeholder: "123456" });
    input.addEventListener("input", function () { input.value = input.value.replace(/[^0-9]/g, "").slice(0, 6); });
    var go = HY.h("button", { class: "btn p", type: "submit" }, "Pair");
    var form = HY.h("form", { novalidate: true },
      HY.h("label", { for: "code" }, "Enter the 6-digit code shown on the device"),
      input, go);
    form.addEventListener("submit", function (ev) {
      ev.preventDefault();
      var code = input.value;
      if (code.length !== 6) { say("The code has 6 digits", true); return; }
      go.disabled = true;
      HY.api("POST", "/api/pair", { code: code }).then(function () {
        HY.info.paired = true;
        HY.ws.start();
        if (HY.alive(token)) { HY.toast("Browser paired"); HY.go("/"); }
      }, function (e) {
        go.disabled = false;
        if (!HY.alive(token)) { return; }
        if (e.error === "wrong_code") {
          left--;
          if (left <= 0) {
            stepStart();
            say("Too many tries. Wait a minute and ask for a new code", true);
          } else {
            say("That code is not right. Tries left: " + left, true);
            input.value = "";
            input.focus();
          }
        } else if (e.error === "blocked") {
          say("Too many tries. Wait a minute and ask for a new code", true);
        } else if (e.status === 404) {
          stepStart();
          say("No code is running. Show a new code on the device", true);
        } else {
          say(HY.errText(e), true);
        }
      });
    });
    box.appendChild(form);
    box.appendChild(msg);
    box.appendChild(HY.h("button", { class: "btn", type: "button", onclick: function () { say(""); stepStart(); } },
      "Show a new code"));
    input.focus();
  }

  stepStart();
});
