import assert from "node:assert/strict";
import { test } from "node:test";
import { GeminiTTS, PCMFramer, SSEDecoder, stripWavHeader } from "../src/gemini-tts.ts";

const sse = (events) => events.map((e) =>
  `event: ${e.event_type}\ndata: ${JSON.stringify(e)}\n\n`).join("") + "event: done\ndata: [DONE]\n\n";
const audio = (bytes) => ({ event_type: "step.delta", index: 0,
  delta: { type: "audio", data: Buffer.from(bytes).toString("base64") } });

function streamOf(text, pieces = 3) {
  const enc = new TextEncoder().encode(text);
  const size = Math.ceil(enc.length / pieces);
  return new ReadableStream({
    start(c) { for (let i = 0; i < enc.length; i += size) c.enqueue(enc.slice(i, i + size)); c.close(); },
  });
}

test("SSE decoder handles split chunks, CRLF and multi-line data", () => {
  const d = new SSEDecoder();
  assert.deepEqual(d.push("event: step.delta\r\ndata: {\"a\""), []);
  assert.deepEqual(d.push(":1}\r\n\r\nevent: x\ndata: one\ndata: two\n\n"), [
    { event: "step.delta", data: "{\"a\":1}" },
    { event: "x", data: "one\ntwo" },
  ]);
});

test("PCM framer emits even frames <= 4096 bytes and carries odd bytes", () => {
  const f = new PCMFramer(4096);
  const frames = [...f.push(new Uint8Array(9001).fill(1)), ...f.push(new Uint8Array(1).fill(1)), ...f.flush()];
  assert.ok(frames.every((x) => x.byteLength <= 4096 && x.byteLength % 2 === 0));
  assert.equal(frames.reduce((n, x) => n + x.byteLength, 0), 9002);
});

test("WAV header is stripped (Gemini unary default is audio/wav)", () => {
  const pcm = new Uint8Array([1, 2, 3, 4]);
  const wav = new Uint8Array(44 + pcm.length);
  wav.set(new TextEncoder().encode("RIFF"), 0);
  wav.set(new TextEncoder().encode("WAVEfmt "), 8);
  new DataView(wav.buffer).setUint32(16, 16, true);
  wav.set(new TextEncoder().encode("data"), 36);
  new DataView(wav.buffer).setUint32(40, pcm.length, true);
  wav.set(pcm, 44);
  assert.deepEqual(stripWavHeader(wav), pcm);
  assert.deepEqual(stripWavHeader(pcm), pcm);
});

function mockFetch(bodies) {
  const calls = [];
  globalThis.fetch = async (url, init) => {
    const body = JSON.parse(init.body);
    calls.push({ url, body, headers: init.headers });
    const text = bodies[body.input[0].content[0].text];
    if (text === 500) return new Response("nope", { status: 500 });
    return new Response(streamOf(text), { status: 200, headers: { "content-type": "text/event-stream" } });
  };
  return calls;
}

const tts = () => new GeminiTTS({
  apiKey: "k", model: "gemini-3.8-flash-lite-tts", leadMs: 1e9,
  getVoice: () => ({ voice: "Algenib", style: "deadpan" }),
});

test("requests headerless l16 PCM with voice + style, and streams the bytes", async () => {
  const calls = mockFetch({ hi: sse([{ event_type: "step.start", index: 0 }, audio([1, 0, 2, 0, 3]), audio([0])]) });
  const out = [];
  for await (const c of tts().synthesizeStream("hi")) out.push(...new Uint8Array(c));
  assert.deepEqual(out, [1, 0, 2, 0, 3, 0]);
  const { url, body, headers } = calls[0];
  assert.match(url, /\/v1beta\/interactions$/);
  assert.equal(headers["x-goog-api-key"], "k");
  assert.deepEqual(body.response_format, { type: "audio", mime_type: "audio/l16", sample_rate: 24000 });
  assert.deepEqual(body.generation_config.speech_config, [{ voice: "Algenib" }]);
  assert.deepEqual(body.input[0].content[0].annotations, [{ type: "speech_metadata", style: "deadpan" }]);
  assert.equal(body.stream, true);
});

test("sentences are delivered in order even when synthesized concurrently", async () => {
  mockFetch({ a: sse([audio([1, 1])]), b: sse([audio([2, 2])]) });
  const t = tts();
  const ga = t.synthesizeStream("a");
  const gb = t.synthesizeStream("b");
  const order = [];
  const drain = async (g) => { for await (const c of g) order.push(new Uint8Array(c)[0]); };
  await Promise.all([drain(gb), drain(ga)]);
  assert.deepEqual(order, [1, 2]);
});

test("HTTP errors and stream error events reject", async () => {
  mockFetch({ bad: 500, err: sse([{ event_type: "error", error: { code: "gateway_timeout", message: "x" } }]) });
  await assert.rejects(async () => { for await (const _ of tts().synthesizeStream("bad")); }, /500/);
  await assert.rejects(async () => { for await (const _ of tts().synthesizeStream("err")); }, /stream error/);
});
