// トライポッド サウンド設定：画面
import { connect, availableMethods } from "./transport.js";
import { Device, utf8Length, MAX_NAME_BYTES, PROTOCOL_VERSION } from "./device.js";
import { prepareUpload, AUDIO_EXT, isWav } from "./convert.js";

const $ = (s) => document.querySelector(s);
const el = (tag, props = {}, ...children) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const c of children) if (c != null) e.append(c);
  return e;
};

// マイコンから送られる場面（#st）→ 表示名と、光らせる場面のカード
const SCENES = {
  idle: ["待機", "idle"],
  standby: ["スタンバイ", "standby"],
  spin: ["回転中", "spin"],
  win1: ["当たり1号", "win"],
  win2: ["当たり2号", "win"],
  win3: ["当たり3号", "win"],
  lose: ["ハズレ", "lose"],
};

const S = {
  link: null,
  dev: null,
  slots: [],       // マイコンの場面の一覧（#slots）
  files: [],       // マイコンの音のファイル（#ls）。MP3はBGMの場面だけ、WAVはどの場面でも使える
  cfg: null,       // 編集中の設定 { slots: { id: [ {t, f, v} ] } }
  savedJson: "",   // マイコンに保存されている設定（変更があるかの比較用）
  scene: null,
  busy: false,     // 転送中
};

// ---------------------------------------------------------------------
// 表示の部品

let toastTimer = 0;
function toast(msg, error = false) {
  const t = $("#toast");
  t.textContent = msg;
  t.classList.toggle("error", error);
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), error ? 6000 : 3000);
}

const fmtSize = (n) => (n >= 1024 * 1024 ? (n / 1024 / 1024).toFixed(2) + " MB" : Math.ceil(n / 1024) + " KB");

function showTab(name) {
  for (const b of document.querySelectorAll(".tabs button")) b.classList.toggle("active", b.dataset.tab === name);
  $("#tab-scenes").hidden = name !== "scenes";
  $("#tab-sounds").hidden = name !== "sounds";
}

function setConnectedUi(on) {
  $("#connect").textContent = on ? "切断" : "接続";
  $("#connect").classList.toggle("primary", !on);
  $("#intro").hidden = on;
  $(".tabs").hidden = !on;
  $("#storage").hidden = !on;
  $("#stop").hidden = !on;
  $("#save-bar").hidden = !on;
  $("#scene").hidden = !on;
  if (on) showTab(document.querySelector(".tabs button.active")?.dataset.tab || "scenes");
  else {
    $("#tab-scenes").hidden = true;
    $("#tab-sounds").hidden = true;
    $("#link-status").textContent = "未接続";
  }
}

// ---------------------------------------------------------------------
// 接続

$("#connect").addEventListener("click", async () => {
  if (S.link) return disconnect();
  try {
    S.link = await connect();
  } catch (e) {
    if (e.name !== "NotFoundError") toast("接続できません：" + e.message, true);
    return;
  }
  S.link.onClose = () => {
    toast("マイコンとの接続が切れました", true);
    disconnect(false);
  };
  S.dev = new Device(S.link);
  S.dev.onState = (id) => setScene(id);
  $("#link-status").textContent = "接続中…";
  setConnectedUi(true);
  try {
    const info = await S.dev.hello();
    if (info.name !== "tripodsnd") throw new Error("本番プログラム（game）ではないようです");
    if (info.version !== PROTOCOL_VERSION) {
      // どちらが古いかで、直し方が違う
      throw new Error(Number(info.version) > Number(PROTOCOL_VERSION)
        ? `GUIが古いままです（GUI 版${PROTOCOL_VERSION}・マイコン 版${info.version}）。再読み込みしてください`
        : `マイコンのプログラムが古いです（GUI 版${PROTOCOL_VERSION}・マイコン 版${info.version}）。gameを書き込み直してください`);
    }
    $("#link-status").textContent = `● 接続中（${S.link.kind}）`;
    S.slots = await S.dev.slots();
    await refreshFiles(info);
    const cfg = await S.dev.config();
    S.cfg = normalize(cfg);
    S.savedJson = JSON.stringify(S.cfg);
    renderSlots();
    updateDirty();
    S.dev.requestState();
  } catch (e) {
    toast("マイコンと通信できません：" + e.message, true);
    disconnect();
  }
});

async function disconnect(close = true) {
  const link = S.link;
  S.link = null;
  S.dev = null;
  if (close && link) await link.close();
  setConnectedUi(false);
}

function setScene(id) {
  S.scene = SCENES[id] ? id : null;
  $("#scene").textContent = S.scene ? "今の場面：" + SCENES[id][0] : "";
  const now = S.scene ? SCENES[S.scene][1] : null;
  for (const card of document.querySelectorAll(".slot[data-slot]")) {
    const on = card.dataset.slot === now;
    card.classList.toggle("now", on);
    card.querySelector(".tag.now").hidden = !on;
  }
}

// ---------------------------------------------------------------------
// 設定（場面ごとの候補）

// 場面で使えるファイルか（BGMはMP3もWAVも、効果音はWAVだけ）。マイコンの fileProblem() と合わせる
const fileFits = (slot, name) => slot.file && (slot.bgm || isWav(name));

// BGMの流し方（マイコンの bgmMode）
const BGM_MODES = [
  ["scene", "場面ごと", "待機中は待機BGM、回転中は回転中BGM。スタンバイ・当たり・ハズレの間はBGMなし"],
  ["through", "プレイ中も流す", "待機BGMを止めずに流し続け、効果音を重ねる。回転中だけ回転中BGMに切り替え、止まったら待機BGMの続きから"],
];

function normalize(cfg) {
  const out = { bgmMode: cfg?.bgmMode === "through" ? "through" : "scene", slots: {} };
  for (const s of S.slots) {
    out.slots[s.id] = (cfg?.slots?.[s.id] || []).map((c) => (c.t === "mp3" ? { ...c, t: "file" } : { ...c }));
  }
  return out;
}

// マイコンに送る形（無音は音量なし、電子音は f なし）
const cleanedCand = (c) =>
  c.t === "file" ? { t: "file", f: c.f, v: c.v ?? 100 } : c.t === "chip" ? { t: "chip", v: c.v ?? 100 } : { t: "none" };

function cleaned() {
  const out = { bgmMode: S.cfg.bgmMode, slots: {} };
  for (const s of S.slots) out.slots[s.id] = S.cfg.slots[s.id].map(cleanedCand);
  return out;
}

function updateDirty() {
  const dirty = JSON.stringify(cleaned()) !== S.savedJson;
  $("#save").disabled = !dirty || S.busy;
  $("#save-bar").classList.toggle("dirty", dirty);
  $("#save-msg").textContent = dirty ? "保存していない変更があります" : "マイコンの設定と同じです";
}

$("#save").addEventListener("click", async () => {
  const cfg = cleaned();
  $("#save").disabled = true;
  try {
    await S.dev.saveConfig(cfg);
    S.savedJson = JSON.stringify(cfg);
    toast("マイコンに保存しました");
  } catch (e) {
    toast("保存できません：" + e.message, true);
  }
  updateDirty();
  renderFiles();  // 「使用中」の表示を更新
});

$("#stop").addEventListener("click", () => S.dev?.stop().catch((e) => toast(e.message, true)));

// 候補の選択肢：value は "chip" / "none" / "file:<名前>"
function candValue(c) {
  return c.t === "file" ? "file:" + c.f : c.t;
}

function renderSlots() {
  const list = $("#slot-list");
  list.replaceChildren(bgmModeCard());
  for (const slot of S.slots) {
    const cands = S.cfg.slots[slot.id];
    const head = el("div", { className: "slot-head" },
      el("span", { className: "name", textContent: slot.name }),
      el("span", { className: "tag now", textContent: "いまここ", hidden: true }),
      cands.length > 1 ? el("span", { className: "tag random", textContent: "ランダム" }) : null,
      el("span", { className: "tag" + (slot.bgm ? " bgm" : ""), textContent: slot.bgm ? "BGM（ループ）" : "効果音" }),
    );
    const card = el("div", { className: "slot" }, head);
    card.dataset.slot = slot.id;

    if (!cands.length) card.append(el("div", { className: "empty", textContent: "無音（候補がありません）" }));
    cands.forEach((c, i) => card.append(candRow(slot, c, i)));

    const add = el("button", { className: "small", textContent: "＋ 候補を追加" });
    add.addEventListener("click", () => {
      // 新しい候補：まだ使っていないファイルがあればそれ、なければ電子音か無音
      const used = new Set(cands.filter((c) => c.t === "file").map((c) => c.f));
      const free = S.files.find((f) => fileFits(slot, f.name) && !used.has(f.name));
      cands.push(free ? { t: "file", f: free.name, v: 100 } : slot.chip ? { t: "chip", v: 100 } : { t: "none" });
      renderSlots();
      updateDirty();
    });
    card.append(el("div", { className: "row", style: "margin-top:6px" }, add));
    list.append(card);
  }
  setScene(S.scene);
}

function bgmModeCard() {
  const card = el("div", { className: "slot mode" },
    el("div", { className: "slot-head" }, el("span", { className: "name", textContent: "BGMの流し方" })));
  for (const [value, label, desc] of BGM_MODES) {
    const radio = el("input", { type: "radio", name: "bgm-mode", value, checked: S.cfg.bgmMode === value });
    radio.addEventListener("change", () => {
      S.cfg.bgmMode = value;
      updateDirty();
    });
    card.append(el("label", { className: "mode-opt" }, radio,
      el("span", {}, el("b", { textContent: label }), el("span", { className: "dim", textContent: desc }))));
  }
  return card;
}

function candRow(slot, c, i) {
  const cands = S.cfg.slots[slot.id];
  const sel = el("select");
  if (slot.chip) sel.append(el("option", { value: "chip", textContent: "電子音（いまの曲）" }));
  sel.append(el("option", { value: "none", textContent: "無音" }));
  for (const f of S.files) {
    if (fileFits(slot, f.name)) sel.append(el("option", { value: "file:" + f.name, textContent: f.name }));
  }
  const missing = c.t === "file" && !S.files.some((f) => f.name === c.f && fileFits(slot, f.name));
  if (missing) sel.append(el("option", { value: "file:" + c.f, textContent: "（見つかりません）" + c.f }));
  sel.value = candValue(c);
  sel.addEventListener("change", () => {
    const v = sel.value;
    cands[i] = v.startsWith("file:") ? { t: "file", f: v.slice(5), v: c.v ?? 100 } : v === "chip" ? { t: "chip", v: c.v ?? 100 } : { t: "none" };
    renderSlots();
    updateDirty();
  });

  const volText = el("span", { textContent: (c.v ?? 100) + "%" });
  const vol = el("input", { type: "range", min: 0, max: 100, step: 5, value: c.v ?? 100, disabled: c.t === "none" });
  vol.addEventListener("input", () => {
    c.v = Number(vol.value);
    volText.textContent = c.v + "%";
    updateDirty();
  });

  const play = el("button", { className: "icon play", textContent: "▶", title: "マイコンで試聴", disabled: c.t === "none" || missing });
  play.addEventListener("click", () => S.dev.preview(slot.id, cleanedCand(c)).catch((e) => toast("試聴できません：" + e.message, true)));
  const del = el("button", { className: "icon del danger", textContent: "✕", title: "この候補を消す" });
  del.addEventListener("click", () => {
    cands.splice(i, 1);
    renderSlots();
    updateDirty();
  });

  return el("div", { className: "cand" + (missing ? " missing" : "") }, sel,
    el("label", { className: "vol" }, "音量", vol, volText), play, del);
}

// ---------------------------------------------------------------------
// 音源（MP3・WAV）

async function refreshFiles(info) {
  info = info || (await S.dev.hello());
  S.files = (await S.dev.files()).sort((a, b) => a.name.localeCompare(b.name, "ja"));
  const pct = info.total ? (info.used / info.total) * 100 : 0;
  $("#storage-used").style.width = pct.toFixed(1) + "%";
  $("#storage-text").textContent = `空き ${fmtSize(info.total - info.used)} / ${fmtSize(info.total)}`;
  renderFiles();
}

// 保存済みの設定と編集中の設定の、どちらかでこのファイルを使っている場面
function usedIn(name) {
  const names = [];
  for (const s of S.slots) {
    if ((S.cfg?.slots[s.id] || []).some((c) => c.t === "file" && c.f === name)) names.push(s.name);
  }
  return names;
}

function renderFiles() {
  const list = $("#file-list");
  list.replaceChildren();
  if (!S.files.length) {
    list.append(el("p", { className: "dim", textContent: "まだ音源がありません。上から転送してください。" }));
    return;
  }
  for (const f of S.files) {
    const used = usedIn(f.name);
    const wav = isWav(f.name);
    const play = el("button", { className: "icon", textContent: "▶", title: "マイコンで試聴" });
    // WAVは効果音（電源ON）として1回、MP3はBGM（待機BGM）として鳴らす。BGMは ■停止 で止める
    play.addEventListener("click", () =>
      S.dev.preview(wav ? "power" : "idle", { t: "file", f: f.name, v: 80 }).catch((e) => toast("試聴できません：" + e.message, true))
    );
    const del = el("button", { className: "icon danger", textContent: "✕", title: "マイコンから消す", disabled: S.busy });
    del.addEventListener("click", async () => {
      const warn = used.length ? `\n「${used.join("」「")}」で使っています。消すとその候補は鳴らなくなります。` : "";
      if (!confirm(`${f.name} をマイコンから消しますか？${warn}`)) return;
      try {
        await S.dev.remove(f.name);
        toast(f.name + " を消しました");
        await refreshFiles();
        renderSlots();
      } catch (e) {
        toast("消せません：" + e.message, true);
      }
    });
    list.append(el("div", { className: "file" },
      el("span", { className: "name", textContent: f.name }),
      el("span", { className: "kind" + (wav ? "" : " bgm"), textContent: wav ? "どこでも" : "BGMだけ" }),
      used.length ? el("span", { className: "used", textContent: "使用中" }) : null,
      el("span", { className: "size", textContent: fmtSize(f.size) }),
      play, del));
  }
}

async function uploadFiles(fileList) {
  const files = [...fileList].filter((f) => AUDIO_EXT.test(f.name));
  if (!files.length) {
    toast("音のファイル（MP3・WAVなど）がありません", true);
    return;
  }
  if (S.busy || !S.dev) return;
  S.busy = true;
  updateDirty();
  const box = $("#upload-list");
  const rows = files.map((f) => {
    const bar = el("div");
    const msg = el("span", { className: "msg", textContent: "待機中" });
    const name = el("span", { className: "name", textContent: f.name });
    const row = el("div", { className: "upload" }, name, el("div", { className: "prog" }, bar), msg);
    box.append(row);
    return { f, row, name, bar, msg };
  });
  let ok = 0;
  for (const r of rows) {
    if (!S.dev) break;
    try {
      // 短いものはWAVに変換する（名前の拡張子も .wav になる）。長いMP3はそのまま
      r.msg.textContent = "変換中";
      const up = await prepareUpload(r.f);
      if (utf8Length(up.name) > MAX_NAME_BYTES || up.name.startsWith(".")) {
        r.row.classList.add("fail");
        r.msg.textContent = "名前が長い";
        continue;
      }
      r.name.textContent = up.name + (up.converted ? `（${fmtSize(up.bytes.length)}・WAVに変換）` : "（BGMだけで使えます）");
      r.msg.textContent = "転送中";
      await S.dev.upload(up.name, up.bytes, (p) => {
        r.bar.style.width = (p * 100).toFixed(1) + "%";
        r.msg.textContent = Math.floor(p * 100) + "%";
      });
      r.row.classList.add("done");
      r.msg.textContent = "完了";
      ok++;
    } catch (e) {
      r.row.classList.add("fail");
      r.msg.textContent = e.message === "nospace" ? "空き不足" : e.message === "crc" ? "壊れた" : "失敗";
      toast(`${r.f.name}：転送できません（${e.message}）`, true);
    }
  }
  S.busy = false;
  if (S.dev) {
    await refreshFiles().catch(() => {});
    renderSlots();
  }
  updateDirty();
  toast(`${ok} / ${rows.length} 個を転送しました`);
  setTimeout(() => {
    for (const r of rows) if (r.row.classList.contains("done")) r.row.remove();
  }, 4000);
}

const drop = $("#drop");
drop.addEventListener("dragover", (e) => {
  e.preventDefault();
  drop.classList.add("over");
});
drop.addEventListener("dragleave", () => drop.classList.remove("over"));
drop.addEventListener("drop", async (e) => {
  e.preventDefault();
  drop.classList.remove("over");
  uploadFiles(await droppedFiles(e.dataTransfer));
});
$("#pick-files").addEventListener("change", (e) => {
  uploadFiles(e.target.files);
  e.target.value = "";
});
$("#pick-dir").addEventListener("change", (e) => {
  uploadFiles(e.target.files);
  e.target.value = "";
});

// フォルダをドロップされたときは中のファイルを全部集める
async function droppedFiles(dt) {
  const entries = [...dt.items].map((i) => i.webkitGetAsEntry?.()).filter(Boolean);
  if (!entries.length) return [...dt.files];
  const out = [];
  const walk = async (entry) => {
    if (entry.isFile) {
      out.push(await new Promise((res, rej) => entry.file(res, rej)));
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      for (;;) {
        const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        if (!batch.length) break;
        for (const e of batch) await walk(e);
      }
    }
  };
  for (const e of entries) await walk(e);
  return out;
}

// ---------------------------------------------------------------------

for (const b of document.querySelectorAll(".tabs button")) b.addEventListener("click", () => showTab(b.dataset.tab));

const av = availableMethods();
if (!av.serial && !av.usb) {
  $("#unsupported").hidden = false;
  $("#connect").disabled = true;
}

if ("serviceWorker" in navigator) navigator.serviceWorker.register("sw.js").catch(() => {});
