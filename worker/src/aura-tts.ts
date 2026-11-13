// Fallback TTS (TTS_PROVIDER=aura): Deepgram Aura 2 on Workers AI.
//
// The device plays raw PCM only, so ask Aura for headerless 24 kHz linear16
// and refuse anything that looks like an encoded container.

type Log = (stage: string, fields?: Record<string, string | number | boolean>) => void;

// A real Workers AI binding returns binary data/streams; Wrangler's remote
// AI shim may return a Response. Normalize before passing bytes to the device.
export async function pcm16FromAI(result: unknown): Promise<ArrayBuffer> {
  let bytes: Uint8Array;
  if (result instanceof Response) {
    if (!result.ok) throw new Error(`TTS failed (${result.status})`);
    const contentType = result.headers.get("Content-Type") ?? "";
    if (/json|text\//i.test(contentType)) throw new Error(`TTS returned ${contentType}, not audio`);
    bytes = new Uint8Array(await result.arrayBuffer());
  } else if (result instanceof ReadableStream) {
    bytes = new Uint8Array(await new Response(result).arrayBuffer());
  } else if (result instanceof Uint8Array) {
    bytes = result;
  } else if (result instanceof ArrayBuffer) {
    bytes = new Uint8Array(result);
  } else if (result && typeof result === "object" && "audio" in result &&
             typeof result.audio === "string") {
    bytes = Uint8Array.from(atob(result.audio), (c) => c.charCodeAt(0));
  } else {
    throw new Error(`Unexpected TTS response type: ${Object.prototype.toString.call(result)}`);
  }
  if (bytes.byteLength === 0 || bytes.byteLength % 2 !== 0) {
    throw new Error("TTS returned empty or odd-length PCM");
  }
  // Reject common container/encoded signatures rather than playing them
  // as raw PCM. The first live request must still verify actual audio.
  const header = bytes.subarray(0, 4);
  const signature = String.fromCharCode(...header);
  if (signature === "RIFF" || signature === "OggS" || signature === "fLaC" ||
      signature.startsWith("ID3") ||
      (header[0] === 0xff && (header[1] & 0xe0) === 0xe0)) {
    throw new Error("Expected headerless PCM, got encoded audio");
  }
  // ArrayBufferView may refer to a slice of a larger allocation.
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
}

export class AuraTTS {
  // The voice SDK prepares sentences concurrently and drains them in order.
  // Start synthesizing the next sentence early, but deliver one at a time,
  // paced to the speaker's 48 KB/s so its bounded queue never overflows.
  private previousSentence: Promise<void> = Promise.resolve();

  private readonly ai: Ai;
  private readonly log: Log;

  constructor(ai: Ai, log: Log = () => {}) {
    this.ai = ai;
    this.log = log;
  }

  async synthesize(text: string, signal?: AbortSignal): Promise<ArrayBuffer | null> {
    const started = Date.now();
    this.log("tts_started", { chars: text.length, model: "aura-2" });
    const result: unknown = await this.ai.run(
      "@cf/deepgram/aura-2-en" as any,
      { text, speaker: "luna", encoding: "linear16", sample_rate: 24000, container: "none" } as any,
      { returnRawResponse: true, ...(signal ? { signal } : {}) },
    );
    const audio = await pcm16FromAI(result);
    this.log("tts_completed", { bytes: audio.byteLength, durationMs: Date.now() - started });
    return audio;
  }

  async *synthesizeStream(text: string, signal?: AbortSignal): AsyncGenerator<ArrayBuffer> {
    const previous = this.previousSentence;
    let release!: () => void;
    this.previousSentence = new Promise<void>((resolve) => { release = resolve; });
    // Handle a rejection right away, even while the previous sentence plays.
    const synthesis = this.synthesize(text, signal).then(
      (audio) => ({ ok: true as const, audio }),
      (error: unknown) => ({ ok: false as const, error }),
    );
    await previous;
    try {
      const outcome = await synthesis;
      if (!outcome.ok) throw outcome.error;
      if (!outcome.audio) return;
      const audio = outcome.audio;
      for (let offset = 0; offset < audio.byteLength; offset += 4096) {
        if (signal?.aborted) return;
        const chunk = audio.slice(offset, Math.min(offset + 4096, audio.byteLength));
        yield chunk;
        await new Promise<void>((r) => setTimeout(r, Math.ceil(chunk.byteLength / 48)));
      }
    } finally {
      release();
    }
  }
}
