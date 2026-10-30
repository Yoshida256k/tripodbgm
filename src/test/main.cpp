// トライポッド爆音光化：回路の動作確認用テストプログラム
// シリアルモニタ（115200）で数字を送ってテストを選ぶ。テスト中は何かキーを送るとメニューに戻る。

#include <Arduino.h>
#include <Wire.h>
#include <I2S.h>
#include <PWMAudio.h>
#include <BackgroundAudioMP3.h>
#include <Adafruit_NeoPixel.h>
#include <LiquidCrystal_I2C.h>

// ---- ピン割り当て（「コンポーネント、配線」より） ----
const uint8_t LCD_SDA_PIN = 16;
const uint8_t LCD_SCL_PIN = 17;
const uint8_t I2S_BCK_PIN = 26;   // LCKはBCK+1（GP27）に固定
const uint8_t I2S_DIN_PIN = 28;
const uint8_t LED_DATA_PIN = 15;

struct InputPin {
  const char *name;
  uint8_t pin;
  uint8_t mode;       // INPUT / INPUT_PULLUP
  bool activeLow;     // trueならLOWで「反応あり」
};

const InputPin INPUTS[] = {
  {"PLC1", 18, INPUT_PULLUP, true},   // フォトカプラ：PLC出力ONでLOW
  {"PLC2", 19, INPUT_PULLUP, true},
  {"PLC3", 20, INPUT_PULLUP, true},
  {"PLC4", 21, INPUT_PULLUP, true},
  {"PLC5", 22, INPUT_PULLUP, true},
  {"DROP1", 12, INPUT_PULLUP, true},  // 落下検知のタクトスイッチ：押すとLOW（外付けプルアップ＋内部プルアップ）
  {"DROP2", 13, INPUT_PULLUP, true},
  {"TCH1",  6, INPUT,        false},  // TTP223：タッチでHIGH
  {"TCH2",  7, INPUT,        false},
};
const size_t NUM_INPUTS = sizeof(INPUTS) / sizeof(INPUTS[0]);
const size_t NUM_PLC = 5;
const size_t NUM_DROP = 2;

// ---- 設定 ----
const uint16_t LED_COUNT = 25;         // テスト用のテープ（約40cm）
const uint8_t LED_BRIGHTNESS = 200;     // 0〜255。本番の150灯では全白で最大約9Aになるので抑える
const uint32_t SAMPLE_RATE = 44100;
const int16_t AUDIO_AMPLITUDE = 8000;  // フルスケール（32767）の約1/4
// I2Sのサンプルのビット数。32だとBCK = 64fs（2.822MHz）、16だとBCK = 32fs（1.411MHz）。
// SCKをGNDにしたPCM5102AはBCKからPLLでクロックを作るので、64fsの方が確実
const int I2S_BITS = 32;

// MP3再生（メニュー5）の出力先。PCM5102が届いたらfalseにするとI2S（GP26/27/28）で鳴る
const bool MP3_USE_PWM = false;
const uint8_t PWM_AUDIO_PIN = 10;      // GP10とGP11の2本を使う（偶数ピン必須）
// trueで差動駆動：左右をモノラルにまとめ、GP10に正相、GP11に逆相を出す。
// GP10とGP11の間に負荷をつなぐと、片側だけのときの2倍の振幅になる。falseで通常のステレオ
const bool PWM_DIFFERENTIAL = true;
const float MP3_GAIN = 1.0;            // 音量（0.0〜）。大きくしすぎると音が割れる

// src/mp3_data.Sで埋め込んだdata/test.mp3
extern "C" const uint8_t test_mp3_start[], test_mp3_end[];

// 音声出力の前に挟み、音の大きさ（ピーク）を記録する。
// differentialがtrueなら、ステレオの各フレームを (L+R)/2 と その反転 に置き換える（PWMの差動駆動）
// wideがtrueなら、16bitで受け取ったサンプルを32bitに広げて出す（I2SのBCKを64fsにするため）
class AudioTap : public AudioOutputBase {
public:
  AudioTap(AudioOutputBase &out, bool differential, bool wide = false)
    : _out(out), _diff(differential), _wide(wide) {}
  bool setBuffers(size_t buffers, size_t bufferWords, int32_t silenceSample = 0) override {
    return _out.setBuffers(buffers, _wide ? bufferWords * 2 : bufferWords, silenceSample);
  }
  bool setBitsPerSample(int bps) override { return _out.setBitsPerSample(_wide ? 32 : bps); }
  bool setFrequency(int freq) override { return _out.setFrequency(freq); }
  bool setStereo(bool stereo = true) override { return _out.setStereo(true); }
  bool begin() override { return _out.begin(); }
  bool end() override { return _out.end(); }
  bool getUnderflow() override { return _out.getUnderflow(); }
  void onTransmit(void (*cb)(void *), void *arg) override { _out.onTransmit(cb, arg); }
  int availableForWrite() override {
    int n = _out.availableForWrite();
    return _wide ? n / 2 : n;
  }
  size_t write(uint8_t) override { return 0; }  // 1バイト単位の書き込みは使わない

  // 16bitステレオ（L, R の順）を受け取る
  size_t write(const uint8_t *buffer, size_t size) override {
    const int16_t *in = (const int16_t *)buffer;
    size_t frames = size / 4, written = 0;
    int32_t peak = 0;
    int16_t chunk[2 * 64];
    int32_t chunk32[2 * 64];
    while (frames) {
      size_t n = frames < 64 ? frames : 64;
      for (size_t i = 0; i < n; i++) {
        int32_t m = ((int32_t)in[2 * i] + in[2 * i + 1]) / 2;
        int32_t a = m < 0 ? -m : m;
        if (a > peak) peak = a;
        if (_diff) {
          chunk[2 * i] = (int16_t)m;
          chunk[2 * i + 1] = (int16_t)(m == -32768 ? 32767 : -m);
        } else {
          chunk[2 * i] = in[2 * i];
          chunk[2 * i + 1] = in[2 * i + 1];
        }
      }
      size_t w;
      if (_wide) {
        for (size_t i = 0; i < 2 * n; i++) chunk32[i] = (int32_t)chunk[i] << 16;
        w = _out.write((const uint8_t *)chunk32, n * 8) / 2;
      } else {
        w = _out.write((const uint8_t *)chunk, n * 4);
      }
      written += w;
      if (w < n * 4) break;
      in += 2 * n;
      frames -= n;
    }
    // 書き込んだ音は数フレーム後に鳴るので、フレームごとのピークを順に残しておく
    _peaks[_head++ % PEAK_HISTORY] = peak;
    return written;
  }

  // いま鳴っている（はずの）フレームのピーク。0〜32767
  uint16_t level() const {
    return _peaks[(_head - PEAK_DELAY) % PEAK_HISTORY];
  }

private:
  static const uint32_t PEAK_HISTORY = 8;
  static const uint32_t PEAK_DELAY = 4;  // 出力バッファの段数ぶん遅らせる
  AudioOutputBase &_out;
  bool _diff;
  bool _wide;
  volatile uint16_t _peaks[PEAK_HISTORY] = {};
  volatile uint32_t _head = 0;
};

Adafruit_NeoPixel strip(LED_COUNT, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);
I2S i2s(OUTPUT);
PWMAudio pwmAudio(PWM_AUDIO_PIN, true);
AudioTap pwmTap(pwmAudio, PWM_DIFFERENTIAL);
AudioTap i2sTap(i2s, false, I2S_BITS == 32);
ROMBackgroundAudioMP3 mp3;
LiquidCrystal_I2C *lcd = nullptr;
uint8_t lcdAddr = 0;

int16_t sineTable[256];

// ---- 共通 ----

// テスト中にキーが送られたらtrue（受信した文字は捨てる）
bool keyPressed() {
  if (!Serial.available()) return false;
  while (Serial.available()) Serial.read();
  return true;
}

// 指定時間待つ。途中でキーが送られたらtrue
bool waitOrKey(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (keyPressed()) return true;
    delay(5);
  }
  return false;
}

bool readInput(const InputPin &in) {
  bool level = digitalRead(in.pin);
  return in.activeLow ? !level : level;
}

// 0x27/0x3FのLCDを探して初期化する。見つかればtrue
bool ensureLcd() {
  if (lcd) return true;
  const uint8_t candidates[] = {0x27, 0x3F};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      lcdAddr = addr;
      lcd = new LiquidCrystal_I2C(addr, 16, 2);
      lcd->init();
      lcd->backlight();
      return true;
    }
  }
  return false;
}

void printMenu() {
  Serial.println();
  Serial.println("===== トライポッド 回路テスト =====");
  Serial.println(" 1: LEDテープ (WS2812B)");
  Serial.println(" 2: LCD1602 (I2C)");
  Serial.println(" 3: 入力モニタ (PLC / 落下スイッチ / タッチ)");
  Serial.println(" 4: 音声 (PCM5102)");
  Serial.printf(" 5: MP3再生 (%s)\n", MP3_USE_PWM ? (PWM_DIFFERENTIAL ? "PWM 差動 GP10/GP11" : "PWM GP10/GP11") : "I2S PCM5102");
  Serial.println(" 6: I2S 440Hz 連続出力（測定用）");
  Serial.println(" 0: 全停止");
  Serial.println("番号を送信してください（テスト中は何かキーで中断）");
}

// ---- 1: LEDテープ ----

void testLed() {
  Serial.println("[LED] 赤→緑→青→白の全灯 → 1灯ずつ流す → レインボー");
  Serial.println("[LED] 色化けやちらつきが出たら、データ線にSN74HC125Nを入れてください");

  const uint32_t colors[] = {
    strip.Color(255, 0, 0), strip.Color(0, 255, 0),
    strip.Color(0, 0, 255), strip.Color(255, 255, 255),
  };
  const char *names[] = {"赤", "緑", "青", "白"};
  for (int i = 0; i < 4; i++) {
    Serial.printf("[LED] 全灯: %s\n", names[i]);
    strip.fill(colors[i]);
    strip.show();
    if (waitOrKey(1000)) goto done;
  }

  Serial.println("[LED] 1灯ずつ流す（入力側→末端）");
  for (uint16_t i = 0; i < LED_COUNT; i++) {
    strip.clear();
    strip.setPixelColor(i, strip.Color(255, 255, 255));
    strip.show();
    if (waitOrKey(20)) goto done;
  }

  Serial.println("[LED] レインボー（キーで終了）");
  for (uint16_t hue = 0;; hue += 256) {
    strip.rainbow(hue);
    strip.show();
    if (waitOrKey(20)) break;
  }

done:
  strip.clear();
  strip.show();
  Serial.println("[LED] 終了（消灯）");
}

// ---- 2: LCD1602 ----

void testLcd() {
  Serial.println("[LCD] I2Cバスをスキャン中...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[LCD] 応答あり: 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("[LCD] I2Cデバイスが見つかりません。SDA/SCL・レベル変換・5V電源を確認してください");
    return;
  }
  if (!ensureLcd()) {
    Serial.println("[LCD] 0x27/0x3Fに応答がありません。LCDのアドレスを確認してください");
    return;
  }

  Serial.printf("[LCD] 0x%02Xで初期化しました。カウンタを表示中（キーで終了）\n", lcdAddr);
  Serial.println("[LCD] 文字が見えなければ、裏の半固定抵抗でコントラストを調整してください");
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print("LCD TEST OK");
  for (uint32_t count = 0;; count++) {
    lcd->setCursor(0, 1);
    lcd->printf("0x%02X  %8lu", lcdAddr, (unsigned long)count);
    if (waitOrKey(200)) break;
  }
  Serial.println("[LCD] 終了");
}

// ---- 3: 入力モニタ ----

void showInputsOnLcd(const bool *state) {
  char line1[17], line2[17];
  char *p = line1 + snprintf(line1, sizeof(line1), "P:");
  for (size_t i = 0; i < NUM_PLC; i++) *p++ = state[i] ? '1' : '0';
  *p = '\0';
  snprintf(line2, sizeof(line2), "SW:%d%d T:%d%d",
           state[5], state[6], state[7], state[8]);
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(line1);
  lcd->setCursor(0, 1);
  lcd->print(line2);
}

void testInputs() {
  bool useLcd = ensureLcd();
  Serial.printf("[入力] モニタ開始（LCD表示: %s）。キーで終了\n", useLcd ? "あり" : "なし");
  Serial.println("[入力] 1 = 反応あり（PLC出力ON / 落下スイッチを押す / タッチ）");

  bool state[NUM_INPUTS];
  for (size_t i = 0; i < NUM_INPUTS; i++) {
    state[i] = readInput(INPUTS[i]);
    Serial.printf("  %s (GP%d): %s\n", INPUTS[i].name, INPUTS[i].pin, state[i] ? "ON" : "OFF");
  }
  if (useLcd) showInputsOnLcd(state);

  while (!waitOrKey(10)) {
    bool changed = false;
    for (size_t i = 0; i < NUM_INPUTS; i++) {
      bool now = readInput(INPUTS[i]);
      if (now != state[i]) {
        state[i] = now;
        changed = true;
        uint32_t ms = millis();
        Serial.printf("[%lu.%03lus] %s %s\n", (unsigned long)(ms / 1000),
                      (unsigned long)(ms % 1000), INPUTS[i].name, now ? "ON" : "OFF");
      }
    }
    if (changed && useLcd) showInputsOnLcd(state);
  }
  Serial.println("[入力] 終了");
}

// ---- 4: 音声 ----

// 位相アキュムレータでサイン波を出す。キーが送られたらtrue
bool playTone(float freqStart, float freqEnd, uint32_t ms, bool left, bool right) {
  uint32_t samples = (uint64_t)SAMPLE_RATE * ms / 1000;
  uint32_t phase = 0;
  for (uint32_t n = 0; n < samples; n++) {
    float freq = freqStart + (freqEnd - freqStart) * n / samples;
    phase += (uint32_t)(freq * 4294967296.0f / SAMPLE_RATE);
    int16_t s = sineTable[phase >> 24];
    int16_t l = left ? s : 0, r = right ? s : 0;
    if (I2S_BITS == 32) i2s.write32((int32_t)l << 16, (int32_t)r << 16);  // バッファが空くまで待つ
    else i2s.write16(l, r);
    if ((n & 1023) == 0 && keyPressed()) return true;
  }
  return false;
}

void testAudio() {
  Serial.println("[音声] アンプの音量は小さめから始めてください");
  i2s.setBCLK(I2S_BCK_PIN);
  i2s.setDATA(I2S_DIN_PIN);
  i2s.setBitsPerSample(I2S_BITS);
  Serial.printf("[音声] I2S %dbit（BCK %s）\n", I2S_BITS, I2S_BITS == 32 ? "64fs = 2.822MHz" : "32fs = 1.411MHz");
  if (!i2s.begin(SAMPLE_RATE)) {
    Serial.println("[音声] I2Sを開始できませんでした");
    return;
  }

  bool stop = false;
  Serial.println("[音声] 440Hz 左右両方");
  stop = playTone(440, 440, 1500, true, true);
  if (!stop) { Serial.println("[音声] 440Hz 左だけ"); stop = playTone(440, 440, 1500, true, false); }
  if (!stop) { Serial.println("[音声] 440Hz 右だけ"); stop = playTone(440, 440, 1500, false, true); }
  if (!stop) { Serial.println("[音声] 200Hz→2kHz スイープ"); playTone(200, 2000, 3000, true, true); }

  i2s.end();
  Serial.println("[音声] 終了");
}

// ---- 6: I2S連続出力（DAC側の測定用） ----

void testAudioContinuous() {
  i2s.setBCLK(I2S_BCK_PIN);
  i2s.setDATA(I2S_DIN_PIN);
  i2s.setBitsPerSample(I2S_BITS);
  if (!i2s.begin(SAMPLE_RATE)) {
    Serial.println("[連続] I2Sを開始できませんでした");
    return;
  }
  Serial.printf("[連続] 440Hzを左右に出し続けます（キーで停止）。期待値: BCK %s / LCK 44.1kHz\n",
                I2S_BITS == 32 ? "2.822MHz" : "1.411MHz");
  while (!playTone(440, 440, 1000, true, true)) {
  }
  i2s.end();
  Serial.println("[連続] 終了");
}

// ---- 5: MP3再生 ----

// 音量メーター：テープの入力側から、緑→黄→赤で伸ばす。上がるのは即座に、下がるのはゆっくり
void showMeter(uint16_t level, float &meter) {
  float target = (float)level * LED_COUNT / 32767.0f * 1.5f;  // 少し感度を上げる
  if (target > meter) meter = target;
  else meter -= (meter - target) * 0.15f + 0.1f;
  if (meter < 0) meter = 0;

  for (uint16_t i = 0; i < LED_COUNT; i++) {
    uint32_t color = 0;
    if (i < meter) {
      uint16_t pos = i * 100 / LED_COUNT;
      if (pos < 60) color = strip.Color(0, 255, 0);
      else if (pos < 85) color = strip.Color(255, 160, 0);
      else color = strip.Color(255, 0, 0);
    }
    strip.setPixelColor(i, color);
  }
  strip.show();
}

void testMp3() {
  const size_t total = test_mp3_end - test_mp3_start;
  Serial.printf("[MP3] data/test.mp3 (%u バイト) を %s で再生します。キーで終了\n",
                (unsigned)total, MP3_USE_PWM ? (PWM_DIFFERENTIAL ? "PWM 差動 GP10/GP11" : "PWM GP10/GP11") : "I2S PCM5102");
  Serial.println("[MP3] アンプの音量は最小から上げてください。再生中はLEDが音量メーターになります");

  AudioTap &tap = MP3_USE_PWM ? pwmTap : i2sTap;
  mp3.setDevice(&tap);
  mp3.setGain(MP3_GAIN);
  mp3.flush();
  mp3.write(test_mp3_start, total);  // ROM用のバッファなのでポインタを渡すだけ
  if (!mp3.begin()) {
    Serial.println("[MP3] 再生を開始できませんでした");
    return;
  }

  bool useLcd = ensureLcd();
  if (useLcd) {
    lcd->clear();
    lcd->setCursor(0, 0);
    lcd->print("MP3 PLAYING");
  }

  uint32_t start = millis();
  uint32_t lastReport = start;
  float meter = 0;  // 表示中のメーターの長さ（LEDの数）
  bool stopped = false;
  while (!mp3.done()) {
    if (waitOrKey(20)) {
      stopped = true;
      break;
    }
    showMeter(tap.level(), meter);

    uint32_t now = millis();
    if (now - lastReport >= 1000) {
      lastReport = now;
      uint32_t sec = (now - start) / 1000;
      unsigned percent = 100 - (unsigned)((uint64_t)mp3.available() * 100 / total);
      Serial.printf("[MP3] %lu:%02lu  %u%%  (デコードエラー %lu, 欠落 %lu)\n",
                    (unsigned long)(sec / 60), (unsigned long)(sec % 60), percent,
                    (unsigned long)mp3.errors(), (unsigned long)mp3.underflows());
      if (useLcd) {
        lcd->setCursor(0, 1);
        lcd->printf("%2lu:%02lu  %3u%%    ", (unsigned long)(sec / 60), (unsigned long)(sec % 60), percent);
      }
    }
  }

  mp3.end();
  strip.clear();
  strip.show();
  Serial.printf("[MP3] %s\n", stopped ? "中断しました" : "最後まで再生しました");
}

// ---- 0: 全停止 ----

void stopAll() {
  strip.clear();
  strip.show();
  mp3.end();
  i2s.end();
  Serial.println("[停止] LED消灯・音声停止");
}

// ---- メイン ----

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  for (int i = 0; i < 256; i++) {
    sineTable[i] = (int16_t)(AUDIO_AMPLITUDE * sinf(2.0f * PI * i / 256));
  }
  for (const InputPin &in : INPUTS) pinMode(in.pin, in.mode);

  i2s.setBCLK(I2S_BCK_PIN);
  i2s.setDATA(I2S_DIN_PIN);

  Wire.setSDA(LCD_SDA_PIN);
  Wire.setSCL(LCD_SCL_PIN);
  Wire.begin();

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

  printMenu();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case '1': testLed(); break;
    case '2': testLcd(); break;
    case '3': testInputs(); break;
    case '4': testAudio(); break;
    case '5': testMp3(); break;
    case '6': testAudioContinuous(); break;
    case '0': stopAll(); break;
    default: return;  // 改行などは無視
  }
  printMenu();
}
