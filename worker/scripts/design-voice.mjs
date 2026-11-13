// Design the Späti voice once with Gemini Voice design, then store the ID:
//   GEMINI_API_KEY=... node scripts/design-voice.mjs
//   npx wrangler secret put SPAETI_VOICE_ID   # paste the printed voice_... ID
// Writes spaeti-preview.wav so you can audition it first.
// Docs: https://ai.google.dev/gemini-api/docs/voice-design
import { writeFileSync } from "node:fs";

const key = process.env.GEMINI_API_KEY;
if (!key) throw new Error("Set GEMINI_API_KEY in your shell (never commit it).");

const res = await fetch("https://generativelanguage.googleapis.com/v1beta/voices", {
  method: "POST",
  headers: { "content-type": "application/json", "x-goog-api-key": key },
  body: JSON.stringify({
    store: true,
    voice: {
      model: process.env.GEMINI_TTS_MODEL ?? "gemini-3.8-flash-lite-tts",
      type: "prompted",
      display_name: "Spaeti (DevFest Berlin)",
      gender: "male",
      language_code: "en-US",
      prompted: {
        input: "A man in his fifties who runs a Berlin late-night corner shop. Low, gravelly voice " +
          "with a light German accent when speaking English. Deadpan, unhurried, mildly unimpressed, " +
          "but with a hint of warmth.",
      },
    },
  }),
});
const body = await res.json();
if (!res.ok) throw new Error(`Voice design failed (${res.status}): ${JSON.stringify(body.error ?? body)}`);
if (body.sample_audio?.data) writeFileSync("spaeti-preview.wav", Buffer.from(body.sample_audio.data, "base64"));
console.log(`Voice ID: ${body.id ?? body.name}`);
console.log("Preview: spaeti-preview.wav");
