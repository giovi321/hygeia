---
title: Cabinet open
description: Remind when the cabinet is opened and a dose is due, with any movement or a movement taught on the device.
---

Cabinet open reminds you of a dose when you open the cabinet where the medicines are kept. Mount the device on the inside of the cabinet door. When the door moves and a dose is overdue or due soon, the device chimes, lights the screen, or both. When no dose is due, nothing happens.

Cabinet open is off by default. Turn it on in Settings › Cabinet open on the device, or on the Cabinet open page of the [web UI](../web-ui/#cabinet-open). Every option is on both.

## Triggers

Trigger has two choices:

- Any movement: the motion sensor reacts to any movement stronger than the set sensitivity. Nothing to teach. This is the default
- Taught movement: you teach the device one movement, and only a movement close to it counts. The movement can be anything, not only opening a door. Taught movement can be chosen only after a movement is taught; until then the device shows "Teach the movement first". Saving a taught movement switches the trigger from Any movement to Taught movement, and forgetting it switches the trigger back

Sensitivity goes from 1 to 5 and shows as "3 of 5" (the default). With Any movement it sets how small a movement wakes the device. With Taught movement it sets how close a movement must be to the taught one: 1 is the strictest, 5 the loosest.

The default sensitivity, 3 of 5, is set to catch the door opening, even a slow one. A knock on the door or a bump on the cabinet can therefore wake the device as well. That costs a little battery and nothing else: without a due dose the device stays dark and silent. If knocks bother you, set a lower sensitivity or use Taught movement.

## Mounting

- Fix the device on the inside of the cabinet door, so it moves with the door. A device that can slide or lift on its own counts those movements too
- The screen can face into the cabinet. Teaching, checking and testing use tones, so you do not need to see the screen while you do the movement
- The device can be mounted in any of the four orientations; teach the movement in the orientation you use
- After you move the device to another place on the door, or to another door, teach the movement again

## Teaching the movement

Teach the movement on the device: Settings › Cabinet open › Teach the movement. The page says "Do the movement 5 times. After Start, put everything back and wait. High tone: do the movement. Two low tones: got it, go back and wait." Tap Start. Start and the page text then give way to a counter ("1 of 5"), five dots and a status line, and Cancel is the only button.

After Start the device shows "Get ready" for 5 seconds. Use them to put the device, and whatever it is fixed to, back where it was: the tap on Start moved it. Movement in these 5 seconds is ignored. For each of the 5 takes:

1. "Hold still": keep the device still for about a second. The device waits up to 20 seconds for that
2. "Now" and a high tone: do the movement
3. Stop and hold still. "Got it" and two low tones end the take (takes 1 to 4). Then go back to where you started and wait. The device ignores movement for 5 seconds, then shows "Hold still" for the next take

The device always signals with sound as well as the screen, so you do not need to see the screen while you do the movement:

| Sound | Meaning |
| --- | --- |
| One high tone | Now: do the movement |
| Two low tones | Got it: go back to where you started and wait |
| One long low tone | Too small, too long or looked different: wait for the next high tone and do it again |
| Three rising tones | Saved. The test recognised the movement |
| Two falling tones | Teaching stopped. The test did not recognise the movement, saw none, did not get a still device, or stopped |

The tones play at the set volume, also when the sound is off or do not disturb is on. Two sounds in a row both play.

The fifth take, and a take asked for again after "That one looked different, do it again", play no "got it" tones, because the device builds the template right after them: within a moment the device plays the long low tone (one take looked different), the falling tones (teaching stopped) or the rising tones (saved). After the fifth take the device compares the five. If one of them looks different from the others, it shows "That one looked different, do it again" and asks for that take again (at most twice; after that the takes are kept as they are). When the five agree, the page changes to "Movement saved" with "Test it now?" and the buttons Test and Done. Done closes the page. Taught movement is then the trigger: if the trigger was Any movement, the device sets it to Taught movement when the movement is saved.

A movement must:

- Last at least about a quarter of a second. A tap or a knock is shorter and cannot be taught. The device shows "Too small, do it again" and plays the long low tone
- Last less than 4 seconds. A longer one shows "Too long, keep it under 4 seconds"
- Have no pause in it. A pause of 400 ms or more ends the take, so a movement with a stop in the middle is cut at the stop

If the device sees no movement for a minute after "Now", it shows "No movement, teaching stopped" and plays the falling tones. If the device does not stay still for a second within 20 seconds, or the takes give nothing usable, it shows "Teaching stopped": tap Start to begin again. Leaving the page also stops teaching, and the movement taught before stays as it was. The screen stays on while teaching runs (it neither dims nor turns off, also on battery); the normal screen timers resume when teaching ends or you leave the page.

The taught movement is kept across a restart. Settings › Cabinet open › Forget the movement removes it after the question "Forget the taught movement?". The web UI has the same button. When the trigger was Taught movement, forgetting also sets it back to Any movement. Without a taught movement, Taught movement does nothing.

## Checking the movement

Test on the "Movement saved" page checks the movement you just taught. The page then says "Put everything back and wait. After the high tone, do the movement once." and shows "Get ready", "Hold still" and "Now" with the same tones as in teaching. Do the movement once. The result:

- "Movement recognised" with the rising tones: the movement matches. Done closes the page
- "Not recognised. Teach the movement again?" with the falling tones: the movement was seen and does not match
- "No movement seen. Teach the movement again?" with the falling tones: nothing moved for a minute after "Now"
- "Not still, test stopped" with the falling tones: the device was not still for a second within 20 seconds. The page returns to "Movement saved" and Test starts it again
- "Test stopped" with the falling tones: the check was stopped from outside, by a sensor fault or the screen turning off. The page returns to "Movement saved"

The check uses the same trigger as the device uses in daily use. A movement too small to start a capture there does not start the check either, and ends as "No movement seen". So "Movement recognised" means the device would act on that movement.

After a miss, Teach again starts teaching at once, with the 5 seconds to get ready, and Keep closes the page and leaves the taught movement as it was. A movement that is too small or too long, or one the device could not capture, counts as a redo of the check: the long low tone, then wait for the next high tone. If the check cannot start, the page says "The test cannot start now". Cancel, or leaving the page, stops the check. The screen stays on while the check runs. The web UI shows "The movement is being tested on the device" and disables Start while it runs.

## Test mode

Settings › Cabinet open › Test, or Start under Test in the web UI, checks the trigger without reminding. When started on the device, the screen shows "Do the movement." and counts (a test started from the web UI does not open this page on the device):

- With Taught movement: "Matches" and "Other movements". On the device a match plays the low tone
- With Any movement: "Movements"

The device stays silent otherwise and does not remind during the test. The screen stays on while the test runs, also when it was started from the web UI (it neither dims nor turns off, also on battery). The test ends when you leave the Test page on the device, when you press Stop in the web UI, when the screen is turned off (for example with the PWR key), when the taught movement is forgotten or after 10 minutes, and the normal screen timers then resume. Leaving the web page does not stop a test. When a test ends while the device shows the Test page, the page shows "Test ended. Open Test again to run it." and the counters stop. The web UI shows the same counters live, and for each Taught capture its distance from the taught movement and the limit.

A good taught movement gives a match for nearly every opening and few matches for other things, such as closing the door, knocking or tapping the screen. If other movements match too often, lower the sensitivity or teach again.

## When the device reminds you

The device reacts when the trigger fires and a dose is:

- Overdue or open, including a dose you snoozed
- Due within the time set in "Remind if due within"

"Remind if due within" goes from 0 to 120 minutes in steps of 5 minutes. The default is 15 minutes. 0 shows as "Overdue only": the device reacts only to a dose whose time has come. For each medicine the time is cut to the medicine's own early window, so the device never reminds you of a dose that cannot be logged yet.

The reaction has two switches, both on by default:

- Chime: one chime, as for a due dose. The Sound switch, do not disturb and quiet hours silence it
- Light the screen: the screen turns on as for a touch. Do not disturb and quiet hours do not block it

When the screen lights and a dose is open, the due card shows. When the dose is not due yet, the device shows Home with the note "Next: Vitamin D at 08:00" for 5 seconds, naming the medicine and its time. The due card itself appears at the dose time.

The device reacts at most once every 2 minutes, however often the door opens. It does not react while the Undo toast shows, so closing the cabinet right after you log a dose changes nothing, and a shake still undoes it. Tapping outside the toast closes it early, and the device reacts again from then on.

When both switches are off, Cabinet open does nothing and the motion sensor is not used.

## Battery

While Cabinet open is on and a switch is on (and, with Taught movement, a movement is taught), the motion sensor stays powered in a low-power mode while the screen is off, and a movement wakes the CPU for a moment. The screen stays off unless the device reacts. Otherwise the motion sensor is powered down while the screen is off, as described in [Idle clock and power](../idle-power/#motion-sensor).

If the motion sensor stops answering, the device first tries to bring it back itself (a few retries, then a reset of the sensor). If that does not work, Cabinet open pauses itself, and tries again at the next time the screen wakes, at most four times an hour. A change of its switches, trigger or sensitivity, or switching it off and on, tries again at once. The Cabinet open page on the device shows "Paused: sensor fault" and the web page shows "Paused: the motion sensor did not answer".

## Downloading samples

The Cabinet open page of the web UI has Download samples. It downloads `cabinet-samples.csv` with:

- The 5 takes of the taught movement (3 for a movement taught by an earlier version)
- The last 8 movements captured with Taught movement, from the test and from use. These are kept in memory only and are gone after a restart

Each row holds the time in milliseconds and the readings of the motion sensor at 50 per second: acceleration in mg and rotation rate in tenths of a degree per second. A `rest_` row before each movement gives the position the device was resting in. The file holds no medicine or log data. It is useful to check or tune the matching on a computer.
