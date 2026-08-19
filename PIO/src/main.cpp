#include <M5Cardputer.h>
#include <lvgl.h>
#include <ui.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "WiFiManagerHandler.h"
#include "GeminiChat.h"
#include <WiFiManager.h>
#include <algorithm>
#include <SD.h>
#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

LV_FONT_DECLARE(rubik_14_cy);

#define LED_PIN 21
#define NUM_LEDS 1

CRGB leds[NUM_LEDS];

GeminiChat geminiChat;

UIScreen currentScreen = UIScreen::LOADING;
UIScreen nextScreen = UIScreen::NONE;

// Global flags
volatile bool wifiConnectedFlag = false;
volatile bool configPortalStartedFlag = false;
volatile bool apiKeySavedFlag = false;

bool wifiConfigMessagePrinted = false;
char getShiftedChar(uint8_t keyCode);
bool isRussianLayout = false;
volatile bool isAIThinking = false;

enum LedEffectId : uint8_t {
  LED_EFFECT_OFF = 0,
  LED_EFFECT_SOLID,
  LED_EFFECT_BLINK,
  LED_EFFECT_BREATHE,
  LED_EFFECT_RAINBOW
};

struct LedState {
  uint8_t effect;
  uint32_t color;     // 0xRRGGBB
  uint8_t brightness; // 0-255
};

static LedState ledState = {
  LED_EFFECT_OFF,
  0,
  0
};

static SemaphoreHandle_t ledMutex = NULL;
static TaskHandle_t ledTaskHandle = NULL;

String currentLedEffect = "solid";
CRGB currentLedColor = CRGB(0, 0, 0);
uint8_t currentLedBrightness = 80;

unsigned long wifiConnectStartTime = 0;
const unsigned long wifiConnectTimeout = 10000;
unsigned long screenStartTime = 0;
bool statusScreenChecked = false;
bool webServerStarted = false;
int batteryPercentage = 0;

TaskHandle_t lvglTaskHandle = NULL;

unsigned long lastStatusCheckTime = 0;
const unsigned long statusCheckInterval = 5000;

bool wifiConfigured = false;
bool wifiConnected = false;
bool apiKeyConfigured = false;

// Function declarations
void lv_tick_task(void *arg);
void lvgl_task(void *arg);
void switchScreen(UIScreen screen);
void updateStatus(const String &status);
void handleEnterKey();
void resetCredentialsAndRestart();
void handleScreenTransitions();
void handleGeminiChatKeyboard();
void handleMenuNavigation();
void handleStatusScreenLogic();
void updateBattery();

// === ВСТАВИТЬ СЮДА ===
uint32_t crgbToUint32(const CRGB &c);
CRGB uint32ToCrgb(uint32_t v);
LedState getLedState();
void setLedState(uint8_t effect, uint32_t color, uint8_t brightness);
uint8_t parseLedEffectId(String effect);
void applyLedCommand(String params);
CRGB parseLedColor(String s);
void renderLed();
void ledTask(void *pvParameters);

// Новые функции для скрытых команд ИИ
int indexOfIgnoreCase(const String &haystack, const String &needle, int from = 0);
int getBatteryPercentage();
void setScreenBrightness(int value);
CRGB parseLedColor(String s);
void applyLedCommand(String params);
String parseDeviceCommands(String text);

void wifiSetupTask(void *pvParameters) {
  WiFiManager wm;
  wm.setConfigPortalBlocking(false);

  if (!wm.autoConnect(configPortalSSID)) {
    Serial.println("Failed to connect to WiFi and starting config portal.");
  }

  while (!wifiConnectedFlag) {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnectedFlag = true;
      Serial.println("WiFi Connected!");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  vTaskDelete(NULL);
}

// LVGL setup for display and tick handling
void lvgl_setup() {
  lv_init();

  static lv_disp_draw_buf_t draw_buf;
  static lv_color_t buf1[240 * 5];
  static lv_color_t buf2[240 * 5];
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, 240 * 5);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.flush_cb = [](lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    M5.Display.startWrite();
    M5.Display.setAddrWindow(area->x1, area->y1, w, h);
    M5.Display.pushPixels((uint16_t *)&color_p->full, w * h, true);
    M5.Display.endWrite();

    lv_disp_flush_ready(disp_drv);
  };
  disp_drv.hor_res = 240;
  disp_drv.ver_res = 135;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
}

// Update status display and log to Serial
void updateStatus(const String &status) {
  lv_textarea_set_text(ui_TextArea_debug_status, (status + "\n").c_str());
  Serial.println(status);
}

// Setup function
void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Keyboard.begin();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  lvgl_setup();

  Serial.println("Creating LVGL tasks...");
  xTaskCreatePinnedToCore(lv_tick_task, "lv_tick_task", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 8192, NULL, 1, &lvglTaskHandle, 1);

  ui_init();
  Serial.println("UI Initialized.");

  lv_obj_set_style_text_font(ui_TextArea_AI_response, &rubik_14_cy, 0);
  lv_obj_set_style_text_font(ui_TextArea_chat_input, &rubik_14_cy, 0);

  ledMutex = xSemaphoreCreateMutex();

  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds, NUM_LEDS);

  // Отключаем дитеринг, потому что он часто выглядит как быстрое мерцание.
  FastLED.setDither(0);

  // Ограничиваем глобальную яркость.
  // Если красные вспышки исчезли, можно позже поднять до 100-150.
  // Но для стабильности лучше начинать с 60-80.
  FastLED.setBrightness(70);

  // Ограничение тока для встроенного маленького LED.
  // Если компилятор ругается, закомментируй эту строку,
  // но тогда оставь FastLED.setBrightness(50-70).
  FastLED.setMaxPowerInVoltsAndMilliamps(3, 100);

  leds[0] = CRGB(0, 0, 0);
  FastLED.show();

  // Отдельная задача для LED.
  // Она работает даже тогда, когда loop() заблокирован запросом к Gemini.
  xTaskCreatePinnedToCore(
    ledTask,
    "ledTask",
    4096,
    NULL,
    2,
    &ledTaskHandle,
   0
  );

  delay(500);

  switchScreen(UIScreen::LOADING);

  xTaskCreatePinnedToCore(wifiSetupTask, "wifiSetupTask", 8192, NULL, 1, NULL, 0);

  String storedApiKey = getStoredApiKey();
  if (!storedApiKey.isEmpty()) {
    geminiChat.setApiKey(storedApiKey);
  }

  geminiChat.initialize();
}

void lv_tick_task(void *arg) {
  while (1) {
    lv_tick_inc(10);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void lvgl_task(void *arg) {
  while (1) {
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Handle screen transitions and manage nextScreen logic
void handleScreenTransitions() {
  if (nextScreen != UIScreen::NONE) {
    switchScreen(nextScreen);
    nextScreen = UIScreen::NONE;
  }

  switch (currentScreen) {
    case UIScreen::LOADING:
      if (millis() - screenStartTime > 5000) {
        switchScreen(UIScreen::STATUS);
      }
      break;

    case UIScreen::STATUS:
      handleStatusScreenLogic();
      break;

    default:
      break;
  }
}

void switchScreen(UIScreen screen) {
  if (currentScreen == screen) return;

  currentScreen = screen;
  screenStartTime = millis();
  String ipAddress;

  switch (screen) {
    case UIScreen::LOADING:
      lv_scr_load(ui_Screen_loading);
      break;

    case UIScreen::STATUS:
      lv_scr_load(ui_Screen_status);
      break;

    case UIScreen::GEMINI_CHAT:
      lv_scr_load(ui_Screen_AiChat);
      break;

    case UIScreen::MENU:
      lv_scr_load(ui_Screen_Menu);
      ipAddress = getIPAddress();
      lv_label_set_text(ui_Label_ip, ipAddress.c_str());
      updateBattery();
      lv_bar_set_value(ui_Bar_Battery, batteryPercentage, LV_ANIM_ON);
      lv_obj_clear_state(ui_Button_reset, LV_STATE_FOCUSED);
      lv_obj_clear_state(ui_Button_back, LV_STATE_FOCUSED);
      break;

    default:
      Serial.println("Invalid screen transition requested.");
      break;
  }
}

void handleStatusScreenLogic() {
  WiFiManager wm;

  if (WiFi.status() != WL_CONNECTED && !wifiConnectedFlag) {
    if (!wifiConfigMessagePrinted) {
      updateStatus("Connecting...");
      delay(2000);
      updateStatus("Connect to\n\"" + String(configPortalSSID) + "\"\nto configure Wi-Fi.");
      wifiConfigMessagePrinted = true;
      wifiConnectStartTime = millis();
    }
    return;
  }
  else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    updateStatus("Wi-Fi connected.");
    wifiConnected = true;
    wifiConfigMessagePrinted = false;
    delay(2000);
  }

  String apiKey = getStoredApiKey();
  String ipAddress = getIPAddress();

  if (apiKey.isEmpty() && !apiKeyConfigured) {
    updateStatus("Set API key at\n\"" + ipAddress + "\"\nRestart if fails.");
    if (!webServerStarted) {
      startWebServer();
      webServerStarted = true;
    }
    delay(2000);
    return;
  } else if (!apiKey.isEmpty()) {
    apiKeyConfigured = true;
    geminiChat.setApiKey(apiKey);
  }

  if (wifiConnected && apiKeyConfigured) {
    updateStatus("All set. Switching to Gemini screen.");
    delay(3000);
    switchScreen(UIScreen::GEMINI_CHAT);
  }
}

// Helper function to return the correct character when Shift is pressed
char getShiftedChar(uint8_t keyCode) {
  switch (keyCode) {
    case '=': return '+';
    case '9': return '(';
    case '8': return '*';
    default: return keyCode;
  }
}

String getMappedChar(char c, bool shift, bool isRu) {
  if (!isRu) {
    char res = shift ? getShiftedChar(c) : c;
    return String(res);
  }

  char lower_c = tolower(c);
  const char* qwerty = "qwertyuiop[]asdfghjkl;'zxcvbnm,.";
  const char* cyrillic[] = {"й","ц","у","к","е","н","г","ш","щ","з","х","ъ","ф","ы","в","а","п","р","о","л","д","ж","э","я","ч","с","м","и","т","ь","б","ю"};
  const char* cyrillic_shift[] = {"Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","Ъ","Ф","Ы","В","А","П","Р","О","Л","Д","Ж","Э","Я","Ч","С","М","И","Т","Ь","Б","Ю"};

  for (int i = 0; i < 32; i++) {
    if (qwerty[i] == lower_c) {
      return shift ? String(cyrillic_shift[i]) : String(cyrillic[i]);
    }
  }

  char res = shift ? getShiftedChar(c) : c;
  return String(res);
}

void handleGeminiChatKeyboard() {
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    static bool focusChatInput = true;

    if (M5Cardputer.Keyboard.isKeyPressed(KEY_TAB) && !status.shift) {
      Serial.println("Tab key pressed - Toggling focus.");
      if (focusChatInput) {
        lv_obj_add_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
        lv_obj_clear_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(ui_TextArea_AI_response, 0);
      } else {
        lv_obj_add_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
        lv_obj_clear_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
      }
      focusChatInput = !focusChatInput;
      return;
    }

    static unsigned long lastScrollTime = 0;
    unsigned long currentTime = millis();

    if (!focusChatInput && status.fn) {
      if (currentTime - lastScrollTime > 100) {
        if (std::find(status.word.begin(), status.word.end(), ';') != status.word.end()) {
          lv_textarea_cursor_up(ui_TextArea_AI_response);
          Serial.println("Scrolling up in AI response textarea");
        } else if (std::find(status.word.begin(), status.word.end(), '.') != status.word.end()) {
          lv_textarea_cursor_down(ui_TextArea_AI_response);
          Serial.println("Scrolling down in AI response textarea");
        }
        lastScrollTime = currentTime;
      }
    }
    else if (focusChatInput) {
      if (M5Cardputer.Keyboard.isKeyPressed(' ') && status.ctrl) {
        isRussianLayout = !isRussianLayout;
        Serial.println(isRussianLayout ? "Layout: RU" : "Layout: EN");
        lv_textarea_set_text(ui_TextArea_AI_response, isRussianLayout ? "Раскладка: РУС" : "Layout: ENG");
        return;
      }

      if (M5Cardputer.Keyboard.isKeyPressed(0x28) && !status.shift) {
        handleEnterKey();
      } else if (M5Cardputer.Keyboard.isKeyPressed(0x2A) && !status.shift) {
        lv_textarea_del_char(ui_TextArea_chat_input);
      } else {
        for (uint8_t keyCode = 32; keyCode <= 126; keyCode++) {
          if (M5Cardputer.Keyboard.isKeyPressed(keyCode)) {
            String mappedStr = getMappedChar(keyCode, status.shift, isRussianLayout);
            lv_textarea_add_text(ui_TextArea_chat_input, mappedStr.c_str());
          }
        }
      }
    }
  }
}

// Handle navigation for MENU screen
void handleMenuNavigation() {
  static lv_obj_t *focusedButton = ui_Button_reset;

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
      Serial.println("Moving focus up in menu.");
      lv_obj_clear_state(focusedButton, LV_STATE_FOCUSED);
      focusedButton = (focusedButton == ui_Button_back) ? ui_Button_reset : ui_Button_back;
      lv_obj_add_state(focusedButton, LV_STATE_FOCUSED);
    } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
      Serial.println("Moving focus down in menu.");
      lv_obj_clear_state(focusedButton, LV_STATE_FOCUSED);
      focusedButton = (focusedButton == ui_Button_reset) ? ui_Button_back : ui_Button_reset;
      lv_obj_add_state(focusedButton, LV_STATE_FOCUSED);
    }
    else if (M5Cardputer.Keyboard.isKeyPressed(0x28)) {
      Serial.println("Enter key pressed - Selecting focused button.");
      if (focusedButton == ui_Button_reset) {
        Serial.println("Reset button selected.");
        resetCredentialsAndRestart();
      } else if (focusedButton == ui_Button_back) {
        Serial.println("Back button selected.");
        nextScreen = UIScreen::GEMINI_CHAT;
      }
    }
  }
}

void renderLed() {
  static uint32_t lastBlinkToggle = 0;
  static bool blinkOn = false;
  static uint8_t lastEffect = 255;

  // Пока ИИ думает, показываем плавное синее дыхание.
  // Это работает даже если основной loop заблокирован HTTP-запросом.
  if (isAIThinking) {
    float phase = (sin((double)millis() / 900.0) + 1.0) / 2.0;
    uint8_t b = (uint8_t)(8 + phase * 60); // 8..68, безопасно и плавно

    leds[0] = CRGB(0, 0, b);
    FastLED.show();

    lastEffect = 255;
    return;
  }

  LedState st = getLedState();

  if (st.effect != lastEffect) {
    lastEffect = st.effect;
    blinkOn = true;
    lastBlinkToggle = millis();
  }

  uint8_t b = st.brightness;
  CRGB c = uint32ToCrgb(st.color);

  if (st.effect == LED_EFFECT_OFF || b == 0) {
    leds[0] = CRGB(0, 0, 0);
    FastLED.show();
    return;
  }

  if (st.effect == LED_EFFECT_SOLID) {
    c.nscale8_video(b);
    leds[0] = c;
  }

  else if (st.effect == LED_EFFECT_BLINK) {
    uint32_t now = millis();

    if (now - lastBlinkToggle >= 600) {
      blinkOn = !blinkOn;
      lastBlinkToggle = now;
    }

    if (blinkOn) {
      c.nscale8_video(b);
      leds[0] = c;
    } else {
      leds[0] = CRGB(0, 0, 0);
    }
  }

  else if (st.effect == LED_EFFECT_BREATHE) {
    float phase = (sin((double)millis() / 800.0) + 1.0) / 2.0;
    uint8_t scaled = (uint8_t)(phase * (float)b);

    c.nscale8_video(scaled);
    leds[0] = c;
  }

  else if (st.effect == LED_EFFECT_RAINBOW) {
    uint8_t hue = (uint8_t)((millis() / 15) % 255);
    leds[0] = CHSV(hue, 255, b);
  }

  else {
    c.nscale8_video(b);
    leds[0] = c;
  }

  FastLED.show();
}

void ledTask(void *pvParameters) {
  while (true) {
    renderLed();

    // 30 ms = примерно 33 кадра в секунду.
    // Это плавно для глаза и меньше нагружает питание/тайминги WS2812.
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void resetCredentialsAndRestart() {
  WiFi.disconnect(true, true);
  delay(1000);

  if (SD.exists("/config.json")) {
    SD.remove("/config.json");
    Serial.println("config.json deleted from SD Card.");
  }

  lv_label_set_text(ui_Label_info, "Credentials reset!");
  delay(1000);
  lv_label_set_text(ui_Label_info, "Restarting...");
  delay(2000);
  ESP.restart();
}

void handleEnterKey() {
  const char *inputText = lv_textarea_get_text(ui_TextArea_chat_input);

  if (inputText != nullptr && strlen(inputText) > 0) {
    String inputString = String(inputText);

    if (inputString.length() > 1024) {
      updateStatus("Error: Input too long!");
      Serial.println("Error: Input too long!");
      return;
    }

    Serial.println("Sending message to Gemini API...");

    lv_textarea_set_text(ui_TextArea_chat_input, "");
    lv_textarea_set_text(ui_TextArea_AI_response, "Thinking...");

    String response = geminiChat.sendMessage(inputString);

    // Сначала выполняем скрытые команды и удаляем теги из текста
    response = parseDeviceCommands(response);

    // Показываем уже очищенный ответ
    lv_textarea_set_text(ui_TextArea_AI_response, response.c_str());
  } else {
    updateStatus("Error: Input is empty.");
    Serial.println("Error: No input to send.");
  }
}

uint32_t crgbToUint32(const CRGB &c) {
  return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

CRGB uint32ToCrgb(uint32_t v) {
  return CRGB(
    (v >> 16) & 0xFF,
    (v >> 8) & 0xFF,
    v & 0xFF
  );
}

LedState getLedState() {
  LedState s;
  s.effect = LED_EFFECT_OFF;
  s.color = 0;
  s.brightness = 0;

  if (ledMutex != NULL && xSemaphoreTake(ledMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    s = ledState;
    xSemaphoreGive(ledMutex);
  }

  return s;
}

void setLedState(uint8_t effect, uint32_t color, uint8_t brightness) {
  if (ledMutex == NULL) {
    return;
  }

  if (xSemaphoreTake(ledMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    ledState.effect = effect;
    ledState.color = color;
    ledState.brightness = brightness;
    xSemaphoreGive(ledMutex);
  }
}

uint8_t parseLedEffectId(String effect) {
  effect.trim();
  effect.toLowerCase();

  if (effect == "off") {
    return LED_EFFECT_OFF;
  }

  if (effect == "solid") {
    return LED_EFFECT_SOLID;
  }

  if (effect == "blink") {
    return LED_EFFECT_BLINK;
  }

  if (effect == "breathe") {
    return LED_EFFECT_BREATHE;
  }

  if (effect == "rainbow") {
    return LED_EFFECT_RAINBOW;
  }

  return LED_EFFECT_SOLID;
}

void loop() {
  handleScreenTransitions();
  M5Cardputer.update();

  switch (currentScreen) {
    case UIScreen::GEMINI_CHAT:
      handleGeminiChatKeyboard();
      break;

    case UIScreen::MENU:
      handleMenuNavigation();
      break;

    default:
      break;
  }

  if (M5Cardputer.Keyboard.isKeyPressed(KEY_OPT)) {
    delay(100);
    nextScreen = UIScreen::MENU;
    Serial.println("KEY_OPT pressed, switching to MENU screen");
  }

  delay(5);
  yield();
}

void updateBattery() {
  float batteryVoltage = M5.Power.getBatteryVoltage() / 1000.0;
  batteryPercentage = (int)(((batteryVoltage - 3.3) / (4.2 - 3.3)) * 100);
  batteryPercentage = constrain(batteryPercentage, 0, 100);

  Serial.println();
  Serial.print("Battery Voltage: ");
  Serial.print(batteryVoltage, 2);
  Serial.print("V, Battery Percentage: ");
  Serial.print(batteryPercentage);
  Serial.println("%");
}

// ============================================================
// НОВЫЕ ФУНКЦИИ ДЛЯ СКРЫТЫХ КОМАНД ИИ
// ============================================================

int indexOfIgnoreCase(const String &haystack, const String &needle, int from) {
  if (from < 0 || from >= (int)haystack.length()) {
    return -1;
  }

  String h = haystack.substring(from);
  String n = needle;

  h.toLowerCase();
  n.toLowerCase();

  int pos = h.indexOf(n);

  if (pos < 0) {
    return -1;
  }

  return from + pos;
}

int getBatteryPercentage() {
  float batteryVoltage = M5.Power.getBatteryVoltage() / 1000.0f;
  int p = (int)(((batteryVoltage - 3.3f) / (4.2f - 3.3f)) * 100.0f);
  p = constrain(p, 0, 100);
  return p;
}

void setScreenBrightness(int value) {
  value = constrain(value, 0, 255);
  M5.Display.setBrightness((uint8_t)value);
  Serial.printf("[DEV] Screen brightness set: %d\n", value);

  // Если M5.Display.setBrightness не компилируется, замени на:
  // M5Cardputer.Display.setBrightness((uint8_t)value);
}

CRGB parseLedColor(String s) {
  s.trim();
  s.toLowerCase();

  if (s.length() == 0) {
    return CRGB(0, 0, 0);
  }

  if (s[0] == '#') {
    s = s.substring(1);
  }

  if (s.length() == 6) {
    uint32_t hex = (uint32_t)strtol(s.c_str(), NULL, 16);

    uint8_t r = (hex >> 16) & 0xFF;
    uint8_t g = (hex >> 8) & 0xFF;
    uint8_t b = hex & 0xFF;

    return CRGB(r, g, b);
  }

  if (s == "red") {
    return CRGB(255, 0, 0);
  }

  if (s == "green") {
    return CRGB(0, 255, 0);
  }

  if (s == "blue") {
    return CRGB(0, 0, 255);
  }

  if (s == "white") {
    return CRGB(255, 255, 255);
  }

  if (s == "black" || s == "off") {
    return CRGB(0, 0, 0);
  }

  if (s == "yellow") {
    return CRGB(255, 255, 0);
  }

  if (s == "cyan") {
    return CRGB(0, 255, 255);
  }

  if (s == "magenta") {
    return CRGB(255, 0, 255);
  }

  if (s == "orange") {
    return CRGB(255, 165, 0);
  }

  if (s == "purple") {
    return CRGB(128, 0, 128);
  }

  if (s == "pink") {
    return CRGB(255, 105, 180);
  }

  return CRGB(255, 255, 255);
}

void applyLedCommand(String params) {
  params.trim();

  if (params.length() == 0) {
    return;
  }

  int p1 = params.indexOf(';');

  String effectStr = (p1 < 0) ? params : params.substring(0, p1);
  effectStr.trim();

  LedState old = getLedState();

  String colorStr = "";
  uint8_t brightness = old.brightness;

  if (brightness == 0) {
    brightness = 80;
  }

  if (p1 >= 0) {
    int p2 = params.indexOf(';', p1 + 1);

    if (p2 < 0) {
      colorStr = params.substring(p1 + 1);
    } else {
      colorStr = params.substring(p1 + 1, p2);

      String b = params.substring(p2 + 1);
      b.trim();

      if (b.length() > 0) {
        brightness = b.toInt();
      }
    }
  }

  brightness = constrain(brightness, 0, 255);

  uint8_t effect = parseLedEffectId(effectStr);

  CRGB color = uint32ToCrgb(old.color);

  if (colorStr.length() > 0) {
    color = parseLedColor(colorStr);
  } else {
    if (old.color == 0) {
      color = CRGB(255, 255, 255);
    }
  }

  if (effect == LED_EFFECT_OFF) {
    setLedState(LED_EFFECT_OFF, 0, 0);
    return;
  }

  setLedState(effect, crgbToUint32(color), brightness);

  Serial.printf("[DEV] LED command applied: effect=%d brightness=%d\n",
                effect,
                brightness);
}

String parseDeviceCommands(String text) {
  // Обработка яркости экрана: [SCREEN:xxx] или [BACKLIGHT:xxx]
  while (true) {
    int startScreen = indexOfIgnoreCase(text, "[SCREEN:");
    int startBacklight = indexOfIgnoreCase(text, "[BACKLIGHT:");

    if (startScreen < 0 && startBacklight < 0) {
      break;
    }

    int start = startScreen;
    int tagLen = 8; // длина "[SCREEN:"

    if (startScreen < 0 ||
        (startBacklight >= 0 && startBacklight < startScreen)) {
      start = startBacklight;
      tagLen = 11; // длина "[BACKLIGHT:"
    }

    int end = text.indexOf(']', start);

    if (end < 0) {
      break;
    }

    String value = text.substring(start + tagLen, end);
    value.trim();
    setScreenBrightness(value.toInt());

    text = text.substring(0, start) + text.substring(end + 1);
  }

  // Обработка LED: [LED:...]
  while (true) {
    int start = indexOfIgnoreCase(text, "[LED:");

    if (start < 0) {
      break;
    }

    int end = text.indexOf(']', start);

    if (end < 0) {
      break;
    }

    String params = text.substring(start + 5, end); // "[LED:" = 5 символов
    applyLedCommand(params);

    text = text.substring(0, start) + text.substring(end + 1);
  }

  // Обработка батареи: [BATTERY]
  String batteryValue = String(getBatteryPercentage()) + "%";

  while (true) {
    int start = indexOfIgnoreCase(text, "[BATTERY]");

    if (start < 0) {
      break;
    }

    text = text.substring(0, start) +
           batteryValue +
           text.substring(start + 9);
  }

  // Дополнительный короткий алиас: [BATT]
  while (true) {
    int start = indexOfIgnoreCase(text, "[BATT]");

    if (start < 0) {
      break;
    }

    text = text.substring(0, start) +
           batteryValue +
           text.substring(start + 6);
  }

  // Если ответ был обрезан и остался незакрытый тег
  int lastOpen = text.lastIndexOf('[');
  if (lastOpen >= 0 && text.indexOf(']', lastOpen) < 0) {
    text = text.substring(0, lastOpen);
  }

  // Чистим лишние пробелы после удаления тегов
  while (text.indexOf("  ") >= 0) {
    text.replace("  ", " ");
  }

  text.trim();

  if (text.length() == 0) {
    text = "OK";
  }

  return text;
}