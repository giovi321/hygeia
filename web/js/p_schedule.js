// Schedule: this week (Monday to Sunday) from /api/schedule, a column per day on a wide screen, a list on a phone.
(function () {
  var STATE_TEXT = {
    upcoming: "Upcoming", due: "Due", late: "Late", missed: "Missed",
    taken: "Taken", skipped: "Skipped", untracked: "Not tracked"
  };

  function hm(utc, zone) {
    return HY._formatter(zone, { hour: "2-digit", minute: "2-digit", hour12: false }, "hm").format(new Date(utc * 1000));
  }

  HY.route("/schedule", function (params, view) {
    var token = HY.token;
    var root = HY.h("div");
    var cache = null;   // {zone, meds}
    view.appendChild(HY.h("h1", null, "Schedule"));
    view.appendChild(root);

    function week(zone) {
      var today = HY.epochToDate(Math.floor(Date.now() / 1000), zone);
      var idx = HY.DAYS.indexOf(HY.dayName(today));
      var monday = HY.addDays(today, -idx);
      var days = [];
      for (var i = 0; i < 7; i++) { days.push(HY.addDays(monday, i)); }
      return { today: today, days: days,
               from: HY.wallToEpoch(monday, "00:00", zone), to: HY.wallToEpoch(HY.addDays(monday, 7), "00:00", zone) };
    }

    function draw(zone, meds, w, slots) {
      HY.clear(root);
      var byDay = {};
      slots.forEach(function (s) {
        var d = HY.epochToDate(s.slot, zone);
        (byDay[d] = byDay[d] || []).push(s);
      });
      var grid = HY.h("div", { class: "week" });
      w.days.forEach(function (d) {
        var list = byDay[d] || [];
        var col = HY.h("section", { class: "panel day" + (d === w.today ? " today" : "") },
          HY.h("h2", null, HY.dayLabel(d) + (d === w.today ? " (today)" : "")));
        if (!list.length) { col.appendChild(HY.h("p", { class: "mut sm" }, "Nothing scheduled")); }
        list.forEach(function (s) {
          var m = null;
          for (var i = 0; i < meds.length; i++) { if (meds[i].id === s.med_id) { m = meds[i]; } }
          col.appendChild(HY.h("div", { class: "slot st-" + s.state },
            HY.h("b", null, hm(s.slot, zone)), " ",
            m ? HY.h("span", null, HY.h("b", { style: { color: HY.medColor(m.color) } }, m.code), " " + m.name) : "Medicine " + s.med_id,
            HY.h("span", { class: "tag" }, STATE_TEXT[s.state] || s.state)));
        });
        grid.appendChild(col);
      });
      root.appendChild(grid);
      root.appendChild(HY.h("p", { class: "mut sm" }, "Medicines taken as needed are not listed."));
    }

    function load() {
      var pre = cache ? Promise.resolve(cache) : Promise.all([HY.deviceZone(), HY.api("GET", "/api/medicines")]).then(function (r) {
        return { zone: r[0], meds: r[1].meds || [] };
      });
      return pre.then(function (c) {
        cache = c;
        var w = week(c.zone);
        return HY.api("GET", "/api/schedule?from=" + w.from + "&to=" + w.to).then(function (r) {
          if (HY.alive(token)) { draw(c.zone, c.meds, w, r.slots || []); }
        });
      }).catch(function (e) {
        if (!HY.alive(token)) { return; }
        HY.clear(root);
        root.appendChild(HY.h("p", { class: "msg bad" }, e.error === "clock_invalid" ?
          "The device clock is not set, so there is no schedule to show yet." : (HY.errText(e) || "Could not load the schedule")));
      });
    }

    load();
    return function () { cache = null; return load(); };
  }, "schedule");
})();
