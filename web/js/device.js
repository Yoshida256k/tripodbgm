// マイコン（本番プログラム game）とのやりとり。
// 「#」で始まる行がGUI用のコマンドと返事。それ以外の行はマイコンのログなので、onLog に渡すだけ。
// コマンドの一覧は src/game/link.cpp の先頭を参照。

const CHUNK = 4096;
const TIMEOUT_MS = 15000;

// ---- CRC32（zlibと同じ。link.cpp と合わせる） ----
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export const utf8Length = (s) => new TextEncoder().encode(s).length;
export const MAX_NAME_BYTES = 127;
// マイコンの link.cpp の LINK_VERSION と合わせる（版2から、効果音はWAVだけ・候補の種類は "file"。版3から bgmMode）
export const PROTOCOL_VERSION = "3";

export class Device {
  constructor(transport) {
    this.t = transport;
    this.onState = () => {};
    this.onLog = () => {};
    this._waiters = [];  // 返事を待っている処理（先に来たものから順に照合）
    this._queue = Promise.resolve();
    transport.onLine = (line) => this._line(line);
  }

  _line(line) {
    if (!line.startsWith("#")) {
      this.onLog(line);
      return;
    }
    if (line.startsWith("#st ")) {
      this.onState(line.slice(4).trim());
      return;
    }
    const w = this._waiters[0];
    if (w && w.take(line)) {
      this._waiters.shift();
      clearTimeout(w.timer);
    }
  }

  // 1行送って、take(line) が true を返すまで待つ
  _wait(take, timeoutMs = TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
      const w = {
        take: (line) => {
          if (line.startsWith("#err")) {
            reject(new Error(line.slice(5) || "エラー"));
            return true;
          }
          const r = take(line);
          if (r !== undefined) resolve(r);
          return r !== undefined;
        },
      };
      w.timer = setTimeout(() => {
        const i = this._waiters.indexOf(w);
        if (i >= 0) this._waiters.splice(i, 1);
        reject(new Error("マイコンから返事がありません"));
      }, timeoutMs);
      this._waiters.push(w);
    });
  }

  // コマンドは1つずつ順番に送る
  _cmd(text, take, timeoutMs) {
    const run = async () => {
      const p = this._wait(take, timeoutMs);
      await this.t.write(text + "\n");
      return p;
    };
    const p = this._queue.then(run, run);
    this._queue = p.catch(() => {});
    return p;
  }

  // 一覧の返事（prefix の行を集めて #end まで）
  _list(cmd, prefix) {
    const rows = [];
    return this._cmd(cmd, (line) => {
      if (line === "#end") return rows;
      if (line.startsWith(prefix)) rows.push(line.slice(prefix.length));
      return undefined;
    });
  }

  _ok(cmd) {
    return this._cmd(cmd, (line) => (line === "#ok" ? true : undefined));
  }

  async hello() {
    await this.t.write("\n");  // 途中まで届いている行があれば区切る
    return this._cmd("#hello", (line) => {
      if (!line.startsWith("#info ")) return undefined;
      const [name, version, total, used] = line.slice(6).split(" ");
      return { name, version, total: Number(total), used: Number(used) };
    });
  }

  async slots() {
    const rows = await this._list("#slots", "#slot ");
    return rows.map((r) => {
      const [id, bgm, chip, file, ...name] = r.split(" ");
      return { id, bgm: bgm === "1", chip: chip === "1", file: file === "1", name: name.join(" ") };
    });
  }

  async files() {
    const rows = await this._list("#ls", "#file ");
    return rows.map((r) => {
      const i = r.indexOf(" ");
      return { size: Number(r.slice(0, i)), name: r.slice(i + 1) };
    });
  }

  config() {
    return this._cmd("#cfg", (line) => (line.startsWith("#cfg ") ? JSON.parse(line.slice(5)) : undefined));
  }

  saveConfig(cfg) {
    return this._ok("#cfgset " + JSON.stringify(cfg));
  }

  remove(name) {
    return this._ok("#rm " + name);
  }

  preview(slotId, cand) {
    const t = cand.t;
    const vol = cand.v ?? 100;
    return this._ok(`#playc ${slotId} ${t} ${vol}${t === "file" ? " " + cand.f : ""}`);
  }

  stop() {
    return this._ok("#stop");
  }

  requestState() {
    return this.t.write("#st?\n");
  }

  // 音のファイルを転送する。4096バイト送るごとにマイコンの #ack を待つ
  upload(name, bytes, onProgress = () => {}) {
    const run = async () => {
      const crc = crc32(bytes).toString(16);
      const ready = this._wait((l) => (l === "#ready" ? true : undefined));
      await this.t.write(`#put ${bytes.length} ${crc} ${name}\n`);
      await ready;
      for (let off = 0; off < bytes.length; off += CHUNK) {
        const end = Math.min(off + CHUNK, bytes.length);
        const last = end === bytes.length;
        const reply = last
          ? this._wait((l) => (l === "#ok" ? true : undefined), 30000)
          : this._wait((l) => (l.startsWith("#ack ") ? true : undefined));
        await this.t.writeBytes(bytes.subarray(off, end));
        await reply;
        onProgress(end / bytes.length);
      }
    };
    const p = this._queue.then(run, run);
    this._queue = p.catch(() => {});
    return p;
  }
}
