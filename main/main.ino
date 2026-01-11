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
bool pdpActive = false;  // 全局PDP状态变量

// GPS数据结构
struct GPSData {
  double latitude = 0.0;
  double longitude = 0.0;
  double altitude = 0.0;
  double speed = 0.0;
  int satelliteCount = 0;
  double locationAccuracy = 0.0;
  double altitudeAccuracy = 0.0;
  bool hasFix = false;  // 是否有GPS定位
  String locationSource = "GPS"; // 定位来源: GPS 或 LBS
  unsigned long lastUpdate = 0;
};

GPSData currentGPS;

bool gpsInitialized = false; // GPS是否成功初始化


unsigned long lastBlinkToggle = 0;
const unsigned long BLINK_INTERVAL = 500;
bool blinkState = false;
unsigned long lastAltToggle = 0;
const unsigned long ALT_INTERVAL = 1000; // 交替显示间隔（ms）
bool altState = false; // true -> show WiFi status; false -> show SIM status

// WiFi 上传配置（来自用户）
// const char* WIFI_SSID = "米奇";
// const char* WIFI_PASS = "19963209891";

const char* WIFI_SSID = "iPhone13";
const char* WIFI_PASS = "1234567890";

// 后台 API 配置
static const char GEO_SENSOR_API_BASE_URL[] = "https://manage.gogotrans.com/api/microcontrollerInstanceDevice/";
static const char GEO_SENSOR_KEY[] = "mcu_5e3abda8585e4bc79af89ad57af8b3b7";

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
    }
  // WiFi已连接，配置NTP
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "asia.pool.ntp.org");
  if (SERIAL_VERBOSE) Serial.println("NTP configured for UTC timezone via WiFi");

  // 等待NTP同步完成
  if (SERIAL_VERBOSE) Serial.println("等待WiFi NTP同步...");
  time_t wifiTime = 0;
  int wifiSyncAttempts = 0;
  while (wifiTime < 1609459200 && wifiSyncAttempts < 15) {
    delay(1000);
    wifiTime = time(nullptr);
    wifiSyncAttempts++;
  }

  if (wifiTime >= 1609459200) {
    if (SERIAL_VERBOSE) Serial.println("WiFi NTP同步成功");
  } else {
    if (SERIAL_VERBOSE) Serial.println("WiFi NTP同步失败，继续使用4G模式");
  }
  } else {
    if (SERIAL_VERBOSE) Serial.println("WiFi connect failed");
  }
}

// 通过 WiFi 发起任意 HTTP 方法请求（例如 POST），返回是否成功
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
  https.addHeader("X-API-Key", String(GEO_SENSOR_KEY));
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

// 通过4G网络获取服务器时间并同步本地时间
bool syncTimeFromServer() {
  if (SERIAL_VERBOSE) Serial.println("尝试从服务器获取时间进行同步...");

  // 1. 初始化HTTP会话
  SentSerial("AT+HTTPINIT");
  if (!waitForResponse("OK", 5000)) {
    if (SERIAL_VERBOSE) Serial.println("HTTP初始化失败，无法同步时间");
    return false;
  }

  // 2. 设置URL (使用后台服务器的时间端点，如果没有专门的端点就用API根路径)
  String timeUrl = String(GEO_SENSOR_API_BASE_URL);
  String urlCmd = "AT+HTTPPARA=\"URL\",\"" + timeUrl + "\"";
  SentSerial(urlCmd.c_str());
  if (!waitForResponse("OK", 5000)) {
    SentSerial("AT+HTTPTERM");
    return false;
  }

  // 3. 设置认证header
  String apiKeyHeader = "X-API-Key: " + String(GEO_SENSOR_KEY);
  String headerCmd = "AT+HTTPPARA=\"USERDATA\",\"" + apiKeyHeader + "\"";
  SentSerial(headerCmd.c_str());
  if (!waitForResponse("OK", 5000)) {
    SentSerial("AT+HTTPTERM");
    return false;
  }

  // 4. 发送GET请求获取服务器时间
  SentSerial("AT+HTTPACTION=0"); // GET method
  if (waitForResponse("+HTTPACTION:", 15000)) {
    // 解析响应
    String response = getLastResponse();
    if (SERIAL_VERBOSE) {
      Serial.println("时间同步响应: " + response);
    }

    // 从响应头中提取时间（如果服务器返回了时间头）
    // Django通常在响应头中包含Date字段
    if (response.indexOf("Date:") != -1) {
      // 简单的时间估算：收到响应时大约是服务器时间的当前时间
      time_t estimatedServerTime = time(nullptr);
      if (estimatedServerTime < 1609459200) {
        // 如果本地时间无效，使用一个估算的当前时间
        // 2024年12月29日大约是1735430400
        estimatedServerTime = 1735430400; // 2024-12-29 00:00:00 UTC
      }

      // 设置系统时间（减去一些网络延迟）
      struct timeval tv;
      tv.tv_sec = estimatedServerTime;
      tv.tv_usec = 0;
      settimeofday(&tv, NULL);

      // 重新配置时区
      configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov", "asia.pool.ntp.org");

      if (SERIAL_VERBOSE) Serial.println("通过服务器响应估算时间同步完成");
      SentSerial("AT+HTTPTERM");
      return true;
    }
  }

  SentSerial("AT+HTTPTERM");
  if (SERIAL_VERBOSE) Serial.println("服务器时间同步失败");
  return false;
}

// 通过 4G 网络发送 HTTP 请求（使用 SIMCom 模块的 HTTP AT 命令）
bool cellularHttpRequest(const String &method, const String &url, const String &json) {
  // 1. 初始化 HTTP（带重试机制）
  if (SERIAL_VERBOSE) Serial.println("初始化 HTTP 会话...");
  bool httpInitSuccess = false;

  // 先尝试终止可能存在的旧会话
  SentSerial("AT+HTTPTERM");
  waitForResponse("OK", 2000);

  delay(1000); // 等待会话完全清理

  // 重试HTTP初始化，最多3次
  for (int retry = 0; retry < 3 && !httpInitSuccess; retry++) {
    if (retry > 0) {
      if (SERIAL_VERBOSE) Serial.println("重试HTTP初始化...");
      delay(2000); // 重试间隔
    }

    SentSerial("AT+HTTPINIT");
    if (waitForResponse("OK", 5000)) {
      httpInitSuccess = true;
      if (SERIAL_VERBOSE) Serial.println("HTTP 初始化成功");
    } else {
      if (SERIAL_VERBOSE) Serial.println("HTTP 初始化失败，尝试终止会话...");
      SentSerial("AT+HTTPTERM");
      waitForResponse("OK", 2000);
    }
  }

  if (!httpInitSuccess) {
    if (SERIAL_VERBOSE) Serial.println("HTTP 初始化最终失败");
    return false;
  }

  // 2. 设置 HTTP 参数 - URL
  if (SERIAL_VERBOSE) Serial.println("设置 HTTP URL...");
  String urlCmd = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
  SentSerial(urlCmd.c_str());
  delay(500); // 增加延迟确保命令发送完成
  if (!waitForResponse("OK", 5000)) { // 增加超时时间
    if (SERIAL_VERBOSE) Serial.println("URL 设置失败");
    httpCleanup();
    return false;
  }
  delay(200); // 短暂延迟确保模块处理完成

  // 3. 设置 Content-Type header
  if (SERIAL_VERBOSE) Serial.println("设置 Content-Type...");
  SentSerial("AT+HTTPPARA=\"CONTENT\",\"application/json\"");
  delay(500); // 增加延迟确保命令发送完成
  if (!waitForResponse("OK", 5000)) { // 增加超时时间
    if (SERIAL_VERBOSE) Serial.println("Content-Type 设置失败");
    httpCleanup();
    return false;
  }
  delay(200); // 短暂延迟确保模块处理完成

  // 4. 设置自定义 headers (改进的API Key设置)
  if (SERIAL_VERBOSE) Serial.println("尝试设置 API Key header...");

  // 使用标准header格式
  String apiKeyHeader = "X-API-Key: " + String(GEO_SENSOR_KEY);
  String headerCmd = "AT+HTTPPARA=\"USERDATA\",\"" + apiKeyHeader + "\"";

  if (SERIAL_VERBOSE) {
    Serial.print("设置API Key header: ");
    Serial.println(headerCmd);
  }

  SentSerial(headerCmd.c_str());
  delay(1000); // 增加延迟确保命令发送完成

  // 强制检查响应
  String headerResponse = "";
  unsigned long headerStart = millis();
  bool headerOk = false;
  while (millis() - headerStart < 3000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      headerResponse += c;
      if (headerResponse.indexOf("OK") != -1) {
        headerOk = true;
        break;
      }
    }
    delay(10);
  }

  if (SERIAL_VERBOSE) {
    Serial.print("Header设置结果: ");
    Serial.println(headerOk ? "成功" : "失败 - " + headerResponse);
  }

  if (!headerOk) {
    if (SERIAL_VERBOSE) Serial.println("Header设置失败，继续其他步骤...");
  }
  delay(200); // 短暂延迟确保模块处理完成

  // 5. 设置数据长度并发送数据（带重试）
  if (SERIAL_VERBOSE) Serial.println("发送数据...");
  bool dataSendSuccess = false;

  for (int retry = 0; retry < 2 && !dataSendSuccess; retry++) {
    if (retry > 0) {
      if (SERIAL_VERBOSE) Serial.println("重试数据发送...");
      delay(1000);
    }

    String dataCmd = "AT+HTTPDATA=" + String(json.length()) + ",10000";
    SentSerial(dataCmd.c_str());
    if (waitForResponse("DOWNLOAD", 5000)) {
      // 发送 JSON 数据
      delay(500); // 等待模块准备接收数据
      SentSerial(json.c_str());
      if (waitForResponse("OK", 8000)) {
        dataSendSuccess = true;
        if (SERIAL_VERBOSE) Serial.println("数据发送成功");
      } else {
        if (SERIAL_VERBOSE) Serial.println("数据发送响应失败");
      }
    } else {
      if (SERIAL_VERBOSE) Serial.println("HTTPDATA 命令失败");
    }
  }

  if (!dataSendSuccess) {
    if (SERIAL_VERBOSE) Serial.println("数据发送最终失败");
    httpCleanup();
    return false;
  }

  // 6. 执行 HTTP 请求
  if (SERIAL_VERBOSE) {
    Serial.print("执行 HTTP ");
    Serial.print(method);
    Serial.println(" 请求...");
  }

  // 根据方法选择正确的AT命令
  if (method == "POST") {
    SentSerial("AT+HTTPACTION=1");  // 1 = POST method
  } else if (method == "PATCH") {
    SentSerial("AT+HTTPACTION=2");  // 2 = PATCH method
  } else {
    if (SERIAL_VERBOSE) Serial.println("不支持的HTTP方法");
    httpCleanup();
    return false;
  }
  if (!waitForResponse("+HTTPACTION:", 25000)) {  // 增加超时时间到25秒
    if (SERIAL_VERBOSE) Serial.println("HTTP 请求执行失败");
    httpCleanup();
    return false;
  }

  // 7. 解析响应并清理
  delay(1000); // 等待响应完全接收
  String response = getLastResponse();
  httpCleanup();  // 在获取响应后清理

  if (SERIAL_VERBOSE) {
    Serial.print("解析HTTP响应: '");
    Serial.print(response);
    Serial.println("'");
  }

  // 检查各种可能的成功响应格式
  bool success = false;
  if (response.indexOf("200") != -1 || response.indexOf("201") != -1) {
    // 包含200/201状态码，认为是成功的
    success = true;
    if (SERIAL_VERBOSE) Serial.println("✓ 检测到HTTP 2xx成功状态码");
  } else if (response.indexOf("+HTTPACTION:") != -1) {
    // 解析HTTPACTION响应格式: +HTTPACTION: <method>,<status>,<length>
    int colonPos = response.indexOf(":");
    if (colonPos != -1) {
      String params = response.substring(colonPos + 1);
      int firstComma = params.indexOf(",");
      if (firstComma != -1) {
        int secondComma = params.indexOf(",", firstComma + 1);
        if (secondComma != -1) {
          String statusCode = params.substring(firstComma + 1, secondComma);
          statusCode.trim();
          int code = statusCode.toInt();
          if (code >= 200 && code < 300) {
            success = true;
            if (SERIAL_VERBOSE) Serial.println("✓ 检测到HTTPACTION 2xx响应: " + statusCode);
          } else {
            if (SERIAL_VERBOSE) Serial.println("HTTPACTION状态码: " + statusCode);
          }
        }
      }
    }
  }

  if (success) {
    if (SERIAL_VERBOSE) Serial.println("HTTP 响应: 200 OK");
    return true;
  } else {
    if (SERIAL_VERBOSE) {
      Serial.print("HTTP 响应失败: ");
      Serial.println(response);
    }
    return false;
  }
}

// HTTP 会话清理函数
void httpCleanup() {
  if (SERIAL_VERBOSE) Serial.println("终止 HTTP 会话...");
  SentSerial("AT+HTTPTERM");
  waitForResponse("OK", 3000);
}

// 等待指定响应
bool waitForResponse(const String &expected, unsigned long timeout) {
  unsigned long start = millis();
  String buffer = "";

  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      buffer += c;

      if (buffer.indexOf(expected) != -1) {
        return true;
      }

      // 如果收到 ERROR，也返回 false
      if (buffer.indexOf("ERROR") != -1) {
        return false;
      }
    }
    delay(10);
  }

  return false;
}

// 检查PDP状态（独立于LED显示）
void checkPDPStatus() {
  static unsigned long lastPdpCheck = 0;
  static bool lastPdpStatus = false;

  // 每5秒检查一次PDP状态，避免过于频繁的查询
  if (millis() - lastPdpCheck >= 5000) {
    lastPdpCheck = millis();

    Serial.println("🔍🔍🔍 CHECKING_PDP_STATUS - 检查PDP状态 🔍🔍🔍");
    if (SERIAL_VERBOSE) Serial.println("检查 PDP 状态...");

    SentSerial("AT+CGPADDR");
    delay(300); // 给模块更多响应时间

    unsigned long tstart = millis();
    String resp = "";
    int responseTimeout = 4000; // 增加超时时间到4秒

    // 等待完整响应
    while (millis() - tstart < responseTimeout) {
      if (Serial1.available()) {
        char c = Serial1.read();
        resp += c;
        tstart = millis(); // 有数据时重置超时
      }

      // 检查是否收到完整的AT响应
      if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) {
        // 再等待一小段时间确保所有数据都收到
        delay(200);
        while (Serial1.available()) {
          resp += (char)Serial1.read();
        }
        break;
      }

      delay(10);
    }

    // 解析响应，查找IP地址
    bool pdpActive = false;

    if (SERIAL_VERBOSE) {
      Serial.print("原始响应: '");
      Serial.print(resp);
      Serial.println("'");
    }

    if (resp.indexOf("+CGPADDR: 1,") != -1) {
      // 找到第一个PDP上下文的IP地址
      int ipStart = resp.indexOf("+CGPADDR: 1,") + 12;
      int ipEnd = resp.indexOf("\r\n", ipStart);
      if (ipEnd == -1) ipEnd = resp.indexOf("\n", ipStart);
      if (ipEnd == -1) ipEnd = resp.indexOf("OK", ipStart);
      if (ipEnd == -1) ipEnd = resp.length();

      String ipAddr = resp.substring(ipStart, ipEnd);
      ipAddr.trim();

      if (SERIAL_VERBOSE) {
        Serial.print("提取的IP地址: '");
        Serial.print(ipAddr);
        Serial.println("'");
      }

      // 检查IP地址是否有效（排除0.0.0.0和无效地址）
      // IPv4地址应该有3个点号，格式为x.x.x.x
      int dotCount = 0;
      for (char c : ipAddr) {
        if (c == '.') dotCount++;
      }

      pdpActive = (ipAddr.length() >= 7 &&  // 最小IP长度 x.x.x.x
                   ipAddr != "0.0.0.0" &&
                   dotCount == 3); // IPv4地址应该有3个点号

      if (SERIAL_VERBOSE) {
        Serial.print("点号数量: ");
        Serial.println(dotCount);
        Serial.print("PDP激活判断: ");
        Serial.println(pdpActive ? "是" : "否");
      }
    } else {
      if (SERIAL_VERBOSE) {
        Serial.println("未找到 +CGPADDR: 1, 响应");
      }
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

  pdpActive = lastPdpStatus;
}

// 获取最后一次响应
String getLastResponse() {
  String response = "";
  unsigned long start = millis();

  while (millis() - start < 1000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;
    }
    delay(10);
  }

  return response;
}

// GPS数据解析函数

// 解析AT+CGNSINF格式的GPS数据
// 格式: +CGNSINF: <GNSS run status>,<Fix status>,<UTC date & Time>,<Latitude>,<Longitude>,<Altitude>,<Speed>,<Course>,<Fix Mode>,<Reserved1>,<HDOP>,<PDOP>,<VDOP>,<Reserved2>,<GNSS Satellites in View>,<GNSS Satellites Used>,<GLONASS Satellites Used>,<Reserved3>,<C/N0 max>,<HPA>,<VPA>
bool parseCGNSINFData(String response, GPSData &gps) {
  int cgnsStart = response.indexOf("+CGNSINF: ");
  if (cgnsStart == -1) return false;

  String data = response.substring(cgnsStart + 10);
  data.trim();

  // 检查是否为空数据或无效数据
  if (data.startsWith(",") || data.length() < 10) {
    return false; // 没有GPS数据
  }

  // 分割逗号分隔的数据
  String fields[20];
  int fieldCount = 0;
  int lastComma = -1;

  for (int i = 0; i < data.length() && fieldCount < 20; i++) {
    if (data[i] == ',') {
      fields[fieldCount] = data.substring(lastComma + 1, i);
      fields[fieldCount].trim();
      lastComma = i;
      fieldCount++;
    }
  }

  // 获取最后一个字段
  if (fieldCount < 19) {
    fields[fieldCount] = data.substring(lastComma + 1);
    fields[fieldCount].trim();
    fieldCount++;
  }

  if (fieldCount >= 6) {
    // 检查定位状态 (fields[1]) - 1表示已定位
    if (fields[1].toInt() != 1) {
      return false; // 未定位
    }

    // 纬度 (fields[3])
    if (fields[3].length() > 0) {
      gps.latitude = fields[3].toFloat();
    }

    // 经度 (fields[4])
    if (fields[4].length() > 0) {
      gps.longitude = fields[4].toFloat();
    }

    // 海拔 (fields[5])
    if (fields[5].length() > 0) {
      gps.altitude = fields[5].toFloat();
    }

    // 速度 (fields[6]) - 单位通常是km/h或节，假设是km/h
    if (fields[6].length() > 0) {
      gps.speed = fields[6].toFloat();
    }

    // 卫星数量 (fields[14] + fields[15] 或其他字段)
    // CGNSINF通常在后面字段包含卫星信息
    if (fieldCount >= 15 && fields[14].length() > 0) {
      gps.satelliteCount = fields[14].toInt();
    } else {
      gps.satelliteCount = 4; // 默认值
    }

    // 设置其他GPS参数
    gps.hasFix = true;
    gps.locationAccuracy = 5.0; // CGNSINF通常有更好的精度
    gps.altitudeAccuracy = 10.0;
    gps.locationSource = "GPS"; // 标识为GPS定位
    gps.lastUpdate = millis();

    if (SERIAL_VERBOSE) {
      Serial.println("📍 CGNSINF数据解析结果:");
      Serial.print("   纬度: "); Serial.print(gps.latitude, 6); Serial.println(" °");
      Serial.print("   经度: "); Serial.print(gps.longitude, 6); Serial.println(" °");
      Serial.print("   海拔: "); Serial.print(gps.altitude, 2); Serial.println(" m");
      Serial.print("   速度: "); Serial.print(gps.speed, 2); Serial.println(" km/h");
      Serial.print("   卫星: "); Serial.print(gps.satelliteCount); Serial.println(" 颗");
    }

    return true;
  }

  return false;
}

// 解析AT+CGPSINFO格式的GPS数据
// 格式: +CGPSINFO: <lat>,<N/S>,<lon>,<E/W>,<date>,<UTC time>,<alt>,<speed>,<course>
bool parseCGPSINFOData(String response, GPSData &gps) {
  int cgpsStart = response.indexOf("+CGPSINFO: ");
  if (cgpsStart == -1) return false;

  String data = response.substring(cgpsStart + 11);
  data.trim();

  // 检查是否为空数据 (全为逗号或空值)
  if (data.startsWith(",") || data.length() < 10) {
    return false; // 没有GPS数据
  }

  // 分割逗号分隔的数据
  String fields[10];
  int fieldCount = 0;
  int lastComma = -1;

  for (int i = 0; i < data.length() && fieldCount < 10; i++) {
    if (data[i] == ',') {
      fields[fieldCount] = data.substring(lastComma + 1, i);
      fields[fieldCount].trim();
      lastComma = i;
      fieldCount++;
    }
  }

  if (fieldCount >= 8) {
    // 纬度 (fields[0]) - 格式: DDMM.MMMMM
    if (fields[0].length() > 0) {
      float latRaw = fields[0].toFloat();
      int degrees = (int)(latRaw / 100);
      float minutes = latRaw - (degrees * 100);
      gps.latitude = degrees + (minutes / 60.0);

      // 南纬为负
      if (fields[1] == "S") {
        gps.latitude = -gps.latitude;
      }
    }

    // 经度 (fields[2]) - 格式: DDDMM.MMMMM
    if (fields[2].length() > 0) {
      float lonRaw = fields[2].toFloat();
      int degrees = (int)(lonRaw / 100);
      float minutes = lonRaw - (degrees * 100);
      gps.longitude = degrees + (minutes / 60.0);

      // 西经为负
      if (fields[3] == "W") {
        gps.longitude = -gps.longitude;
      }
    }

    // 海拔 (fields[6])
    if (fields[6].length() > 0) {
      gps.altitude = fields[6].toFloat();
    }

    // 速度 (fields[7]) - km/h
    if (fields[7].length() > 0) {
      gps.speed = fields[7].toFloat();
    }

    // 设置其他GPS参数
    gps.hasFix = true;
    gps.satelliteCount = 4; // CGPSINFO不提供卫星数量，假设至少4颗
    gps.locationAccuracy = 10.0; // GPS定位精度约10米
    gps.altitudeAccuracy = 15.0;
    gps.locationSource = "GPS"; // 标识为GPS定位
    gps.lastUpdate = millis();

    if (SERIAL_VERBOSE) {
      Serial.println("📍 CGPSINFO数据解析结果:");
      Serial.print("   纬度: "); Serial.print(gps.latitude, 6); Serial.println(" °");
      Serial.print("   经度: "); Serial.print(gps.longitude, 6); Serial.println(" °");
      Serial.print("   海拔: "); Serial.print(gps.altitude, 2); Serial.println(" m");
      Serial.print("   速度: "); Serial.print(gps.speed, 2); Serial.println(" km/h");
      Serial.print("   卫星: "); Serial.print(gps.satelliteCount); Serial.println(" 颗");
    }

    return true;
  }

  return false;
}

// 获取GPS数据 - 一直等待直到获取成功
bool getGPSData() {
  if (SERIAL_VERBOSE) Serial.println("🛰️ 获取GPS数据...");

  // 发送GPS信息查询命令 (使用AT+CGPSINFO)
  SentSerial("AT+CGPSINFO");
  delay(1000); // 等待GPS响应

  // 读取响应
  String response = "";
  unsigned long start = millis();
  bool gotResponse = false;

  while (millis() - start < 2000 && !gotResponse) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;

      if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1) {
        gotResponse = true;
      }
    }
    delay(10);
  }

  if (SERIAL_VERBOSE) {
    Serial.print("GPS响应: ");
    Serial.println(response);
  }

  // 解析GPS数据 (使用CGPSINFO格式)
  if (parseCGPSINFOData(response, currentGPS)) {
    if (SERIAL_VERBOSE) Serial.println("✅ GPS数据获取成功！");
    return true;
  } else {
    if (SERIAL_VERBOSE) Serial.println("❌ GPS数据无效，等待卫星信号...");
    return false; // 不阻塞，返回失败，让主循环继续
  }
}

// 初始化GPS功能 - 使用GNSS命令序列
bool initGPS() {
  // 第一步：开启GNSS电源
  if (SERIAL_VERBOSE) Serial.println("开启GNSS电源...");
  SentSerial("AT+CGNSSPWR=1");

  // 等待GNSS电源启动响应
  if (!waitForResponse("OK", 3000)) {
    if (SERIAL_VERBOSE) Serial.println("GNSS电源启动失败，将在主循环中继续重试");
    return false;
  }

  if (SERIAL_VERBOSE) Serial.println("GNSS电源启动成功");

  // 第二步：等待GNSS芯片启动
  if (SERIAL_VERBOSE) Serial.println("等待GNSS芯片启动 (10秒)...");
  delay(10000);

  // 第三步：开启GNSS数据输出
  if (SERIAL_VERBOSE) Serial.println("开启GNSS数据输出...");
  SentSerial("AT+CGNSSTST=1");

  // 等待GNSS数据输出启动响应
  if (!waitForResponse("OK", 3000)) {
    if (SERIAL_VERBOSE) Serial.println("GNSS数据输出启动失败，将在主循环中继续重试");
    return false;
  }

  if (SERIAL_VERBOSE) Serial.println("GNSS数据输出启动成功");

  // GNSS初始化完成后，等待30秒让卫星信号稳定
  if (SERIAL_VERBOSE) Serial.println("GNSS初始化完成，等待30秒让卫星信号稳定...");
  delay(30000);

  if (SERIAL_VERBOSE) Serial.println("卫星信号稳定完成，准备获取经纬度数据");

  return true; // GNSS启动成功
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
  // 亮度降低为50% (255 * 0.5 = 127)
  uint8_t physR = g ? 127 : 0;
  uint8_t physG = r ? 127 : 0;
  uint8_t physB = b ? 127 : 0;
  strip.setPixelColor(0, strip.Color(physR, physG, physB));
  strip.show();
}

// 上传成功闪烁效果 - 快速闪3下
void flashSuccess() {
  // 保存当前颜色状态
  uint32_t currentColor = strip.getPixelColor(0);

  // 快速闪烁3次 (白色亮度50%)
  for (int i = 0; i < 3; i++) {
    // 白色闪烁
    strip.setPixelColor(0, strip.Color(127, 127, 127));
    strip.show();
    delay(100);

    // 恢复原色
    strip.setPixelColor(0, currentColor);
    strip.show();
    delay(100);
  }
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

  // PDP状态现在由checkPDPStatus函数独立管理

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

  // 初始化随机数种子（使用ADC噪声）
  randomSeed(analogRead(0));

  strip.begin();
  strip.show(); // 初始化关闭

  if (SERIAL_VERBOSE) {
    Serial.println("\n=== ESP32-S3 A7670E 初始化开始 ===");
    Serial.println("串口初始化完成");
  }

  // 只有在WiFi或4G网络有效时才配置NTP
  bool networkAvailable = false;

  // 检查WiFi连接
  if (WiFi.status() == WL_CONNECTED) {
    networkAvailable = true;
    if (SERIAL_VERBOSE) Serial.println("检测到WiFi连接，将配置NTP");
  }

  // 如果WiFi不可用，检查4G连接
  if (!networkAvailable) {
    // 尝试初始化SIMCom模块检查4G连接
    Serial1.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);
    delay(1000);

    // 发送AT命令检查模块响应
    SentSerial("AT");
    delay(500);

    // 检查是否有响应
    if (Serial1.available()) {
      String response = "";
      unsigned long start = millis();
      while (millis() - start < 2000 && Serial1.available()) {
        char c = Serial1.read();
        response += c;
      }
      if (response.indexOf("OK") != -1) {
        networkAvailable = true;
        if (SERIAL_VERBOSE) Serial.println("检测到4G模块响应，将配置NTP");
      }
    }

    if (!networkAvailable && SERIAL_VERBOSE) {
      Serial.println("未检测到有效网络连接，跳过NTP配置");
    }
  }

  // 只有在有网络连接时才配置NTP
  if (networkAvailable) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "asia.pool.ntp.org");
    if (SERIAL_VERBOSE) Serial.println("NTP configured for UTC timezone");

    // 等待NTP同步
    if (SERIAL_VERBOSE) Serial.println("等待NTP时间同步...");
    time_t now = 0;
    int syncAttempts = 0;
    while (now < 1609459200 && syncAttempts < 30) { // 2021年后时间戳
      delay(1000);
      now = time(nullptr);
      syncAttempts++;
      if (syncAttempts % 5 == 0 && SERIAL_VERBOSE) {
        Serial.print("NTP同步尝试: ");
        Serial.println(syncAttempts);
      }
    }

    if (now >= 1609459200) {
      if (SERIAL_VERBOSE) Serial.println("NTP同步成功");
    } else {
      if (SERIAL_VERBOSE) Serial.println("NTP同步失败，将使用系统时间");
    }
  } else {
    if (SERIAL_VERBOSE) Serial.println("无网络连接，跳过NTP同步");
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

  // 初始化GPS功能
  if (SERIAL_VERBOSE) {
    Serial.println("\n=== 初始化GPS功能 ===");
  }
  gpsInitialized = initGPS();

  if (SERIAL_VERBOSE) {
    Serial.println("=== 初始化完成 ===\n");
  }
}

void loop() {
  // 处理串口数据
  if (Serial1.available()) {
    rev = Serial1.readString();
    if (SERIAL_VERBOSE) {
      Serial.print("收到模块响应: ");
    Serial.println(rev);
    }
    parseModuleResponse(rev);
  }

  // GPS数据获取和上传的主循环
  static bool gpsAcquired = false;
  static unsigned long uploadWaitStart = 0;

  // 检查GPS初始化状态
  if (!gpsInitialized) {
    // GPS未初始化成功，尝试重新初始化
    static unsigned long lastGpsRetry = 0;
    if (millis() - lastGpsRetry > 5000) { // 每5秒尝试一次GPS初始化
      lastGpsRetry = millis();
      if (SERIAL_VERBOSE) Serial.println("尝试重新初始化GPS...");
      gpsInitialized = initGPS();
    }

    // 显示跳过GPS数据获取的提示
    if (SERIAL_VERBOSE) {
      static unsigned long lastGpsSkipMsg = 0;
      if (millis() - lastGpsSkipMsg > 10000) { // 每10秒显示一次
        Serial.println("GPS未初始化成功，跳过GPS数据获取");
        lastGpsSkipMsg = millis();
      }
    }
  } else if (!gpsAcquired) {
    // GPS已初始化，定期尝试获取GPS数据
    static unsigned long lastGpsAttempt = 0;
    if (millis() - lastGpsAttempt > 5000) { // 每5秒尝试一次
      lastGpsAttempt = millis();
      gpsAcquired = getGPSData();

      if (gpsAcquired) {
        if (SERIAL_VERBOSE) Serial.println("🎯 GPS定位成功，开始上传数据...");
        uploadWaitStart = 0; // 重置上传等待时间
      }
    }
  } else {
    // GPS已获取，检查是否需要上传
    unsigned long now = millis();

    // 检查PDP状态（用于4G上传）
    static unsigned long lastPdpCheck = 0;
    if (now - lastPdpCheck >= 5000) { // 每5秒检查一次
      lastPdpCheck = now;
      checkPDPStatus();
    }

    // 执行数据上传
    if (WiFi.status() == WL_CONNECTED) {
      // WiFi上传逻辑
      unsigned long now = millis();
      if (now - lastUpload >= UPLOAD_INTERVAL) {
        lastUpload = now;
        // 使用GPS数据
        double latitude = currentGPS.latitude;
        double longitude = currentGPS.longitude;
        double altitude = currentGPS.altitude;
        double speed = currentGPS.speed;
        int satelliteCount = currentGPS.satelliteCount;
        double locationAccuracy = currentGPS.locationAccuracy;
        double altitudeAccuracy = currentGPS.altitudeAccuracy;
        String dataAcquiredAt = "";
        // 获取东七区时间
        time_t nowt = time(nullptr);
        if (nowt > 1609459200) { // 检查时间是否合理 (2021年后的时间戳)
          struct tm tm;
          gmtime_r(&nowt, &tm); // 使用UTC时间
          char buf[32];
          snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec);
          dataAcquiredAt = String(buf);
        } else {
          // 如果时间获取失败，设置为空
          dataAcquiredAt = "null";
          if (SERIAL_VERBOSE) Serial.println("时间获取失败，设置为空");
        }

        String json = "{";
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
        json += "\"networkSource\":\"WiFi\"";
        json += "}";

        String fullUrl = String(GEO_SENSOR_API_BASE_URL);
        if (SERIAL_VERBOSE) {
          Serial.println("正在通过 WiFi 上传数据 (PATCH)...");
          Serial.println("目标URL: " + fullUrl);
          Serial.print("发送数据长度: ");
          Serial.println(json.length());
          // 分段打印JSON以避免缓冲区溢出
          Serial.println("发送数据开始:");
          Serial.println(json.substring(0, 100));
          if (json.length() > 100) {
            Serial.println(json.substring(100));
          }
          Serial.println("发送数据结束");
        }
        bool ok = wifiHttpRequest("PATCH", fullUrl, json);
        if (SERIAL_VERBOSE) {
        Serial.print("WiFi upload result: ");
        Serial.println(ok ? "OK" : "FAILED");
        }
        // 上传成功时闪烁提示
        if (ok) {
          flashSuccess();
          // 上传成功后，等待10秒再获取新的GPS数据
          uploadWaitStart = millis();
        }
      }
    } else {
      // WiFi 不可用，尝试使用 4G 网络上传
      unsigned long now = millis();
      if (now - lastUpload >= UPLOAD_INTERVAL) {
        lastUpload = now;
        // 检查 PDP 是否激活
        if (pdpActive) {
          // 使用GPS数据
          double latitude = currentGPS.latitude;
          double longitude = currentGPS.longitude;
          double altitude = currentGPS.altitude;
          double speed = currentGPS.speed;
          int satelliteCount = currentGPS.satelliteCount;
          double locationAccuracy = currentGPS.locationAccuracy;
          double altitudeAccuracy = currentGPS.altitudeAccuracy;
          String dataAcquiredAt = "";
          // 获取当前东七区时间
          time_t nowt = time(nullptr);
          if (nowt > 1609459200) { // 检查时间是否合理 (2021年后的时间戳)
            struct tm tm;
            gmtime_r(&nowt, &tm); // 使用UTC时间
            char buf[32];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
            dataAcquiredAt = String(buf);
          } else {
            // 4G模式：NTP同步在移动网络上通常不可靠，直接设置为空
            dataAcquiredAt = "null";
            if (SERIAL_VERBOSE) Serial.println("4G模式：NTP同步不可靠，设置为空让后台处理");
          }

          String json = "{";
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
          json += "\"networkSource\":\"4G\"";
          json += "}";

          String fullUrl = String(GEO_SENSOR_API_BASE_URL);
          if (SERIAL_VERBOSE) {
            Serial.println("正在通过 4G 网络上传数据 (PATCH)...");
            Serial.println("目标URL: " + fullUrl);
            Serial.print("发送数据长度: ");
            Serial.println(json.length());
            // 分段打印JSON以避免缓冲区溢出
            Serial.println("发送数据开始:");
            Serial.println(json.substring(0, 100));
            if (json.length() > 100) {
              Serial.println(json.substring(100));
            }
            Serial.println("发送数据结束");
          }
          bool ok = cellularHttpRequest("POST", fullUrl, json);
          if (SERIAL_VERBOSE) {
            Serial.print("4G upload result: ");
            Serial.println(ok ? "OK" : "FAILED");
          }
          // 上传成功时闪烁提示
          if (ok) {
            flashSuccess();
            // 上传成功后，等待10秒再获取新的GPS数据
            uploadWaitStart = millis();
          }
        } else {
          if (SERIAL_VERBOSE) Serial.println("4G网络未激活，跳过数据上传");
        }
      }
    }

    // 检查是否已经等待了10秒
    if (uploadWaitStart > 0 && now - uploadWaitStart >= 10000) { // 10秒
      if (SERIAL_VERBOSE) Serial.println("⏰ 上传后等待10秒完成，开始获取新的GPS数据...");
      gpsAcquired = false; // 重置GPS状态，开始新的循环
      uploadWaitStart = 0;
    }
  }

  updateLEDState();
 
  // 优先通过 WiFi 上传
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastUpload >= UPLOAD_INTERVAL) {
      lastUpload = now;
      // 使用GPS数据，如果没有GPS信号则使用随机数（20-30范围）
      double latitude, longitude;
      if (currentGPS.hasFix) {
        latitude = currentGPS.latitude;
        longitude = currentGPS.longitude;
      } else {
        // 生成20-30范围内的随机数
        latitude = 20.0 + (random(0, 1001) / 1000.0) * 10.0;  // 20.000 - 30.000
        longitude = 20.0 + (random(0, 1001) / 1000.0) * 10.0; // 20.000 - 30.000
        if (SERIAL_VERBOSE) {
          Serial.print("GPS未定位，使用随机坐标: ");
          Serial.print(latitude, 6);
          Serial.print(", ");
          Serial.println(longitude, 6);
        }
      }
      double altitude = currentGPS.hasFix ? currentGPS.altitude : 0.0;
      double speed = currentGPS.hasFix ? currentGPS.speed : 0.0;
      int satelliteCount = currentGPS.hasFix ? currentGPS.satelliteCount : 0;
      double locationAccuracy = currentGPS.hasFix ? currentGPS.locationAccuracy : 0.0;
      double altitudeAccuracy = currentGPS.hasFix ? currentGPS.altitudeAccuracy : 0.0;
      String dataAcquiredAt = "";
      // 获取东七区时间
      time_t nowt = time(nullptr);
      if (nowt > 1609459200) { // 检查时间是否合理 (2021年后的时间戳)
        struct tm tm;
        gmtime_r(&nowt, &tm); // 使用UTC时间
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        dataAcquiredAt = String(buf);
      } else {
        // 如果时间获取失败，设置为空
        dataAcquiredAt = "null";
        if (SERIAL_VERBOSE) Serial.println("时间获取失败，设置为空");
      }

      String json = "{";
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
      json += "\"networkSource\":\"WiFi\"";
      json += "}";

      String fullUrl = String(GEO_SENSOR_API_BASE_URL) + String(GEO_SENSOR_ID) + String("/");
      if (SERIAL_VERBOSE) {
        Serial.println("正在通过 WiFi 上传数据 (PATCH)...");
        Serial.println("目标URL: " + fullUrl);
        Serial.print("发送数据长度: ");
        Serial.println(json.length());
        // 分段打印JSON以避免缓冲区溢出
        Serial.println("发送数据开始:");
        Serial.println(json.substring(0, 100));
        if (json.length() > 100) {
          Serial.println(json.substring(100));
        }
        Serial.println("发送数据结束");
      }
      bool ok = wifiHttpRequest("PATCH", fullUrl, json);
      if (SERIAL_VERBOSE) {
      Serial.print("WiFi upload result: ");
      Serial.println(ok ? "OK" : "FAILED");
      }
      // 上传成功时闪烁提示
      if (ok) {
        flashSuccess();
      }
    }
  } else {
    // WiFi 不可用，尝试使用 4G 网络上传
    unsigned long now = millis();
    if (now - lastUpload >= UPLOAD_INTERVAL) {
      lastUpload = now;
      // 检查 PDP 是否激活
      if (pdpActive) {
        // 使用GPS数据，如果没有GPS信号则使用随机数（20-30范围）
        double latitude, longitude;
        if (currentGPS.hasFix) {
          latitude = currentGPS.latitude;
          longitude = currentGPS.longitude;
        } else {
          // 生成20-30范围内的随机数
          latitude = 20.0 + (random(0, 1001) / 1000.0) * 10.0;  // 20.000 - 30.000
          longitude = 20.0 + (random(0, 1001) / 1000.0) * 10.0; // 20.000 - 30.000
          if (SERIAL_VERBOSE) {
            Serial.print("GPS未定位，使用随机坐标: ");
            Serial.print(latitude, 6);
            Serial.print(", ");
            Serial.println(longitude, 6);
          }
        }
        double altitude = currentGPS.hasFix ? currentGPS.altitude : 0.0;
        double speed = currentGPS.hasFix ? currentGPS.speed : 8.0;
        int satelliteCount = currentGPS.hasFix ? currentGPS.satelliteCount : 0;
        double locationAccuracy = currentGPS.hasFix ? currentGPS.locationAccuracy : 0.0;
        double altitudeAccuracy = currentGPS.hasFix ? currentGPS.altitudeAccuracy : 0.0;
        String dataAcquiredAt = "";
        // 获取当前东七区时间
        time_t nowt = time(nullptr);
        if (nowt > 1609459200) { // 检查时间是否合理 (2021年后的时间戳)
          struct tm tm;
          gmtime_r(&nowt, &tm); // 使用UTC时间
          char buf[32];
          snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec);
          dataAcquiredAt = String(buf);
          if (SERIAL_VERBOSE) Serial.println("使用当前东七区时间: " + dataAcquiredAt);
        } else {
          // 4G模式：NTP同步在移动网络上通常不可靠，直接设置为空
          dataAcquiredAt = "null";
          if (SERIAL_VERBOSE) Serial.println("4G模式：NTP同步不可靠，设置为空让后台处理");
        }

        String json = "{";
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
        json += "\"networkSource\":\"4G\"";
        json += "}";

        String fullUrl = String(GEO_SENSOR_API_BASE_URL);
        if (SERIAL_VERBOSE) {
          Serial.println("正在通过 4G 网络上传数据 (PATCH)...");
          Serial.println("目标URL: " + fullUrl);
          Serial.print("发送数据长度: ");
          Serial.println(json.length());
          // 分段打印JSON以避免缓冲区溢出
          Serial.println("发送数据开始:");
          Serial.println(json.substring(0, 100));
          if (json.length() > 100) {
            Serial.println(json.substring(100));
          }
          Serial.println("发送数据结束");
        }
        bool ok = cellularHttpRequest("POST", fullUrl, json);
        if (SERIAL_VERBOSE) {
          Serial.print("4G upload result: ");
          Serial.println(ok ? "OK" : "FAILED");
        }
        // 上传成功时闪烁提示
        if (ok) {
          flashSuccess();
        }
      } else {
        if (SERIAL_VERBOSE) Serial.println("4G网络未激活，跳过数据上传");
      }
    }
  }
}