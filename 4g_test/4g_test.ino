/*
 * ESP32-S3 A7670E 4G Network Test
 *
 * 专门测试4G网络连接和数据上传功能
 * 从main.ino中提取4G上传相关代码
 */

#include <Arduino.h>
#include <time.h>

// 是否启用串口打印（调试用），设置为 1 可显示所有网络连接和调试信息
#define SERIAL_VERBOSE 1

static const int RXPin = 17, TXPin = 18;
static const uint32_t GPSBaud = 115200;

bool simPresent = false;
bool networkRegistered = false;
bool pdpActive = false;  // 全局PDP状态变量

// 后台 API 配置
static const char GEO_SENSOR_API_BASE_URL[] = "https://manage.gogotrans.com/api/microcontrollerInstanceDevice/";
static const char GEO_SENSOR_KEY[] = "mcu_5e3abda8585e4bc79af89ad57af8b3b9";


unsigned long lastUpload = 0;
const unsigned long UPLOAD_INTERVAL = 10000; // 10秒

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
      String rev = Serial1.readString();
      if (rev.indexOf("OK") != -1) {
        if (SERIAL_VERBOSE) Serial.println("Got OK!");
        return true;
      }
    }
  }
  return false;
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

// 测试4G数据上传功能
void test4GUpload() {
  Serial.println("\n=== 开始4G网络数据上传测试 ===");

  // 创建测试GPS数据（模拟定位数据）
  double latitude = 39.904200;      // 北京的纬度
  double longitude = 116.407396;    // 北京的经度
  double altitude = 44.0;           // 海拔
  double speed = 0.0;               // 速度
  int satelliteCount = 8;           // 卫星数量
  double locationAccuracy = 5.0;    // 定位精度
  double altitudeAccuracy = 10.0;   // 海拔精度

  // 获取当前时间戳
  String dataAcquiredAt = "";
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
    // 如果时间获取失败，使用null
    dataAcquiredAt = "null";
  }

  // 构建与main.ino相同的JSON格式
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
    Serial.println("正在通过 4G 网络上传测试数据 (POST)...");
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

  // 执行4G上传
  bool success = cellularHttpRequest("POST", fullUrl, json);

  if (SERIAL_VERBOSE) {
    Serial.print("4G upload result: ");
    Serial.println(success ? "OK" : "FAILED");
  }

  if (success) {
    Serial.println("✅ 4G网络上传测试成功!");
  } else {
    Serial.println("❌ 4G网络上传测试失败!");
  }

  Serial.println("=== 4G网络测试完成 ===\n");
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);

  Serial.println("=== ESP32-S3 A7670E 4G Network Test ===");
  Serial.println("4G网络连接和数据上传测试程序");
  Serial.println();

  // 配置NTP时间同步
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

  // 测试基本AT通信
  Serial.println("1. 测试AT通信...");
  while (!SentMessage("AT", 2000)) {
    Serial.println("等待模块响应...");
    delay(1000);
  }
  Serial.println("✅ AT通信正常");

  // 获取模块信息
  Serial.println("\n2. 获取模块信息...");
  SentSerial("ATI");
  SentSerial("AT+SIMCOMATI");
  delay(2000);

  // 配置APN和激活PDP
  Serial.println("\n3. 配置APN和PDP...");
  configureAPNAndActivatePDP();

  // 等待网络稳定
  Serial.println("\n4. 等待网络稳定...");
  delay(5000);

  // 测试4G数据上传
  Serial.println("5. 测试4G数据上传...");
  test4GUpload();

  Serial.println("=== 初始化完成 ===");
  Serial.println("4G网络测试程序运行中...");
  Serial.println("每30秒自动测试一次数据上传");
}

void loop() {
  // 处理串口数据
  if (Serial1.available()) {
    String rev = Serial1.readString();
    if (SERIAL_VERBOSE) {
      Serial.print("收到模块响应: ");
      Serial.println(rev);
    }
    parseModuleResponse(rev);
  }

  // 定期检查PDP状态
  checkPDPStatus();

  // 每30秒测试一次4G数据上传
  static unsigned long lastTest = 0;
  if (millis() - lastTest > 30000) { // 30秒
    lastTest = millis();
    if (pdpActive) {
      test4GUpload();
    } else {
      Serial.println("⚠️ PDP未激活，跳过数据上传测试");
    }
  }
}
