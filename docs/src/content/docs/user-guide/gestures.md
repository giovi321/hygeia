---
title: Gestures
description: Every touch gesture, with the thresholds the firmware uses.
---

## Gestures

| Gesture | Effect |
|---|---|
| Swipe left or right | pager: Home, Overview, History |
| Pull down from near the top edge | quick settings, from any screen |
| Swipe up from near the bottom edge | back to Home; closes quick settings |
| Swipe right from near the left edge | closes the current detail or settings page |
| Vertical swipe on Home | scrolls the tiles |
| Tap a tile | logs the medicine as taken, with a 5 second undo |
| Long press a tile | opens the medicine detail |
| Slide the knob right on the due card | logs the dose as taken |
| Swipe left on the due card | snoozes |
| Swipe up on the due card | shows the next due dose |
| Swipe down on the due card | shows the previous due dose |
| Double tap on History | switches between 7 and 30 days |
| Turn the device | the screen follows in all four orientations once the device has stayed still in the new position, unless Rotation lock is on |
| Tap anywhere outside Undo while the Undo toast shows | closes the toast; the dose stays logged and the tap does nothing else |
| Shake the device while the Undo toast shows | undoes the dose or skip, the same as tapping Undo |
| Double tap while the screen is dimmed or off | wakes the screen; nothing else happens |
| Single tap while the screen is dimmed or off | does nothing, unless Wake on single tap is on; then it wakes the screen |

## Edge gestures

Start an edge gesture near the edge: on the border itself or a little inside the screen. When a corner mount or a case keeps your finger off the border, start the swipe on the glass as close to the edge as the case allows and swipe toward the middle. A swipe that starts in the middle of the screen is an ordinary swipe.

A keyboard key, a roller or a slider takes the swipe that starts on it, and a list takes a swipe that scrolls it. An edge swipe from inside the screen therefore works where nothing types, turns or slides, and over a list only when the list has nothing left to scroll that way: pull down from near the top with the list at its top, or swipe up from near the bottom with the list at its bottom. The list does not stretch while the screen navigates. From the border itself the edge gesture works everywhere except on a slider. A tap on a button near an edge is still a tap.

## Turning and shaking

When the screen turns, swipe directions and edges turn with it. Pull down from the top edge always means the edge at the top of the screen as you see it.

The screen turns once the device has stayed still in the new position for about 1 second. If you turn the device while a finger is on the screen, the screen turns straight to the position the device is held in once you lift the finger. Turn on Rotation lock in Settings to keep the current orientation (see [Quick settings and settings](../settings/)).

After you log or skip a dose, the Undo toast shows for 5 seconds, or until you tap outside it. Shaking the device while it shows does what tapping Undo does, and the toast "Undone by shake" confirms it. The shake must be firm: four quick strokes back and forth. A light shake or a bump does not count. A shake at any other time, including after you close the toast, does nothing.

## Waking the screen

By default a double tap wakes the dimmed or dark screen: two taps within 300 ms and within 40 px of each other. A lone tap does nothing, and a dark screen stays dark. A long press or a swipe does not wake it either. The taps that wake the screen are not passed to the screen underneath.

Wake on single tap, in Settings under Display and in the web UI, is off by default. When it is on, one tap wakes the screen.

A due card, the PWR key, the user key, plugging in USB and a [Cabinet open](../cabinet-open/) reminder wake the screen whatever the setting. The power-off confirmation, the battery critical countdown and the pairing code wake on a single touch too.

## Thresholds

Measured on the panel on 2026-10-29:

- Swipe: movement on the dominant axis of at least 60 px and at least 1.5 times the other axis
- Tap: at most 16 px of movement and under 500 ms. A long press has the same movement limit and lasts 500 ms or more
- Double tap: the second press starts within 300 ms of the first release and within 40 px of it
- A stroke that leaves the start point and returns is not a tap: the largest deviation from the start must also be 16 px or less
- A stroke that starts on the border is never taken by a widget, except sliders

Not yet measured: a stroke that starts on the border always counts as an edge gesture, and so does one that starts a little inside the screen when no widget takes it. The distance inside is provisional and is being measured on the panel
