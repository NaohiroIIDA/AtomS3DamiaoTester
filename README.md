# AtomS3DamiaoTester

M5Stack **AtomS3** と **ATOMIC CANBus Base** で DAMIAO モーター (DM-J4340-2EC など) をテストするツールです。
PC の USB シリアルから**テキストのコマンド**を送ってモーターを操作します。

[M5DamiaoTester](https://github.com/NaohiroIIDA/M5DamiaoTester) (CoreS3 + PwrCAN、タッチ画面版) を小型化した別バージョンです。
CAN 通信部分 (`DmMotor`) は同じコードを使っています。

> [!CAUTION]
> ## ⚠️ モーターは必ず「CAN 2.0 / 1Mbps」に設定してください
>
> **CAN FD モードのモーターとは通信できません。** ESP32-S3 の CAN 回路は CAN 2.0 専用です。
> 設定の切り替えには、M5DamiaoTester の [tools/dm_baud](https://github.com/NaohiroIIDA/M5DamiaoTester/blob/main/tools/README.md) (CANable 2.0 用 GUI) も使えます。

## ハードウェア

| 機器 | 役割 |
|---|---|
| [M5Stack AtomS3](https://docs.m5stack.com/en/core/AtomS3) | 本体 (ESP32-S3、0.85 インチ画面、画面ボタン) |
| [ATOMIC CANBus Base](https://shop.m5stack.com/products/atomic-canbus-base-ca-is3050g) | 絶縁 CAN トランシーバ (CA-IS3050G、最大 1Mbps) |
| DAMIAO DM-J4340-2EC など | 制御対象のモーター (**CAN 2.0 / 1Mbps に設定すること**) |

### 接続

- AtomS3 を ATOMIC CANBus Base に載せます。CAN は **G5 (TX) / G6 (RX)** です。
- モーターの CANH / CANL / GND をベースの端子台 (HT3.96-4P) につなぎます。
- **ベースには 120Ω 終端抵抗が入っていません。CANH-CANL 間に 120Ω を付けてください。**
  モーター 1 台なら 120Ω 1 本で通信できることを確認しています。抵抗がないと通信できません。
- **AtomS3 の電源は USB-C から取ります。** モーターの 24V は本体には給電されません。

ピン番号などは [include/config.h](include/config.h) で変更できます。

## ビルドと書き込み

[PlatformIO](https://platformio.org/) を使います。

```sh
pio run -t upload     # ビルドして書き込み
pio device monitor    # コマンド入力 (115200bps、Enter で送信)
```

`pio device monitor` は入力した行を Enter で送る設定にしてあります。
Arduino IDE のシリアルモニタなど、ほかのターミナルでも使えます (改行は LF / CR / CRLF のどれでも可)。

## 使い方

```
> scan
# scanning 0x001-0x7FE ...
OK found id=0x001 master=0x011 pmax=12.50 vmax=10.00 tmax=28.00
> on
OK on pos=12.34
> speed 60
OK speed=60.0
> pos 90
OK target=90.00
> s
STAT mode=POS id=0x001 st=1 pos=45.21 vel=59.88 tq=0.120 tgt=90.00 spd=60.0 tmos=35 trot=33 range=ok
> vel -30
OK vel=-30.0
> stop
OK vel=0 (stopped)
> off
OK off
```

### コマンド

角度の単位は **deg**、速度は **deg/s** です。コマンドの大文字・小文字は区別しません。

| コマンド | 内容 |
|---|---|
| `help` / `?` | コマンド一覧 |
| `scan [id]` | モーターを探す。id を省略すると 0x001〜0x7FE を走査し、最初に応答したモーターを使う |
| `info` | ID・Master ID・PMAX / VMAX / TMAX |
| `on` | 有効化 (位置モード)。現在位置で静止する。未検出なら先に `scan` を行う |
| `off` | 無効化 (フリー) |
| `pos <deg>` | 絶対位置へ移動 |
| `move <deg>` | 今の目標位置からの相対移動 |
| `speed <deg/s>` | 位置移動の速度上限 (初期値 90、上限 360) |
| `vel <deg/s>` | 速度モードで回し続ける。`vel 0` で停止 |
| `stop` | その場で停止する。位置モードでは現在位置を保持、速度モードでは速度 0 にする (モードは切り替えない) |
| `clear` | モーターのエラーを解除 (無効状態になる) |
| `zero` | 現在位置を 0 にする (`off` のときのみ) |
| `status` / `s` | 状態を 1 行表示 |
| `mon <ms>` / `mon off` | 状態を一定周期で表示 / 停止 |
| `rr <rid>` | レジスタを読む (例: `rr 0x15` で PMAX) |
| `diag` | CAN バスの診断 (エラーカウンタなど) |

`pos` `move` `vel` `stop` は、`on` で有効化してからでないと使えません。
`pos` と `vel` を切り替えると、モーターの制御モード (POS_VEL / VEL) も切り替わります。
制御モードはフラッシュに保存しないので、モーターの電源を入れ直すと元に戻ります。

### 応答の形式

1 行ずつ、行頭で種類がわかるようにしています。スクリプトから使うときは行頭で判定してください。

| 行頭 | 意味 |
|---|---|
| `OK ...` | コマンドが成功した |
| `ERR ...` | コマンドが失敗した (理由つき) |
| `STAT ...` | 状態 (`key=value` を空白区切り) |
| `EVT ...` | コマンドと関係なく起きた通知 (モーターのエラー、通信途絶、ボタン停止) |
| `# ...` / `[CAN] ...` | ログ・ヘルプ |

`STAT` の項目は次のとおりです。

| 項目 | 内容 |
|---|---|
| `mode` | `OFF` / `POS` / `VEL` |
| `id` | モーターの CAN ID |
| `st` | モーターの状態 (0 無効、1 有効、8 過電圧、9 低電圧、A 過電流、B MOS 過熱、C コイル過熱、D 通信断、E 過負荷) |
| `pos` / `vel` / `tq` | 現在の位置 [deg]、速度 [deg/s]、トルク [Nm] |
| `tgt` | 目標 (位置モードは deg、速度モードは deg/s) |
| `spd` | 位置移動の速度上限 [deg/s] |
| `tmos` / `trot` | MOS / ローターの温度 [℃] |
| `range` | 位置が ±PMAX の範囲内なら `ok`、超えていれば `over` (下記) |

### 安全のための動作

- **本体の画面 (ボタン) を押すと、いつでもモーターを無効化**します。
- 運転中に 1 秒以上モーターから応答がないと、無効化して `EVT feedback lost` を出します。
- モーターがエラー (過電流など) になると、無効化して `EVT motor error` を出します。`clear` のあと `on` で再開します。
- 目標位置は ±680° (設定値) と PMAX の範囲に制限されます。速度は ±360 deg/s までです。

### 速度モードで回しすぎたとき (PMAX 範囲外)

モーターが返す位置は ±PMAX (J4340 は ±12.5 rad ≒ ±716°) の範囲しか表せず、超えると反対側に回り込みます。
この状態で位置モードにすると、モーターは回り込んだ値を目標にして何回転も戻ってしまいます。

そのため、`vel` で回して範囲を超えたときは次のように動作します。

- `EVT position beyond PMAX range` を出し、`STAT` の `range` が `over` になります。
- `pos` / `move` / `on` は `ERR ... beyond PMAX range` で拒否します (速度モードのまま速度 0 で止まります)。
- 位置モードに戻すには、`off` → `zero` → `on` の順に送り、今の位置を 0 にしてください。

`pos` 表示は回り込みを数えて連続した値にしています。ただし、モーターの電源を入れ直したときは `scan` をやり直してください。

### 画面

AtomS3 の画面には、制御モード (OFF / POS / VEL)、現在位置、目標、最後のエラーを表示します。

## 設定 ([include/config.h](include/config.h))

| 項目 | 初期値 | 内容 |
|---|---|---|
| `CAN_TX_PIN` / `CAN_RX_PIN` | 5 / 6 | ATOMIC CANBus Base の CAN ピン |
| `DM_SCAN_ID_MIN` / `DM_SCAN_ID_MAX` | 0x001 / 0x7FE | `scan` の走査範囲 |
| `DM_DEFAULT_PMAX` / `VMAX` / `TMAX` | 12.5 / 8.0 / 28.0 | モーターから読めなかった場合の換算用上限値 |
| `CONNECT_TIMEOUT_MS` | 3000 | モーター探索のタイムアウト [ms] |
| `FEEDBACK_LOST_MS` | 1000 | 運転中の通信途絶判定時間 [ms] |
| `CONTROL_PERIOD_MS` | 10 | モーター指令の周期 [ms] |
| `DEFAULT_SPEED_DPS` / `MAX_SPEED_DPS` | 90 / 360 | 速度上限の初期値 / 最大値 [deg/s] |
| `TARGET_MIN_DEG` / `TARGET_MAX_DEG` | -680 / 680 | 目標位置のソフトリミット [deg] |

## モーター制御の仕組み

| 用途 | CAN ID | データ |
|---|---|---|
| 有効化 / 無効化 / エラー解除 / 原点設定 | モーター ID | `FF FF FF FF FF FF FF FC` / `FD` / `FB` / `FE` |
| 位置速度指令 (10ms 周期) | 0x100 + モーター ID | 目標位置 float + 速度上限 float (リトルエンディアン) |
| 速度指令 (10ms 周期) | 0x200 + モーター ID | 目標速度 float |
| レジスタ読み書き | 0x7FF | `ID_L ID_H 33/55 RID データ(4byte)` |

## トラブルシューティング

`ERR motor not found (...)` の括弧内や `diag` の結果を確認してください。

| 症状 | 原因と対処 |
|---|---|
| **TEC0、REC128、BE が大きい、RX0** | **モーターが CAN FD モード。** CAN 2.0 / 1Mbps に変更する |
| `transmit failed repeatedly (no ACK?)`、BUSOFF を繰り返す | 応答がない。モーターの電源、CANH/CANL の配線、**120Ω 終端抵抗**を確認する。それでもだめなら config.h の TX / RX を入れ替えて試す |
| TEC0、REC0、RX0 | モーターは信号を受けているが応答しない。モーターのボーレート設定を確認する |
| コマンドに何も応答しない | シリアルの速度 (115200) と改行の設定を確認する。書き込み直後は USB の再接続に数秒かかる |

## ソース構成

```
platformio.ini      ビルド設定 (M5Unified)
include/config.h    ピン・パラメータ設定
src/DmMotor.h/.cpp  DAMIAO モーターの CAN 通信 (ESP32 TWAI)
src/main.cpp        シリアルコマンド・画面表示・制御
```

## 注意

- AtomS3 + ATOMIC CANBus Base + DM-J4340 で、位置モード・速度モードとも動作を確認しています (CAN ピンは初期値の G5 = TX / G6 = RX、終端抵抗は 120Ω 1 本)。
- 初めて動かすときは、モーターを負荷から外し、すぐ電源を切れる状態で試してください。
