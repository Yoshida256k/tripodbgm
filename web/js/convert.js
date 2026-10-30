// 転送する前の音のファイルの変換
//
// マイコンはMP3を2本同時にデコードするとCPUが足りないので、効果音はWAVだけ鳴らす。
// そこで、短いファイル（効果音・短いループ）はWAV（44.1kHz・モノラル・16bit）に変換して送る。
// 長いファイル（BGM）はWAVだと大きすぎる（1分で約5MB）ので、MP3のまま送る（BGMの場面だけで使える）。

export const WAV_MAX_SECONDS = 30;  // これ以下の長さならWAVにする
const RATE = 44100;

export const AUDIO_EXT = /\.(mp3|wav|m4a|aac|ogg|oga|opus|flac)$/i;

export const isWav = (name) => /\.wav$/i.test(name);
export const isMp3 = (name) => /\.mp3$/i.test(name);

// 送るファイル名とデータを返す。{ name, bytes, seconds, converted }
export async function prepareUpload(file) {
  const original = await file.arrayBuffer();
  // decodeAudioData は渡したデータを使えなくするので、コピーを渡す
  const audio = await decode(original.slice(0));
  const seconds = audio.duration;
  if (seconds > WAV_MAX_SECONDS && isMp3(file.name)) {
    return { name: file.name, bytes: new Uint8Array(original), seconds, converted: false };
  }
  const name = file.name.replace(/\.[^.]+$/, "") + ".wav";
  return { name, bytes: toWav(audio), seconds, converted: true };
}

// 44.1kHz に変換して読み込む（OfflineAudioContext は、そのサンプリング周波数に合わせて読み込む）
async function decode(buffer) {
  const ctx = new OfflineAudioContext(1, 1, RATE);
  try {
    return await ctx.decodeAudioData(buffer);
  } catch {
    throw new Error("音のファイルとして読めません");
  }
}

// モノラル・16bit の WAV にする
function toWav(audio) {
  const n = audio.length;
  const chans = [];
  for (let c = 0; c < audio.numberOfChannels; c++) chans.push(audio.getChannelData(c));
  const out = new ArrayBuffer(44 + n * 2);
  const v = new DataView(out);
  const str = (off, s) => [...s].forEach((ch, i) => v.setUint8(off + i, ch.charCodeAt(0)));
  str(0, "RIFF");
  v.setUint32(4, 36 + n * 2, true);
  str(8, "WAVE");
  str(12, "fmt ");
  v.setUint32(16, 16, true);       // fmt の長さ
  v.setUint16(20, 1, true);        // PCM
  v.setUint16(22, 1, true);        // モノラル
  v.setUint32(24, RATE, true);
  v.setUint32(28, RATE * 2, true); // 1秒のバイト数
  v.setUint16(32, 2, true);        // 1サンプルのバイト数
  v.setUint16(34, 16, true);       // 16bit
  str(36, "data");
  v.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) {
    let s = 0;
    for (const ch of chans) s += ch[i];
    s /= chans.length;
    s = Math.max(-1, Math.min(1, s));
    v.setInt16(44 + i * 2, Math.round(s * 32767), true);
  }
  return new Uint8Array(out);
}
