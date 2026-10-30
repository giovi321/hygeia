---
title: Medicine detail
description: Per-medicine page with schedule, recent log, notes, earlier logging and a 30-day grid.
---

![Design mockup of the medicine detail page](/hygeia/assets/mockups/05-medicine-detail.png)

Open the detail page with a long press on a tile, or with a tap on a History row. Swipe right from the left edge to close it.

The page scrolls vertically and contains:

- The medicine ring, name and dose
- The schedule line: the fixed times separated by commas, or "as needed, at least 4 h apart"
- Pills left, when the medicine has a pill count: "12 tablets left" or "1 tablet left"
- The three newest log entries, including skipped and missed ones, or "No doses logged yet"
- The buttons "Log with note" and "Log earlier..."
- A 30-day grid and the adherence over it
- The buttons Edit and Archive at the bottom; see [Add, edit and archive medicines](../medicines/)

## Log with note

"Log with note" opens an on-screen keyboard with the hint "Note (optional)" and a Log button. The note is stored with a taken entry. The on-screen keyboard is also used for medicine names and doses.

## Log earlier

"Log earlier..." opens two rollers, hour (00 to 23) and minute (steps of 5), preset to the current time rounded down to 5 minutes. The line below shows "Today 07:35" or "Yesterday 22:10": a time later than now means yesterday. The button "Log at 07:35" writes a taken entry at that time. This is a deliberate entry, so the too-soon warning does not apply. An invalid time shows "That time is not valid".
