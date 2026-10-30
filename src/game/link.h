// GUI（web/）とのやりとり。USBシリアルで「#」で始まる行をやりとりする。
// それ以外の行は今までどおりのログで、GUIは読み飛ばす。コマンドの一覧は link.cpp の先頭。
#pragma once
#include <Arduino.h>

// シリアルの1文字を渡す。GUIのコマンドの一部ならtrue（呼び出し側では扱わない）
bool linkFeedChar(char c);
// ファイルの転送中はtrue。このあいだはシリアルの受信をすべてlinkが読む
bool linkUploading();
// loop()から呼ぶ。転送中のデータを受け取る
void linkUpdate(uint32_t now);
// 場面が変わったらGUIに知らせる（#st <場面>）
void linkNotifyState(const char *sceneId);

// main.cpp で定義する
const char *currentSceneId();  // 今の場面（idle / standby / spin / win1〜3 / lose）
void restoreSceneBgm();        // 試聴や転送のあと、今の場面のBGMに戻す
