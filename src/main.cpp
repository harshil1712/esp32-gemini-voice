#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <atomic>
#include <time.h>

// Copy src/secrets.example.h to src/secrets.h (already gitignored).
// Missing configuration still builds and shows an actionable screen.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define DEVICE_TOKEN ""
#define SERVER_HOST ""
#define SERVER_CA_CERT ""
#endif

// DevFest talk mode: keep ONE agent session per boot so the conversation
// history survives between taps. Long-press for a fresh session.
// Set to 0 for a new random session (empty history) on every tap.
#ifndef KEEP_SESSION
#define KEEP_SESSION 1
#endif
#ifndef SPEAKER_VOLUME
#define SPEAKER_VOLUME 220 // 0-255
#endif

namespace {
constexpr uint32_t kInputRate = 16000;
constexpr uint32_t kOutputRate = 24000;
constexpr size_t kMicSamples = 640;       // 40 ms, PCM16LE mono
constexpr size_t kPlaySamples = 960;      // 40 ms of output per speaker request
constexpr size_t kPlaySlots = 64;         // Bounded 2.56 s jitter/queue capacity (PSRAM)
constexpr uint32_t kWifiRetryMs = 7000;
constexpr uint32_t kReplyTimeoutMs = 25000;
constexpr uint32_t kSilenceMs = 1200;    // Flush model-driven STT on tap-to-stop
constexpr uint32_t kPlaybackQuietMs = 250;
constexpr uint8_t kFree = 0, kReady = 1, kPlaying = 2;

WebSocketsClient socketClient;
int16_t* micBuffers[2] = {nullptr, nullptr};
std::atomic<bool> micReady[2];
struct PlaySlot {
  int16_t* data = nullptr;
  size_t count = 0;
  std::atomic<uint8_t> state{kFree};
};
PlaySlot playSlots[kPlaySlots];
size_t playWrite = 0, playRead = 0, micNext = 0;
uint8_t pendingByte = 0;
bool hasPendingByte = false;

enum class Phase : uint8_t { Idle, Connecting, Recording, Waiting, Playback };
Phase phase = Phase::Idle;
bool configured = false, wifiStarted = false, clockStarted = false;
bool wifiLogged = false, healthChecked = false;
uint32_t lastHealthAttempt = 0;
bool connected = false, serverListening = false;
bool outputIsPCM = false, receivedAudio = false, binaryFragment = false;
uint32_t lastWifiAttempt = 0, connectSince = 0, waitSince = 0, silenceUntil = 0;
uint32_t nextSilence = 0, lastAudioAt = 0, lastDraw = 0;
bool captionDirty = false;
String endpoint;
String caption;
String message = "Starting...";
String serverStatus;
String sessionSuffix;            // Reused across taps when KEEP_SESSION
String personaName = "SPAETI";   // Updated by {"type":"persona"} from the agent
uint16_t accent = TFT_CYAN;

bool readyForTLS() { return time(nullptr) > 1700000000; }

void drawFace() {
  auto& d = M5.Display;
  d.fillScreen(0x0841);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_WHITE, 0x0841);
  d.setTextSize(2);
  d.setTextColor(accent, 0x0841);
  d.drawString(personaName, d.width() / 2, 25);
  const int cx = d.width() / 2;
  const int cy = 91;
  d.fillCircle(cx - 38, cy - 12, 12, accent);
  d.fillCircle(cx + 38, cy - 12, 12, accent);
  if (phase == Phase::Recording) d.fillRoundRect(cx - 35, cy + 18, 70, 10, 5, accent);
  else d.drawArc(cx, cy + 14, 40, 32, 20, 160, accent);
  d.setTextSize(1);
  d.setTextColor(TFT_YELLOW, 0x0841);
  d.drawString(message, cx, 145);
  // M5GFX supports wrapping with print(), unlike drawString(). Keep the
  // caption bounded so it cannot overrun the display or exhaust RAM.
  d.setTextDatum(top_left);
  d.setTextColor(TFT_WHITE, 0x0841);
  d.setTextWrap(true);
  d.setCursor(12, 164);
  d.print(caption);
  captionDirty = false;
  lastDraw = millis();
}

void setMessage(const String& text) {
  if (message != text) {
    message = text;
    Serial.printf("[spaeti] %s\n", message.c_str()); // Status only; never captions/audio/secrets.
    drawFace();
  }
}

void setCaption(const char* text, bool append = false) {
  if (!text) return;
  if (!append) caption = "";
  caption += text;
  // Keep the latest part of a long answer. A truncated UTF-8 lead byte is
  // skipped so it cannot corrupt the rendering of the remaining caption.
  if (caption.length() > 190) {
    size_t start = caption.length() - 190;
    while (start < caption.length() &&
           (static_cast<uint8_t>(caption[start]) & 0xc0) == 0x80) ++start;
    caption.remove(0, start);
  }
  captionDirty = true;
}

void onMicRelease(void*, void* data, size_t length) {
  for (size_t i = 0; i < 2; ++i) {
    if (data == micBuffers[i] && length == kMicSamples)
      micReady[i].store(true, std::memory_order_release);
  }
}

void onSpeakerRelease(void*, const void* data, uint8_t) {
  for (auto& slot : playSlots) {
    if (data == slot.data) {
      slot.count = 0; // Reset before publishing FREE to the network producer.
      slot.state.store(kFree, std::memory_order_release);
      break;
    }
  }
}

// The CoreS3-Lite mic and speaker share I2S port 1, and M5Unified's
// Speaker.end() uninstalls that driver even when the speaker isn't running.
// Calling it while the mic task is reading crashes the board (LoadProhibited in
// i2s_read from mic_task). Always stop the mic first, and only end a running speaker.
void stopSpeakerIfRunning() {
  if (M5.Speaker.isRunning()) M5.Speaker.end();
}

void resetAudio() {
  // Both end() calls wait for their worker tasks and callbacks to finish.
  M5.Mic.end();
  stopSpeakerIfRunning();
  for (auto& slot : playSlots) {
    slot.state.store(kFree, std::memory_order_release);
    slot.count = 0;
  }
  for (auto& flag : micReady) flag.store(false, std::memory_order_release);
  playWrite = playRead = micNext = 0;
  hasPendingByte = false;
  receivedAudio = false;
  binaryFragment = false;
}

void endCall(const String& reason) {
  resetAudio();
  if (connected) socketClient.sendTXT("{\"type\":\"end_call\"}");
  // Do not run socketClient.loop() again until a new random session is made.
  phase = Phase::Idle;
  connected = serverListening = false;
  socketClient.disconnect();
  setMessage(reason);
}

bool startMic() {
  // The CoreS3-Lite speaker and mic share the I2S/codec bus. Stop the idle mic
  // that was restored after playback, then the speaker, then start recording.
  M5.Mic.end();
  stopSpeakerIfRunning();
  for (auto& flag : micReady) flag.store(false, std::memory_order_release);
  micNext = 0;
  if (!M5.Mic.begin() || !M5.Mic.record(micBuffers[0], kMicSamples, kInputRate) ||
      !M5.Mic.record(micBuffers[1], kMicSamples, kInputRate)) {
    M5.Mic.end();
    return false;
  }
  phase = Phase::Recording;
  setMessage("Listening - tap to stop");
  return true;
}

// A bounded queue: playRaw keeps a pointer until its release callback fires.
// Never reuse a queued slot merely because playRaw() has returned.
bool enqueueAudio(const uint8_t* data, size_t length) {
  if (!outputIsPCM) return false;
  if (phase == Phase::Recording) {
    M5.Mic.end();
    phase = Phase::Playback;
  }
  if (phase == Phase::Waiting) phase = Phase::Playback;
  if (!M5.Speaker.isRunning()) {
    M5.Mic.end();
    if (!M5.Speaker.begin()) return false;
    M5.Speaker.setVolume(SPEAKER_VOLUME);
    // The number for the talk: headroom with WiFi + TLS + WebSocket + I2S live.
    Serial.printf("[spaeti] playback start, free internal heap: %u bytes (min ever %u)\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
  }
  receivedAudio = true;
  lastAudioAt = millis();
  for (size_t pos = 0; pos < length; ++pos) {
    if (!hasPendingByte) {
      pendingByte = data[pos];
      hasPendingByte = true;
      continue;
    }
    auto& slot = playSlots[playWrite];
    if (slot.state.load(std::memory_order_acquire) != kFree) return false;
    // Explicit little-endian conversion (the ESP32-S3 itself is little-endian).
    slot.data[slot.count++] = static_cast<int16_t>(static_cast<uint16_t>(pendingByte) |
                                                   (static_cast<uint16_t>(data[pos]) << 8));
    hasPendingByte = false;
    if (slot.count == kPlaySamples) {
      slot.state.store(kReady, std::memory_order_release);
      playWrite = (playWrite + 1) % kPlaySlots;
    }
  }
  // A WebSocket message often ends mid-slot; play it promptly. An odd byte
  // may be completed by the next binary/fragment frame.
  auto& slot = playSlots[playWrite];
  if (slot.count) {
    slot.state.store(kReady, std::memory_order_release);
    playWrite = (playWrite + 1) % kPlaySlots;
  }
  setMessage("Speaking...");
  return true;
}

void playQueued() {
  if (phase != Phase::Playback) return;
  while (M5.Speaker.isPlaying(0) < 2) {
    auto& slot = playSlots[playRead];
    if (slot.state.load(std::memory_order_acquire) != kReady) break;
    // Publish ownership before queuing: the speaker task may release a very
    // short block before playRaw() returns.
    slot.state.store(kPlaying, std::memory_order_release);
    if (!M5.Speaker.playRaw(slot.data, slot.count, kOutputRate, false, 1, 0)) {
      endCall("Speaker error - tap to retry");
      return;
    }
    playRead = (playRead + 1) % kPlaySlots;
  }
}

bool playbackDrained() {
  if (hasPendingByte || M5.Speaker.isPlaying()) return false;
  for (auto& slot : playSlots)
    if (slot.state.load(std::memory_order_acquire) == kReady) return false;
  return true;
}

void onSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (phase == Phase::Idle) return;
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[spaeti] websocket connected");
      connected = true;
      serverListening = false;
      setMessage("Starting voice call...");
      socketClient.sendTXT("{\"type\":\"hello\",\"protocol_version\":1}");
      socketClient.sendTXT("{\"type\":\"start_call\"}");
      break;
    case WStype_DISCONNECTED:
    case WStype_ERROR:
      Serial.printf("[spaeti] websocket %s (phase=%u)\n",
                    type == WStype_ERROR ? "error" : "disconnected",
                    static_cast<unsigned>(phase));
      if (phase == Phase::Connecting && !connected) {
        // Retry this call's initial upgrade, never a previous call's.
        setMessage("WSS unavailable - retrying...");
      } else {
        // A dropped active call cannot be resumed safely; the next tap MUST
        // generate a new session ID, so the next call starts with an empty history.
        endCall("Disconnected - tap to retry");
      }
      break;
    case WStype_TEXT: {
      if (length > 2048) break;
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) break;
      const char* name = doc["type"] | "";
      if (!strcmp(name, "audio_config")) {
        const char* format = doc["format"] | "";
        const int rate = doc["sampleRate"] | 0;
        outputIsPCM = !strcmp(format, "pcm16") && rate == kOutputRate;
        Serial.printf("[spaeti] audio config compatible=%s rate=%d\n",
                      outputIsPCM ? "yes" : "no", rate);
        if (!outputIsPCM) endCall("Need 24kHz PCM16 output");
      } else if (!strcmp(name, "status")) {
        serverStatus = doc["status"] | "";
        if (serverStatus == "listening") {
          serverListening = true;
          if (phase == Phase::Connecting) setMessage("Ready to listen...");
        } else if (serverStatus == "thinking" || serverStatus == "speaking") {
          if (phase != Phase::Playback) setMessage("Thinking...");
        }
      } else if (!strcmp(name, "persona")) {
        // Sent by the agent on call start: the name and colour for the screen.
        const char* persona = doc["name"] | "";
        if (*persona) personaName = String(persona).substring(0, 16);
        accent = static_cast<uint16_t>(doc["accent"] | static_cast<int>(TFT_CYAN));
        Serial.printf("[spaeti] persona=%s\n", personaName.c_str());
        drawFace();
      } else if (!strcmp(name, "transcript_start")) {
        const char* role = doc["role"] | "";
        if (!strcmp(role, "assistant")) {
          setCaption("");
        }
      } else if (!strcmp(name, "transcript_delta")) {
        setCaption(doc["text"] | "", true);
      } else if (!strcmp(name, "transcript_end")) {
        setCaption(doc["text"] | "");
      } else if (!strcmp(name, "transcript")) {
        const char* role = doc["role"] | "";
        if (!strcmp(role, "assistant")) setCaption(doc["text"] | "");
      } else if (!strcmp(name, "error")) {
        // The free-form message could contain private model context. Log only
        // bounded protocol codes, not transcripts or raw server error text.
        Serial.printf("[spaeti] voice error code=%.32s stage=%.32s\n",
                      doc["code"] | "unknown", doc["stage"] | "unknown");
        endCall("Voice error - tap to retry");
      }
      break;
    }
    case WStype_BIN:
      if (length && !enqueueAudio(payload, length)) endCall("Audio overflow/format error");
      break;
    case WStype_FRAGMENT_BIN_START:
      binaryFragment = true;
      if (length && !enqueueAudio(payload, length)) endCall("Audio overflow/format error");
      break;
    case WStype_FRAGMENT_TEXT_START:
      binaryFragment = false; // Ignore oversized fragmented JSON, never play it as audio.
      break;
    case WStype_FRAGMENT:
      if (binaryFragment && length && !enqueueAudio(payload, length))
        endCall("Audio overflow/format error");
      break;
    case WStype_FRAGMENT_FIN:
      if (binaryFragment && length && !enqueueAudio(payload, length))
        endCall("Audio overflow/format error");
      binaryFragment = false;
      break;
    default:
      break; // Welcome and unrelated Agents protocol messages are harmless.
  }
}

void startCall() {
  if (!configured || WiFi.status() != WL_CONNECTED || !readyForTLS()) return;
  // The previous call's buffers and connection were drained by endCall.
  // Leave the restored idle mic running until startMic begins the new call.
  caption = "";
  outputIsPCM = false;
  serverStatus = "";
  connected = serverListening = false;
  if (!KEEP_SESSION || sessionSuffix.isEmpty()) {
    // esp_fill_random is hardware RNG backed by the ESP32 radio entropy source.
    uint8_t id[8];
    esp_fill_random(id, sizeof(id));
    char suffix[17];
    for (size_t i = 0; i < sizeof(id); ++i)
      snprintf(suffix + 2 * i, sizeof(suffix) - 2 * i, "%02x", id[i]);
    sessionSuffix = suffix;
  }
  endpoint = "/agents/spaeti/session-";
  endpoint += sessionSuffix;
  Serial.printf("[spaeti] call: %s\n", endpoint.c_str()); // Never print the token.
  phase = Phase::Connecting;
  connectSince = millis();
  socketClient.onEvent(onSocketEvent);
  socketClient.beginSslWithCA(SERVER_HOST, 443, endpoint.c_str(), SERVER_CA_CERT, "");
  // beginSslWithCA resets the client's auth fields; set the Bearer header AFTER
  // begin(), and before the first socketClient.loop() sends the HTTP upgrade.
  String authorization = String("Bearer ") + DEVICE_TOKEN;
  socketClient.setAuthorization(authorization.c_str()); // Library copies into String.
  socketClient.setReconnectInterval(3000);
  socketClient.enableHeartbeat(15000, 4000, 2);
  setMessage("Connecting securely...");
}

void stopRecording() {
  M5.Mic.end(); // Any completed mic buffers are still owned by us.
  for (size_t i = 0; i < 2; ++i) {
    size_t index = (micNext + i) % 2;
    if (micReady[index].exchange(false, std::memory_order_acq_rel) && connected)
      socketClient.sendBIN(reinterpret_cast<const uint8_t*>(micBuffers[index]),
                           kMicSamples * sizeof(int16_t));
  }
  phase = Phase::Waiting;
  waitSince = millis();
  silenceUntil = waitSince + kSilenceMs;
  nextSilence = waitSince;
  setMessage("Processing - please wait...");
}

void runNetwork() {
  if (!configured) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLogged) Serial.println("[spaeti] Wi-Fi disconnected");
    wifiLogged = healthChecked = false;
    if (phase != Phase::Idle) endCall("Wi-Fi lost - tap to retry");
    clockStarted = false;
    if (!wifiStarted || millis() - lastWifiAttempt >= kWifiRetryMs) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      wifiStarted = true;
      lastWifiAttempt = millis();
      Serial.printf("[spaeti] Wi-Fi connecting (status=%d)\n", WiFi.status());
    }
    setMessage("Disconnected - connecting Wi-Fi");
    return;
  }
  if (!wifiLogged) {
    wifiLogged = true;
    Serial.printf("[spaeti] Wi-Fi connected channel=%d RSSI=%d dBm\n",
                  WiFi.channel(), WiFi.RSSI());
  }
  if (!clockStarted) {
    configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
    clockStarted = true;
  }
  if (!readyForTLS()) {
    setMessage("Syncing clock for secure WSS...");
    return;
  }
  // Exercise DNS, validated TLS and the public route on this actual network
  // before a call starts recording. An HTTP failure cannot silently turn
  // into a successful-looking voice session; retry without disabling TLS.
  if (!healthChecked) {
    if (lastHealthAttempt && millis() - lastHealthAttempt < 8000) return;
    lastHealthAttempt = millis();
    setMessage("Checking secure Worker...");
    WiFiClientSecure client;
    client.setCACert(SERVER_CA_CERT);
    client.setTimeout(10);
    HTTPClient http;
    http.setTimeout(10000);
    const String url = String("https://") + SERVER_HOST + "/health";
    int status = http.begin(client, url) ? http.GET() : -1;
    Serial.printf("[spaeti] validated HTTPS health status=%d\n", status);
    if (status < 0) {
      char detail[96] = {};
      client.lastError(detail, sizeof(detail));
      Serial.printf("[spaeti] TLS/network detail=%.95s\n", detail);
    }
    http.end();
    if (status != 200) {
      setMessage("Worker HTTPS failed - retrying");
      return;
    }
    healthChecked = true;
  }
  if (phase == Phase::Idle) setMessage("Ready - tap to talk");
  else socketClient.loop();
}

} // namespace

void setup() {
  M5.begin(M5.config());
  Serial.begin(115200);
  Serial.printf("CoreS3-Lite, PSRAM: %u bytes\n", static_cast<unsigned>(ESP.getPsramSize()));
  // Talk number: internal (non-PSRAM) heap is what WiFi, TLS and I2S DMA compete for.
  Serial.printf("[spaeti] free internal heap: %u bytes (largest block %u)\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  configured = WIFI_SSID[0] && DEVICE_TOKEN[0] && SERVER_HOST[0] &&
               strstr(SERVER_CA_CERT, "-----BEGIN CERTIFICATE-----") &&
               !strstr(SERVER_HOST, "://") && !strchr(SERVER_HOST, '/') &&
               !strchr(SERVER_HOST, ':');
  for (auto& slot : playSlots) {
    slot.data = static_cast<int16_t*>(heap_caps_malloc(kPlaySamples * sizeof(int16_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slot.data) configured = false;
  }
  for (auto& ptr : micBuffers) {
    ptr = static_cast<int16_t*>(heap_caps_malloc(kMicSamples * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ptr) configured = false;
  }
  M5.Mic.setBufferReleaseCallback(nullptr, onMicRelease);
  M5.Speaker.setBufferReleaseCallback(nullptr, onSpeakerRelease);
  WiFi.mode(WIFI_STA);
  if (configured) {
    // The ESP32-S3 sees only 2.4 GHz APs; check this SSID on-device instead
    // of assuming the laptop's 5 GHz connection can be used.
    const int networks = WiFi.scanNetworks(false, true);
    int matches = 0;
    for (int i = 0; i < networks; ++i) {
      if (WiFi.SSID(i) == WIFI_SSID) {
        ++matches;
        Serial.printf("[spaeti] configured Wi-Fi found channel=%d RSSI=%d dBm auth=%d\n",
                      WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i));
      }
    }
    Serial.printf("[spaeti] configured 2.4 GHz AP matches=%d (scan status=%d)\n",
                  matches, networks);
    WiFi.scanDelete();
  }
  setMessage(configured ? "Connecting Wi-Fi..." : "No config/PSRAM - see device README");
  drawFace();
}

void loop() {
  M5.update();
  runNetwork();
  if (!configured) { delay(20); return; }

  const auto touch = M5.Touch.getDetail();
  if (touch.wasHold() && phase == Phase::Idle) {
    // Fresh agent session: new history, default persona.
    sessionSuffix = "";
    personaName = "SPAETI";
    accent = TFT_CYAN;
    caption = "";
    setMessage("New session - tap to talk");
    drawFace();
  } else if (touch.wasClicked()) {
    if (phase == Phase::Recording) stopRecording();
    else if (phase == Phase::Idle) startCall();
    // During connection/response a tap does not hijack the current call.
  }
  if (phase == Phase::Connecting && millis() - connectSince > 20000)
    endCall("WSS/auth failed - tap to retry");
  if (phase == Phase::Connecting && connected && serverListening && outputIsPCM)
    if (!startMic()) endCall("Mic error - tap to retry");
  if (phase == Phase::Recording && connected) {
    if (micReady[micNext].exchange(false, std::memory_order_acq_rel)) {
      if (!socketClient.sendBIN(reinterpret_cast<const uint8_t*>(micBuffers[micNext]),
                                kMicSamples * sizeof(int16_t)) ||
          !M5.Mic.record(micBuffers[micNext], kMicSamples, kInputRate)) {
        endCall("Mic/network error - tap to retry");
      } else micNext ^= 1;
    }
  }
  if (phase == Phase::Waiting && connected && millis() < silenceUntil &&
      static_cast<int32_t>(millis() - nextSilence) >= 0) {
    static const uint8_t silence[kMicSamples * sizeof(int16_t)] = {};
    if (!socketClient.sendBIN(silence, sizeof(silence))) endCall("Network error - tap to retry");
    nextSilence += kMicSamples * 1000 / kInputRate;
  }
  playQueued();
  if (phase == Phase::Playback && serverStatus == "listening" && hasPendingByte &&
      millis() - lastAudioAt > 2000)
    endCall("Invalid odd-length PCM output");
  if (phase == Phase::Playback && receivedAudio && serverStatus == "listening" &&
      millis() - lastAudioAt > kPlaybackQuietMs && playbackDrained()) {
    endCall("Tap to talk");
    // resetAudio() stopped the speaker and waited for its release callbacks.
    // Restore the shared codec/mic for the next call, but don't capture
    // anything until that call has started.
    if (!M5.Mic.begin()) Serial.println("Could not restore mic after playback");
  }
  if ((phase == Phase::Waiting || phase == Phase::Playback) &&
      millis() - waitSince > kReplyTimeoutMs && !receivedAudio)
    endCall("No reply - tap to retry");
  if ((captionDirty && millis() - lastDraw > 120) || millis() - lastDraw > 30000)
    drawFace();
  delay(2);
}
