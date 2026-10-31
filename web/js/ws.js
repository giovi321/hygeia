// Live updates: any event from the device, or a resync, refetches the current page's data.
HY.ws = (function () {
  var sock = null, timer = null, wait = null, tries = 0, running = false;
  var delays = [1000, 2000, 5000, 10000];

  function poke() {
    clearTimeout(wait);
    wait = setTimeout(function () { HY.refresh(); }, 300);
  }

  function retry() {
    if (!running) { return; }
    var d = delays[Math.min(tries, delays.length - 1)];
    tries++;
    clearTimeout(timer);
    timer = setTimeout(open, d);
  }

  function open() {
    if (!running) { return; }
    var url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws";
    try { sock = new WebSocket(url); } catch (e) { retry(); return; }
    sock.onopen = function () { tries = 0; poke(); };
    sock.onmessage = function () { poke(); };
    sock.onclose = function () { sock = null; retry(); };
    sock.onerror = function () { /* onclose follows */ };
  }

  return {
    start: function () { if (!running) { running = true; tries = 0; open(); } },
    stop: function () {
      running = false;
      clearTimeout(timer);
      if (sock) { sock.onclose = null; sock.close(); sock = null; }
    }
  };
})();
