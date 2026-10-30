#include <Arduino.h>

// ==================== 硬件引脚配置 ====================
constexpr int PIN_OPTICAL_SYNC = 1;  // 光学同步输入引脚 (GPIO 1)
constexpr int PIN_DLP_LED      = 13; // DLP 脉冲发射引脚 (GPIO 13)
constexpr uint32_t SERIAL_BAUD = 115200;

// 470kΩ 电阻接 3.3V (上拉)：屏幕变白导通接地，触发下降沿
constexpr int OPTICAL_TRIGGER_EDGE = FALLING;

// ==================== DLP-Link 120Hz 严格时序 ====================
constexpr uint32_t SUBFRAME_PERIOD_US = 8333; // 120Hz 子帧周期 (8.333ms)
constexpr uint32_t PULSE_WIDTH_US     = 25;   // 同步光脉冲持续宽度 (微秒)

// DLP-Link 左右眼偏移量 (微秒)
constexpr uint32_t OFFSET_LEFT_US  = 500;
constexpr uint32_t OFFSET_RIGHT_US = 700;

// ==================== 信号超时与硬件防抖 ====================
constexpr uint32_t SIGNAL_TIMEOUT_MS   = 500;
constexpr uint32_t OPTICAL_DEBOUNCE_US = 4000; // 过滤半周期内的背光杂波与多次触发

// ==================== 串口协议定义 ====================
constexpr uint8_t PROTO_SYNC         = 0xAA;
constexpr uint8_t PROTO_MARKER_LEFT  = 0x01;
constexpr uint8_t PROTO_MARKER_RIGHT = 0x02;
constexpr uint8_t PROTO_PHASE        = 0x03;

// ==================== 状态机与全局变量 ====================
bool standby=false;
bool opticalEnabled=true;
bool buttonRaw=false,buttonStable=false;
uint32_t buttonChangedMs=0;
bool isActive = false;
bool serialDriven = false;
bool serialPulsePending = false;
uint32_t lastSerialMs = 0;
bool currentEyeLeft = true;
uint32_t nextPulseTime = 0;
uint32_t lastMarkerTime = 0;

// 光学中断变量
volatile uint32_t g_lastOpticalEdgeUs = 0;
volatile bool     g_opticalEventPending = false;

// 串口解析状态
uint8_t rxState = 0;
uint8_t rxPhaseHigh = 0;
volatile int32_t g_phaseOffsetUs = 0;

// ==================== 快速 LED 控制 ====================
inline void ledOn()  { digitalWrite(PIN_DLP_LED, HIGH); }
inline void ledOff() { digitalWrite(PIN_DLP_LED, LOW); }

inline void preciseDelayUs(uint32_t us) {
    if (us == 0) return;
    const uint32_t start = micros();
    while ((micros() - start) < us) {
        // busy wait 确保微秒级精确时序
    }
}

void emitSyncPulse(uint32_t width, uint32_t offset) {
    preciseDelayUs(offset);
    ledOn();
    preciseDelayUs(width);
    ledOff();
}

// ==================== GPIO 1 外部硬件中断 ====================
void IRAM_ATTR onOpticalSyncISR() {
    const uint32_t now = micros();
    if ((now - g_lastOpticalEdgeUs) >= OPTICAL_DEBOUNCE_US) {
        g_lastOpticalEdgeUs = now;
        g_opticalEventPending = true;
    }
}

// ==================== 光学锁相环相位校准 ====================
void handleOpticalSync() {
    if(standby || !opticalEnabled) {g_opticalEventPending=false;return;}
    if (!g_opticalEventPending) return;

    noInterrupts();
    const uint32_t edgeTimeUs = g_lastOpticalEdgeUs;
    g_opticalEventPending = false;
    interrupts();

    if(serialDriven && millis()-lastSerialMs < SIGNAL_TIMEOUT_MS) return;
    serialDriven=false;
    // 收到屏幕光学刷新信号，更新保活时间戳
    lastMarkerTime = millis();

    // 计算包含用户滤镜微调的绝对目标时刻
    int32_t modOffset = g_phaseOffsetUs % static_cast<int32_t>(SUBFRAME_PERIOD_US * 2);
    if (modOffset < 0) {
        modOffset += SUBFRAME_PERIOD_US * 2;
    }

    if (!isActive) {
        // 首次激活：强制对齐为左眼基准，消除左右眼颠倒
        isActive = true;
        currentEyeLeft = true;
        nextPulseTime = edgeTimeUs + modOffset;
        return;
    }

    // 连续锁相：计算当前发生器时序与真实屏幕物理刷新时刻的相位误差
    const uint32_t targetTime = edgeTimeUs + modOffset;
    // White edges identify LEFT, not just an arbitrary 120 Hz tick.
    // Compare against the scheduled LEFT slot over the full stereo period.
    const uint32_t nextLeftTime = nextPulseTime + (currentEyeLeft ? 0 : SUBFRAME_PERIOD_US);
    const int32_t phaseError = static_cast<int32_t>(targetTime - nextLeftTime);

    int32_t wrappedError = phaseError % static_cast<int32_t>(SUBFRAME_PERIOD_US * 2);
    if (wrappedError > static_cast<int32_t>(SUBFRAME_PERIOD_US)) {
        wrappedError -= SUBFRAME_PERIOD_US * 2;
    } else if (wrappedError < -static_cast<int32_t>(SUBFRAME_PERIOD_US)) {
        wrappedError += SUBFRAME_PERIOD_US * 2;
    }

    // A half-cycle slip is an eye-identity error. Reacquire from the white LEFT
    // edge rather than slowly sweeping the pulse clock through the wrong eye.
    if (wrappedError > static_cast<int32_t>(SUBFRAME_PERIOD_US / 2) ||
        wrappedError < -static_cast<int32_t>(SUBFRAME_PERIOD_US / 2)) {
        // Correct eye identity without discarding or postponing the due pulse.
        currentEyeLeft = !currentEyeLeft;
        if (wrappedError > 0) wrappedError -= SUBFRAME_PERIOD_US;
        else wrappedError += SUBFRAME_PERIOD_US;
        nextPulseTime += wrappedError / 4;
        return;
    }

    // 软锁相收敛（每次补偿 25% 误差），既消除温漂又不破坏 DLP-Link 连续性
    nextPulseTime += (wrappedError / 4);
}

// ==================== 串口命令处理 ====================
void handleSerialInput() {
    while (Serial.available() > 0) {
        const uint8_t b = static_cast<uint8_t>(Serial.read());
        standby=false; // Keep USB alive so incoming bytes can wake the device.

        switch (rxState) {
            case 0:
                if (b == PROTO_SYNC) {
                    rxState = 1;
                }
                break;

            case 1:
                if(b==0x05){
                    opticalEnabled=true;standby=false;serialDriven=false;
                    isActive=false;serialPulsePending=false;g_opticalEventPending=false;rxState=0;
                } else if(b==0x04){
                    opticalEnabled=false;isActive=false;serialPulsePending=false;ledOff();rxState=0;
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
                    const int32_t delta = static_cast<int32_t>(newOffset) - g_phaseOffsetUs;
                    nextPulseTime += delta;

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

// ==================== 系统初始化 ====================
void setup() {
    // 脉冲发射引脚初始化
    pinMode(0, INPUT_PULLUP);
    pinMode(PIN_DLP_LED, OUTPUT);
    ledOff();

    // 配置 GPIO 1 为标准数字输入，并挂载下降沿硬件中断
    pinMode(PIN_OPTICAL_SYNC, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_OPTICAL_SYNC), onOpticalSyncISR, OPTICAL_TRIGGER_EDGE);

    // 原生 USB CDC 串口通信初始化
    Serial.begin(SERIAL_BAUD);
    delay(100);

    lastMarkerTime = millis();

    // 开机自检：GPIO 13 短促闪烁 100ms 提示固件启动就绪
    ledOn();
    delay(100);
    ledOff();
}

// ==================== 主任务循环 ====================
void loop() {
    const bool pressed=digitalRead(0)==LOW;
    const uint32_t buttonNow=millis();
    if(pressed!=buttonRaw){buttonRaw=pressed;buttonChangedMs=buttonNow;}
    if(buttonStable!=buttonRaw && buttonNow-buttonChangedMs>=35){
        buttonStable=buttonRaw;
        if(buttonStable){
            standby=!standby;isActive=false;serialPulsePending=false;rxState=0;
            g_opticalEventPending=false;ledOff();
        }
    }
    if(standby){handleSerialInput();if(standby){delay(1);return;}}

    // 1. 执行光学中断边沿与硬件锁相环校准
    handleOpticalSync();

    // 2. 接收 PotPlayer 串口数据（心跳保活与滤镜面板相位滑块）
    handleSerialInput();

    // 3. 超时保护：无光脉冲且无串口信号超过 500ms 时，关灯休眠
    if (isActive && (millis() - lastMarkerTime > SIGNAL_TIMEOUT_MS)) {
        isActive = false;
        ledOff();
    }

    // 4. 高精度 120Hz 严格交替 DLP-Link 脉冲发生器
    if (isActive && (!serialDriven || serialPulsePending)) {
        const uint32_t now = micros();
        if (static_cast<int32_t>(now - nextPulseTime) >= 0) {
            const uint32_t offset = currentEyeLeft ? OFFSET_LEFT_US : OFFSET_RIGHT_US;
            emitSyncPulse(PULSE_WIDTH_US, offset);

            if(serialDriven) serialPulsePending=false;
            else currentEyeLeft=!currentEyeLeft;
            nextPulseTime += SUBFRAME_PERIOD_US;

            // 防止异常阻塞导致的累积脉冲风暴
            if (static_cast<int32_t>(micros() - nextPulseTime) >
                static_cast<int32_t>(SUBFRAME_PERIOD_US)) {
                nextPulseTime = micros() + SUBFRAME_PERIOD_US;
            }
        }
    }
}