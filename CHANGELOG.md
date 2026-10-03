# Changelog

## 0.3.0

First public baseline.

- Added persistent optional automatic recording start with the EB stream.
- Automatic start works with the EB Recorder dialog closed.
- Manual stop suppresses another automatic restart during the same stream session.
- Kept TOP-only reuse of the existing Twitch EB video encoder.
- Kept reuse of the existing EB live AAC encoder.
- MKV recording through OBS `ffmpeg_muxer`.
- Matroska `cluster_time_limit=1000` for crash-resilient output.
- Local recording stops before EB encoder teardown.
- Reduced diagnostics UI activity: 1-second coarse polling and repaint only when state changes.
- Automatic-start polling is lazy and stops after success.
- No display/VRR/G-SYNC configuration APIs are used.
- Improved dependency download retry/integrity diagnostics.
- Production Windows package now builds the plugin as CMake `Release`, excludes PDB/debug-symbol files, and emits a SHA-256 sidecar for the release asset.

## 0.2.1

- Switched local output from FLV to Matroska (`.mkv`).
- Added short Matroska clusters for crash resilience.
- Verified that a recording remains playable after forced OBS termination.

## 0.2.0

- Added actual local recording.
- Reused the existing highest-resolution Enhanced Broadcasting video encoder.
- Reused the existing EB live AAC encoder.
- Confirmed no additional video encode thread was introduced in the tested setup.

## 0.1.1

- Stable diagnostics UI and localization.
- Added duplicate logical module-load guard for the tested OBS plugin layout.
- Fixed dialog lifecycle and shutdown handling.

## 0.1.0

- Initial read-only diagnostics prototype.
- Enumerated OBS Enhanced Broadcasting video encoders and selected the highest-resolution active rendition.
