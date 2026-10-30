// 音の出し方（sound.h を参照）
//
//   電子音（Voice bgm + sfx を合成）──┐
//   BGM用 MP3デコーダ ───────────┤
//   BGM用 WAVデコーダ ───────────┼─▶ ミキサー（44.1kHz、16bit）─▶ 16→32bit変換 ─▶ I2S（PCM5102A）
//   効果音用 WAVデコーダ ─────────┘
//
// MP3のサンプリング周波数が違っても（24k/48kHzなど）、ミキサーが44.1kHzに変換する。
// MP3の1フレーム（26ms分）のデコードに200MHzで約11msかかり、2本同時だと間に合わないので、効果音はWAVだけにしている。

#include "sound.h"
#include <I2S.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <BackgroundAudioMP3.h>
#include <BackgroundAudioWAV.h>
#include <BackgroundAudioMixer.h>
#include <hardware/irq.h>

// ---- ピン割り当て ----
const uint8_t I2S_BCK_PIN = 26;  // LCKはBCK+1（GP27）に固定
const uint8_t I2S_DIN_PIN = 28;

// ---- 音量 ----
const uint32_t SAMPLE_RATE = 44100;
const int32_t SFX_VOLUME = 6000;   // 電子音の効果音・ジングルの音量（候補の音量100%のとき、0〜32767）
const int32_t BGM_VOLUME = 3000;   // 電子音のBGMの音量（同上）
const float FILE_MAX_GAIN = 0.5f;  // MP3・WAVの音量100%のときの倍率。市販の音源はほぼ最大音量なので、電子音と釣り合うよう下げる

const char *CONFIG_PATH = "/config.json";
const char *SOUND_DIR = "/snd/";

const SlotInfo SLOT_INFO[NUM_SLOTS] = {
  {"power", "電源ON", false, true},
  {"idle", "待機BGM", true, true},
  {"standby", "スタンバイ", false, true},
  {"spin", "回転中BGM", true, true},
  {"tick", "ランプのピッ", false, false},
  {"win", "当たり", false, true},
  {"lose", "ハズレ", false, true},
  {"get", "景品獲得", false, true},
  {"reset", "リセット", false, true},
};

// ===================================================================
// 電子音（矩形波）
// ===================================================================

struct Note {
  uint16_t freq;  // Hz。0は休符
  uint16_t ms;
};

// 音名（Hz）
enum : uint16_t {
  N_R = 0,
  N_E4 = 330, N_F4 = 349, N_FS4 = 370, N_G4 = 392,
  N_C5 = 523, N_D5 = 587, N_E5 = 659, N_F5 = 698, N_G5 = 784, N_A5 = 880, N_B5 = 988,
  N_C6 = 1047, N_D6 = 1175, N_E6 = 1319, N_G6 = 1568, N_C7 = 2093,
};

// 曲は「周波数と長さ」の並び。差し替えるときはここを書き換える
const Note SONG_START[] = {{N_C5, 100}, {N_E5, 100}, {N_G5, 100}, {N_C6, 300}};
const Note SONG_SPIN[] = {  // 回転中のBGM（ループ）
  {N_C5, 70}, {N_E5, 70}, {N_G5, 70}, {N_E5, 70},
  {N_F5, 70}, {N_A5, 70}, {N_C6, 70}, {N_A5, 70},
  {N_G5, 70}, {N_B5, 70}, {N_D6, 70}, {N_B5, 70},
  {N_C6, 70}, {N_G5, 70}, {N_E5, 70}, {N_G5, 70},
};
const Note SONG_TICK[] = {{N_G6, 25}};
const Note SONG_WIN[] = {
  {N_G5, 120}, {N_G5, 120}, {N_G5, 120}, {N_C6, 400}, {N_R, 60},
  {N_A5, 120}, {N_B5, 120}, {N_C6, 120}, {N_E6, 600},
};
const Note SONG_LOSE[] = {{N_G4, 200}, {N_FS4, 200}, {N_F4, 200}, {N_E4, 600}};
const Note SONG_RESET[] = {{N_C6, 60}, {N_G5, 60}, {N_C5, 120}};
const Note SONG_GET[] = {  // 景品獲得：駆け上がり＋トリル
  {N_C6, 80}, {N_E6, 80}, {N_G6, 80}, {N_C7, 200}, {N_R, 40},
  {N_G6, 50}, {N_C7, 50}, {N_G6, 50}, {N_C7, 50}, {N_G6, 50}, {N_C7, 50}, {N_C7, 400},
};

struct Song {
  const Note *notes;
  size_t len;
};
#define SONG(x) {x, sizeof(x) / sizeof(x[0])}

// 場面ごとの電子音の曲（SLOT_INFOと同じ順）。待機BGMには電子音の曲がない
const Song CHIP_SONG[NUM_SLOTS] = {
  SONG(SONG_START),  // 電源ON
  {nullptr, 0},      // 待機BGM
  SONG(SONG_START),  // スタンバイ
  SONG(SONG_SPIN),   // 回転中BGM
  SONG(SONG_TICK),   // ランプのピッ
  SONG(SONG_WIN),    // 当たり
  SONG(SONG_LOSE),   // ハズレ
  SONG(SONG_GET),    // 景品獲得
  SONG(SONG_RESET),  // リセット
};

class Voice {
public:
  void play(const Note *song, size_t len, bool loop, float pitch = 1.0f) {
    _song = song;
    _len = len;
    _loop = loop;
    _pitch = pitch;
    _index = 0;
    startNote();
  }
  void stop() { _song = nullptr; }
  bool playing() const { return _song != nullptr; }

  // 1サンプル分（-1〜1の矩形波に音量の変化を掛けたもの、×1000）
  int32_t next() {
    if (!_song) return 0;
    if (_remain == 0) {
      if (++_index >= _len) {
        if (!_loop) { _song = nullptr; return 0; }
        _index = 0;
      }
      startNote();
    }
    _remain--;
    if (_inc == 0) return 0;
    _phase += _inc;
    // 音の頭（2ms）と終わり（5ms）で音量を変えて、プチノイズを防ぐ
    uint32_t pos = _total - _remain;
    int32_t env = 1000;
    if (pos < ATTACK) env = pos * 1000 / ATTACK;
    else if (_remain < RELEASE) env = _remain * 1000 / RELEASE;
    return (_phase & 0x80000000u) ? env : -env;
  }

private:
  static const uint32_t ATTACK = SAMPLE_RATE * 2 / 1000;
  static const uint32_t RELEASE = SAMPLE_RATE * 5 / 1000;

  void startNote() {
    const Note &n = _song[_index];
    _total = _remain = (uint32_t)n.ms * SAMPLE_RATE / 1000;
    _inc = n.freq ? (uint32_t)(n.freq * _pitch * 4294967296.0f / SAMPLE_RATE) : 0;
  }

  const Note *_song = nullptr;
  size_t _len = 0, _index = 0;
  bool _loop = false;
  float _pitch = 1.0f;
  uint32_t _total = 0, _remain = 0, _phase = 0, _inc = 0;
};

Voice bgm, sfx;
int32_t bgmAmp = BGM_VOLUME, sfxAmp = SFX_VOLUME;

// ===================================================================
// 出力（ミキサー → 16→32bit変換 → I2S）
// ===================================================================

// ミキサーは16bitで出すが、SCKなしのPCM5102AはBCK＝64fs（32bit）の方が確実なので、32bitに広げてI2Sに渡す
class I2S32Out : public AudioOutputBase {
public:
  explicit I2S32Out(I2S &out) : _out(out) {}
  // 16bitステレオ1組（1ワード）が、32bitでは2ワードになる
  bool setBuffers(size_t buffers, size_t bufferWords, int32_t silenceSample = 0) override {
    return _out.setBuffers(buffers, bufferWords * 2, silenceSample);
  }
  bool setBitsPerSample(int) override { return _out.setBitsPerSample(32); }
  bool setFrequency(int freq) override { return _out.setFrequency(freq); }
  bool setStereo(bool) override { return _out.setStereo(true); }
  bool begin() override { return _out.begin(); }
  bool end() override { return _out.end(); }
  bool getUnderflow() override { return _out.getUnderflow(); }
  void onTransmit(void (*cb)(void *), void *arg) override { _out.onTransmit(cb, arg); }
  int availableForWrite() override { return _out.availableForWrite() / 2; }
  size_t write(uint8_t) override { return 0; }  // 1バイト単位の書き込みは使わない

  size_t write(const uint8_t *buffer, size_t size) override {
    const int16_t *in = (const int16_t *)buffer;
    size_t samples = size / 2, done = 0;
    int32_t chunk[128];
    while (done < samples) {
      size_t n = samples - done < 128 ? samples - done : 128;
      for (size_t i = 0; i < n; i++) chunk[i] = (int32_t)in[done + i] << 16;
      size_t w = _out.write((const uint8_t *)chunk, n * 4) / 4;
      done += w;
      if (w < n) break;
    }
    return done * 2;
  }

private:
  I2S &_out;
};

I2S i2s(OUTPUT);
I2S32Out i2sOut(i2s);
BackgroundAudioMixer<512> mixer(i2sOut, SAMPLE_RATE);  // 1回に約12ms分を合成する
BackgroundAudioMixerInput *chipLeg = nullptr;

// デコーダ（MP3・WAV）とミキサーの入力の間に挟む中継。
// デコーダは空きを「バイト」で見るが、ミキサーの入力は「ワード（16bitステレオ1組）」で返すため、
// そのままだと1フレーム分しか先にデコードせず、デコードが遅れると無音が挟まる（ぶつぶつ切れて、遅く聞こえる）。
// 鳴らしている間（deep）は空きを4倍してバイトで返し、バッファいっぱいまで先にデコードさせる。
// 止めている間は元のまま返し、無音を溜めすぎないようにする（溜めると次に鳴らすときに遅れる）。
// 鳴らし始めにバッファいっぱいまで続けてデコードすると、割り込みが長く続いてloop()が止まるので、
// ミキサーの合図1回につき2フレームまでにする（合図はバッファ1つ分を使うごとに来るので、それでも溜まっていく）
class DecoderLeg : public AudioOutputBase {
public:
  // frameWords：デコーダが1回に書くフレームの長さ（MP3は1152、WAVは512）
  void attach(AudioOutputBase *leg, size_t buffers, size_t frameWords) {
    _leg = leg;
    _buffers = buffers;
    _frameWords = frameWords;
  }
  void setDeep(bool deep) { _deep = deep; }

  bool setBuffers(size_t, size_t bufferWords, int32_t silenceSample = 0) override {
    return _leg->setBuffers(_buffers, bufferWords, silenceSample);
  }
  bool setBitsPerSample(int bps) override { return _leg->setBitsPerSample(bps); }
  bool setFrequency(int freq) override { return _leg->setFrequency(freq); }
  bool setStereo(bool stereo) override { return _leg->setStereo(stereo); }
  bool begin() override { return _leg->begin(); }
  bool end() override { return _leg->end(); }
  bool getUnderflow() override { return _leg->getUnderflow(); }
  void onTransmit(void (*cb)(void *), void *arg) override {
    _cb = cb;
    _cbArg = arg;
    _leg->onTransmit(request, this);
  }
  size_t write(uint8_t) override { return 0; }

  int availableForWrite() override {
    int words = _leg->availableForWrite();
    if (!_deep) return words;
    if (_budget == 0) return 0;
    // デコーダはこのあと1フレームをデコードしてwriteする。その間の時間を測る
    if (words >= (int)_frameWords) _t0 = time_us_32();
    return words * 4;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (_deep && _t0 && size == _frameWords * 4) {
      uint32_t us = time_us_32() - _t0;
      if (us > maxUs) maxUs = us;
      totalUs += us;
      count++;
    }
    _t0 = 0;
    if (_budget) _budget--;
    return _leg->write(buffer, size);
  }

  // デコード時間（? で表示して0に戻す）
  volatile uint32_t maxUs = 0, totalUs = 0, count = 0;

private:
  AudioOutputBase *_leg = nullptr;
  size_t _buffers = 5, _frameWords = 0;
  volatile bool _deep = false;
  uint32_t _t0 = 0;
  void (*_cb)(void *) = nullptr;
  void *_cbArg = nullptr;
  volatile uint8_t _budget = 0;  // 今回の合図でデコードしてよいフレームの数

  static void request(void *self) {
    DecoderLeg *l = (DecoderLeg *)self;
    l->_budget = 2;
    if (l->_cb) l->_cb(l->_cbArg);
  }
};

// 音のファイルを1本鳴らす。LittleFSのファイルを少しずつデコーダに渡す。
// Dec は BackgroundAudioMP3Class か BackgroundAudioWAVClass。
// MP3デコーダの割り込みの登録先は型ごとに1つなので、MP3を2本使うときはバッファの大きさを変えて別の型にすること
template <class Dec>
class FileChannel {
public:
  FileChannel(bool mp3, size_t frameWords) : _mp3(mp3), _frameWords(frameWords) {}

  // buffers：ミキサーの入力に置くデコード済みのフレームの数
  void begin(AudioOutputBase *mixLeg, size_t buffers) {
    _leg.attach(mixLeg, buffers, _frameWords);
    _dec.setDevice(&_leg);
    _dec.begin();
  }

  bool play(const char *name, float gain, bool loop) {
    stop();
    String path = String(SOUND_DIR) + name;
    _file = LittleFS.open(path, "r");
    if (!_file) return false;
    _start = _mp3 ? id3Size() : 0;  // WAVは先頭のヘッダごと渡す（ループで先頭に戻ると、デコーダがヘッダを読み直す）
    _loop = loop && _file.size() > _start;
    _file.seek(_start);
    _eof = false;
    _dec.setGain(gain);
    _dec.flush();
    feed();
    _leg.setDeep(true);
    _dec.unpause();
    _active = true;
    return true;
  }

  void stop() {
    idle();
    _dec.flush();
    if (_file) _file.close();
    _active = false;
    _suspended = false;
    _eof = true;
  }

  // 鳴らしている途中で一時停止する。ファイルの位置とデコーダの中身は残すので、resume()で続きから鳴る。
  // MP3で1フレームもデコードしていないうちは一時停止できない（idle()を参照）ので、止めてfalseを返す
  bool suspend() {
    if (!_active || _suspended) return _suspended;
    if (_mp3 && _dec.frames() == 0) {
      stop();
      return false;
    }
    _dec.pause();
    _leg.setDeep(false);
    _suspended = true;
    return true;
  }

  void resume() {
    if (!_suspended) return;
    _suspended = false;
    _leg.setDeep(true);
    _dec.unpause();
  }

  bool active() const { return _active; }
  bool suspended() const { return _suspended; }
  uint32_t underflows() { return _dec.underflows(); }
  DecoderLeg &leg() { return _leg; }

  void update() {
    if (_suspended) return;  // 一時停止の間はファイルを読まない
    if (!_active) {
      if (!_dec.paused()) idle();
      return;
    }
    feed();
    // ファイルを最後まで渡したあと、デコーダがデータ切れになったら鳴り終わり。
    // （ライブラリの done() は、ファイルの終わり方によっては true にならないことがある）
    if (_eof && (_dec.done() || _dec.underflows() - _eofUnderflows >= 2)) {
      idle();
      _active = false;
    }
  }

private:
  // 鳴らしていないときは一時停止して、デコードせずに無音を出す（CPUを空ける）。
  // ただしMP3は、1フレームもデコードしていないうちに一時停止すると、ライブラリが長さ0の無音を
  // 書き続けて割り込みの中で止まってしまう（フレームの長さが決まっていないため）。
  // そのため、最初の1フレーム（データがなければ無音のフレーム）をデコードしてから止める
  void idle() {
    if (!_mp3 || _dec.frames() > 0) {
      _dec.pause();
      _leg.setDeep(false);
    }
  }

  // 先頭のタグ（ID3v2）の大きさ。ループで先頭に戻るとき、タグを飛ばす
  uint32_t id3Size() {
    uint8_t h[10];
    if (_file.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    uint32_t size = ((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
    return 10 + size + (h[5] & 0x10 ? 10 : 0);
  }

  // 1回に読むのは4KBまで。一度にバッファいっぱい読むと、そのあいだloop()が止まる（場面の判定が遅れる）
  void feed() {
    uint8_t buf[1024];
    for (int k = 0; k < 4 && !_eof && _dec.availableForWrite() >= sizeof(buf); k++) {
      int n = _file.read(buf, sizeof(buf));
      if (n > 0) _dec.write(buf, n);
      if (n < (int)sizeof(buf)) {
        if (_loop) {
          _file.seek(_start);
        } else {
          _file.close();
          _eof = true;
          _eofUnderflows = _dec.underflows();
        }
      }
    }
  }

  const bool _mp3;
  const size_t _frameWords;
  Dec _dec;
  DecoderLeg _leg;
  File _file;
  uint32_t _start = 0;
  uint32_t _eofUnderflows = 0;
  bool _loop = false, _active = false, _eof = true, _suspended = false;
};

// BGMはMP3かWAV、効果音はWAVだけ（MP3を2本同時にデコードするとCPUが足りない）。
// WAVは44.1kHz・16bit（GUIが転送するときに変換する）で約88KB/秒（モノラル）なので、バッファは約0.1秒分
FileChannel<BackgroundAudioMP3Class<RawDataBuffer<16 * 1024>>> bgmMp3(true, 1152);  // 320kbpsで約0.4秒分
FileChannel<BackgroundAudioWAVClass<RawDataBuffer<16 * 1024>>> bgmWav(false, 512);
FileChannel<BackgroundAudioWAVClass<RawDataBuffer<16 * 1024>>> sfxWav(false, 512);
bool muted = false;
uint32_t i2sUnderflows = 0;  // I2Sに渡す音が間に合わなかった回数（CPUが足りない）
// ミキサーの入力に音が間に合わず、無音が挟まった回数（電子音・BGM・効果音）
uint32_t chipGaps = 0, bgmGaps = 0, sfxGaps = 0;

// 電子音は割り込みで作る（loop()が遅れても切れないように）。
// ミキサーが電子音のバッファを1つ使い終わるたびに、この割り込みを起こして書き足す。
// MP3のデコード（優先度0xc0）より優先にする。電子音の計算は軽い
uint8_t chipIrq = 0;

void fillChip() {
  while (chipLeg->availableForWrite() >= 64) {  // 単位は16bitステレオ1組
    int16_t buf[64 * 2];
    for (int i = 0; i < 64; i++) {
      int32_t s = (bgm.next() * bgmAmp + sfx.next() * sfxAmp) / 1000;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
    }
    chipLeg->write((const uint8_t *)buf, sizeof(buf));
  }
}

void chipIrqHandler() {
  fillChip();
  irq_clear(chipIrq);
}

void chipRequest(void *) {
  irq_set_pending(chipIrq);
}

// ===================================================================
// 設定（場面ごとの候補）
// ===================================================================

struct SlotCfg {
  uint8_t n = 0;
  Cand c[MAX_CAND];
  int8_t last = -1;  // 前回選んだ候補（続けて同じものを選ばないため）
};
SlotCfg slots[NUM_SLOTS];
SlotCfg tmpCfg[NUM_SLOTS];  // 読み込んだ設定を検証するための置き場
// BGMの流し方。false：場面ごと（待機と回転中だけ）、true：通し（プレイ中も待機BGMを流し続ける）
bool bgmThrough = false;

bool chipAvailable(Slot s) {
  return CHIP_SONG[s].notes != nullptr;
}

bool slotFromId(const char *id, Slot &out) {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (strcmp(SLOT_INFO[i].id, id) == 0) {
      out = (Slot)i;
      return true;
    }
  }
  return false;
}

// 設定がないときは、今までどおり電子音にする（電源ONと待機BGMは無音）
void setDefaultConfig(SlotCfg *cfg) {
  for (int i = 0; i < NUM_SLOTS; i++) {
    cfg[i] = SlotCfg();
    if (i == SL_POWER || !chipAvailable((Slot)i)) continue;
    cfg[i].n = 1;
    cfg[i].c[0] = {CAND_CHIP, 100, ""};
  }
}

bool hasExt(const char *name, const char *ext) {
  size_t n = strlen(name), e = strlen(ext);
  return n > e && strcasecmp(name + n - e, ext) == 0;
}

const char *fileProblem(Slot s, const char *name) {
  if (!SLOT_INFO[s].fileAllowed) return "この場面はファイルを選べません";
  if (hasExt(name, ".wav")) return nullptr;
  if (hasExt(name, ".mp3")) return SLOT_INFO[s].bgm ? nullptr : "効果音にはWAVしか使えません";
  return "MP3かWAVのファイルではありません";
}

const char *typeName(CandType t) {
  return t == CAND_CHIP ? "chip" : t == CAND_FILE ? "file" : "none";
}

// JSONを読んでcfgとthroughに入れる。おかしな値があればerrに理由を入れてfalse
bool parseConfig(const char *json, SlotCfg *cfg, bool &through, const char *&err) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    err = "JSONを読めません";
    return false;
  }
  JsonObject obj = doc["slots"];
  if (obj.isNull()) {
    err = "slotsがありません";
    return false;
  }
  const char *mode = doc["bgmMode"] | "scene";  // ないとき（版2までの設定）は場面ごと
  if (strcmp(mode, "scene") != 0 && strcmp(mode, "through") != 0) {
    err = "bgmModeがおかしいです";
    return false;
  }
  through = strcmp(mode, "through") == 0;
  for (int i = 0; i < NUM_SLOTS; i++) cfg[i] = SlotCfg();
  for (JsonPair kv : obj) {
    Slot s;
    if (!slotFromId(kv.key().c_str(), s)) continue;  // 知らない場面は無視
    for (JsonObject o : kv.value().as<JsonArray>()) {
      if (cfg[s].n >= MAX_CAND) {
        err = "候補が多すぎます";
        return false;
      }
      Cand &c = cfg[s].c[cfg[s].n];
      const char *t = o["t"] | "none";
      int v = o["v"] | 100;
      c.vol = v < 0 ? 0 : v > 100 ? 100 : v;
      c.file[0] = '\0';
      if (strcmp(t, "chip") == 0) {
        if (!chipAvailable(s)) {
          err = "電子音の曲がない場面です";
          return false;
        }
        c.type = CAND_CHIP;
      } else if (strcmp(t, "file") == 0 || strcmp(t, "mp3") == 0) {  // "mp3" は前の版の書き方
        const char *f = o["f"] | "";
        if (!SLOT_INFO[s].fileAllowed) {
          err = "この場面はファイルを選べません";
          return false;
        }
        if (!*f || strlen(f) >= FILE_NAME_LEN || strchr(f, '/')) {
          err = "ファイル名がおかしいです";
          return false;
        }
        strcpy(c.file, f);
        // 前の版では効果音にMP3を使えた。GUIは転送するときに同じ名前のWAVに変換するので、そちらを使う
        if (!SLOT_INFO[s].bgm && hasExt(c.file, ".mp3")) strcpy(c.file + strlen(c.file) - 4, ".wav");
        if (const char *why = fileProblem(s, c.file)) {
          err = why;
          return false;
        }
        c.type = CAND_FILE;
      } else {
        c.type = CAND_NONE;
      }
      cfg[s].n++;
    }
  }
  return true;
}

void printConfigJson(Print &out) {
  JsonDocument doc;
  doc["bgmMode"] = bgmThrough ? "through" : "scene";
  JsonObject obj = doc["slots"].to<JsonObject>();
  for (int i = 0; i < NUM_SLOTS; i++) {
    JsonArray arr = obj[SLOT_INFO[i].id].to<JsonArray>();
    for (int k = 0; k < slots[i].n; k++) {
      const Cand &c = slots[i].c[k];
      JsonObject o = arr.add<JsonObject>();
      o["t"] = typeName(c.type);
      if (c.type == CAND_FILE) o["f"] = c.file;
      if (c.type != CAND_NONE) o["v"] = c.vol;
    }
  }
  serializeJson(doc, out);
}

bool applyConfigJson(const char *json, const char *&err) {
  bool through;
  if (!parseConfig(json, tmpCfg, through, err)) return false;
  for (int i = 0; i < NUM_SLOTS; i++) slots[i] = tmpCfg[i];
  bgmThrough = through;
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    err = "設定を保存できません";
    return false;
  }
  printConfigJson(f);
  f.close();
  return true;
}

void loadConfig() {
  setDefaultConfig(slots);
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println("[音] 設定がないので、電子音の設定で動きます");
    return;
  }
  String json = f.readString();
  f.close();
  const char *err = nullptr;
  bool through;
  if (parseConfig(json.c_str(), tmpCfg, through, err)) {
    for (int i = 0; i < NUM_SLOTS; i++) slots[i] = tmpCfg[i];
    bgmThrough = through;
    Serial.printf("[音] 設定を読み込みました（BGMの流し方：%s）\n", bgmThrough ? "プレイ中も流す" : "場面ごと");
  } else {
    Serial.printf("[音] 設定を読めないので、電子音の設定で動きます（%s）\n", err);
  }
}

// ===================================================================
// 鳴らす
// ===================================================================

// 今BGMとして鳴っているのがどの場面の音か（NUM_SLOTS＝なし、または試聴）
Slot bgmSlot = NUM_SLOTS;

void stopBgm() {
  bgm.stop();
  bgmMp3.stop();
  bgmWav.stop();
  bgmSlot = NUM_SLOTS;
}

// 鳴っているBGMを止める。一時停止中の待機BGM（回転中BGMのあとに続きから鳴らす）は残す
void stopBgmPlaying() {
  bgm.stop();
  if (!bgmMp3.suspended()) bgmMp3.stop();
  if (!bgmWav.suspended()) bgmWav.stop();
  bgmSlot = NUM_SLOTS;
}

void stopSfx() {
  sfx.stop();
  sfxWav.stop();
}

void stopAllSound() {
  stopBgm();
  stopSfx();
}

void soundSetMuted(bool m) {
  muted = m;
  if (m) stopAllSound();
}

bool playCand(Slot s, const Cand &c, float pitch, bool log) {
  const SlotInfo &info = SLOT_INFO[s];
  if (info.bgm) {
    // 一時停止中の待機BGMと同じチャンネル（どちらもMP3、どちらもWAV）を使うときは、play()がそれを止める
    stopBgmPlaying();
    bgmSlot = s;
  } else {
    stopSfx();
  }
  switch (c.type) {
    case CAND_CHIP: {
      const Song &song = CHIP_SONG[s];
      if (!song.notes) return false;
      noInterrupts();  // 電子音を作る割り込みと取り合わないように
      if (info.bgm) {
        bgmAmp = BGM_VOLUME * c.vol / 100;
        bgm.play(song.notes, song.len, true, pitch);
      } else {
        sfxAmp = SFX_VOLUME * c.vol / 100;
        sfx.play(song.notes, song.len, false, pitch);
      }
      interrupts();
      if (log) Serial.printf("[音] %s：電子音（%u%%）\n", info.name, c.vol);
      return true;
    }
    case CAND_FILE: {
      if (const char *why = fileProblem(s, c.file)) {
        Serial.printf("[音] %s：%s … %s\n", info.name, c.file, why);
        return false;
      }
      float gain = FILE_MAX_GAIN * c.vol / 100;
      bool ok;
      if (!info.bgm) ok = sfxWav.play(c.file, gain, false);
      else if (hasExt(c.file, ".mp3")) ok = bgmMp3.play(c.file, gain, true);
      else ok = bgmWav.play(c.file, gain, true);
      if (log || !ok) Serial.printf("[音] %s：%s（%u%%）%s\n", info.name, c.file, c.vol, ok ? "" : " … ファイルがありません");
      return ok;
    }
    default:
      if (log) Serial.printf("[音] %s：無音\n", info.name);
      return true;
  }
}

void playSlot(Slot s, float pitch) {
  if (muted) return;
  SlotCfg &cfg = slots[s];
  if (cfg.n == 0) {
    if (SLOT_INFO[s].bgm) stopBgm();
    else stopSfx();
    return;
  }
  // 2つ以上あれば、前回と違うものをランダムに選ぶ
  int pick = 0;
  if (cfg.n > 1) {
    pick = random(cfg.n - (cfg.last >= 0 ? 1 : 0));
    if (cfg.last >= 0 && pick >= cfg.last) pick++;
  }
  cfg.last = pick;
  playCand(s, cfg.c[pick], pitch, s != SL_TICK);  // ピッは回数が多いのでログに出さない
}

bool previewCand(Slot s, const Cand &c) {
  if (muted) return false;
  bool ok = playCand(s, c, 1.0f, true);
  if (SLOT_INFO[s].bgm) bgmSlot = NUM_SLOTS;  // 試聴は場面の音ではない（場面が変わったら鳴らし直す）
  return ok;
}

// 回転中BGMに切り替える前に、待機BGMを一時停止しておく
void holdIdleMusic() {
  if (bgmSlot != SL_IDLE) return;
  bgmMp3.suspend();
  bgmWav.suspend();
  bgmSlot = NUM_SLOTS;
}

// 一時停止しておいた待機BGMを、続きから鳴らす。なければfalse
bool resumeIdleMusic() {
  if (!bgmMp3.suspended() && !bgmWav.suspended()) return false;
  stopBgmPlaying();  // 回転中BGMを止める
  bgmMp3.resume();
  bgmWav.resume();
  bgmSlot = SL_IDLE;
  Serial.println("[音] 待機BGM：続きから");
  return true;
}

void sceneMusic(Music m) {
  if (muted) return;
  if (m == MUSIC_SPIN) {
    if (bgmThrough) {
      if (slots[SL_SPIN].n == 0) return;  // 回転中BGMがなければ、待機BGMのまま
      holdIdleMusic();
    }
    playSlot(SL_SPIN);
    return;
  }
  if (m == MUSIC_PLAY && !bgmThrough) {
    stopBgm();
    return;
  }
  if (bgmSlot == SL_IDLE) return;  // 待機BGMが鳴り続けている
  if (resumeIdleMusic()) return;
  playSlot(SL_IDLE);
}

// ===================================================================

void soundBegin() {
  randomSeed(rp2040.hwrand32());

  if (!LittleFS.begin()) Serial.println("[音] LittleFSを使えません（MP3は鳴りません）");
  LittleFS.mkdir("/snd");
  loadConfig();

  i2s.setBCLK(I2S_BCK_PIN);
  i2s.setDATA(I2S_DIN_PIN);
  chipLeg = mixer.add();
  AudioOutputBase *bgmMp3Leg = mixer.add();
  AudioOutputBase *bgmWavLeg = mixer.add();
  AudioOutputBase *sfxLeg = mixer.add();
  // 電子音のバッファは小さめ（約23ms）。大きいと、鳴らしてから音が出るまでが遅れる
  chipLeg->setBuffers(4, 256);
  chipLeg->begin();
  fillChip();
  chipIrq = user_irq_claim_unused(true);
  irq_set_exclusive_handler(chipIrq, chipIrqHandler);
  irq_set_priority(chipIrq, 0x80);
  irq_set_enabled(chipIrq, true);
  chipLeg->onTransmit(chipRequest, nullptr);
  mixer.begin();
  // BGMは長く鳴るので先読みを多めにする（8フレーム＝約0.2秒、RAM約37KB）
  bgmMp3.begin(bgmMp3Leg, 8);
  bgmWav.begin(bgmWavLeg, 6);  // WAVの1フレームは約12ms・2KB
  sfxWav.begin(sfxLeg, 6);
}

void soundUpdate() {
  bgmMp3.update();
  bgmWav.update();
  sfxWav.update();
  if (i2s.getUnderflow()) i2sUnderflows++;
  if (chipLeg->getUnderflow()) chipGaps++;
  if (bgmMp3.leg().getUnderflow() | bgmWav.leg().getUnderflow()) bgmGaps++;
  if (sfxWav.leg().getUnderflow()) sfxGaps++;
}

// MP3の1フレーム（約26ms分）のデコードにかかった時間
void printDecodeTime(DecoderLeg &leg) {
  noInterrupts();
  uint32_t maxUs = leg.maxUs, total = leg.totalUs, n = leg.count;
  leg.maxUs = leg.totalUs = leg.count = 0;
  interrupts();
  if (n == 0) return;
  // printfは小数を出せないので、0.1ms単位の整数にして出す
  uint32_t avg = total / n / 100, mx = maxUs / 100;
  Serial.printf("  MP3の1フレーム（26ms分）のデコード 平均 %lu.%lums・最大 %lu.%lums（%luフレーム）\n",
                (unsigned long)(avg / 10), (unsigned long)(avg % 10), (unsigned long)(mx / 10),
                (unsigned long)(mx % 10), (unsigned long)n);
}

void printSoundStats() {
  static uint32_t lastMp3 = 0;
  uint32_t m = bgmMp3.underflows();
  Serial.printf("音の負荷（前回の ? から） CPU %luMHz、空きメモリ %luKB\n", (unsigned long)(rp2040.f_cpu() / 1000000),
                (unsigned long)(rp2040.getFreeHeap() / 1024));
  Serial.printf("  音切れ（I2S） %lu回、無音が挟まった回数 電子音 %lu回・BGM %lu回・効果音 %lu回、MP3のデータ切れ %lu回\n",
                (unsigned long)i2sUnderflows, (unsigned long)chipGaps, (unsigned long)bgmGaps, (unsigned long)sfxGaps,
                (unsigned long)(m - lastMp3));
  printDecodeTime(bgmMp3.leg());
  i2sUnderflows = chipGaps = bgmGaps = sfxGaps = 0;
  lastMp3 = m;
}
