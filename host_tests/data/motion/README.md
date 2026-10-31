# Recorded motion

CSV files replayed by `test_motion_rec.c`. Header `t_ms,x,y,z`, one sample per line, acceleration in mg in the screen frame, 50 Hz. `t_ms` is the boot time from `imu rec` and may start at any value; the test rebases it to the first sample. Make a file from a console log with `grep '^imu,' log | cut -d, -f2-`, then add the header line `t_ms,x,y,z` on top. A test whose file is absent is ignored.
