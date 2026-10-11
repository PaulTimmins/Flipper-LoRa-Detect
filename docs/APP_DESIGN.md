# On-device app: design notes

A Flipper Zero activity detector with experimental test transmission that answers: **"Is there MeshCore or
Meshtastic activity nearby, and which kind?"** It detects LoRa preambles, reads the sync
word, and names the likely network and preset. Version 0.2 adds bounded MeshCore US payload decoding; the Python
tools in `tools/` retain the more capable offline recovery algorithms.

Target: **official firmware**, built with `ufbt` against SDK **1.4.3**. **US/Canada**
frequencies first.

## What the app does

* **Watch mode** (default): tune to one profile, for example MeshCore US at 910.525 MHz,
  and report every LoRa packet heard.
* **Survey mode**: cycle through all profiles with a ~10 s dwell each, logging what turns
  up.
* For each detection, show the network guess, the SF/BW, the RSSI, how many times it's
  been heard, and how long ago. Blink or vibrate on a new detection.
* Optionally append detections to `APP_DATA_PATH("detections.csv")`.

Example screen:

```
910.525  MeshCore US        WATCH
-------------------------------
MeshCore US   62.5/SF7  x3  -88dB  12s
Mt LongFast   250/SF11  x1  -97dB  2m
-------------------------------
<> profile   OK survey   BACK exit
```

## Radio setup (verified against firmware 1.4.3 source)

Use the `subghz_devices_*` API, the same one the official Sub-GHz app uses in
`applications/main/subghz/helpers/subghz_txrx.c`:

```c
subghz_devices_init();
const SubGhzDevice* dev = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
// internal CC1101: interconnect .begin is NULL, no begin() needed
subghz_devices_reset(dev);
subghz_devices_idle(dev);
subghz_devices_load_preset(dev, FuriHalSubGhzPresetCustom, preset_bytes);
subghz_devices_set_frequency(dev, freq_hz);
subghz_devices_flush_rx(dev);
subghz_devices_start_async_rx(dev, rx_callback, app);   // also switches the chip to RX
...
subghz_devices_stop_async_rx(dev);
subghz_devices_idle(dev);
subghz_devices_sleep(dev);
subghz_devices_deinit();
```

All of these are exported in `api_symbols.csv` for SDK 1.4.3. Notes:

* `furi_hal_subghz_start_async_rx` itself calls `furi_hal_subghz_rx()`, so no separate
  `set_rx` is needed.
* The capture callback is `void cb(bool level, uint32_t duration_us, void* ctx)`, called
  from the TIM2 ISR. `level` is the level that just *ended* (true means a high pulse), the
  same convention as positive values in `.sub` RAW files.
* TIM2's input capture filter discards pulses **shorter than ~16 µs**, which conveniently
  suppresses discriminator chatter.
* In the ISR, push packed words (bit 31 = level, low bits = duration) into a
  `FuriStreamBuffer` with timeout 0, which is ISR-safe. Process them in a worker thread.
  `subghz_worker.c` uses the same pattern.
* RSSI: `subghz_devices_get_rssi(dev)` is safe to call while async RX runs. Read it right
  after a preamble is detected, while the payload is still on air.
* To retune (survey mode): stop async RX, idle, load the preset if the BW changed, set the
  frequency, flush, then restart async RX.

### Presets (compiled into the app)

Same register layout as `sd-card/subghz/assets/setting_user`: (reg, value) pairs, then
`00 00`, then 8 PATABLE bytes. The base comes from Flipper's stock 2-FSK async preset, with:

* `FOCCFG = 0x14`: frequency offset compensation **off** (the stock 0x16 tracks ±BW/4
  and chases the chirp)
* `MDMCFG3 = 0xF8`, `MDMCFG4` low nibble `0xB`: **99.97 kBaud**. The CC1101 clocks its
  async output at 8× the data rate, so this gives ~1.25 µs edge timing. The stock 4.8 kBaud
  gives a ~26 µs grid, which ruins the wrap-edge timing at 250 kHz.

| LoRa BW | MDMCFG4 | RX filter | DEVIATN | deviation |
|---|---|---|---|---|
| 62.5 kHz | 0xAB | 135 kHz | 0x42 | 31.7 kHz |
| 125 kHz  | 0x9B | 162 kHz | 0x52 | 63.5 kHz |
| 250 kHz  | 0x5B | 325 kHz | 0x62 | 127 kHz |
| 500 kHz  | 0x1B | 650 kHz | 0x72 | 254 kHz |

62.5 and 250 kHz are validated on real captures. 125 and 500 kHz are computed but untested.

Base preset bytes (swap in the MDMCFG4/DEVIATN values for each BW):

```
02 0D 0B 06 08 32 07 04 14 00 13 02 12 04 11 F8 10 xx 15 yy 18 18 19 14
1D 91 1C 00 1B 07 20 FB 22 10 21 56 00 00  C0 00 00 00 00 00 00 00
```

## Detection algorithm (validated offline on real captures)

Run one detector per SF (7–12) in parallel for the current profile's BW. Each detector has
period T = 2^SF / BW and chip time Tc = 1/BW.

1. **Wrap edges.** A falling edge followed by a long low (≥ T/8) is a chirp wrap. In the
   captures these have ~0.6 µs jitter with the presets above. (The rising threshold
   crossings are much noisier, about 25 µs, so don't use them.) A falling edge's low
   duration is only known when the next pulse arrives, so hold the edge pending until then.
2. **Preamble.** Look for consecutive qualifying wraps spaced T apart within a tolerance of
   about max(3·Tc, 14 µs). Refine the period estimate as the run grows. Require ≥ 6
   periods before believing it. Off-period wraps reset the run, which keeps an SF8
   detector from locking onto an SF7 signal and vice versa.
3. **Sync word.** When the run breaks, let g = last preamble wrap + T. Collect falling
   edges up to about g + 1.7T:
   * each sync wrap is cyclic within its symbol: sync 1 occupies `[g − T, g)`,
     sync 2 occupies `[g, g + T)`
   * positive shifts land at `g − s1·Tc` and `g + T − s2·Tc`
   * negative shifts wrap to the **start** of that interval: a sync 2 shift of −40
     lands at `g + 40·Tc`, not `g + T + 40·Tc`
   * for each, `nibble = round(s/8) & 0xF`, with s read as signed, so a value arriving
     40 chips late is 0xB. Accept errors up to about 1.5 chips.
   * pick the candidate closest to a multiple of 8 chips, breaking ties by longer low.
     The sync 1 wrap's low can be short, e.g. 66 µs on LongFast when sync 2 starts high,
     so don't require a long low here.
4. **Emit** `{sf, bw, preamble_len = periods + 1, sync (or −1), period, time}`.

What the offline tools saw on real captures:

* MeshCore US (SF7/62.5): 31 periods at 2047 µs, so 32 preamble symbols; sync 0x12. No
  false runs in 30+ s of noise.
* Meshtastic LongFast (SF11/250): 15 periods at ~8191 µs, so 16 symbols. The first sync
  period is ~8125–8127 µs (T − 16 chips), which falls *inside* a loose ±5% tolerance, hence the
  tight tolerance above. Sync 0x2B arrives as wrap offsets of +16 and −40 chips.
* Captured timing differs slightly from nominal (2047 vs 2048 µs for MeshCore), so
  estimate T from the run rather than hardcoding it.

### Identification table

| sync | BW / SF | label |
|---|---|---|
| 0x12 | 62.5 / 7 | MeshCore US/Canada |
| 0x12 | 62.5 / 8 | MeshCore EU/UK narrow |
| 0x12 | other | MeshCore-like (0x12 is also RadioLib's default private word) |
| 0x2B | 250 / 11 | Meshtastic LongFast |
| 0x2B | 250 / 9 | Meshtastic MediumFast |
| 0x2B | 250 / 10 | Meshtastic MediumSlow |
| 0x2B | 250 / 8 | Meshtastic ShortSlow |
| 0x2B | 250 / 7 | Meshtastic ShortFast |
| 0x2B | 125 / 11 | Meshtastic LongModerate |
| 0x2B | 125 / 12 | Meshtastic LongSlow |
| 0x2B | 500 / 11, 9, 7 | Meshtastic LongTurbo / MediumTurbo / ShortTurbo |
| 0x34 | any | LoRaWAN |
| none read | — | "LoRa SFx/BW". Preamble length 32 suggests MeshCore (SF ≤ 8); 16 suggests Meshtastic |

### Profiles (US/Canada defaults)

Meshtastic default slots come from the firmware's djb2 hash of the preset name mod the
number of channels (US 902–928 MHz, no spacing). LongFast landing on 906.875 MHz matches
the well-known value.

| Profile | MHz | BW |
|---|---|---|
| MeshCore US/CA | 910.525 | 62.5 |
| Mt LongFast | 906.875 | 250 |
| Mt MediumFast | 913.125 | 250 |
| Mt MediumSlow | 914.875 | 250 |
| Mt ShortFast | 918.875 | 250 |
| Mt ShortSlow | 920.625 | 250 |
| Mt LongTurbo | 908.750 | 500 |
| Mt MediumTurbo | 925.250 | 500 |
| Mt ShortTurbo | 926.750 | 500 |
| Mt LongModerate | 902.6875 | 125 |
| Mt LongSlow | 905.3125 | 125 |

Nodes on a custom channel name use a different slot. A future version could load extra
profiles from an SD file.

## Code layout

```
app/
  application.fam    appid lora_detect, EXTERNAL, category "Sub-GHz", requires gui
  detector.c / .h    pure C, no furi includes (desktop-testable)
  profiles.h         profile table + preset byte arrays
  lora_detect.c      app: ViewPort UI, input queue, worker thread, radio, logging
tests/
  host_detect.c      reads a .sub, feeds the detector, prints events
  Makefile           cc -O2 -lm; run against captures/*.sub
```

Threads: the GUI/input loop runs on the main thread. A worker thread owns the radio (all
`subghz_devices_*` calls), drains the stream buffer, runs the detectors, handles survey
hops, and posts detections into a mutex-guarded model. The ISR does nothing but push into
the stream buffer.

## Test plan

1. **Desktop first:** `tests/host_detect` on the existing captures must report
   * `RAW-20261010-140003.sub` (MeshCore, old 4.8 kBaud preset): 3 × SF7, sync 0x12,
     preamble ≈ 32 (coarse timing; may need the 14 µs tolerance floor)
   * `RAW-20261010-142535.sub` (LongFast, 99.97 kBaud preset): 2 × SF11, sync 0x2B,
     preamble ≈ 16
2. Build with `ufbt`, launch on the device with `ufbt launch`, and check the RX callback
   rate and stream buffer headroom on a noisy band.
3. Field test: companion flood advert while watching 910.525, then a Meshtastic traceroute
   while watching 906.875.

## Out of scope

* **Native LoRa transmitting.** The CC1101 has no native LoRa
  mode. A separate precomputed OOK-sideband experiment now lives in
  `experiments/tx_bench`; digital-model checks pass, the user reported the exact test text in MeshCore
  chat, and recorded SDR IQ independently yields the identical packet with valid
  CRC and Public-channel MAC. Reliability is not yet measured. The earlier claim that precomputation could never help
  was too strong. An external SX126x remains a native LoRa transmit option.
* Full payload recovery, Meshtastic decryption and advert signature verification remain
  in the offline Python tools. Version 0.2 adds the limited MeshCore decoder below.

## Implementation status

`app/` now implements the detector, profiles, receive worker, UI and optional CSV logging.
The sync timing correction above was required to pass the actual LongFast capture:
its second sync wrap is only about 160 µs after `g`. Relative timestamps are converted
to single precision only after subtracting 64-bit edge times, avoiding loss of timing
precision at long uptime while using the Cortex-M4's hardware float support.

`make -C tests check` verifies synthetic SF7–SF12 signals at every bandwidth, positive
and signed sync shifts, incomplete sync, sample discontinuities, noise and long uptime.
It also checks the two private captures when present: three MeshCore 0x12 packets with
32 preamble symbols and two LongFast 0x2B packets with 16 symbols. Missing private
captures are explicitly skipped, so synthetic tests remain usable in a clean checkout.

The app builds with official firmware 1.4.3 / API 87.1. Live RF sensitivity, noisy-band
throughput and 125/500 kHz profiles still require device/field validation. USB upload and launch succeeded on the connected Flipper after closing qFlipper and
reconnecting USB. Packet reception and performance under load still need a live RF test.

## Version 0.2: bounded MeshCore payload decode

`payload.c/.h` implement a pure-C streaming SF7/62.5 kHz decoder. A 256-pulse ring
retains the preamble, which builds a 1024-point averaged discriminator template.
Integer template-prefix sums perform cyclic correlation over constant-level pulse runs; soft Gray/deinterleave/Hamming
decoding recovers explicit headers and payloads, followed by dewhitening and CRC.
Decoder memory is fixed (~26 KB): prefix sums avoid FFT scratch storage and roundoff,
and only the final 134 bytes need soft-metric history for advert combining. There is
no unbounded correction search. Other PHYs retain the existing detection behavior.
The radio worker records up to 8,192 pulse words (32 KB), including a pre-trigger ring,
and hands completed snippets to a decoder thread at the same priority as RX (to prevent starvation). Samples are never shared
while being written. A generation number associates results with the original tuning
session. Radio loss/retuning abort a capture, but an already handed-off snippet remains
valid and can finish. No correlation/FEC work runs in the radio worker. Five bounded timing offsets (0, −8,
−4, +4, +8 µs) are tried to tolerate coarse edge timing. For each packet, independently
CRC-checked results are preferred, then structurally valid results, then the strongest
average template correlation. Intermediate candidates can improve as passes complete.

A bounds-checked MeshCore parser extracts packet type and advert name. Nearby repeated
adverts with approximately matching public-key/timestamp bytes can accumulate quantized
soft metrics, aligning the body from the packet tail to tolerate different repeater
paths. This is a readability heuristic, not identity verification. Combined results
never become CRC-checked unless the original received packet independently passed CRC.
No Ed25519 verification runs on-device, even for CRC-correct packets. The UI and
`traffic.csv` explicitly distinguish CRC-checked from unverified candidates.

Hold Up toggles the traffic/RF view. Hold OK opens Settings; Logging controls detection and traffic CSVs.
Payloads are not logged in full. Captures stop after 2.5 seconds or when the fixed
buffer fills; incomplete packets in a finished snippet remain undecoded. New detections
continue while the decoder is busy, but do not start an additional snippet. Skipped
packets increment a counter without evicting captured history. The display shows
progress/elapsed time and a terminal Done status. Back stops
both workers and discards pending work promptly.

Host validation includes synthetic complete adverts at every coding rate, damaged
payloads, parser fuzz/bounds checks, reset behavior and the existing private capture.
The latter recovers the same candidate name as the offline signature-verified result,
without falsely promoting the damaged packet to CRC-checked status.

Device replay testing exposed why the decoder must be asynchronous: an initial FFT
prototype took about 4.2 seconds per 120/122-byte private advert on the Flipper CPU. The same
on-device replay recovered the candidate name while leaving CRC status unverified.
This is not a real-time payload demodulator; it is background best-effort analysis of
bounded received snippets. Strong-signal live reception and busy-band queue behavior
still need field testing.

The final integer-correlation implementation was replayed on the physical Flipper with
the same 8,192-word/2.5-second bounded capture used by the live app. It processed the
three captured adverts in about 7.2 seconds per timing pass (2.3–2.4 seconds each),
36 seconds for all five passes, and selected the correct candidate name. All three
packets correctly remained unverified. The decoder state on ARM was 26,344 bytes.
Background live decoding has additional scheduling overhead; initial candidates appear
before all timing passes finish. No new snippet is captured during those passes.

The v0.3 background-worker regression (`tests/device_check.c`) replays that snippet
while the real radio worker receives, and injects 32 additional busy detections.
All three rows survived; the candidate name was recovered and remained unverified.
With explicit one-tick scheduling pauses, the five-pass search completed in 106.7
seconds, with 34 skipped arrivals and zero lost pulses. USB `loader info` responded
in 0.84 seconds while both workers ran. The footer now shows percent and elapsed
time until Done; skipped packets no longer produce permanent “Decoder busy” rows.

### iPhone text report (v0.4)

`LoRa Logs.txt` is created in the app data directory on startup, then appended to
alongside `traffic.csv` when recording is enabled and a decode job finishes. It is
plain UTF-8 for the stock Flipper iOS app's File Manager, with one readable block
per result. Detection timestamps are captured from the device RTC with the original
RSSI, so a later decode or retune cannot change their meaning. Missing metadata is
explicitly marked unavailable. CRC failures and unchecked signatures remain visible.
The phone viewer loads a snapshot; reopen the file to refresh. Existing logs are
preserved and entries append oldest first. `tests/test_report.c` checks formatting,
missing fields, confidence labels, control-character handling and buffer bounds.


### Saved settings and Meshtastic support (v0.5)

Hold OK opens the scrolling settings view. Logging and packet sound default On;
decoding defaults Thorough to retain the best recovery of damaged MeshCore adverts.
Quick performs one timing pass, Thorough five, and Off cancels pending decode work.
MT LongFast chat and MC Public chat both default On and can be disabled separately. Settings use the firmware's
versioned/checksummed saved-structure API; invalid data falls back to defaults.
A short OK press wakes the display. Hold OK opens Settings and waits for release
before accepting menu changes; logging is controlled in the menu. Packet sounds use
the notification service and respect system volume/mute preferences.

The existing 1024-sample correlation workspace is retained. Meshtastic SF7–SF11 uses
sharp falling-edge timing to score every chip, including SF11's 2048 chip positions.
Higher-SF header blocks also carry payload nibbles, which are now preserved. SF12
and LDRO modes with symbol periods above 16 ms remain detection-only. Pulse-history
lookups use binary search, and symbol-to-bit scoring shares work between bit positions.
No FFT or Ed25519 verification runs on-device. Public-PSK AES-CTR is separate from
radio decoding and is attempted only for CRC-checked packets with LongFast hash 0x08.

MT metadata includes source/destination IDs. Optional decryption identifies node-info,
routing, traceroute query/reply, neighbor, telemetry, position and text traffic;
traceroute and neighbor reports include route/neighbor counts. Chat text is previewed on screen and retained in the phone log. Keys for private channels/PKI are not guessed. CRC does not authenticate
the sender, and encrypted or malformed data is never labeled as a decoded application.

MC Public group text uses AES-128 ECB with the standard Public PSK, channel hash 0x11, and a two-byte HMAC-SHA256 check over ciphertext. A matching MAC permits recovery even when the outer CRC fails; the original CRC status and ciphertext remain in the logs. Sender names are unverified. Settings version 4 adds the MC chat toggle and preserves older saved preferences during migration.

MC control packets expose discovery query tags, repeater reply tags and public-key
prefixes. Status/login/telemetry request bodies remain encrypted; the outer request
or response label does not imply their subtype or a verified repeater identity.
Protocol references:
- https://github.com/meshcore-dev/MeshCore/blob/main/src/Packet.h
- https://github.com/meshcore-dev/MeshCore/blob/main/examples/simple_repeater/MyMesh.cpp
- https://github.com/meshtastic/firmware/blob/develop/src/mesh/RadioInterface.h
- https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto

Host tests cover synthetic MT waveforms SF7–SF11 through CRC, AES and trace query,
NIST AES-CTR vectors, decryption-off and CRC-failure gating, query/reply parsing,
node names, malformed protobufs, and MC discovery. ASan/UBSan checks passed. The noisy
private LongFast capture yields tentative headers but still fails CRC; this is not
claimed as fully decoded. The original private MC regression still recovers its name.
Device live-RX benchmarks retained three rows with no lost pulses: five passes took
96.5 s before the final constant-size-loop adjustment; final Quick took 16.4 s but
produced much poorer tentative names, hence the Thorough default. Live RF load affects
elapsed time. The expanded GUI snapshot is stored in app memory rather than on the
GUI service's small stack; device checks showed 1260 bytes minimum GUI stack remaining.

Final device UI checks verified saved logging/sound/decode Off across a restart,
Quick/Thorough switching, MT key On/Off, and ignoring Right-repeat events until the
menu-opening key is released. Defaults were restored to On/On/Thorough/Off. Both
worker threads remained active; final free heap was about 19.8 KB. The final GUI draw
function uses a 140-byte local frame instead of the previous 1348-byte frame.

### Decoder optimization measurement (2026-10-11)

An on-device replay of the same 7,939 recorded pulse words (three damaged
MeshCore packets, one timing phase, live RX paused) measured 5,906 ms at the
firmware default `-Os` and 5,235 ms with only `payload.c` built at `-O2`.
Both runs produced three results and zero CRC-valid packets. This is an 11%
reduction in elapsed time for that workload, not a live-reception benchmark.
The targeted compiler setting retains normal floating-point semantics.

DWT cycle-counter instrumentation around template construction, payload feed,
symbol processing, and the correlation loop gave these approximate baseline
elapsed times (interrupt time is included): template 609 ms, sampling 1,084 ms,
correlation 1,154 ms, other symbol processing 2,959 ms. The last category includes
bit likelihood scoring, timing refinement, error correction, and completion.
These measurements do not yet separate scoring from error correction. They
suggest profiling that category before replacing the correlation algorithm.

Packet notification now lasts 100 ms at half relative volume, still respecting
system volume/mute. Turning Packet sound on in Settings plays the same chirp
as a preview, without requiring a received packet.

## Integrated test transmission (0.6)

The main app now owns RX and experimental OOK-sideband TX in the same radio worker.
Left is inactive. Right opens four actions: MC Public text, MC local repeater
discovery, MT LongFast text, MT local NodeInfo query. Hold OK opens the saved menu
with Channel, Mode (Watch/Scan), TX route (Local/Flood), and the previous options.
Queries always use zero hops and never automatically repeat. They return to Watch
on the appropriate profile with a 60-second listening window; decoded, CRC-valid
responses matching the request ID increment the verified reply count; damaged MC
replies with a matching tag increment a separate tentative count. MT NodeInfo replies
are subject to firmware/role throttling and are not a repeater-specific census.

`transmit_packet.c` builds packets; `mc_public.c` supplies the Public text builder.
`transmit_wave.c` does integer chirp synthesis into two packed-bit buffers, borrowing
capture.data plus the decoder workspace. TX pending cancels decoding; the radio
worker waits until capture.busy clears after all decoder logging before touching
either buffer. No new RX capture is possible because asynchronous RX is stopped.
The DMA callback only reads packed bits. On every exit path the worker restores RX;
a scoped US region exception restores the original region immediately after tuning,
and the cached permission is re-evaluated after TX. Counter storage is committed
before transmission so restarting does not repeat old packet IDs.

MC uses the bench-proven 10–72.5 kHz upper-sideband sweep sampled at 1 us. LongFast
uses a centered -125–125 kHz sweep sampled at 2 us. This halves the average pulse
rate compared with the initially attempted 10–260 kHz sweep, which stretched on the
stock DMA path. Both main-app message waveforms have now been captured with RTL-SDR
and decoded with valid CRC; the MT packet also decrypts to the exact test text.
A CRC-valid rebroadcast of the MT test message with hop limit reduced from 3 to 2
was subsequently captured by the main app, confirming native-node acceptance.
The ISR reader uses bit-run counting with the CPU's bit instructions rather than
walking each sample. No floating-point work or cryptography occurs in the ISR.

Protocol references: MeshCore `examples/simple_repeater/MyMesh.cpp`
`onControlDataRecv`/`sendNodeDiscoverReq` and Meshtastic firmware
`src/modules/NodeInfoModule.cpp`. MT broadcast queries use hop_limit=0 and stable
app-specific node identity `Flipper test`/`FLIP`, without pretending to be a router.

### Public message composer

Each protocol has a third TX action, Write message. A temporary custom chat View /
ViewDispatcher keyboard overlays the existing viewport while RX continues. The
keyboard is freed before transmission, preserving the small heap budget. Done
returns to a preview; a separate short OK sends one packet. Back from the preview
reopens editing; Back from the keyboard cancels without consuming a sequence ID.
Left is active inside the chat keyboard only. Explicit Space, Del, case, and
123/abc page controls support normal text and punctuation. Long OK inverts case
for one letter; held Back deletes. The draft tail scrolls with a character count.

MC supports 34 typed characters, prefixed with `Flipper: `; MT supports 44 typed
characters. Both fit the current 64-byte packet encoder. Builders reject empty or
oversized messages rather than truncate them. The worker snapshots the draft and
logs sent text if logging is enabled. Existing numbered tests and local queries
remain separate actions. Host tests decrypt and coherently demodulate short and
maximum-length composed messages for both protocols, including buffer boundaries.
