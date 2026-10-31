---
title: Gestures
description: Every touch gesture, with the thresholds the firmware uses.
---

## Gestures

| Gesture | Effect |
|---|---|
| Swipe left or right | pager: Home, Overview, History |
| Pull down from the top edge | quick settings, from any screen |
| Swipe up from the bottom edge | back to Home; closes quick settings |
| Swipe right from the left edge | closes the current detail or settings page |
| Vertical swipe on Home | scrolls the tiles |
| Tap a tile | logs the medicine as taken, with a 5 second undo |
| Long press a tile | opens the medicine detail |
| Slide the knob right on the due card | logs the dose as taken |
| Swipe left on the due card | snoozes |
| Swipe up on the due card | shows the next due dose |
| Swipe down on the due card | shows the previous due dose |
| Double tap on History | switches between 7 and 30 days |
| Turn the device | the screen follows in all four orientations once the device has stayed still in the new position, unless Rotation lock is on |
| Shake the device while the Undo toast shows | undoes the dose or skip, the same as tapping Undo |
| Any touch while the screen is dimmed | wakes the screen; nothing else happens |

## Turning and shaking

When the screen turns, swipe directions and edges turn with it. Pull down from the top edge always means the edge at the top of the screen as you see it.

The screen turns once the device has stayed still in the new position for about 1 second. If you turn the device while a finger is on the screen, the screen turns straight to the position the device is held in once you lift the finger. Turn on Rotation lock in Settings to keep the current orientation (see [Quick settings and settings](../settings/)).

After you log or skip a dose, the Undo toast shows for 5 seconds. Shaking the device during that time does what tapping Undo does, and the toast "Undone by shake" confirms it. The shake must be firm: four quick strokes back and forth. A light shake or a bump does not count. A shake at any other time does nothing.

## Thresholds

Measured on the panel on 2026-10-29:

- Edge zone: 40 px from the screen border
- Swipe: movement on the dominant axis of at least 60 px and at least 1.5 times the other axis
- Tap: at most 16 px of movement and under 500 ms. A long press has the same movement limit and lasts 500 ms or more
- Double tap: the second press starts within 300 ms of the first release and within 40 px of it
- A stroke that leaves the start point and returns is not a tap: the largest deviation from the start must also be 16 px or less
- A stroke that starts in an edge zone is never taken by a widget, except sliders
