#ifndef WIFIMANAGERHANDLER_H
#define WIFIMANAGERHANDLER_H

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Arduino.h>

enum class UIScreen {
    LOADING,
    STATUS,
    GEMINI_CHAT,
    MENU,
    MODEL_SELECT,
    NONE
};

extern UIScreen nextScreen;

extern volatile bool wifiConnectedFlag;
extern Preferences preferences;
extern volatile bool apiKeySavedFlag;

bool saveApiKey(const String &apiKey);

void startWebServer();
String getStoredApiKey();
String getIPAddress();

bool saveSelectedModel(const String &modelName);
String getStoredModel();

bool saveWifiCredentials(const String &ssid, const String &password);
String getStoredWifiSSID();
String getStoredWifiPassword();

void clearAllCredentials();
bool isSDCardAvailable();

struct DeviceStateInfo {
  int screenBrightness;
  String ledEffect;
  String ledColorHex;
  int ledBrightness;
  int batteryPercentage;
  bool isThinking;
  String model;
  int wifiRssi;
  uint32_t freeHeap;
  uint32_t uptimeSec;
  bool sdMounted;
};

typedef std::function<String(const String &prompt)> WebChatHandler;
typedef std::function<DeviceStateInfo()> DeviceStateProvider;
typedef std::function<bool(const String &text)> RemoteInputHandler;
typedef std::function<String()> TranscriptProvider;
typedef std::function<void()> ShakeEasterEggTrigger;

void setWebChatHandler(WebChatHandler handler);
void setDeviceStateProvider(DeviceStateProvider provider);
void setRemoteInputHandler(RemoteInputHandler handler);
void setTranscriptProvider(TranscriptProvider provider);
void setShakeEasterEggTrigger(ShakeEasterEggTrigger trigger);

#endif