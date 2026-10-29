---
title: Overview and history
description: The today overview and the 7-day and 30-day history grids.
---

Swipe left from Home to reach Overview, and left again to reach History. Swipe right to go back.

## Overview

![Design mockup of the Overview screen](/hygeia/assets/mockups/03-overview.png)

Overview shows today in three parts:

- A ring with the count of doses taken against doses scheduled today, for example `2/4`, with the label "doses"
- One row per scheduled dose of today with its state: taken, due, late, missed, skipped or untracked (a dash). The list scrolls
- A 24-hour timeline with labels 00, 12 and 24

As-needed medicines are listed as "as needed" with the number of doses logged today.

## History

![Design mockup of the History screen](/hygeia/assets/mockups/04-history.png)

History shows a grid with one row per medicine and one cell per day for the last 7 days. Double tap anywhere on the page to switch to the last 30 days, and double tap again to go back. The weekday initials start on Monday.

Each cell shows the outcome of that day. The legend names three marks: on time, late and missed. A dose taken late gets its own mark, so it differs from a day that is only partly done. Periods the device could not observe appear as untracked and are left out of adherence.

The page also shows "Adherence N%" over the shown range, or "Adherence" with a dash when nothing is scheduled. Tap a row to open that medicine's [detail](../detail/).

The device cannot edit or delete log entries, apart from the 5 second undo after logging. Editing is planned for the web UI (see [Roadmap](../../roadmap/)).
