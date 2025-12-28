#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

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
unsigned long lastAltToggle = 0;
const unsigned long ALT_INTERVAL = 1000; // 交替显示间隔（ms）
bool altState = false; // true -> show WiFi status; false -> show SIM status

// WiFi 上传配置（来自用户）
const char* WIFI_SSID = "米奇";
const char* WIFI_PASS = "19963209891";

// 后台 API 配置
static const char GEO_SENSOR_API_BASE_URL[] = "https://manage.gogotrans.com/api/device/geoSensor/";
static const char GEO_SENSOR_KEY[] = "mcu_5e3abda8585e4bc79af89ad57af8b3b7";
static const char GEO_SENSOR_ID[] = "6df617a4-e332-11f0-abbb-9ed80c0d9d5f";

unsigned long lastUpload = 0;
const unsigned long UPLOAD_INTERVAL = 10000; // 10秒

// 连接 WiFi（阻塞，带超时）
void wifiConnect() {
  Serial.print("Connecting to WiFi ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("WiFi connect failed");
  }
}

// 通过 WiFi 发起任意 HTTP 方法请求（例如 PATCH），返回是否成功
bool wifiHttpRequest(const String &method, const String &url, const String &json) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  Serial.print("WiFi ");
  Serial.print(method);
  Serial.print(" to: ");
  Serial.println(url);
  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");
  https.addHeader("x-api-key", String(GEO_SENSOR_KEY));
  int httpCode = https.sendRequest(method.c_str(), (uint8_t*)json.c_str(), json.length());
  Serial.print("HTTP code: ");
  Serial.println(httpCode);
  if (httpCode > 0) {
    String payload = https.getString();
    Serial.print("Payload: ");
    Serial.println(payload);
  }
  https.end();
  return (httpCode >= 200 && httpCode < 300);
}

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
  unsigned long now = millis();
  // 切换显示 WiFi / SIM 状态（交替）
  if (now - lastAltToggle >= ALT_INTERVAL) {
    altState = !altState;
    lastAltToggle = now;
  }

  // WiFi 指示阶段
  if (altState) {
    if (WiFi.status() == WL_CONNECTED) {
      // 有 WiFi：橘黄色常亮
      setColor(true, true, false);
    } else {
      // 无 WiFi：橘黄色闪烁
      if (now - lastBlinkToggle >= BLINK_INTERVAL) {
        blinkState = !blinkState;
        lastBlinkToggle = now;
      }
      if (blinkState) setColor(true, true, false);
      else setColor(false, false, false);
    }
    return;
  }

  // SIM 指示阶段
  if (!simPresent) {
    // 无 SIM：红色闪烁
    if (now - lastBlinkToggle >= BLINK_INTERVAL) {
      blinkState = !blinkState;
      lastBlinkToggle = now;
    }
    if (blinkState) setColor(true, false, false);
    else setColor(false, false, false);
    return;
  }

  // 有 SIM：判断是否注册与是否有数据承载（PDP）
  // 尝试查询 PDP/IP 地址，若能获得非 0.0.0.0 的 IP 则认为有网络
  SentSerial("AT+CGPADDR?");
  unsigned long tstart = millis();
  String resp = "";
  while (millis() - tstart < 800) {
    if (Serial1.available()) {
      resp += Serial1.readString();
    }
  }
  Serial.print("AT+CGPADDR 返回: ");
  Serial.println(resp);
  bool pdpActive = (resp.indexOf('.') != -1 && resp.indexOf("0.0.0.0") == -1);
  Serial.print("PDP 激活: ");
  Serial.println(pdpActive ? "是" : "否");

  if (!networkRegistered) {
    // 有 SIM 但未注册：蓝色闪烁
    if (now - lastBlinkToggle >= BLINK_INTERVAL) {
      blinkState = !blinkState;
      lastBlinkToggle = now;
    }
    if (blinkState) setColor(false, false, true);
    else setColor(false, false, false);
  } else {
    // SIM 已注册
    if (pdpActive) {
      // 已注册且有网络：绿色常亮
      setColor(false, true, false);
    } else {
      // 已注册但无网络：蓝色常亮
      setColor(false, false, true);
    }
  }
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
  // 连接 WiFi（优先）
  wifiConnect();

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
 
  // 优先通过 WiFi 上传
  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastUpload >= UPLOAD_INTERVAL) {
      lastUpload = now;
      // 构建上传数据（占位，后续可替换为真实传感器数据）
      double latitude = 0.0;
      double longitude = 0.0;
      double altitude = 0.0;
      double speed = 0.0;
      int satelliteCount = 0;
      double locationAccuracy = 0.0;
      double altitudeAccuracy = 0.0;
      String dataAcquiredAt = "";
      // 尝试通过系统时间获取 ISO8601（UTC）
      time_t nowt = time(nullptr);
      if (nowt != ((time_t)-1)) {
        struct tm tm;
        gmtime_r(&nowt, &tm);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        dataAcquiredAt = String(buf);
      }

      String json = "{";
      json += "\"id\":\"";
      json += GEO_SENSOR_ID;
      json += "\",";
      json += "\"latitude\":";
      json += String(latitude, 6);
      json += ",";
      json += "\"longitude\":";
      json += String(longitude, 6);
      json += ",";
      json += "\"altitude\":";
      json += String(altitude, 2);
      json += ",";
      json += "\"speed\":";
      json += String(speed, 2);
      json += ",";
      json += "\"satelliteCount\":";
      json += String(satelliteCount);
      json += ",";
      json += "\"locationAccuracy\":";
      json += String(locationAccuracy, 2);
      json += ",";
      json += "\"altitudeAccuracy\":";
      json += String(altitudeAccuracy, 2);
      json += ",";
      json += "\"dataAcquiredAt\":\"";
      json += dataAcquiredAt;
      json += "\",";
      json += "\"networkSource\":\"WiFi\"";
      json += "}";

      Serial.println("Uploading via WiFi (PATCH)...");
      String fullUrl = String(GEO_SENSOR_API_BASE_URL) + String(GEO_SENSOR_ID) + String("/");
      bool ok = wifiHttpRequest("PATCH", fullUrl, json);
      Serial.print("WiFi upload result: ");
      Serial.println(ok ? "OK" : "FAILED");
    }
  } else {
    // 若未连接 WiFi，可考虑使用 4G（保留原有逻辑）
  }
}
