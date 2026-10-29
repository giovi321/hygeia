---
title: Home and logging
description: The tile grid, one-tap logging, undo and the top bar.
---

The images on the user guide pages are design mockups rendered from the repository, not photos of the board. The firmware follows them but small details can differ.

![Design mockup of the Home screen](/hygeia/assets/mockups/01-home.png)

## Top bar

The top bar shows the local time on the left. It shows `--:--` until the clock is valid. On the right, the battery field shows a placeholder dash and a percent sign: the battery percentage is not built (see [Roadmap](../../roadmap/)).

The Home, Overview and History screens form a horizontal pager. Page dots show the position.

## Tiles

Home is a grid of two tiles per row that scrolls vertically. Each active medicine has one tile:

- The code (one or two characters) in the medicine colour, inside a ring
- The ring shows the time elapsed from the last dose towards the next one
- A status line under the code

The status line reads:

| Text | Meaning |
|---|---|
| as needed | an as-needed medicine with its last dose not shown yet |
| no dose yet | no dose logged |
| just now, 3 h ago | time since the last dose |
| Due now | a scheduled dose is due |
| Late | the dose is past its late limit |
| Snoozed · 14:30 | the due card was snoozed until that time |
| Missed | the dose is past its missed limit |
| clock not set | the clock is invalid |

A due tile shows a full ring and an outline in the medicine colour. A "+" tile at the end shows a toast: "Add medicines in the web UI". The web UI is not built, so the tile does nothing useful yet.

## Logging a dose

Tap a tile to log the medicine as taken now. A toast shows the code and the time, for example "V logged 08:31", with a countdown "Undo 5". Tap Undo within 5 seconds to remove the entry. After 5 seconds, undo is no longer possible on the device ("Too late to undo").

- While the toast is showing, taps on other tiles do not log
- Logging needs a valid clock. With an invalid clock, a tap shows "Set the time first"
- For an as-needed medicine with a minimum interval, a tap sooner than the interval shows "Last dose 2 h ago, minimum 4 h. Tap again to log". A second tap within 5 seconds logs the dose
- A dose logged for a fixed medicine near a scheduled time satisfies that occurrence; see [Scheduling rules](../../reference/scheduling/)
- If a medicine has a pill count, the count goes down by one for each taken dose

Long press a tile to open the [medicine detail](../detail/).

Errors show a toast: "Medicine not found", "Storage error", "Too many entries" or "Could not log".
