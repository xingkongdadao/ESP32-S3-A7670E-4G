#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// ========== 硬件配置（按需修改）==========
#define LED_STRIP_PIN    38    // WS2812B 数据引脚（GPIO38）
#define LED_COUNT        1

#define MODEM_RX_PIN     17    // 与 A7670E TX 相连
#define MODEM_TX_PIN     18    // 与 A7670E RX 相连
#define MODEM_BAUD       115200

// ========== 对象与状态 ==========
Adafruit_NeoPixel strip(LED_COUNT, LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);
HardwareSerial SerialAT(1); // 使用 UART1 与模块通信

enum ModemState {
  STATE_NO_SIM,
  STATE_SIM_NO_SIGNAL,
  STATE_SIM_HAS_SIGNAL
};

ModemState currentState = STATE_NO_SIM;

// 定时
unsigned long lastModemCheck = 0;
const unsigned long MODEM_CHECK_INTERVAL = 5000; // 每 5s 检查一次

unsigned long lastBlinkToggle = 0;
bool blinkOn = false;
unsigned long blinkIntervalMs = 500; // 切换间隔：500ms -> 周期 1s；1000ms -> 周期 2s

// ========== AT 交互 ==========
String sendAT(const String &cmd, unsigned long timeoutMs = 1000) {
  while (SerialAT.available()) SerialAT.read(); // 清空缓冲
  SerialAT.print(cmd);
  SerialAT.print("\r\n");

  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      String line = SerialAT.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        response += line + "\n";
      }
    }
  }
  return response;
}

bool hasSIM() {
  String rsp = sendAT("AT+CPIN?", 1000);
  Serial.print("CPIN rsp: ");
  Serial.println(rsp);
  if (rsp.indexOf("READY") >= 0) return true;
  return false;
}

int getRSSI() {
  String rsp = sendAT("AT+CSQ", 1000);
  Serial.print("CSQ rsp: ");
  Serial.println(rsp);
  int idx = rsp.indexOf("+CSQ:");
  if (idx >= 0) {
    int comma = rsp.indexOf(',', idx);
    if (comma > idx) {
      String num = rsp.substring(idx + 5, comma);
      num.trim();
      int rssi = num.toInt();
      return rssi; // 0-31 有效，99 表示未知
    }
  }
  return 99;
}

// ========== LED 控制 ==========
void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  strip.setPixelColor(0, strip.Color(r, g, b));
  strip.show();
}

void turnOffLed() {
  setLedColor(0, 0, 0);
}

// ========== setup / loop ===========
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("启动：ESP32-S3 + A7670E 状态指示 (WS2812B)");

  strip.begin();
  strip.setBrightness(120);
  strip.show(); // 熄灭

  // 初始化与模块的串口（UART1）
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(200);

  // 预热 AT 通信
  Serial.println("发送 AT 测试...");
  String r = sendAT("AT", 500);
  Serial.print("AT -> ");
  Serial.println(r);

  lastModemCheck = millis() - MODEM_CHECK_INTERVAL; // 立即检测一次
}

void loop() {
  unsigned long now = millis();

  // 定时检测 SIM / 信号
  if (now - lastModemCheck >= MODEM_CHECK_INTERVAL) {
    lastModemCheck = now;
    bool simPresent = hasSIM();
    if (!simPresent) {
      currentState = STATE_NO_SIM;
      Serial.println("检测: 无 SIM 卡");
    } else {
      int rssi = getRSSI();
      Serial.print("RSSI: ");
      Serial.println(rssi);
      if (rssi == 99 || rssi == 0) {
        currentState = STATE_SIM_NO_SIGNAL;
        Serial.println("检测: 有 SIM，但无信号");
      } else {
        currentState = STATE_SIM_HAS_SIGNAL;
        Serial.println("检测: 有 SIM 且有信号");
      }
    }

    if (currentState == STATE_NO_SIM) {
      blinkIntervalMs = 500; // 0.5s 切换 -> 1s 周期
    } else if (currentState == STATE_SIM_NO_SIGNAL) {
      blinkIntervalMs = 1000; // 1s 切换 -> 2s 周期
    } else {
      blinkOn = true;
    }
  }

  // 根据状态控制灯
  if (currentState == STATE_SIM_HAS_SIGNAL) {
    // 绿色常亮
    setLedColor(0, 255, 0);
  } else if (currentState == STATE_NO_SIM || currentState == STATE_SIM_NO_SIGNAL) {
    if (now - lastBlinkToggle >= blinkIntervalMs) {
      lastBlinkToggle = now;
      blinkOn = !blinkOn;
      if (blinkOn) {
        setLedColor(255, 0, 0);
      } else {
        turnOffLed();
      }
    }
  } else {
    turnOffLed();
  }

  delay(10);
}


