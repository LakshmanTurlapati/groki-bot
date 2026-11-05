[English](quick-start.md) | [中文](quick-start.zh-CN.md) | [日本語](quick-start.ja.md)

# クイックスタート：Grok Bot で会話する（macOS のみ）

必要なもの：Groki Bot 本体（CoreS3 + Stack-chan ベース）、USB-C ケーブル、デスクトップ版 Chrome または Edge、同じ Wi-Fi 上で Grok Bot アプリにログイン済みの Mac（動作確認バージョン 0.71.2、内部 API を使用しているためアプリのアップデートにより動作しなくなる可能性があります）、Node.js、[uv](https://docs.astral.sh/uv/)。

1. ファームウェアの書き込みと Wi-Fi 設定：[path 1](voice-backends.md#path-1-firmware-only) のステップ 1 と 2 を行います。API キーの入力はスキップできます。
2. Mac にゲートウェイをインストール：本リポジトリを `git clone` し、`cd groki-bot/gateway`、`uv sync --all-extras`、`cp .env.example .env` を実行します。
3. [Grok Bot ネイティブ音声の設定](#grok-bot-ネイティブ音声の設定) に従って、`gateway/.env` の 4 項目を設定します。
4. 別のターミナルで [Grok Bot ネイティブ音声の設定](#grok-bot-ネイティブ音声の設定) の完全なコマンドを実行し、ログインヘルパーを常駐させます。
5. [path 2](voice-backends.md#path-2-firmware--gateway) と同様にゲートウェイを起動し、BLE 設定ページでロボットの接続先を `ws://<コンピュータの LAN IP>:8765` に設定して、「Hey Grok」と話しかけます。

デフォルトのバックエンドは Gemini のままです。`STACKCHAN_VOICE_BACKEND=gemini` を設定することでいつでも戻せます。その他の音声オプション（OpenAI Realtime、Gemini Live、`gbot` CLI によるタスク転送）は後述の [他の音声バックエンド](voice-backends.ja.md#他の音声バックエンド) を参照してください。

### Grok Bot ネイティブ音声の設定

コードの既定の音声バックエンドは Gemini のままで Grok には自動で切り替わらないため、Grok Bot ネイティブ音声を使うには `gateway/.env` で自分で切り替えてください。

ネイティブ音声は macOS のみで、Grok Bot デスクトップアプリの内部 API（検証バージョン 0.71.2）を使うためアプリ更新で動作しなくなる可能性があり、動作しない場合や利用をやめる場合は Gemini の API キーを残したうえで `STACKCHAN_VOICE_BACKEND=gemini` に変更してゲートウェイを再起動すれば、ファームウェアの再書き込みは不要です。

`gateway/.env` に次の 4 行を追加し、名前と ID を自分のものに置き換えてください。

```dotenv
STACKCHAN_VOICE_BACKEND=gbot
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID
STACKCHAN_TOOL_BOT=assistant
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID
```

- `STACKCHAN_VOICE_BACKEND=gbot` は Grok 音声バックエンドを選択します。
- `STACKCHAN_GBOT_VOICE_BOT_ID` は音声セッションで使うエージェントの ID です。
- `STACKCHAN_TOOL_BOT` は自分で付けたエージェント名で、コードは空でないことだけを確認し、アプリ内で名前を使った検索は行いません。
- `STACKCHAN_TOOL_BOT_ID` はデスクトップタスクで使うエージェントの ID で、音声用の ID と一文字ずつ一致させる必要があり、大文字、小文字、空白は自動で正規化されません。

自分のエージェント名と、その同じエージェントの ID を使ってください。任意の `gbot` CLI がある場合は、次のコマンドで確認できます。

```sh
gbot bots list
gbot bots list --json
gbot bots get "<名前または ID>"
```

これらのコマンドは、この Mac 上の Grok Bot エージェントディレクトリを読みます。ディレクトリが見つからない場合はエラーとなり、`--dir` または環境変数 `GROK_BOT_AGENTS_DIR` で場所を指定するよう表示されます（2026-10-18、gbot 0.11.3 で確認）。`--json` は JSON 出力を指定しますが、ヘルプには出力の列名やフィールド名は記載されていません。ID を取得できない場合は先に進まず、Grok Bot アプリまたは自分のエージェント設定で ID を確認してください。ID があれば、ネイティブ音声の実行自体に CLI は不要です。

同じ Mac の Grok Bot デスクトップアプリにログインしてください。別のターミナルを開き、`gateway/` で同じ名前と ID を渡して次の完全なコマンドを実行し、そのターミナルを開いたままにしてください。

```sh
STACKCHAN_GBOT_VOICE_BOT_ID=YOUR_AGENT_ID \
STACKCHAN_TOOL_BOT=assistant \
STACKCHAN_TOOL_BOT_ID=YOUR_AGENT_ID \
node scripts/gbot_voice_session_bridge.mjs
```

ログインヘルパーは `.env` を読み込まないため、この 3 つの環境変数を明示的に渡します。待ち受け先は `127.0.0.1:18771` のみです。

任意設定：`STACKCHAN_GBOT_VOICE_LANGUAGE` の既定値は `zh` で、必要に応じて `gateway/.env` で `en` または `auto` に変更できます。その他の詳細設定は [gateway setup](../gateway/README.md#grok-bot-login-voice-macos) を参照してください。

ゲートウェイ起動ログに `voice_backend=gbot` があることを確認し、別のターミナルからログインヘルパーも確認してください。

```sh
curl http://127.0.0.1:18771/health
```

ヘルパーが動いていれば `"ok": true` が返ります。この 2 つはバックエンドの選択とヘルパーの設定を確認するもので、ログインの有効性や音声接続の成功を保証しません。ロボットに「Hey Grok」と話しかけ、応答が聞こえることを確認してください。

Gemini に戻すには、`STACKCHAN_VOICE_BACKEND=gemini` に変更するかその行を削除し、有効な `GEMINI_API_KEY` を残してゲートウェイを再起動してください。別の起動コマンドやサービスの環境でも `gbot` を指定している場合は、そちらも変更してください。独立したログインヘルパーは自動終了しないため、不要になったらそのターミナルで終了してください。

Grok 音声の起動が失敗してもゲートウェイは Gemini に自動で戻らず、ロボットがオンライン表示でも音声が出ない場合があり、ログには `Grok voice startup failed` が出るため、その場合は上記の手順で Gemini に戻してください。

### Grok Bot のログイン状態で音声対話

macOS でログイン済みの Grok Bot アプリがあれば、ゲートウェイの `STACKCHAN_VOICE_BACKEND=gbot` を指定して音声対話を利用できます。別途 xAI 音声 API キーを設定する必要はありません。デフォルトは Gemini のままで、`STACKCHAN_VOICE_BACKEND=gemini` に戻せます。ゲートウェイの切り替えにファームウェアの再書き込みは不要です。

Grok 側はネイティブの `send_task`、`search_conversations`、`end_the_call` と共通のローカル機器ツールを直接使用します。`gbot` CLI は不要で、CLI 転送サービスは Gemini 側のオプション連携機能として残されています。[セットアップ手順](../gateway/README.md#grok-bot-login-voice-macos)（英語）を参照してください。

実機検証において、タスクの受付、完了結果の音声モデルへの伝達、およびモデルによる出力音声の生成は確認済みです。実機スピーカーでの具体的な発話内容も正常であることを確認しました（2026-10-18）。デスクトップのテキストメッセージ購読は一時的にエラーとなる場合があり、その際は個別に再接続されます（切断中に届かなかったメッセージの再送は行われません）。CoreS3 ファームウェアは発話中にマイク入力を停止する場合があるため、声だけの割り込みが動作するとは保証できません。詳細は[検証範囲](../gateway/GBOT_VOICE_VALIDATION.md)を参照してください。
