#include "GeminiChat.h"
#include "WiFiManagerHandler.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <lvgl.h>

extern volatile bool isAIThinking;

struct AsyncChatTaskParams {
  GeminiChat *instance;
  String message;
  ChatCompletionCallback callback;
};

GeminiChat::GeminiChat() : _apiKey(""), _model("gemini-3.1-flash-lite") {}

void GeminiChat::setApiKey(const String &apiKey) {
  _apiKey = apiKey;
  _apiKey.trim();
}

void GeminiChat::initialize() {
  if (_apiKey.isEmpty()) {
    _apiKey = getStoredApiKey();
    _apiKey.trim();
  }

  String storedModel = getStoredModel();
  if (!storedModel.isEmpty() && storedModel != "gemini-1.5-flash") {
    setModel(storedModel);
  } else {
    setModel("gemini-3.1-flash-lite");
  }
}

void GeminiChat::setModel(const String &modelName) {
  _model = modelName;
  _model.trim();

  if (_model.startsWith("models/")) {
    _model = _model.substring(7);
  }
  if (_model.isEmpty() || _model == "gemini-1.5-flash") {
    _model = "gemini-3.1-flash-lite";
  }
}

String GeminiChat::getModel() const {
  return _model;
}

void GeminiChat::setDeviceFunctionHandler(DeviceFunctionHandler handler) {
  _functionHandler = handler;
}

void GeminiChat::resetHistory() {
  _history.clear();
}

size_t GeminiChat::historySize() const {
  return _history.size();
}

void GeminiChat::trimHistory() {
  while (_history.size() > kMaxHistoryTurns) {
    _history.erase(_history.begin());
  }
}

void GeminiChat::sendMessageTask(void *param) {
  auto *taskParams = static_cast<AsyncChatTaskParams*>(param);
  GeminiChat *instance = taskParams->instance;
  String message = taskParams->message;
  ChatCompletionCallback cb = taskParams->callback;

  String response = instance->sendMessage(message);

  if (cb) {
    cb(response);
  }

  delete taskParams;
  vTaskDelete(NULL);
}

void GeminiChat::sendMessageAsync(const String &message, ChatCompletionCallback callback) {
  auto *taskParams = new AsyncChatTaskParams{this, message, callback};
  xTaskCreatePinnedToCore(
    sendMessageTask,
    "GeminiChatTask",
    16384,
    taskParams,
    1,
    NULL,
    1
  );
}

String cleanResponse(String responseText) {
  responseText.replace("**", "");
  responseText.replace("* ", "");
  responseText.replace("*", " ");
  while (responseText.indexOf("  ") >= 0) {
    responseText.replace("  ", " ");
  }
  responseText.trim();
  return responseText;
}

static void addToolDeclarations(JsonObject &tools) {
  JsonArray decls = tools.createNestedArray("functionDeclarations");

  {
    JsonObject fn = decls.createNestedObject();
    fn["name"] = "set_screen_brightness";
    fn["description"] = "Sets the Cardputer screen/backlight brightness.";
    JsonObject params = fn.createNestedObject("parameters");
    params["type"] = "OBJECT";
    JsonObject props = params.createNestedObject("properties");
    JsonObject level = props.createNestedObject("brightness_percent");
    level["type"] = "INTEGER";
    level["description"] = "Brightness from 1 to 100 percent.";
    JsonArray req = params.createNestedArray("required");
    req.add("brightness_percent");
  }

  {
    JsonObject fn = decls.createNestedObject();
    fn["name"] = "set_led_effect";
    fn["description"] = "Controls the onboard RGB status LED (the 'lamp').";
    JsonObject params = fn.createNestedObject("parameters");
    params["type"] = "OBJECT";
    JsonObject props = params.createNestedObject("properties");
    JsonObject effect = props.createNestedObject("effect");
    effect["type"] = "STRING";
    effect["description"] = "One of: off, solid, blink, breathe, rainbow.";
    JsonObject color = props.createNestedObject("color");
    color["type"] = "STRING";
    color["description"] = "Color name (red, green, blue, white, cyan, yellow, pink, orange, ...) or #RRGGBB hex.";
    JsonObject brightness = props.createNestedObject("brightness_percent");
    brightness["type"] = "INTEGER";
    brightness["description"] = "LED brightness from 0 to 100.";
    JsonArray req = params.createNestedArray("required");
    req.add("effect");
  }

  {
    JsonObject fn = decls.createNestedObject();
    fn["name"] = "generate_led_pattern";
    fn["description"] =
      "Plays a custom color animation on the LED using RGB frames and duration in milliseconds.";
    JsonObject params = fn.createNestedObject("parameters");
    params["type"] = "OBJECT";
    JsonObject props = params.createNestedObject("properties");
    JsonObject steps = props.createNestedObject("steps");
    steps["type"] = "ARRAY";
    steps["description"] =
      "2 to 16 frames. Each frame: {r:0-255, g:0-255, b:0-255, delay_ms:50-2000}.";
    JsonObject items = steps.createNestedObject("items");
    items["type"] = "OBJECT";
    JsonObject itemProps = items.createNestedObject("properties");
    itemProps.createNestedObject("r")["type"] = "INTEGER";
    itemProps.createNestedObject("g")["type"] = "INTEGER";
    itemProps.createNestedObject("b")["type"] = "INTEGER";
    itemProps.createNestedObject("delay_ms")["type"] = "INTEGER";
    JsonArray req = params.createNestedArray("required");
    req.add("steps");
  }

  {
    JsonObject fn = decls.createNestedObject();
    fn["name"] = "get_battery_percentage";
    fn["description"] = "Returns the current battery charge percentage (0-100).";
    JsonObject params = fn.createNestedObject("parameters");
    params["type"] = "OBJECT";
    params.createNestedObject("properties");
  }
}

String GeminiChat::sendMessage(const String &message) {
  isAIThinking = true;

  if (WiFi.status() != WL_CONNECTED) {
    isAIThinking = false;
    Serial.println("[Gemini Error] Wi-Fi is not connected.");
    return "Error: Wi-Fi is not connected.";
  }

  if (_apiKey.isEmpty()) {
    _apiKey = getStoredApiKey();
    _apiKey.trim();
  }

  if (_apiKey.isEmpty()) {
    isAIThinking = false;
    return "API key not set.\nConfigure via Web UI or config.json.";
  }

  _apiKey.trim();
  _apiKey.replace("\r", "");
  _apiKey.replace("\n", "");
  _apiKey.replace(" ", "");

  String model = _model.isEmpty() || _model == "gemini-1.5-flash" ? "gemini-3.1-flash-lite" : _model;
  if (model.startsWith("models/")) {
    model = model.substring(7);
  }

  String url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += model;
  url += ":generateContent?key=";
  url += _apiKey;

  DynamicJsonDocument jsonRequest(8192);

  JsonObject systemInstruction = jsonRequest.createNestedObject("systemInstruction");
  JsonArray sysParts = systemInstruction.createNestedArray("parts");
  JsonObject sysPart = sysParts.createNestedObject();
  sysPart["text"] =
    "У тебя крошечный экран 240x135, где помещается около 96 букв. "
    "Отвечай максимально коротко, емко и без воды. "
    "Длинные ответы давай только если пользователь прямо об этом попросит. "
    "Ты помнишь предыдущие сообщения в этом диалоге. "
    "Для управления устройством (яркость экрана, светодиод-лампа, кастомные "
    "цветовые анимации, заряд батареи) НЕ пиши текстовые теги в видимом "
    "ответе — вместо этого вызывай соответствующую функцию (function call). "
    "Вызывай функцию только когда пользователь явно просит изменить яркость, "
    "светодиод/лампу или узнать заряд батареи. "
    "После вызова функции коротко подтверди результат обычным текстом.";

  JsonObject tools = jsonRequest.createNestedObject("tools");
  addToolDeclarations(tools);

  JsonArray contents = jsonRequest.createNestedArray("contents");

  for (const ChatTurn &turn : _history) {
    JsonObject c = contents.createNestedObject();
    c["role"] = turn.role;
    JsonArray parts = c.createNestedArray("parts");
    parts.createNestedObject()["text"] = turn.text;
  }

  {
    JsonObject c = contents.createNestedObject();
    c["role"] = "user";
    JsonArray parts = c.createNestedArray("parts");
    parts.createNestedObject()["text"] = message;
  }

  JsonObject genConfig = jsonRequest.createNestedObject("generationConfig");
  genConfig["maxOutputTokens"] = 350;
  JsonObject thinkConfig = genConfig.createNestedObject("thinkingConfig");
  thinkConfig["thinkingLevel"] = "minimal";

  String textResponse = "";
  String lastFunctionResult = "";
  bool done = false;
  const int maxFunctionCallRounds = 3;

  for (int round = 0; round < maxFunctionCallRounds && !done; round++) {
    String requestBody;
    serializeJson(jsonRequest, requestBody);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(25000);

    Serial.printf("[Gemini] Round %d Request -> %s\n", round, model.c_str());

    if (!https.begin(client, url)) {
      Serial.println("[Gemini Error] Failed to initialize HTTPS connection.");
      isAIThinking = false;
      return "Connection to Gemini API failed.";
    }

    https.addHeader("Content-Type", "application/json");
    int httpResponseCode = https.POST(requestBody);

    if (httpResponseCode != HTTP_CODE_OK) {
      String errorPayload = https.getString();
      Serial.printf("[Gemini Error] HTTP Code: %d\nPayload: %s\n", httpResponseCode, errorPayload.c_str());
      https.end();
      isAIThinking = false;
      if (httpResponseCode == 404) {
        return "Model " + model + " not found (404). Check model name in Settings.";
      } else if (httpResponseCode == 400) {
        return "Invalid API Key or request error (400).";
      } else if (httpResponseCode == 429) {
        return "API rate limit reached (429). Try again shortly.";
      }
      return "HTTP Error: " + String(httpResponseCode);
    }

    String response = https.getString();
    https.end();

    DynamicJsonDocument jsonResponse(6144);
    DeserializationError error = deserializeJson(jsonResponse, response);

    if (error) {
      Serial.print("[Gemini Error] JSON parse failed: ");
      Serial.println(error.c_str());
      isAIThinking = false;
      return "Response parse failed.";
    }

    if (!jsonResponse.containsKey("candidates") || jsonResponse["candidates"].isNull() ||
        jsonResponse["candidates"].size() == 0) {
      done = true;
      break;
    }

    JsonObject candidate = jsonResponse["candidates"][0];
    if (!candidate.containsKey("content") || !candidate["content"].containsKey("parts")) {
      done = true;
      break;
    }

    JsonArray respParts = candidate["content"]["parts"];
    bool hasFunctionCall = false;

    for (JsonObject respPart : respParts) {
      if (respPart.containsKey("functionCall")) {
        hasFunctionCall = true;
        break;
      }
    }

    if (hasFunctionCall) {
      // 1. Preserve entire model turn with thought_signature
      String candidateContentStr;
      serializeJson(candidate["content"], candidateContentStr);
      DynamicJsonDocument modelDoc(4096);
      deserializeJson(modelDoc, candidateContentStr);
      JsonObject modelTurn = contents.createNestedObject();
      modelTurn.set(modelDoc.as<JsonObject>());

      // 2. Execute each function call and add function response turns
      for (JsonObject respPart : respParts) {
        if (respPart.containsKey("functionCall")) {
          JsonObject fc = respPart["functionCall"];
          String rawFnName = fc["name"].as<String>();
          String cleanFnName = rawFnName;
          if (cleanFnName.indexOf(':') >= 0) {
            cleanFnName = cleanFnName.substring(cleanFnName.indexOf(':') + 1);
          }

          String argsJson = "";
          if (fc.containsKey("args")) {
            serializeJson(fc["args"], argsJson);
          }
          if (argsJson.isEmpty()) {
            argsJson = "{}";
          }

          Serial.printf("[Gemini] functionCall: %s args=%s\n", cleanFnName.c_str(), argsJson.c_str());

          String result = "ok";
          if (_functionHandler) {
            result = _functionHandler(cleanFnName, argsJson);
          } else {
            result = "no handler registered";
          }
          lastFunctionResult = result;

          JsonObject fnTurn = contents.createNestedObject();
          fnTurn["role"] = "function";
          JsonArray fnParts = fnTurn.createNestedArray("parts");
          JsonObject fnResponsePart = fnParts.createNestedObject();
          JsonObject fnResponse = fnResponsePart.createNestedObject("functionResponse");
          fnResponse["name"] = rawFnName;
          JsonObject fnResponseBody = fnResponse.createNestedObject("response");
          fnResponseBody["output"] = result;
        }
      }
    } else {
      for (JsonObject respPart : respParts) {
        if (respPart.containsKey("text")) {
          textResponse = respPart["text"].as<String>();
        }
      }
    }

    jsonResponse.clear();

    if (!hasFunctionCall) {
      done = true;
    }
  }

  if (textResponse.isEmpty()) {
    if (!lastFunctionResult.isEmpty()) {
      textResponse = lastFunctionResult;
    } else {
      textResponse = "OK";
    }
  }

  textResponse = cleanResponse(textResponse);

  const int maxResponseLength = 700;
  if (textResponse.length() > maxResponseLength) {
    textResponse = textResponse.substring(0, maxResponseLength) + "...";
  }

  _history.push_back({"user", message});
  _history.push_back({"model", textResponse});
  trimHistory();

  jsonRequest.clear();
  isAIThinking = false;
  return textResponse;
}

std::vector<GeminiModelInfo> GeminiChat::fetchAvailableModels() {
  std::vector<GeminiModelInfo> result;

  if (_apiKey.isEmpty()) {
    _apiKey = getStoredApiKey();
    _apiKey.trim();
  }
  if (_apiKey.isEmpty() || WiFi.status() != WL_CONNECTED) {
    return result;
  }

  String url = "https://generativelanguage.googleapis.com/v1beta/models?key=" + _apiKey;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.setTimeout(15000);

  if (!https.begin(client, url)) {
    Serial.println("[Gemini] Failed to init HTTP for model list.");
    return result;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Gemini] Model list HTTP error: %d\n", code);
    https.end();
    return result;
  }

  DynamicJsonDocument filter(1024);
  filter["models"][0]["name"] = true;
  filter["models"][0]["displayName"] = true;
  filter["models"][0]["supportedGenerationMethods"] = true;

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();

  if (error) {
    Serial.print("[Gemini] Model list JSON parse failed: ");
    Serial.println(error.c_str());
    return result;
  }

  if (!doc.containsKey("models")) {
    return result;
  }

  static const char *kExcludedSubstrings[] = {
    "-tts", "-image", "veo", "lyria", "embedding", "robotics"
  };

  for (JsonObject m : doc["models"].as<JsonArray>()) {
    String name = m["name"].as<String>();
    String lowerName = name;
    lowerName.toLowerCase();

    bool excluded = false;
    for (const char *sub : kExcludedSubstrings) {
      if (lowerName.indexOf(sub) >= 0) {
        excluded = true;
        break;
      }
    }
    if (excluded) continue;

    bool hasGenerateContent = false;
    if (m.containsKey("supportedGenerationMethods")) {
      for (JsonVariant method : m["supportedGenerationMethods"].as<JsonArray>()) {
        if (String(method.as<const char*>()) == "generateContent") {
          hasGenerateContent = true;
          break;
        }
      }
    }
    if (!hasGenerateContent) continue;

    GeminiModelInfo info;
    info.name = name;
    info.displayName = m.containsKey("displayName") ? m["displayName"].as<String>() : name;
    result.push_back(info);
  }

  return result;
}