#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

static const int RXPin = 17, TXPin = 18;
static const uint32_t GPSBaud = 115200;

// 彩灯引脚和数量定义
#define LED_STRIP_PIN    38    // WS2812B彩灯数据引脚（GPIO38）
#define LED_COUNT        1     // WS2812B-0807通常只有1个灯珠
// 彩灯对象
Adafruit_NeoPixel strip(LED_COUNT, LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);

String rev;

bool simPresent = false;
bool networkRegistered = false;

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL = 5000;

unsigned long lastBlinkToggle = 0;
const unsigned long BLINK_INTERVAL = 500;
bool blinkState = false;

void SentSerial(const char *p_char) {
  for (int i = 0; i < strlen(p_char); i++) {
    Serial1.write(p_char[i]);
    delay(10);
  }
  Serial1.write('\r');
  delay(10);
  Serial1.write('\n');
  delay(10);
}

bool SentMessage(const char *p_char, unsigned long timeout = 2000) {
  SentSerial(p_char);

  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      rev = Serial1.readString();
      if (rev.indexOf("OK") != -1) {
        Serial.println("Got OK!");
        return true;
      }
    }
  }
  return false;
}

void setColor(bool r, bool g, bool b) {
  // 如果硬件通道与逻辑颜色不一致，可在这里映射物理通道
  // 下面将逻辑 R/G 互换以适配某些 WS2812B 的通道顺序
  uint8_t physR = g ? 255 : 0;
  uint8_t physG = r ? 255 : 0;
  uint8_t physB = b ? 255 : 0;
  strip.setPixelColor(0, strip.Color(physR, physG, physB));
  strip.show();
}

void updateLEDState() {
  if (networkRegistered) {
    // 网络已注册：绿色常亮
    setColor(false, true, false);
    return;
  }

  if (!simPresent) {
    // 无 SIM：红色闪烁
    unsigned long now = millis();
    if (now - lastBlinkToggle >= BLINK_INTERVAL) {
      blinkState = !blinkState;
      lastBlinkToggle = now;
    }
    if (blinkState) setColor(true, false, false);
    else setColor(false, false, false);
    return;
  }

  // 有卡但未注册网络：红色常亮
  setColor(true, false, false);
}

void parseModuleResponse(const String &response) {
  if (response.indexOf("+CPIN:") != -1) {
    if (response.indexOf("READY") != -1) simPresent = true;
    else simPresent = false;
  }

  if (response.indexOf("+CGREG:") != -1) {
    int commaIndex = response.indexOf(',');
    if (commaIndex != -1) {
      int stat = response.substring(commaIndex + 1).toInt();
      networkRegistered = (stat == 1 || stat == 5);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);

  strip.begin();
  strip.show(); // 初始化关闭

  while (!SentMessage("AT", 2000)) {
    delay(1000);
  }
  SentSerial("ATE1;");
  SentSerial("AT+CPIN?");
  SentSerial("AT+COPS?");
  SentSerial("AT+CGDCONT?");
  SentSerial("AT+CGREG?");
  SentSerial("AT+SIMCOMATI");
}

void loop() {
  if (Serial1.available()) {
    rev = Serial1.readString();
    Serial.println(rev);
    parseModuleResponse(rev);
  }

  unsigned long now = millis();
  if (now - lastPoll >= POLL_INTERVAL) {
    lastPoll = now;
    SentSerial("AT+CPIN?");
    SentSerial("AT+CGREG?");
  }

  updateLEDState();
}
