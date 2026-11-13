# esp32-gemini-voice

Späti is a voice agent on an M5Stack CoreS3-Lite (ESP32-S3). He's a grumpy Berlin late-night-shop owner. This is the demo for the DevFest Berlin 2026 talk *Giving an ESP32 a Gemini Voice*.

The device records and plays audio. Everything else runs in the cloud:

```
CoreS3-Lite ──WSS──▶ Worker ──▶ Durable Object (one per conversation)
 mic, speaker,        token and     ├─ Nova-3 speech-to-text (Workers AI)
 screen               path check    ├─ Gemini LLM, in character as Späti
                                    ├─ Gemini TTS, streamed as 24 kHz audio/l16
                                    └─ SQLite: conversation history
```

| Folder | What's in it |
|---|---|
| [`src/`](src/README.md) | Device firmware (PlatformIO, Arduino, M5Unified) |
| [`worker/`](worker/README.md) | The Worker and the `Spaeti` Durable Object |
| `bench/` | Offline firmware that measures free memory and mic/speaker behaviour |

## Quick start

```sh
# 1. The Worker
cd worker
npm ci
npx wrangler secret put DEVICE_TOKEN       # any long random string
npx wrangler secret put GEMINI_API_KEY
npm test && npm run deploy

# 2. The device
cd ..
cp src/secrets.example.h src/secrets.h     # Wi-Fi, DEVICE_TOKEN and SERVER_HOST
python3 -m platformio run -e cores3-lite -t upload
```

Tap the screen to talk, tap again to stop. Long-press for a fresh session.

## The audio contract

| Direction | Format |
|---|---|
| Mic to cloud | PCM16 LE mono, 16 kHz, 40 ms frames |
| Cloud to speaker | PCM16 LE mono, 24 kHz, frames of up to 4 KB, sent at playback speed |

The Worker asks Gemini TTS for `audio/l16` and strips a WAV header if one arrives anyway. A 44-byte WAV header played as PCM is a loud click.

## Things that will bite you

- **Never run the mic and the speaker at the same time** on the CoreS3-Lite. With M5Unified the board reboots (`i2s_write: TX mode is not enabled`, then a stack overflow in `spk_task`). The firmware is strictly half-duplex, push to talk.
- **Keep API keys off the device.** Anyone with a USB cable can read the flash. The device only holds a per-device token that the Worker checks.
- **Persist state in the Durable Object.** The object can be evicted between taps, so anything kept only in memory is lost. The voice SDK writes the conversation to SQLite.
- **The ESP32 only joins 2.4 GHz networks** and can't get past a captive portal.

## Secrets

`src/secrets.h`, `.device-token` and `worker/.dev.vars` are gitignored. Don't commit them.

## License

MIT. See [LICENSE](LICENSE).
