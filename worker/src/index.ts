import { Agent, routeAgentRequest, type Connection } from "agents";
import { withVoice, WorkersAINova3STT, type VoiceTurnContext } from "@cloudflare/voice";
import { streamText } from "ai";
import { createGoogleGenerativeAI } from "@ai-sdk/google";
import { createWorkersAI } from "workers-ai-provider";
import { AuraTTS } from "./aura-tts";
import { GEMINI_SAMPLE_RATE, GeminiTTS } from "./gemini-tts";
import { spaeti, systemPrompt } from "./spaeti";

interface Env {
  AI: Ai;
  Spaeti: DurableObjectNamespace;
  DEVICE_TOKEN: string;
  GEMINI_API_KEY: string;
  /** Text model for the agent's replies, e.g. "gemini-3.5-flash-lite". */
  GEMINI_MODEL: string;
  /** Speech model, e.g. "gemini-3.8-flash-lite-tts". */
  GEMINI_TTS_MODEL: string;
  /** Optional designed voice (`voice_...`) from scripts/design-voice.mjs. */
  SPAETI_VOICE_ID?: string;
  /** Optional, e.g. an AI Gateway URL ending in /v1beta. */
  GEMINI_API_BASE?: string;
  /** Emergency fallbacks for stage day: "aura" / "workers-ai". */
  TTS_PROVIDER?: "gemini" | "aura";
  LLM_PROVIDER?: "gemini" | "workers-ai";
}

// Structured events for `wrangler tail`. Never log audio, transcripts, model
// output, credentials or provider error bodies.
function log(stage: string, fields: Record<string, string | number | boolean> = {}) {
  console.log({ app: "spaeti", stage, ...fields });
}

function logFailure(stage: string, error: unknown) {
  console.error({ app: "spaeti", stage,
    errorType: error instanceof Error ? error.name : "unknown",
    message: error instanceof Error ? error.message.slice(0, 120) : "" });
}

const VoiceBase = withVoice(Agent, {
  audioFormat: "pcm16",
  sampleRate: GEMINI_SAMPLE_RATE, // Gemini TTS: 24 kHz mono PCM16
  historyLimit: 8,
  maxMessageCount: 40,
});

export class Spaeti extends VoiceBase<Env> {
  // Streaming speech-to-text. The device sends 1.2 s of silence after the
  // tap, which lets Nova-3's endpointing finish the transcript.
  transcriber = new WorkersAINova3STT(this.env.AI, {
    language: "en", endpointingMs: 500, utteranceEndMs: 1200,
  });

  private readonly character = spaeti(this.env.SPAETI_VOICE_ID);

  tts = this.env.TTS_PROVIDER === "aura"
    ? new AuraTTS(this.env.AI, log)
    : new GeminiTTS({
        apiKey: this.env.GEMINI_API_KEY,
        model: this.env.GEMINI_TTS_MODEL,
        apiBase: this.env.GEMINI_API_BASE,
        getVoice: () => this.character,
        log,
      });

  // Name and colour for the device screen.
  private announceCharacter(connection: Connection) {
    // The firmware's protocol calls this message "persona".
    connection.send(JSON.stringify({
      type: "persona", id: "spaeti", name: this.character.screenName, accent: this.character.accent,
    }));
  }

  async onCallStart(connection: Connection) {
    this.announceCharacter(connection);
    log("call_started");
  }
  onCallEnd() { log("call_ended"); }

  afterTranscribe(transcript: string) {
    const trimmed = transcript.trim();
    log("stt_completed", { chars: trimmed.length, accepted: trimmed.length >= 3 });
    return trimmed.length < 3 ? null : trimmed;
  }

  private model() {
    // Fallback: Inkling 2 Small with thinking off (first text in ~0.5-0.7 s).
    // Llama 3.x on workers-ai-provider 4.0.x streams every token twice
    // ("MeMe,, I I'm'm"), which the device would speak and show twice.
    if (this.env.LLM_PROVIDER === "workers-ai")
      return createWorkersAI({ binding: this.env.AI })("@cf/thinkingmachines/inkling-2-small");
    const google = createGoogleGenerativeAI({
      apiKey: this.env.GEMINI_API_KEY,
      ...(this.env.GEMINI_API_BASE ? { baseURL: this.env.GEMINI_API_BASE } : {}),
    });
    return google(this.env.GEMINI_MODEL);
  }

  async onTurn(transcript: string, context: VoiceTurnContext) {
    log("turn_started", { inputChars: transcript.length, historyMessages: context.messages.length });
    const result = streamText({
      model: this.model(),
      system: systemPrompt(this.character),
      messages: [
        ...context.messages.map((m) => ({
          role: m.role as "user" | "assistant",
          content: m.content,
        })),
        { role: "user" as const, content: transcript },
      ],
      providerOptions: {
        google: { thinkingConfig: { thinkingLevel: "minimal" } },
        // Inkling still emits a short reasoning block; the voice SDK doesn't speak it.
        "workers-ai": { reasoning_effort: "none", chat_template_kwargs: { enable_thinking: false } },
      },
      abortSignal: context.signal,
      // Reasoning tokens count against this on Workers AI, so allow more there.
      maxOutputTokens: this.env.LLM_PROVIDER === "workers-ai" ? 300 : 120,
      onFinish: ({ finishReason, usage }) =>
        log("llm_completed", { finishReason, tokens: usage.totalTokens ?? 0 }),
      onError: ({ error }) => logFailure("llm_failed", error),
    });
    // Preserve finish/error events so the voice SDK can distinguish a failed
    // model stream from a completed (possibly partial) answer.
    return result.stream;
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (url.pathname === "/health" && request.method === "GET") {
      return Response.json({ ok: true, service: "spaeti" });
    }
    // Fail closed, including WebSocket upgrades. The session name isn't a
    // secret; the device picks it from its hardware RNG at boot.
    if (!env.DEVICE_TOKEN ||
        request.headers.get("Authorization") !== `Bearer ${env.DEVICE_TOKEN}`) {
      log("auth_rejected", { upgrade: request.headers.get("Upgrade") === "websocket" });
      return new Response("Unauthorized", { status: 401 });
    }
    if (!/^\/agents\/spaeti\/session-[a-f0-9]{16}$/.test(url.pathname)) {
      return new Response("Not found", { status: 404 });
    }
    return (await routeAgentRequest(request, env)) ?? new Response("Not found", { status: 404 });
  },
} satisfies ExportedHandler<Env>;
