/*
 * A7670E GPS Final Test - 确定GPS功能
 *
 * 专门测试A7670E模块的GPS卫星定位功能
 * 使用多种GPS命令和长时间测试
 */

#define RX_PIN 17
#define TX_PIN 18
#define BAUD_RATE 115200

String response_buffer;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("==========================================");
  Serial.println("A7670E GPS FINAL TEST - 确定GPS功能");
  Serial.println("==========================================");
  Serial.println();

  // 初始化串口
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);

  // 测试1: 基本AT通信
  Serial.println("测试1: 基本AT通信");
  if (sendATCommand("AT")) {
    Serial.println("✅ AT通信正常");
  } else {
    Serial.println("❌ AT通信失败 - 检查硬件连接");
    while(1);
  }

  // 测试2: 模块信息
  Serial.println("\n测试2: 模块信息");
  sendATCommand("ATI");
  sendATCommand("AT+CGMM");
  sendATCommand("AT+CGMR");

  // 测试3: GPS功能测试
  Serial.println("\n测试3: GPS功能测试");
  testGPSFunctionality();

  // 测试4: 长时间GPS监控
  Serial.println("\n测试4: 长时间GPS监控 (60秒)");
  monitorGPSLongTerm();

  Serial.println("\n==========================================");
  Serial.println("GPS测试完成");
  Serial.println("==========================================");
}

void testGPSFunctionality() {
  // 方法1: 标准GPS测试
  Serial.println("方法1: 标准GNSS命令测试");
  sendATCommand("AT+CGNSSPWR=1");
  delay(2000);
  sendATCommand("AT+CGNSSPWR?");
  sendATCommand("AT+CGNSSTST=1");
  delay(1000);
  sendATCommand("AT+CGNSINF");

  // 方法2: 替代GPS命令
  Serial.println("\n方法2: 替代GPS命令测试");
  sendATCommand("AT+CGPSPWR=1");
  sendATCommand("AT+CGPSSTATUS");
  sendATCommand("AT+CGPSINFO");

  // 方法3: 检查GPS固件
  Serial.println("\n方法3: GPS固件和配置检查");
  sendATCommand("AT+CGNSSMOD=?");
  sendATCommand("AT+CGPSSTATUS=?");
  sendATCommand("AT+CGNSINF=?");

  // 方法4: 强制GPS模式
  Serial.println("\n方法4: 强制GPS模式设置");
  sendATCommand("AT+CGNSSMOD=1,1,0,0"); // GPS + GLONASS
  delay(1000);
  sendATCommand("AT+CGNSSURC=1"); // 开启NMEA输出
  sendATCommand("AT+CGNSSINF=1"); // 设置信息更新
}

void monitorGPSLongTerm() {
  Serial.println("开始60秒GPS监控，每10秒检查一次...");
  Serial.println("请确保设备在室外，有GPS天线连接");

  for (int i = 0; i < 6; i++) {
    Serial.print("监控 ");
    Serial.print((i + 1) * 10);
    Serial.println("秒...");

    // 发送GPS查询
    sendATCommand("AT+CGNSINF");
    sendATCommand("AT+CGPSINFO");

    // 检查是否有GPS数据
    delay(2000);
    String gpsData = readSerialData();

    // 分析GPS数据
    if (gpsData.indexOf("$GPGGA") != -1 || gpsData.indexOf("$GPRMC") != -1) {
      Serial.println("🎉 检测到NMEA GPS数据流！");
    }

    if (gpsData.indexOf("+CGNSINF:") != -1) {
      Serial.println("📡 检测到CGNSINF GPS响应！");
      if (gpsData.indexOf("+CGNSINF: 1,1,") != -1) {
        Serial.println("🎯 GPS定位成功！卫星信号已获取");
      }
    }

    if (gpsData.indexOf("+CGPSINFO:") != -1) {
      Serial.println("📡 检测到CGPSINFO GPS响应！");
    }

    delay(8000); // 等待到10秒
  }

  Serial.println("GPS监控完成");
}

bool sendATCommand(const char* command) {
  // 清空缓冲区
  while (Serial1.available()) Serial1.read();

  Serial.print("发送: ");
  Serial.println(command);

  Serial1.println(command);
  delay(1000);

  String response = readSerialData();
  Serial.print("响应: ");
  Serial.println(response);

  return response.length() > 0;
}

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

void loop() {
  // 显示任何来自模块的数据
  if (Serial1.available()) {
    char c = Serial1.read();
    Serial.print(c);

    // 检查GPS数据特征
    static String buffer = "";
    buffer += c;

    if (buffer.indexOf("\r\n") != -1) {
      if (buffer.indexOf("+CGNSINF:") != -1 ||
          buffer.indexOf("+CGPSINFO:") != -1 ||
          buffer.indexOf("$GPGGA") != -1 ||
          buffer.indexOf("$GPRMC") != -1) {
        Serial.println(" ← GPS数据检测!");
      }
      buffer = "";
    }
  }

  // 每30秒自动查询GPS
  static unsigned long lastQuery = 0;
  if (millis() - lastQuery > 30000) {
    lastQuery = millis();
    Serial.println("\n--- 自动GPS查询 ---");
    sendATCommand("AT+CGNSINF");
    sendATCommand("AT+CGPSINFO");
  }
}
