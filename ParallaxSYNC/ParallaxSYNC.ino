#include <Arduino.h>

// ==================== 硬件配置 ====================
constexpr int PIN_DLP_LED = 13;
constexpr uint32_t SERIAL_BAUD = 115200;

// ==================== DLP-Link 时序参数 ====================
constexpr uint32_t SUBFRAME_PERIOD_US = 8333; // 120Hz (8.333ms)
constexpr uint32_t PULSE_WIDTH_US = 25;       // 同步光脉冲持续时间 (微秒)

// DLP-Link 左右眼偏移量 (微秒)
// 左眼通常为 500us，右眼通常为 700us
constexpr uint32_t OFFSET_LEFT_US = 500;
constexpr uint32_t OFFSET_RIGHT_US = 700;

// ==================== 信号超时参数 ====================
constexpr uint32_t SIGNAL_TIMEOUT_MS = 500;

// ==================== 状态变量 ====================
bool isActive = false;
bool currentEyeLeft = true;
uint32_t nextPulseTime = 0;
uint32_t lastMarkerTime = 0;
uint8_t rxState = 0;

// ==================== 快速 LED 控制 ====================
inline void ledOn()  { digitalWrite(PIN_DLP_LED, HIGH); }
inline void ledOff() { digitalWrite(PIN_DLP_LED, LOW); }

inline void preciseDelayUs(uint32_t us) {
    if (us == 0) return;
    uint32_t start = micros();
    while ((micros() - start) < us) {
        // busy wait 保证微秒级精确时序
    }
}

void emitSyncPulse(uint32_t width, uint32_t offset) {
    preciseDelayUs(offset);
    ledOn();
    preciseDelayUs(width);
    ledOff();
}

void setup() {
    pinMode(PIN_DLP_LED, OUTPUT);
    ledOff();

    Serial.begin(SERIAL_BAUD);
    delay(100);

    lastMarkerTime = millis();

    // 启动自检闪烁指示：快闪 3 次
    for (int i = 0; i < 3; i++) {
        ledOn();  delay(50);
        ledOff(); delay(50);
    }
}

void loop() {
    // ---------- 1. 处理串口标记（硬锁相与眼别强制对齐） ----------
    while (Serial.available() > 0) {
        uint8_t b = Serial.read();
        if (rxState == 0) {
            if (b == 0xAA) {
                rxState = 1;
            }
        } else if (rxState == 1) {
            if (b == 0x01 || b == 0x02) {
                lastMarkerTime = millis();
                isActive = true;

                // 强制对齐眼别：0x01 = 左眼, 0x02 = 右眼
                currentEyeLeft = (b == 0x01);

                // 立即发射当前眼别的 DLP-Link 同步脉冲，实现与 PC 显卡微秒级硬锁相
                uint32_t offset = currentEyeLeft ? OFFSET_LEFT_US : OFFSET_RIGHT_US;
                emitSyncPulse(PULSE_WIDTH_US, offset);

                // 重新校准下一个预期的本地脉冲基准时间
                nextPulseTime = micros() + SUBFRAME_PERIOD_US;
            }
            rxState = 0;
        }
    }

    // ---------- 2. 超时检测：无信号则熄灯待机保护 ----------
    if (isActive && (millis() - lastMarkerTime > SIGNAL_TIMEOUT_MS)) {
        isActive = false;
        ledOff();
    }

    // ---------- 3. 补帧防闪烁守护 (Watchdog Fallback) ----------
    // 仅在 PC 偶发微小丢帧或卡顿、没有按时送达下一帧标记时触发
    if (isActive) {
        uint32_t now = micros();
        // 允许 1.5ms 的容差窗口，超过窗口则补发一次交替脉冲保持眼镜同步锁相
        if ((int32_t)(now - (nextPulseTime + 1500)) >= 0) {
            currentEyeLeft = !currentEyeLeft;
            uint32_t offset = currentEyeLeft ? OFFSET_LEFT_US : OFFSET_RIGHT_US;
            emitSyncPulse(PULSE_WIDTH_US, offset);
            nextPulseTime = now + SUBFRAME_PERIOD_US;
        }
    }
}