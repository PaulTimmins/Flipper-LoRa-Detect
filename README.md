# flipper-lora-detect

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
