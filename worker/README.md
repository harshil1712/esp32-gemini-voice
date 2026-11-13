# Worker

A Cloudflare Worker plus one `Spaeti` Durable Object per conversation, built on `@cloudflare/voice` and the Agents SDK.

- `src/index.ts`: the Worker (token and path check) and the `Spaeti` Durable Object.
- `src/gemini-tts.ts`: streaming Gemini TTS over the Interactions API, framed to 4 KB and paced to playback speed.
- `src/spaeti.ts`: Späti's character prompt, Gemini voice and delivery style.
- `src/aura-tts.ts`: the fallback TTS (Aura 2 on Workers AI, raw PCM).

## Deploy

```sh
npm ci
npx wrangler secret put DEVICE_TOKEN
npx wrangler secret put GEMINI_API_KEY
npx wrangler secret put SPAETI_VOICE_ID    # optional, from scripts/design-voice.mjs
npm test && npm run deploy
```

If your login has several accounts, set `CLOUDFLARE_ACCOUNT_ID` first.

## Routes

- `GET /health` is public.
- `/agents/spaeti/session-<16 hex>` needs `Authorization: Bearer <DEVICE_TOKEN>`. Everything else returns 404.

## Scripts

```sh
GEMINI_API_KEY=... node scripts/tts-smoke.mjs      # time to first audio, writes tts-smoke.wav
GEMINI_API_KEY=... node scripts/design-voice.mjs   # designs Späti's voice, prints a voice_… ID
```

## Fallbacks

Set these in `wrangler.jsonc` if Gemini is unavailable:

- `TTS_PROVIDER=aura` uses Workers AI Aura 2 for speech.
- `LLM_PROVIDER=workers-ai` uses Inkling 2 Small on Workers AI, with thinking off. (Llama 3.x streams every token twice through workers-ai-provider 4.0.x.)
