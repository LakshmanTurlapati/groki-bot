[English](features.md) | [中文](features.zh-CN.md) | [日本語](features.ja.md)

# 機能

- **AI 音声対話**: OpenAI Realtime / Google Gemini Live / XiaoZhi サーバーに WebSocket
  接続。マイク入力 → 応答音声 → 口パク連動。半二重 CoreS3 では発話中マイクを止め、
  応答中は LCD タップ / 頭タッチ (Si12T) で barge-in 可能。サーバー側 VAD で
  ターン検出。OpenAI / Gemini ではツール (`set_expression` / `set_head_pose` /
  `speak_katakoto`) で表情・首振り・カタコト声を制御、XiaoZhi は感情を表情にマップ。
  応答テキストは吹き出しに進行表示。
- **アバター描画**: M5GFX で 30 fps。呼吸 / saccade / blink、6 表情 (Neutral / Happy
  / Sad / Angry / Doubt / Sleepy)。**Avatar DSL** (`.avdsl` ソース → `.avbc` バイトコード)
  で顔のレイアウトとアニメーションを差し替え可能 (BLE / Wi-Fi 経由でライブ更新)。
- **マイク連動口パク (lip sync)**: FFT + 帯域 log + spectral flux で開口度推定、
  雑音床 EWMA AGC で環境ノイズに追従。
- **サーボ**: SCS0009 yaw + pitch を UART1 (1 Mbps) で制御。台形速度
  プロファイル `PathGenerator`、駆動時のみトルク有効化。ボード別レンジ
  キャリブレーション (ServoLimits) を NVS 保存。
- **顔追従** (Mac 上のゲートウェイと併用): Mac のカメラで顔を検出する
  [tools/vision-tracker/](../tools/vision-tracker/README.md) の位置情報から、ゲートウェイが
  XiaoZhi の `head` メッセージで首を顔の方へ向けます。ロボットが話している間と
  ウェイクワード後に聞いている間は追従を止めます。設定手順は
  [gateway/README.md の Face tracking](../gateway/README.md#face-tracking)（英語）。
- **スピーカー / オーディオ**: 起動音 (C5–E5–G5、設定で OFF 可)、jtts ランダム
  babble、AAC 録音再生、BLE オーディオ ストリーム、Wi-Fi RTP (L16 / μ-law / AAC) 受信。
  音量は **0..200%** をライブ制御 (BLE / Wi-Fi / 本体 UI)。
- **NeoPixel**: nekomimi 用 LED ストリップ アニメーション (虹 / 単色 / リップシンク
  レベルメーター モード)。
- **LT タイマー**: 発表時間アシスト。残り N 秒で予告 → 時刻ぴったり → 超過繰返し
  アナウンス (jtts)。
- **オンデバイス UI**:
  - CoreS3 / StopWatch (touch panel): 画面右上タップで 5 タブ UI (情報 / 設定 /
    操作 / 範囲 / 会話 / LT)
  - AtomS3R / AtomS3 (button only): BtnA 短押でステータス オーバーレイ、長押で
    operation_mode サイクル
- **吹き出し**: 24 px ゴシック フォントの白パネル、長文は marquee スクロール。
- **BLE 設定サービス** (NimBLE GATT): `tools/settings.html` (Web Bluetooth) から
  Wi-Fi 接続先・API キー・各種設定・OTA 更新。Bluetooth 4.2 以降の Just Works
  ペアリング + アプリケーション層 X25519 + AES-256-GCM 暗号化、optional password 認証。
  設定は NVS に保存、Apply で再起動して反映。
- **Wi-Fi 設定サービス**: 接続後 HTTP サーバー (port 80) + `settings_wifi.html`、
  mDNS (`stackchan-XXXXXX.local`)。SSID / プロバイダ / API キー / システム
  プロンプト / 追加 HTTP ヘッダ / jtts / Avatar DSL / OTA 全て対応。
- **SoftAP プロビジョニング (iOS フレンドリー)**: STA が未設定 / 接続失敗時に
  本体トリガで AP モード起動 (`Stackchan-XXXXXX` + WPA2)。LCD に Wi-Fi QR 表示、
  iPhone Camera スキャン → 接続 → **captive portal が自動で `settings_wifi.html` を開く**
  (DNS hijack + HTTP 404 catch-all)。AP モード中は require_auth が bypass され、
  設定が即触れる。
- **OTA 更新**: デュアル OTA パーティション、起動検証 + ロールバック対応。
  - **BLE 経由**: settings.html → 暗号化された chunk
  - **Wi-Fi ローカル ファイル**: settings_wifi.html で `.bin` をアップロード
  - **Wi-Fi デバイス側 fetch** (v0.7.4+): `POST /api/ota/release {tag}` で device
    自身が GitHub Pages から自分のボード用バイナリをダウンロード → 適用 (STA 必要)

![Pill face states](images/pill-face-states.png)

![AI voice conversation](images/voice-flow.svg)
