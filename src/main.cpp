// DAMIAO モーター テストツール (USB シリアル コマンド版)
//   M5Stack AtomS3 + ATOMIC CANBus Base (CA-IS3050G)
//
// USB シリアル (115200bps) から 1 行 1 コマンドで操作する。
// 応答の行頭: OK / ERR = コマンドの結果, STAT = 状態, EVT = 非同期の通知, # = ログ

#include <M5Unified.h>

#include "DmMotor.h"
#include "config.h"

// ------------------------------------------------------------
//  状態
// ------------------------------------------------------------
static constexpr float kDegToRad = PI / 180.0f;
static constexpr float kRadToDeg = 180.0f / PI;

enum class Mode { Off, Pos, Vel };

static DmMotor motor;
static M5Canvas canvas(&M5.Display);

static bool found = false;  // モーター検出済み
static Mode mode  = Mode::Off;

static float targetPos  = 0;                               // [rad]
static float speedLimit = DEFAULT_SPEED_DPS * kDegToRad;  // [rad/s] 位置移動の速度上限
static float targetVel  = 0;                               // [rad/s] 速度モードの目標

static uint32_t monPeriodMs = 0;  // 0 = モニタ停止
static uint8_t lastStatus   = 0;
static String lastError;          // 画面表示用

static float radToDeg(float r) { return r * kRadToDeg; }

static const char *modeName(Mode m)
{
    switch (m) {
        case Mode::Pos: return "POS";
        case Mode::Vel: return "VEL";
        default:        return "OFF";
    }
}

static void reply(bool ok, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.printf("%s %s\n", ok ? "OK" : "ERR", buf);
    if (!ok) lastError = buf;
}

// 目標位置のソフトリミット (設定値と PMAX の両方で制限)
static float clampTarget(float rad)
{
    float lo = max(TARGET_MIN_DEG * kDegToRad, -motor.pmax());
    float hi = min(TARGET_MAX_DEG * kDegToRad, motor.pmax());
    return constrain(rad, lo, hi);
}

static void printStatus()
{
    Serial.printf("STAT mode=%s id=0x%03X st=%X pos=%.2f vel=%.2f tq=%.3f tgt=%.2f spd=%.1f tmos=%d trot=%d range=%s\n",
                  modeName(mode), motor.canId(), motor.status(), radToDeg(motor.position()),
                  radToDeg(motor.velocity()), motor.torque(),
                  mode == Mode::Vel ? radToDeg(targetVel) : radToDeg(targetPos), radToDeg(speedLimit),
                  motor.tempMos(), motor.tempRotor(), motor.positionInRange() ? "ok" : "over");
}

// 無効状態でフィードバックを取得する (無効化コマンドへの応答を使う。モーターは動かない)
static bool refreshWhileOff(uint32_t timeoutMs = 50)
{
    motor.disable();
    return motor.waitFeedback(timeoutMs);
}

// ------------------------------------------------------------
//  モーター制御
// ------------------------------------------------------------
static void disableMotor()
{
    for (int i = 0; i < 3; i++) {  // 確実にフリーにする
        motor.disable();
        delay(5);
    }
    motor.poll();
    mode = Mode::Off;
}

static bool connectMotor(uint16_t minId, uint16_t maxId)
{
    if (mode != Mode::Off) disableMotor();
    found = false;

    Serial.printf("# scanning 0x%03X-0x%03X ...\n", minId, maxId);
    float pmax        = 0;
    uint32_t deadline = millis() + CONNECT_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        if (motor.scan(minId, maxId, &pmax, deadline)) {
            found = true;
            break;
        }
    }
    if (!found) {
        motor.printDiag();
        reply(false, "motor not found (%s)", motor.diagText().c_str());
        return false;
    }

    float vmax = DM_DEFAULT_VMAX, tmax = DM_DEFAULT_TMAX;
    if (!(pmax > 0.1f && pmax < 1000.0f)) pmax = DM_DEFAULT_PMAX;
    if (!motor.readFloat(DmMotor::RID_VMAX, &vmax) || !(vmax > 0)) vmax = DM_DEFAULT_VMAX;
    if (!motor.readFloat(DmMotor::RID_TMAX, &tmax) || !(tmax > 0)) tmax = DM_DEFAULT_TMAX;
    motor.setLimits(pmax, vmax, tmax);
    refreshWhileOff();
    lastStatus = motor.status();
    lastError  = "";
    reply(true, "found id=0x%03X master=0x%03X pmax=%.2f vmax=%.2f tmax=%.2f", motor.canId(), motor.masterId(),
          pmax, vmax, tmax);
    return true;
}

static bool ensureFound()
{
    if (found) return true;
    return connectMotor(DM_SCAN_ID_MIN, DM_SCAN_ID_MAX);
}

// 制御モードを切り替える (RAM のみ。フラッシュには保存しないので電源を入れ直すと元に戻る)
static bool setCtrlMode(uint32_t m)
{
    if (motor.writeU32(DmMotor::RID_CTRL_MODE, m, 100)) return true;
    reply(false, "ctrl mode write failed");
    return false;
}

// 無効状態から有効化する。現在位置で静止 (POS) または速度 0 (VEL)。
static bool enableFromOff(Mode m)
{
    if (!setCtrlMode(m == Mode::Vel ? DmMotor::MODE_VEL : DmMotor::MODE_POS_VEL)) return false;
    motor.clearError();
    delay(5);
    if (!refreshWhileOff(200)) {
        reply(false, "no feedback");
        return false;
    }
    if (m == Mode::Pos && !motor.positionInRange()) {
        reply(false, "position %.0f deg is beyond PMAX range (run 'zero' first)", radToDeg(motor.position()));
        return false;
    }
    targetPos = clampTarget(motor.position());
    targetVel = 0;
    motor.enable();
    delay(2);
    if (m == Mode::Vel) motor.sendVel(0);
    else motor.sendPosVel(targetPos, speedLimit);
    if (!motor.waitFeedback(200)) {
        disableMotor();
        reply(false, "no feedback after enable");
        return false;
    }
    mode       = m;
    lastStatus = motor.status();
    lastError  = "";
    return true;
}

// 有効状態のまま POS / VEL を切り替える
static bool switchMode(Mode m)
{
    if (mode == m) return true;
    if (mode == Mode::Off) return enableFromOff(m);

    if (mode == Mode::Vel) {
        targetVel = 0;
        motor.sendVel(0);
        motor.waitFeedback(20);
        if (m == Mode::Pos && !motor.positionInRange()) {
            // 位置が正しく表せないので POS にすると大きく動いてしまう。VEL のまま止める
            reply(false, "position %.0f deg is beyond PMAX range: stopped in VEL (run 'off', 'zero', 'on')",
                  radToDeg(motor.position()));
            return false;
        }
    }
    if (!setCtrlMode(m == Mode::Vel ? DmMotor::MODE_VEL : DmMotor::MODE_POS_VEL)) {
        disableMotor();
        return false;
    }
    motor.poll();
    targetPos = clampTarget(motor.position());
    targetVel = 0;
    mode      = m;
    return true;
}

static bool requireEnabled()
{
    if (mode != Mode::Off) return true;
    reply(false, "motor is off (run 'on' first)");
    return false;
}

// ------------------------------------------------------------
//  コマンド
// ------------------------------------------------------------
static bool parseFloat(const char *s, float *out)
{
    if (!s) return false;
    char *end;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

static bool parseUInt(const char *s, uint32_t *out)
{
    if (!s) return false;
    char *end;
    unsigned long v = strtoul(s, &end, 0);  // 0x 付きなら 16 進
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

static void printHelp()
{
    Serial.print(
        "# ---- コマンド一覧 (角度は deg、速度は deg/s) ----\n"
        "#  help | ?           この一覧\n"
        "#  scan [id]          モーターを探す (id 省略時は全 ID を走査)\n"
        "#  info               ID と上限値\n"
        "#  on                 有効化 (位置モード、現在位置で静止)\n"
        "#  off                無効化 (フリー)\n"
        "#  pos <deg>          絶対位置へ移動\n"
        "#  move <deg>         相対移動\n"
        "#  speed <deg/s>      位置移動の速度上限\n"
        "#  vel <deg/s>        速度モードで回転 (0 で停止)\n"
        "#  stop               その場で停止 (速度モード中は速度 0)\n"
        "#  clear              モーターのエラー解除 (無効化される)\n"
        "#  zero               現在位置を 0 にする (off のときのみ)\n"
        "#  status | s         状態を 1 行表示\n"
        "#  mon <ms> | mon off 状態の定期表示\n"
        "#  rr <rid>           レジスタ読み出し (例: rr 0x15)\n"
        "#  diag               CAN バスの診断\n"
        "# 本体の画面 (ボタン) を押すと、いつでも無効化します\n"
        "# st: 0=無効 1=有効 8=過電圧 9=低電圧 A=過電流 B=MOS過熱 C=コイル過熱 D=通信断 E=過負荷\n");
}

static void handleCommand(char *line)
{
    char *argv[4] = {};
    int argc      = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 4; tok = strtok(nullptr, " \t")) argv[argc++] = tok;
    if (argc == 0) return;

    String cmd = argv[0];
    cmd.toLowerCase();
    const char *arg = argv[1];
    float f;
    uint32_t u;

    if (cmd == "help" || cmd == "?") {
        printHelp();

    } else if (cmd == "scan") {
        if (!arg) connectMotor(DM_SCAN_ID_MIN, DM_SCAN_ID_MAX);
        else if (parseUInt(arg, &u) && u >= 1 && u <= 0x7FE) connectMotor(u, u);
        else reply(false, "usage: scan [id]  (1..0x7FE)");

    } else if (cmd == "info") {
        if (!found) return reply(false, "motor not found (run 'scan')");
        reply(true, "id=0x%03X master=0x%03X pmax=%.2f vmax=%.2f tmax=%.2f mode=%s", motor.canId(),
              motor.masterId(), motor.pmax(), motor.vmax(), motor.tmax(), modeName(mode));

    } else if (cmd == "on") {
        if (!ensureFound()) return;
        if (mode != Mode::Off) return reply(true, "already on (%s)", modeName(mode));
        if (enableFromOff(Mode::Pos)) reply(true, "on pos=%.2f", radToDeg(targetPos));

    } else if (cmd == "off") {
        if (!found) return reply(true, "off");
        disableMotor();
        reply(true, "off");

    } else if (cmd == "pos" || cmd == "move") {
        if (!parseFloat(arg, &f)) return reply(false, "usage: %s <deg>", cmd.c_str());
        if (!requireEnabled() || !switchMode(Mode::Pos)) return;
        float t   = f * kDegToRad + (cmd == "move" ? targetPos : 0);
        targetPos = clampTarget(t);
        if (targetPos != t) reply(true, "target=%.2f (limited)", radToDeg(targetPos));
        else reply(true, "target=%.2f", radToDeg(targetPos));

    } else if (cmd == "speed") {
        if (!parseFloat(arg, &f) || f <= 0) return reply(false, "usage: speed <deg/s>  (>0)");
        speedLimit = min(f, MAX_SPEED_DPS) * kDegToRad;
        reply(true, "speed=%.1f", radToDeg(speedLimit));

    } else if (cmd == "vel") {
        if (!parseFloat(arg, &f)) return reply(false, "usage: vel <deg/s>");
        if (!requireEnabled() || !switchMode(Mode::Vel)) return;
        targetVel = constrain(f, -MAX_SPEED_DPS, MAX_SPEED_DPS) * kDegToRad;
        reply(true, "vel=%.1f", radToDeg(targetVel));

    } else if (cmd == "stop") {
        if (!requireEnabled()) return;
        if (mode == Mode::Vel) {
            // VEL のまま速度 0 で止める (POS に切り替えると位置が飛ぶことがあるため)
            targetVel = 0;
            return reply(true, "vel=0 (stopped)");
        }
        motor.poll();
        targetPos = clampTarget(motor.position());
        reply(true, "hold pos=%.2f", radToDeg(targetPos));

    } else if (cmd == "clear") {
        if (!ensureFound()) return;
        if (mode != Mode::Off) disableMotor();
        motor.clearError();
        delay(5);
        refreshWhileOff();
        lastStatus = motor.status();
        lastError  = "";
        reply(true, "cleared st=%X", motor.status());

    } else if (cmd == "zero") {
        if (!ensureFound()) return;
        if (mode != Mode::Off) return reply(false, "run 'off' first");
        motor.setZero();
        delay(20);
        refreshWhileOff();
        reply(true, "zero set pos=%.2f", radToDeg(motor.position()));

    } else if (cmd == "status" || cmd == "s") {
        if (!found) return reply(false, "motor not found (run 'scan')");
        if (mode == Mode::Off) refreshWhileOff();
        printStatus();

    } else if (cmd == "mon") {
        if (arg && String(arg) == "off") {
            monPeriodMs = 0;
            reply(true, "mon off");
        } else if (parseUInt(arg, &u) && u >= 10) {
            monPeriodMs = u;
            reply(true, "mon %lu ms", (unsigned long)u);
        } else {
            reply(false, "usage: mon <ms>(>=10) | mon off");
        }

    } else if (cmd == "rr") {
        if (!parseUInt(arg, &u) || u > 0xFF) return reply(false, "usage: rr <rid>");
        if (!ensureFound()) return;
        uint8_t b[4];
        if (!motor.readRegister(u, b, 100)) return reply(false, "no reply rid=0x%02X", (unsigned)u);
        uint32_t raw;
        float fv;
        memcpy(&raw, b, 4);
        memcpy(&fv, b, 4);
        reply(true, "rid=0x%02X u32=%lu hex=0x%08lX float=%g", (unsigned)u, (unsigned long)raw,
              (unsigned long)raw, fv);

    } else if (cmd == "diag") {
        motor.printDiag();
        reply(true, "%s", motor.diagText().c_str());

    } else {
        reply(false, "unknown command '%s' (type 'help')", argv[0]);
    }
}

static void readSerial()
{
    static char buf[96];
    static size_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (len == 0) continue;
            buf[len] = '\0';
            len      = 0;
            handleCommand(buf);
        } else if (c == '\b' || c == 0x7F) {
            if (len) len--;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ------------------------------------------------------------
//  画面 (128x128)
// ------------------------------------------------------------
static void drawScreen()
{
    char buf[32];
    canvas.fillScreen(TFT_BLACK);
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(top_left);

    canvas.fillRect(0, 0, 128, 18, 0x18E3);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("DM Tester", 4, 1);
    if (found) {
        snprintf(buf, sizeof(buf), "%03X", motor.canId());
        canvas.setTextDatum(top_right);
        canvas.setTextColor(TFT_YELLOW);
        canvas.drawString(buf, 124, 1);
    }

    uint16_t bg = mode == Mode::Pos ? TFT_DARKGREEN : mode == Mode::Vel ? TFT_BLUE : 0x4208;
    canvas.fillRoundRect(4, 22, 120, 26, 4, bg);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(TFT_WHITE);
    canvas.setFont(&fonts::Font4);
    canvas.drawString(found ? modeName(mode) : "NO MOTOR", 64, 36);

    canvas.setFont(&fonts::Font4);
    canvas.setTextDatum(top_right);
    canvas.setTextColor(found ? TFT_CYAN : TFT_DARKGREY);
    if (found) snprintf(buf, sizeof(buf), "%.1f", radToDeg(motor.position()));
    else snprintf(buf, sizeof(buf), "---");
    canvas.drawString(buf, 114, 54);
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(top_left);
    canvas.drawString("o", 116, 52);

    canvas.setTextColor(TFT_LIGHTGREY);
    if (mode == Mode::Vel) snprintf(buf, sizeof(buf), "tgt %.1f deg/s", radToDeg(targetVel));
    else if (mode == Mode::Pos) snprintf(buf, sizeof(buf), "tgt %.1f deg", radToDeg(targetPos));
    else buf[0] = '\0';
    canvas.drawString(buf, 4, 82);

    if (found && motor.hasError()) {
        canvas.fillRect(0, 104, 128, 24, TFT_RED);
        canvas.setTextColor(TFT_WHITE);
        snprintf(buf, sizeof(buf), "MOTOR ERR st=%X", motor.status());
        canvas.drawString(buf, 4, 108);
    } else if (!lastError.isEmpty()) {
        canvas.fillRect(0, 104, 128, 24, 0x7800);
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString(lastError.substring(0, 20).c_str(), 4, 108);
    } else if (found) {
        canvas.setTextColor(TFT_DARKGREY);
        snprintf(buf, sizeof(buf), "%.2fNm %dC/%dC", motor.torque(), motor.tempMos(), motor.tempRotor());
        canvas.drawString(buf, 4, 108);
    }

    canvas.pushSprite(0, 0);
}

// ------------------------------------------------------------
void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(SERIAL_BAUD);

    canvas.setColorDepth(16);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    if (!motor.beginBus(CAN_TX_PIN, CAN_RX_PIN)) lastError = "CAN init failed";
    drawScreen();

    delay(500);  // USB シリアルの接続待ち
    Serial.println("# DAMIAO motor tester (AtomS3 + ATOMIC CANBus Base). type 'help'");
}

void loop()
{
    M5.update();
    readSerial();

    // 本体ボタン = 非常停止 (無効化)
    if (M5.BtnA.wasPressed() && mode != Mode::Off) {
        disableMotor();
        Serial.println("EVT button: motor disabled");
        lastError = "button stop";
    }

    static uint32_t lastCtrl = 0, lastMon = 0, lastDraw = 0;
    uint32_t now = millis();

    // モーター指令
    if (mode != Mode::Off && now - lastCtrl >= CONTROL_PERIOD_MS) {
        lastCtrl = now;
        motor.poll();
        if (mode == Mode::Vel) motor.sendVel(targetVel);
        else motor.sendPosVel(targetPos, speedLimit);

        if (motor.status() != lastStatus) {
            lastStatus = motor.status();
            Serial.printf("EVT status=%X (%s)\n", lastStatus, DmMotor::statusText(lastStatus));
            if (motor.hasError()) {
                disableMotor();
                Serial.println("EVT motor error: disabled (run 'clear' then 'on')");
            }
        }
        static bool wasInRange = true;
        bool inRange = motor.positionInRange();
        if (wasInRange && !inRange) Serial.println("EVT position beyond PMAX range (pos/stop-to-POS disabled until 'zero')");
        wasInRange = inRange;

        if (millis() - motor.lastFeedbackMs() > FEEDBACK_LOST_MS) {
            disableMotor();
            Serial.println("EVT feedback lost: disabled");
            lastError = "feedback lost";
        }
    }

    // 状態の定期表示
    if (monPeriodMs && found && now - lastMon >= monPeriodMs) {
        lastMon = now;
        if (mode == Mode::Off) refreshWhileOff(20);
        printStatus();
    }

    if (now - lastDraw >= 100) {
        lastDraw = now;
        drawScreen();
    }
}
