# EB Recorder

[Русская версия](README_RU.md)

**Windows 10/11 • OBS Studio plugin • Twitch Enhanced Broadcasting**

**EB Recorder** records the highest-resolution active Twitch Enhanced Broadcasting rendition locally by reusing the encoder that OBS has already created for the stream.

The goal is simple: keep a local high-quality VOD **without starting another video encode**.

## Why this exists

OBS Enhanced Broadcasting can create several simultaneous video renditions for Twitch. Starting a normal local recording can add another encode session and extra GPU load.

EB Recorder instead finds OBS's active Enhanced Broadcasting encoders, selects the highest-resolution rendition, and attaches that existing video encoder — plus the existing EB audio encoder — to a separate local Matroska output.

```text
Existing Twitch EB TOP video encoder ─┐
                                      ├─ EB Recorder -> MKV
Existing Twitch EB live AAC encoder ──┘
```

No extra video encoder is created by EB Recorder.

## Features

- Records only the highest-resolution active Twitch Enhanced Broadcasting rendition.
- Reuses the existing EB video encoder instead of creating another one.
- Reuses the existing EB live AAC encoder.
- Writes Matroska (`.mkv`) files to the current OBS recording directory.
- Crash-resilient recording using short Matroska clusters (`cluster_time_limit=1000`).
- Optional **automatic recording start with the EB stream**.
- Automatic stop before the Enhanced Broadcasting encoder pipeline is torn down.
- Manual Start / Stop remains available.
- English and Russian UI.
- Does not modify Windows display mode, refresh rate, VRR, G-SYNC, NVIDIA settings, registry, services, power plans or PATH.

## Tested configuration

The current release has been tested with:

- OBS Studio `32.2.2` x64 on Windows
- Twitch Enhanced Broadcasting / Multitrack Video
- NVIDIA NVENC
- TOP rendition: HEVC `2560x1440 60 fps`, approximately `9000 kbps`
- AAC stereo `48 kHz`, `160 kbps`

Other Enhanced Broadcasting encoder combinations may work, but are not yet part of the tested matrix.

## Installation

1. Open the repository's **Releases** page.
2. Download the latest `EBRecorder-vX.Y.Z-Windows-x64.zip` release archive.
3. Close OBS completely.
4. Extract the archive.
5. Run `INSTALL.cmd`.
6. Start OBS and open **Tools -> EB Recorder**.

The installer uses this isolated OBS plugin directory:

```text
C:\ProgramData\obs-studio\plugins\eb-recorder\
```

It does not create registry entries, services, scheduled tasks or PATH changes.

To remove EB Recorder, close OBS and run `UNINSTALL.cmd` from the extracted release package.

## Usage

Start a Twitch Enhanced Broadcasting stream first. Once OBS's EB encoders become active, open **Tools -> EB Recorder**.

The diagnostics table shows the discovered EB video encoders. The highest-resolution active encoder is marked as **TOP**.

Press **Start recording** to begin a local MKV recording, or enable:

> **Automatically start recording with the EB stream**

When automatic mode is enabled, EB Recorder waits for the EB video/audio encoders to become active and then starts the local recording automatically. The plugin window does not need to remain open.

Disabling the checkbox does not stop an already-running recording. Manually stopping an automatically started recording does not restart it during the same stream session.

## Output

Files are written to the same folder configured in OBS for local recordings. In **Simple** output mode, this is the path from **Settings → Output → Recording → Recording Path**. EB Recorder does not keep a separate recording-folder setting of its own.

```text
EBRecorder_yyyy-MM-dd_HH-mm-ss.mkv
```

The local file contains only the selected TOP EB video rendition plus the EB live AAC track.

## Crash resilience

EB Recorder uses OBS's FFmpeg muxer with Matroska and:

```text
cluster_time_limit=1000
```

In testing, an MKV remained playable after forcibly terminating OBS while recording. With a real system crash or power loss, a small final tail may still be lost because data can remain in OS/device write caches, but the already-written recording should not depend on a clean finalization step to remain usable.

## Performance model

EB Recorder does not create a new video encoder. Its local recording path receives already-encoded packets from OBS's existing Enhanced Broadcasting encoder and muxes them to disk.

In the tested three-rendition EB session, OBS still showed three GPU encode threads while EB Recorder was active — no fourth video encode thread was introduced by the plugin.

## VRR / G-SYNC behavior

EB Recorder does not call Windows display configuration APIs, DXGI presentation/swap-chain APIs, NVIDIA NvAPI, registry APIs, DWM configuration APIs, timer-resolution APIs or power-plan tools.

The diagnostics window polls encoder state once per second with a coarse Qt timer and only repaints when the displayed state changes. With the dialog closed and automatic recording already started, there is no periodic EB Recorder UI repaint loop.

A visible OBS/Qt window still participates in normal Windows desktop composition, so systems using windowed VRR/G-SYNC can still exhibit display-specific compositor flicker unrelated to persistent settings changes.

## Building from source

See [BUILD.md](BUILD.md).

## Current limitations

- Windows x64 is the only tested release platform.
- The plugin currently records the highest-resolution active EB rendition only; manual rendition selection is not implemented.
- The EB encoder names are discovered from OBS's current Multitrack Video naming convention.
- The release has been validated against OBS Studio 32.2.2; future OBS changes can require compatibility updates.
- EB Recorder records the EB live audio track exposed by OBS; separate local audio stems are not currently supported.

## Bugs and feature requests

Please use GitHub Issues. Include:

- EB Recorder version,
- OBS Studio version,
- Windows version,
- GPU model,
- Enhanced Broadcasting encoder/rendition information if known,
- the relevant OBS log section containing `[EB Recorder]`,
- exact reproduction steps.

## Project status

`v0.3.0` is the first feature-complete public baseline: TOP-only encoder reuse, MKV output, crash-resilient recording, manual controls and optional automatic start with the EB stream.

## License

EB Recorder is licensed under the [GNU General Public License v2.0 or later](LICENSE).

OBS Studio is a separate project. Twitch and Enhanced Broadcasting are trademarks/services of their respective owners. EB Recorder is not affiliated with or endorsed by OBS Studio or Twitch.
