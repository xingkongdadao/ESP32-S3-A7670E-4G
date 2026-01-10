/*
 * A7670E GPS Test - Official Pin Configuration
 *
 * 根据A7670E官方引脚定义：
 * UART_RXD GPIO 18  (A7670E UART RX → ESP32 TX)
 * UART_TXD GPIO 17  (A7670E UART TX → ESP32 RX)
 * RI GPIO 40        (Ring Indicator)
 * DTR GPIO 45       (Data Terminal Ready)
 * USB_DN GPIO 19    (USB Data-)
 * USB_DP GPIO 20    (USB Data+)
 *
 * 测试内容：
 * 1. GNSS卫星定位 (AT+CGNSSPWR, AT+CGNSINF)
 * 2. LBS基站定位 (AT+CLBS=1)
 * 3. 实时坐标解析和显示
 *
 * 注意：GPS需要室外环境和天线，LBS可在室内工作
 */

#include <Arduino.h>

// 根据A7670E官方引脚定义
static const int UART_RXD = 18;  // A7670E UART RX (ESP32 TX)
static const int UART_TXD = 17;  // A7670E UART TX (ESP32 RX)
static const int RI_PIN = 40;    // Ring Indicator
static const int DTR_PIN = 45;   // Data Terminal Ready

static const uint32_t UART_BAUD = 115200;

String response_buffer;
unsigned long last_gps_check = 0;
const unsigned long GPS_CHECK_INTERVAL = 5000; // 5秒检查一次

unsigned long last_lbs_check = 0;
const unsigned long LBS_CHECK_INTERVAL = 30000; // 30秒检查一次LBS

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 初始化硬件串口连接到A7670E
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RXD, UART_TXD);

  // 可选：设置RI和DTR引脚（如果需要）
  // pinMode(RI_PIN, INPUT);
  // pinMode(DTR_PIN, OUTPUT);
  // digitalWrite(DTR_PIN, HIGH);

  Serial.println("=== A7670E GPS Test - Official Pins ===");
  Serial.println("UART_RXD (ESP32→A7670E): GPIO 18");
  Serial.println("UART_TXD (A7670E→ESP32): GPIO 17");
  Serial.println("Baud Rate: 115200");
  Serial.println();

  // 测试基本AT通信
  Serial.println("1. Testing AT communication...");
  if (sendATCommand("AT", 2000)) {
    Serial.println("✅ AT communication OK");
  } else {
    Serial.println("❌ AT communication failed");
    Serial.println("   Check wiring and power");
    while(1); // 停止执行
  }

  // 获取模块信息
  Serial.println("2. Getting module information...");
  sendATCommand("ATI", 2000);
  sendATCommand("AT+SIMCOMATI", 2000);

  // 初始化GPS - 使用A7670E官方推荐的命令序列
  Serial.println("3. Initializing GPS...");

  // 开启GNSS电源
  Serial.println("   Enabling GNSS power...");
  if (sendATCommand("AT+CGNSSPWR=1", 3000)) {
    Serial.println("   ✅ GNSS power enabled");
  } else {
    Serial.println("   ❌ GNSS power failed");
  }

  // 等待GNSS芯片启动
  Serial.println("   Waiting for GNSS chip startup (10s)...");
  delay(10000);

  // 开启GNSS数据输出
  Serial.println("   Enabling GNSS data output...");
  if (sendATCommand("AT+CGNSSTST=1", 3000)) {
    Serial.println("   ✅ GNSS data output enabled");
  } else {
    Serial.println("   ❌ GNSS data output failed");
  }

  // 可选：设置GNSS端口切换
  Serial.println("   Setting GNSS port switch...");
  sendATCommand("AT+CGNSSPORTSWITCH=0,1", 2000);

  Serial.println("=== GPS Initialization Complete ===");
  Serial.println("Waiting for GPS satellite signals...");
  Serial.println("This may take 1-3 minutes for first fix");
  Serial.println();
  Serial.println("💡 Tips:");
  Serial.println("   - Ensure GPS antenna is connected and outdoors");
  Serial.println("   - If GPS fails, the module may support LBS base station location");
  Serial.println("   - Try AT+CLBS=1 for base station positioning");
  Serial.println();
}

// 发送AT命令并等待响应
bool sendATCommand(const char* command, unsigned long timeout) {
  // 清空之前的响应
  while (Serial1.available()) {
    Serial1.read();
  }

  // 发送命令
  Serial1.print(command);
  Serial1.print("\r\n");

  // 等待响应
  unsigned long start = millis();
  String response = "";

  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;

      // 检查是否收到完整响应
      if (response.indexOf("OK\r\n") != -1) {
        return true;
      }
      if (response.indexOf("ERROR\r\n") != -1) {
        return false;
      }
    }
    delay(10);
  }

  return false;
}

// 获取完整的AT响应
String getATResponse(unsigned long timeout) {
  unsigned long start = millis();
  String response = "";

  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;

      // 检查响应结束
      if (response.endsWith("\r\nOK\r\n") || response.endsWith("\r\nERROR\r\n")) {
        break;
      }
    }
    delay(10);
  }

  return response;
}

// 解析并显示GPS坐标数据
void parseAndDisplayGPSData(String gpsResponse) {
  // CGNSINF格式: +CGNSINF: <GNSS run status>,<Fix status>,<UTC date & Time>,<Latitude>,<Longitude>,<MSL Altitude>,<Speed Over Ground>,<Course Over Ground>,<Fix Mode>,<Reserved1>,<HDOP>,<PDOP>,<VDOP>,<Reserved2>,<GNSS Satellites in View>,<GNSS Satellites Used>,<GLONASS Satellites Used>,<Reserved3>,<C/N0 max>,<HPA>,<VPA>

  int startIdx = gpsResponse.indexOf("+CGNSINF: ");
  if (startIdx == -1) return;

  String data = gpsResponse.substring(startIdx + 10);
  data.trim();

  // 分割逗号分隔的数据
  int commaCount = 0;
  int lastComma = -1;
  String fields[25];

  for (int i = 0; i < data.length() && commaCount < 25; i++) {
    if (data[i] == ',') {
      fields[commaCount] = data.substring(lastComma + 1, i);
      fields[commaCount].trim();
      lastComma = i;
      commaCount++;
    }
  }

  if (commaCount >= 6) {
    Serial.println("📍 GPS定位信息:");

    // 纬度 (fields[3])
    if (fields[3].length() > 0) {
      Serial.print("   纬度: ");
      Serial.print(fields[3].toFloat(), 6);
      Serial.println(" °");
    }

    // 经度 (fields[4])
    if (fields[4].length() > 0) {
      Serial.print("   经度: ");
      Serial.print(fields[4].toFloat(), 6);
      Serial.println(" °");
    }

    // 海拔 (fields[5])
    if (fields[5].length() > 0) {
      Serial.print("   海拔: ");
      Serial.print(fields[5].toFloat(), 2);
      Serial.println(" m");
    }

    // 速度 (fields[6])
    if (fields[6].length() > 0) {
      Serial.print("   速度: ");
      Serial.print(fields[6].toFloat(), 2);
      Serial.println(" km/h");
    }

    // 卫星数量 (fields[14])
    if (commaCount >= 15 && fields[14].length() > 0) {
      Serial.print("   卫星: ");
      Serial.print(fields[14].toInt());
      Serial.println(" 颗");
    }

    // HDOP精度 (fields[10])
    if (commaCount >= 11 && fields[10].length() > 0) {
      Serial.print("   HDOP: ");
      Serial.println(fields[10].toFloat(), 1);
    }

    // 时间 (fields[2])
    if (fields[2].length() > 0) {
      Serial.print("   UTC时间: ");
      Serial.println(fields[2]);
    }
  }
}

// 解析并显示LBS基站定位数据
void parseAndDisplayLBSData(String lbsResponse) {
  // CLBS格式: +CLBS: <longitude>,<latitude>,<accuracy>

  int startIdx = lbsResponse.indexOf("+CLBS: ");
  if (startIdx == -1) return;

  String data = lbsResponse.substring(startIdx + 7);
  data.trim();

  // 分割逗号分隔的数据
  int firstComma = data.indexOf(",");
  int secondComma = data.indexOf(",", firstComma + 1);

  if (firstComma != -1 && secondComma != -1) {
    String lonStr = data.substring(0, firstComma);
    String latStr = data.substring(firstComma + 1, secondComma);
    String accStr = data.substring(secondComma + 1);

    Serial.println("📍 LBS基站定位信息:");

    // 经度
    if (lonStr.length() > 0) {
      Serial.print("   经度: ");
      Serial.print(lonStr.toFloat(), 6);
      Serial.println(" °");
    }

    // 纬度
    if (latStr.length() > 0) {
      Serial.print("   纬度: ");
      Serial.print(latStr.toFloat(), 6);
      Serial.println(" °");
    }

    // 精度
    if (accStr.length() > 0) {
      Serial.print("   定位精度: ~");
      Serial.print(accStr.toInt());
      Serial.println(" 米");
    }

    Serial.println("✅ LBS基站定位成功！");
  } else {
    Serial.println("⚠️ LBS响应格式不正确");
  }
}

void loop() {
  // 显示从A7670E接收到的所有数据
  if (Serial1.available()) {
    char c = Serial1.read();
    Serial.print(c);

    // 检查GPS数据
    if (c == '$') { // NMEA语句开始
      Serial.println(" 📡 NMEA GPS Data Detected!");
    }
  }

  // 定期检查GPS状态
  if (millis() - last_gps_check > GPS_CHECK_INTERVAL) {
    last_gps_check = millis();

    Serial.println("🔍 Checking GPS status...");

    // 发送GPS信息查询命令
    Serial1.println("AT+CGNSINF");

    // 等待并显示响应
    delay(2000);
    String gpsResponse = "";
    while (Serial1.available()) {
      char c = Serial1.read();
      gpsResponse += c;
    }

    if (gpsResponse.length() > 0) {
      Serial.println("GPS Response:");
      Serial.println(gpsResponse);

      // 分析GPS响应
      if (gpsResponse.indexOf("+CGNSINF:") != -1) {
        Serial.println("✅ GPS module responding");

        // 检查定位状态
        if (gpsResponse.indexOf("+CGNSINF: 1,1,") != -1) {
          Serial.println("🎯 GPS定位成功！已获取卫星信号");

          // 解析并显示坐标
          parseAndDisplayGPSData(gpsResponse);

        } else if (gpsResponse.indexOf("+CGNSINF: 1,0,") != -1) {
          Serial.println("📡 GPS正在搜索卫星...");
        } else if (gpsResponse.indexOf("+CGNSINF: 0,") != -1) {
          Serial.println("❌ GPS未启动");
        }

      } else if (gpsResponse.indexOf("ERROR") != -1) {
        Serial.println("❌ GPS command error - check module configuration");
      }
    } else {
      Serial.println("❌ No GPS response received");
    }

    Serial.println("--- GPS Check Complete ---");
    Serial.println();
  }

  // 定期测试LBS基站定位（每30秒一次，作为GPS的备选方案）
  if (millis() - last_lbs_check > LBS_CHECK_INTERVAL) {
    last_lbs_check = millis();

    Serial.println("📶 Testing LBS Base Station Location...");

    // 发送LBS基站定位命令
    Serial1.println("AT+CLBS=1");

    // 等待并显示响应
    delay(5000); // LBS可能需要更长时间
    String lbsResponse = "";
    while (Serial1.available()) {
      char c = Serial1.read();
      lbsResponse += c;
    }

    if (lbsResponse.length() > 0) {
      Serial.println("LBS Response:");
      Serial.println(lbsResponse);

      // 分析LBS响应
      if (lbsResponse.indexOf("+CLBS:") != -1) {
        Serial.println("✅ LBS base station location responding");

        // 解析LBS坐标信息
        parseAndDisplayLBSData(lbsResponse);

      } else if (lbsResponse.indexOf("ERROR") != -1) {
        Serial.println("❌ LBS command error - module may not support LBS");
      }
    } else {
      Serial.println("❌ No LBS response received");
    }

    Serial.println("--- LBS Check Complete ---");
    Serial.println();
  }
}
