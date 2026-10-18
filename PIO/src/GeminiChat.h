#ifndef GEMINICHAT_H
#define GEMINICHAT_H

#include <Arduino.h>
#include <vector>
#include <functional>

struct ChatTurn {
  String role;
  String text;
};

struct GeminiModelInfo {
  String name;
  String displayName;
};

typedef std::function<String(const String &functionName, const String &argsJson)> DeviceFunctionHandler;
typedef std::function<void(const String &response)> ChatCompletionCallback;

class GeminiChat {
public:
  GeminiChat();

  void setApiKey(const String &apiKey);
  void initialize();

  void setModel(const String &modelName);
  String getModel() const;

  void setDeviceFunctionHandler(DeviceFunctionHandler handler);

  void resetHistory();
  size_t historySize() const;

  void sendMessageAsync(const String &message, ChatCompletionCallback callback = nullptr);
  String sendMessage(const String &message);

  std::vector<GeminiModelInfo> fetchAvailableModels();

private:
  String _apiKey;
  String _model;
  DeviceFunctionHandler _functionHandler;
  std::vector<ChatTurn> _history;
  static const size_t kMaxHistoryTurns = 20;

  static void sendMessageTask(void *param);

  void trimHistory();
};

#endif