# Cabinet open samples

Sample downloads replayed by `test_owner_samples` in `test_cab_rec.c`. The test is skipped when this folder has no `*_teach.csv`.

## Format

The file the web UI downloads from `GET /api/cabinet/samples.csv`, unchanged. Header `kind,take,t_ms,ax,ay,az,gx,gy,gz`, one sample per line at 50 Hz:

| Column | Meaning |
| --- | --- |
| kind | `teach` (a stored take), `test` (a capture from the test screen) or `live` (a recent capture) |
| take | the take or capture number; each (kind, take) pair is one sequence |
| t_ms | milliseconds from the first sample of the sequence |
| ax, ay, az | acceleration in mg, device frame |
| gx, gy, gz | rotation rate in deci-dps (0.1 degree per second); all 0 when the gyro was off |

A row of kind `rest_<kind>` (`rest_teach`, `rest_test` or `rest_live`, as in `rest_live,<take>,0,ax,ay,az,0,0,0`) gives the rest vector of the one sequence with that kind and take, so a teach take and a live capture with the same number keep their own rests. Without it the replay takes the last sample of each sequence as its rest, which is where the capture became still.

## Naming

Group the files of one movement by a common name:

- `<what>_teach.csv`: the download right after teaching; the replay builds the model from its 3 `teach` takes
- `<what>_match.csv`: a download whose `test` and `live` captures are the taught movement; each must match
- `<what>_other.csv`: a download whose `test` and `live` captures are anything else; none may match

`<what>` is a short description of the movement, for example `door_left_teach.csv`, `door_left_match.csv`, `door_left_other.csv`.

## Privacy

The files hold motion only. Do not add anything else: no names, medicine names, times of day or notes in the file names or contents.
