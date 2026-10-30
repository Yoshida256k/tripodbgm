// トライポッド爆音光化：本番用プログラム（音と光と監視モニター）
// PLCのY出力5本（フォトカプラ経由、GP18〜GP22）からゲームの場面を判定し、場面ごとに電子音を鳴らし、
// WS2812B LEDテープを光らせ、LCD1602に監視情報を表示する。
// 当たりの後は、落下検知のタクトスイッチ2個（GP12・GP13）で景品の獲得を判定する。
// 基板上のWS2812B（GP23）はステータス表示、タッチセンサ2個（GP6・GP7）は係員用の操作に使う。
// LEDとLCDはもう片方のコア（core1）で動かすので、音と入力の処理を邪魔しない。
// 判定のしかたは「トライポッド既存構成_分析レポート.md」の4.4・5章を参照。
// シリアル（115200）から疑似信号を作るシミュレーション機能つき（? で使い方を表示）。
// 場面ごとの音（電子音・MP3・無音）は sound.cpp、GUI（web/）とのやりとりは link.cpp。

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "sound.h"
#include "link.h"

// ---- ピン割り当て（I2SのGP26〜28は sound.cpp） ----
const uint8_t LED_DATA_PIN = 15;
const uint8_t LCD_SDA_PIN = 16;
const uint8_t LCD_SCL_PIN = 17;
const uint8_t STATUS_LED_PIN = 23;  // YD-RP2040の基板上のWS2812B（裏の「RGB」ジャンパがつながっていること）

// 係員用のタッチセンサ（TTP223、触るとHIGH）
// タッチ1を短く：警告の確認　タッチ2を短く：デモ演出　2つ同時に長押し：記録リセット
const int NUM_TOUCH = 2;
const uint8_t TOUCH_PIN[NUM_TOUCH] = {6, 7};

// 第1段階の5本。フォトカプラ経由なので、YがONのときGPIOはLOW
enum Channel { CH_Y010, CH_Y004, CH_Y005, CH_Y007, CH_Y003, NUM_CH };
const uint8_t CH_PIN[NUM_CH] = {18, 19, 20, 21, 22};
const char *CH_NAME[NUM_CH] = {"Y010", "Y004", "Y005", "Y007", "Y003"};

// 落下検知のスイッチ（外付けプルアップ＋内部プルアップ）。どちらが押されても「落下」（OR）
// 手持ちの都合で接点の種類が違う。a接点は押すとLOW、b接点（常時閉）は押すとHIGH（離しているときLOW）
const int NUM_DROP = 2;
const uint8_t DROP_PIN[NUM_DROP] = {12, 13};
const bool DROP_NC[NUM_DROP] = {false, true};  // true＝b接点（GP12はa接点、GP13はb接点）

// 端子の状態（HIGHか）から、スイッチが押されているか
inline bool dropPressed(int i, bool high) {
  return DROP_NC[i] ? high : !high;
}

// ---- 判定のしきい値（実測に合わせて調整する） ----
const uint32_t DEBOUNCE_MS = 3;       // 同じ値がこの時間続いたら確定（リレー接点のチャタリング対策）
const uint32_t STOP_MS = 85;          // ランプがこれ以上点きっぱなしなら停止（回転中は約67ms、当たりの点滅は約100ms）
const uint32_t LOSE_TIMEOUT_MS = 300; // 回転中にこれだけ監視ランプが変化しなければ、監視外のマスで停止（ハズレ）
const uint32_t DROP_DEBOUNCE_MS = 20;    // 落下スイッチのチャタリング対策
const uint32_t DROP_LOCKOUT_MS = 2000;   // 1回反応したら、景品が跳ねても反応しないようにこの時間は無視
const uint32_t DROP_WINDOW_MS = 15000;   // 当たりからこの時間だけ落下を受け付ける。過ぎたら「景品未検知」

// ---- 光 ----
const uint16_t LED_COUNT = 57;         // テスト用のテープ（約40cm）。本番の150灯にするときはここを変える
const uint8_t LED_BRIGHTNESS = 40;    // 0〜255。150灯では電源容量に合わせて下げる
const uint32_t FRAME_MS = 20;          // 50コマ/秒
// テープ全体の一斉フラッシュの間隔。光過敏の来場者に配慮して、1秒に3回以下（333ms以上）にする
const uint32_t FLASH_PERIOD_MS = 400;
const uint8_t STATUS_BRIGHTNESS = 40;  // 基板上のステータスLEDの明るさ（0〜255）

// ---- 係員の操作 ----
const uint32_t TOUCH_DEBOUNCE_MS = 30;
const uint32_t TOUCH_LONG_MS = 2000;   // 2つ同時にこの時間触り続けると記録リセット

// ===================================================================
// 入力（本物のPLC入力、またはシミュレーション）
// ===================================================================

bool simActive = false;
enum SimLamps { SIM_DARK, SIM_SPIN, SIM_BLINK, SIM_STEADY_Y004 };
bool simY010 = false;
SimLamps simLamps = SIM_DARK;
Channel simBlinkCh = CH_Y003;
uint32_t simSince = 0;
uint32_t simDropUntil = 0;  // この時刻まで落下スイッチ1を押していることにする

// ---- 入力の記録（タイマー割り込み、1msごと） ----
// MP3のデコードは割り込みで動くので、loop()がときどき数十ms遅れる。
// 遅れても判定がずれないよう、端子の状態は割り込みで「読んだ時刻」と一緒に記録し、
// loop()でまとめて取り出して、その時刻で判定する
struct PinSample {
  uint32_t ms;
  uint32_t gpio;  // 全端子の状態（bit n = GPn）
};
const uint32_t SAMPLE_RING = 512;  // 約0.5秒分。loop()がこれ以上遅れると古い分を捨てる
volatile PinSample sampleRing[SAMPLE_RING];
volatile uint32_t sampleHead = 0;
uint32_t sampleTail = 0;
uint32_t curGpio = 0;              // いま判定に使っている記録の端子の状態
repeating_timer_t sampleTimer;
volatile uint32_t maxLagMs = 0;    // 記録してからloop()で判定するまでの最大の遅れ（?で表示）

bool sampleInputs(repeating_timer_t *) {
  uint32_t h = sampleHead;
  sampleRing[h % SAMPLE_RING].ms = millis();
  sampleRing[h % SAMPLE_RING].gpio = gpio_get_all();
  sampleHead = h + 1;
  return true;
}

inline bool pinHigh(uint8_t pin) {
  return (curGpio >> pin) & 1;
}

// 生の値（true = YがON）
bool readRaw(Channel ch, uint32_t now) {
  if (!simActive) return !pinHigh(CH_PIN[ch]);
  if (ch == CH_Y010) return simY010;
  switch (simLamps) {
    case SIM_SPIN: {
      // 67msごとにM0→M7。M0=Y004、M1=Y005、M3=Y007、M7=Y003、ほかは監視外
      uint32_t pos = (now - simSince) / 67 % 8;
      return (ch == CH_Y004 && pos == 0) || (ch == CH_Y005 && pos == 1) ||
             (ch == CH_Y007 && pos == 3) || (ch == CH_Y003 && pos == 7);
    }
    case SIM_BLINK:  // 100ms消灯 → 100ms点灯 を繰り返す
      return ch == simBlinkCh && (now - simSince) / 100 % 2 == 1;
    case SIM_STEADY_Y004:
      return ch == CH_Y004;
    default:
      return false;
  }
}

struct Input {
  bool on = false;          // 確定した値
  bool raw = false;
  uint32_t rawSince = 0;    // 生の値が今の値になった時刻
  uint32_t onSince = 0;     // 確定した値がONになった時刻
};
Input inputs[NUM_CH];

// ===================================================================
// 場面の判定
// ===================================================================

enum State { ST_IDLE, ST_STANDBY, ST_SPIN, ST_WIN, ST_LOSE };
// core1（LED）からも読むのでvolatile
volatile State state = ST_IDLE;
volatile int winArm = 0;
volatile uint32_t stateSince = 0;  // 今の状態になった時刻
volatile uint32_t tickCount = 0;   // 回転中に監視ランプが点いた回数
volatile uint32_t resetAt = 0;     // 最後にリセットした時刻

// 監視モニター（LCD）用の記録
volatile uint8_t inputBits = 0;    // 確定した入力（bit0=Y010, 1=Y004, 2=Y005, 3=Y007, 4=Y003）
volatile uint32_t plays = 0;       // 止まった回数（当たり＋ハズレ）
volatile uint32_t hits[4] = {};    // 号ごとの当たり回数（[1]〜[3]）
volatile uint32_t lapMs = 0;       // 回転1周の実測（Y004が点く間隔）
volatile uint32_t squareMs = 0;    // 1マスの実測（回転中にランプが点いていた時間）
uint32_t lastY004Rise = 0;
uint32_t lastLampChange = 0;

// 落下検知
struct DropSwitch {
  bool on = false;  // 確定した値（true = 押されている）
  bool raw = false;
  uint32_t rawSince = 0;
};
DropSwitch drops[NUM_DROP];
volatile uint8_t dropBits = 0;      // 押されているスイッチ（bit0=GP12, bit1=GP13）
bool dropWindowOpen = false;        // 落下の受付中か
uint32_t dropWindowFrom = 0;
uint32_t lastDropAt = 0;
volatile uint32_t getAt = 0;        // 最後に景品を獲得した時刻（0 = まだ）
volatile bool prizeMissing = false; // 受付時間内に落ちなかった（係員を呼ぶ表示を出す）
volatile uint32_t prizeGets = 0;         // 獲得回数
volatile uint32_t prizeMisses = 0;       // 未検知回数

// 係員の操作とデモ演出
volatile bool demoActive = false;   // デモ演出中（結果を記録に数えない）
uint32_t demoStart = 0;
size_t demoStep = 0;
volatile uint8_t touchBits = 0;     // 触れているタッチ（bit0=GP6, bit1=GP7）
volatile const char *opMessage = nullptr;  // 係員の操作の結果（LCDに2秒出す）
volatile uint32_t opMessageAt = 0;

void printTime(uint32_t now) {
  Serial.printf("[%lu.%03lus] ", (unsigned long)(now / 1000), (unsigned long)(now % 1000));
}

const char *stateName(State s) {
  switch (s) {
    case ST_IDLE: return "待機";
    case ST_STANDBY: return "スタンバイ";
    case ST_SPIN: return "回転中";
    case ST_WIN: return "当たり";
    case ST_LOSE: return "ハズレ";
  }
  return "?";
}

void printState(State s) {
  if (s == ST_WIN) Serial.printf("当たり(%d号)", winArm);
  else Serial.print(stateName(s));
}

void setState(State next, uint32_t now, const char *reason) {
  printTime(now);
  printState(state);
  Serial.print(" → ");
  printState(next);
  Serial.printf("  （%s）\n", reason);
  state = next;
  stateSince = now;

  switch (next) {
    case ST_IDLE:
      dropWindowOpen = false;
      prizeMissing = false;
      stopSfx();
      sceneMusic(MUSIC_IDLE);
      break;
    case ST_STANDBY:
      sceneMusic(MUSIC_PLAY);
      playSlot(SL_STANDBY);
      break;
    case ST_SPIN:
      prizeMissing = false;
      lastY004Rise = 0;
      stopSfx();
      sceneMusic(MUSIC_SPIN);
      break;
    case ST_WIN: {
      if (!demoActive) {
        plays++;
        hits[winArm]++;
      }
      dropWindowOpen = true;  // 落下の受付を開始
      dropWindowFrom = now;
      prizeMissing = false;
      sceneMusic(MUSIC_PLAY);
      // 電子音のときは、1号は原調、2号は全音、3号は長3度上げる
      const float pitch[] = {1.0f, 1.0f, 1.122f, 1.26f};
      playSlot(SL_WIN, pitch[winArm]);
      break;
    }
    case ST_LOSE:
      if (!demoActive) plays++;
      sceneMusic(MUSIC_PLAY);
      playSlot(SL_LOSE);
      break;
  }
  linkNotifyState(currentSceneId());
}

const char *currentSceneId() {
  switch (state) {
    case ST_IDLE: return "idle";
    case ST_STANDBY: return "standby";
    case ST_SPIN: return "spin";
    case ST_WIN: return winArm == 1 ? "win1" : winArm == 2 ? "win2" : "win3";
    case ST_LOSE: return "lose";
  }
  return "idle";
}

// 試聴やファイルの転送のあと、今の場面のBGMに戻す（効果音は鳴らし直さない）
void restoreSceneBgm() {
  sceneMusic(state == ST_IDLE ? MUSIC_IDLE : state == ST_SPIN ? MUSIC_SPIN : MUSIC_PLAY);
}

// 1msごとに呼ぶ
void updateInputs(uint32_t now) {
  for (int ch = 0; ch < NUM_CH; ch++) {
    Input &in = inputs[ch];
    bool raw = readRaw((Channel)ch, now);
    if (raw != in.raw) {
      in.raw = raw;
      in.rawSince = now;
    }
    if (in.raw == in.on || now - in.rawSince < DEBOUNCE_MS) continue;

    // 値が確定して変わった
    in.on = in.raw;
    uint32_t onMs = now - in.onSince;
    if (in.on) in.onSince = now;

    if (ch == CH_Y010) {
      if (in.on && state == ST_IDLE) {
        setState(ST_STANDBY, now, "Y010 ON");
      } else if (!in.on && state != ST_IDLE) {
        setState(ST_IDLE, now, "Y010 OFF＝リセット");
        playSlot(SL_RESET);
        resetAt = now;
      }
      continue;
    }

    // ランプ（Y004/Y005/Y007/Y003）
    lastLampChange = now;
    if (!inputs[CH_Y010].on) continue;
    if (in.on) {
      if (state == ST_SPIN) {
        playSlot(SL_TICK);
        tickCount++;
        if (ch == CH_Y004) {
          if (lastY004Rise) lapMs = now - lastY004Rise;
          lastY004Rise = now;
        }
      }
    } else if (onMs < STOP_MS) {
      squareMs = onMs;
      if (state == ST_SPIN) continue;
      char reason[40];
      snprintf(reason, sizeof(reason), "%sが%lums点灯", CH_NAME[ch], (unsigned long)onMs);
      setState(ST_SPIN, now, reason);
    }
  }

  uint8_t bits = 0;
  for (int ch = 0; ch < NUM_CH; ch++) bits |= inputs[ch].on << ch;
  inputBits = bits;

  if (state != ST_SPIN) return;

  // 停止の判定
  for (int ch = CH_Y004; ch < NUM_CH; ch++) {
    const Input &in = inputs[ch];
    if (!in.on || now - in.onSince < STOP_MS) continue;
    char reason[40];
    snprintf(reason, sizeof(reason), "%sが%lums以上点灯", CH_NAME[ch], (unsigned long)STOP_MS);
    if (ch == CH_Y004) {
      setState(ST_LOSE, now, reason);
    } else {
      winArm = ch == CH_Y003 ? 1 : ch == CH_Y005 ? 2 : 3;
      setState(ST_WIN, now, reason);
    }
    return;
  }
  if (now - lastLampChange >= LOSE_TIMEOUT_MS) {
    setState(ST_LOSE, now, "監視外のマスで停止");
  }
}

// w コマンド：落下スイッチの端子の状態を定期的に表示する（配線の確認用）
bool dropWatch = false;

void printDropWatch(uint32_t now) {
  static uint32_t last = 0;
  if (!dropWatch || now - last < 250) return;
  last = now;
  printTime(now);
  for (int i = 0; i < NUM_DROP; i++) {
    bool high = digitalRead(DROP_PIN[i]);
    Serial.printf("GP%d(%s接点)=%s（%s） ", DROP_PIN[i], DROP_NC[i] ? "b" : "a", high ? "HIGH" : "LOW",
                  dropPressed(i, high) ? "押" : "離");
  }
  Serial.println();
}

// 落下スイッチを読んで、景品の獲得を判定する。1msごとに呼ぶ
void updateDrop(uint32_t now) {
  int pressed = -1;
  uint8_t bits = 0;
  for (int i = 0; i < NUM_DROP; i++) {
    DropSwitch &sw = drops[i];
    // 実物のスイッチはシミュレーション中も読む（d の疑似入力はスイッチ1に重ねる）
    bool raw = dropPressed(i, pinHigh(DROP_PIN[i])) || (simActive && i == 0 && (int32_t)(simDropUntil - now) > 0);
    if (raw != sw.raw) {
      sw.raw = raw;
      sw.rawSince = now;
    }
    if (sw.raw != sw.on && now - sw.rawSince >= DROP_DEBOUNCE_MS) {
      sw.on = sw.raw;
      if (sw.on) pressed = i;
      // 反応したかPCで分かるよう、押した・離したを必ず出す
      printTime(now);
      Serial.printf("落下スイッチ%d（GP%d）%s\n", i + 1, DROP_PIN[i], sw.on ? "ON（押された）" : "OFF（離された）");
    }
    bits |= sw.on << i;
  }
  dropBits = bits;

  if (dropWindowOpen && now - dropWindowFrom >= DROP_WINDOW_MS) {
    dropWindowOpen = false;
    prizeMissing = true;
    if (!demoActive) prizeMisses++;
    printTime(now);
    Serial.printf("景品未検知（当たりから%lu秒たっても落下スイッチが押されない。係員の確認が必要）\n",
                  (unsigned long)(DROP_WINDOW_MS / 1000));
  }

  if (pressed < 0) return;
  if (lastDropAt && now - lastDropAt < DROP_LOCKOUT_MS) {  // 景品が跳ねた分は無視
    printTime(now);
    Serial.printf("落下スイッチ%d：前の反応から%lums以内のため無視\n", pressed + 1, (unsigned long)DROP_LOCKOUT_MS);
    return;
  }
  lastDropAt = now;
  printTime(now);
  if (!dropWindowOpen) {
    Serial.printf("落下スイッチ%d：受付外のため無視\n", pressed + 1);
    return;
  }
  dropWindowOpen = false;
  prizeMissing = false;
  getAt = now;
  if (!demoActive) prizeGets++;
  playSlot(SL_GET);
  Serial.printf("景品獲得（落下スイッチ%d、当たりから%lu.%lu秒）\n", pressed + 1,
                (unsigned long)((now - dropWindowFrom) / 1000), (unsigned long)((now - dropWindowFrom) % 1000 / 100));
}

// ===================================================================
// シリアル（シミュレーションのコマンド）
// ===================================================================

void printHelp() {
  Serial.println();
  Serial.println("===== トライポッド 本番プログラム（音・光・LCD） =====");
  Serial.printf("入力: %s\n", demoActive ? "デモ演出" : simActive ? "シミュレーション" : "PLC（GP18〜GP22）");
  Serial.print("状態: ");
  printState(state);
  Serial.print("\n信号: ");
  for (int ch = 0; ch < NUM_CH; ch++) Serial.printf("%s=%s ", CH_NAME[ch], inputs[ch].on ? "ON" : "off");
  Serial.printf("\n落下スイッチ: GP12(a接点)=%s GP13(b接点)=%s（端子 %s / %s）  受付: %s\n", drops[0].on ? "ON" : "off",
                drops[1].on ? "ON" : "off", digitalRead(DROP_PIN[0]) ? "HIGH" : "LOW",
                digitalRead(DROP_PIN[1]) ? "HIGH" : "LOW", dropWindowOpen ? "受付中" : "受付外");
  Serial.println("確認用: w: 落下スイッチの端子の状態を0.25秒ごとに表示（もう一度 w で止める）");
  Serial.println("シミュレーション:");
  Serial.println(" s: スタート（Y010 ON）     g: 回転");
  Serial.println(" 1/2/3: 当たり1/2/3号で停止  x: ハズレ（Y004で停止）  z: ハズレ（監視外のマスで停止）");
  Serial.println(" d: 落下スイッチ1を押す（当たりの後なら景品獲得）");
  Serial.println(" r: リセット（Y010 OFF）    p: PLCの入力に戻す     ?: この表示");
  Serial.println("係員の操作（タッチセンサと同じ）:");
  Serial.println(" a: 警告の確認（タッチ1）   e: デモ演出（タッチ2）   c: 記録リセット（2つ同時に2秒）");
  Serial.println("場面ごとの音（電子音・MP3）は、GUI（トライポッド サウンド設定）から変えられます");
  printSoundStats();
  Serial.printf("判定の最大の遅れ: %lums（前回の ? から）\n", (unsigned long)maxLagMs);
  maxLagMs = 0;
}

// シミュレーションの疑似信号を切り替える（シリアルのコマンドとデモ演出から使う）
void simCommand(char c, uint32_t now) {
  if (!simActive) {
    simActive = true;
    simY010 = false;
    simLamps = SIM_DARK;
    Serial.println("[シミュ] 開始。PLCの入力の代わりに疑似信号を使います（p で戻る）");
  }
  if (c == 'd') {
    simDropUntil = now + 100;
    Serial.println("[シミュ] 落下スイッチ1を押す");
    return;
  }
  simSince = now;
  switch (c) {
    case 's': simY010 = true; simLamps = SIM_DARK; Serial.println("[シミュ] スタート"); break;
    case 'g': simY010 = true; simLamps = SIM_SPIN; Serial.println("[シミュ] 回転"); break;
    case '1': simLamps = SIM_BLINK; simBlinkCh = CH_Y003; Serial.println("[シミュ] 当たり1号で停止"); break;
    case '2': simLamps = SIM_BLINK; simBlinkCh = CH_Y005; Serial.println("[シミュ] 当たり2号で停止"); break;
    case '3': simLamps = SIM_BLINK; simBlinkCh = CH_Y007; Serial.println("[シミュ] 当たり3号で停止"); break;
    case 'x': simLamps = SIM_STEADY_Y004; Serial.println("[シミュ] ハズレ（Y004）で停止"); break;
    case 'z': simLamps = SIM_DARK; Serial.println("[シミュ] ハズレ（監視外）で停止"); break;
    case 'r': simY010 = false; simLamps = SIM_DARK; Serial.println("[シミュ] リセット"); break;
  }
}

// ===================================================================
// 係員の操作（タッチセンサ、またはシリアルの a / e / c）
// ===================================================================

void showOpMessage(const char *msg, uint32_t now) {
  opMessage = msg;
  opMessageAt = now;
}

// 景品未検知の警告を消す
void acknowledgeAlert(uint32_t now) {
  printTime(now);
  if (prizeMissing) {
    prizeMissing = false;
    Serial.println("[係員] 景品未検知の警告を確認しました");
    showOpMessage("ｹｲｺｸ ｶｸﾆﾝ", now);
  } else {
    Serial.println("[係員] 確認する警告はありません");
    showOpMessage("ｹｲｺｸ ﾅｼ", now);
  }
}

// プレイ回数などの記録を0に戻す
void clearStats(uint32_t now) {
  plays = 0;
  for (int i = 0; i < 4; i++) hits[i] = 0;
  prizeGets = 0;
  prizeMisses = 0;
  printTime(now);
  Serial.println("[係員] 記録を0に戻しました");
  showOpMessage("ｷﾛｸ ﾘｾｯﾄ", now);
}

// デモ演出：シミュレーションの疑似信号を決まった時間で流す
struct DemoStep {
  uint32_t at;  // 開始からの時間（ms）
  char cmd;     // シミュレーションのコマンド
};
const DemoStep DEMO[] = {{0, 's'}, {1500, 'g'}, {5000, '2'}, {7000, 'd'}, {11000, 'r'}};
const uint32_t DEMO_END_MS = 12500;

void startDemo(uint32_t now) {
  printTime(now);
  if (state != ST_IDLE || simActive) {
    Serial.println("[係員] デモ演出は待機中（シミュレーション中でないとき）だけ動きます");
    showOpMessage("ﾃﾞﾓﾊ ﾀｲｷﾁｭｳﾉﾐ", now);
    return;
  }
  Serial.println("[係員] デモ演出を開始（本物のPLCが動き出したら中止します）");
  showOpMessage("ﾃﾞﾓ ｶｲｼ", now);
  demoActive = true;
  demoStart = now;
  demoStep = 0;
}

void stopDemo(uint32_t now, const char *reason) {
  demoActive = false;
  simActive = false;
  simY010 = false;
  simLamps = SIM_DARK;
  printTime(now);
  Serial.printf("[係員] デモ演出を終了（%s）。PLCの入力に戻します\n", reason);
  // 判定をやり直すため、いったん待機に戻す。PLCが動いていれば、すぐにスタンバイになる
  inputs[CH_Y010].on = false;
  if (state != ST_IDLE) setState(ST_IDLE, now, "デモ終了");
}

// 1msごとに呼ぶ
void updateDemo(uint32_t now) {
  if (!demoActive) return;
  // 本物のPLCが動き出したら（Y010の端子がLOW）すぐにやめる
  if (!pinHigh(CH_PIN[CH_Y010])) {
    stopDemo(now, "PLCが動き出したため中止");
    return;
  }
  uint32_t t = now - demoStart;
  while (demoStep < sizeof(DEMO) / sizeof(DEMO[0]) && t >= DEMO[demoStep].at) {
    simCommand(DEMO[demoStep].cmd, now);
    demoStep++;
  }
  if (t >= DEMO_END_MS) stopDemo(now, "最後まで流した");
}

// タッチセンサ。触ってから離したときに短い操作を、2つ同時に長く触ったときに記録リセットをする
struct TouchSwitch {
  bool on = false;
  bool raw = false;
  uint32_t rawSince = 0;
  uint32_t onSince = 0;
  bool withOther = false;  // 触っている間にもう片方も触れた（短い操作はしない）
  bool longDone = false;   // 長押しの操作をした
};
TouchSwitch touches[NUM_TOUCH];

// 1msごとに呼ぶ
void updateTouch(uint32_t now) {
  uint8_t bits = 0;
  for (int i = 0; i < NUM_TOUCH; i++) {
    TouchSwitch &t = touches[i];
    bool raw = pinHigh(TOUCH_PIN[i]);
    if (raw != t.raw) {
      t.raw = raw;
      t.rawSince = now;
    }
    if (t.raw != t.on && now - t.rawSince >= TOUCH_DEBOUNCE_MS) {
      t.on = t.raw;
      printTime(now);
      Serial.printf("タッチ%d（GP%d）%s\n", i + 1, TOUCH_PIN[i], t.on ? "ON" : "OFF");
      if (t.on) {
        t.onSince = now;
        t.withOther = false;
        t.longDone = false;
      } else if (!t.withOther && !t.longDone) {
        if (i == 0) acknowledgeAlert(now);
        else startDemo(now);
      }
    }
    bits |= t.on << i;
  }
  touchBits = bits;

  if (touches[0].on && touches[1].on) {
    touches[0].withOther = touches[1].withOther = true;
    uint32_t since = max(touches[0].onSince, touches[1].onSince);
    if (!touches[0].longDone && now - since >= TOUCH_LONG_MS) {
      touches[0].longDone = touches[1].longDone = true;
      clearStats(now);
    }
  }
}

void handleSerial(uint32_t now) {
  while (Serial.available() && !linkUploading()) {  // 転送中のデータは link が読む
    char c = Serial.read();
    if (linkFeedChar(c)) continue;  // 「#」で始まる行はGUIのコマンド
    if (c == '?') { printHelp(); continue; }
    if (c == 'a') { acknowledgeAlert(now); continue; }
    if (c == 'e') { startDemo(now); continue; }
    if (c == 'c') { clearStats(now); continue; }
    if (c == 'w') {
      dropWatch = !dropWatch;
      Serial.printf("[確認] 落下スイッチの端子の表示を%s\n", dropWatch ? "開始（w で止める）" : "停止");
      continue;
    }
    if (c == 'p') {
      if (demoActive) {
        stopDemo(now, "p で中止");
        continue;
      }
      simActive = false;
      Serial.println("[シミュ] 終了。PLCの入力を読みます");
      continue;
    }
    if (strchr("sg123xzrd", c)) simCommand(c, now);  // 改行などは無視
  }
}

// ===================================================================
// 光（WS2812B）。core1で動かす。ドパミン全開の演出
// ===================================================================

Adafruit_NeoPixel strip(LED_COUNT, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);
uint8_t sparkle[LED_COUNT];  // キラキラ（白を足す量）。毎コマ減衰する
uint32_t lastTickSeen = 0;

uint32_t hsv(uint16_t hue, uint8_t sat, uint8_t val) {
  return strip.gamma32(strip.ColorHSV(hue, sat, val));
}

// 色に白を足す
uint32_t addWhite(uint32_t c, uint8_t w) {
  uint16_t r = ((c >> 16) & 0xff) + w, g = ((c >> 8) & 0xff) + w, b = (c & 0xff) + w;
  return strip.Color(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

void spawnSparkles(int count) {
  for (int k = 0; k < count; k++) sparkle[random(LED_COUNT)] = 255;
}

// 当たりの号ごとの色（1号 赤、2号 マゼンタ、3号 金）。既存のルーレットの緑・水色と区別できる色
uint16_t armHue(int arm) {
  return arm == 1 ? 0 : arm == 2 ? 54613 : 7000;
}

void renderFrame(uint32_t now) {
  const int n = LED_COUNT;
  State s = state;
  uint32_t t = now - stateSince;

  for (int i = 0; i < n; i++) sparkle[i] = sparkle[i] * 3 / 4;

  switch (s) {
    case ST_IDLE:
      if (resetAt && now - resetAt < 600) {
        // リセット：白が端から消えていく
        int lit = n - (int)((now - resetAt) * n / 600);
        for (int i = 0; i < n; i++) strip.setPixelColor(i, i < lit ? strip.Color(120, 120, 120) : 0);
      } else {
        // 待機：ゆっくり流れる虹＋ときどきキラッ
        for (int i = 0; i < n; i++) strip.setPixelColor(i, hsv(i * 65536 / n + now * 15, 255, 90));
        if (random(8) == 0) spawnSparkles(1);
      }
      break;

    case ST_STANDBY: {
      // スタンバイ：中央から金の輪が広がる → 白の呼吸＋虹色のキラキラが増えていく
      float breath = (1 - cosf(now / 250.0f)) / 2;
      for (int i = 0; i < n; i++) {
        uint32_t c = hsv(0, 0, 40 + (uint8_t)(100 * breath));
        if (t < 400) {
          float r = t / 400.0f * n / 2, d = fabsf(i - (n - 1) / 2.0f);
          if (fabsf(d - r) < 1.5f) c = hsv(7000, 200, 255);
        }
        strip.setPixelColor(i, c);
      }
      spawnSparkles(1 + t / 1000);
      break;
    }

    case ST_SPIN: {
      // 回転中：高速で流れる虹＋白い彗星＋ランプが点くたびにキラキラが弾ける
      int head = (now / 15) % n;
      for (int i = 0; i < n; i++) {
        uint32_t c = hsv(i * 65536 * 2 / n + now * 500, 255, 200);
        int back = (head - i + n) % n;  // 彗星の頭からの距離
        if (back < 5) c = addWhite(c, 255 - back * 50);
        strip.setPixelColor(i, c);
      }
      uint32_t ticks = tickCount;
      if (ticks != lastTickSeen) {
        lastTickSeen = ticks;
        spawnSparkles(4);
      }
      break;
    }

    case ST_WIN: {
      uint16_t armH = armHue(winArm);
      if (t < 400) {
        // 当たり：中央から大爆発
        float r = t / 400.0f * n / 2;
        for (int i = 0; i < n; i++) {
          float d = fabsf(i - (n - 1) / 2.0f);
          uint32_t c = d < r ? hsv(armH, 255, 255) : 0;
          if (fabsf(d - r) < 2) c = strip.Color(255, 255, 255);
          strip.setPixelColor(i, c);
        }
      } else {
        // そのあと：超高速の虹が往復＋号の色で一斉フラッシュ＋キラキラ大量
        bool flash = (t % FLASH_PERIOD_MS) < 80;
        for (int i = 0; i < n; i++) {
          int dir = (t / 1000) % 2 ? 1 : -1;
          uint32_t c = flash ? hsv(armH, 255, 255) : hsv(dir * (int)(i * 65536 * 3 / n) + now * 900, 255, 230);
          strip.setPixelColor(i, c);
        }
        spawnSparkles(3);
      }
      break;
    }

    case ST_LOSE:
      if (t < 300) {
        // ハズレ：赤で1回ドン
        uint8_t v = 255 - t * 200 / 300;
        for (int i = 0; i < n; i++) strip.setPixelColor(i, hsv(0, 255, v));
      } else if (t < 1500) {
        // 赤が末端から抜け落ちていく
        int lit = n - (int)((t - 300) * n / 1200);
        for (int i = 0; i < n; i++) strip.setPixelColor(i, i < lit ? hsv(0, 255, 60) : 0);
      } else {
        // 暗い紫でしょんぼり呼吸
        float breath = (1 - cosf(now / 600.0f)) / 2;
        for (int i = 0; i < n; i++) strip.setPixelColor(i, hsv(50000, 255, 8 + (uint8_t)(30 * breath)));
      }
      break;
  }

  // 景品獲得：3秒間、金色の演出を上に重ねる
  uint32_t got = getAt;
  if (got && now - got < 3000) {
    uint32_t tg = now - got;
    if (tg < 500) {
      // 中央から金の輪が広がる（輪の内側は金）
      float r = tg / 500.0f * n / 2;
      for (int i = 0; i < n; i++) {
        float d = fabsf(i - (n - 1) / 2.0f);
        if (fabsf(d - r) < 2) strip.setPixelColor(i, strip.Color(255, 255, 255));
        else if (d < r) strip.setPixelColor(i, hsv(7000, 230, 255));
      }
    } else {
      // 金色のうねり＋一斉フラッシュ（FLASH_PERIOD_MSごと）＋金のキラキラ大量
      bool flash = (tg % FLASH_PERIOD_MS) < 80;
      for (int i = 0; i < n; i++) {
        uint8_t v = flash ? 255 : 120 + (uint8_t)(100 * (1 + sinf(i * 0.8f + now / 60.0f)) / 2);
        strip.setPixelColor(i, hsv(7000, flash ? 120 : 230, v));
      }
      spawnSparkles(4);
    }
  }

  for (int i = 0; i < n; i++) {
    if (sparkle[i]) strip.setPixelColor(i, addWhite(strip.getPixelColor(i), sparkle[i]));
  }
  strip.show();
}

// ===================================================================
// 監視モニター（LCD1602）。core1で動かす。
// 1行目：場面・入力5本・落下スイッチ（▼）・動作マーク・入力元（P=PLC、S=シミュレーション）
// 2行目：場面が変わった直後はメッセージ、それ以外は監視ページを順に切り替え
// I2Cの書き込みは遅いので、変わった文字だけを少しずつ書き換える
// ===================================================================

LiquidCrystal_I2C *lcd = nullptr;
uint8_t lcdWant[2][16];   // 表示したい内容
uint8_t lcdShown[2][16];  // 実際に表示している内容

// LCDの自作文字（CGRAM 0〜6）
enum { GLYPH_OFF = 0, GLYPH_ON = 1, GLYPH_SPIN = 2, GLYPH_DROP = 6 };  // 2〜5は回るマーク
const uint8_t GLYPHS[7][8] = {
  {0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111, 0},  // ランプ消灯（□）
  {0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0},  // ランプ点灯（■）
  {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0},  // |
  {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000, 0},  // /
  {0, 0, 0, 0b11111, 0, 0, 0, 0},                                      // -
  {0b10000, 0b01000, 0b01000, 0b00100, 0b00010, 0b00010, 0b00001, 0},  // ＼
  {0, 0b11111, 0b11111, 0b01110, 0b01110, 0b00100, 0b00100, 0},        // ▼（落下スイッチが押されている）
};

// UTF-8の文字列をLCDの文字コードに変換して書き込む。半角カタカナ（U+FF61〜FF9F）はLCDの0xA1〜0xDFに対応する
void lcdText(int row, int col, const char *utf8, int width) {
  const uint8_t *p = (const uint8_t *)utf8;
  int x = col;
  while (*p && x < col + width) {
    uint8_t c;
    if (p[0] < 0x80) {
      c = *p++;
    } else if (p[0] == 0xEF && (p[1] == 0xBD || p[1] == 0xBE) && p[2]) {
      uint16_t cp = (p[1] == 0xBD ? 0xFF40 : 0xFF80) + (p[2] & 0x3F);
      c = (cp >= 0xFF61 && cp <= 0xFF9F) ? cp - 0xFEC0 : '?';
      p += 3;
    } else {
      c = '?';
      p++;
      while ((*p & 0xC0) == 0x80) p++;
    }
    lcdWant[row][x++] = c;
  }
  while (x < col + width) lcdWant[row][x++] = ' ';
}

void lcdPrintf(int row, const char *fmt, ...) {
  char buf[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  lcdText(row, 0, buf, 16);
}

void setupLcd() {
  Wire.setSDA(LCD_SDA_PIN);
  Wire.setSCL(LCD_SCL_PIN);
  Wire.begin();
  const uint8_t candidates[] = {0x27, 0x3F};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) continue;
    lcd = new LiquidCrystal_I2C(addr, 16, 2);
    lcd->init();
    lcd->backlight();
    for (int i = 0; i < 7; i++) lcd->createChar(i, (uint8_t *)GLYPHS[i]);
    lcd->clear();
    memset(lcdShown, ' ', sizeof(lcdShown));
    memset(lcdWant, ' ', sizeof(lcdWant));
    return;
  }
}

// 1行目の場面名（半角カタカナ、7文字まで）
const char *stateLabel(State s) {
  switch (s) {
    case ST_IDLE: return "ﾀｲｷ";
    case ST_STANDBY: return "ｽﾀﾝﾊﾞｲ";
    case ST_SPIN: return "ｶｲﾃﾝﾁｭｳ";
    case ST_WIN: return winArm == 1 ? "ｱﾀﾘ1ｺﾞｳ" : winArm == 2 ? "ｱﾀﾘ2ｺﾞｳ" : "ｱﾀﾘ3ｺﾞｳ";
    case ST_LOSE: return "ﾊｽﾞﾚ";
  }
  return "?";
}

// 場面が変わった直後に2行目に出すメッセージ（16文字まで。濁点も1文字）
const char *stateMessage(State s, uint32_t now) {
  uint32_t t = now - stateSince;
  switch (s) {
    case ST_IDLE:
      if (resetAt && now - resetAt < 3000) return "ﾘｾｯﾄ ｶﾝﾘｮｳ";
      if (now < 4000) return "ｼｽﾃﾑ ｷﾄﾞｳ ｶﾝﾘｮｳ";
      return nullptr;
    case ST_STANDBY: return t < 3000 ? "ｼﾞｭﾝﾋﾞ ｶﾝﾘｮｳ" : nullptr;
    case ST_SPIN: return t < 1500 ? "ｶｲﾃﾝ ｶｲｼ" : nullptr;
    case ST_WIN: return t < 4000 ? "ｵﾒﾃﾞﾄｳｺﾞｻﾞｲﾏｽ" : nullptr;
    case ST_LOSE: return t < 3000 ? "ﾊｽﾞﾚ" : nullptr;
  }
  return nullptr;
}

void composeLcd(uint32_t now) {
  State s = state;

  // 1行目：場面（7文字）＋入力5本（Y010 Y004 Y005 Y007 Y003）＋回るマーク＋入力元
  lcdText(0, 0, stateLabel(s), 7);
  lcdWant[0][7] = ' ';
  uint8_t bits = inputBits;
  for (int ch = 0; ch < NUM_CH; ch++) lcdWant[0][8 + ch] = (bits >> ch) & 1 ? GLYPH_ON : GLYPH_OFF;
  lcdWant[0][13] = dropBits ? GLYPH_DROP : ' ';
  uint32_t spinPeriod = s == ST_SPIN ? 80 : s == ST_WIN ? 150 : 500;
  lcdWant[0][14] = GLYPH_SPIN + (now / spinPeriod) % 4;
  lcdWant[0][15] = demoActive ? 'D' : simActive ? 'S' : 'P';

  // 2行目：景品獲得・景品未検知のメッセージが最優先
  uint32_t got = getAt;
  const char *op = (const char *)opMessage;
  const char *msg = op && now - opMessageAt < 2000 ? op
                    : got && now - got < 3000      ? "ｹｲﾋﾝ ｶｸﾄｸ"
                    : prizeMissing          ? "ｹｲﾋﾝ ﾐｹﾝﾁ ｶｶﾘｲﾝﾍ"
                                            : stateMessage(s, now);
  if (msg) {
    lcdText(1, 0, msg, 16);
    return;
  }
  uint32_t p = plays, h = hits[1] + hits[2] + hits[3];
  switch ((now / 2500) % 6) {
    case 0:  // プレイ回数と当たり率
      lcdPrintf(1, "PLY%04lu HIT%3lu%%", (unsigned long)p, (unsigned long)(p ? h * 100 / p : 0));
      break;
    case 1:  // 号ごとの当たり回数
      lcdPrintf(1, "ARM %02lu/%02lu/%02lu", (unsigned long)hits[1], (unsigned long)hits[2], (unsigned long)hits[3]);
      break;
    case 2:  // 回転の実測（1周、1マス）
      lcdPrintf(1, "LAP%4lu SQ%3lums", (unsigned long)lapMs, (unsigned long)squareMs);
      break;
    case 3: {  // 稼働時間と入力元
      uint32_t sec = now / 1000;
      lcdPrintf(1, "UP%3lu:%02lu:%02lu %s", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60),
                (unsigned long)(sec % 60), simActive ? "SIM" : "PLC");
      break;
    }
    case 4:  // 景品の獲得回数と未検知回数
      lcdPrintf(1, "GET%03lu MISS%02lu", (unsigned long)prizeGets, (unsigned long)prizeMisses);
      break;
    case 5:
      lcdText(1, 0, "SYSTEM OK", 16);
      break;
  }
}

// 変わった文字を最大maxChars文字だけ書き込む
void flushLcd(int maxChars) {
  for (int row = 0; row < 2 && maxChars > 0; row++) {
    for (int col = 0; col < 16 && maxChars > 0; col++) {
      if (lcdWant[row][col] == lcdShown[row][col]) continue;
      lcd->setCursor(col, row);
      lcd->write(lcdWant[row][col]);
      lcdShown[row][col] = lcdWant[row][col];
      maxChars--;
    }
  }
}

// ===================================================================
// 基板上のステータスLED（WS2812B、GP23）。core1で動かす
// ===================================================================

Adafruit_NeoPixel statusLed(1, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

void renderStatus(uint32_t now) {
  uint32_t c;
  if (prizeMissing) {
    c = (now / 150) % 2 ? strip.Color(255, 0, 0) : 0;  // 景品未検知：赤の速い点滅
  } else if (touchBits) {
    c = strip.Color(255, 255, 255);                    // タッチに触れている：白
  } else if (dropBits) {
    c = strip.Color(255, 180, 0);                      // 落下スイッチが押されている：黄
  } else if (demoActive) {
    c = hsv(now * 100, 255, 255);                      // デモ演出：虹色
  } else if (simActive) {
    c = (now / 300) % 2 ? strip.Color(255, 0, 255) : 0;  // シミュレーション：マゼンタの点滅
  } else {
    // 通常：場面ごとの色でゆっくり明暗
    uint8_t v = 60 + (uint8_t)(195 * (1 - cosf(now / 400.0f)) / 2);
    State s = state;
    c = s == ST_IDLE      ? hsv(21845, 255, v)   // 緑
        : s == ST_STANDBY ? hsv(0, 0, v)         // 白
        : s == ST_SPIN    ? hsv(43690, 255, v)   // 青
        : s == ST_WIN     ? hsv(7000, 230, v)    // 金
                          : hsv(50000, 255, v);  // 紫
  }
  if (!lcd && now % 2000 < 100) c = strip.Color(255, 80, 0);  // LCDが見つからない：2秒に1回オレンジ
  statusLed.setPixelColor(0, c);
  statusLed.show();
}

void setup1() {
  setupLcd();
  statusLed.begin();
  statusLed.setBrightness(STATUS_BRIGHTNESS);
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();
}

void loop1() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last >= FRAME_MS) {
    last = now;
    renderFrame(now);
    renderStatus(now);
    if (lcd) composeLcd(now);
  }
  if (lcd) flushLcd(4);  // 1文字 約1ms。LEDのコマを遅らせないよう少しずつ
}

// ===================================================================

void setup() {
  Serial.begin(115200);
  for (int ch = 0; ch < NUM_CH; ch++) pinMode(CH_PIN[ch], INPUT_PULLUP);
  for (int i = 0; i < NUM_DROP; i++) pinMode(DROP_PIN[i], INPUT_PULLUP);  // 外付けプルアップに加えて内部も有効に
  for (int i = 0; i < NUM_TOUCH; i++) pinMode(TOUCH_PIN[i], INPUT);         // TTP223は出力がHIGH/LOWを出す

  uint32_t start = millis();
  while (!Serial && millis() - start < 2000) delay(10);
  soundBegin();
  printHelp();
  playSlot(SL_POWER);
  sceneMusic(MUSIC_IDLE);
  add_repeating_timer_us(-1000, sampleInputs, nullptr, &sampleTimer);  // MP3のデコードより優先度が高い
}

void loop() {
  uint32_t now = millis();
  handleSerial(now);
  uint32_t head = sampleHead;
  if (head - sampleTail > SAMPLE_RING) sampleTail = head - SAMPLE_RING;  // 遅れすぎた分は捨てる
  while (sampleTail != head) {
    const volatile PinSample &smp = sampleRing[sampleTail % SAMPLE_RING];
    uint32_t t = smp.ms;
    curGpio = smp.gpio;
    sampleTail++;
    uint32_t lag = millis() - t;
    if (lag > maxLagMs) maxLagMs = lag;
    updateInputs(t);
    updateDrop(t);
    printDropWatch(t);
    updateTouch(t);
    updateDemo(t);
  }
  linkUpdate(now);
  soundUpdate();
}
