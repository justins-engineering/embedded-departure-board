# stop_id gate tests (host)

```sh
./tests/stop_id/run.sh
```

Runs from anywhere, needs no board, no Zephyr build and no network. Exits 0
on pass, 1 on a failed assertion, 2 if something could not be built or the
config could not be read.

Same idiom and same caveat as `../parse_swiftly`: **not wired into anything**
(no CI workflow runs this), and it compiles the real `app/src/stop_id.c`
against stub Zephyr headers, with `CONFIG_STOP_ID` read out of
`app/sign.conf` rather than repeated here. The stub mutex is a no-op, so
this covers the seed-vs-synced state machine only, never the locking.

## What is covered

| case | asserts |
| --- | --- |
| boot state | value is the compiled-in seed, `stop_id_synced()` false |
| NULL / empty set | rejected, gate stays closed, value untouched |
| runtime value equal to the seed | gate opens anyway (the bench sign's shadow carries the same stop the firmware was built with; a change-only gate would leave it dark forever) |
| a different stop | value updated |
| overlong id | truncated to `STOP_ID_MAX_LEN - 1`, the documented truncate-not-reject policy |

The flag is monotonic per boot, so case order is load-bearing: the unsynced
assertions run before the first accepted set and cannot be reordered after
it.
