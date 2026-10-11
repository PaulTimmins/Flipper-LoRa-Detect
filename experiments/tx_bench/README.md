# LoRa TX Bench — experimental; first over-air reception reported

Separate Flipper app for testing whether its internal CC1101 can synthesize a
MeshCore-compatible LoRa signal using OOK sidebands. The main detector app now integrates this method and adds LongFast transmission;
this directory preserves the earlier standalone MeshCore experiment. Starting this app does not transmit. Each short OK press sends one
bounded waveform; Back stops an active burst or exits when idle. Left/Right selects the test.
**Hold Up** switches Direct/Flood routing (shown on screen). Up/Down selects nominal -30, -20, or -10 dBm; startup defaults
to -10 dBm and MC packet CR5. There is a one-second cooldown and a 1.5-second timeout.

## First receiver test

1. Put a MeshCore US/Canada receiver nearby (approximately 0.5–1 m for the initial
   low-power test). Use the normal MeshCore US/Canada channel, 910.525 MHz, SF7, BW 62.5 kHz, explicit header and normal IQ.
2. Open its raw KISS/RF diagnostics. Note current RX, header-error and CRC-error
   counters if available. Do not rely solely on the chat UI.
3. Select **MC packet CR5**, choose Direct or Flood with **Hold Up**, and press OK once. Watch for the test payload or changes
   in those counters. Save timestamp, RSSI/SNR, CRC result and raw hex.
4. Try **MC packet CR8** once if needed. Each packet press now creates a fresh test number and timestamp; compare received
   numbers against the transmission log rather than repeating one cached packet.

Each packet contains `Flipper bench: TEST N` on Public, with a fresh sequence
number and UTC timestamp. Direct mode uses an empty path for nearby reception;
Flood mode permits normal mesh forwarding. The next number is saved to
`sequence.bin` before transmission, including attempts that subsequently fail.
After a restart it continues from that saved number. A preparation/save failure
prevents transmission, and the counter never wraps. The screen shows the next
number; `bench.txt` records the number actually sent, timestamp, routing and power.

The runtime generator finishes the complete pulse buffer before starting the
radio. Its integer phase accumulator reproduces the Python generator's samples
exactly in the tested cases, without doing encoding/crypto inside the TX callback.
The original fixed `TEST 01` tables remain as reference fixtures, not the source
of new packet-mode transmissions. Tone and preamble modes do not consume numbers.

The Flipper RTC here stores local wall time. A saved signed offset in
`utc_offset.bin` is added to convert to UTC; this device was configured with
+14400 seconds for America/Detroit daylight time. A missing offset defaults to
zero (appropriate for a UTC-set RTC). Reconfigure the offset if the device's
clock/time-zone convention changes or daylight time ends; it is not a time-zone
database. The install check compares the resulting packet timestamp with host UTC.

**Tone 41.25k** and **MC preamble** are diagnostic signals, not complete packets.
They should not appear as chats. An SDR is needed to inspect their spectrum;
absence from a KISS packet stream says nothing about their RF shape.

The app records completed/stopped/blocked bursts to
`/ext/apps_data/lora_tx_bench/bench.txt`. Completion means the timer/DMA finished,
not that a receiver decoded the packet. No automatic retries or transmissions
on startup. The fixed-frequency US exception below applies only to this bench.

## Waveform and limits

The CC1101 carrier is requested at 910.483750 MHz. A phase-continuous binary
waveform sweeps its switching frequency from 10 to 72.5 kHz, putting the first
upper sideband at 910.493750–910.556250 MHz (center 910.525 MHz). The synthesizer
quantizes the actual carrier; the app records its returned value.

This also produces a residual carrier, a mirrored lower sideband centered at
910.442500 MHz, and switching harmonics. It is not a clean native LoRa transmitter.
The original PA entry was 0x03 (-30 dBm). Selectable entries are now 0x03
(-30 dBm), 0x0E (-20 dBm), and 0x27 (-10 dBm), using TI's nominal
915 MHz table. Startup defaults to -10 dBm, 100x the original nominal power. Radiated
power and useful sideband power have not been measured. Use a nearby receiver or
an attenuated/shielded bench arrangement for characterization.

Reference-table pulses are precomputed on the computer; fresh packet waveforms
are precomputed on the Flipper before playback. The callback only fetches a duration
from RAM; the Flipper's standard timer/DMA path handles the edges. Resolution is
1 microsecond with the official firmware's minimum duration of 2 microseconds.
Packet-table pulses are at least 6 microseconds. The OOK async modem sampling
clock is configured with a roughly 600 kbaud rate, not the very slow stock OOK
preset. Hardware modulation filtering, clock error, DMA refill jitter, RF phase,
receiver synchronization and spectral images remain possible failure sources.

This first experiment targets MeshCore SF7/BW62.5 only. LongFast is not included:
its wider chirp raises the required switching rate and needs separate timing
and memory work. Rapid synthesizer-register tuning and GPIO harmonics are also
not implemented in this experiment.

## Reproduce

```sh
python3 experiments/tx_bench/generate.py
make -C tests host_payload
python3 experiments/tx_bench/check.py
python3 experiments/tx_bench/check_runtime.py
cd experiments/tx_bench
ufbt
```

Generation/checking requires Python, cryptography, and NumPy (NumPy only for
checking). `waveforms.h` and `waveforms.json` are generated artifacts. Build output
is `dist/lora_tx_bench.fap`.

The checks isolate the upper sideband of the actual binary waveform with an ideal
FFT filter and recover every expected symbol using dechirping/FFT. A separate
ideal discriminator model passes the existing C decoder through header/FEC,
whitening and CRC. Those checks share the project's PHY conventions and do not
replace independent hardware interoperability testing. Neither models CC1101
analog response nor firmware scheduling. Generated tables obey HAL pulse limits.

## References

- [TI CC1101 datasheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf): async input,
  OOK, modem rate and PA settings.
- [Official Flipper 1.4.3 Sub-GHz HAL](https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/targets/f7/furi_hal/furi_hal_subghz.c): timer/DMA output and minimum pulse duration.
- [LoLRa](https://github.com/cnlohr/lolra): precedent for digital LoRa synthesis;
  no source code was copied from that project. This CC1101 sideband method is
  a separate implementation, not a claimed LoLRa port.

## Result record

2026-10-11: generated CR4/5 and CR4/8 waveforms pass digital-model symbol recovery
and the project's CRC decoder. Standalone FAP builds for API 87.1.

**First successful receiver report:** during the RTL-SDR bench session, the user
reported seeing the exact `Flipper bench: TEST 01` text in their MeshCore chat.
The associated Flipper log records MC packet CR5, target 910.525 MHz, actual
programmed carrier 910.483612 MHz, PA 0x27 (nominal -10 dBm), and complete
waveform playback. This is user-observed over-air application reception, not yet
an independently retrieved receiver packet/CRC log or a measured success rate.
The transmitter uses the internal CC1101 OOK path, without an external LoRa radio.

**SDR confirmation:** `capture02.cu8` was decoded from recorded RF samples without
payload-assisted symbol selection. The received header checksum and payload CRC
pass. All 37 bytes exactly match the transmitted frame, with zero payload parity
errors. The received Public-channel HMAC also passes, and AES decryption yields
`Flipper bench: TEST 01`. Thus this is now byte-for-byte RF waveform confirmation,
not solely a user reception report. It does not establish rooftop propagation or
a success rate. The weaker first recording (`capture01.cu8`) fails CRC and has
14 parity errors at the SDR; receive gain was 14.4 versus 40.2 dB. Transmitter
power was unchanged at nominal -10 dBm.

Reproduce the RF decode (local private recordings required):

```sh
python3 tools/decode_tx_bench_iq.py captures/tx-bench-rtl/capture02.cu8 \
  --expected experiments/tx_bench/waveforms.json \
  --output captures/tx-bench-rtl/capture02-decoded.json
```

The decoder estimates the carrier and timing from the recording, checks sync,
decodes length and coding rate from the header, and makes hard FFT symbol
decisions. Expected bytes are compared only after decoding. There is no payload
substitution or CRC-guided candidate search. Its scope is this known SF7/BW62.5
bench waveform and a specified recording window, not a general SDR receiver.

Local, gitignored evidence: `captures/tx-bench-rtl/capture01.cu8` and
`capture02.cu8` (1 MS/s unsigned 8-bit interleaved IQ, tuned 910.250 MHz,
receive gains 14.4 and 40.2 dB). These recordings contain ambient traffic and
must remain private. Initial SDR decoding is complete; repeatability and range testing remain pending.

The device's provisioned US table allows 915–928 MHz, not 910.525 MHz. The initial
attempt was blocked before its callback ran. A 915.525 MHz workaround was rejected
by the user because the receiver should stay on its normal channel.

The bench now uses 910.525 MHz with a temporary in-memory exception for its exact
carrier on provisioned US devices. It copies the original region, adds one exact
frequency entry, tunes, and immediately restores the original region contents
before starting the waveform. Completion/cancellation also clears the HAL's cached
TX permission by rechecking the original region. There is no persistent region
file change or firmware patch. Other country codes do not receive this exception.

[47 CFR 15.249](https://www.ecfr.gov/current/title-47/section-15.249) covers
902–928 MHz subject to field-strength and unwanted-emission limits. Inclusion in
that band alone does not establish compliance of this experimental waveform.
The selected low PA setting is not a substitute for emissions measurements.
Startup writes the original provisioned bands to `region.txt` beside `bench.txt`.

### Unique-message verification

2026-10-11: runtime-generated TEST 2 (Direct) and TEST 3 (Flood) were recovered
from `capture03.cu8` with valid PHY CRC, valid Public MAC and zero payload parity
errors. The initial device-clock timestamps exposed a four-hour UTC conversion
error, corrected with the saved offset described above. Sequence persistence was
tested across an app restart. Default power remains nominal -10 dBm; default
routing is Direct, with Hold Up selecting Flood.

TEST 4 (Flood) also passed SDR CRC/MAC checks with zero parity errors after the
UTC fix; its decoded timestamp matched the host time of transmission. Three
unique packets passed this local SDR check; that is not a network delivery-rate
measurement. The app is left on Flood with TEST 5 next.
