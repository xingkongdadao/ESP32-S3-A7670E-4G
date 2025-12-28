#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// 是否启用串口打印（调试用），设置为 1 可显示所有网络连接和调试信息
#define SERIAL_VERBOSE 1

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
  if (SERIAL_VERBOSE) {
    Serial.print("Connecting to WiFi ");
    Serial.println(WIFI_SSID);
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    if (SERIAL_VERBOSE) Serial.print(".");
  }
  if (SERIAL_VERBOSE) Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    if (SERIAL_VERBOSE) {
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    } else {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    }
  } else {
    if (SERIAL_VERBOSE) Serial.println("WiFi connect failed");
  }
}

// 通过 WiFi 发起任意 HTTP 方法请求（例如 PATCH），返回是否成功
bool wifiHttpRequest(const String &method, const String &url, const String &json) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (SERIAL_VERBOSE) {
    Serial.print("WiFi ");
    Serial.print(method);
    Serial.print(" to: ");
    Serial.println(url);
  }
  if (!https.begin(client, url)) {
    if (SERIAL_VERBOSE) Serial.println("HTTPS begin failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");
  https.addHeader("x-api-key", String(GEO_SENSOR_KEY));
  int httpCode = https.sendRequest(method.c_str(), (uint8_t*)json.c_str(), json.length());
  if (SERIAL_VERBOSE) {
    Serial.print("HTTP code: ");
    Serial.println(httpCode);
    if (httpCode > 0) {
      String payload = https.getString();
      Serial.print("Payload: ");
      Serial.println(payload);
    }
  } else {
    // still consume payload to avoid blocking on some implementations
    if (httpCode > 0) { (void)https.getString(); }
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
        if (SERIAL_VERBOSE) Serial.println("Got OK!");
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
  // 修复：使用更可靠的查询方式，避免干扰其他AT命令
  static unsigned long lastPdpCheck = 0;
  static bool lastPdpStatus = false;

  // 每5秒检查一次PDP状态，避免过于频繁的查询
  if (millis() - lastPdpCheck >= 5000) {
    lastPdpCheck = millis();

    if (SERIAL_VERBOSE) Serial.println("检查 PDP 状态...");

    SentSerial("AT+CGPADDR");
    delay(100); // 给模块一点响应时间

    unsigned long tstart = millis();
    String resp = "";
    bool gotResponse = false;

    // 等待完整响应，超时2秒
    while (millis() - tstart < 2000 && !gotResponse) {
      if (Serial1.available()) {
        char c = Serial1.read();
        resp += c;

        // 检查是否收到完整的响应
        if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) {
          gotResponse = true;
        }
      }
      delay(10);
    }

    // 解析响应，查找IP地址
    bool pdpActive = false;
    if (resp.indexOf("+CGPADDR: 1,") != -1) {
      // 检查是否包含有效的IP地址（不是0.0.0.0）
      int ipStart = resp.indexOf("+CGPADDR: 1,") + 12;
      int ipEnd = resp.indexOf("\n", ipStart);
      if (ipEnd == -1) ipEnd = resp.indexOf("OK", ipStart);
      if (ipEnd == -1) ipEnd = resp.length();

      String ipAddr = resp.substring(ipStart, ipEnd);
      ipAddr.trim();

      // 检查IP地址是否有效
      pdpActive = (ipAddr.length() > 0 &&
                   ipAddr != "0.0.0.0" &&
                   ipAddr.indexOf('.') != -1 &&
                   !ipAddr.startsWith("0."));
    }

    lastPdpStatus = pdpActive;

    if (SERIAL_VERBOSE) {
      Serial.print("PDP 查询响应: ");
      Serial.println(resp);
      Serial.print("PDP 激活状态: ");
      Serial.println(pdpActive ? "激活 ✓" : "未激活 ✗");
      if (pdpActive) {
        Serial.println("✓✓✓ 4G网络连接正常 ✓✓✓");
      } else {
        Serial.println("⚠️⚠️⚠️ 4G网络连接异常 ⚠️⚠️⚠️");
      }
    }
  }

  bool pdpActive = lastPdpStatus;

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
    if (response.indexOf("READY") != -1) {
      simPresent = true;
      if (SERIAL_VERBOSE) Serial.println("✓ SIM 卡状态: READY (SIM 卡正常)");
    } else {
      simPresent = false;
      if (SERIAL_VERBOSE) Serial.println("✗ SIM 卡状态: ERROR (SIM 卡异常)");
    }
  }

  if (response.indexOf("+CGREG:") != -1) {
    int commaIndex = response.indexOf(',');
    if (commaIndex != -1) {
      int stat = response.substring(commaIndex + 1).toInt();
      bool wasRegistered = networkRegistered;
      networkRegistered = (stat == 1 || stat == 5);

      if (SERIAL_VERBOSE) {
        Serial.print("网络注册状态: ");
        switch (stat) {
          case 0: Serial.println("未注册，正在搜索"); break;
          case 1: Serial.println("已注册到本地网络"); break;
          case 2: Serial.println("未注册，正在搜索"); break;
          case 3: Serial.println("注册被拒绝"); break;
          case 4: Serial.println("未知"); break;
          case 5: Serial.println("已注册到漫游网络"); break;
          default: Serial.println("状态码: " + String(stat)); break;
        }
      }

      if (!wasRegistered && networkRegistered && SERIAL_VERBOSE) {
        Serial.println("✓ 网络注册成功！");
      } else if (wasRegistered && !networkRegistered && SERIAL_VERBOSE) {
        Serial.println("✗ 网络注册丢失！");
      }
    }
  }

  // 显示其他重要响应
  if (SERIAL_VERBOSE) {
    if (response.indexOf("+CSQ:") != -1) {
      Serial.print("信号强度: ");
      Serial.println(response);
    }
    if (response.indexOf("+COPS:") != -1) {
      Serial.print("运营商信息: ");
      Serial.println(response);
    }
    if (response.indexOf("+CGDCONT:") != -1) {
      Serial.print("PDP上下文: ");
      Serial.println(response);
    }
    if (response.indexOf("+CGATT:") != -1) {
      Serial.print("分组域附着: ");
      Serial.println(response);
    }
    if (response.indexOf("+CGPADDR:") != -1) {
      Serial.print("IP地址: ");
      Serial.println(response);
      if (response.indexOf("0.0.0.0") == -1 && response.indexOf("+CGPADDR: 1,") != -1) {
        Serial.println("✓ PDP 激活成功，获得IP地址！");
      }
    }
  }
}

void configureAPNAndActivatePDP() {
  Serial.println("🔧🔧🔧 CONFIGURE_APN_START - 开始配置 APN 和 PDP 🔧🔧🔧");
  if (SERIAL_VERBOSE) Serial.println("开始配置 APN 和 PDP...");

  // 先测试模块是否响应
  Serial.println("🧪🧪🧪 TESTING_MODULE_RESPONSE - 测试模块响应 🧪🧪🧪");
  SentSerial("AT");
  delay(1000);
  unsigned long testStart = millis();
  bool moduleResponds = false;
  while (millis() - testStart < 2000) {
    if (Serial1.available()) {
      String testResponse = Serial1.readString();
      Serial.println("模块测试响应: " + testResponse);
      if (testResponse.indexOf("OK") != -1) {
        moduleResponds = true;
        Serial.println("✓✓✓ 模块响应正常 ✓✓✓");
        break;
      }
    }
    delay(100);
  }

  if (!moduleResponds) {
    Serial.println("❌❌❌ 模块无响应，跳过 APN 配置 ❌❌❌");
    return;
  }

  // 常用 APN 列表，按优先级尝试
  const char* apnList[] = {"internet", "web", "cmnet", "cmwap", "3gnet", "uninet"};
  const int apnCount = sizeof(apnList) / sizeof(apnList[0]);

  for (int i = 0; i < apnCount; i++) {
    String apn = apnList[i];
    if (SERIAL_VERBOSE) Serial.println("尝试 APN: " + apn);

    // 1. 设置 PDP context
    String cmd = "AT+CGDCONT=1,\"IP\",\"" + apn + "\"";
    if (SERIAL_VERBOSE) Serial.println("发送命令: " + cmd);
    SentSerial(cmd.c_str());

    // 等待响应
    delay(2000);
    bool apnSet = false;
    unsigned long start = millis();
    while (millis() - start < 3000) {
      if (Serial1.available()) {
        String response = Serial1.readString();
        if (SERIAL_VERBOSE) Serial.println("APN 设置响应: " + response);
        if (response.indexOf("OK") != -1) {
          apnSet = true;
          if (SERIAL_VERBOSE) Serial.println("✓ APN 设置成功: " + apn);
          break;
        }
      }
      delay(100);
    }

    if (!apnSet) {
      if (SERIAL_VERBOSE) Serial.println("✗ APN 设置失败: " + apn);
      continue; // 尝试下一个 APN
    }

    // 2. 确保附着到分组域
    if (SERIAL_VERBOSE) Serial.println("检查分组域附着状态...");
    SentSerial("AT+CGATT?");
    delay(2000);

    bool attached = false;
    start = millis();
    while (millis() - start < 3000) {
      if (Serial1.available()) {
        String response = Serial1.readString();
        if (SERIAL_VERBOSE) Serial.println("CGATT 响应: " + response);
        if (response.indexOf("+CGATT: 1") != -1) {
          attached = true;
          if (SERIAL_VERBOSE) Serial.println("✓ 已附着到分组域");
          break;
        } else if (response.indexOf("+CGATT: 0") != -1) {
          if (SERIAL_VERBOSE) Serial.println("未附着，尝试附着...");
          SentSerial("AT+CGATT=1");
          delay(3000);
          break;
        }
      }
      delay(100);
    }

    // 3. 激活 PDP
    if (SERIAL_VERBOSE) Serial.println("激活 PDP...");
    SentSerial("AT+CGACT=1,1");
    delay(3000);

    bool pdpActivated = false;
    start = millis();
    while (millis() - start < 5000) {
      if (Serial1.available()) {
        String response = Serial1.readString();
        if (SERIAL_VERBOSE) Serial.println("PDP 激活响应: " + response);
        if (response.indexOf("OK") != -1) {
          pdpActivated = true;
          if (SERIAL_VERBOSE) Serial.println("✓ PDP 激活成功");
          break;
        }
      }
      delay(100);
    }

    if (pdpActivated) {
      // 4. 查询 IP 地址确认
      if (SERIAL_VERBOSE) Serial.println("查询 IP 地址...");
      delay(1000);
      SentSerial("AT+CGPADDR");
      delay(2000);

      start = millis();
      while (millis() - start < 3000) {
        if (Serial1.available()) {
          String response = Serial1.readString();
          if (SERIAL_VERBOSE) Serial.println("IP 地址响应: " + response);
          break;
        }
        delay(100);
      }

      // 如果激活成功，停止尝试其他 APN
      if (SERIAL_VERBOSE) Serial.println("✓ PDP 配置完成，使用 APN: " + apn);
      break;
    } else {
      if (SERIAL_VERBOSE) Serial.println("✗ PDP 激活失败，尝试下一个 APN");
    }

    delay(2000); // 等待间隔
  }

  if (SERIAL_VERBOSE) Serial.println("APN/PDP 配置流程结束");
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);

  strip.begin();
  strip.show(); // 初始化关闭

  if (SERIAL_VERBOSE) {
    Serial.println("\n=== ESP32-S3 A7670E 初始化开始 ===");
    Serial.println("串口初始化完成");
  }

  // 连接 WiFi（优先）
  wifiConnect();

  if (SERIAL_VERBOSE) {
    Serial.println("\n=== SIMCom A7670E 模块初始化 ===");
    Serial.println("正在连接 SIMCom 模块...");
  }

  while (!SentMessage("AT", 2000)) {
    if (SERIAL_VERBOSE) Serial.println("等待 SIMCom 模块响应...");
    delay(1000);
  }

  if (SERIAL_VERBOSE) Serial.println("SIMCom 模块连接成功！");

  SentSerial("ATE1;");
  SentSerial("AT+CPIN?");
  SentSerial("AT+COPS?");
  SentSerial("AT+CGDCONT?");
  SentSerial("AT+CGREG?");
  SentSerial("AT+SIMCOMATI");

  // 配置 APN 和激活 PDP
  Serial.println("⚙️⚙️⚙️ ABOUT_TO_CONFIGURE_APN - 即将开始配置 APN ⚙️⚙️⚙️");
  if (SERIAL_VERBOSE) {
    Serial.println("\n=== 开始自动配置 APN 和 PDP ===");
  }
  delay(2000); // 等待模块稳定
  Serial.println("🚀🚀🚀 CALLING_CONFIGURE_FUNCTION - 调用配置函数 🚀🚀🚀");
  configureAPNAndActivatePDP();

  if (SERIAL_VERBOSE) {
    Serial.println("=== 初始化完成 ===\n");
  }
}

void loop() {
  if (Serial1.available()) {
    rev = Serial1.readString();
    if (SERIAL_VERBOSE) {
      Serial.print("收到模块响应: ");
      Serial.println(rev);
    }
    parseModuleResponse(rev);
  }

  unsigned long now = millis();
  if (now - lastPoll >= POLL_INTERVAL) {
    lastPoll = now;
    if (SERIAL_VERBOSE) Serial.println("--- 状态轮询 ---");
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

      String fullUrl = String(GEO_SENSOR_API_BASE_URL) + String(GEO_SENSOR_ID) + String("/");
      if (SERIAL_VERBOSE) Serial.println("正在通过 WiFi 上传数据 (PATCH)...");
      bool ok = wifiHttpRequest("PATCH", fullUrl, json);
      if (SERIAL_VERBOSE) {
        Serial.print("WiFi upload result: ");
        Serial.println(ok ? "OK" : "FAILED");
      }
    }
  } else {
    // 若未连接 WiFi，可考虑使用 4G（保留原有逻辑）
  }
}
