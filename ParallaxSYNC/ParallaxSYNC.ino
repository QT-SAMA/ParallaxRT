/*
 * ParallaxSYNC_FreeRun - 配合 PotPlayer ParallaxRT 滤镜的 DLP-Link 同步器
 * 串口协议: 115200, 8N1
 * 接收: 0xAA 0x01 (左眼) / 0xAA 0x02 (右眼)
 * 输出: ESP32 自主运行 120Hz 脉冲，标记仅用于唤醒和超时检测
 * 无信号超时后自动关闭 LED 待机
 */

#include <Arduino.h>

// ==================== 硬件配置 ====================
constexpr int PIN_DLP_LED = 13;
constexpr uint32_t SERIAL_BAUD = 115200;

// ==================== DLP-Link 时序参数 ====================
constexpr uint32_t SUBFRAME_PERIOD_US = 8333; // 120Hz
constexpr uint32_t PULSE_WIDTH_US = 25;
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

// ==================== LED 控制 ====================
inline void ledOn()  { digitalWrite(PIN_DLP_LED, HIGH); }
inline void ledOff() { digitalWrite(PIN_DLP_LED, LOW); }

inline void preciseDelayUs(uint32_t us) {
    uint32_t start = micros();
    while ((micros() - start) < us) { /* busy wait */ }
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

    // 启动指示：快闪 3 次
    for (int i = 0; i < 3; i++) {
        ledOn();  delay(50);
        ledOff(); delay(50);
    }
}

void loop() {
    // ---------- 1. 处理串口接收（仅用于唤醒和超时检测） ----------
    while (Serial.available() > 0) {
        uint8_t b = Serial.read();
        if (rxState == 0) {
            if (b == 0xAA) {
                rxState = 1;
            }
        } else if (rxState == 1) {
            if (b == 0x01 || b == 0x02) {
                lastMarkerTime = millis();
                if (!isActive) {
                    // 从待机唤醒：重置时间基准，但眼别由 ESP32 自由交替
                    isActive = true;
                    nextPulseTime = micros() + SUBFRAME_PERIOD_US;
                }
            }
            rxState = 0;
        }
    }

    // ---------- 2. 超时检测：无信号则关灯待机 ----------
    if (isActive && (millis() - lastMarkerTime > SIGNAL_TIMEOUT_MS)) {
        isActive = false;
        ledOff();
    }

    // ---------- 3. 自主运行 120Hz 脉冲发生器 ----------
    if (isActive) {
        uint32_t now = micros();
        if ((int32_t)(now - nextPulseTime) >= 0) {
            // 眼别完全由内部计数器交替，不受串口标记影响
            currentEyeLeft = !currentEyeLeft;

            uint32_t offset = currentEyeLeft ? OFFSET_LEFT_US : OFFSET_RIGHT_US;
            emitSyncPulse(PULSE_WIDTH_US, offset);

            nextPulseTime += SUBFRAME_PERIOD_US;

            // 落后太多则重置时间基准
            if ((int32_t)(micros() - nextPulseTime) > (int32_t)SUBFRAME_PERIOD_US) {
                nextPulseTime = micros() + SUBFRAME_PERIOD_US;
            }
        }
    }
}