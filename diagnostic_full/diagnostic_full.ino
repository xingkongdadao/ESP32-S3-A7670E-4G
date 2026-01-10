/*
 * ESP32-A7670E Full Diagnostic Program
 *
 * Comprehensive testing for hardware connection, power, and communication
 */

#define TEST_RX_PIN 17   // ESP32 RX (from A7670E TX)
#define TEST_TX_PIN 18   // ESP32 TX (to A7670E RX)

#define PWRKEY_PIN 46   // A7670E power key pin (if connected)
#define RESET_PIN 40    // A7670E reset pin (if connected)

// Test different baud rates
const long baudRates[] = {9600, 19200, 38400, 57600, 115200};
const int numBaudRates = 5;

HardwareSerial *testSerial = &Serial1;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("=========================================");
  Serial.println("ESP32-A7670E FULL DIAGNOSTIC PROGRAM");
  Serial.println("=========================================");
  Serial.println();

  // Step 1: Hardware pin check
  Serial.println("STEP 1: HARDWARE PIN VERIFICATION");
  Serial.println("----------------------------------");

  // Check if pins are valid for ESP32-S3
  Serial.print("RX Pin (GPIO ");
  Serial.print(TEST_RX_PIN);
  Serial.println("): Checking validity...");
  if (TEST_RX_PIN >= 0 && TEST_RX_PIN <= 48) {
    Serial.println("  ✅ Pin number valid");
  } else {
    Serial.println("  ❌ Invalid pin number");
  }

  Serial.print("TX Pin (GPIO ");
  Serial.print(TEST_TX_PIN);
  Serial.println("): Checking validity...");
  if (TEST_TX_PIN >= 0 && TEST_TX_PIN <= 48) {
    Serial.println("  ✅ Pin number valid");
  } else {
    Serial.println("  ❌ Invalid pin number");
  }
  Serial.println();

  // Step 2: Power supply check
  Serial.println("STEP 2: POWER SUPPLY CHECK");
  Serial.println("---------------------------");

  // Check ESP32's own power
  float esp32Voltage = analogReadMilliVolts(1) / 1000.0; // ADC reading
  Serial.print("ESP32 supply voltage: ");
  Serial.print(esp32Voltage, 2);
  Serial.println("V");

  if (esp32Voltage > 3.0 && esp32Voltage < 3.6) {
    Serial.println("  ✅ ESP32 power OK");
  } else {
    Serial.println("  ⚠️  ESP32 voltage unusual (should be ~3.3V)");
  }

  // Check if power control pins are available
  Serial.println("Power control pins check:");
  if (PWRKEY_PIN != -1) {
    pinMode(PWRKEY_PIN, OUTPUT);
    digitalWrite(PWRKEY_PIN, HIGH);
    Serial.print("  PWRKEY pin (GPIO ");
    Serial.print(PWRKEY_PIN);
    Serial.println(") set HIGH");
  } else {
    Serial.println("  PWRKEY pin not configured");
  }

  Serial.println();

  // Step 3: Serial port configuration test
  Serial.println("STEP 3: SERIAL PORT CONFIGURATION");
  Serial.println("----------------------------------");

  Serial.println("Testing different baud rates...");

  for (int i = 0; i < numBaudRates; i++) {
    long baud = baudRates[i];
    Serial.print("Testing baud rate: ");
    Serial.println(baud);

    // Reinitialize serial with new baud rate
    testSerial->end();
    delay(100);
    testSerial->begin(baud, SERIAL_8N1, TEST_RX_PIN, TEST_TX_PIN);
    delay(100);

    // Send AT command
    testSerial->println("AT");
    delay(500);

    // Check response
    if (testSerial->available()) {
      String response = testSerial->readString();
      Serial.print("  Response: ");
      Serial.println(response);

      if (response.indexOf("OK") != -1) {
        Serial.println("  🎉 SUCCESS! AT communication working at this baud rate!");
        Serial.println("  Continuing with full tests...");
        runFullTests(baud);
        return;
      } else if (response.indexOf("AT") != -1) {
        Serial.println("  📡 Echo detected - module might be responding");
      } else {
        Serial.println("  📝 Response received but not OK");
      }
    } else {
      Serial.println("  ❌ No response at this baud rate");
    }

    Serial.println();
  }

  // If no baud rate worked
  Serial.println("❌ NO BAUD RATE WORKED!");
  Serial.println("FINAL DIAGNOSTIC REPORT:");
  Serial.println("=========================");
  printDiagnosticReport();
}

void runFullTests(long workingBaud) {
  Serial.println("\nSTEP 4: COMPREHENSIVE MODULE TESTS");
  Serial.println("===================================");

  // Test 1: Basic AT commands
  Serial.println("Test 1: Basic AT commands");
  testATCommand("AT", "Basic AT command");
  testATCommand("AT+CSQ", "Signal quality check");
  testATCommand("AT+CPIN?", "SIM card status");
  testATCommand("AT+COPS?", "Network operator");

  // Test 2: GPS capability check
  Serial.println("\nTest 2: GPS capability check");
  testATCommand("AT+CGNSSPWR=1", "Enable GNSS power");
  delay(2000);
  testATCommand("AT+CGNSINF", "GPS information query");

  // Test 3: LBS capability check
  Serial.println("\nTest 3: LBS capability check");
  testATCommand("AT+CLBS=1", "LBS location request");

  // Test 4: Module information
  Serial.println("\nTest 4: Module information");
  testATCommand("ATI", "Module identification");
  testATCommand("AT+CGMM", "Model identification");
  testATCommand("AT+CGMR", "Firmware version");

  Serial.println("\n🎯 DIAGNOSTIC COMPLETE!");
  Serial.println("=======================");
  Serial.println("If all tests passed: Module is fully functional!");
  Serial.println("If GPS tests failed: Module may not support GPS hardware");
  Serial.println("If LBS tests failed: Network location services unavailable");
}

void testATCommand(const char* command, const char* description) {
  Serial.print("  ");
  Serial.print(description);
  Serial.print(": ");

  // Clear any pending data
  while (testSerial->available()) {
    testSerial->read();
  }

  // Send command
  testSerial->println(command);
  delay(1000);

  // Read response
  if (testSerial->available()) {
    String response = testSerial->readString();
    if (response.indexOf("OK") != -1) {
      Serial.println("✅ PASS");
    } else if (response.indexOf("ERROR") != -1) {
      Serial.println("❌ FAIL (Command error)");
    } else {
      Serial.println("⚠️  PARTIAL (Response received)");
      Serial.println("    Response: " + response.substring(0, 50) + "...");
    }
  } else {
    Serial.println("❌ FAIL (No response)");
  }
}

void printDiagnosticReport() {
  Serial.println();
  Serial.println("TROUBLESHOOTING CHECKLIST:");
  Serial.println("===========================");
  Serial.println("□ HARDWARE CONNECTIONS:");
  Serial.println("  - ESP32 GPIO 17 ↔ A7670E UART_TXD");
  Serial.println("  - ESP32 GPIO 18 ↔ A7670E UART_RXD");
  Serial.println("  - Common GND connection");
  Serial.println("  - Check for loose connections or damaged wires");
  Serial.println();
  Serial.println("□ POWER SUPPLY:");
  Serial.println("  - A7670E needs 3.3-4.2V stable power");
  Serial.println("  - Check voltage with multimeter");
  Serial.println("  - Ensure sufficient current capacity (up to 2A)");
  Serial.println("  - Try different power source");
  Serial.println();
  Serial.println("□ MODULE STATE:");
  Serial.println("  - Check if PWR LED is lit on A7670E");
  Serial.println("  - Try pressing RST button on A7670E");
  Serial.println("  - Check PWRKEY timing (if connected)");
  Serial.println("  - Verify SIM card is inserted (if required)");
  Serial.println();
  Serial.println("□ SERIAL CONFIGURATION:");
  Serial.println("  - Try different baud rates (9600, 115200)");
  Serial.println("  - Check if RX/TX pins are swapped");
  Serial.println("  - Verify no other devices are using same pins");
  Serial.println();
  Serial.println("□ ESP32 CONFIGURATION:");
  Serial.println("  - Correct board selected in Arduino IDE");
  Serial.println("  - Correct COM port selected");
  Serial.println("  - Try different ESP32-S3 board if available");
  Serial.println();
  Serial.println("Run this diagnostic again after checking each item!");
}

void loop() {
  // Echo any incoming data from A7670E
  if (testSerial->available()) {
    char c = testSerial->read();
    Serial.print(c);
  }

  // Allow manual AT command input from Serial Monitor
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      Serial.print("Sending: ");
      Serial.println(cmd);
      testSerial->println(cmd);
    }
  }
}
