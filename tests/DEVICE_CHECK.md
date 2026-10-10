# Background decoder regression check

`device_check.c` includes the real application workers. It runs live receive while
replaying one bounded saved snippet, injects 32 busy arrivals through `detected`,
and writes the retained history and completion status. It does not transmit.
The temporary app has no screen or button handler; allow it to finish (up to two
minutes plus worker shutdown). Only run it when the device is available for testing.

To stage a temporary ufbt project, copy `device_check.c` as `check.c`, and copy
`app/{lora_detect.c,detector.c,detector.h,payload.c,payload.h,profiles.h,report.c,report.h}` into the same
directory. Use this manifest:

```python
App(appid="lora_check", name="LoRa Check", apptype=FlipperAppType.EXTERNAL,
    entry_point="lora_check_app", requires=["gui"], stack_size=4096,
    sources=["check.c", "payload.c", "detector.c", "report.c"])
```

Prepare at most 8192 pulse words from a bounded SF7/BW62500 capture, including
preamble history. Each little-endian uint32 contains duration in microseconds in
bits 0–30 and high level in bit 31. Upload it to
`/ext/apps_data/lora_detect/check.bin`, then run `ufbt launch` in the staging folder.
Read `/ext/apps_data/lora_detect/check.txt` after completion.

For the private three-advert fixture, require `done=1`, `progress=100`, `seeded=3`,
`retained=3`, `skipped>=32`, no rows stuck in states 1 or 6, and the expected candidate
name in one row with state 3 (unverified). Inspect `lost` for receive starvation.
While the test runs, check that USB CLI `loader info` responds. Remove the test
FAP, input and report afterward, and reinstall/launch the production app.
