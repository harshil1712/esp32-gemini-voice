# Firmware

PlatformIO project for the M5Stack CoreS3-Lite. Build and flash from the repo root:

```sh
python3 -m platformio run -e cores3-lite -t upload
python3 -m platformio device monitor
```

## Configuration

Copy `secrets.example.h` to `secrets.h` (gitignored) and fill in:

- `WIFI_SSID` and `WIFI_PASSWORD`: a 2.4 GHz network without a captive portal.
- `DEVICE_TOKEN`: the same value as the Worker's `DEVICE_TOKEN` secret.
- `SERVER_HOST`: the Worker's hostname only, with no `https://`, port or path.
- `SERVER_CA_CERT`: trusted root CAs for that hostname. The example has the roots that cover `*.workers.dev`.

The firmware validates TLS with `beginSslWithCA` and syncs the clock over NTP before it connects. It checks `GET /health` at boot.

## Behaviour

- Tap to start talking, tap again to stop. The device sends 1.2 s of silence so the speech model can finish the transcript.
- `KEEP_SESSION 1` keeps one session per boot, so the conversation history carries over between taps. Long-press for a new random session.
- The session name is `session-<16 hex>` from the hardware RNG.
- Playback uses a 64-slot queue (2.56 s, in PSRAM). If it overflows, the call ends instead of dropping audio.
- The device logs free internal heap at boot and when playback starts, prefixed with `[spaeti]`.
