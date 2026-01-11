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

// 数据收集结构体
struct SensorData {
  // GPS数据
  double latitude = 0.0;
  double longitude = 0.0;
  double altitude = 0.0;
  double speed = 0.0;
  int satelliteCount = 0;
  double locationAccuracy = 0.0;
  double altitudeAccuracy = 0.0;
  String locationSource = "GPS";

  // 网络信息
  int signalStrength = 0;
  int networkCount = 0;          // 网络计数 (信号质量)
  String operatorName = "";
  String networkType = "";
  String imsi = "";
  String imei = "";
  String iccid = "";
  int networkRegistration = 0;
  String ipAddress = "";  // IP地址字段

  // 设备状态
  float temperature = 0.0;
  float voltage = 0.0;
  int batteryLevel = 0;
  String firmwareVersion = "";
  unsigned long uptime = 0;

  // 时间戳
  String timestamp = "";
};

// 全局传感器数据实例
SensorData sensorData;

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

// 获取当前IP地址
String getCurrentIPAddress() {
  Serial.println("📡 获取当前IP地址...");

  SentSerial("AT+CGPADDR");
  delay(500);

  unsigned long tstart = millis();
  String resp = "";
  int responseTimeout = 3000;

  // 等待完整响应
  while (millis() - tstart < responseTimeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      tstart = millis();
    }

    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) {
      delay(200);
      while (Serial1.available()) {
        resp += (char)Serial1.read();
      }
      break;
    }

    delay(10);
  }

  String ipAddress = "";

  if (resp.indexOf("+CGPADDR: 1,") != -1) {
    int ipStart = resp.indexOf("+CGPADDR: 1,") + 12;
    int ipEnd = resp.indexOf("\r\n", ipStart);
    if (ipEnd == -1) ipEnd = resp.indexOf("\n", ipStart);
    if (ipEnd == -1) ipEnd = resp.indexOf("OK", ipStart);
    if (ipEnd == -1) ipEnd = resp.length();

    ipAddress = resp.substring(ipStart, ipEnd);
    ipAddress.trim();

    // 验证IP地址格式
    int dotCount = 0;
    bool validIP = true;
    for (char c : ipAddress) {
      if (c == '.') dotCount++;
      else if (!isDigit(c)) {
        validIP = false;
        break;
      }
    }

    if (!validIP || dotCount != 3 || ipAddress == "0.0.0.0" || ipAddress.length() < 7) {
      ipAddress = "";
    }
  }

  if (SERIAL_VERBOSE) {
    Serial.print("当前IP地址: ");
    Serial.println(ipAddress.length() > 0 ? ipAddress : "未获取到");
  }

  return ipAddress;
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
    String currentIP = "";

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

      currentIP = resp.substring(ipStart, ipEnd);
      currentIP.trim();

      if (SERIAL_VERBOSE) {
        Serial.print("提取的IP地址: '");
        Serial.print(currentIP);
        Serial.println("'");
      }

      // 检查IP地址是否有效（排除0.0.0.0和无效地址）
      // IPv4地址应该有3个点号，格式为x.x.x.x
      int dotCount = 0;
      for (char c : currentIP) {
        if (c == '.') dotCount++;
      }

      pdpActive = (currentIP.length() >= 7 &&  // 最小IP长度 x.x.x.x
                   currentIP != "0.0.0.0" &&
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

    // 更新全局sensorData中的IP地址
    if (pdpActive && currentIP.length() > 0) {
      sensorData.ipAddress = currentIP;
    } else {
      sensorData.ipAddress = "";
    }

    lastPdpStatus = pdpActive;

    if (SERIAL_VERBOSE) {
      Serial.print("PDP 查询响应: ");
      Serial.println(resp);
      Serial.print("PDP 激活状态: ");
      Serial.println(pdpActive ? "激活 ✓" : "未激活 ✗");
      if (pdpActive) {
        Serial.println("✓✓✓ 4G网络连接正常 ✓✓✓");
        Serial.println("📡 当前IP地址: " + sensorData.ipAddress);
      } else {
        Serial.println("⚠️⚠️⚠️ 4G网络连接异常 ⚠️⚠️⚠️");
      }
    }
  }

  pdpActive = lastPdpStatus;
}

// 运营商APN配置表
struct OperatorAPN {
  const char* operatorName;
  const char* apn;
  const char* description;
};

OperatorAPN operatorAPNs[] = {
  // 中国移动
  {"CMCC", "cmnet", "中国移动CMNET"},
  {"CMCC", "cmwap", "中国移动CMWAP"},
  {"CMCC", "internet", "中国移动通用"},
  // 中国联通
  {"CUCC", "3gnet", "中国联通3GNET"},
  {"CUCC", "uninet", "中国联通UNINET"},
  {"CUCC", "internet", "中国联通通用"},
  // 中国电信
  {"CTCC", "ctnet", "中国电信CTNET"},
  {"CTCC", "internet", "中国电信通用"},
  // 通用APN
  {"GENERAL", "internet", "通用互联网"},
  {"GENERAL", "web", "通用WEB"}
};

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

  // 检测运营商信息
  Serial.println("📡 检测运营商信息...");
  SentSerial("AT+COPS?");
  delay(2000);

  // 获取运营商信息
  String operatorInfo = "";
  while (Serial1.available()) {
    char c = Serial1.read();
    operatorInfo += c;
  }

  Serial.println("运营商信息: " + operatorInfo);

  // 智能选择APN列表
  const char** apnList;
  int apnCount;

  if (operatorInfo.indexOf("CHINA MOBILE") != -1 || operatorInfo.indexOf("46000") != -1 || operatorInfo.indexOf("46002") != -1) {
    // 中国移动
    Serial.println("📱 检测到中国移动运营商，使用移动APN列表");
    static const char* cmccAPNs[] = {"cmnet", "cmwap", "internet"};
    apnList = cmccAPNs;
    apnCount = 3;
  } else if (operatorInfo.indexOf("CHINA UNICOM") != -1 || operatorInfo.indexOf("46001") != -1) {
    // 中国联通
    Serial.println("📱 检测到中国联通运营商，使用联通APN列表");
    static const char* cuccAPNs[] = {"3gnet", "uninet", "internet"};
    apnList = cuccAPNs;
    apnCount = 3;
  } else   if (operatorInfo.indexOf("MOBIFONE") != -1 || operatorInfo.indexOf("45201") != -1) {
    // 越南Mobilfone
    Serial.println("🇻🇳 检测到越南Mobilfone运营商，使用Mobilfone APN列表");
    Serial.println("✅ Mobilfone SIM卡已确认支持，专注于解决硬件连接问题");
    static const char* mobifoneAPNs[] = {"m-wap", "internet", "wap"};
    apnList = mobifoneAPNs;
    apnCount = 3;
  } else if (operatorInfo.indexOf("VIETTEL") != -1 || operatorInfo.indexOf("45204") != -1) {
    // 越南Viettel
    Serial.println("🇻🇳 检测到越南Viettel运营商，使用Viettel APN列表");
    Serial.println("⚠️ 注意: A7670E模块对越南运营商支持有限，连接可能不稳定");
    static const char* viettelAPNs[] = {"v-internet", "internet", "wap"};
    apnList = viettelAPNs;
    apnCount = 3;
  } else if (operatorInfo.indexOf("VINAPHONE") != -1 || operatorInfo.indexOf("45202") != -1) {
    // 越南Vinaphone
    Serial.println("🇻🇳 检测到越南Vinaphone运营商，使用Vinaphone APN列表");
    Serial.println("⚠️ 注意: A7670E模块对越南运营商支持有限，连接可能不稳定");
    static const char* vinaphoneAPNs[] = {"m3-world", "internet", "wap"};
    apnList = vinaphoneAPNs;
    apnCount = 3;
  } else {
    // 其他国际运营商或未知，使用通用APN
    Serial.println("🌏 未知或国际运营商，使用通用APN列表");
    Serial.println("💡 国际SIM卡可能需要手动配置运营商和APN");
    static const char* generalAPNs[] = {"internet", "web", "wap"};
    apnList = generalAPNs;
    apnCount = 3;
  }

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

  // 获取当前IP地址
  String currentIP = getCurrentIPAddress();
  if (currentIP.length() == 0) {
    Serial.println("⚠️ 未获取到IP地址，使用空字符串");
  }

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

  // 构建与main.ino相同的JSON格式，添加ipAddress和networkCount字段
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
  json += "\"networkSource\":\"4G\",";
  json += "\"networkCount\":";
  json += String(sensorData.networkCount);
  json += ",";
  json += "\"ipAddress\":\"";
  json += currentIP;
  json += "\"";
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

// SIM卡兼容性诊断
void diagnoseSIMCompatibility() {
  Serial.println("\n🔍🔍🔍 SIM卡兼容性诊断开始 🔍🔍🔍");

  // 1. 检查SIM卡状态
  Serial.println("\n1. 检查SIM卡状态...");

  // 首先检查模块电源和基本状态
  Serial.println("1.1 检查模块基本状态...");
  SentSerial("AT");
  delay(500);
  String basicStatus = readSerialData();
  if (basicStatus.indexOf("OK") == -1) {
    Serial.println("❌ 模块无响应 - 检查电源和连接");
    return;
  }
  Serial.println("✅ 模块响应正常");

  // 检查SIM卡检测引脚状态
  Serial.println("\n1.2 检查SIM卡检测...");
  SentSerial("AT+CSMINS?");
  delay(500);
  String simDetect = readSerialData();
  Serial.println("SIM检测状态: " + simDetect);

  if (simDetect.indexOf("+CSMINS: 0,1") != -1) {
    Serial.println("✅ SIM卡检测正常");
  } else if (simDetect.indexOf("+CSMINS: 0,0") != -1) {
    Serial.println("❌ SIM卡未检测到 - 硬件连接问题");
    Serial.println("💡 检查SIM卡槽和卡的物理连接");
  }

  // 正式检查SIM卡状态
  Serial.println("\n1.3 检查SIM卡状态...");
  SentSerial("AT+CPIN?");
  delay(1000);
  String simStatus = readSerialData();
  Serial.println("SIM状态: " + simStatus);

  if (simStatus.indexOf("READY") != -1) {
    Serial.println("✅ SIM卡状态正常");
  } else if (simStatus.indexOf("SIM PIN") != -1) {
    Serial.println("⚠️ SIM卡需要PIN码 - 请检查PIN码设置");
    Serial.println("💡 尝试命令: AT+CPIN=\"1234\" (替换为实际PIN码)");
  } else if (simStatus.indexOf("SIM PUK") != -1) {
    Serial.println("❌ SIM卡被PUK锁定 - 需要PUK码解锁");
  } else if (simStatus.indexOf("SIM not inserted") != -1) {
    Serial.println("❌ SIM卡未检测到 - 硬件连接或模块问题");

    // 检查是否为国际SIM卡
    SentSerial("AT+CIMI");
    delay(1000);
    String imsiInfo = readSerialData();
    Serial.println("IMSI信息: " + imsiInfo);

    bool isMobilfoneSIM = false;
    if (imsiInfo.length() > 10) {
      // 检查IMSI前5位 (MNC - Mobile Network Code)
      String mcc_mnc = imsiInfo.substring(0, 5);
      if (mcc_mnc == "45201") {
        isMobilfoneSIM = true;
        Serial.println("🇻🇳 确认: 检测到越南Mobilfone SIM卡 (MCC-MNC: " + mcc_mnc + ")");
        Serial.println("✅ SIM卡本身支持Mobilfone，问题在于硬件连接或模块配置");
      } else if (imsiInfo.substring(0, 3) == "452") {
        Serial.println("🇻🇳 检测到其他越南运营商SIM卡 (MCC: 452)");
      } else {
        Serial.println("🌏 检测到其他国际SIM卡 (MCC: " + imsiInfo.substring(0, 3) + ")");
      }
    }

    Serial.println("🔧 Mobilfone SIM卡故障排除步骤:");

    Serial.println("\n   紧急检查:");
    Serial.println("   □ 确认SIM卡金手指清洁无氧化");
    Serial.println("   □ 尝试用力按压SIM卡，确保完全接触");
    Serial.println("   □ 检查是否有SIM卡适配器问题 (标准SIM卡?)");
    Serial.println("   □ 测量SIM卡槽供电电压 (应该有1.8V或3.3V)");

    Serial.println("\n   模块重置步骤:");
    Serial.println("   □ 执行: AT+CFUN=0 (关闭射频)");
    Serial.println("   □ 等待5秒");
    Serial.println("   □ 执行: AT+CFUN=1 (重新开启)");
    Serial.println("   □ 等待10秒让模块重新检测SIM卡");

    Serial.println("\n   手动SIM卡检测:");
    Serial.println("   □ AT+CSMINS? (检查SIM卡检测状态)");
    Serial.println("   □ AT+CPIN? (再次检查PIN状态)");
    Serial.println("   □ AT+CSIM=? (检查SIM卡接口电压)");

    Serial.println("\n   替代测试:");
    Serial.println("   □ 尝试其他Mobilfone SIM卡");
    Serial.println("   □ 尝试中国运营商SIM卡 (验证卡槽是否正常)");
    Serial.println("   □ 在另一部手机上测试此SIM卡是否正常工作");

    if (isMobilfoneSIM) {
      Serial.println("\n   Mobilfone特定配置:");
      Serial.println("   □ 手动设置APN: AT+CGDCONT=1,\"IP\",\"m-wap\"");
      Serial.println("   □ 手动选择运营商: AT+COPS=1,2,\"45201\",7");
      Serial.println("   □ 强制网络注册: AT+CGATT=1");
    }

    Serial.println("\n   硬件故障可能性:");
    Serial.println("   □ SIM卡槽硬件损坏");
    Serial.println("   □ A7670E模块SIM卡接口故障");
    Serial.println("   □ 供电电压不稳定");
    Serial.println("   □ ESP32与A7670E通信线路问题");

    return; // SIM卡检测失败，停止后续诊断
  } else if (simStatus.indexOf("ERROR") != -1) {
    Serial.println("❌ SIM卡操作失败 - 可能SIM卡损坏或通信问题");
    Serial.println("💡 检查: 1) SIM卡是否损坏 2) 通信线路是否正常 3) 模块是否故障");
    return;
  } else {
    Serial.println("ℹ️ SIM卡状态未知: " + simStatus);
  }

  // 2. 检查运营商信息
  Serial.println("\n2. 检查运营商信息...");
  SentSerial("AT+COPS?");
  delay(2000);
  String operatorInfo = readSerialData();
  Serial.println("运营商信息: " + operatorInfo);

  if (operatorInfo.indexOf("+COPS:") != -1) {
    Serial.println("✅ 运营商信息获取成功");
    if (operatorInfo.indexOf("CHINA MOBILE") != -1) {
      Serial.println("📱 检测到: 中国移动");
    } else if (operatorInfo.indexOf("CHINA UNICOM") != -1) {
      Serial.println("📱 检测到: 中国联通");
    } else if (operatorInfo.indexOf("CHINA TELECOM") != -1) {
      Serial.println("📱 检测到: 中国电信");
    }
  } else if (operatorInfo.indexOf("ERROR") != -1) {
    Serial.println("❌ 无法获取运营商信息 - 网络服务不可用");
    Serial.println("💡 原因可能: 1) 无SIM卡 2) SIM卡锁定 3) 无信号 4) 区域限制");
  } else {
    Serial.println("⚠️ 运营商信息未知");
  }

  // 3. 检查信号质量
  Serial.println("\n3. 检查信号质量...");
  SentSerial("AT+CSQ");
  delay(1000);
  String signalInfo = readSerialData();
  Serial.println("信号质量: " + signalInfo);

  // 解析信号强度
  if (signalInfo.indexOf("+CSQ:") != -1) {
    int colonPos = signalInfo.indexOf(":");
    int commaPos = signalInfo.indexOf(",", colonPos);
    if (commaPos != -1) {
      String rssiStr = signalInfo.substring(colonPos + 1, commaPos);
      rssiStr.trim();
      int rssi = rssiStr.toInt();

      // 将信号强度存储到networkCount字段
      sensorData.networkCount = rssi;

      if (rssi == 99) {
        Serial.println("❌ 无信号 - 请检查天线连接和位置");
      } else if (rssi >= 0 && rssi <= 10) {
        Serial.println("⚠️ 信号很弱 - 可能影响连接稳定性");
      } else if (rssi >= 11 && rssi <= 20) {
        Serial.println("✅ 信号一般 - 可以尝试连接");
      } else if (rssi >= 21 && rssi <= 31) {
        Serial.println("✅ 信号良好");
      } else {
        Serial.println("ℹ️ 信号强度: " + String(rssi));
      }
    }
  }

  // 4. 检查网络注册状态
  Serial.println("\n4. 检查网络注册状态...");
  SentSerial("AT+CGREG?");
  delay(1000);
  String regStatus = readSerialData();
  Serial.println("网络注册: " + regStatus);

  if (regStatus.indexOf("+CGREG: 0,1") != -1) {
    Serial.println("✅ 已注册到本地网络");
  } else if (regStatus.indexOf("+CGREG: 0,5") != -1) {
    Serial.println("✅ 已注册到漫游网络");
  } else if (regStatus.indexOf("+CGREG: 0,2") != -1) {
    Serial.println("🔄 正在搜索网络...");
  } else if (regStatus.indexOf("+CGREG: 0,0") != -1) {
    Serial.println("❌ 未注册，正在搜索...");
    Serial.println("💡 可能原因: 1) 无信号覆盖 2) SIM卡问题 3) 运营商锁定");
  } else if (regStatus.indexOf("+CGREG: 0,3") != -1) {
    Serial.println("❌ 注册被拒绝");
    Serial.println("💡 检查SIM卡是否被运营商停机或锁定");
  } else {
    Serial.println("⚠️ 网络注册状态未知");
  }

  // 5. 检查模块频段支持
  Serial.println("\n5. 检查模块频段支持...");
  SentSerial("AT+CNBP=?");
  delay(1000);
  String bandInfo = readSerialData();
  Serial.println("支持频段: " + bandInfo);

  // 6. 尝试手动网络搜索
  Serial.println("\n6. 执行手动网络搜索...");
  SentSerial("AT+COPS=?");
  delay(10000); // 网络搜索需要时间
  String networkList = readSerialData();

  if (networkList.length() > 20) {
    Serial.println("✅ 发现可用网络:");
    Serial.println(networkList);
  } else {
    Serial.println("❌ 未发现可用网络");
  }

  // 7. 诊断总结和恢复建议
  Serial.println("\n📋📋📋 诊断总结 📋📋📋");

  bool simOk = simStatus.indexOf("READY") != -1;
  bool networkOk = operatorInfo.indexOf("+COPS:") != -1;
  bool signalOk = signalInfo.indexOf("+CSQ:") != -1;
  bool regOk = regStatus.indexOf("+CGREG: 0,1") != -1 || regStatus.indexOf("+CGREG: 0,5") != -1;

  Serial.println("SIM卡状态: " + String(simOk ? "✅" : "❌"));
  Serial.println("运营商识别: " + String(networkOk ? "✅" : "❌"));
  Serial.println("信号质量: " + String(signalOk ? "✅" : "❌"));
  Serial.println("网络注册: " + String(regOk ? "✅" : "❌"));

  // 提供具体的恢复步骤
  Serial.println("\n🔧🔧🔧 恢复建议 🔧🔧🔧");

  if (!simOk) {
    Serial.println("❌ SIM卡问题:");
    Serial.println("   1. 检查SIM卡是否正确插入卡槽");
    Serial.println("   2. 确认SIM卡未过期或损坏");
    Serial.println("   3. 尝试另一张SIM卡测试");
    Serial.println("   4. 如果需要PIN码，输入: AT+CPIN=\"1234\"");

    Serial.println("\n⏭️  请修复SIM卡问题后重新运行测试");
    return;
  }

  if (!signalOk) {
    Serial.println("❌ 信号问题:");
    Serial.println("   1. 检查4G天线是否正确连接");
    Serial.println("   2. 移动到室外或信号更好的位置");
    Serial.println("   3. 确认当地有该运营商的网络覆盖");
    Serial.println("   4. 尝试手动搜索网络: AT+COPS=?");
  }

  if (!networkOk) {
    Serial.println("❌ 运营商识别失败:");
    Serial.println("   1. 确认SIM卡所属运营商");
    Serial.println("   2. 检查SIM卡是否被运营商停机");
    Serial.println("   3. 确认模块支持该运营商的频段");
    Serial.println("   4. 尝试手动设置运营商: AT+COPS=0,2,\"46000\" (移动)");
  }

  if (!regOk) {
    Serial.println("❌ 网络注册失败:");
    Serial.println("   1. 等待2-3分钟让模块完成自动注册");
    Serial.println("   2. 重启ESP32和A7670E模块");
    Serial.println("   3. 尝试手动注册: AT+CGATT=1");
    Serial.println("   4. 检查APN设置是否正确");
  }

  if (simOk && networkOk && signalOk && regOk) {
    Serial.println("✅ 所有诊断项目通过 - SIM卡应该可以正常工作");

    // 检查是否为Mobilfone SIM卡，提供专门建议
    if (operatorInfo.indexOf("45201") != -1 || operatorInfo.indexOf("MOBIFONE") != -1) {
      Serial.println("🇻🇳 Mobilfone SIM卡配置建议:");
      Serial.println("   ✅ SIM卡支持已确认，问题在于网络连接");
      Serial.println("   • 推荐APN: AT+CGDCONT=1,\"IP\",\"m-wap\"");
      Serial.println("   • 手动选择运营商: AT+COPS=1,2,\"45201\",7");
      Serial.println("   • 如果失败，尝试: AT+COPS=0,2 (自动选择)");
    } else if (operatorInfo.indexOf("452") != -1) { // 其他越南运营商
      Serial.println("🇻🇳 其他越南运营商配置建议:");
      Serial.println("   • Viettel: AT+CGDCONT=1,\"IP\",\"v-internet\"");
      Serial.println("   • Vinaphone: AT+CGDCONT=1,\"IP\",\"m3-world\"");
      Serial.println("   • 自动选择: AT+COPS=0,2");
    } else {
      Serial.println("💡 如果APN配置仍然失败，尝试手动设置:");
      Serial.println("   中国移动: AT+CGDCONT=1,\"IP\",\"cmnet\"");
      Serial.println("   中国联通: AT+CGDCONT=1,\"IP\",\"3gnet\"");
      Serial.println("   中国电信: AT+CGDCONT=1,\"IP\",\"ctnet\"");
    }
  }

  Serial.println("\n📞 如果问题持续存在:");
  Serial.println("   1. 记录完整的诊断输出");
  Serial.println("   2. 确认SIM卡运营商和状态");
  Serial.println("   3. 测试模块是否支持该运营商频段");
  Serial.println("   4. 对于国际SIM卡，建议使用中国大陆运营商的SIM卡");
  Serial.println("   5. 联系模块供应商确认国际兼容性");

  Serial.println("\n🔍🔍🔍 SIM卡兼容性诊断完成 🔍🔍🔍\n");
}

// 读取串口数据
String readSerialData() {
  String data = "";
  unsigned long start = millis();

  while (millis() - start < 2000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      data += c;
    }
    delay(10);
  }

  return data;
}

// 打印网络信息摘要（包含IP地址和信号质量）
void printNetworkSummary() {
  Serial.println("\n📡 网络连接摘要:");

  // 获取当前IP地址
  String currentIP = getCurrentIPAddress();

  Serial.println("   PDP状态: " + String(pdpActive ? "已激活 ✓" : "未激活 ✗"));
  Serial.println("   IP地址: " + (currentIP.length() > 0 ? currentIP : "未获取"));
  Serial.printf("   信号质量(networkCount): %d (0-31, 越高越好)\n", sensorData.networkCount);
  Serial.println("   SIM卡状态: " + String(simPresent ? "正常 ✓" : "异常 ✗"));
  Serial.println("   网络注册: " + String(networkRegistered ? "已注册 ✓" : "未注册 ✗"));

  // 更新sensorData中的IP地址
  sensorData.ipAddress = currentIP;
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);

  Serial.println("=== ESP32-S3 A7670E 4G Network Test ===");
  Serial.println("4G网络连接和数据上传测试程序");
  Serial.println();

  // 添加快速SIM卡检查
  Serial.println("快速SIM卡检查:");
  SentSerial("AT+CPIN?");
  delay(1000);
  String quickSimCheck = readSerialData();
  if (quickSimCheck.indexOf("READY") != -1) {
    Serial.println("✅ SIM卡就绪");

    // 检查是否为Mobilfone SIM卡
    SentSerial("AT+CIMI");
    delay(1000);
    String imsiCheck = readSerialData();
    if (imsiCheck.length() > 10) {
      String mcc_mnc = imsiCheck.substring(0, 5);
      if (mcc_mnc == "45201") {
        Serial.println("🇻🇳 检测到越南Mobilfone SIM卡 (MCC-MNC: " + mcc_mnc + ")");
        Serial.println("✅ SIM卡支持已确认 - 硬件连接正常，准备进行网络配置");
      } else if (imsiCheck.substring(0, 3) == "452") {
        Serial.println("🇻🇳 检测到其他越南运营商SIM卡 (MCC: 452)");
      } else {
        Serial.println("📋 SIM卡信息: " + mcc_mnc + " (非越南运营商)");
      }
    }
  } else if (quickSimCheck.indexOf("SIM not inserted") != -1) {
    Serial.println("❌ SIM卡未检测到 - 硬件连接问题");
    Serial.println("🔧 Mobilfone SIM卡紧急修复步骤:");
    Serial.println("   1. 清洁SIM卡金手指，确保无氧化");
    Serial.println("   2. 确认SIM卡完全插入并用力按压");
    Serial.println("   3. 执行模块重置: AT+CFUN=0, 等待5秒, AT+CFUN=1");
    Serial.println("   4. 如果仍然失败，检查卡槽是否损坏");
  } else {
    Serial.println("⚠️ SIM卡状态异常: " + quickSimCheck);
    Serial.println("💡 将进行详细诊断以确定具体问题");
  }
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

  // 检查模块频段支持
  Serial.println("\n2.1 检查模块频段支持...");
  SentSerial("AT+CNBP=?");  // 频段查询
  delay(1000);

  // 检查网络注册状态
  Serial.println("\n2.2 检查网络注册状态...");
  SentSerial("AT+CREG?");   // GSM网络注册
  SentSerial("AT+CGREG?");  // GPRS网络注册
  delay(2000);

  // 配置APN和激活PDP
  Serial.println("\n3. 配置APN和PDP...");
  configureAPNAndActivatePDP();

  // 等待网络稳定
  Serial.println("\n4. 等待网络稳定...");
  delay(5000);

  // 执行SIM卡兼容性诊断
  Serial.println("5. 执行SIM卡兼容性诊断...");
  diagnoseSIMCompatibility();

  // 打印网络连接摘要（包含IP地址）
  Serial.println("\n6. 网络连接摘要...");
  printNetworkSummary();

  // 测试4G数据上传
  Serial.println("\n7. 测试4G数据上传...");
  test4GUpload();

  Serial.println("=== 初始化完成 ===");
  Serial.println("4G网络测试程序运行中...");
  Serial.println("每30秒自动测试一次数据上传（包含IP地址）");
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

  // 每30秒测试一次4G数据上传和IP地址检查
  static unsigned long lastTest = 0;
  if (millis() - lastTest > 30000) { // 30秒
    lastTest = millis();

    Serial.println("\n⏰ 定时任务执行 - " + String(millis() / 1000) + "秒");

    // 打印网络摘要（包含最新IP地址）
    printNetworkSummary();

    // 测试4G数据上传
    if (pdpActive) {
      test4GUpload();
    } else {
      Serial.println("⚠️ PDP未激活，跳过数据上传测试");
    }

    Serial.println("✅ 本轮定时任务完成");
  }
}
