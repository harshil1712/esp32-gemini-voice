import assert from "node:assert/strict";
import { test } from "node:test";
import { pcm16FromAI } from "../src/aura-tts.ts";

const pcm = new Uint8Array([0, 0, 3, 0, 0xfd, 0xff]);

for (const [kind, value] of [
  ["ArrayBuffer", pcm.buffer],
  ["Uint8Array view", new Uint8Array(new Uint8Array([1, ...pcm, 2]).buffer, 1, pcm.length)],
  ["Response", new Response(pcm)],
  ["ReadableStream", new ReadableStream({ start(controller) { controller.enqueue(pcm); controller.close(); } })],
  ["base64 object", { audio: Buffer.from(pcm).toString("base64") }],
]) {
  test(`normalizes ${kind} to identical PCM`, async () => {
    assert.deepEqual(new Uint8Array(await pcm16FromAI(value)), pcm);
  });
}

test("rejects error and JSON Responses", async () => {
  await assert.rejects(() => pcm16FromAI(new Response("busy", { status: 429 })), /429/);
  await assert.rejects(() => pcm16FromAI(Response.json({ audio: "AAADAA==" })), /not audio/);
});

test("rejects encoded audio, empty audio, and odd bytes", async () => {
  for (const data of [Buffer.from("RIFFdata"), Buffer.from("ID3test!"),
                      Buffer.from("OggSdata"), new Uint8Array(), new Uint8Array([1])]) {
    await assert.rejects(() => pcm16FromAI(data));
  }
});
