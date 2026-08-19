#include "GeminiChat.h"
#include "WiFiManagerHandler.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <lvgl.h>

extern volatile bool isAIThinking;

GeminiChat::GeminiChat() : _apiKey("") {}

void GeminiChat::setApiKey(const String &apiKey) {
  _apiKey = apiKey;
  _apiKey.trim();
}

void GeminiChat::initialize() {
  if (_apiKey.isEmpty()) {
    _apiKey = getStoredApiKey();
    _apiKey.trim();
  }
}

void GeminiChat::sendMessageTask(void *param) {
  auto *taskParams = static_cast<std::pair<GeminiChat*, String>*>(param);
  GeminiChat *instance = taskParams->first;
  String message = taskParams->second;
  instance->sendMessage(message);
  delete taskParams;
  vTaskDelete(NULL);
}

void GeminiChat::sendMessageAsync(const String &message) {
  auto *taskParams = new std::pair<GeminiChat*, String>(this, message);
  xTaskCreatePinnedToCore(
    sendMessageTask,
    "GeminiChatTask",
    4096,
    taskParams,
    1,
    NULL,
    1
  );
}

// Function to clean unwanted characters from response text
String cleanResponse(String responseText) {
  responseText.replace("**", "");
  responseText.replace("* ", "");
  responseText.replace("*", " ");
  // НЕ удаляем '#', он нужен для hex-цветов в LED-командах:
  // [LED:solid;#FF0000;120]
  // responseText.replace("#", "");
  responseText.replace("  ", "");
  return responseText;
}

String GeminiChat::sendMessage(const String &message) {
  isAIThinking = true;

  if (_apiKey.isEmpty()) {
    _apiKey = getStoredApiKey();
    _apiKey.trim();
  }

  if (_apiKey.isEmpty()) {
    isAIThinking = false;
    return "API key not set. Configure via Web UI or config.json.";
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(30000);

  const char* MODEL = "gemini-3.5-flash-lite";

  _apiKey.trim();
  _apiKey.replace("\r", "");
  _apiKey.replace("\n", "");
  _apiKey.replace(" ", "");

  String url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += MODEL;
  url += ":generateContent?key=";
  url += _apiKey;

  Serial.print("[Gemini] Requesting URL: ");
  Serial.println(url);

  if (!https.begin(client, url)) {
    Serial.println("[Gemini Error] Failed to initialize HTTP connection.");
    Serial.flush();
    isAIThinking = false;
    return "Failed to connect to Gemini API.";
  }

  https.addHeader("Content-Type", "application/json");

  DynamicJsonDocument jsonRequest(2048);

  JsonObject systemInstruction = jsonRequest.createNestedObject("systemInstruction");
  JsonArray sysParts = systemInstruction.createNestedArray("parts");
  JsonObject sysPart = sysParts.createNestedObject();

  sysPart["text"] =
    "У тебя крошечный экран 240x135, где помещается около 96 букв. "
    "Отвечай максимально коротко, емко и без воды. "
    "Длинные ответы давай только если пользователь прямо об этом попросит. "
    "Ты можешь управлять устройством с помощью скрытых тегов. "
    "Они выполняются прошивкой и не показываются пользователю. "
    "Форматы команд: "
    "[SCREEN:0-255] или [BACKLIGHT:0-255] - яркость экрана. "
    "[LED:off|solid|blink|breathe|rainbow;цвет;0-255] - встроенный светодиод. "
    "Цвет: #RRGGBB или red, green, blue, yellow, cyan, magenta, white, orange, purple, black. "
    "[BATTERY] - прошивка заменит этот тег на текущий процент заряда батареи. "
    "Если пользователь называет яркость в процентах, переводи проценты в 0-255. "
    "Например, 50% примерно равно 128. "
    "Используй теги только когда пользователь явно просит изменить яркость экрана, светодиод или показать заряд батареи. "
    "Не объясняй синтаксис тегов и не показывай их в видимом тексте.";

  JsonArray contents = jsonRequest.createNestedArray("contents");
  JsonObject content = contents.createNestedObject();
  JsonArray parts = content.createNestedArray("parts");
  JsonObject part = parts.createNestedObject();
  part["text"] = message;

  JsonObject genConfig = jsonRequest.createNestedObject("generationConfig");
  genConfig["maxOutputTokens"] = 220;

  String requestBody;
  serializeJson(jsonRequest, requestBody);
  jsonRequest.clear();

  Serial.print("[Gemini] Sending payload size: ");
  Serial.println(requestBody.length());

  int httpResponseCode = https.POST(requestBody);

  if (httpResponseCode != HTTP_CODE_OK) {
    String errorPayload = https.getString();
    Serial.printf("[Gemini Error] HTTP Code: %d\n", httpResponseCode);
    Serial.print("[Gemini Error] Response: ");
    Serial.println(errorPayload);
    Serial.flush();
    https.end();
    isAIThinking = false;
    return "HTTP Error: " + String(httpResponseCode) + " (Check Serial)";
  }

  String response = https.getString();
  https.end();

  Serial.print("[Gemini] Success! Response length: ");
  Serial.println(response.length());

  DynamicJsonDocument jsonResponse(4096);
  DeserializationError error = deserializeJson(jsonResponse, response);

  if (error) {
    Serial.print("[Gemini Error] JSON parse failed: ");
    Serial.println(error.c_str());
    Serial.flush();
    isAIThinking = false;
    return "JSON parsing failed";
  }

  String textResponse = "Unexpected response format or no candidates found.";

  if (jsonResponse.containsKey("candidates") && !jsonResponse["candidates"].isNull()) {
    JsonArray candidates = jsonResponse["candidates"];
    if (candidates.size() > 0) {
      JsonObject candidate = candidates[0];
      if (candidate.containsKey("content") && candidate["content"].containsKey("parts")) {
        JsonArray respParts = candidate["content"]["parts"];
        if (respParts.size() > 0) {
          textResponse = respParts[0]["text"].as<String>();
          textResponse = cleanResponse(textResponse);

          const int maxResponseLength = 700;
          if (textResponse.length() > maxResponseLength) {
            textResponse = textResponse.substring(0, maxResponseLength) + "...";
          }

          // Отключено: ответ сначала должен быть обработан в main.cpp
          // через parseDeviceCommands(), чтобы выполнить скрытые команды
          // и удалить теги из видимого текста.
          // #ifdef ui_TextArea_AI_response
          // lv_textarea_set_text(ui_TextArea_AI_response, textResponse.c_str());
          // #endif
        }
      }
    }
  }

  jsonResponse.clear();
  isAIThinking = false;
  return textResponse;
}