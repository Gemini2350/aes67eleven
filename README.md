# AES67eleven — Virtual Soundcard with NMOS Support

Virtual AES67 soundcard for macOS (Core Audio) with NMOS IS-04/IS-05 (+ IS-12/BCP-008,
IS-13 Annotation). Protocol stack ported from the author's pi-audio-node project; driver
built on [libASPL](https://github.com/gavv/libASPL) (MIT). Also runs as a **headless
Linux/Docker gateway** (same code, no driver) — image:
[`gemini2350/aes67eleven-gw`](https://hub.docker.com/r/gemini2350/aes67eleven-gw) /
`ghcr.io/gemini2350/aes67eleven-gw` (built by CI).

## Architecture

```
Apps ──play──▶ [AES67 Out] ─loopback─▶ daemon ─▶ RTP sender (PTP-paced) ─▶ network
Network ─▶ RTP receiver ─jitter buffer─▶ daemon ─▶ [AES67 In] ─loopback─▶ apps record
        Daemon: PTP slave ─▶ clock discipline (custom property) ─▶ driver clock
        Daemon: NMOS IS-04/05 node · IS-12/BCP-008 · IS-13 · web UI
```

- **`AES67VSC.driver`** (AudioServerPlugIn, runs inside coreaudiod): **one visible
  device "AES67eleven"** (64 in / 64 out, 48 kHz, Float32) for apps — output goes to
  the network (TX), input comes from the network (RX). Internally there is an
  additional **hidden bridge device** (`AES67VSC:Bridge`,
  `kAudioDevicePropertyIsHidden`) the daemon attaches to; both are cross-wired
  through timeline rings and share one zero-timestamp generator (identical sample
  clock). **The device clock is PTP-disciplined**: the daemon measures d(PTP)/d(host)
  with its IEEE 1588 slave and publishes the rate ~1×/s through a custom property
  (`'ptpS'`) into the driver; the driver's zero timestamps slew towards it
  (±500 ppm clamp, ≤2 ppm per period). No resampling in the audio path — samples run
  1:1 on the media clock. Without daemon/lock: holdover at the last known rate.
- **`aes67vsc-daemon`** (LaunchAgent, no root): PTP slave (slave-only, BMCA, E2E),
  RTP TX/RX (L24/L16, 1 ms ptime, ST 2022-7 legs), NMOS node, web UI.

## Build & install

```bash
scripts/install.sh      # builds, installs driver (sudo) + daemon, restarts coreaudiod
```

Note: restarting coreaudiod briefly interrupts all system audio.

- Web UI: `http://localhost:8180`
- NMOS Node API: `http://<ip>:8081/x-nmos/node/v1.3/`, Connection API
  `…/connection/v1.1/`, Annotation API (IS-13) `…/annotation/v1.0/`
  (port via `nmos.http_port`)
- Config: `~/Library/Application Support/aes67-vsc/config.json`
  (`network.interface_primary` = media interface, e.g. `en6`)
- Logs: `~/Library/Logs/aes67-vsc/daemon.log`
- Uninstall: `scripts/uninstall.sh`

On first start macOS asks for the **"Local Network" permission** for the daemon —
allow it (System Settings → Privacy & Security → Local Network), otherwise no
multicast will arrive. PTP needs wired Ethernet; Wi-Fi timestamps are unusable.

## Sending / receiving

- **Sending**: route app output to the device → an NMOS sender streams its mapped
  channels as L24 multicast.
- **Receiving**: patch a receiver via IS-05 (SDP as transport_file) or connect it in
  the web UI → the stream appears on the device input for recording.

## Dynamic streams on the 64-channel matrix

AES67 limits a single stream at 48 kHz / 1 ms / L24 to 8 channels; the 64 device
channels are served by **dynamically created senders/receivers** (up to 64 each) with
free channel mapping:

- **Create/remove at runtime** (web UI or API): `POST /api/action/stream-add`
  `{type: "sender"|"receiver", channels, channel_offset or channel_map, label}` ·
  `stream-remove` `{type, index}` ·
  `stream-map` `{type, index, channel_map: [0-based device channels]}`.
- **Matrix routing**: a per-stream `channel_map` routes stream channel i onto device
  channel map[i] — arbitrary, including non-contiguous channels (e.g. a mono stream
  onto channel 33). Without a map, the contiguous block `channel_offset`+`channels`
  applies.
- Every stream carries a persistent `uid` in the config → **NMOS UUIDs stay stable**
  regardless of what is created or removed next to it. Newly created streams get
  deterministic slot uids (`tx-N`/`rx-N`), so deleting and re-creating a stream
  yields the same UUID again. `uid: "legacy"` = the v1 IDs (existing controller
  routes stay valid).
- A topology change is a short soft rebuild: streams stop, NMOS re-registers all
  resources, removed ones are DELETEd from the registry, IS-12 monitors are
  re-enumerated (controllers reconnect). Running streams are interrupted for a few
  seconds.
- Audio path: IOProcs read the tap lists as atomically swapped snapshots — no lock
  on the realtime path.
- Old configs (single sender or fixed slots) are migrated at startup.
- **Web UI**: the "Matrix" tab shows the TX/RX matrix graphically (clicking a cell
  assigns or removes a device channel; the digit is the stream channel). The
  receiver/sender pages have a stream selector (RX 1…n / TX 1…n) — meters, BCP-008
  status, connect and transport refer to the selected stream.
- The daemon runs as **"AES67eleven.app"** (`/Applications`) with a **menu bar
  icon**: status line (PTP/TX/RX), "Open web UI", "Quit" (clean stop; launchd only
  restarts after crashes, not after a deliberate quit). Double-clicking in Finder
  starts the app, or opens the web UI when it is already running. Signed with the
  local identity "AES67 VSC Signing" so the "Local Network" grant survives updates.
- **ST 2022-7**: set `network.interface_secondary` (e.g. `en5`) → senders duplicate
  onto leg 2 (`multicast_secondary`), receivers merge seamlessly (`a=group:DUP`),
  and the PTP analyzer compares amber/blue via **meta BMCA** (PTP page of the web
  UI). Interface changes apply at runtime.

## Monitor mode (listen to received audio locally)

The daemon can route the receiver's monitor channels (monitor matrix in the UI,
`receiver.monitor_left/right`) directly to a local output device:

- UI: **"Monitor"** button on the receiver page, or config: `monitor.enabled`
  (bool), `monitor.device` (`"default"` | device name, e.g.
  `"MacBook Air Speakers"` | device UID), `monitor.gain_db`.
- With several output devices attached, prefer setting the **device name
  explicitly** — `default` is resolved when the monitor starts and does not follow
  later changes of the system output.
- The monitor path runs on the output device's crystal (not PTP): drift is absorbed
  by trim/silence — meant for listening, not as a synchronous output.

## Player

The web UI has a **Player** tab with 4 decks: upload WAV/MP3 (decoded to 48 kHz on
load), play/pause/stop, loop, **auto-play on service start**, gain, free choice of
the L/R target channels on the 64-channel matrix, seek. The players mix onto the TX
bus — every sender whose matrix covers those channels streams them as AES67.

## Linux / Docker gateway

The same daemon builds on Linux as `aes67vsc-gateway` (no Core Audio): player,
matrix, NMOS, PTP, ST 2022-7 — a headless AES67 playout/monitoring node.

```bash
cd docker && docker compose up -d       # or: image gemini2350/aes67eleven-gw:latest
```

Mandatory: `network_mode: host` (multicast/PTP/DNS-SD); recommended:
`cap_add: [SYS_NICE]` plus an rtprio ulimit for stable packet pacing. Config lives
in the volume at `/data/config.json` (see `docker/config.example.json` — set a
unique `uuid_seed` per host!). Note: inside a VM, PTP suffers from vCPU stalls; the
servo is hardened against them, but bare metal remains clearly better.

## NMOS / AMWA conformance

- IS-04 v1.3 (Node), IS-05 v1.1 (Connection), IS-12/MS-05 (Control, WebSocket),
  **IS-13 v1.0 (Annotation)** — labels/descriptions/tags of node, device, senders,
  receivers, sources and flows can be changed by controllers and persist;
  BCP-008-01/-02 monitors with statusReportingDelay per spec; BCP-002-02 asset tags
  (read-only via IS-13).
- IS-13 test suite (garethsb `is-13-annotation-tests`): **0 fails**.

## Verification

- `api/status` provides PTP offset/rate (`ptp.rate_ppm`), sender pacing
  (`late_avg_us`/`late_max_us`), ring fill levels (`device.*`), driver clock echo
  (`device.clock` = "ppb locked updates").
- On-wire check: `tcpdump -i en6 -w cap.pcap udp port 5004`, then analyze the
  capture with an AES67 PCAP tool.

## Limitations

- 48 kHz only, no SAP (discovery via NMOS/SDP), max. 8 channels per stream
  (AES67 MTU).
- Software PTP: sub-ms phase (good enough for the media clock), no hardware
  timestamping.
- LaunchAgent = runs only while a user is logged in.

## License

MIT — see [LICENSE](LICENSE). Bundled third-party components: civetweb (MIT),
nlohmann/json (MIT), dr_wav (public domain/MIT-0), minimp3 (CC0), libASPL (MIT,
via CMake).
