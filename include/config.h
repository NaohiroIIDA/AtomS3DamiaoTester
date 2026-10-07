#pragma once

// ============================================================
//  ハードウェア設定
// ============================================================

// ATOMIC CANBus Base (CA-IS3050G) … AtomS3 では G5 = TX, G6 = RX
// 通信できない場合は TX / RX を入れ替えて試すこと
#define CAN_TX_PIN 5
#define CAN_RX_PIN 6

#define SERIAL_BAUD 115200

// ============================================================
//  DAMIAO モーター設定
// ============================================================
// scan コマンドで ID を省略したときの走査範囲
// (1台のみ接続する前提。最初に応答したモーターを使う)
#define DM_SCAN_ID_MIN 0x001
#define DM_SCAN_ID_MAX 0x7FE

// 位置・速度・トルクのマッピング範囲。接続時にモーターの
// レジスタから読み出すが、読めなかった場合はこの値を使う。
#define DM_DEFAULT_PMAX 12.5f  // [rad]
#define DM_DEFAULT_VMAX 8.0f   // [rad/s]  (J4340)
#define DM_DEFAULT_TMAX 28.0f  // [Nm]     (J4340)

// ============================================================
//  動作パラメータ
// ============================================================
#define CONNECT_TIMEOUT_MS 3000  // この時間モーターが見つからなければエラー
#define FEEDBACK_LOST_MS 1000    // 運転中にこの時間応答が無ければ無効化してエラー
#define CONTROL_PERIOD_MS 10     // モーター指令周期

#define DEFAULT_SPEED_DPS 90.0f  // 位置移動の速度上限の初期値 [deg/s]
#define MAX_SPEED_DPS 360.0f     // speed / vel コマンドで指定できる上限 [deg/s]

// 目標位置のソフトリミット [deg] (モーターの PMAX の範囲内にも制限される)
#define TARGET_MIN_DEG -680.0f
#define TARGET_MAX_DEG 680.0f
