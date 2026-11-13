// Stage-day smoke test for Gemini TTS, independent of the device and Worker:
//   GEMINI_API_KEY=... node scripts/tts-smoke.mjs ["Text to say"] [voice]
// Prints time-to-first-audio and writes tts-smoke.wav (header added locally,
// the API returns headerless 24 kHz PCM16 because we ask for audio/l16).
import { writeFileSync } from "node:fs";

const key = process.env.GEMINI_API_KEY;
if (!key) throw new Error("Set GEMINI_API_KEY in your shell (never commit it).");
const text = process.argv[2] ?? "Ja, ja. Another demo. <sigh> Fine, I'm listening.";
const voice = process.argv[3] ?? process.env.SPAETI_VOICE_ID ?? "Algenib";
const model = process.env.GEMINI_TTS_MODEL ?? "gemini-3.8-flash-lite-tts";

const started = performance.now();
const res = await fetch("https://generativelanguage.googleapis.com/v1beta/interactions", {
  method: "POST",
  headers: { "content-type": "application/json", "x-goog-api-key": key },
  body: JSON.stringify({
    model,
    input: [{ type: "user_input", content: [{ type: "text", text,
      annotations: [{ type: "speech_metadata", style: "deadpan and unimpressed" }] }] }],
    response_format: { type: "audio", mime_type: "audio/l16", sample_rate: 24000 },
    generation_config: { speech_config: [{ voice }] },
    stream: true,
  }),
});
if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`);

const chunks = [];
let firstAudio = 0, buffer = "";
const decoder = new TextDecoder();
for await (const part of res.body) {
  buffer += decoder.decode(part, { stream: true }).replace(/\r\n/g, "\n");
  let i;
  while ((i = buffer.indexOf("\n\n")) !== -1) {
    const block = buffer.slice(0, i); buffer = buffer.slice(i + 2);
    const data = block.split("\n").filter((l) => l.startsWith("data:")).map((l) => l.slice(5).trim()).join("\n");
    if (!data || data === "[DONE]") continue;
    const ev = JSON.parse(data);
    if (ev.event_type === "error") throw new Error(JSON.stringify(ev.error));
    if (ev.event_type === "step.delta" && ev.delta?.type === "audio") {
      if (!firstAudio) firstAudio = performance.now() - started;
      chunks.push(Buffer.from(ev.delta.data, "base64"));
    }
  }
}
const pcm = Buffer.concat(chunks);
const header = Buffer.alloc(44);
header.write("RIFF", 0); header.writeUInt32LE(36 + pcm.length, 4); header.write("WAVEfmt ", 8);
header.writeUInt32LE(16, 16); header.writeUInt16LE(1, 20); header.writeUInt16LE(1, 22);
header.writeUInt32LE(24000, 24); header.writeUInt32LE(48000, 28); header.writeUInt16LE(2, 32);
header.writeUInt16LE(16, 34); header.write("data", 36); header.writeUInt32LE(pcm.length, 40);
writeFileSync("tts-smoke.wav", Buffer.concat([header, pcm]));
console.log(`model=${model} voice=${voice}`);
console.log(`first audio: ${Math.round(firstAudio)} ms, total: ${Math.round(performance.now() - started)} ms`);
console.log(`audio: ${pcm.length} bytes = ${(pcm.length / 48000).toFixed(2)} s (starts with RIFF? ${pcm.subarray(0, 4).toString() === "RIFF"})`);
