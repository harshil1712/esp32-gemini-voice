// Gemini TTS for a device that can only play raw PCM.
//
// Uses the Gemini API Interactions endpoint with `stream: true` and an
// explicit `audio/l16` response format. Unary Gemini 3.8 TTS requests default
// to WAV (44-byte RIFF header), which an ESP32 would play as a click. We ask
// for headerless 24 kHz mono PCM16 and strip a RIFF header defensively anyway.
// Docs: https://ai.google.dev/gemini-api/docs/speech-generation

export const GEMINI_SAMPLE_RATE = 24000;
const BYTES_PER_SECOND = GEMINI_SAMPLE_RATE * 2; // PCM16 mono
const CHUNK_BYTES = 4096; // ESP32 WebSocket frame budget

export interface VoiceSelection {
  /** Prebuilt voice name (e.g. "Algenib") or a designed `voice_...` ID. */
  voice: string;
  /** Short turn-level delivery hint for `speech_metadata.style`. */
  style: string;
}

export interface GeminiTTSOptions {
  apiKey: string;
  model: string;
  apiBase?: string;
  /** The voice and delivery style for each sentence. */
  getVoice: () => VoiceSelection;
  /** How far ahead of the device's playhead we may send audio. */
  leadMs?: number;
  log?: (stage: string, fields?: Record<string, string | number | boolean>) => void;
}

// ---------------------------------------------------------------------------
// Server-sent events
// ---------------------------------------------------------------------------

export interface SSEEvent {
  event: string;
  data: string;
}

/** Incremental SSE decoder: feed arbitrary text chunks, get complete events. */
export class SSEDecoder {
  private buffer = "";

  push(text: string): SSEEvent[] {
    this.buffer += text.replace(/\r\n?/g, "\n");
    const events: SSEEvent[] = [];
    let boundary: number;
    while ((boundary = this.buffer.indexOf("\n\n")) !== -1) {
      const block = this.buffer.slice(0, boundary);
      this.buffer = this.buffer.slice(boundary + 2);
      let event = "message";
      const data: string[] = [];
      for (const line of block.split("\n")) {
        if (!line || line.startsWith(":")) continue;
        const colon = line.indexOf(":");
        const field = colon === -1 ? line : line.slice(0, colon);
        let value = colon === -1 ? "" : line.slice(colon + 1);
        if (value.startsWith(" ")) value = value.slice(1);
        if (field === "event") event = value;
        else if (field === "data") data.push(value);
      }
      if (data.length) events.push({ event, data: data.join("\n") });
    }
    return events;
  }
}

// ---------------------------------------------------------------------------
// PCM framing
// ---------------------------------------------------------------------------

/** Re-frames arbitrary byte chunks into even-length frames of <= maxBytes. */
export class PCMFramer {
  private pending: Uint8Array = new Uint8Array(0);
  private headerChecked = false;
  private readonly maxBytes: number;

  constructor(maxBytes = CHUNK_BYTES) {
    this.maxBytes = maxBytes;
  }

  push(bytes: Uint8Array): Uint8Array[] {
    let data: Uint8Array = concat(this.pending, bytes);
    if (!this.headerChecked) {
      if (data.byteLength < 44) { this.pending = data; return []; }
      data = stripWavHeader(data);
      this.headerChecked = true;
    }
    const frames: Uint8Array[] = [];
    let offset = 0;
    while (data.byteLength - offset >= this.maxBytes) {
      frames.push(data.slice(offset, offset + this.maxBytes));
      offset += this.maxBytes;
    }
    // Emit what we have (even length) so playback starts promptly; keep an
    // odd trailing byte for the next chunk.
    const evenEnd = offset + ((data.byteLength - offset) & ~1);
    if (evenEnd > offset) frames.push(data.slice(offset, evenEnd));
    this.pending = data.slice(evenEnd);
    return frames;
  }

  /** Remaining bytes at end of stream (header-less tiny clips included). */
  flush(): Uint8Array[] {
    let data: Uint8Array = this.pending;
    this.pending = new Uint8Array(0);
    if (!this.headerChecked) { data = stripWavHeader(data); this.headerChecked = true; }
    const even = data.byteLength & ~1;
    return even ? [data.slice(0, even)] : [];
  }
}

/** If `bytes` starts with a RIFF/WAVE header, return only the `data` payload. */
export function stripWavHeader(bytes: Uint8Array): Uint8Array {
  if (bytes.byteLength < 12 || ascii(bytes, 0, 4) !== "RIFF" || ascii(bytes, 8, 4) !== "WAVE")
    return bytes;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let offset = 12;
  while (offset + 8 <= bytes.byteLength) {
    const id = ascii(bytes, offset, 4);
    const size = view.getUint32(offset + 4, true);
    if (id === "data") return bytes.slice(offset + 8);
    offset += 8 + size + (size & 1);
  }
  return new Uint8Array(0);
}

function ascii(bytes: Uint8Array, start: number, length: number) {
  return String.fromCharCode(...bytes.subarray(start, start + length));
}

function concat(a: Uint8Array, b: Uint8Array) {
  if (!a.byteLength) return b;
  const out = new Uint8Array(a.byteLength + b.byteLength);
  out.set(a, 0);
  out.set(b, a.byteLength);
  return out;
}

function base64ToBytes(b64: string) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; ++i) out[i] = bin.charCodeAt(i);
  return out;
}

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

// ---------------------------------------------------------------------------
// TTS provider
// ---------------------------------------------------------------------------

export class GeminiTTS {
  private previousSentence: Promise<void> = Promise.resolve();
  /** Wall-clock time (ms) at which the device finishes what we've sent. */
  private playheadAt = 0;
  private readonly options: GeminiTTSOptions;

  constructor(options: GeminiTTSOptions) {
    this.options = options;
  }

  private request(text: string, signal?: AbortSignal) {
    const { voice, style } = this.options.getVoice();
    const base = this.options.apiBase ?? "https://generativelanguage.googleapis.com/v1beta";
    return fetch(`${base}/interactions`, {
      method: "POST",
      signal,
      headers: {
        "content-type": "application/json",
        "x-goog-api-key": this.options.apiKey,
      },
      body: JSON.stringify({
        model: this.options.model,
        input: [{
          type: "user_input",
          content: [{
            type: "text",
            text,
            ...(style ? { annotations: [{ type: "speech_metadata", style }] } : {}),
          }],
        }],
        response_format: { type: "audio", mime_type: "audio/l16", sample_rate: GEMINI_SAMPLE_RATE },
        generation_config: { speech_config: [{ voice }] },
        stream: true,
      }),
    });
  }

  /** Streams PCM frames as Gemini produces them. */
  private async *stream(text: string, signal?: AbortSignal): AsyncGenerator<Uint8Array> {
    const log = this.options.log ?? (() => {});
    const started = Date.now();
    log("tts_started", { chars: text.length, model: this.options.model });
    const response = await this.request(text, signal);
    if (!response.ok || !response.body) {
      log("tts_failed", { status: response.status });
      throw new Error(`Gemini TTS failed (${response.status})`);
    }
    const decoder = new TextDecoder();
    const sse = new SSEDecoder();
    const framer = new PCMFramer();
    let bytes = 0;
    let firstAudioMs = -1;
    const reader = response.body.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (const event of sse.push(decoder.decode(value, { stream: true }))) {
          if (event.data === "[DONE]") continue;
          let payload: any;
          try { payload = JSON.parse(event.data); } catch { continue; }
          const type = payload?.event_type ?? event.event;
          if (type === "error") {
            log("tts_failed", { code: String(payload?.error?.code ?? "unknown").slice(0, 48) });
            throw new Error("Gemini TTS stream error");
          }
          // Unknown event and delta types are skipped, per the API docs.
          if (type !== "step.delta" || payload?.delta?.type !== "audio" || !payload.delta.data)
            continue;
          if (firstAudioMs < 0) firstAudioMs = Date.now() - started;
          for (const frame of framer.push(base64ToBytes(payload.delta.data))) {
            bytes += frame.byteLength;
            yield frame;
          }
        }
      }
      for (const frame of framer.flush()) {
        bytes += frame.byteLength;
        yield frame;
      }
    } finally {
      reader.releaseLock();
    }
    log("tts_completed", { bytes, firstAudioMs, durationMs: Date.now() - started });
  }

  async synthesize(text: string, signal?: AbortSignal): Promise<ArrayBuffer | null> {
    const frames: Uint8Array[] = [];
    for await (const frame of this.stream(text, signal)) frames.push(frame);
    const total = frames.reduce((n, f) => n + f.byteLength, 0);
    if (!total) return null;
    const out = new Uint8Array(total);
    let offset = 0;
    for (const f of frames) { out.set(f, offset); offset += f.byteLength; }
    return out.buffer;
  }

  /**
   * The voice SDK prepares sentences concurrently and drains them in order.
   * Start Gemini immediately (buffering frames), but deliver each sentence
   * only after the previous one, paced against a shared device playhead so
   * the ESP32's bounded 2.56 s speaker queue never overflows.
   */
  synthesizeStream(text: string, signal?: AbortSignal): AsyncGenerator<ArrayBuffer> {
    // Ordering and the Gemini request are set up *synchronously* at call time,
    // so the sentence order is the call order, not the order of first next().
    const previous = this.previousSentence;
    let release!: () => void;
    this.previousSentence = new Promise<void>((resolve) => { release = resolve; });
    // An interrupted turn may never iterate this generator; don't let it
    // block every later sentence.
    signal?.addEventListener("abort", () => release(), { once: true });

    const queue: Uint8Array[] = [];
    let finished = false;
    let failure: unknown = null;
    let wake: (() => void) | null = null;
    const notify = () => { wake?.(); wake = null; };
    const producer = (async () => {
      try { for await (const frame of this.stream(text, signal)) { queue.push(frame); notify(); } }
      catch (error) { failure = error; }
      finally { finished = true; notify(); }
    })();

    const self = this;
    return (async function* () {
      try {
        await previous;
        const leadMs = self.options.leadMs ?? 1000;
        for (;;) {
          if (signal?.aborted) return;
          const frame = queue.shift();
          if (!frame) {
            if (finished) break;
            await new Promise<void>((r) => { wake = r; });
            continue;
          }
          const ahead = self.playheadAt - Date.now();
          if (ahead > leadMs) await sleep(ahead - leadMs);
          self.playheadAt = Math.max(self.playheadAt, Date.now()) +
            (frame.byteLength / BYTES_PER_SECOND) * 1000;
          yield frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.byteLength) as ArrayBuffer;
        }
        await producer;
        if (failure) throw failure;
      } finally {
        release();
      }
    })();
  }
}
