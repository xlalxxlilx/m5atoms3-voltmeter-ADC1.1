# M5AtomS3 Voltmeter Project

M5AtomS3とATOMIC TF Card Reader、Voltmeter Unitを使用した電圧測定・記録システムです。

## Features

- 📊 **リアルタイム電圧測定**: Unit ADC v1.1 (ADS1110) を使用した高精度電圧測定
- 🎨 **滑らかな画面表示**: スプライト方式による高速描画（約50fps）
- 💾 **SDカード記録**: CSV形式で0.5秒間隔の自動記録
- 📡 **クラウド連携**: WiFi経由でGoogle Apps Scriptへ自動POST（1秒間隔）
- 🕐 **NTP時刻同期**: WiFi接続時は自動時刻同期、オフライン時は内部RTC使用
- ⚡ **非同期処理**: FreeRTOSマルチタスクによる高速レスポンス

## Hardware

### 必要な機器

- **M5AtomS3** (ESP32-S3)
- **ATOMIC TF Card Reader** (SDカード用)
- **M5Stack Unit ADC v1.1** (ADS1110搭載, 0-12V入力)
- **microSDカード**

### ピン接続

**ATOMIC TF Card Base (SPI接続):**
- CS: -1 (自動)
- MOSI: GPIO6
- MISO: GPIO8
- SCK: GPIO7

**Voltmeter Unit (I2C接続、Grove Port A):**
- SDA: GPIO2
- SCL: GPIO1
- I2Cアドレス: 0x48 (ADS1110)

## Setup

### 1. PlatformIO環境のセットアップ

```bash
# リポジトリをクローン
git clone <your-repository-url>
cd m5atoms3

# 依存関係のインストール（platformio.iniから自動）
pio lib install
```

### 2. 設定ファイルの作成

SDカードのルートに`config.txt`を作成します：

```ini
ssid=your_wifi_ssid
password=your_wifi_password
machine_id=ATOMS3-01
script_url=https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec
full_scale_V=3.0
full_scale_P=100.0
unit_P=%
voltage_offset=0
```

**パラメータ説明:**
- `ssid`: WiFi SSID
- `password`: WiFiパスワード
- `machine_id`: 機器識別ID
- `script_url`: Google Apps ScriptのWebアプリURL
- `full_scale_V`: フルスケール電圧（V単位）
- `full_scale_P`: フルスケール時の表示値
- `unit_P`: 表示値の単位
- `voltage_offset`: 電圧入力のゼロ点補正値（V、既定: 0）

**計算式:** `表示値 = (測定電圧 / full_scale_V) × full_scale_P`

### 3. ビルドとアップロード

```bash
# ビルド
pio run

# アップロード
pio run --target upload

# シリアルモニタ
pio device monitor
```

## Usage

### 起動時の動作

1. M5AtomS3が起動すると自動で以下を実行：
   - スプライト（ダブルバッファ）初期化
   - MACアドレス表示
   - タイムゾーン設定（JST: UTC+9）
   - SDカードから設定読み込み
   - WiFi接続試行（10秒タイムアウト）
   - **成功時**: NTPで時刻同期、Google Apps Scriptへrebootステータス送信
   - **失敗時**: RTCを2000-01-01 00:00:00に設定

### 記録操作

- **ボタン押下**: 記録開始
  - LEDが3回点滅
  - 画面に`[REC]`表示
  - CSVファイルへ0.5秒間隔で記録
  - WiFi接続時は1秒間隔でPOST送信
- **停止**: デバイスをリセット

### 画面表示

```
[REC]               ← 記録中のみ表示
1.234V             ← 測定電圧
45.678             ← 換算値（大きく表示）
%                  ← 単位
```

### データ形式

**CSV (LOGDATA.csv):**
```csv
timestamp,voltage,scaled_value,unit
2024-01-01 12:00:00,1.234,45.678,%
2024-01-01 12:00:00,1.235,45.712,%
```

**HTTP POST (JSON):**
```json
{
  "machine_id": "ATOMS3-01",
  "status": "rec",
  "unit_P": "%",
  "pressure": "45.7",
  "voltage": "1.234",
  "timestamp": "2024-01-01 12:00:00"
}
```

## Architecture

### マルチタスク構成

- **Core 1 (Main)**: ループ制御、画面更新（約50fps）
- **Core 0 (Background)**:
  - `PostTask`: HTTP POSTリクエスト処理
  - `RecordTask`: CSV記録処理

### 最適化ポイント

1. **スプライト方式**: オフスクリーンバッファで描画してから一括転送（ちらつき防止）
2. **非同期処理**: SDカード書き込みとHTTP通信を別タスクで実行
3. **高速時刻取得**: WiFi未接続時は`localtime()`を使用（タイムアウト回避）

## Project Structure

```
m5atoms3/
├── .github/
│   └── copilot-instructions.md    # GitHub Copilot設定
├── src/
│   └── main.cpp                    # メインプログラム
├── platformio.ini                  # PlatformIO設定
├── config.txt.example              # 設定ファイルサンプル
├── .gitignore                      # Git除外設定
└── README.md                       # このファイル
```

## Dependencies

以下のライブラリが自動的にインストールされます（platformio.iniに記載）:

- M5AtomS3
- M5Unified
- ArduinoJson
- WiFi
- HTTPClient
- SD
- SPI
- Wire

## Troubleshooting

### WiFi接続失敗時

- 設定ファイルのSSID/パスワードを確認
- WiFiルーターの2.4GHz帯を確認（5GHzは非対応）
- タイムスタンプは2000-01-01から開始されます（正常動作）

### SDカード読み込みエラー

- SDカードのフォーマット確認（FAT32推奨）
- ピン接続確認
- 別のSDカードで試す

### GPIO 227エラー

- M5AtomS3ライブラリ内部の無害な警告です（無視してOK）

## License

このプロジェクトはMITライセンスの下で公開されています。

## Author

Created for M5AtomS3 Voltmeter Application