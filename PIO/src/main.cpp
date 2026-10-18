#include <M5Cardputer.h>
#include <lvgl.h>
#include <ui.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include "WiFiManagerHandler.h"
#include "GeminiChat.h"
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

static String conversationTranscript = "";
static SemaphoreHandle_t transcriptMutex = NULL;

void appendToTranscript(const String &text);
String getConversationTranscript();
bool handleRemoteInput(const String &text);
void triggerPukeEasterEgg();
void chatWorkerTask(void *pvParameters);

UIScreen currentScreen = UIScreen::LOADING;
UIScreen nextScreen = UIScreen::NONE;

volatile bool wifiConnectedFlag = false;
volatile bool apiKeySavedFlag = false;

bool isRussianLayout = false;
volatile bool isAIThinking = false;
static bool chatTaskRunning = false;
static int currentScreenBrightness = 180;

enum LedEffectId : uint8_t {
  LED_EFFECT_OFF = 0,
  LED_EFFECT_SOLID,
  LED_EFFECT_BLINK,
  LED_EFFECT_BREATHE,
  LED_EFFECT_RAINBOW,
  LED_EFFECT_PATTERN
};

struct LedStep {
  CRGB color;
  uint16_t delayMs;
};

static const size_t MAX_LED_STEPS = 16;
static LedStep ledPatternSteps[MAX_LED_STEPS];
static size_t ledPatternCount = 0;

struct LedState {
  uint8_t effect;
  uint32_t color;
  uint8_t brightness;
};

static LedState ledState = {
  LED_EFFECT_OFF,
  0,
  0
};

static SemaphoreHandle_t ledMutex = NULL;
static SemaphoreHandle_t lvglMutex = NULL;
static TaskHandle_t ledTaskHandle = NULL;
static TaskHandle_t lvglTaskHandle = NULL;
static TaskHandle_t chatTaskHandle = NULL;

static lv_obj_t *langToastObj = NULL;
static lv_obj_t *langToastLabel = NULL;
static uint32_t langToastHideAt = 0;

unsigned long screenStartTime = 0;
bool webServerStarted = false;
int batteryPercentage = 0;

bool wifiConnected = false;
bool apiKeyConfigured = false;

void lvglLock() {
  if (lvglMutex != NULL) {
    xSemaphoreTake(lvglMutex, portMAX_DELAY);
  }
}

void lvglUnlock() {
  if (lvglMutex != NULL) {
    xSemaphoreGive(lvglMutex);
  }
}

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
void handleWifiSetupKeyboard();
void updateBattery();

uint32_t crgbToUint32(const CRGB &c);
CRGB uint32ToCrgb(uint32_t v);
LedState getLedState();
void setLedState(uint8_t effect, uint32_t color, uint8_t brightness);
void setLedPatternFromJson(JsonArray steps);
uint8_t parseLedEffectId(String effect);
void applyLedCommand(String params);
CRGB parseLedColor(String s);
void renderLed();
void ledTask(void *pvParameters);

int getBatteryPercentage();
void setScreenBrightness(int value);
String parseDeviceCommands(String text);

void initLangToast();
void showLangToast(bool isRu);

enum class WifiSetupState : uint8_t {
  TRYING_SAVED,
  SCANNING,
  AWAITING_SELECTION,
  AWAITING_PASSWORD,
  CONNECTING,
  CONNECTED
};

volatile WifiSetupState wifiSetupState = WifiSetupState::TRYING_SAVED;

const int kMaxWifiNetworks = 9;
String scannedSSIDs[kMaxWifiNetworks];
int scannedNetworkCount = 0;

String wifiSelectedSSID = "";
String wifiSelectedPassword = "";
String wifiPasswordInput = "";

void scanWifiNetworks() {
  scannedNetworkCount = 0;
  int found = WiFi.scanNetworks();

  for (int i = 0; i < found && scannedNetworkCount < kMaxWifiNetworks; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;

    bool duplicate = false;
    for (int j = 0; j < scannedNetworkCount; j++) {
      if (scannedSSIDs[j] == ssid) { duplicate = true; break; }
    }
    if (duplicate) continue;

    scannedSSIDs[scannedNetworkCount++] = ssid;
  }

  WiFi.scanDelete();
}

bool tryConnect(const String &ssid, const String &password, unsigned long timeoutMs) {
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  return WiFi.status() == WL_CONNECTED;
}

void wifiConnectTask(void *pvParameters) {
  WiFi.mode(WIFI_STA);

  String savedSSID = getStoredWifiSSID();
  String savedPassword = getStoredWifiPassword();

  if (!savedSSID.isEmpty()) {
    wifiSetupState = WifiSetupState::TRYING_SAVED;
    if (tryConnect(savedSSID, savedPassword, 12000)) {
      wifiConnectedFlag = true;
      wifiSetupState = WifiSetupState::CONNECTED;
      vTaskDelete(NULL);
      return;
    }
  }

  while (true) {
    wifiSetupState = WifiSetupState::SCANNING;
    scanWifiNetworks();
    wifiSetupState = WifiSetupState::AWAITING_SELECTION;

    while (wifiSetupState == WifiSetupState::AWAITING_SELECTION ||
           wifiSetupState == WifiSetupState::AWAITING_PASSWORD) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (wifiSetupState != WifiSetupState::CONNECTING) {
      continue;
    }

    if (tryConnect(wifiSelectedSSID, wifiSelectedPassword, 15000)) {
      saveWifiCredentials(wifiSelectedSSID, wifiSelectedPassword);
      wifiConnectedFlag = true;
      wifiSetupState = WifiSetupState::CONNECTED;
      vTaskDelete(NULL);
      return;
    } else {
      updateStatus("Connection failed!\nRetrying scan...");
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
  }
}

void lvgl_setup() {
  lv_init();

  static lv_disp_draw_buf_t draw_buf;
  static lv_color_t buf1[240 * 10];
  static lv_color_t buf2[240 * 10];
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, 240 * 10);

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

void updateStatus(const String &status) {
  lvglLock();
  lv_textarea_set_text(ui_TextArea_debug_status, (status + "\n").c_str());
  lvglUnlock();
  Serial.println(status);
}

void initLangToast() {
  langToastObj = lv_obj_create(ui_Screen_AiChat);
  lv_obj_remove_style_all(langToastObj);
  lv_obj_set_size(langToastObj, 110, 40);
  lv_obj_set_align(langToastObj, LV_ALIGN_CENTER);
  lv_obj_set_style_radius(langToastObj, 8, 0);
  lv_obj_set_style_bg_color(langToastObj, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(langToastObj, 200, 0);
  lv_obj_set_style_border_color(langToastObj, lv_color_hex(0x4EBD92), 0);
  lv_obj_set_style_border_width(langToastObj, 2, 0);
  lv_obj_set_style_border_opa(langToastObj, 230, 0);
  lv_obj_clear_flag(langToastObj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  langToastLabel = lv_label_create(langToastObj);
  lv_obj_set_align(langToastLabel, LV_ALIGN_CENTER);
  lv_obj_set_style_text_color(langToastLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(langToastLabel, &rubik_14_cy, 0);
  lv_label_set_text(langToastLabel, "РУС");

  lv_obj_add_flag(langToastObj, LV_OBJ_FLAG_HIDDEN);
}

void showLangToast(bool isRu) {
  if (!langToastObj) return;
  lvglLock();
  lv_label_set_text(langToastLabel, isRu ? "[ РУС ]" : "[ ENG ]");
  lv_obj_clear_flag(langToastObj, LV_OBJ_FLAG_HIDDEN);
  langToastHideAt = millis() + 1200;
  lvglUnlock();
}

void appendToTranscript(const String &text) {
  if (transcriptMutex != NULL) xSemaphoreTake(transcriptMutex, portMAX_DELAY);
  conversationTranscript += text;
  if (conversationTranscript.length() > 6000) {
    conversationTranscript = conversationTranscript.substring(conversationTranscript.length() - 6000);
  }
  if (transcriptMutex != NULL) xSemaphoreGive(transcriptMutex);
}

String getConversationTranscript() {
  String copy = "";
  if (transcriptMutex != NULL && xSemaphoreTake(transcriptMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
    copy = conversationTranscript;
    xSemaphoreGive(transcriptMutex);
  } else {
    copy = conversationTranscript;
  }
  return copy;
}

bool handleRemoteInput(const String &text) {
  if (isAIThinking || chatTaskRunning) {
    return false;
  }
  String prompt = text;
  prompt.trim();
  if (prompt.isEmpty()) return false;

  if (prompt == "/puke" || prompt == "/shake") {
    triggerPukeEasterEgg();
    return true;
  }

  lvglLock();
  lv_textarea_set_text(ui_TextArea_chat_input, "");
  appendToTranscript("> Web:\n" + prompt + "\n");
  lv_textarea_set_text(ui_TextArea_AI_response, (conversationTranscript + "[Thinking...]\n").c_str());
  lv_textarea_set_cursor_pos(ui_TextArea_AI_response, LV_TEXTAREA_CURSOR_LAST);
  lvglUnlock();

  chatTaskRunning = true;
  String *pPrompt = new String(prompt);
  xTaskCreatePinnedToCore(
    chatWorkerTask,
    "chatWorkerTask",
    16384,
    pPrompt,
    1,
    &chatTaskHandle,
    1
  );
  return true;
}

String handleWebChat(const String &prompt) {
  handleRemoteInput(prompt);
  return "OK";
}

DeviceStateInfo getDeviceState() {
  DeviceStateInfo info;
  info.screenBrightness = currentScreenBrightness;

  LedState st = getLedState();
  info.ledEffect = (st.effect == LED_EFFECT_OFF) ? "off" :
                   (st.effect == LED_EFFECT_SOLID) ? "solid" :
                   (st.effect == LED_EFFECT_BLINK) ? "blink" :
                   (st.effect == LED_EFFECT_BREATHE) ? "breathe" :
                   (st.effect == LED_EFFECT_RAINBOW) ? "rainbow" : "pattern";

  char hexBuf[10];
  snprintf(hexBuf, sizeof(hexBuf), "#%06X", (unsigned int)st.color);
  info.ledColorHex = String(hexBuf);
  info.ledBrightness = st.brightness;
  info.batteryPercentage = getBatteryPercentage();
  info.isThinking = isAIThinking || chatTaskRunning;
  info.model = geminiChat.getModel();
  info.wifiRssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  info.freeHeap = ESP.getFreeHeap();
  info.uptimeSec = millis() / 1000;
  info.sdMounted = isSDCardAvailable();

  return info;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialize NVS with auto-format on corruption/space exhaustion
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = true;
  cfg.external_imu = true;
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Keyboard.begin();

  lvglMutex = xSemaphoreCreateMutex();
  ledMutex = xSemaphoreCreateMutex();
  transcriptMutex = xSemaphoreCreateMutex();

  lvgl_setup();

  Serial.println("[System] Creating LVGL tasks...");
  xTaskCreatePinnedToCore(lv_tick_task, "lv_tick_task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 8192, NULL, 2, &lvglTaskHandle, 1);

  lvglLock();
  ui_init();
  lv_obj_set_style_text_font(ui_TextArea_AI_response, &rubik_14_cy, 0);
  lv_obj_set_style_text_font(ui_TextArea_chat_input, &rubik_14_cy, 0);
  lv_obj_set_style_text_font(ui_TextArea_debug_status, &rubik_14_cy, 0);
  initLangToast();
  lvglUnlock();
  Serial.println("[System] UI Initialized with Cyrillic font & toast.");

  // FastLED: 1 WS2812 LED without power limiter or dithering to prevent flickering/dimming
  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setDither(DISABLE_DITHER);
  FastLED.setBrightness(255);
  leds[0] = CRGB(0, 0, 0);
  FastLED.show();

  if (M5.Led.isEnabled()) {
    M5.Led.setBrightness(0);
    M5.Led.setAllColor(0, 0, 0);
    M5.Led.display();
  }

  xTaskCreatePinnedToCore(
    ledTask,
    "ledTask",
    4096,
    NULL,
    2,
    &ledTaskHandle,
    0
  );

  switchScreen(UIScreen::LOADING);

  xTaskCreatePinnedToCore(wifiConnectTask, "wifiConnectTask", 8192, NULL, 1, NULL, 0);

  // Register Web Chat, Remote Keyboard & Device State providers
  setWebChatHandler(handleWebChat);
  setDeviceStateProvider(getDeviceState);
  setRemoteInputHandler(handleRemoteInput);
  setTranscriptProvider(getConversationTranscript);
  setShakeEasterEggTrigger(triggerPukeEasterEgg);

  // Initialize Gemini Chat and Hardware Functions
  geminiChat.setDeviceFunctionHandler([](const String &fnName, const String &argsJson) -> String {
    Serial.printf("[FunctionCall] %s args: %s\n", fnName.c_str(), argsJson.c_str());
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, argsJson);

    if (fnName.endsWith("set_screen_brightness")) {
      int pct = 80;
      if (!err) {
        if (doc.containsKey("brightness_percent")) {
          pct = doc["brightness_percent"].as<int>();
        } else if (doc.containsKey("brightness")) {
          pct = doc["brightness"].as<int>();
        } else if (doc.containsKey("level")) {
          pct = doc["level"].as<int>();
        } else if (doc.containsKey("value")) {
          pct = doc["value"].as<int>();
        }
      }
      pct = constrain(pct, 1, 100);
      int val = (pct * 255) / 100;
      if (val < 5) val = 5;
      setScreenBrightness(val);
      return "Яркость экрана установлена на " + String(pct) + "%";
    }

    if (fnName.endsWith("set_led_effect")) {
      String effect = "solid";
      String color = "white";
      int brightnessPct = 80;

      if (!err) {
        if (doc.containsKey("effect")) effect = doc["effect"].as<String>();
        if (doc.containsKey("color")) color = doc["color"].as<String>();
        if (doc.containsKey("brightness_percent")) brightnessPct = doc["brightness_percent"].as<int>();
        else if (doc.containsKey("brightness")) brightnessPct = doc["brightness"].as<int>();
      }

      brightnessPct = constrain(brightnessPct, 0, 100);
      uint8_t b = (brightnessPct * 255) / 100;
      uint8_t effId = parseLedEffectId(effect);
      CRGB c = parseLedColor(color);

      setLedState(effId, crgbToUint32(c), b);
      return "Светодиод переключен в режим " + effect + " (" + color + ", " + String(brightnessPct) + "%)";
    }

    if (fnName.endsWith("generate_led_pattern")) {
      if (!err && doc.containsKey("steps")) {
        setLedPatternFromJson(doc["steps"].as<JsonArray>());
        return "Пользовательская анимация светодиода запущена";
      }
      return "Ошибка параметров анимации";
    }

    if (fnName.endsWith("get_battery_percentage")) {
      int bat = getBatteryPercentage();
      return "Текущий заряд аккумулятора составляет " + String(bat) + "%";
    }

    return "Команда выполнена";
  });

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
    if (lvglMutex != NULL && xSemaphoreTake(lvglMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      lv_timer_handler();
      xSemaphoreGive(lvglMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void handleScreenTransitions() {
  if (nextScreen != UIScreen::NONE) {
    switchScreen(nextScreen);
    nextScreen = UIScreen::NONE;
  }

  switch (currentScreen) {
    case UIScreen::LOADING:
      if (millis() - screenStartTime > 3000) {
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

static lv_obj_t *focusedMenuButton = NULL;

void switchScreen(UIScreen screen) {
  if (currentScreen == screen) return;

  currentScreen = screen;
  screenStartTime = millis();

  lvglLock();
  switch (screen) {
    case UIScreen::LOADING:
      lv_scr_load(ui_Screen_loading);
      break;

    case UIScreen::STATUS:
      lv_scr_load(ui_Screen_status);
      break;

    case UIScreen::GEMINI_CHAT:
      lv_scr_load(ui_Screen_AiChat);
      lv_obj_add_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
      lv_obj_clear_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
      break;

    case UIScreen::MENU:
      lv_scr_load(ui_Screen_Menu);
      lv_label_set_text(ui_Label_ip, getIPAddress().c_str());
      updateBattery();
      lv_bar_set_value(ui_Bar_Battery, batteryPercentage, LV_ANIM_ON);

      focusedMenuButton = ui_Button_back;
      lv_obj_clear_state(ui_Button_reset, LV_STATE_FOCUSED);
      lv_obj_add_state(ui_Button_back, LV_STATE_FOCUSED);
      break;

    default:
      Serial.println("[UI] Invalid screen transition requested.");
      break;
  }
  lvglUnlock();
}

void renderWifiSetupStatus() {
  static WifiSetupState lastRendered = WifiSetupState::CONNECTED;
  static int lastPasswordLen = -1;

  if (wifiSetupState == lastRendered &&
      !(wifiSetupState == WifiSetupState::AWAITING_PASSWORD &&
        (int)wifiPasswordInput.length() != lastPasswordLen)) {
    return;
  }
  lastRendered = wifiSetupState;
  lastPasswordLen = wifiPasswordInput.length();

  switch (wifiSetupState) {
    case WifiSetupState::TRYING_SAVED:
      updateStatus("Connecting to\nsaved Wi-Fi...");
      break;

    case WifiSetupState::SCANNING:
      updateStatus("Scanning for\nWi-Fi networks...");
      break;

    case WifiSetupState::AWAITING_SELECTION: {
      String list = "Select Wi-Fi (r=scan):\n";
      if (scannedNetworkCount == 0) {
        list += "None found. Press 'r'.";
      } else {
        for (int i = 0; i < scannedNetworkCount; i++) {
          list += String(i + 1) + ". " + scannedSSIDs[i] + "\n";
        }
      }
      updateStatus(list);
      break;
    }

    case WifiSetupState::AWAITING_PASSWORD: {
      String masked = "";
      for (size_t i = 0; i < wifiPasswordInput.length(); i++) masked += "*";
      updateStatus("Password for\n\"" + wifiSelectedSSID + "\":\n" + masked + "_\n(Opt/Esc to back)");
      break;
    }

    case WifiSetupState::CONNECTING:
      updateStatus("Connecting to\n\"" + wifiSelectedSSID + "\"...");
      break;

    default:
      break;
  }
}

void handleWifiSetupKeyboard() {
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  if (wifiSetupState == WifiSetupState::AWAITING_SELECTION) {
    if (M5Cardputer.Keyboard.isKeyPressed('r')) {
      wifiSetupState = WifiSetupState::SCANNING;
      return;
    }
    for (uint8_t k = '1'; k <= '9'; k++) {
      if (M5Cardputer.Keyboard.isKeyPressed(k)) {
        int idx = k - '1';
        if (idx < scannedNetworkCount) {
          wifiSelectedSSID = scannedSSIDs[idx];
          wifiPasswordInput = "";
          wifiSetupState = WifiSetupState::AWAITING_PASSWORD;
        }
        return;
      }
    }
    return;
  }

  if (wifiSetupState == WifiSetupState::AWAITING_PASSWORD) {
    if (status.opt || M5Cardputer.Keyboard.isKeyPressed(KEY_OPT) || M5Cardputer.Keyboard.isKeyPressed('`')) {
      wifiSetupState = WifiSetupState::AWAITING_SELECTION;
      return;
    }

    if (status.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
      wifiSelectedPassword = wifiPasswordInput;
      wifiSetupState = WifiSetupState::CONNECTING;
      return;
    }

    if (status.del || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
      if (wifiPasswordInput.length() > 0) {
        wifiPasswordInput.remove(wifiPasswordInput.length() - 1);
      }
      return;
    }

    for (char c : status.word) {
      if (c >= 32 && c <= 126) {
        wifiPasswordInput += c;
      }
    }
  }
}

void handleStatusScreenLogic() {
  renderWifiSetupStatus();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      updateStatus("Wi-Fi connected!\nIP: " + getIPAddress());
      if (!webServerStarted) {
        startWebServer();
        webServerStarted = true;
      }
    }

    String apiKey = getStoredApiKey();
    if (apiKey.isEmpty()) {
      static unsigned long lastPromptTime = 0;
      if (millis() - lastPromptTime > 3000) {
        lastPromptTime = millis();
        updateStatus("Set API key at:\nhttp://" + getIPAddress() + "/\nor via config.json");
      }
      return;
    } else if (!apiKeyConfigured) {
      apiKeyConfigured = true;
      geminiChat.setApiKey(apiKey);
      updateStatus("API Key configured!\nStarting Gemini Chat...");
      delay(1000);
      switchScreen(UIScreen::GEMINI_CHAT);
    }
  }
}

String getMappedChar(char c, bool shift, bool isRu) {
  if (!isRu) {
    return String(c);
  }

  if (c == '`') return shift ? "Ё" : "ё";
  if (c == '~') return "Ё";

  char lower_c = tolower(c);
  const char* qwerty = "qwertyuiop[]asdfghjkl;'zxcvbnm,.";
  const char* cyrillic[] = {"й","ц","у","к","е","н","г","ш","щ","з","х","ъ","ф","ы","в","а","п","р","о","л","д","ж","э","я","ч","с","м","и","т","ь","б","ю"};
  const char* cyrillic_shift[] = {"Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","Ъ","Ф","Ы","В","А","П","Р","О","Л","Д","Ж","Э","Я","Ч","С","М","И","Т","Ь","Б","Ю"};

  for (int i = 0; i < 32; i++) {
    if (qwerty[i] == lower_c) {
      return (shift || isupper(c)) ? String(cyrillic_shift[i]) : String(cyrillic[i]);
    }
  }

  return String(c);
}

void chatWorkerTask(void *pvParameters) {
  String *promptPtr = static_cast<String*>(pvParameters);
  String prompt = *promptPtr;
  delete promptPtr;

  Serial.println("[Chat] Worker started for: " + prompt);
  String response = geminiChat.sendMessage(prompt);
  response = parseDeviceCommands(response);

  appendToTranscript("Gemini:\n" + response + "\n\n");

  lvglLock();
  lv_textarea_set_text(ui_TextArea_AI_response, conversationTranscript.c_str());
  lv_textarea_set_cursor_pos(ui_TextArea_AI_response, LV_TEXTAREA_CURSOR_LAST);
  lvglUnlock();

  chatTaskRunning = false;
  vTaskDelete(NULL);
}

void handleGeminiChatKeyboard() {
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  static bool focusChatInput = true;

  // Toggle Focus between Chat Input and AI Response with Tab
  if (status.tab || M5Cardputer.Keyboard.isKeyPressed(KEY_TAB)) {
    focusChatInput = !focusChatInput;
    lvglLock();
    if (focusChatInput) {
      lv_obj_add_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
      lv_obj_clear_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
    } else {
      lv_obj_add_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
      lv_obj_clear_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
    }
    lvglUnlock();
    return;
  }

  // When focused on AI Response: Scroll Up/Down/PageUp/PageDown
  if (!focusChatInput) {
    bool up = (std::find(status.word.begin(), status.word.end(), ';') != status.word.end());
    bool down = (std::find(status.word.begin(), status.word.end(), '.') != status.word.end());
    bool pageUp = (std::find(status.word.begin(), status.word.end(), ',') != status.word.end());
    bool pageDown = (std::find(status.word.begin(), status.word.end(), '/') != status.word.end());

    lvglLock();
    if (up) {
      lv_textarea_cursor_up(ui_TextArea_AI_response);
    } else if (down) {
      lv_textarea_cursor_down(ui_TextArea_AI_response);
    } else if (pageUp) {
      for (int i = 0; i < 4; i++) lv_textarea_cursor_up(ui_TextArea_AI_response);
    } else if (pageDown) {
      for (int i = 0; i < 4; i++) lv_textarea_cursor_down(ui_TextArea_AI_response);
    }
    lvglUnlock();

    // If user presses Enter or begins typing while focused on transcript, return focus to chat input
    if (status.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
      focusChatInput = true;
      lvglLock();
      lv_obj_add_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
      lv_obj_clear_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
      lvglUnlock();
      return;
    }

    for (char c : status.word) {
      if (c >= 32 && c <= 126 && c != ';' && c != '.' && c != ',' && c != '/') {
        focusChatInput = true;
        lvglLock();
        lv_obj_add_state(ui_TextArea_chat_input, LV_STATE_FOCUSED);
        lv_obj_clear_state(ui_TextArea_AI_response, LV_STATE_FOCUSED);
        String mapped = getMappedChar(c, status.shift, isRussianLayout);
        lv_textarea_add_text(ui_TextArea_chat_input, mapped.c_str());
        lvglUnlock();
        return;
      }
    }
    return;
  }

  // When focused on Chat Input:
  if (focusChatInput) {
    // Quick transcript scroll with Fn + ; (Up) and Fn + . (Down) even when in input
    if (status.fn) {
      if (std::find(status.word.begin(), status.word.end(), ';') != status.word.end()) {
        lvglLock();
        lv_textarea_cursor_up(ui_TextArea_AI_response);
        lvglUnlock();
        return;
      }
      if (std::find(status.word.begin(), status.word.end(), '.') != status.word.end()) {
        lvglLock();
        lv_textarea_cursor_down(ui_TextArea_AI_response);
        lvglUnlock();
        return;
      }
      if (std::find(status.word.begin(), status.word.end(), ',') != status.word.end()) {
        lvglLock();
        lv_textarea_cursor_left(ui_TextArea_chat_input);
        lvglUnlock();
        return;
      }
      if (std::find(status.word.begin(), status.word.end(), '/') != status.word.end()) {
        lvglLock();
        lv_textarea_cursor_right(ui_TextArea_chat_input);
        lvglUnlock();
        return;
      }
    }

    // Ctrl + Space toggles layout EN / RU and shows popup toast
    if (status.ctrl && status.space) {
      isRussianLayout = !isRussianLayout;
      Serial.println(isRussianLayout ? "[Keyboard] Layout: RU" : "[Keyboard] Layout: EN");
      showLangToast(isRussianLayout);
      return;
    }

    // Enter sends message
    if (status.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
      handleEnterKey();
      return;
    }

    // Backspace deletes char
    if (status.del || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
      lvglLock();
      lv_textarea_del_char(ui_TextArea_chat_input);
      lvglUnlock();
      return;
    }

    // Text characters
    for (char c : status.word) {
      if (c == 0) continue;
      String mapped = getMappedChar(c, status.shift, isRussianLayout);
      lvglLock();
      lv_textarea_add_text(ui_TextArea_chat_input, mapped.c_str());
      lvglUnlock();
    }
  }
}

void handleMenuNavigation() {
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  if (status.opt || M5Cardputer.Keyboard.isKeyPressed(KEY_OPT)) {
    nextScreen = UIScreen::GEMINI_CHAT;
    return;
  }

  bool up = (std::find(status.word.begin(), status.word.end(), ';') != status.word.end());
  bool down = (std::find(status.word.begin(), status.word.end(), '.') != status.word.end());

  if (up || down) {
    lvglLock();
    lv_obj_clear_state(focusedMenuButton, LV_STATE_FOCUSED);
    focusedMenuButton = (focusedMenuButton == ui_Button_back) ? ui_Button_reset : ui_Button_back;
    lv_obj_add_state(focusedMenuButton, LV_STATE_FOCUSED);
    lvglUnlock();
    return;
  }

  if (status.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
    if (focusedMenuButton == ui_Button_reset) {
      resetCredentialsAndRestart();
    } else {
      nextScreen = UIScreen::GEMINI_CHAT;
    }
  }
}

void triggerPukeEasterEgg() {
  Serial.println("[EasterEgg] Puke animation triggered!");
  setLedState(LED_EFFECT_BLINK, crgbToUint32(CRGB(57, 255, 20)), 255);

  M5Cardputer.Speaker.tone(280, 120);
  delay(120);
  M5Cardputer.Speaker.tone(180, 160);
  delay(160);
  M5Cardputer.Speaker.tone(90, 250);

  lvglLock();
  lv_textarea_set_text(ui_TextArea_AI_response,
    "🤢 (>﹏<) *HUUUUUURRRRRKKK*...\n\n"
    "🤮🤮🤮\n"
    "░░░░▓▓▓▓████████▓▓▓▓░░░░\n"
    "BLEEEEAAARRRRRGGGHHHHHH!\n"
    "▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓\n"
    "≈≈≈≈≈ [ СЛИЗЬ И РВОТА ] ≈≈≈≈≈\n\n"
    "(x_x) *whew...*\n"
    "Пожалуйста, не тряси меня!"
  );
  lv_textarea_set_cursor_pos(ui_TextArea_AI_response, 0);
  lvglUnlock();

  delay(3500);

  lvglLock();
  lv_textarea_set_text(ui_TextArea_AI_response, conversationTranscript.c_str());
  lv_textarea_set_cursor_pos(ui_TextArea_AI_response, LV_TEXTAREA_CURSOR_LAST);
  lvglUnlock();

  setLedState(LED_EFFECT_OFF, 0, 0);
}

void renderLed() {
  static uint32_t lastBlinkToggle = 0;
  static bool blinkOn = false;
  static uint8_t lastEffect = 255;
  static uint32_t lastColor = 0xFFFFFFFF;
  static uint8_t lastBrightness = 255;
  static size_t currentPatternIndex = 0;
  static uint32_t lastPatternStepTime = 0;

  if (isAIThinking || chatTaskRunning) {
    float phase = (sin((double)millis() / 800.0) + 1.0) / 2.0;
    uint8_t b = (uint8_t)(10 + phase * 80);

    if (M5.Led.isEnabled()) {
      M5.Led.setBrightness(b);
      M5.Led.setAllColor(0, 0, 255);
      M5.Led.display();
    }
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
    currentPatternIndex = 0;
    lastPatternStepTime = millis();
  }

  uint8_t b = st.brightness;
  CRGB c = uint32ToCrgb(st.color);

  if (st.effect == LED_EFFECT_OFF || b == 0) {
    if (lastBrightness != 0 || lastEffect != LED_EFFECT_OFF) {
      if (M5.Led.isEnabled()) {
        M5.Led.setBrightness(0);
        M5.Led.setAllColor(0, 0, 0);
        M5.Led.display();
      }
      leds[0] = CRGB(0, 0, 0);
      FastLED.show();
      lastBrightness = 0;
      lastColor = 0;
    }
    return;
  }

  if (st.effect == LED_EFFECT_SOLID) {
    if (lastColor != st.color || lastBrightness != b || lastEffect != LED_EFFECT_SOLID) {
      if (M5.Led.isEnabled()) {
        M5.Led.setBrightness(b);
        M5.Led.setAllColor(c.r, c.g, c.b);
        M5.Led.display();
      }
      c.nscale8_video(b);
      leds[0] = c;
      FastLED.show();
      lastColor = st.color;
      lastBrightness = b;
      lastEffect = LED_EFFECT_SOLID;
    }
    return;
  }

  if (st.effect == LED_EFFECT_BLINK) {
    uint32_t now = millis();
    if (now - lastBlinkToggle >= 500) {
      blinkOn = !blinkOn;
      lastBlinkToggle = now;

      if (blinkOn) {
        if (M5.Led.isEnabled()) {
          M5.Led.setBrightness(b);
          M5.Led.setAllColor(c.r, c.g, c.b);
          M5.Led.display();
        }
        c.nscale8_video(b);
        leds[0] = c;
      } else {
        if (M5.Led.isEnabled()) {
          M5.Led.setBrightness(0);
          M5.Led.setAllColor(0, 0, 0);
          M5.Led.display();
        }
        leds[0] = CRGB(0, 0, 0);
      }
      FastLED.show();
    }
    return;
  }

  if (st.effect == LED_EFFECT_BREATHE) {
    float phase = (sin((double)millis() / 800.0) + 1.0) / 2.0;
    uint8_t scaled = (uint8_t)(phase * (float)b);
    if (M5.Led.isEnabled()) {
      M5.Led.setBrightness(scaled);
      M5.Led.setAllColor(c.r, c.g, c.b);
      M5.Led.display();
    }
    c.nscale8_video(scaled);
    leds[0] = c;
    FastLED.show();
    return;
  }

  if (st.effect == LED_EFFECT_RAINBOW) {
    uint8_t hue = (uint8_t)((millis() / 15) % 255);
    CRGB rgbRainbow = CHSV(hue, 255, b);
    if (M5.Led.isEnabled()) {
      M5.Led.setBrightness(b);
      M5.Led.setAllColor(rgbRainbow.r, rgbRainbow.g, rgbRainbow.b);
      M5.Led.display();
    }
    leds[0] = rgbRainbow;
    FastLED.show();
    return;
  }

  if (st.effect == LED_EFFECT_PATTERN && ledPatternCount > 0) {
    uint32_t now = millis();
    if (now - lastPatternStepTime >= ledPatternSteps[currentPatternIndex].delayMs) {
      currentPatternIndex = (currentPatternIndex + 1) % ledPatternCount;
      lastPatternStepTime = now;

      CRGB patColor = ledPatternSteps[currentPatternIndex].color;
      if (M5.Led.isEnabled()) {
        M5.Led.setBrightness(b);
        M5.Led.setAllColor(patColor.r, patColor.g, patColor.b);
        M5.Led.display();
      }
      patColor.nscale8_video(b);
      leds[0] = patColor;
      FastLED.show();
    }
    return;
  }

  if (M5.Led.isEnabled()) {
    M5.Led.setBrightness(b);
    M5.Led.setAllColor(c.r, c.g, c.b);
    M5.Led.display();
  }
  c.nscale8_video(b);
  leds[0] = c;
  FastLED.show();
}

void ledTask(void *pvParameters) {
  while (true) {
    renderLed();
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void resetCredentialsAndRestart() {
  clearAllCredentials();

  lvglLock();
  lv_label_set_text(ui_Label_info, "Credentials reset!");
  lvglUnlock();
  delay(1000);

  lvglLock();
  lv_label_set_text(ui_Label_info, "Restarting...");
  lvglUnlock();
  delay(1500);

  ESP.restart();
}

void handleEnterKey() {
  if (isAIThinking || chatTaskRunning) {
    return;
  }

  lvglLock();
  const char *inputText = lv_textarea_get_text(ui_TextArea_chat_input);
  if (inputText == nullptr || strlen(inputText) == 0) {
    lvglUnlock();
    return;
  }

  String prompt = String(inputText);
  prompt.trim();
  if (prompt.isEmpty()) {
    lvglUnlock();
    return;
  }

  if (prompt == "/puke" || prompt == "/shake") {
    lv_textarea_set_text(ui_TextArea_chat_input, "");
    lvglUnlock();
    triggerPukeEasterEgg();
    return;
  }

  lv_textarea_set_text(ui_TextArea_chat_input, "");
  appendToTranscript("> User:\n" + prompt + "\n");
  lv_textarea_set_text(ui_TextArea_AI_response, (conversationTranscript + "[Thinking...]\n").c_str());
  lv_textarea_set_cursor_pos(ui_TextArea_AI_response, LV_TEXTAREA_CURSOR_LAST);
  lvglUnlock();

  chatTaskRunning = true;
  String *pPrompt = new String(prompt);
  xTaskCreatePinnedToCore(
    chatWorkerTask,
    "chatWorkerTask",
    16384,
    pPrompt,
    1,
    &chatTaskHandle,
    1
  );
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
  if (ledMutex == NULL) return;

  if (xSemaphoreTake(ledMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    ledState.effect = effect;
    ledState.color = color;
    ledState.brightness = brightness;
    xSemaphoreGive(ledMutex);
  }
}

void setLedPatternFromJson(JsonArray steps) {
  if (ledMutex == NULL) return;

  if (xSemaphoreTake(ledMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    ledPatternCount = 0;
    for (JsonObject step : steps) {
      if (ledPatternCount >= MAX_LED_STEPS) break;
      uint8_t r = step["r"] | 0;
      uint8_t g = step["g"] | 0;
      uint8_t b = step["b"] | 0;
      uint16_t delayMs = step["delay_ms"] | 200;
      delayMs = constrain(delayMs, 50, 5000);

      ledPatternSteps[ledPatternCount].color = CRGB(r, g, b);
      ledPatternSteps[ledPatternCount].delayMs = delayMs;
      ledPatternCount++;
    }

    if (ledPatternCount > 0) {
      ledState.effect = LED_EFFECT_PATTERN;
      ledState.brightness = 100;
    }
    xSemaphoreGive(ledMutex);
  }
}

uint8_t parseLedEffectId(String effect) {
  effect.trim();
  effect.toLowerCase();

  if (effect == "off") return LED_EFFECT_OFF;
  if (effect == "solid") return LED_EFFECT_SOLID;
  if (effect == "blink") return LED_EFFECT_BLINK;
  if (effect == "breathe") return LED_EFFECT_BREATHE;
  if (effect == "rainbow") return LED_EFFECT_RAINBOW;
  if (effect == "pattern") return LED_EFFECT_PATTERN;

  return LED_EFFECT_SOLID;
}

void loop() {
  handleScreenTransitions();
  M5Cardputer.update();

  // IMU shake easter egg detection (3 seconds of continuous vigorous shaking)
  static uint32_t shakeStartTime = 0;
  static bool pukeRunning = false;
  if (!pukeRunning && M5.Imu.isEnabled()) {
    float ax = 0, ay = 0, az = 0;
    M5.Imu.getAccel(&ax, &ay, &az);
    float g = sqrt(ax * ax + ay * ay + az * az);
    float jerk = abs(g - 1.0f);
    if (jerk > 0.70f) {
      if (shakeStartTime == 0) shakeStartTime = millis();
      if (millis() - shakeStartTime >= 3000) {
        pukeRunning = true;
        triggerPukeEasterEgg();
        pukeRunning = false;
        shakeStartTime = 0;
      }
    } else {
      if (millis() - shakeStartTime > 400) {
        shakeStartTime = 0;
      }
    }
  }

  switch (currentScreen) {
    case UIScreen::GEMINI_CHAT:
      handleGeminiChatKeyboard();
      break;

    case UIScreen::MENU:
      handleMenuNavigation();
      break;

    case UIScreen::STATUS:
      handleWifiSetupKeyboard();
      break;

    default:
      break;
  }

  // KEY_OPT toggles MENU on/off cleanly with debounce
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isKeyPressed(KEY_OPT)) {
    if (currentScreen == UIScreen::MENU) {
      nextScreen = UIScreen::GEMINI_CHAT;
    } else if (currentScreen == UIScreen::GEMINI_CHAT) {
      nextScreen = UIScreen::MENU;
    }
  }

  // Language toast auto-hide timer
  if (langToastHideAt > 0 && millis() >= langToastHideAt) {
    lvglLock();
    if (langToastObj) lv_obj_add_flag(langToastObj, LV_OBJ_FLAG_HIDDEN);
    lvglUnlock();
    langToastHideAt = 0;
  }

  delay(5);
  yield();
}

void updateBattery() {
  int level = getBatteryPercentage();
  batteryPercentage = level;
  Serial.printf("[Power] Battery percentage: %d%%\n", batteryPercentage);
}

int getBatteryPercentage() {
  int level = M5.Power.getBatteryLevel();
  if (level >= 0) {
    return constrain(level, 0, 100);
  }

  float batteryVoltage = M5.Power.getBatteryVoltage() / 1000.0f;
  int p = (int)(((batteryVoltage - 3.3f) / (4.2f - 3.3f)) * 100.0f);
  return constrain(p, 0, 100);
}

void setScreenBrightness(int value) {
  value = constrain(value, 5, 255);
  currentScreenBrightness = value;
  M5.Display.setBrightness((uint8_t)value);
  Serial.printf("[DEV] Screen brightness set: %d\n", value);
}

CRGB parseLedColor(String s) {
  s.trim();
  s.toLowerCase();

  if (s.length() == 0) return CRGB(0, 0, 0);
  if (s[0] == '#') s = s.substring(1);

  if (s.length() == 6) {
    uint32_t hex = (uint32_t)strtol(s.c_str(), NULL, 16);
    uint8_t r = (hex >> 16) & 0xFF;
    uint8_t g = (hex >> 8) & 0xFF;
    uint8_t b = hex & 0xFF;
    return CRGB(r, g, b);
  }

  if (s == "red" || s == "красный") return CRGB(255, 0, 0);
  if (s == "green" || s == "зеленый" || s == "зелёный") return CRGB(0, 255, 0);
  if (s == "blue" || s == "синий") return CRGB(0, 0, 255);
  if (s == "white" || s == "белый") return CRGB(255, 255, 255);
  if (s == "black" || s == "off" || s == "черный" || s == "чёрный") return CRGB(0, 0, 0);
  if (s == "yellow" || s == "желтый" || s == "жёлтый") return CRGB(255, 255, 0);
  if (s == "cyan" || s == "голубой" || s == "бирюзовый") return CRGB(0, 255, 255);
  if (s == "magenta") return CRGB(255, 0, 255);
  if (s == "orange" || s == "оранжевый") return CRGB(255, 140, 0);
  if (s == "purple" || s == "фиолетовый") return CRGB(128, 0, 128);
  if (s == "pink" || s == "розовый") return CRGB(255, 105, 180);

  return CRGB(255, 255, 255);
}

void applyLedCommand(String params) {
  params.trim();
  if (params.length() == 0) return;

  int p1 = params.indexOf(';');
  String effectStr = (p1 < 0) ? params : params.substring(0, p1);
  effectStr.trim();

  LedState old = getLedState();
  String colorStr = "";
  uint8_t brightness = old.brightness;
  if (brightness == 0) brightness = 100;

  if (p1 >= 0) {
    int p2 = params.indexOf(';', p1 + 1);
    if (p2 < 0) {
      colorStr = params.substring(p1 + 1);
    } else {
      colorStr = params.substring(p1 + 1, p2);
      String b = params.substring(p2 + 1);
      b.trim();
      if (b.length() > 0) brightness = b.toInt();
    }
  }

  brightness = constrain(brightness, 0, 255);
  uint8_t effect = parseLedEffectId(effectStr);
  CRGB color = uint32ToCrgb(old.color);

  if (colorStr.length() > 0) {
    color = parseLedColor(colorStr);
  } else {
    if (old.color == 0) color = CRGB(255, 255, 255);
  }

  if (effect == LED_EFFECT_OFF) {
    setLedState(LED_EFFECT_OFF, 0, 0);
    return;
  }

  setLedState(effect, crgbToUint32(color), brightness);
  Serial.printf("[DEV] LED command applied: effect=%d brightness=%d\n", effect, brightness);
}

String parseDeviceCommands(String text) {
  while (true) {
    int startScreen = text.indexOf("[SCREEN:");
    int startBacklight = text.indexOf("[BACKLIGHT:");

    if (startScreen < 0 && startBacklight < 0) break;

    int start = startScreen;
    int tagLen = 8;
    if (startScreen < 0 || (startBacklight >= 0 && startBacklight < startScreen)) {
      start = startBacklight;
      tagLen = 11;
    }

    int end = text.indexOf(']', start);
    if (end < 0) break;

    String value = text.substring(start + tagLen, end);
    value.trim();
    setScreenBrightness(value.toInt());

    text = text.substring(0, start) + text.substring(end + 1);
  }

  while (true) {
    int start = text.indexOf("[LED:");
    if (start < 0) break;

    int end = text.indexOf(']', start);
    if (end < 0) break;

    String params = text.substring(start + 5, end);
    applyLedCommand(params);

    text = text.substring(0, start) + text.substring(end + 1);
  }

  String batteryValue = String(getBatteryPercentage()) + "%";

  while (true) {
    int start = text.indexOf("[BATTERY]");
    if (start < 0) break;
    text = text.substring(0, start) + batteryValue + text.substring(start + 9);
  }

  while (true) {
    int start = text.indexOf("[BATT]");
    if (start < 0) break;
    text = text.substring(0, start) + batteryValue + text.substring(start + 6);
  }

  int lastOpen = text.lastIndexOf('[');
  if (lastOpen >= 0 && text.indexOf(']', lastOpen) < 0) {
    text = text.substring(0, lastOpen);
  }

  while (text.indexOf("  ") >= 0) {
    text.replace("  ", " ");
  }

  text.trim();
  if (text.length() == 0) text = "OK";

  return text;
}