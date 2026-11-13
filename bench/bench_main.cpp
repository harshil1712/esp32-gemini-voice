// CoreS3-Lite bench firmware for the DevFest talk. No network credentials
// needed. Measures memory headroom, Wi-Fi visibility, and the mic/speaker
// interactions the production firmware works around.
//
//   python3 -m platformio run -e cores3-lite-bench -t upload
//   then read serial: every result line starts with "BENCH ".
//
// Restore the voice firmware afterwards with `python3 -m platformio run -t upload`.
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>

namespace {
constexpr uint32_t kInRate = 16000, kOutRate = 24000;
constexpr size_t kMicSamples = 640;   // 40 ms, as in production
constexpr size_t kPlaySamples = 960;  // 40 ms, as in production
constexpr size_t kPlaySlots = 64;

int16_t* mic = nullptr;
int16_t* slots[kPlaySlots] = {};
int line = 0;

void heap(const char* stage) {
  Serial.printf("BENCH heap stage=%s internal_free=%u internal_largest=%u internal_min=%u "
                "psram_free=%u\n", stage,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void screen(const char* text) {
  M5.Display.setCursor(4, 4 + 12 * (line++ % 19));
  M5.Display.print(text);
  Serial.printf("BENCH step %s\n", text);
}

struct Level { float rms; int peak; bool ok; };

// Record `ms` of mic audio. Returns RMS (0..1 of full scale) and peak.
Level record(uint32_t ms) {
  Level out{0, 0, false};
  const uint32_t frames = ms / 40;
  double squares = 0;
  size_t samples = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    if (!M5.Mic.record(mic, kMicSamples, kInRate)) return out;
    const uint32_t start = millis();
    while (M5.Mic.isRecording() && millis() - start < 500) delay(1);
    if (f < 3) continue; // skip codec settling
    for (size_t i = 0; i < kMicSamples; ++i) {
      squares += double(mic[i]) * mic[i];
      out.peak = max(out.peak, abs(int(mic[i])));
    }
    samples += kMicSamples;
  }
  out.rms = samples ? sqrt(squares / samples) / 32768.0 : 0;
  out.ok = true;
  return out;
}

void report(const char* name, const Level& l) {
  Serial.printf("BENCH mic test=%s ok=%d rms_permille=%.2f peak=%d\n", name, l.ok, l.rms * 1000, l.peak);
}

void fillTone(float hz, float amplitude) {
  for (size_t s = 0; s < kPlaySlots; ++s)
    for (size_t i = 0; i < kPlaySamples; ++i) {
      const float t = float(s * kPlaySamples + i) / kOutRate;
      slots[s][i] = int16_t(amplitude * 32767 * sinf(2 * PI * hz * t));
    }
}

// Play `count` 40 ms slots like production (keep <= 2 queued) and time it.
uint32_t playSlots(size_t count) {
  const uint32_t start = millis();
  size_t next = 0;
  while (next < count) {
    if (M5.Speaker.isPlaying(0) < 2) {
      if (!M5.Speaker.playRaw(slots[next % kPlaySlots], kPlaySamples, kOutRate, false, 1, 0)) {
        Serial.println("BENCH speaker playRaw_failed=1");
        return 0;
      }
      ++next;
    }
    delay(1);
  }
  while (M5.Speaker.isPlaying()) delay(1);
  return millis() - start;
}
} // namespace

void setup() {
  M5.begin(M5.config());
  Serial.begin(115200);
  delay(2500); // give the host time to open the port
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setTextSize(1);
  Serial.println("BENCH start board=CoreS3-Lite");
  Serial.printf("BENCH chip model=%s cores=%d mhz=%u psram_total=%u flash=%u\n",
                ESP.getChipModel(), ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz(),
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFlashChipSize());
  heap("boot");

  // Same allocations as production, in PSRAM.
  mic = (int16_t*)heap_caps_malloc(kMicSamples * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  for (auto& s : slots)
    s = (int16_t*)heap_caps_malloc(kPlaySamples * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  heap("buffers_psram");

  // --- Wi-Fi: what can a 2.4 GHz-only radio see here? ---------------------
  screen("wifi scan...");
  WiFi.mode(WIFI_STA);
  heap("wifi_sta_init");
  const int n = WiFi.scanNetworks(false, true);
  int open = 0, strongest = -127;
  for (int i = 0; i < n; ++i) {
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ++open;
    strongest = max(strongest, int(WiFi.RSSI(i)));
    Serial.printf("BENCH ap channel=%d rssi=%d open=%d ssid_len=%u\n", WiFi.channel(i),
                  WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN, (unsigned)WiFi.SSID(i).length());
  }
  Serial.printf("BENCH wifi aps_2g4=%d open=%d strongest_rssi=%d\n", n, open, strongest);
  WiFi.scanDelete();
  heap("wifi_after_scan");

  // --- Mic baseline -----------------------------------------------------
  screen("mic baseline (stay quiet)...");
  M5.Speaker.end();
  if (!M5.Mic.begin()) Serial.println("BENCH mic begin_failed=1");
  heap("mic_running");
  report("baseline_room", record(2000));

  // --- Speaker: 1 s tone through the production slot path -----------------
  screen("speaker tone 1s...");
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(120);
  fillTone(440, 0.3f);
  heap("speaker_running");
  const uint32_t played = playSlots(25);
  Serial.printf("BENCH speaker slots=25 audio_ms=1000 wall_ms=%u\n", (unsigned)played);

  // --- Bug 1 on CoreS3: mic without tearing the speaker down --------------
  // RESULT (Nov 12): on the CoreS3-Lite this crashes the device. Mic.begin()
  // flips the shared I2S port to RX, the speaker task's i2s_write fails with
  // "TX mode is not enabled", and spk_task overflows its stack -> reboot.
  // Build with -DBENCH_CRASH_TEST=1 to reproduce it on purpose.
#if BENCH_CRASH_TEST
  screen("mic after playback, no Speaker.end...");
  const bool beganDirty = M5.Mic.begin();
  Serial.printf("BENCH mic_after_play_no_speaker_end begin=%d speaker_running=%d\n",
                beganDirty, M5.Speaker.isRunning());
  report("after_play_no_speaker_end", record(2000));
#endif

  // --- Production path: Speaker.end -> Mic.end -> Mic.begin -----------------
  screen("mic after playback, prod reinit...");
  M5.Speaker.end();
  M5.Mic.end();
  delay(10);
  Serial.printf("BENCH mic_prod_reinit begin=%d\n", M5.Mic.begin());
  report("after_play_prod_reinit", record(2000));

  // --- Full duplex? Speaker while mic is running ----------------------------
  // RESULT (Nov 12): also crashes. Speaker.begin() takes the shared I2S port,
  // mic_task's i2s_read fails ("RX mode is not enabled") and trips its stack
  // canary -> panic + reboot. The ES7210 is full-duplex on paper; with
  // M5Unified on this board it is strictly half-duplex. Push-to-talk it is.
#if BENCH_CRASH_TEST
  screen("duplex: tone while recording...");
  const bool spk = M5.Speaker.begin();
  Serial.printf("BENCH duplex speaker_begin=%d mic_running_after=%d\n", spk, M5.Mic.isRunning());
  M5.Speaker.setVolume(120);
  size_t queued = 0;
  for (; queued < 2; ++queued) M5.Speaker.playRaw(slots[queued], kPlaySamples, kOutRate, false, 50, 0);
  report("during_tone_duplex", record(1600));
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.end();
  M5.Mic.begin();
  report("after_duplex_reinit", record(1500));
#endif

  // --- The 44-byte click: a WAV header played as PCM ----------------------
  screen("click test: WAV header as PCM...");
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(120);
  static const uint8_t wavHeader[44] = {'R','I','F','F',0x24,0x77,1,0,'W','A','V','E','f','m','t',' ',
    16,0,0,0,1,0,1,0,0xC0,0x5D,0,0,0x80,0xBB,0,0,2,0,16,0,'d','a','t','a',0,0x77,1,0};
  static int16_t click[kPlaySamples] = {};
  memcpy(click, wavHeader, sizeof(wavHeader)); // the rest is silence
  for (int i = 0; i < 3; ++i) {
    M5.Speaker.playRaw(click, kPlaySamples, kOutRate, false, 1, 0);
    while (M5.Speaker.isPlaying()) delay(1);
    delay(600);
  }
  int maxAbs = 0;
  for (int i = 0; i < 22; ++i) maxAbs = max(maxAbs, abs(int(click[i])));
  Serial.printf("BENCH click samples=22 duration_us=917 max_abs=%d pct_full_scale=%d\n",
                maxAbs, maxAbs * 100 / 32767);
  M5.Speaker.end();

  heap("end");
  Serial.println("BENCH done=1");
  screen("DONE - see serial");
}

void loop() {
  delay(1000);
}
