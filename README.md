# Flipper-LoRa-Detect

Detect, identify, and in many cases fully decode **LoRa mesh traffic (MeshCore and
Meshtastic) with a stock Flipper Zero**, even though its CC1101 radio can't demodulate LoRa.

It also experimentally **transmits MeshCore Public and Meshtastic LongFast messages**
using the same internal radio, with real-node reception demonstrated. No external
LoRa module is involved.

The goal is a quick answer to "is there a mesh around here worth getting my radio out
for?". It turns out the Flipper can do a lot more than that.

## How reception works

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

From there the desktop tools run the standard LoRa receive chain (Gray mapping, diagonal
deinterleaving, Hamming decoding, dewhitening and CRC), following
[gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr). Symbols are only accurate to
about one chip, so decoding is soft-decision:

* per-bit likelihoods go into maximum-likelihood Hamming decoding;
* a best-first search over corrections uses the fact that the LoRa CRC is linear;
* every candidate must pass an independent check: the Ed25519 signature on a MeshCore
  advert, or for Meshtastic a payload that decrypts with the public LongFast key into a
  valid protobuf.

For MeshCore flood adverts, every repeater retransmission carries the same signed body, so
the copies are combined at the soft-metric level. These are the desktop recovery
methods: the Flipper uses a smaller bounded decoder, does not check advert
signatures, and can show tentative candidates with a `?`. Its CRC status remains
visible rather than treating a plausible name as proof.

### The data-rate trick

The CC1101 clocks its async output at **8× the programmed data rate**. Flipper's stock FSK
presets use 4.8 kBaud, which snaps every edge to a ~26 µs grid (≈7.5 µs RMS jitter). That's
fine for MeshCore at 62.5 kHz (16 µs chips) but useless for Meshtastic at 250 kHz (4 µs
chips). The presets here run at 99.97 kBaud for a ~1.25 µs grid.

## How transmission works

The CC1101 still has no LoRa modem. We use its **asynchronous OOK input as a
one-bit waveform generator**: rapidly switching a carrier on and off creates RF
sidebands. If that switching waveform is a chirp, a sideband is also a chirp.
Cyclically shifting each chirp encodes LoRa symbols that a native LoRa receiver
can demodulate.

### Packet bytes to radio waves

1. **Build a real network packet.** MeshCore Public messages use AES-128 ECB and
   the protocol's two-byte HMAC-SHA256 over ciphertext. Meshtastic messages carry
   protobuf data encrypted with the default LongFast AES-CTR key, using the packet
   ID and sender ID in the nonce. Local discovery builds the corresponding query
   instead. These are shared public keys, not private-message key recovery.
2. **Encode the LoRa PHY.** Generate the explicit header and its checksum, payload
   CRC, whitening, Hamming coding, diagonal interleaving, and Gray-to-symbol
   mapping. Add the network's preamble, sync symbols, and downchirp delimiter.
   The integrated transmitter uses CR4/5 payload coding.
3. **Synthesize the entire waveform first.** An integer phase accumulator turns
   the chirps into packed one-bit samples, preserving phase across symbol
   boundaries. The Flipper generates a fresh waveform for each packet, rather
   than replaying a recording or one fixed test message.
4. **Play it through hardware timing.** The callback extracts runs of equal bits
   as level/duration pairs. The existing Sub-GHz timer/DMA path schedules the OOK
   edges. Packet construction and encryption are finished before playback, so
   that work cannot delay an edge inside the transmit callback.
5. **Return to listening.** RX and payload decoding share the radio and memory
   with TX. A send cancels pending decode work, waits for its buffers, borrows the
   20 KB packed capture buffer plus decoder workspace, then restores reception before
   logging the result. This is half-duplex; it cannot receive during a burst.

The implementation is in [transmit_packet.c](app/transmit_packet.c),
[transmit_wave.c](app/transmit_wave.c), and the radio worker in
[lora_detect.c](app/lora_detect.c). The earlier standalone
[TX bench](experiments/tx_bench/README.md) preserves the fixed fixtures and
experiments that led to the integrated implementation.

### Two different chirp arrangements

| | MeshCore US/CA Public | Meshtastic US LongFast |
|---|---|---|
| Target center | 910.525 MHz | 906.875 MHz |
| PHY | SF7 / BW62.5 kHz | SF11 / BW250 kHz |
| Requested CC1101 carrier | 910.483750 MHz | 906.875 MHz |
| Synthesized chirp offset | +10 to +72.5 kHz | −125 to +125 kHz |
| Packed sample interval | 1 µs | 2 µs |
| Preamble | 32 symbols | 16 symbols |

For MeshCore, the first upper sideband lands on the desired channel. There is
also a residual carrier, a mirrored lower sideband, and switching harmonics.
The CC1101 synthesizer quantizes the requested carrier; the bench recorded its
programmed value as 910.483612 MHz.

For LongFast, a wide chirp shifted entirely above the carrier demanded too many
edges for reliable playback. **Centering the chirp around zero reduces the
maximum switching frequency**, and 2 µs samples reduce memory use. Because the
OOK waveform is real rather than complex IQ, it also emits the conjugate chirp;
the desired component can still be recovered by coherent LoRa demodulation.
This arrangement produced CRC-valid LongFast packets in SDR recordings and a
native-node rebroadcast. It is not equivalent to a spectrally clean LoRa modem.

### How we checked that it was real

We used three complementary checks, keeping expected bytes out of RF symbol
selection:

* **Host waveform tests:** demodulate the actual packed C-generated OOK waveform,
  recover the exact packet and PHY CRC, and independently check encryption with
  Python cryptography/HMAC. Tests include both chat formats, both local queries,
  maximum-length messages, and buffer/length rejection. Ideal digital tests do
  not model the CC1101 analog response or DMA scheduling.
* **RTL-SDR recordings:** estimate timing/carrier from received IQ and recover
  symbols with dechirping and FFT, then check header checksum, payload CRC, and
  public-channel crypto. Expected bytes are compared only after decoding.
  The desktop SDR verifier uses FFTs; the Flipper's live receiver does not.
* **Native network reception:** Paul saw the exact MeshCore test text in Public
  and Meshtastic `Flipper TEST 10` in chat. The Flipper also captured the latter's
  CRC-valid rebroadcast with its hop limit reduced from three to two. That is
  evidence of native-node acceptance beyond a local waveform simulation.

A repeater's RSSI describes the hop it received, not necessarily the original
Flipper-to-repeater link. A dashboard's forwarded packet or a completed local
TX callback alone does not establish reception. Local discovery replies remain
separately marked CRC-checked or tentative; neither protocol has a measured
query-response success rate here. We have demonstrated interoperability, not a
quantified range, delivery rate, or emissions-compliance result.

Recordings contain ambient traffic and stay under the gitignored `captures/`
directory. Public source includes the encoders, synthetic fixtures, and checks.
See [device validation](tests/DEVICE_CHECK.md) for the specific hardware results.

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

The app in `app/` identifies LoRa preambles and sync words on the internal
CC1101. It displays a likely network/preset, SF, bandwidth, RSSI, detection count and
age. Network labels are guesses based on modulation and sync, not authenticated identities.
The on-device decoder supports **MeshCore US (SF7 / 62.5 kHz)** packet types and advert
names, plus discovery queries and repeater replies. Meshtastic SF7–SF11 at supported
125/250/500 kHz profiles now has experimental payload decoding with wrap-edge timing
refinement. SF12 and low-data-rate-optimization modes (symbol periods over 16 ms)
remain detection-only. No external radio is required. Experimental test transmission uses the internal CC1101 OOK sideband technique described below.

**Hold OK** opens the saved menu: **Channel**, **Mode** (Watch/Scan), **TX route** (Local/Flood), **MC hop bytes** (1/2/3; default 2), logging, packet sound, **Off / Quick / Thorough**
decoding, and separate **MT LongFast chat** and **MC Public chat** decoding toggles. Up/Down selects a row; Right
or OK changes it; Back returns to reception. Changes are saved immediately and persist across launches. Logging and sound change
immediately; Quick/Thorough and chat decoding choices apply to the next capture. Off cancels
current decoding. Fresh defaults are **logging On, sound On, Thorough decoding, MT LongFast chat On, MC Public chat On**.
Enabling Packet sound plays a preview of the 100 ms chirp. Sound respects the Flipper's system volume/mute setting.
**Auto backlight** (On by default, configurable in Settings) wakes the screen
on every detected packet and when a decoding job finishes,
even with packet sound off. It uses the system brightness and normal backlight timeout.

Quick makes one timing pass; Thorough tries five. Off cancels pending work and keeps
RF detection, RSSI, alerts and detection CSV logging active. The decoder workspace is
reserved when memory permits so it can be re-enabled without restarting. If memory
is temporarily tight at launch, allocation is retried once per second. Common
pulses use one 16-bit word; long gaps use a three-word escape, retaining full
microsecond precision. The allocation check reserves room for decoder and service
threads and checks the largest contiguous heap block.

The MT LongFast chat option tries only the public LongFast PSK/channel hash, and only after a
packet passes CRC. It can label node info, routing, traceroute query/reply, neighbor,
position, telemetry and text traffic, with a short chat preview on screen and full text in the phone log. Private
channels and PKI messages remain opaque. CRC is not sender authentication. MeshCore
Public chat uses its standard public key and checks the two-byte channel MAC before displaying text. The MAC does not authenticate the sender name. Both chat options apply to the next capture. MeshCore status/login/telemetry requests are encrypted: only their outer request/reply type is
visible. Unencrypted MeshCore discovery exposes the query tag and a replying node's
public-key prefix, not its name unless an advert provides one.

Build with `ufbt` using the official 1.4.3 SDK. Host checks require a C compiler
and Python with `numpy`, `scipy`, and `cryptography`:

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

* **Left:** no action.
* **Right:** open the transmit menu; Up/Down selects, OK sends one packet, Back cancels/closes.
* **OK:** wake the screen without changing reception mode.
* **Menu → Channel:** select the receive profile, saved across launches.
* **Menu → Mode:** Watch or Scan (10 seconds per profile), saved across launches. Watch is the default.
* **Up / Down:** browse the eight most recently heard signal types.
* **Hold Up:** switch between traffic details and the original RF view.
* **Hold OK:** open settings and save defaults.
* **Settings → Logging:** toggle and save text/CSV logging; `REC` means enabled, `ERR` means a write failed.
* **Back:** stop reception and exit.

### Test transmission and local discovery

Right opens three actions for each protocol: **test**, **local query**, and **write message**.
Choose **MC write message** or **MT write message** to open the Flipper keyboard.
The chat keyboard has visible **Space**, **Del**, case, and **123** punctuation keys.
Select **Done** to preview, then OK to send once; Back returns to editing,
and Back from the keyboard cancels. Left moves within the keyboard but remains inactive
on the main screen and menus. MC allows 34 characters (sent as `Flipper: your text`);
LongFast allows 44. Empty messages are rejected, and successful sends are included
in the phone log when logging is enabled. Each OK sends one fresh packet; there are no automatic retries.
MC uses US/CA 910.525 MHz, SF7/BW62.5/CR4/5. MT uses US LongFast 906.875 MHz,
SF11/BW250/CR4/5. Actions select their own fixed network profile, then leave the app
in Watch mode on that profile. The Channel menu controls listening, not arbitrary TX tuning.
TX route applies to test messages: MC Flood or empty-path Direct; MT three-hop or
zero-hop broadcast. Queries are always local, regardless of TX route.

**MC hop bytes** selects 1-, 2-, or 3-byte MeshCore path hashes, with **2 bytes**
as the default, including when upgrading existing settings. It applies to both
Public message actions and local repeater queries. This is the width of each
repeater hash, not a hop limit. The initial path has zero hops and encodes the
selected width in its top two bits: `0x00`, `0x40`, or `0x80`. Received paths use
the width carried by the packet; Meshtastic is unaffected. Four-byte hashes are
reserved by [MeshCore](https://github.com/meshcore-dev/MeshCore/blob/main/src/Packet.cpp)
and are not offered. Channel, route, and hop width persist across launches.

MC discovery requests full public keys from nearby repeaters, with a unique tag.
MT sends a NodeInfo announcement named `Flipper test`/`FLIP` with `want_response` and
hop limit zero. This discovers participating nodes, not exclusively repeaters.
Firmware throttling, node role and channel settings can suppress replies. The saved
MT node ID stays stable between launches. No node identity is impersonated.

Queries leave a 60-second listening window and a one-minute TX cooldown; messages
have a five-second cooldown. Replies matching the query ID show separate `OK` (CRC-checked) and `?`
(tentative MeshCore decode) counts. Normal history and phone logs hold received details and signal levels.
Decoding/chat settings still apply; enable decoding and MT LongFast chat for MT replies.
The receive capture remains bounded, so dense simultaneous responses can be missed.

The transmit worker cancels decoding before borrowing its memory. It precomputes a
packed waveform, sends it via timer/DMA, then restores RX before logging. MC uses 1 us
samples; MT uses a centered -125–125 kHz chirp at 2 us sampling to reduce the
pulse rate enough for the stock DMA refill path. Its conjugate chirp is also emitted. TX completion means the burst ended,
not that a remote node acknowledged it. Nominal PA is fixed at -10 dBm as in the bench
experiment; useful sideband power is lower. The scoped US-region exception and
immediate restoration follow the bench implementation; no region file is changed.

Fresh packet counters persist before transmission. MC timestamps use the existing
bench UTC-offset file, if present; otherwise the Flipper clock is assumed to be UTC.
`python3 tests/check_transmit.py` independently checks crypto and demodulates the actual
packed waveform to verify all four packet formats and their PHY checksums.

Main-app hardware validation on 2026-10-11: the RTL-SDR recovered MC `Flipper bench:
TEST 6` with CRC and Public MAC valid, and MT `Flipper TEST 10` with CRC valid and
correct default-key decryption. The Flipper also captured a CRC-valid network rebroadcast of `Flipper TEST 10`
with hop limit reduced from 3 to 2, confirming native-node acceptance. Private IQ and decoded evidence are under `captures/tx-main/` (gitignored).

The traffic view shows an advert name when recoverable, a traffic type such as advert,
ACK, channel message or path, and the received RSSI. Public-channel chat appears when the corresponding decoding option is enabled.
`CRC OK` means the packet checksum passed; advert signatures are **not** checked on the
Flipper. `UNVERIFIED` and a `?` mean damaged/tentative data: names and types may be wrong.
Close repeats with similar identity and timestamp bytes are soft-combined to improve
advert readability, but remain unverified. Wait for further copies if the name is missing.
The full recovered name (up to 32 bytes) is retained in the log; the small screen shows
its first 22 characters. Non-ASCII bytes display as `?`.

The decoder uses a fixed ~27 KB working area plus a 20 KB packed snippet buffer, and falls
back to detection-only if there is insufficient free heap. After a supported detection,
it captures up to 2.5 seconds (or 10,240 packed 16-bit words), then decodes on a background thread that time-shares with the radio worker, trying one (Quick) or five (Thorough) small timing offsets. Expect several seconds per packet before initial details appear; the full timing search
can take around 1–2 minutes with live reception running and may improve tentative candidates. The radio
keeps detecting while a snippet is processed; packets arriving during decoding are
skipped for payload capture, counted on screen, and still logged when recording. They
do not replace the captured packets in history. The footer shows progress and elapsed
seconds, then `Done`; an unsuccessful attempt ends with `No payload recovered`. A retune or lost samples abort an unfinished capture. Previously captured snippets
can finish decoding after a retune, with their original frequency and RSSI. The Python decoder remains more capable at recovering damaged signals.

Logging defaults on; saved preferences override the initial defaults. Records append to `/ext/apps_data/lora_detect/detections.csv`;
timestamps are device uptime in milliseconds, sync is decimal (`-1` means unreadable),
and RSSI is sampled when detection completes. With logging enabled, payload results
also append to `/ext/apps_data/lora_detect/traffic.csv`, including type, name, RSSI,
confidence and combined-copy count. A green LED blink indicates a detection.

For the stock iPhone Flipper app, decoded results also append to
`/ext/apps_data/lora_detect/LoRa Logs.txt`. It shows detection date/time (from the
Flipper clock), traffic type, recovered name, RSSI, frequency and verification status.
Check that `REC` is shown (Hold OK → Logging controls it); allow the background decode to finish before viewing results.
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
Flipper. Live MeshCore and LongFast reception has been exercised; the computed
125/500 kHz presets still need field validation. See [design notes](docs/APP_DESIGN.md) for details.

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
* The CC1101 has no native LoRa modem. The integrated experimental transmitter uses OOK sidebands, based on the [bench experiment](experiments/tx_bench/README.md). MeshCore bench chat reception and exact SDR decoding have been demonstrated; this is not a native LoRa radio or a measured emissions-compliance result.

## Privacy note

MeshCore adverts and Meshtastic position packets often include **GPS coordinates**, and
anyone with a Flipper can now read them. Raw captures contain whatever was on the air, so
`captures/` is gitignored. Don't publish captures without scrubbing them.

## Decoder performance and validation

The on-device decoder uses integer cyclic correlation, soft deinterleaving/Hamming
correction, and CRC. It does not use FFTs or verify Ed25519 signatures. Most work is
recovering symbols from the CC1101's one-bit discriminator samples. v0.5 replaces a
linear pulse-history search per sample with binary search and shares bit-score work.
AES-CTR for a short CRC-checked MT packet is a separate, optional, small step.
Turning public-chat crypto off does not remove the expensive symbol search.
The implementation already uses the radio capture hardware and timer/DMA for
edge timing; it does not rely on an FFT accelerator or hardware AES engine.

There is one bounded payload job, not a queue of full captures. While that job
runs, detection and alerts continue, but additional payload captures are skipped
and counted. Quick reduces the timing search; Thorough spends more time trying
to improve recovery. Adding a queue would require additional capture memory and
would not increase the decoder's throughput.

Host tests cover clean MT waveforms at SF7–SF11 through CRC, AES and traceroute
classification, independent AES test vectors, malformed protobufs, MeshCore discovery,
and the older MC regressions. The existing noisy LongFast recording yields tentative
headers but still fails CRC: desktop list decoding remains more capable at repairing
those packets. Synthetic success is not a promise of reliable decoding of every live
Meshtastic packet.

### Packet bytes for later analysis

With logging enabled, `apps_data/lora_detect/packets.csv` stores the best candidate
per packet in each captured snippet, including unknown, encrypted, and incomplete
results. It includes the detection timestamp (Flipper clock), frequency, bandwidth,
SF, coding rate, RSSI, network, completeness, CRC metadata, and uppercase payload hex.
`LoRa Logs.txt` includes the same recovered bytes for the phone viewer.

Hex is the dewhitened on-air payload candidate, including encrypted bytes; it is
not decrypted text or raw RF samples. Partial results contain only recovered whole
bytes, never padding. A failed header has no payload hex and its declared length
is untrusted. CRC fields are meaningful only for a complete packet with
`crc_present=1`. Combined advert names can come from multiple copies while the hex
remains the individual packet candidate. Existing logs cannot be reconstructed
retroactively. With Decode Off, unsupported PHYs, or packets skipped while busy,
only detection logging is available. Logging Off disables hex logging too.

### Tentative identification

The screen prioritizes a likely packet type and any recovered node name, marking
uncertain types with `?`. For a damaged MeshCore advert with a corrupt path length,
the decoder can recover a tentative name if exactly one alternative advert layout
has valid node flags and a mostly printable name. It does not correct spelling,
change the packet hex, or promote the packet to CRC-valid. Details identify inferred
layouts; logs retain the CRC status. Ambiguous layouts are left unnamed.

Meshtastic public-channel decoding also distinguishes store-and-forward messages
(including router heartbeats), routing ACK/NAK, and decoded but unrecognized app
ports. Protocol definitions: [Meshtastic protobufs](https://github.com/meshtastic/protobufs).

### MeshCore edge timing

MeshCore decoding now fits a symbol clock to long-low falling edges in the
preamble. With at least six edges and RMS timing residual at most 0.3 chip, it
uses wrap-edge timing to refine symbol scores; otherwise it keeps the existing
correlation decoder. This is automatic in Quick and Thorough modes. It does not
relax CRC checks or suppress tentative names.

Synthetic MeshCore tests with 16–64 microseconds of discriminator-threshold
movement now pass CRC where correlation alone fails. The existing coarse private
capture is rejected by the edge-quality check and retains its prior recovery.
This is a regression result, not a measured live-traffic success rate.

## Credits

- **Paul Timmins** — project direction, hardware testing, field testing, and maintenance.
- **Claude (Anthropic)** — initial radio presets, Python analysis and decoding tools,
  and the original Flipper app design notes.
- **Codex (OpenAI)** — native Flipper app implementation and subsequent work on
  background decoding, on-device settings, logging, protocol summaries, performance,
  device validation, OOK-sideband MeshCore/LongFast transmission, discovery, and
  the message composer, in collaboration with Paul.

## License

MIT, see [LICENSE](LICENSE).
