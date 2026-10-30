// GUI（web/）とのやりとり
//
// | コマンド                         | 返事                                  | 内容 |
// |----------------------------------|---------------------------------------|------|
// | #hello                           | #info tripodsnd <版> <全容量> <使用量> | 接続の確認 |
// | #slots                           | #slot <id> <bgm> <chip> <file> <名前> … #end | 場面の一覧（0/1は、BGMか・電子音の曲があるか・ファイルを選べるか） |
// | #ls                              | #file <サイズ> <名前> … #end           | 音のファイルの一覧 |
// | #cfg                             | #cfg <JSON>                           | 設定の読み出し |
// | #cfgset <JSON>                   | #ok / #err <理由>                     | 設定の保存 |
// | #put <サイズ> <CRC32(16進)> <名前> | #ready → 4096バイトごとに #ack <受信済み> → #ok | ファイルの転送。CRCが合わなければ消して #err |
// | #rm <名前>                       | #ok                                   | ファイルの削除 |
// | #playc <場面> <chip/file/none> <音量> [名前] | #ok                       | 候補を1つ試聴 |
// | #play <場面>                     | #ok                                   | 場面の音を鳴らす（ランダムに選ぶ） |
// | #stop                            | #ok                                   | 試聴をやめて、今の場面のBGMに戻す |
// | #st?                             | #st <場面>                            | 今の場面。場面が変わったときもマイコンから送る |
//
// ファイルはMP3かWAV。BGMの場面はどちらも、効果音の場面はWAVだけ鳴らせる（fileProblem()）。
// 版2から：設定の "t" と #playc の種類は "file"（版1の "mp3" も受け付ける）
// 版3から：設定に "bgmMode"（"scene"＝場面ごと、"through"＝プレイ中も待機BGMを流す）。ないときは "scene"

#include "link.h"
#include "sound.h"
#include <LittleFS.h>

const char *LINK_VERSION = "3";
const char *UPLOAD_TMP = "/snd/.upload";
const size_t CHUNK = 4096;
const uint32_t UPLOAD_TIMEOUT_MS = 10000;

static char line[8192];
static size_t lineLen = 0;
static bool inLine = false;

// ---- CRC32（zlibと同じ。GUIの device.js と合わせる） ----
static uint32_t crcTable[256];

static void crcInit() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[i] = c;
  }
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) crc = crcTable[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

// ---- ファイルの転送 ----
static struct {
  bool active = false;
  File file;
  char name[FILE_NAME_LEN];
  uint32_t size = 0, got = 0, crc = 0, expect = 0, lastAt = 0;
  size_t bufLen = 0;
  uint8_t buf[CHUNK];
} up;

bool linkUploading() {
  return up.active;
}

static void endUpload() {
  up.active = false;
  soundSetMuted(false);
  restoreSceneBgm();
}

static void abortUpload(const char *why) {
  if (up.file) up.file.close();
  LittleFS.remove(UPLOAD_TMP);
  Serial.printf("#err %s\n", why);
  Serial.printf("[GUI] %s の転送を中止（%s）\n", up.name, why);
  endUpload();
}

static void finishUpload() {
  up.file.close();
  if (up.crc != up.expect) {
    LittleFS.remove(UPLOAD_TMP);
    Serial.println("#err crc");
    Serial.printf("[GUI] %s のCRCが合わないので消しました\n", up.name);
    endUpload();
    return;
  }
  String path = String("/snd/") + up.name;
  LittleFS.remove(path);
  if (!LittleFS.rename(UPLOAD_TMP, path)) {
    LittleFS.remove(UPLOAD_TMP);
    Serial.println("#err rename");
    endUpload();
    return;
  }
  Serial.println("#ok");
  Serial.printf("[GUI] %s を保存しました（%lu バイト）\n", up.name, (unsigned long)up.size);
  endUpload();
}

void linkUpdate(uint32_t) {
  if (!up.active) return;
  // 呼び出し側の時刻は、転送を始めた時刻（up.lastAt）より古いことがあるので、ここで取り直す
  uint32_t now = millis();
  while (up.got < up.size) {
    int avail = Serial.available();
    if (avail <= 0) break;
    size_t want = CHUNK - up.bufLen;
    if (want > up.size - up.got) want = up.size - up.got;
    if (want > (size_t)avail) want = avail;
    size_t n = Serial.readBytes(up.buf + up.bufLen, want);
    up.bufLen += n;
    up.got += n;
    up.lastAt = now;
    if (up.bufLen == CHUNK || up.got == up.size) {
      if (up.file.write(up.buf, up.bufLen) != up.bufLen) {
        abortUpload("write");
        return;
      }
      up.crc = crcUpdate(up.crc, up.buf, up.bufLen);
      up.bufLen = 0;
      if (up.got < up.size) Serial.printf("#ack %lu\n", (unsigned long)up.got);
    }
  }
  if (up.got == up.size) finishUpload();
  else if (now - up.lastAt > UPLOAD_TIMEOUT_MS) abortUpload("timeout");
}

static bool validName(const char *name) {
  size_t len = strlen(name);
  return len > 0 && len < FILE_NAME_LEN && name[0] != '.' && !strchr(name, '/') && !strchr(name, '\\');
}

// "#put <サイズ> <CRC> <名前>"
static void startUpload(char *args) {
  char *end;
  uint32_t size = strtoul(args, &end, 10);
  uint32_t crc = strtoul(end, &end, 16);
  while (*end == ' ') end++;
  if (!size || !validName(end)) {
    Serial.println("#err bad");
    return;
  }
  FSInfo info;
  LittleFS.info(info);
  String path = String("/snd/") + end;
  uint32_t reuse = 0;
  if (LittleFS.exists(path)) {
    File old = LittleFS.open(path, "r");
    reuse = old.size();
    old.close();
  }
  // 余裕（ブロック数個分）を見て、空きが足りるか
  if ((uint64_t)size + 64 * 1024 > (uint64_t)info.totalBytes - info.usedBytes + reuse) {
    Serial.println("#err nospace");
    return;
  }
  soundSetMuted(true);  // フラッシュに書いている間は音が途切れるので止める
  up.file = LittleFS.open(UPLOAD_TMP, "w");
  if (!up.file) {
    soundSetMuted(false);
    Serial.println("#err open");
    return;
  }
  strcpy(up.name, end);
  up.size = size;
  up.expect = crc;
  up.got = 0;
  up.crc = 0;
  up.bufLen = 0;
  up.lastAt = millis();
  up.active = true;
  Serial.printf("[GUI] %s を受信します（%lu バイト）\n", up.name, (unsigned long)size);
  Serial.println("#ready");
}

// ---- コマンド ----

static void cmdInfo() {
  FSInfo info;
  LittleFS.info(info);
  Serial.printf("#info tripodsnd %s %lu %lu\n", LINK_VERSION, (unsigned long)info.totalBytes,
                (unsigned long)info.usedBytes);
}

static void cmdSlots() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    const SlotInfo &s = SLOT_INFO[i];
    Serial.printf("#slot %s %d %d %d %s\n", s.id, s.bgm, chipAvailable((Slot)i), s.fileAllowed, s.name);
  }
  Serial.println("#end");
}

static void cmdList() {
  Dir d = LittleFS.openDir("/snd");
  while (d.next()) {
    if (!d.isFile() || d.fileName().startsWith(".")) continue;
    Serial.printf("#file %lu %s\n", (unsigned long)d.fileSize(), d.fileName().c_str());
  }
  Serial.println("#end");
}

// "#playc <場面> <chip/file/none> <音量> [名前]"
static void cmdPreview(char *args) {
  char *slotId = strtok(args, " ");
  char *type = strtok(nullptr, " ");
  char *vol = strtok(nullptr, " ");
  char *name = strtok(nullptr, "");
  Slot s;
  if (!slotId || !type || !vol || !slotFromId(slotId, s)) {
    Serial.println("#err bad");
    return;
  }
  Cand c = {CAND_NONE, (uint8_t)constrain(atoi(vol), 0, 100), ""};
  if (strcmp(type, "chip") == 0) {
    c.type = CAND_CHIP;
  } else if (strcmp(type, "file") == 0 || strcmp(type, "mp3") == 0) {
    if (!name || !validName(name)) {
      Serial.println("#err bad");
      return;
    }
    if (const char *why = fileProblem(s, name)) {
      Serial.printf("#err %s\n", why);
      return;
    }
    c.type = CAND_FILE;
    strcpy(c.file, name);
  }
  Serial.println(previewCand(s, c) ? "#ok" : "#err play");
}

static void handleLine(char *l) {
  char *args = strchr(l, ' ');
  if (args) *args++ = '\0';
  else args = l + strlen(l);

  if (strcmp(l, "#hello") == 0) {
    cmdInfo();
  } else if (strcmp(l, "#slots") == 0) {
    cmdSlots();
  } else if (strcmp(l, "#ls") == 0) {
    cmdList();
  } else if (strcmp(l, "#cfg") == 0) {
    Serial.print("#cfg ");
    printConfigJson(Serial);
    Serial.println();
  } else if (strcmp(l, "#cfgset") == 0) {
    const char *err = nullptr;
    if (applyConfigJson(args, err)) {
      Serial.println("#ok");
      Serial.println("[GUI] 音の設定を保存しました");
      stopBgm();  // 待機BGMなどを新しい設定で鳴らし直す
      restoreSceneBgm();
    } else {
      Serial.printf("#err %s\n", err);
    }
  } else if (strcmp(l, "#put") == 0) {
    startUpload(args);
  } else if (strcmp(l, "#rm") == 0) {
    if (!validName(args)) {
      Serial.println("#err bad");
      return;
    }
    stopAllSound();  // 鳴らしているファイルを消すかもしれないので止める
    bool ok = LittleFS.remove(String("/snd/") + args);
    Serial.println(ok ? "#ok" : "#err notfound");
    restoreSceneBgm();
  } else if (strcmp(l, "#playc") == 0) {
    cmdPreview(args);
  } else if (strcmp(l, "#play") == 0) {
    Slot s;
    if (!slotFromId(args, s)) {
      Serial.println("#err bad");
      return;
    }
    playSlot(s);
    Serial.println("#ok");
  } else if (strcmp(l, "#stop") == 0) {
    stopAllSound();
    restoreSceneBgm();
    Serial.println("#ok");
  } else if (strcmp(l, "#st?") == 0) {
    linkNotifyState(currentSceneId());
  } else {
    Serial.println("#err unknown");
  }
}

bool linkFeedChar(char c) {
  static bool crcReady = false;
  if (!crcReady) {
    crcInit();
    crcReady = true;
  }
  if (!inLine) {
    if (c != '#') return false;
    inLine = true;
    lineLen = 0;
  }
  if (c == '\n' || c == '\r') {
    inLine = false;
    line[lineLen] = '\0';
    if (lineLen > 1) handleLine(line);
    return true;
  }
  if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
  return true;
}

void linkNotifyState(const char *sceneId) {
  Serial.printf("#st %s\n", sceneId);
}
