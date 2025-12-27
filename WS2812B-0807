
#include <Adafruit_NeoPixel.h>
// 彩灯引脚和数量定义
#define LED_STRIP_PIN    38    // WS2812B彩灯数据引脚（GPIO38）
#define LED_COUNT        1     // WS2812B-0807通常只有1个灯珠
// 彩灯对象
Adafruit_NeoPixel strip(LED_COUNT, LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);
// 颜色数组（12种颜色）
const uint32_t colors[] = {
    strip.Color(255, 0, 0),     // 红色
    strip.Color(255, 127, 0),   // 橙色
    strip.Color(255, 255, 0),   // 黄色
    strip.Color(0, 255, 0),     // 绿色
    strip.Color(0, 255, 255),   // 青色
    strip.Color(0, 0, 255),     // 蓝色
    strip.Color(127, 0, 255),   // 紫色
    strip.Color(255, 0, 255),   // 品红
    strip.Color(255, 192, 203), // 粉色
    strip.Color(255, 255, 255), // 白色
    strip.Color(255, 215, 0),   // 金色
    strip.Color(0, 255, 127)    // 春绿色
};
const int colorCount = 12;
int currentColorIndex = 0;
unsigned long lastChangeTime = 0;
const unsigned long CHANGE_INTERVAL = 1000; // 颜色切换间隔（毫秒）
// 颜色名称数组
const char* colorNames[] = {
    "红色", "橙色", "黄色", "绿色", "青色", "蓝色",
    "紫色", "品红", "粉色", "白色", "金色", "春绿色"
};
void setup() {
    // 初始化串口
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n====================================");
    Serial.println("  WS2812B-0807 彩灯控制程序");
    Serial.println("  引脚: GPIO38");
    Serial.println("  每秒自动切换颜色");
    Serial.println("====================================\n");
    
    // 初始化彩灯
    strip.begin();
    strip.setBrightness(100);  // 设置亮度（0-255）
    strip.show();  // 初始化为熄灭状态
    
    Serial.println("彩灯初始化完成！");
    Serial.println("开始循环显示颜色...\n");
    
    lastChangeTime = millis();
}
void loop() {
    unsigned long currentTime = millis();
    
    // 每秒切换颜色
    if (currentTime - lastChangeTime >= CHANGE_INTERVAL) {
        lastChangeTime = currentTime;
        
        // 切换到下一个颜色
        currentColorIndex = (currentColorIndex + 1) % colorCount;
        
        // 设置颜色并显示
        strip.setPixelColor(0, colors[currentColorIndex]);
        strip.show();
        
        // 在串口显示当前颜色
        Serial.print("当前颜色: ");
        Serial.print(colorNames[currentColorIndex]);
        Serial.print(" (RGB: ");
        Serial.print((colors[currentColorIndex] >> 16) & 0xFF);
        Serial.print(", ");
        Serial.print((colors[currentColorIndex] >> 8) & 0xFF);
        Serial.print(", ");
        Serial.print(colors[currentColorIndex] & 0xFF);
        Serial.print(")");
        Serial.print(" 亮度: ");
        Serial.print(strip.getBrightness());
        Serial.print("/255");
        
        // 显示运行时间
        unsigned long seconds = currentTime / 1000;
        unsigned long minutes = seconds / 60;
        seconds = seconds % 60;
        Serial.print("  运行时间: ");
        Serial.print(minutes);
        Serial.print("分 ");
        Serial.print(seconds);
        Serial.println("秒");
    }
    
    delay(10);  // 小延迟，避免循环太快
}
// 彩虹效果函数（可选）
void rainbowEffect() {
    for (int i = 0; i < 256; i++) {
        strip.setPixelColor(0, Wheel(i & 255));
        strip.show();
        delay(20);
    }
}
// 彩虹色轮函数
uint32_t Wheel(byte WheelPos) {
    WheelPos = 255 - WheelPos;
    if (WheelPos < 85) {
        return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
    }
    if (WheelPos < 170) {
        WheelPos -= 85;
        return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
    }
    WheelPos -= 170;
    return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

