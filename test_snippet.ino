  if (WiFi.status() == WL_CONNECTED) {
      // 使用GPS数据，如果没有GPS信号则设置为空值
      double latitude, longitude;
      if (currentGPS.hasFix) {
        latitude = currentGPS.latitude;
        longitude = currentGPS.longitude;
      } else {
        // 设置为空值
        latitude = 0.0;
        longitude = 0.0;
        if (SERIAL_VERBOSE) {
          Serial.println("GPS未定位，设置坐标为空值");
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
      json += "\"networkSource\":\"WiFi";
      if (currentGPS.satelliteCount > 0) {
        json += "-GPS定位";
      } else if (currentGPS.locationAccuracy < 1000) {
        json += "-LBS定位";
      } else {
        json += "-模拟定位";
      }
      json += "\"";
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
      // 上传成功时闪烁提示并更新上传时间戳
      if (ok) {
        flashSuccess();
        lastUpload = now; // 更新上传时间戳，避免重复上传
      }
    }
  } else {
    // WiFi 不可用，尝试使用 4G 网络上传
