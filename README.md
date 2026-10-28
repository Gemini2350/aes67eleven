# AES67eleven — Virtual Soundcard mit NMOS Support

Virtuelle AES67-Soundkarte für macOS (Core Audio) mit NMOS IS-04/IS-05 (+ IS-12/BCP-008, IS-13 Annotation).
Protokoll-Stack portiert vom eigenen pi-audio-node-Projekt; Treiber auf Basis von
[libASPL](https://github.com/gavv/libASPL) (MIT). Läuft zusätzlich als **headless
Linux/Docker-Gateway** (gleicher Code, ohne Treiber) — Image:
[`gemini2350/aes67eleven-gw`](https://hub.docker.com/r/gemini2350/aes67eleven-gw).

## Architektur

```
Apps ──play──▶ [AES67 Out] ─Loopback─▶ Daemon ─▶ RTP-Sender (PTP-getaktet) ─▶ Netz
Netz ─▶ RTP-Receiver ─Jitterbuffer─▶ Daemon ─▶ [AES67 In] ─Loopback─▶ Apps record
        Daemon: PTP-Slave ─▶ Clock-Discipline (Custom Property) ─▶ Treiber-Clock
        Daemon: NMOS IS-04/05-Node · IS-12/BCP-008 · IS-13 · WebUI
```

- **`AES67VSC.driver`** (AudioServerPlugIn, läuft in coreaudiod): **ein sichtbares Device
  „AES67"** (64 In / 64 Out, 48 kHz, Float32) für Apps — Output geht ins Netz (TX),
  Input kommt aus dem Netz (RX). Intern existiert zusätzlich ein **verstecktes
  Bridge-Device** (`AES67VSC:Bridge`, `kAudioDevicePropertyIsHidden`), über das der
  Daemon andockt; beide sind über Timeline-Ringe kreuzverdrahtet und teilen sich
  einen ZeroTimestamp-Generator (identische Sample-Clock).
  **Die Device-Clock ist PTP-diszipliniert**: Der Daemon misst d(PTP)/d(Host) mit seinem
  IEEE-1588-Slave und publiziert die Rate ~1×/s per Custom Property (`'ptpS'`) in den
  Treiber; dessen ZeroTimestamps slewen dorthin (±500 ppm Clamp, ≤2 ppm/Periode).
  Kein Resampling im Audiopfad — Samples laufen 1:1 auf der Media-Clock.
  Ohne Daemon/Lock: Holdover mit letzter Rate.
- **`aes67vsc-daemon`** (LaunchAgent, kein Root): PTP-Slave (Slave-only, BMCA, E2E),
  RTP TX/RX (L24/L16, 1 ms ptime, ST 2022-7 Legs), NMOS-Node, WebUI.

## Build & Installation

```bash
scripts/install.sh      # baut, installiert Treiber (sudo) + Daemon, startet coreaudiod neu
```

Achtung: Der coreaudiod-Neustart unterbricht kurz sämtliches Systemaudio.

- Web-UI: `http://localhost:8180`
- NMOS Node API: `http://<ip>:8081/x-nmos/node/v1.3/`, Connection API `…/connection/v1.1/`,
  Annotation API (IS-13) `…/annotation/v1.0/` (Port via `nmos.http_port`)
- Config: `~/Library/Application Support/aes67-vsc/config.json`
  (`network.interface_primary` = Media-Interface, z. B. `en6`)
- Logs: `~/Library/Logs/aes67-vsc/daemon.log`
- Deinstallation: `scripts/uninstall.sh`

Beim ersten Start fragt macOS nach der **„Lokales Netzwerk"-Berechtigung** für den
Daemon — zulassen (System Settings → Privacy & Security → Local Network), sonst kommt
kein Multicast an. PTP braucht verkabeltes Ethernet; WLAN-Timestamps sind unbrauchbar.

## Senden / Empfangen

- **Senden**: App-Ausgabe auf „AES67 Out" legen → NMOS-Sender streamt die ersten
  `sender.channels` Kanäle als L24-Multicast. `sender.source` kann für Tests auch
  `tone`/`sweep`/`file` sein.
- **Empfangen**: Receiver per IS-05 patchen (SDP als transport_file) oder über die
  WebUI verbinden → Stream liegt auf „AES67 In" zum Aufnehmen an.

## Dynamische Streams auf der 64ch-Matrix

AES67 begrenzt einen Stream bei 48 kHz / 1 ms / L24 auf 8 Kanäle; die 64 Device-Kanäle
werden von **dynamisch anlegbaren Sendern/Receivern** (bis je 64) mit freiem
Kanal-Mapping bedient:

- **Anlegen/Löschen zur Laufzeit** (Web-UI „Streams"-Karte oder API):
  `POST /api/action/stream-add` `{type: "sender"|"receiver", channels, channel_offset
  oder channel_map, label}` · `stream-remove` `{type, index}` ·
  `stream-map` `{type, index, channel_map: [0-basierte Device-Kanäle]}`.
- **Matrix-Routing**: `channel_map` je Stream routet Stream-Kanal i auf Device-Kanal
  map[i] — beliebige, auch nicht zusammenhängende Kanäle (z. B. Mono-Stream → Kanal 33).
  Ohne Map gilt der zusammenhängende Block `channel_offset`+`channels`.
- Jeder Stream trägt eine persistente `uid` in der Config → **NMOS-UUIDs bleiben stabil**,
  egal was daneben angelegt/gelöscht wird. `uid: "legacy"` = die v1-IDs (bestehende
  Controller-Routen bleiben gültig).
- Topologie-Änderung = kurzer Soft-Rebuild: Streams stoppen, NMOS re-registriert alle
  Ressourcen, entfernte werden per DELETE aus der Registry geräumt, IS-12-Monitore
  werden neu enumeriert (Controller verbindet sich neu). Laufende Streams sind dabei
  wenige Sekunden unterbrochen.
- Audio-Pfad: IOProcs lesen die Tap-Listen als atomar getauschte Snapshots — kein Lock
  im Realtime-Pfad.
- Alte Configs (Single-Sender oder feste 8er-Slots) werden beim Start migriert.
- **Web-UI**: Tab „Matrix" zeigt TX-/RX-Matrix grafisch (Klick auf eine Zelle ordnet
  einen Device-Kanal zu bzw. entfernt ihn; die Ziffer ist der Stream-Kanal). Die
  Receiver-/Sender-Seiten haben eine Stream-Auswahl (RX 1…n / TX 1…n) — Meter,
  BCP-008-Status, Connect und Transport beziehen sich auf den gewählten Stream.
- Der Daemon läuft als **„AES67eleven.app"** (`/Applications`) mit **Menüleisten-Icon**: Status-Zeile (PTP/TX/RX), „Web-UI öffnen", „Beenden" (sauberer Stopp;
  launchd startet nur nach Crashes neu, nicht nach bewusstem Beenden). Doppelklick
  im Finder startet die App bzw. öffnet die Web-UI, wenn sie schon läuft. Signiert
  mit der lokalen Identität „AES67 VSC Signing" — die „Lokales Netzwerk"-Freigabe
  übersteht Updates.
- **ST 2022-7**: `network.interface_secondary` setzen (z. B. `en5`) → Sender
  duplizieren auf Leg 2 (`multicast_secondary`), Receiver mergen seamless
  (`a=group:DUP`), und der PTP-Analyzer vergleicht Amber/Blue per **Meta-BMCA**
  (PTP-Seite der Web-UI). Interface-Wechsel greifen zur Laufzeit.

## Monitor-Modus (Empfang lokal abhören)

Der Daemon kann die Monitor-Kanäle des Receivers (Monitor-Matrix in der UI,
`receiver.monitor_left/right`) direkt auf ein lokales Ausgabegerät legen:

- UI: Button **„Monitor"** auf der Receiver-Seite, oder Config:
  `monitor.enabled` (bool), `monitor.device` (`"default"` | Gerätename z. B.
  `"MacBook Air Speakers"` | Device-UID), `monitor.gain_db`.
- Bei mehreren angeschlossenen Ausgabegeräten lieber den **Gerätenamen explizit**
  setzen — `default` wird beim Monitor-Start aufgelöst und folgt späteren
  Wechseln der Systemausgabe nicht.
- Der Monitorpfad läuft auf dem Quarz des Ausgabegeräts (nicht PTP): Drift wird
  per Trim/Silence abgefangen — für Abhören gedacht, nicht als synchroner Ausgang.

## Verifikation

- `api/status` liefert PTP-Offset/Rate (`ptp.rate_ppm`), Sender-Pacing
  (`late_avg_us`/`late_max_us`), Ring-Füllstände (`device.*`), Treiber-Clock-Echo
  (`device.clock` = "ppb locked updates").
- On-Wire-Check: `tcpdump -i en6 -w cap.pcap udp port 5004`, dann mit dem
  AES67-PCAP-Tool analysieren.
- Getestete Basis (ohne Treiber, Software-Loop): PTP-Lock gegen echten GM,
  1000 pkt/s Pacing (late_avg ≈ 34 µs), IS-05-SDP-Roundtrip, 0 Paketverluste.

## Grenzen

- 48 kHz only, kein SAP (Discovery via NMOS/SDP), max. 8 Kanäle pro Stream (AES67-MTU).
- Software-PTP: Phase sub-ms (Media-Clock-tauglich), kein Hardware-Timestamping.
- LaunchAgent = läuft nur bei angemeldetem User.

## Player

Die Web-UI hat einen **Player**-Tab mit 4 Decks: WAV/MP3 hochladen (Decode auf 48 kHz
beim Laden), Play/Pause/Stop, Loop, **Auto-play beim Dienststart**, Gain, freie Wahl
der Zielkanäle L/R auf der 64er-Matrix, Seek. Die Player mischen auf den TX-Bus —
jeder Sender, dessen Matrix die Kanäle enthält, streamt sie als AES67.

## Linux / Docker-Gateway

Derselbe Daemon baut unter Linux als `aes67vsc-gateway` (ohne CoreAudio): Player,
Matrix, NMOS, PTP, ST 2022-7 — als headless AES67-Zuspieler/Monitor-Node.

```bash
cd docker && docker compose up -d       # oder: Image gemini2350/aes67eleven-gw:latest
```

Pflicht: `network_mode: host` (Multicast/PTP/DNS-SD), empfohlen `cap_add: [SYS_NICE]`
plus rtprio-Ulimit für stabiles Paket-Pacing. Config im Volume unter `/data/config.json`
(siehe `docker/config.example.json` — `uuid_seed` pro Host eindeutig setzen!).
Hinweis: In einer VM leidet PTP unter vCPU-Stalls; der Servo ist dagegen gehärtet,
bare-metal bleibt aber klar besser.

## NMOS / AMWA-Konformität

- IS-04 v1.3 (Node), IS-05 v1.1 (Connection), IS-12/MS-05 (Control, WebSocket),
  **IS-13 v1.0 (Annotation)** — Labels/Descriptions/Tags von Node, Device, Sendern,
  Receivern, Sources und Flows sind vom Controller aus änderbar und persistent;
  BCP-008-01/-02 Monitore mit statusReportingDelay nach Spec; BCP-002-02 Asset-Tags
  (read-only via IS-13).
- IS-13-Testsuite (garethsb `is-13-annotation-tests`): **0 Fails**.

## Lizenz

MIT — siehe [LICENSE](LICENSE). Enthaltene Third-Party-Komponenten: civetweb (MIT),
nlohmann/json (MIT), dr_wav (public domain/MIT-0), minimp3 (CC0), libASPL (MIT, via CMake).
