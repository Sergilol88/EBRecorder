# EB Recorder v0.3.0

First public release of EB Recorder.

## Highlights

- Records only the highest-resolution active Twitch Enhanced Broadcasting rendition.
- Reuses OBS's existing EB video encoder — no additional video encoder is created by the plugin.
- Reuses the existing EB live AAC encoder.
- Writes crash-resilient Matroska (`.mkv`) recordings to the current OBS recording directory.
- Optional automatic recording start with the EB stream.
- Automatic stop before the EB encoder pipeline is torn down.
- Manual Start / Stop controls remain available.
- English and Russian UI.
- No Windows display, refresh-rate, VRR/G-SYNC, NVIDIA, registry, service, power-plan or PATH configuration changes.

## Tested with

- OBS Studio 32.2.2 x64
- Windows 10/11 x64
- Twitch Enhanced Broadcasting / Multitrack Video
- NVIDIA NVENC
- HEVC 2560x1440 60 fps TOP rendition
- AAC stereo 48 kHz / 160 kbps

## Installation

1. Close OBS.
2. Extract `EBRecorder-v0.3.0-Windows-x64.zip`.
3. Run `INSTALL.cmd`.
4. Start OBS and open **Tools -> EB Recorder**.

## Important notes

This release is currently validated against OBS Studio 32.2.2. Future OBS changes to Multitrack Video encoder naming or lifecycle may require an EB Recorder compatibility update.

The crash-resilient MKV path has been tested by forcibly terminating OBS during an active EB Recorder recording; the resulting file remained playable. A real power loss can still lose a small amount of data that has not yet reached persistent storage.
