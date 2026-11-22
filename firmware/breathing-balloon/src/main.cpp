/*
 * 创客星辰·呼吸气球控制系统
 * 主控：ESP8266 D1 Mini
 * 功能：Wi-Fi热点、手机网页控制、手动充气、手动抽气和自动呼吸
 * 构建环境：PlatformIO + Arduino for ESP8266
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Servo.h>
#include "web_ui.h"

// ───────────────── 引脚定义 ─────────────────
static constexpr uint8_t PIN_C = 14;  // D5 = GPIO14  充气阀
static constexpr uint8_t PIN_X = 12;  // D6 = GPIO12  抽气阀
static constexpr uint8_t PIN_P = 15;  // D8 = GPIO15  真空泵

// ───────────────── 脉宽常量 ─────────────────
// 50Hz、20ms 周期；500μs = 关，2500μs = 开
static constexpr uint16_t PWM_OFF = 500;
static constexpr uint16_t PWM_ON  = 2500;

// ───────────────── Wi-Fi 设置 ─────────────────
static const char* AP_SSID     = "呼吸气球（密码8个8）";
static const char* AP_PASSWORD = "88888888";
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GW(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

// ───────────────── 呼吸默认值 ─────────────────
static constexpr unsigned long DEFAULT_INFLATE_MS = 2800;
static constexpr unsigned long DEFAULT_DEFLATE_MS = 2000;
static constexpr float DEFAULT_BREATHS_PER_MIN = 12.5f;
static constexpr float MIN_BREATHS_PER_MIN = 6.0f;
static constexpr float MAX_BREATHS_PER_MIN = 30.0f;

// ───────────────── 手动安全上限（固件本机硬超时） ─────────────────
static constexpr unsigned long MANUAL_MAX_RUN_MS = 12000;

// ───────────────── EEPROM ─────────────────
static constexpr uint32_t EEPROM_MAGIC   = 0x424C4E31;  // "BLN1"
static constexpr uint8_t  EEPROM_VERSION = 2;
static constexpr size_t   EEPROM_SIZE    = 4096;

struct __attribute__((packed)) Config {
  uint32_t magic;
  uint8_t  version;
  unsigned long inflate_ms;
  unsigned long deflate_ms;
};

// ───────────────── 运行模式 ─────────────────
enum Mode {
  MODE_STOPPED,
  MODE_MANUAL_INFLATE,
  MODE_MANUAL_DEFLATE,
  MODE_AUTO
};

// ───────────────── 自动呼吸子阶段 ─────────────────
enum AutoPhase {
  AUTO_INFLATE,
  AUTO_DEFLATE
};

// ───────────────── 全局状态 ─────────────────
static Config          cfg;
static Mode            mode            = MODE_STOPPED;
static AutoPhase       autoPhase       = AUTO_DEFLATE;
static unsigned long   stageStartMs    = 0;
static unsigned long   stageDurationMs = 0;

// 待生效参数
static bool            pendingUpdate    = false;
static unsigned long   pendingInflateMs = DEFAULT_INFLATE_MS;
static unsigned long   pendingDeflateMs = DEFAULT_DEFLATE_MS;

// 手动模式 12 秒独立计时
static unsigned long   manualStartMs    = 0;

// LED
static unsigned long   lastLedToggleMs = 0;
static bool            ledState        = false;

// ───────────────── 网络对象 ─────────────────
static ESP8266WebServer server(80);
static DNSServer        dnsServer;

// ───────────────── 舵机对象 ─────────────────
static Servo servoC;
static Servo servoX;
static Servo servoP;

// ───────────────── 辅助函数 ─────────────────

static unsigned long timeSince(unsigned long start) {
  return (unsigned long)(millis() - start);
}

static void loadConfig() {
  Config saved;
  EEPROM.get(0, saved);
  if (saved.magic == EEPROM_MAGIC && saved.version == EEPROM_VERSION) {
    cfg = saved;
    Serial.println(F("[EEPROM] 参数已从 Flash 加载"));
  } else {
    cfg.magic      = EEPROM_MAGIC;
    cfg.version    = EEPROM_VERSION;
    cfg.inflate_ms = DEFAULT_INFLATE_MS;
    cfg.deflate_ms = DEFAULT_DEFLATE_MS;
    Serial.println(F("[EEPROM] Flash 数据无效，已恢复默认参数"));
  }
}

static void saveConfig() {
  cfg.magic   = EEPROM_MAGIC;
  cfg.version = EEPROM_VERSION;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ───────────────── 硬件控制 ─────────────────

static void setDeflate() {
  // 抽气 / 气球变小
  servoC.writeMicroseconds(PWM_ON);
  servoX.writeMicroseconds(PWM_OFF);
  servoP.writeMicroseconds(PWM_ON);
}

static void setInflate() {
  // 充气 / 气球变大
  servoC.writeMicroseconds(PWM_OFF);
  servoX.writeMicroseconds(PWM_ON);
  servoP.writeMicroseconds(PWM_ON);
}

static void setStop() {
  // 停止：P 优先置 OFF，再关 C、X
  servoP.writeMicroseconds(PWM_OFF);
  servoC.writeMicroseconds(PWM_OFF);
  servoX.writeMicroseconds(PWM_OFF);
}

static const char* modeString() {
  switch (mode) {
    case MODE_MANUAL_INFLATE: return "manual_inflate";
    case MODE_MANUAL_DEFLATE: return "manual_deflate";
    case MODE_AUTO:           return "auto";
    default:                  return "stopped";
  }
}

// ───────────────── LED ─────────────────

static void updateLed() {
  switch (mode) {
    case MODE_STOPPED:
      digitalWrite(LED_BUILTIN, HIGH);  // 板载 LED 低电平点亮，HIGH = 灭
      break;
    case MODE_MANUAL_INFLATE:
    case MODE_MANUAL_DEFLATE:
      digitalWrite(LED_BUILTIN, LOW);   // 常亮
      break;
    case MODE_AUTO:
      if (timeSince(lastLedToggleMs) >= 500) {
        lastLedToggleMs = millis();
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
      }
      break;
  }
}

// ───────────────── 模式切换 ─────────────────

static void switchToStopped() {
  setStop();
  mode = MODE_STOPPED;
  pendingUpdate = false;
  manualStartMs = 0;
  Serial.println(F("[MODE] 停止"));
}

static void switchToManualInflate() {
  setInflate();
  mode = MODE_MANUAL_INFLATE;
  manualStartMs = millis();   // 全新 12 秒计时
  pendingUpdate = false;
  Serial.println(F("[MODE] 持续充气"));
}

static void switchToManualDeflate() {
  setDeflate();
  mode = MODE_MANUAL_DEFLATE;
  manualStartMs = millis();   // 全新 12 秒计时
  pendingUpdate = false;
  Serial.println(F("[MODE] 持续抽气"));
}

static void startAutoMode() {
  if (cfg.deflate_ms < 1 || cfg.inflate_ms < 1) return;

  // 应用待生效参数
  if (pendingUpdate) {
    cfg.inflate_ms = pendingInflateMs;
    cfg.deflate_ms = pendingDeflateMs;
    pendingUpdate = false;
  }

  autoPhase = AUTO_DEFLATE;
  setDeflate();
  mode = MODE_AUTO;
  stageStartMs = millis();
  stageDurationMs = cfg.deflate_ms;
  Serial.println(F("[MODE] 自动呼吸 - 开始抽气"));
}

// ───────────────── HTTP 处理：页面 ─────────────────

static void handleRoot() {
  String page(WEB_PAGE);
  page.replace("%SSID%", AP_SSID);
  server.send(200, "text/html", page);
}

static void handleFavicon() {
  server.send(204);
}

// ───────────────── Captive Portal 路由 ─────────────────

static void registerCaptiveRoutes() {
  // Android
  server.on("/generate_204", handleRoot);
  server.on("/gen_204", handleRoot);
  // iOS / macOS
  server.on("/hotspot-detect.html", handleRoot);
  server.on("/success.html", handleRoot);
  // Windows
  server.on("/connecttest.txt", handleRoot);
  server.on("/ncsi.txt", handleRoot);
  server.on("/fwlink", handleRoot);
  // 通用
  server.on("/favicon.ico", handleFavicon);
}

// ───────────────── HTTP 处理：API ─────────────────

static void handleStatus() {
  // 显示用户最新设定值（pending 优先，否则 cfg）
  unsigned long displayInflate = pendingUpdate ? pendingInflateMs : cfg.inflate_ms;
  unsigned long displayDeflate = pendingUpdate ? pendingDeflateMs : cfg.deflate_ms;
  float breatheMin = 0;
  if (displayInflate > 0 && displayDeflate > 0) {
    breatheMin = 60000.0f / (displayInflate + displayDeflate);
  }

  String json = "{";
  json += "\"mode\":\"" + String(modeString()) + "\"";
  json += ",\"inflate_ms\":" + String(displayInflate);
  json += ",\"deflate_ms\":" + String(displayDeflate);
  json += ",\"breaths_per_min\":" + String(breatheMin, 1);
  json += ",\"pendingUpdate\":" + String(pendingUpdate ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

static void handleControl() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"缺少请求体\"}");
    return;
  }

  String action = "";
  String body = server.arg("plain");
  int idxA = body.indexOf("\"action\"");
  if (idxA >= 0) {
    int q1 = body.indexOf('"', idxA + 8);
    int q2 = body.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) action = body.substring(q1 + 1, q2);
  }

  if (action == "stop") {
    switchToStopped();
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"stopped\"}");
  } else if (action == "inflate") {
    switchToManualInflate();
    server.send(200, "application/json",
      "{\"ok\":true,\"mode\":\"manual_inflate\"}");
  } else if (action == "deflate") {
    switchToManualDeflate();
    server.send(200, "application/json",
      "{\"ok\":true,\"mode\":\"manual_deflate\"}");
  } else if (action == "auto") {
    if (cfg.inflate_ms < 1 || cfg.deflate_ms < 1) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"充气和抽气时间必须大于0\"}");
      return;
    }
    startAutoMode();
    server.send(200, "application/json", "{\"ok\":true,\"mode\":\"auto\"}");
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"未知动作: " + action + "\"}");
  }
}

static void handleSettings() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"缺少请求体\"}");
    return;
  }

  String body = server.arg("plain");
  unsigned long newInflate = cfg.inflate_ms;
  unsigned long newDeflate = cfg.deflate_ms;

  // 提取 inflate_ms
  {
    int idx = body.indexOf("\"inflate_ms\"");
    if (idx >= 0) {
      int colon = body.indexOf(':', idx);
      if (colon >= 0) {
        int numStart = colon + 1;
        while (numStart < (int)body.length() && body[numStart] == ' ') numStart++;
        int numEnd = numStart;
        while (numEnd < (int)body.length() && body[numEnd] >= '0' && body[numEnd] <= '9') numEnd++;
        if (numEnd > numStart) newInflate = strtoul(body.substring(numStart, numEnd).c_str(), NULL, 10);
      }
    }
  }
  // 提取 deflate_ms
  {
    int idx = body.indexOf("\"deflate_ms\"");
    if (idx >= 0) {
      int colon = body.indexOf(':', idx);
      if (colon >= 0) {
        int numStart = colon + 1;
        while (numStart < (int)body.length() && body[numStart] == ' ') numStart++;
        int numEnd = numStart;
        while (numEnd < (int)body.length() && body[numEnd] >= '0' && body[numEnd] <= '9') numEnd++;
        if (numEnd > numStart) newDeflate = strtoul(body.substring(numStart, numEnd).c_str(), NULL, 10);
      }
    }
  }

  // 校验
  if (newInflate < 100 || newInflate > 60000) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"充气时间范围：0.1~60 秒\"}");
    return;
  }
  if (newDeflate < 100 || newDeflate > 60000) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"抽气时间范围：0.1~60 秒\"}");
    return;
  }

  // 运行中 → 待生效；停止中 → 立即生效
  if (mode == MODE_AUTO) {
    pendingInflateMs = newInflate;
    pendingDeflateMs = newDeflate;
    pendingUpdate = true;
  } else {
    cfg.inflate_ms = newInflate;
    cfg.deflate_ms = newDeflate;
    pendingUpdate = false;
  }

  server.send(200, "application/json",
    "{\"ok\":true,\"pendingUpdate\":" + String(pendingUpdate ? "true" : "false") +
    ",\"inflate_ms\":" + String(cfg.inflate_ms) +
    ",\"deflate_ms\":" + String(cfg.deflate_ms) + "}");
}

static void handleSave() {
  if (cfg.inflate_ms < 100 || cfg.deflate_ms < 100) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"参数无效，未保存\"}");
    return;
  }
  saveConfig();
  server.send(200, "application/json",
    "{\"ok\":true,\"inflate_ms\":" + String(cfg.inflate_ms) +
    ",\"deflate_ms\":" + String(cfg.deflate_ms) + "}");
}

static void handleNotFound() {
  // API 路径返回 JSON 错误，不被重定向吞掉
  if (server.uri().startsWith("/api/")) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"未找到: " + server.uri() + "\"}");
    return;
  }
  // 其他路径重定向到根（Captive Portal 兜底）
  server.sendHeader("Location", String("http://") + AP_IP.toString() + "/");
  server.send(302, "text/plain", "");
}

// ───────────────── 前向声明 ─────────────────
static void runAuto();

// ───────────────── setup() ─────────────────

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F("  创客星辰·呼吸气球控制系统"));
  Serial.println(F("  Wi-Fi AP + 网页控制 + 自动呼吸"));
  Serial.println(F("========================================"));

  // LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // 灭

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  // 舵机初始化 —— 脉宽范围 500~2500μs，初始 OFF
  servoC.attach(PIN_C, PWM_OFF, PWM_ON);
  servoX.attach(PIN_X, PWM_OFF, PWM_ON);
  servoP.attach(PIN_P, PWM_OFF, PWM_ON);
  setStop();

  // 默认停止
  mode = MODE_STOPPED;
  pendingUpdate = false;
  manualStartMs = 0;

  // Wi-Fi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print(F("[WiFi] 热点已启动: "));
  Serial.println(AP_SSID);
  Serial.print(F("[WiFi] 密码: "));
  Serial.println(AP_PASSWORD);
  Serial.print(F("[WiFi] 地址: "));
  Serial.println(AP_IP);

  // DNS（Captive Portal 通配解析）
  dnsServer.start(53, "*", AP_IP);

  // HTTP 路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/control",   HTTP_POST, handleControl);
  server.on("/api/settings",  HTTP_POST, handleSettings);
  server.on("/api/save",      HTTP_POST, handleSave);
  registerCaptiveRoutes();
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("[HTTP] 服务器已启动，端口 80"));
  Serial.println(F("[系统] 上电默认停止，等待网页命令"));
}

// ───────────────── loop() ─────────────────

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  updateLed();

  // ── 手动模式 12 秒硬超时（固件本机，不依赖网页） ──
  if (mode == MODE_MANUAL_INFLATE || mode == MODE_MANUAL_DEFLATE) {
    if (timeSince(manualStartMs) >= MANUAL_MAX_RUN_MS) {
      Serial.println(F("[保护] 手动连续运行 12 秒超时"));
      switchToStopped();
      return;
    }
  }

  // ── 状态机 ──
  switch (mode) {
    case MODE_STOPPED:
    case MODE_MANUAL_INFLATE:
    case MODE_MANUAL_DEFLATE:
      break;

    case MODE_AUTO:
      runAuto();
      break;
  }

  yield();  // 让出 CPU 给 Wi-Fi 协议栈
}

// ───────────────── 自动呼吸状态机 ─────────────────

static void runAuto() {
  unsigned long elapsed = timeSince(stageStartMs);

  if (elapsed < stageDurationMs) return;

  if (cfg.inflate_ms < 1 || cfg.deflate_ms < 1) {
    switchToStopped();
    return;
  }

  // ── 切换阶段 ──
  if (autoPhase == AUTO_DEFLATE) {
    // 抽气结束 → 充气
    autoPhase = AUTO_INFLATE;
    setInflate();
    stageStartMs = millis();
    stageDurationMs = cfg.inflate_ms;
    Serial.println(F("[呼吸] → 充气"));
  } else {
    // 充气结束 → 完整一轮呼吸结束，在此处应用 pending 参数
    if (pendingUpdate) {
      cfg.inflate_ms = pendingInflateMs;
      cfg.deflate_ms = pendingDeflateMs;
      pendingUpdate = false;
      Serial.println(F("[参数] 新参数已生效"));
    }
    autoPhase = AUTO_DEFLATE;
    setDeflate();
    stageStartMs = millis();
    stageDurationMs = cfg.deflate_ms;
    Serial.println(F("[呼吸] → 抽气"));
  }
}
