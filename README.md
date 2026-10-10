# Flipper-LoRa-Detect

Detect, identify, and in many cases fully decode **LoRa mesh traffic (MeshCore and
Meshtastic) with a stock Flipper Zero**, even though its CC1101 radio can't demodulate LoRa.

The goal is a quick answer to "is there a mesh around here worth getting my radio out
for?". It turns out the Flipper can do a lot more than that.

## How it works

The CC1101 can't demodulate LoRa's chirp spread spectrum. In **2-FSK async mode**, though,
its 1-bit frequency discriminator output tracks whether the instantaneous frequency is
above or below the tuned centre, and the Flipper timestamps every edge (that's what
Sub-GHz *Read RAW* records). A LoRa chirp sweeps linearly across its bandwidth and then
wraps, so in that 1-bit stream:

* **The preamble** is a square wave with period T = 2^SF / BW, e.g. 2.048 ms for MeshCore
  US (SF7 / 62.5 kHz) and 8.192 ms for Meshtastic LongFast (SF11 / 250 kHz).
* **The sync word** shows up as the next two symbols' wrap edges arriving early by
  8 × nibble chips. 0x12 is MeshCore and 0x2B is Meshtastic (the SX126x sends the nibble as
  a signed 7-bit value, so 0xB arrives as −40).
* **Data symbols** are the same chirp cyclically shifted by *s* chips. The falling edge at
  the frequency wrap is sharp (≈0.6 µs jitter with the presets here), so each symbol value
  can be read from where its wrap lands.

From there the tools run the standard LoRa receive chain (Gray mapping, diagonal
deinterleaving, Hamming decoding, dewhitening and CRC), following
[gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr). Symbols are only accurate to
about one chip, so decoding is soft-decision:

* per-bit likelihoods go into maximum-likelihood Hamming decoding;
* a best-first search over corrections uses the fact that the LoRa CRC is linear;
* every candidate must pass an independent check: the Ed25519 signature on a MeshCore
  advert, or for Meshtastic a payload that decrypts with the public LongFast key into a
  valid protobuf.

For MeshCore flood adverts, every repeater retransmission carries the same signed body, so
the copies are combined at the soft-metric level.

### The data-rate trick

The CC1101 clocks its async output at **8× the programmed data rate**. Flipper's stock FSK
presets use 4.8 kBaud, which snaps every edge to a ~26 µs grid (≈7.5 µs RMS jitter). That's
fine for MeshCore at 62.5 kHz (16 µs chips) but useless for Meshtastic at 250 kHz (4 µs
chips). The presets here run at 99.97 kBaud for a ~1.25 µs grid.

## Presets

Copy `sd-card/subghz/assets/setting_user` to `/ext/subghz/assets/setting_user` on the
Flipper (merge it with yours if you already have one) and restart the Sub-GHz app. It adds:

| Preset  | RX filter | Use with |
|---|---|---|
| `LoRa62`  | ~135 kHz | MeshCore US/Canada, 910.525 MHz |
| `LoRa250` | ~325 kHz | Meshtastic LongFast 906.875 MHz, MediumFast 913.125 MHz, … |

Both use 2-FSK async with frequency-offset compensation **off** (the stock presets track
±BW/4, which chases the chirp) and the 99.97 kBaud data rate.

Then: **Sub-GHz → Read RAW → Config**, choose the frequency and preset, set the RSSI
threshold to its lowest setting, record, and make a node transmit (flood advert, channel
message, traceroute…).

## Flipper app

The receive-only app in `app/` identifies LoRa preambles and sync words on the internal
CC1101. It displays a likely network/preset, SF, bandwidth, RSSI, detection count and
age. Network labels are guesses based on modulation and sync, not authenticated identities.
The on-device decoder supports **MeshCore US (SF7 / 62.5 kHz)** packet types and advert
names. Meshtastic and other PHYs retain detection/RSSI; their payload decoding remains
in the Python tools.

Build with `ufbt` using the official 1.4.3 SDK:

```bash
make -C tests check
cd app
ufbt
ufbt launch
```

The binary is `app/dist/lora_detect.fap`. You can also copy it to
`/ext/apps/Sub-GHz/lora_detect.fap` with qFlipper. Close/disconnect qFlipper before
using `ufbt launch`, since only one program can hold the USB serial connection.
The app includes its own presets; installing `setting_user` is not required.

* **Left / Right:** choose a profile and enter Watch mode.
* **OK:** toggle Survey mode (10 seconds per profile).
* **Up / Down:** browse the eight most recently heard signal types.
* **Hold Up:** switch between traffic details and the original RF view.
* **Hold OK:** toggle text and CSV logging; `REC` means enabled, `ERR` means a write failed.
* **Back:** stop reception and exit.

The traffic view shows an advert name when recoverable, a traffic type such as advert,
ACK, channel message or path, and the received RSSI. It does not decrypt message text.
`CRC OK` means the packet checksum passed; advert signatures are **not** checked on the
Flipper. `UNVERIFIED` and a `?` mean damaged/tentative data: names and types may be wrong.
Close repeats with similar identity and timestamp bytes are soft-combined to improve
advert readability, but remain unverified. Wait for further copies if the name is missing.
The full recovered name (up to 32 bytes) is retained in the log; the small screen shows
its first 22 characters. Non-ASCII bytes display as `?`.

The decoder uses a fixed ~26 KB working area plus a 32 KB snippet buffer, and falls
back to detection-only if there is insufficient free heap. After a MeshCore detection,
it captures up to 2.5 seconds (or 8,192 pulse words), then decodes on a background thread that time-shares with the radio worker, trying five small timing offsets. Expect several seconds per packet before initial details appear; the full timing search
can take around 1–2 minutes with live reception running and may improve tentative candidates. The radio
keeps detecting while a snippet is processed; packets arriving during decoding are
skipped for payload capture, counted on screen, and still logged when recording. They
do not replace the captured packets in history. The footer shows progress and elapsed
seconds, then `Done`; an unsuccessful attempt ends with `No payload recovered`. A retune or lost samples abort an unfinished capture. Previously captured snippets
can finish decoding after a retune, with their original frequency and RSSI. The Python decoder remains more capable at recovering damaged signals.

Logging defaults off. Records append to `/ext/apps_data/lora_detect/detections.csv`;
timestamps are device uptime in milliseconds, sync is decimal (`-1` means unreadable),
and RSSI is sampled when detection completes. With logging enabled, payload results
also append to `/ext/apps_data/lora_detect/traffic.csv`, including type, name, RSSI,
confidence and combined-copy count. A green LED blink indicates a detection.

For the stock iPhone Flipper app, decoded results also append to
`/ext/apps_data/lora_detect/LoRa Logs.txt`. It shows detection date/time (from the
Flipper clock), traffic type, recovered name, RSSI, frequency and verification status.
Hold OK to enable `REC`; allow the background decode to finish before viewing results.
On iPhone, connect by Bluetooth and open **Options → File Manager → ext (SD Card) →
apps_data → lora_detect → LoRa Logs.txt**. Reopen the file to refresh; it does not
update live. The file is created at startup with instructions even before any results
are recorded. Entries are oldest first; CSV logging remains available.

The idle screen shows callback rate, sampled stream headroom and lost pulses; `!loss`
on the detection screen flags sample loss. On overflow the worker restarts reception
and resets the detector so gaps cannot be mistaken for a continuous preamble.

The supplied MeshCore and LongFast captures pass desktop regression checks. Synthetic
MeshCore adverts pass full waveform-to-name tests at CR4/5 through CR4/8; malformed
frames and damaged packets are checked with address/undefined-behavior sanitizers.
The old MeshCore capture's combined candidate name matches the offline signature-verified
name, while correctly remaining unverified on-device. The build
passes the official SDK import check, and USB upload/launch succeeded on the connected
Flipper. Live packet reception and the computed 125/500 kHz presets still need field validation. See [design notes](docs/APP_DESIGN.md) for details.

## Tools

All tools take a Flipper `.sub` RAW capture. They need Python 3 with `numpy` and
`cryptography`.

```bash
# find LoRa preambles, symbol period, sync word and frequency offset
python3 tools/analyze_sub.py capture.sub --profile meshcore-us        # or meshtastic-longfast

# raw LoRa PHY decode (header, payload, CRC)
python3 tools/lora_decode.py capture.sub --sf 7 --bw 62500

# MeshCore adverts: combine copies, verify signature, print name/type/position/path
python3 tools/meshcore_advert.py capture.sub

# Meshtastic: decode and decrypt default-channel (LongFast) packets
python3 tools/meshtastic_decode.py capture.sub
```

## Limits

* The Flipper's radio and antenna are far less sensitive than a LoRa chip, so you'll hear
  nearby nodes and line-of-sight repeaters, not the whole mesh. Silence doesn't mean
  there's no mesh.
* Only the *public* Meshtastic channel key can be decrypted. Private channels and PKI
  direct messages show up as headers only.
* Receive only. The CC1101 can't transmit LoRa.

## Privacy note

MeshCore adverts and Meshtastic position packets often include **GPS coordinates**, and
anyone with a Flipper can now read them. Raw captures contain whatever was on the air, so
`captures/` is gitignored. Don't publish captures without scrubbing them.

## License

MIT, see [LICENSE](LICENSE).
