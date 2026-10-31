// HY.api(method, path, body, headers) -> Promise of the parsed JSON (null when empty).
// Failures reject with {status, error, field, message, data}.
HY.api = function (method, path, body, headers) {
  var isWrite = method !== "GET" && method !== "HEAD";
  var opts = { method: method, credentials: "same-origin", cache: "no-store", headers: {} };
  var k;
  if (isWrite) { opts.headers["X-Hygeia"] = "1"; }
  if (body !== undefined && body !== null) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  if (headers) { for (k in headers) { opts.headers[k] = headers[k]; } }

  var attempt = function (retry) {
    return fetch(path, opts).then(function (res) {
      return res.text().then(function (text) {
        var data = null;
        if (text) { try { data = JSON.parse(text); } catch (e) { data = null; } }
        if (res.ok) { return data; }
        var err = {
          status: res.status,
          error: (data && data.error) || "http",
          field: (data && data.field) || null,
          message: (data && data.message) || ("The device answered " + res.status),
          data: data
        };
        if (res.status === 401 && path.indexOf("/api/pair") !== 0) {
          if (HY.info) { HY.info.paired = false; }
          HY.ws.stop();
          location.hash = "#/pair";
          err.handled = true;
        } else if (res.status === 503) {
          if (!isWrite && retry) {
            return new Promise(function (r) { setTimeout(r, 2000); }).then(function () { return attempt(false); });
          }
          if (isWrite) {
            // The write may still have happened: say so and show what the device holds now.
            err.handled = true;
            HY.toast("The device was busy. Check the result before trying again");
            HY.refresh();
          }
        }
        throw err;
      });
    }, function () {
      throw { status: 0, error: "network", field: null, message: "The device did not answer" };
    });
  };
  return attempt(true);
};

// Text for an error, empty when it was already shown.
HY.errText = function (e) { return e && !e.handled ? (e.message || "Something went wrong") : ""; };
