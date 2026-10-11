# Install LoRa Detect — no compiler required

1. Download **lora_detect.fap** from the [GitHub Releases page](https://github.com/PaulTimmins/Flipper-LoRa-Detect/releases). Expand **Assets** if needed. The Source code archives are for developers; they are not the app.
2. Connect your Flipper Zero to your computer with USB and open [qFlipper](https://flipper.net/pages/downloads).
3. Close any running app on the Flipper. In qFlipper, open **File Manager → SD Card → apps → Sub-GHz**. Create the Sub-GHz folder if it does not exist.
4. Copy **lora_detect.fap** into that folder, replacing the old file if updating.
5. On the Flipper, open **Apps → Sub-GHz → LoRa Detect**.

An SD card is required. No Python, compiler, Codex, external LoRa module, or custom firmware is needed. Presets and the icon are built into the app. Settings and logs are created on the SD card automatically. Replacing the app preserves them.

## Supported build

This prerelease is built and device-tested with **official firmware 1.4.3, API 87.1, hardware target f7**. Other firmware versions may work if their APIs are compatible, but custom firmware is not tested. If the device reports an API mismatch, use a compatible app build or firmware; renaming the file does not fix it.

`SHA256SUMS.txt` contains the checksum of the release binary. This is an experimental release, with native-node and SDR interoperability demonstrated but no measured delivery-rate or range guarantee.

## First use

- **Hold OK:** settings. Select Channel and Watch/Scan mode here.
- **Short OK:** wake the screen.
- **Right:** send menu. Choose a numbered test, local discovery, or write a message for MeshCore Public or Meshtastic LongFast.
- **Write message:** type with the visible Space and punctuation keys; Done previews, then OK sends once. Back cancels/returns to editing.
- **Back from the listening screen:** exit.

Logging, sound, automatic backlight, Thorough decoding, and both public-chat decoders default on. MeshCore hop hashes default to 2 bytes and are configurable. Existing saved settings take precedence. Decoding takes time; the radio continues detecting while a payload is processed.

TX uses fixed US network profiles: MeshCore 910.525 MHz and Meshtastic LongFast 906.875 MHz. It is an experimental internal-radio OOK-sideband transmitter, not a native LoRa modem. It emits mirrored components and harmonics; emission compliance has not been measured. On provisioned US devices, it uses the documented temporary in-memory region exception for its carrier and restores the original region. No persistent region file is changed. It does not provide arbitrary-frequency TX or a general non-US exception. See the main README for methods and limitations.

## Logs and updates

Logs live in `SD Card/apps_data/lora_detect/`. The iPhone Flipper app can open `LoRa Logs.txt` through its File Manager; reopen to refresh. Files may contain node names, messages, and other received traffic—review before sharing.

For updates, download the new `.fap`, exit LoRa Detect, and replace the existing file through qFlipper. This prerelease is distributed through GitHub, not yet through the Apps Catalog.
