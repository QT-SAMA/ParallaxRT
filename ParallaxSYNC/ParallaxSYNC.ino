/*
 * ParallaxSYNC - 配合 PotPlayer ParallaxRT 滤镜的 DLP-Link 120Hz 同步固件
 *
 * 硬件连接:
 *   ESP32 GPIO 13 -> 红色/白光 LED (串联 100~220 欧姆电阻) -> GND
 *   ESP32 GND     -> LED 阴极
 *
 * 核心机制:
 * 1. 恒定等间隔 120Hz 脉冲列 (8333us 周期):
 *    DLP-Link 快门眼镜必须依靠持续、严丝合缝、严格交替的 120Hz 光脉冲维持锁相。
 *    左眼偏移 500us，右眼偏移 700us。
 *    严禁在收到串口数据时直接插入脉冲或打乱 L/R 交替顺序。
 * 2. 串口保活与自动休眠:
 *    PC 端 ParallaxRT 滤镜在播放视频时持续发送心跳标记 (0xAA 0x01 / 0xAA 0x02)。
 *    ESP32 收到标记后维持工作状态；视频暂停或退出超过 500ms 自动熄灯休眠。
 * 3. 相位微调 (Phase Offset):
 *    通过串口命令 0xAA 0x03 <high> <low> 传入 16 位有符号整数（单位微秒）。
 *    范围建议 -4000 .. +4000（120Hz 半周期 = 4166us）。
 *    收到相位命令后，一次性位移 nextPulseTime，不打断脉冲的等间隔特性。
 * 4. 重影与锁相消除关键设置:
 *    - 显示器刷新率必须在 Windows 设置中调至 120 Hz（与 120Hz 快门严格匹配）。
 *    - 用 PotPlayer 或滤镜面板的相位滑块调整，直到重影最小。
 *    - 遇到左右眼反转，按一下眼镜上的开机键即可单击翻转左右眼相位。
 */

#include <Arduino.h>

// ==================== 硬件配置 ====================
constexpr int PIN_DLP_LED = 13;
constexpr uint32_t SERIAL_BAUD = 115200;

// ==================== DLP-Link 120Hz 严格时序 ====================
constexpr uint32_t SUBFRAME_PERIOD_US = 8333; // 120Hz 周期 (8.333ms)
constexpr uint32_t PULSE_WIDTH_US = 25;       // 同步光脉冲持续时间 (微秒)

// DLP-Link 左右眼偏移量 (微秒)
constexpr uint32_t OFFSET_LEFT_US = 500;
constexpr uint32_t OFFSET_RIGHT_US = 700;

// ==================== 信号超时参数 ====================
constexpr uint32_t SIGNAL_TIMEOUT_MS = 500;

// ==================== 串口协议定义 ====================
// 0xAA 0x01           左眼心跳（仅保活）
// 0xAA 0x02           右眼心跳（仅保活）
// 0xAA 0x03 H L       相位微调，int16 big-endian，单位微秒
constexpr uint8_t PROTO_SYNC = 0xAA;
constexpr uint8_t PROTO_MARKER_LEFT = 0x01;
constexpr uint8_t PROTO_MARKER_RIGHT = 0x02;
constexpr uint8_t PROTO_PHASE = 0x03;

// ==================== 状态变量 ====================
bool isActive = false;
bool currentEyeLeft = true;
uint32_t nextPulseTime = 0;
uint32_t lastMarkerTime = 0;

// 串口解析状态机
// 0: 等待 0xAA
// 1: 等待命令字节 (0x01/0x02/0x03)
// 2: 相位命令第二字节 (high)
// 3: 相位命令第三字节 (low)
uint8_t rxState = 0;
uint8_t rxPhaseHigh = 0;

// 当前相位偏移（微秒），可正可负
volatile int32_t g_phaseOffsetUs = 0;

// ==================== 快速 LED 控制 ====================
inline void ledOn()  { digitalWrite(PIN_DLP_LED, HIGH); }
inline void ledOff() { digitalWrite(PIN_DLP_LED, LOW); }

inline void preciseDelayUs(uint32_t us) {
    if (us == 0) return;
    const uint32_t start = micros();
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

// ==================== 串口命令处理 ====================
void handleSerialInput() {
    while (Serial.available() > 0) {
        const uint8_t b = static_cast<uint8_t>(Serial.read());

        switch (rxState) {
            case 0:
                if (b == PROTO_SYNC) {
                    rxState = 1;
                }
                break;

            case 1:
                if (b == PROTO_MARKER_LEFT || b == PROTO_MARKER_RIGHT) {
                    // 保活心跳，只更新时间戳
                    lastMarkerTime = millis();
                    if (!isActive) {
                        // 从待机唤醒：初始化 120Hz 基准时钟
                        isActive = true;
                        currentEyeLeft = true;
                        nextPulseTime = micros() + SUBFRAME_PERIOD_US;
                    }
                    rxState = 0;
                } else if (b == PROTO_PHASE) {
                    rxState = 2;
                } else {
                    rxState = 0;
                }
                break;

            case 2:
                rxPhaseHigh = b;
                rxState = 3;
                break;

            case 3: {
                const int16_t newOffset =
                    static_cast<int16_t>((static_cast<uint16_t>(rxPhaseHigh) << 8) | b);
                if (isActive) {
                    // 一次性位移脉冲序列到新相位
                    const int32_t delta = static_cast<int32_t>(newOffset) - g_phaseOffsetUs;
                    nextPulseTime += delta;

                    // 防止位移过大造成 nextPulseTime 严重落后或超前
                    const int32_t lag = static_cast<int32_t>(micros() - nextPulseTime);
                    if (lag > static_cast<int32_t>(SUBFRAME_PERIOD_US)) {
                        nextPulseTime = micros() + SUBFRAME_PERIOD_US;
                    } else if (lag < -static_cast<int32_t>(SUBFRAME_PERIOD_US * 2)) {
                        nextPulseTime = micros() + SUBFRAME_PERIOD_US;
                    }
                }
                g_phaseOffsetUs = newOffset;
                rxState = 0;
                break;
            }

            default:
                rxState = 0;
                break;
        }
    }
}

// ==================== 初始化 ====================
void setup() {
    pinMode(PIN_DLP_LED, OUTPUT);
    ledOff();

    Serial.begin(SERIAL_BAUD);
    delay(100);

    lastMarkerTime = millis();

}

// ==================== 主循环 ====================
void loop() {
    // ---------- 1. 处理串口（保活 + 相位命令） ----------
    handleSerialInput();

    // ---------- 2. 超时检测：无播放信号时关灯待机保护 ----------
    if (isActive && (millis() - lastMarkerTime > SIGNAL_TIMEOUT_MS)) {
        isActive = false;
        ledOff();
    }

    // ---------- 3. 严格交替、等间隔 120Hz 脉冲发生器 ----------
    if (isActive) {
        const uint32_t now = micros();
        if (static_cast<int32_t>(now - nextPulseTime) >= 0) {
            // 严格等间隔交替发射 L / R DLP-Link 脉冲
            currentEyeLeft = !currentEyeLeft;
            const uint32_t offset = currentEyeLeft ? OFFSET_LEFT_US : OFFSET_RIGHT_US;
            emitSyncPulse(PULSE_WIDTH_US, offset);

            nextPulseTime += SUBFRAME_PERIOD_US;

            // 若时钟严重落后则重置基准，防止脉冲风暴
            if (static_cast<int32_t>(micros() - nextPulseTime) >
                static_cast<int32_t>(SUBFRAME_PERIOD_US)) {
                nextPulseTime = micros() + SUBFRAME_PERIOD_US;
            }
        }
    }
}