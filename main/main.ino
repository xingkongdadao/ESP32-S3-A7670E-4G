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

// 记录上一次状态，用于检测状态切换
ModemState previousState = STATE_NO_SIM;

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

// ========== GPS ==========
bool gpsHasFix = false;
float gpsLatitude = 0.0;
float gpsLongitude = 0.0;
float gpsAltitude = 0.0;
float gpsSpeed = 0.0; // m/s
unsigned long lastGPSCheck = 0;
const unsigned long GPS_CHECK_INTERVAL = 5000; // 每 5s 检查一次

// ========== 上传配置 ==========
inline constexpr char GEO_SENSOR_KEY[] = "mcu_0fda5a6b27214e1eb30fe7fe2c5d4f69";
inline constexpr char GEO_SENSOR_ID[] = "4ccd94bc-c947-11f0-9ea2-12d3851b737f";
const char UPLOAD_HOST[] = "manage.gogotrans.com";
const int UPLOAD_PORT = 443;
const char UPLOAD_PATH[] = "/api/device/geoSensor/";
const unsigned long UPLOAD_INTERVAL = 10000; // 每 10 秒上传一次
unsigned long lastUpload = 0;

// 将时间字符串 YYYYMMDDhhmmss.sss 转为 ISO8601 并加时区偏移（小时）
String formatIsoFromCGNSTime(const String &utc, int tzHours = 8) {
  if (utc.length() < 14) return String("");
  String year = utc.substring(0, 4);
  String month = utc.substring(4, 6);
  String day = utc.substring(6, 8);
  String hour = utc.substring(8, 10);
  String min = utc.substring(10, 12);
  String sec = utc.substring(12, 14);
  char buf[40];
  sprintf(buf, "%s-%s-%sT%s:%s:%s%+03d:00", year.c_str(), month.c_str(), day.c_str(), hour.c_str(), min.c_str(), sec.c_str(), tzHours);
  return String(buf);
}

// ========== Quectel APN 激活（针对中国移动） ==========
String getIMSI() {
  String rsp = sendAT("AT+CIMI", 2000);
  rsp.trim();
  // 提取首个连续数字串
  String digits = "";
  for (unsigned int i = 0; i < rsp.length(); ++i) {
    if (isDigit(rsp.charAt(i))) {
      unsigned int j = i;
      while (j < rsp.length() && isDigit(rsp.charAt(j))) j++;
      digits = rsp.substring(i, j);
      break;
    }
  }
  Serial.print("IMSI: ");
  Serial.println(digits);
  return digits;
}

bool tryQuectelApn(const String &apn) {
  Serial.print("Try Quectel APN: ");
  Serial.println(apn);
  // 设置 PDP，上报最后一个参数为 1 表示自动激活 DNS 等（兼容性更好）
  sendAT("AT+QICSGP=1,1,\"" + apn + "\",\"\",\"\",1", 2000);
  // 激活 PDP，需要较长时间，延长超时到 60s
  String r = sendAT("AT+QIACT=1", 60000); // 激活 PDP（等待更长时间）
  Serial.print("QIACT rsp: ");
  Serial.println(r);
  delay(500);
  // 检查是否有 IP 地址
  String p = sendAT("AT+QIACT?", 2000);
  Serial.print("QIACT? -> ");
  Serial.println(p);
  if (p.indexOf('.') >= 0) return true;
  // 兼容检查 CGPADDR
  String pa = sendAT("AT+CGPADDR?", 2000);
  Serial.print("CGPADDR -> ");
  Serial.println(pa);
  if (pa.indexOf('.') >= 0) return true;
  return false;
}

bool ensureDataConnectedQuectel() {
  Serial.println("Ensure data connection (Quectel) ...");
  String rsp = sendAT("AT+CGATT?", 2000);
  Serial.print("CGATT? -> ");
  Serial.println(rsp);
  if (rsp.indexOf("+CGATT: 1") < 0) {
    sendAT("AT+CGATT=1", 5000);
    delay(1000);
  }
  // 检查已有 IP
  rsp = sendAT("AT+CGPADDR?", 2000);
  Serial.print("CGPADDR -> ");
  Serial.println(rsp);
  if (rsp.indexOf('.') >= 0) return true;

  String imsi = getIMSI();
  // 默认中国移动 APN 列表优先
  const String apnCandidates[] = {"cmnet", "cmwap", "3gnet"};
  for (int i = 0; i < (int)(sizeof(apnCandidates)/sizeof(apnCandidates[0])); ++i) {
    const String &candidate = apnCandidates[i];
    // 在尝试前先主动解除可能的 PDP 激活，增加成功率
    sendAT("AT+QIDEACT=1", 2000);
    delay(500);
    // 每个 APN 尝试多次激活
    int maxAttempts = 2;
    for (int a = 0; a < maxAttempts; ++a) {
      Serial.print("Attempt ");
      Serial.print(a + 1);
      Serial.print("/");
      Serial.print(maxAttempts);
      Serial.print(" for APN ");
      Serial.println(candidate);
      if (tryQuectelApn(candidate)) return true;
      // 如果失败，等待并重试
      delay(2000);
    }
  }
  Serial.println("Quectel APN attempts failed");
  return false;
}

// 发送 HTTP POST 数据通过 AT+CIPSTART + AT+CIPSEND（TCP/SSL）
bool sendHttpPostViaTCP(const String &host, int port, const String &path, const String &json, unsigned long timeoutMs = 10000) {
  String cmd = "AT+CIPSTART=\"SSL\",\"";
  cmd += host;
  cmd += "\",";
  cmd += String(port);
  String rsp = sendAT(cmd, 5000);
  Serial.print("CIPSTART rsp: ");
  Serial.println(rsp);
  // 有些模块 返回 "CONNECT OK" 或 "OK"
  if (rsp.indexOf("ERROR") >= 0) {
    // 尝试不使用 SSL
    cmd = "AT+CIPSTART=\"TCP\",\"";
    cmd += host;
    cmd += "\",";
    cmd += String(port);
    rsp = sendAT(cmd, 5000);
    Serial.print("CIPSTART(plain) rsp: ");
    Serial.println(rsp);
    if (rsp.indexOf("ERROR") >= 0) {
      return false;
    }
  }

  // 构建 HTTP 请求
  String req = "POST ";
  req += path;
  req += " HTTP/1.1\r\n";
  req += "Host: ";
  req += host;
  req += "\r\nContent-Type: application/json\r\n";
  req += "Connection: close\r\n";
  req += "Content-Length: ";
  req += String(json.length());
  req += "\r\n\r\n";
  req += json;

  // 发送长度并等待 '>' 提示
  String sendCmd = "AT+CIPSEND=" + String(req.length());
  rsp = sendAT(sendCmd, 2000);
  Serial.print("CIPSEND rsp: ");
  Serial.println(rsp);
  // 若返回包含 ">" 则可以直接写入；否则等待提示
  bool hasPrompt = (rsp.indexOf(">") >= 0);
  if (!hasPrompt) {
    unsigned long start = millis();
    while (millis() - start < 2000) {
      while (SerialAT.available()) {
        String s = SerialAT.readStringUntil('\n');
        if (s.indexOf(">") >= 0) {
          hasPrompt = true;
          break;
        }
      }
      if (hasPrompt) break;
    }
  }
  if (!hasPrompt) {
    Serial.println("No prompt for CIPSEND");
    // 关闭连接
    sendAT("AT+CIPCLOSE", 1000);
    return false;
  }

  // 发送请求主体
  SerialAT.print(req);
  // 结束符（部分模块需要 Ctrl+Z）
  SerialAT.write((uint8_t)0x1A);

  // 读取响应
  unsigned long start = millis();
  String full = "";
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      String line = SerialAT.readStringUntil('\n');
      full += line + "\n";
    }
  }
  Serial.print("HTTP resp: ");
  Serial.println(full);
  sendAT("AT+CIPCLOSE", 1000);
  return (full.length() > 0);
}

// 使用 Quectel QHTTP 命令通过 HTTPS 上传（更适配 Quectel 模组）
bool sendHttpPostViaQuectel(const String &host, int port, const String &path, const String &json, unsigned long timeoutMs = 60000) {
  // 配置 SSL（上下文 id = 1）
  sendAT("AT+QSSLCFG=\"sslversion\",1,3", 2000); // TLS1.2
  sendAT("AT+QSSLCFG=\"seclevel\",1,2", 2000);

  // 构建完整 URL
  String url = "https://";
  url += host;
  url += path;

  // 发送 URL
  String cmd = "AT+QHTTPURL=" + String(url.length()) + ",60";
  String rsp = sendAT(cmd, 5000);
  Serial.print("QHTTPURL rsp: ");
  Serial.println(rsp);
  if (rsp.indexOf("CONNECT") >= 0 || rsp.indexOf(">") >= 0) {
    // 模块已提示输入 URL
    SerialAT.print(url);
    SerialAT.print("\r\n");
    delay(500);
  } else {
    // 尝试继续（某些模组直接接受 URL 无需 CONNECT）
    // 如果返回 ERROR 则放弃
    if (rsp.indexOf("ERROR") >= 0) return false;
  }

  // 发起 POST 请求，模块会在成功后返回 +QHTTPPOST: <err> , <http_status>
  String postCmd = "AT+QHTTPPOST=" + String(json.length()) + ",60";
  rsp = sendAT(postCmd, timeoutMs);
  Serial.print("QHTTPPOST rsp: ");
  Serial.println(rsp);
  if (rsp.indexOf("CONNECT") >= 0 || rsp.indexOf(">") >= 0) {
    // 发送主体
    SerialAT.print(json);
    SerialAT.print("\r\n");
    delay(500);
    // 一些模组不需要额外结束符；等待返回
    unsigned long start = millis();
    String full = "";
    while (millis() - start < timeoutMs) {
      while (SerialAT.available()) {
        String line = SerialAT.readStringUntil('\n');
        full += line + "\n";
      }
    }
    Serial.print("QHTTPPOST full: ");
    Serial.println(full);
    if (full.indexOf("+QHTTPPOST:") >= 0 && full.indexOf(",200") >= 0) return true;
    if (full.indexOf("OK") >= 0) return true;
    return false;
  } else {
    // 如果 QHTTPPOST 的初始返回直接包含结果（无 CONNECT）
    if (rsp.indexOf("+QHTTPPOST:") >= 0 && rsp.indexOf(",200") >= 0) return true;
    return (rsp.indexOf("OK") >= 0);
  }
}

// 将 ddmm.mmmm 字符串转换为十进制度（N/S/E/W 需要外部处理符号）
float parseDegMin(const String &str) {
  if (str.length() == 0) return 0.0;
  double v = str.toFloat();
  int deg = (int)(v / 100);
  double min = v - deg * 100;
  return (float)(deg + min / 60.0);
}

// 解析 +CGNSINF 风格的返回，尝试提取经纬度并返回是否有定位
bool parseGpsFromCGNSINF(const String &rsp, float &outLat, float &outLon) {
  int idx = rsp.indexOf("+CGNSINF:");
  if (idx < 0) return false;
  String tail = rsp.substring(idx + 8);
  tail.trim();
  // 按逗号拆分字段
  String parts[30];
  int p = 0;
  int start = 0;
  for (int i = 0; i <= tail.length() && p < 30; ++i) {
    if (i == tail.length() || tail.charAt(i) == ',') {
      parts[p++] = tail.substring(start, i);
      start = i + 1;
    }
  }
  if (p < 6) return false;
  // 通常字段: <GNSSrun status>,<Fix status>,<UTC>,<lat>,<lon>,<alt>,<speed>,...
  String utcStr = parts[2];
  String latStr = parts[3];
  String lonStr = parts[4];
  String altStr = parts[5];
  String speedStr = (p > 6) ? parts[6] : String("0");
  latStr.trim();
  lonStr.trim();
  if (latStr.length() == 0 || lonStr.length() == 0) return false;
  outLat = latStr.toFloat();
  outLon = lonStr.toFloat();
  // 更新全局高度和速度（若可用）
  gpsAltitude = altStr.toFloat();
  // CGNSINF speed 通常以 m/s 或 km/h 视模块而定，尽量解析为 m/s（若 > 100 假设为 km/h）
  float sp = speedStr.toFloat();
  if (sp > 100.0) {
    // 可能是以 km/h 表示，转为 m/s
    sp = sp / 3.6;
  }
  gpsSpeed = sp;
  if (outLat == 0.0 && outLon == 0.0) return false;
  return true;
}

// 尝试主动查询 GPS：启用 GNSS 并读取信息（兼容常见 AT 接口）
bool getGPSFix(float &outLat, float &outLon) {
  // 启用 GNSS（若模块已启用也无害）
  sendAT("AT+CGNSPWR=1", 500);
  delay(200);
  // 优先尝试 +CGNSINF
  String rsp = sendAT("AT+CGNSINF", 1000);
  if (rsp.indexOf("+CGNSINF:") >= 0) {
    if (parseGpsFromCGNSINF(rsp, outLat, outLon)) return true;
  }
  // 尝试 AT+CGPSINFO 格式
  rsp = sendAT("AT+CGPSINFO", 1000);
  int idx = rsp.indexOf("+CGPSINFO:");
  if (idx >= 0) {
    String tail = rsp.substring(idx + 9);
    tail.trim();
    // 常见: lat, N/S, lon, E/W, ...
    int comma1 = tail.indexOf(',');
    if (comma1 > 0) {
      String latPart = tail.substring(0, comma1);
      String rest = tail.substring(comma1 + 1);
      int comma2 = rest.indexOf(',');
      if (comma2 > 0) {
        String ns = rest.substring(0, comma2);
        rest = rest.substring(comma2 + 1);
        int comma3 = rest.indexOf(',');
        if (comma3 > 0) {
          String lonPart = rest.substring(0, comma3);
          String ew = rest.substring(comma3 + 1, comma3 + 2);
          float lat = parseDegMin(latPart);
          float lon = parseDegMin(lonPart);
          if (ns == "S") lat = -lat;
          if (ew == "W") lon = -lon;
          // CGPSINFO 有时包含速度/altitude later; not handled here
          outLat = lat;
          outLon = lon;
          if (outLat == 0.0 && outLon == 0.0) return false;
          return true;
        }
      }
    }
  }
  // 还可以尝试解析 NMEA 语句中的速度/高度，但此处省略，返回 false
  // 无定位
  return false;
}

// ========== LED 控制 ==========
void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  strip.setPixelColor(0, strip.Color(r, g, b));
  strip.show();
}

void turnOffLed() {
  setLedColor(0, 0, 0);
}

// 逻辑色映射：某些灯带颜色通道可能互换，使用逻辑色封装便于调整
void setLogicalGreen() {
  // 物理通道目前表现为 R<->G 互换，所以把参数交换以确保逻辑上的绿色显示
  setLedColor(255, 0, 0);
}

void setLogicalRed() {
  setLedColor(0, 255, 0);
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
        // 有信号时保持亮态（绿灯），并确保重置闪烁定时器
        blinkOn = true;
    }
      // 如果状态发生变化，重置闪烁定时器，立即应用新状态
      if (currentState != previousState) {
        lastBlinkToggle = now;
        previousState = currentState;
      }
  }

  // 定时检查 GPS 定位状态
  if (now - lastGPSCheck >= GPS_CHECK_INTERVAL) {
    lastGPSCheck = now;
    float lat = 0.0, lon = 0.0;
    if (getGPSFix(lat, lon)) {
      gpsHasFix = true;
      gpsLatitude = lat;
      gpsLongitude = lon;
      Serial.print("GPS fix: ");
      Serial.print(gpsLatitude, 6);
      Serial.print(", ");
      Serial.println(gpsLongitude, 6);
    } else {
      gpsHasFix = false;
      Serial.println("GPS: no fix");
    }
  }
  // 定时上传数据
  if (now - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = now;
    // 构建时间字段（尽量从 CGNSINF 的 UTC，若不可用使用占位）
    String datetime = formatIsoFromCGNSTime(String(""), 8); // 默认空
    // 构建 JSON：即使无 GPS fix 也上传，GPS 字段置为空字符串
    String json = "{";
    json += "\"id\":\"";
    json += GEO_SENSOR_ID;
    json += "\",\"key\":\"";
    json += GEO_SENSOR_KEY;
    json += "\",\"latitude\":";
    if (gpsHasFix) json += String(gpsLatitude, 6); else json += String(0.0, 6);
    json += ",\"longitude\":";
    if (gpsHasFix) json += String(gpsLongitude, 6); else json += String(0.0, 6);
    json += ",\"altitude\":";
    if (gpsHasFix) json += String(gpsAltitude, 2); else json += String(0.0, 2);
    json += ",\"speed\":";
    if (gpsHasFix) json += String(gpsSpeed, 2); else json += String(0.0, 2);
    json += ",\"datetime\":\"";
    json += datetime;
    json += "\"}";

    Serial.print("Uploading JSON: ");
    Serial.println(json);
    // 使用 Quectel 自动激活 APN（中国移动），然后上传
    if (!ensureDataConnectedQuectel()) {
      Serial.println("Data not connected after Quectel APN attempts, skip upload");
    } else {
      // 优先使用 Quectel 的 HTTPS 上传命令
      bool ok = sendHttpPostViaQuectel(String(UPLOAD_HOST), UPLOAD_PORT, String(UPLOAD_PATH), json, 60000);
      if (!ok) {
        Serial.println("Quectel HTTPS upload failed, fallback to TCP");
        ok = sendHttpPostViaTCP(String(UPLOAD_HOST), UPLOAD_PORT, String(UPLOAD_PATH), json, 10000);
      }
      Serial.print("Upload result: ");
      Serial.println(ok ? "OK" : "FAILED");
    }
  }

  // 根据状态控制灯
  if (currentState == STATE_SIM_HAS_SIGNAL) {
    // 有信号：绿色常亮（不断写入保证灯不被其他逻辑覆盖）
    setLogicalGreen();
  } else if (currentState == STATE_NO_SIM || currentState == STATE_SIM_NO_SIGNAL) {
    // 无SIM或有SIM但无信号：红色按配置闪烁
    if (now - lastBlinkToggle >= blinkIntervalMs) {
      lastBlinkToggle = now;
      blinkOn = !blinkOn;
      if (blinkOn) {
        setLogicalRed();
      } else {
        turnOffLed();
      }
    }
  } else {
    turnOffLed();
  }

  delay(10);
}


