/*
 * ESP32 Serial Connection Test for A7670E
 *
 * This program tests if ESP32 can communicate with A7670E module
 * using the official pin configuration.
 */

#define RX_PIN 17  // ESP32 RX (connected to A7670E TX)
#define TX_PIN 18  // ESP32 TX (connected to A7670E RX)
#define BAUD_RATE 115200

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=== ESP32-A7670E Serial Connection Test ===");
  Serial.println("Testing hardware serial communication");
  Serial.println();

  // Initialize hardware serial
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println("1. Testing basic serial loopback...");
  Serial.println("   Sending test data...");

  // Send test data
  Serial1.println("HELLO_A7670E");
  delay(500);

  // Check for response
  if (Serial1.available()) {
    String response = Serial1.readString();
    Serial.println("   ✅ Received response: " + response);
    Serial.println("   → Hardware connection appears to be working!");
  } else {
    Serial.println("   ❌ No response received");
    Serial.println("   → Check your wiring connections:");
    Serial.println("     ESP32 GPIO 17 ↔ A7670E UART_TXD");
    Serial.println("     ESP32 GPIO 18 ↔ A7670E UART_RXD");
    Serial.println("     Common GND connection");
  }

  Serial.println();
  Serial.println("2. Testing AT command communication...");

  // Test AT command
  Serial1.println("AT");
  delay(1000);

  if (Serial1.available()) {
    String atResponse = Serial1.readString();
    if (atResponse.indexOf("OK") != -1) {
      Serial.println("   ✅ AT command successful: " + atResponse);
      Serial.println("   → A7670E module is responding!");
    } else {
      Serial.println("   ⚠️  AT response received but not OK: " + atResponse);
      Serial.println("   → Module might be in different mode or needs reset");
    }
  } else {
    Serial.println("   ❌ No AT response");
    Serial.println("   → Possible issues:");
    Serial.println("     - A7670E not powered on");
    Serial.println("     - Wrong baud rate (try 9600)");
    Serial.println("     - Module is in sleep mode");
    Serial.println("     - Hardware connection problem");
  }

  Serial.println();
  Serial.println("=== Test Complete ===");
  Serial.println("If both tests failed, check:");
  Serial.println("1. Power supply to A7670E (3.3-4.2V)");
  Serial.println("2. Wiring connections (TX↔RX, RX↔TX)");
  Serial.println("3. Baud rate setting");
  Serial.println("4. SIM card insertion (if required)");
}

void loop() {
  // Echo any data received from A7670E to serial monitor
  if (Serial1.available()) {
    char c = Serial1.read();
    Serial.print(c);
  }

  // Allow sending commands from serial monitor to A7670E
  if (Serial.available()) {
    char c = Serial.read();
    Serial1.print(c);
  }
}
