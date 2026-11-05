[English](repository.md) | [中文](repository.zh-CN.md) | [日本語](repository.ja.md)

# Groki Bot

Groki Bot デスクトップロボットのファームウェア、ゲートウェイ、Grok Bot 連携をまとめたリポジトリです。M5Stack の CoreS3 / AtomS3R / AtomS3 / StopWatch (C152) で動く
Stack-chan ファームウェア。ESP-IDF 5.5 / C++20。AI 音声対話 (OpenAI / Gemini / XiaoZhi)、
BLE / Wi-Fi / SoftAP の 3 経路設定、デバイス側 OTA をサポートします。

Groki は Grok bot バックエンドによるネイティブ音声に対応し、デスクトップアプリと連携してロボットと直接会話できるようになりました。Grok Bot にログイン済みの Mac であれば既存のログイン状態で音声を利用でき、追加の xAI 音声 API キー設定やファームウェアの再書き込みは不要です。

> PC 側のゲートウェイ（Gemini / Grok ログイン音声、ウェイクワード、MCP、ネイティブ Grok Bot 連携）は [gateway/](../gateway/README.md)、
> 全体の構成と通信は [docs/architecture.md](architecture.md)（英語）を参照してください。
> ウェイクワードは既定で「Hey Grok」、`gateway/.env` に `STACKCHAN_WAKE_PHRASE=hey groki` を設定すると「Hey Groki」になります。

## 起動シーケンス (CoreS3 標準パス)

1. M5 / Avatar 初期化、起動音 (C5–E5–G5 アルペジオ、設定で OFF 可)
2. NVS から設定読み込み → BLE 設定サービス起動 (常時 advertising)
3. SSID があれば Wi-Fi STA 接続を非ブロッキング開始
   - STA 接続後 → mDNS + HTTP 設定サーバー + SNTP 開始
4. マイク loopback (2 秒録音 → 再生) で動作確認
5. サーボ電源 ON → ping (Yaw / Pitch) → 1.5 s 待機
6. Render Task (顔描画 30 fps, core 1) + Servo Task (20 ms 周期, core 0) を起動。
   会話が有効なら Conversation Task が Wi-Fi 接続を待って AI 対話を開始
7. demo_loop 開始 — 会話アイドル時はランダム babble + ランダム ポーズ +
   なでなで反応、会話中は AI 対話タスクがアバターと音声を制御
8. 画面右上タップでオンデバイス UI、応答中の画面タップで barge-in、
   操作タブの「AP モード」で SoftAP プロビジョニング

## リポジトリ構成

```
.
├── components/
│   ├── avatar/             顔描画 + アニメーション (Breath / Saccade / Blink、6 表情)
│   ├── avatar_vm/          Avatar DSL バイトコード VM + ストレージ
│   ├── board/              CoreS3 / AtomS3R / StopWatch HW 初期化 (ボード自動検出)
│   ├── scs_servo/          SCS0009 ドライバ + PathGenerator (台形速度)
│   ├── jtts/               日本語カタコト TTS (babble / speak_katakoto / LT 通知)
│   ├── conversation/       AI 音声対話クライアント (OpenAI / Gemini / XiaoZhi)
│   ├── config_service/     BLE GATT 設定サービス + NVS + OTA + X25519/AES-GCM
│   ├── wifi_config_service/ Wi-Fi HTTP 設定 + 内蔵 Web ページ + release OTA
│   ├── telegram/           Telegram Bot API (TLS) 通知クライアント (oss)
│   ├── M5GFX/              submodule (upstream)
│   ├── M5Unified/          submodule (upstream + 1 patch)
│   └── tl_expected/        tl::expected backport (submodule)
├── main/                   app_main, render/servo task, demo_loop, ap_screen,
│                           captive_portal, device_ui, atom_status, wifi_sta
├── patches/                upstream-targeted patches
├── tools/                  apply-m5-patches.sh, monitor_log.py, settings.html,
│                           avatar_dsl/ (コンパイラ + WASM 連携),
│                           vision-tracker/ (Mac の顔追従プログラム、Swift)
├── assets/                 .avdsl ソース (default_face, omega_mouth, aokko_face, grok_face)
├── partitions.csv          OTA 配置 (ota_0 / ota_1 / nvs / storage)
├── sdkconfig.defaults*     共通 + ボード別 (.cores3 / .atoms3r / .atoms3 / .stopwatch)
└── Makefile                idf.py の薄いラッパ (BOARD= 切替)
```

## ライセンス

このリポジトリの自前ソース (`components/board`, `components/scs_servo`, `components/avatar`,
`components/avatar_vm`, `components/groki_motion`, `components/jtts`, `components/conversation`,
`components/config_service`, `components/wifi_config_service`,
`components/telegram`, `main`, `tools`) は
**Boost Software License 1.0** ([LICENSE](../LICENSE)) の下で配布されます。

Submodule (`components/M5GFX` / `components/M5Unified` / `components/tl_expected/expected`)
と managed_components (`espressif/esp_audio_codec` / `espressif/esp_websocket_client` /
`espressif/mdns` / `espressif/esp_jpeg` / `espressif/esp32-camera` 等)
はそれぞれの upstream ライセンスに従います。

### 第三者ソフトウェア・音声データの帰属表示

HMM 音声合成に使う **hts_engine API** (Modified BSD / 名古屋工業大学・東京工業大学)
と、同梱・配布する **HMM ボイス "Mei"** (CC BY 3.0 / 名古屋工業大学・MMDAgent
Project Team) をはじめとする第三者コンポーネントの帰属表示は
**[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)** にまとめています。
HTML 版 (Web フラッシャー・設定ページからも参照可):
<https://josssphzhou.github.io/groki-bot/licenses.html>。
