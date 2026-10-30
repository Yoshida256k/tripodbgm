// 音の出し方：場面（スロット）ごとに、電子音・MP3・無音の候補を持ち、鳴るたびにランダムに1つ選ぶ。
// 電子音・BGM（MP3かWAV）・効果音（WAV）をミキサーで合わせてI2S（PCM5102A）に出す。
// MP3を2本同時にデコードするとCPUが足りないので、効果音はWAVだけにしている（GUIが転送するときに変換する）。
// 音のファイルと設定はLittleFS（/snd/、/config.json）に置き、GUIから書き換える（link.cpp）。
#pragma once
#include <Arduino.h>

// 音を割り当てる場面。GUIとのやりとりでは SLOT_INFO の id（英字）を使う
enum Slot { SL_POWER, SL_IDLE, SL_STANDBY, SL_SPIN, SL_TICK, SL_WIN, SL_LOSE, SL_GET, SL_RESET, NUM_SLOTS };

enum CandType : uint8_t { CAND_NONE, CAND_CHIP, CAND_FILE };  // FILEはMP3かWAV

const size_t MAX_CAND = 8;        // 1つの場面に登録できる候補の数
const size_t FILE_NAME_LEN = 128; // 音のファイル名（UTF-8のバイト数、終端込み）

struct Cand {
  CandType type;
  uint8_t vol;  // 0〜100
  char file[FILE_NAME_LEN];
};

struct SlotInfo {
  const char *id;    // GUIとのやりとりで使う名前
  const char *name;  // ログ・GUIに出す名前
  bool bgm;          // trueならBGM（ループ）、falseなら効果音
  bool fileAllowed;  // ランプのピッは回数が多いので、電子音か無音だけ
};
extern const SlotInfo SLOT_INFO[NUM_SLOTS];

// ミキサー、I2S、LittleFS、設定の読み込み
void soundBegin();
// loop()から何度も呼ぶ。電子音の書き込みと、MP3ファイルの読み出し
void soundUpdate();

// 場面の音を鳴らす（候補からランダムに選ぶ）。pitchは電子音の高さ（当たりの号で変える）
void playSlot(Slot s, float pitch = 1.0f);
// 場面が変わったときのBGM。設定の「BGMの流し方」によって次のようにする
//   場面ごと：IDLEは待機BGM、SPINは回転中BGM、PLAY（スタンバイ・当たり・ハズレ）はBGMなし
//   通し　　：IDLEとPLAYは待機BGMを流し続け、SPINだけ回転中BGMにする（待機BGMは止まったら続きから）
enum Music { MUSIC_IDLE, MUSIC_PLAY, MUSIC_SPIN };
void sceneMusic(Music m);
void stopBgm();
void stopSfx();
void stopAllSound();
// GUIの試聴：場面の鳴らし方（BGM/効果音）で、指定した候補を1つ鳴らす
bool previewCand(Slot s, const Cand &c);
// ファイル転送中は音を止め、場面が変わっても鳴らさない
void soundSetMuted(bool muted);

// 負荷の確認用：音切れなどの回数を表示して0に戻す（? で表示）
void printSoundStats();

// 設定（JSON）
bool slotFromId(const char *id, Slot &out);
bool chipAvailable(Slot s);            // その場面に電子音の曲があるか
// その場面で鳴らせるファイルか。BGMはMP3かWAV、効果音はWAVだけ。だめなら理由を返す
const char *fileProblem(Slot s, const char *name);
void printConfigJson(Print &out);       // 今の設定をJSONで出す
bool applyConfigJson(const char *json, const char *&err);  // 検証して反映し、/config.jsonに保存
