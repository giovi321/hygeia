# Recorded motion

CSV files replayed by `test_motion_rec.c`. Header `t_ms,x,y,z`, one sample per line, acceleration in mg in the screen frame, 50 Hz. `t_ms` is the boot time from `imu rec` and may start at any value; the test rebases it to the first sample. Make a file from a console log with `grep '^imu,' log | cut -d, -f2-`, then add the header line `t_ms,x,y,z` on top. A test whose file is absent is ignored.

## Files

All recorded on the board at 50 Hz after the IMU mount correction, 25 to 30 s long except carry_walk (66 s).

| File | What it is |
| --- | --- |
| shake_firm.csv | Three firm shakes. The first is a fragment (the recording starts inside it), the other two last about 1.5 s and 2.3 s. Expected: 2 detections (the 3-stroke fragment at t = 0 is not counted at 4 strokes) |
| shake_light.csv | Two light shakes (about 1.1 s and 1.4 s). Informative: the test prints the count and asserts nothing |
| shake_live_combo.csv | Recorded while the detector was armed in debug mode. Two shake bursts (about 3.2 to 4.7 s and 6.8 to 8.7 s) and a 0.9 g bump at 10.8 s. The previous detector fired twice, both in the second burst. Expected: 2 detections |
| shake_live_arm.csv | Recorded in a debug run of the detector, from a window that ends about 40 s before the detector was armed. The board lay still (every axis within 46 mg), so there is no shake. Expected: 0 detections |
| tap_desk.csv, tap_held.csv, tap_double.csv | Touch taps: on a desk, with the board held, and a double tap. Expected: no shake |
| carry_walk.csv | Carried while walking. Expected: no shake |
| putdown_pickup.csv | Put down on a table and picked up. Expected: no shake |
| table_knock.csv | Knocks on the table the board lies on (the board itself barely moves, 88 mg at most). Expected: no shake |
| rotate_cycle.csv | Turned through all four edges: left edge up, bottom, right, top. Expected: switches to 270, 180, 90, 0 degrees, each after the 1 s dwell |
| tilt_diagonal.csv | Held at a slant between two edges and wobbled. Expected: three switches (270, 0, 270 degrees); each is a real tilt into the 30 degree cone of the new edge held for the dwell |
| flat_spin.csv | Spun on a table while lying flat. Expected: no rotation switch |
