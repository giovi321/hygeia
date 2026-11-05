---
title: Add, edit and archive medicines
description: Manage medicines on the device with the step-by-step form and the archive confirmation.
---

The device can add, edit and archive medicines without a network. Weekdays, the early, late and missed windows, active dates, pill count and order are not editable on the device. The [web UI](../web-ui/#medicines) sets them. A medicine keeps its current values for these fields when you edit it.

## Add a medicine

Tap the "+" tile at the end of the Home grid. The form has these steps, with "Step n of 6" at the top:

1. Name. The first letter is capitalised automatically
2. Dose, optional, at most 16 characters, for example "1 tab" or "5 ml"
3. Short code, one or two letters or digits, picked from a grid. The code is shown on the tile and must be unused
4. Colour
5. How it is taken: "At set times" or "When needed"
6. The daily times, or for "When needed" the minimum time between doses

Then a review page shows the values with a Save button. A new medicine gets these defaults: every day, early, late and missed windows of 60, 30 and 120 minutes, no pill count and no active dates. It is placed after the last tile.

Nothing is stored before Save. Back leaves a step, and leaving with unsaved changes asks "Discard changes?" with Keep editing and Discard.

On the times page, "Add a time" opens an hour and minute picker in 5-minute steps, and its button reads "Add 08:00". Tap a time in the list to change it: the same picker opens on that time, titled "Change time", and the button reads "Change to 08:30". The list stays in order. Choosing the same time changes nothing, and choosing a time that is already in the list shows "08:30 is already in the list" and leaves the list as it was. Cancel returns to the times page without a change. Remove deletes a time and does not open the picker. A time set in the web UI that is not on a 5-minute step, for example 08:07, opens on the nearest lower step (08:05); the time stays 08:07 until you tap the button.

A time that is added so that today's dose is already inside its window becomes due right away, with the chime. The review page says so before you save.

## Keyboard

Names and doses use the on-screen keyboard. The Shift key capitalises the next letter only; press it again to cancel. A dose is not capitalised automatically.

## Edit a medicine

Open the medicine detail with a long press on its tile and tap Edit. The same form opens with the current values, and the review page reads "Edit" followed by the name. Save stores the change. If the medicine has a snoozed dose, the review page warns that the dose will show again now.

## Archive a medicine

On the detail page, tap Archive. A confirmation page asks "Archive Antibiotic?" and says that the medicine leaves the home screen and stops reminding, and that its history is kept. If a dose is due, it adds that the dose will not be logged. Only the Archive button on that page archives. No log entry is written, and any due card for the medicine goes away.

The device cannot bring an archived medicine back. Bring it back in the [web UI](../web-ui/#medicines).

## Limits

The device holds at most 50 active medicines. Archived medicines do not count toward that limit, and the store holds at most 100 medicines in total. At the limit the form says to archive a medicine first, or to ask for help in the web UI.
