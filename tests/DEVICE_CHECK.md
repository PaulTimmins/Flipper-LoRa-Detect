# Background decoder regression check

`device_check.c` includes the real application workers. It runs live receive while
replaying one bounded saved snippet, injects 32 busy arrivals through `detected`,
and writes the retained history and completion status. It does not transmit.
The temporary app has no screen or button handler; allow it to finish (up to two
minutes plus worker shutdown). Only run it when the device is available for testing.

To stage a temporary ufbt project, copy `device_check.c` as `check.c`, and copy
`app/*.{c,h}` into the same
directory. Use this manifest:

```python
App(appid="lora_check", name="LoRa Check", apptype=FlipperAppType.EXTERNAL,
    entry_point="lora_check_app", requires=["gui"], stack_size=4096,
    sources=["check.c", "payload.c", "detector.c", "report.c", "protocol.c",
             "mc_public.c", "transmit_packet.c", "transmit_wave.c", "chat_keyboard.c"])
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

## Integrated TX smoke check (0.6, 2026-10-11)

On official 1.4.3 / API 87.1, main-app MeshCore text was recovered from RTL-SDR IQ
with valid CRC/Public MAC (`Flipper bench: TEST 6`). Centered-chirp LongFast text
was independently recovered with valid CRC/default-key decrypt (`Flipper TEST 10`).
The main app subsequently logged a CRC-valid network rebroadcast with hop limit
3 -> 2, and the user confirmed the message in chat from `!b73539f8`.
The local MT NodeInfo request was also recovered from IQ with valid CRC, port 4,
want_response=true and hop limit zero. A subsequent packet addressed to the saved
Flipper ID was logged but failed receive CRC; do not count it as a verified reply.

CLI button smoke test passed: Left does not modify the channel, including in the
menu; Channel saves and reloads across app restart; Watch/Scan toggles save; Right
opens the four-action TX menu and OK emits one burst. TX returns to reception and
USB remains responsive, with approximately 12.5 KB total free heap. While running,
use small CLI storage chunks (512 bytes), since the default 8192-byte read buffer
can exceed the largest free block. Upload with the app closed; reconnect the CLI
before launching to release transfer allocations.

Private recording evidence is in `captures/tx-main/` (not committed). The host
check `check_transmit.py` demodulates the actual packed bitstream independently,
checks crypto with Python cryptography/HMAC, and rejects an undersized buffer.

The MC local query (tag 12) elicited a 40-byte repeater reply with the same tag,
key prefix `4E75BDF0`, RSSI -71 dBm. Its CRC failed, so the final UI counts such
a result as `?`, separately from CRC-checked `OK` replies. The SDR stalled during
that query recording; do not treat the partial IQ file as waveform validation.
Query classification tests cover mismatched tags, invalid routes, tentative vs
CRC-checked MC replies, and matched/unmatched MT request IDs.

Composer smoke check: the stock keyboard was used to type `Test`. Save opened the
preview without consuming a packet sequence; Back/edit and keyboard cancel also
left the sequence unchanged. Subsequent MC Public and LongFast sends each logged
the exact composed text, then returned to RX. The app remained responsive with
about 9.9 KB free while the keyboard was open and 11.2 KB after closing it.

Chat keyboard replacement smoke check: typed `A b!` using the visible Space key
and 123 punctuation page, selected Done, then explicitly sent once. The device
log recorded `Sent on MC Public: A b!`. The keyboard freed its view on Done,
and the app remained running and returned to reception. Build and diff checks passed.
