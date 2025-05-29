#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <BleCombo.h>
#include "WebServer.h"
#include "SD_MMC.h"
#include "esp_camera.h"

// ——— Configuration ———
const char* SSID               = "SSID";
const char* PASSWORD           = "PASSWD";
const char* STATUS_URL         = "http://192.168.1.30:5678/status";
const int   MAX_SERVER_RETRIES = 5;
const int   RETRY_DELAY_MS     = 3000;
const int   CYCLE_DELAY_MS     = 15000;
IPAddress local_IP(192, 168, 1, 20);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);


//Auto Start
#define AUTOSTART_CONFIG   "/configs/autostart.txt"
static const char* ENDPOINTS_TABLE = "/server/endpoints";

// Pins
const int RELAY_PIN  = 3;  // LOW = ON

// BLE HID
BleComboKeyboard keyboard("cheese-ble-hid", "Espressif", 100);

void logMsg(const char *msg);

// Server-poll state
unsigned long lastCheck   = 0;
int           retryCount  = 0;
bool          gotResponse = false;
int           serverValue = -1;
volatile bool scriptRunning = false;
volatile bool commandRunning = false;


// Camera (AI-Thinker ESP32-CAM) pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

camera_config_t camera_config;
int frameSize = FRAMESIZE_SVGA;
int fpsValue  = 10;

bool AutoCharge = true;
bool AutoRestart = true;
bool Camera = false;
bool Charge = false;
// Web server
WebServer server(80);

//script
using ArgList = std::vector<String>;
using CmdFn   = std::function<void(const ArgList&)>;

// Command dispatch table
std::map<String,CmdFn> commands;


// Forward declarations
void connectWiFi();
bool isServerUp(int &statusValue);
void sendRecoverySequence();
void deinitAllExceptBle();
void initCamera(int resolution);
void handleRoot();
void handleControl();
void handleStream();
void handleOpenFile();
void handleRestart();
void handleRestartTermux();
void handleFileManagerPage();
void handleList();
void handleDelete();
void handleRename();
void handleMkdir();
void handleUpload();
void handleNotFound();
void handleLogs();
void handleEnableCamera();
void handleDisableCamera();
void handleEnableAutoRestart();
void handleDisableAutoRestart();
void handleEnableAutoCharge();
void handleDisableAutoCharge();
void handleGetStatus();
void handleEnableChargeServer();
void handleDisableChargeServer();
void handleRunScript();
void handleListApps();
void loadAutoStart();
void handleAutoStartAdd();
void handleAutoStartRemove();

String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".htm"))  return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".mjs"))  return "application/javascript"; // ES6 modules
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".xml"))  return "application/xml";
  if (path.endsWith(".txt"))  return "text/plain";
  if (path.endsWith(".csv"))  return "text/csv";
  if (path.endsWith(".tsv"))  return "text/tab-separated-values";

  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".gif"))  return "image/gif";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".webp")) return "image/webp";
  if (path.endsWith(".bmp"))  return "image/bmp";
  if (path.endsWith(".apng")) return "image/apng";

  if (path.endsWith(".mp4"))  return "video/mp4";
  if (path.endsWith(".webm")) return "video/webm";
  if (path.endsWith(".ogg"))  return "video/ogg";
  if (path.endsWith(".mp3"))  return "audio/mpeg";
  if (path.endsWith(".wav"))  return "audio/wav";
  if (path.endsWith(".opus")) return "audio/opus";

  if (path.endsWith(".pdf"))  return "application/pdf";
  if (path.endsWith(".wasm")) return "application/wasm";
  if (path.endsWith(".zip"))  return "application/zip";
  if (path.endsWith(".rar"))  return "application/vnd.rar";

  // fallback
  return "application/octet-stream";
}


void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  if (!SD_MMC.begin()) {
    logMsg("SD_MMC Mount Failed");
  } else {
    logMsg("SD_MMC Mounted");
  }

  connectWiFi();

  ArduinoOTA
    .onStart([]() { logMsg("  OTA Start"); })
    .onEnd([]() { logMsg("  OTA End"); })
    .onError([](ota_error_t error) {
      if (error == OTA_AUTH_ERROR) logMsg("OTA Error: Auth Failed");
      else if (error == OTA_BEGIN_ERROR) logMsg("OTA Error: Begin Failed");
      else if (error == OTA_CONNECT_ERROR) logMsg("OTA Error: Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) logMsg("OTA Error: Receive Failed");
      else if (error == OTA_END_ERROR) logMsg("OTA Error: End Failed");
    });
  ArduinoOTA.begin();

  initCamera(frameSize);

  server.on("/",               HTTP_GET, handleRoot);
  server.on("/control",        HTTP_GET, handleControl);
  server.on("/stream",         HTTP_GET, handleStream);
  server.on("/filemanager.html", HTTP_GET, handleFileManagerPage);
  server.on("/logs.html", HTTP_GET, handleLogsPage);
  server.on("/list",           HTTP_GET, handleList);
  server.on("/delete",         HTTP_GET, handleDelete);
  server.on("/open", HTTP_GET, handleOpenFile);
  server.on("/restart", HTTP_GET, handleRestart);
  server.on("/restartserver", HTTP_GET, handleRestartTermux);
  server.on("/rename", HTTP_GET, handleRename);
  server.on("/mkdir",          HTTP_GET, handleMkdir);
  server.on("/upload", HTTP_POST, [](){}, handleUpload);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/disableautocharge", HTTP_GET, handleDisableAutoCharge);
  server.on("/enableautocharge", HTTP_GET, handleEnableAutoCharge);
  server.on("/disableautorestart", HTTP_GET, handleDisableAutoRestart);
  server.on("/enableautorestart", HTTP_GET, handleEnableAutoRestart);
  server.on("/enablecamera", HTTP_GET,  handleEnableCamera);
  server.on("/disablecamera", HTTP_GET, handleDisableCamera);
  server.on("/disablecharge", HTTP_GET, handleDisableChargeServer);
  server.on("/enablecharge", HTTP_GET, handleEnableChargeServer);
  server.on("/getstatus", HTTP_GET, handleGetStatus);
  server.on("/runscript", HTTP_GET, handleRunScript);
  server.on("/runcommand", HTTP_GET, handleRunCommand);
  server.on("/listapps", HTTP_GET, handleListApps);
  server.on("/getautostart", HTTP_GET,  handleGetAutoStart);
  // Inside setup(), alongside your other server.on() calls:
  server.on("/autostart/add",    HTTP_GET, handleAutoStartAdd);
  server.on("/autostart/remove", HTTP_GET, handleAutoStartRemove);


  server.on("/clearlogs", HTTP_GET, []() {
    SD_MMC.remove("/logs.txt");
    logMsg("Logs manually cleared");
    server.send(200, "text/plain", "Logs cleared");
  });
    server.on("/download", HTTP_GET, [](){
    String path = server.arg("path");
    if (!SD_MMC.exists(path)) {
      server.send(404, "text/plain", "Not Found");
      return;
    }
    File f = SD_MMC.open(path, FILE_READ);
    String fn = path;
    int slash = fn.lastIndexOf('/');
    if (slash >= 0) fn = fn.substring(slash + 1);
    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"" + fn + "\"");
    server.streamFile(f, "application/octet-stream");
    f.close();
  });
  server.onNotFound(handleNotFound);
  server.begin();
  lastCheck = millis();
  logMsg("HTTP server started");
  initScriptEngine();
  loadAutoStart();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (AutoCharge){
    if (millis() - lastCheck >= CYCLE_DELAY_MS) {
      lastCheck = millis();
      retryCount = 0;
      gotResponse = false;
      while (retryCount < MAX_SERVER_RETRIES) {
        if (isServerUp(serverValue)) { gotResponse = true; break; }
        retryCount++;
        delay(RETRY_DELAY_MS);
      }
      if (gotResponse) {
        if (serverValue == 1) {
          digitalWrite(RELAY_PIN, LOW);
        } else {
          digitalWrite(RELAY_PIN, HIGH);
        }
      } else {
        if(AutoRestart){
          logMsg("Server unreachable   BLE recovery");
          if(WiFi.status() == WL_CONNECTED){
          sendRecoverySequence();
          }else{
            logMsg("Skipping Recovery Sequence as WiFi is not connected!");
          }
        }else{
          logMsg("Recovery Disabled Skipping..");
        }
      }
    }
  }else{
    if(Charge){
      digitalWrite(RELAY_PIN, LOW);
    }else{
      digitalWrite(RELAY_PIN, HIGH);
    }
  }
}

bool parseLine(const String& line, String& name, ArgList& args) {
  auto p = line.indexOf('(');
  auto q = line.lastIndexOf(')');
  if (p < 1 || q <= p) return false;
  name = line.substring(0, p);
  String body = line.substring(p+1, q);
  args.clear();
  int start = 0;
  for (;;) {
    int comma = body.indexOf(',', start);
    if (comma < 0) {
      String a = body.substring(start);
      a.trim(); if (a.length()) args.push_back(a);
      break;
    }
    String a = body.substring(start, comma);
    a.trim(); if (a.length()) args.push_back(a);
    start = comma + 1;
  }
  return true;
}

void initScriptEngine(){
  commands["logmsg"]      = cmd_logMsg;
  commands["httpget"]     = cmd_httpGet;
  commands["httppost"]    = cmd_httpPost;
  commands["setpin"]      = cmd_setPin;
  commands["readpin"] = cmd_readpin;
  commands["delayms"]     = cmd_delayMs;
  commands["writedata"]   = cmd_writeData;
  commands["listcmds"]    = cmd_listcmds;   // ← new
  commands["clearlogs"] = cmd_clearlogs;
  commands["pinmode"] = cmd_pinmode;
  commands["serverbind"] = cmd_serverBind;
  commands["serverunbind"] = cmd_serverUnbind;
  commands["rm"] = cmd_rm;
  // … add your own here …
}

void loadAutoStart() {
  if (!SD_MMC.exists(AUTOSTART_CONFIG)) {
    logMsg("AutoStart config not found");
    return;
  }

  File cfg = SD_MMC.open(AUTOSTART_CONFIG, FILE_READ);
  if (!cfg) {
    logMsg("Failed to open autostart.txt");
    return;
  }

  while (cfg.available()) {
    String line = cfg.readStringUntil('\n');
    line.trim();
    // skip blank lines or comments
    if (line.length() == 0 || line.startsWith("//")) 
      continue;

    // runScript expects the path to the .run file
    logMsg(("AutoStarted: " + line).c_str());
    runScript("/apps/" + line);
  }

  cfg.close();
}


// Helper to read current list
static std::vector<String> readAutoStartList() {
  std::vector<String> lines;
  const char* cfg = "/configs/autostart.txt";
  File f = SD_MMC.open(cfg, FILE_READ);
  if (!f) return lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0 && !line.startsWith("//")) {
      lines.push_back(line);
    }
  }
  f.close();
  return lines;
}

// Helper to write back list
static bool writeAutoStartList(const std::vector<String>& lines) {
  File f = SD_MMC.open("/configs/autostart.txt", FILE_WRITE);
  if (!f) return false;
  for (auto& l : lines) {
    f.println(l);
  }
  f.close();
  return true;
}

// POST /autostart/add?script=/apps/foo.run
void handleAutoStartAdd() {
  if (!server.hasArg("script")) {
    server.send(400, "text/plain", "missing script");
    return;
  }
  String script = server.arg("script");
  auto list = readAutoStartList();
  // only add if not present
  if (std::find(list.begin(), list.end(), script) == list.end()) {
    list.push_back(script);
    if (!writeAutoStartList(list)) {
      server.send(500, "text/plain", "write failed");
      return;
    }
  }
  server.send(200, "text/plain", "ok");
}

// GET /autostart/remove?script=/apps/foo.run
void handleAutoStartRemove() {
  if (!server.hasArg("script")) {
    server.send(400, "text/plain", "missing script");
    return;
  }
  String script = server.arg("script");
  auto list = readAutoStartList();
  // remove any matches
  auto it = std::remove(list.begin(), list.end(), script);
  if (it != list.end()) {
    list.erase(it, list.end());
    if (!writeAutoStartList(list)) {
      server.send(500, "text/plain", "write failed");
      return;
    }
  }
  server.send(200, "text/plain", "ok");
}


// GET /getautostart
void handleGetAutoStart() {
  const char* cfg = "/configs/autostart.txt";
  String json = "[";
  File f = SD_MMC.open(cfg, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0 || line.startsWith("//")) continue;
      json += "\"" + line + "\",";
    }
    f.close();
  }
  if (json.endsWith(",")) json.remove(json.length()-1);
  json += "]";
  server.sendHeader("Content-Type","application/json");
  server.send(200,"application/json",json);
}




void handleListApps() {
  const String dirPath = "/apps";
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    server.send(500, "application/json", "{\"error\":\"/apps not found\"}");
    return;
  }

  String json = "[";
  File f;
  while ((f = dir.openNextFile())) {
    String fname = f.name();            // e.g. "/apps/foo.run"
    if (!fname.endsWith(".run") && !fname.endsWith(".wx")) { f.close(); continue; }

    // read first few comment lines for name + icon
    String name   = "Unknown";
    String icon   = "default.png";
    for (int i = 0; i < 5 && f.available(); ++i) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (!line.startsWith("//")) break;

      // parse “// name: …” or “// icon: …”
      int c = line.indexOf(':');
      if (c > 2) {
        String key = line.substring(2, c);
        String val = line.substring(c + 1);
        val.trim();
        if      (key == "name") name = val;
        else if (key == "icon") icon = val;
      }
    }
    f.close();

    json += "{";
    json += "\"file\":\"" + fname + "\",";
    json += "\"name\":\"" + name + "\",";
    json += "\"icon\":\"" + icon + "\"";
    json += "},";
  }
  if (json.endsWith(",")) json.remove(json.length() - 1);
  json += "]";

  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", json);
}


void handleRunCommand(){
  if (!server.hasArg("command")) {
    server.send(400, "text/plain", "command arg required");
    return;
  }
  String cmdLine = server.arg("command");
  runCommand(cmdLine);
  server.send(200, "text/plain", "Command executed");
}

void runCommand(const String& commandLine) {

  if(commandRunning){
    logMsg();
  }
  String name;
  ArgList args;
  // parseLine splits "name(arg1,arg2,...)" into name + args vector
  if (!parseLine(commandLine, name, args)) {
    logMsg(("runCommand: parse error: "+commandLine).c_str());
    return;
  }
  auto it = commands.find(name);
  if (it == commands.end()) {
    logMsg(("runCommand: command not found: "+name).c_str());
    return;
  }
  // dispatch with the parsed ArgList
  it->second(args);
}




void connectWiFi() {
  WiFi.mode(WIFI_STA);
  
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    logMsg("❌ Failed to configure static IP");
  }

  WiFi.begin(SSID, PASSWORD);

  WiFi.setAutoReconnect(true);   // ✅ Auto reconnect on drop
  WiFi.persistent(true);         // 💾 Optional: store credentials in flash

  logMsg("⏳ Connecting to Wi-Fi");

  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 15000; // 15 seconds timeout

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < timeout) {
    delay(500);
    logMsg(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    char buf[32];
    snprintf(buf, sizeof(buf), "✅ Connected! IP: %s", WiFi.localIP().toString().c_str());
    logMsg(buf);
  } else {
    logMsg("\n❌ Failed to connect to Wi-Fi within timeout!");
    delay(200);
    ESP.restart();
  }
}


void deinitAllExceptBle() {
  server.stop();
  esp_camera_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void initCamera(int res) {
  camera_config.pin_pwdn = PWDN_GPIO_NUM;
  camera_config.pin_reset = RESET_GPIO_NUM;
  camera_config.pin_xclk = XCLK_GPIO_NUM;
  camera_config.pin_sscb_sda = SIOD_GPIO_NUM;
  camera_config.pin_sscb_scl = SIOC_GPIO_NUM;
  camera_config.pin_d7 = Y9_GPIO_NUM;
  camera_config.pin_d6 = Y8_GPIO_NUM;
  camera_config.pin_d5 = Y7_GPIO_NUM;
  camera_config.pin_d4 = Y6_GPIO_NUM;
  camera_config.pin_d3 = Y5_GPIO_NUM;
  camera_config.pin_d2 = Y4_GPIO_NUM;
  camera_config.pin_d1 = Y3_GPIO_NUM;
  camera_config.pin_d0 = Y2_GPIO_NUM;
  camera_config.pin_vsync = VSYNC_GPIO_NUM;
  camera_config.pin_href  = HREF_GPIO_NUM;
  camera_config.pin_pclk  = PCLK_GPIO_NUM;
  camera_config.xclk_freq_hz = 20000000;
  camera_config.ledc_timer   = LEDC_TIMER_0;
  camera_config.ledc_channel = LEDC_CHANNEL_0;
  camera_config.pixel_format = PIXFORMAT_JPEG;
  camera_config.frame_size   = (framesize_t)res;
  camera_config.jpeg_quality = 12;
  camera_config.fb_count     = 2;
  camera_config.grab_mode    = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    logMsg("Camera init failed");
  } else {
    logMsg("Camera initialized");
  }
}

bool isServerUp(int &statusValue) {
  HTTPClient http;
  http.begin(STATUS_URL);
  http.setTimeout(2000);
  int code = http.GET();
  if (code == 200) {
    statusValue = http.getString().toInt();
    http.end();
    return true;
  }
  http.end();
  return false;
}

void sendRecoverySequence() {
  logMsg("Stopping Everything!!!!");
  deinitAllExceptBle();
  logMsg("Starting BLE recovery...");
  keyboard.begin(); while(!keyboard.isConnected()) delay(500);
  delay(500); keyboard.write(KEY_ESC); delay(2000);
  keyboard.write(KEY_ESC); delay(2000);
  keyboard.print("ter"); delay(2000);
  keyboard.write(KEY_RETURN); delay(2000);
  keyboard.print("./startbot.sh"); delay(2000);
  keyboard.write(KEY_RETURN); delay(2000);
  logMsg("Recovery sequence Completed !");
  ESP.restart();
}


void logMsg(const char *msg) {
  File f = SD_MMC.open("/logs.txt", FILE_APPEND);
  if (!f) return;
  f.printf("[%lu] ", millis());
  f.println(msg);
  f.close();
}

void handleRoot() {
  if (SD_MMC.exists("/index.html")) {
    File f = SD_MMC.open("/index.html", FILE_READ);
    server.streamFile(f, "text/html"); f.close();
  } else server.send(404, "text/plain", "index.html not found");
}

void handleDisableAutoCharge(){
  AutoCharge = false;
  server.send(200, "text/plain", "Disabled Auto Charge !");
}

void handleEnableAutoCharge(){
  AutoCharge = true;
  server.send(200, "text/plain", "Enable Auto Charge !");
}

void handleDisableAutoRestart(){
  AutoRestart = false;
  server.send(200, "text/plain", "Disable Auto Restart !");
}

void handleEnableAutoRestart(){
  AutoRestart = true;
  server.send(200, "text/plain", "Enabled Auto Restart !");
}

void handleDisableCamera(){
  Camera = false;
  server.send(200, "text/plain", "Disabled Camera !");
}

void handleEnableCamera(){
  Camera = true;
  server.send(200, "text/plain", "Enable Server");
}

void handleDisableChargeServer(){
  Charge = false;
  server.send(200, "text/plain", "Disabled Charging");
}

void handleEnableChargeServer(){
  Charge = true;
  server.send(200, "text/plain", "Enabled Chargind");
}

void handleGetStatus() {
  // Build a small JSON with the three booleans
  String payload = "{";
  payload += "\"autoCharge\":"  + String(AutoCharge  ? "true" : "false") + ",";
  payload += "\"autoRestart\":" + String(AutoRestart ? "true" : "false") + ",";
  payload += "\"camera\":"      + String(Camera      ? "true" : "false") + ",";
  payload += "\"charge\":"      + String(Charge      ? "true" : "false");
  payload += "}";

  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", payload);
}


void handleControl() {
  if (server.hasArg("res")) { frameSize = server.arg("res").toInt(); initCamera(frameSize); }
  if (server.hasArg("fps")) fpsValue = server.arg("fps").toInt();
  server.send(200, "text/plain", "OK");
}

// Add this handler somewhere after your other server.on() calls in setup():
//   server.on("/disableCam", HTTP_GET, handleDisableCam);
//   server.on("/enableCam",  HTTP_GET, handleEnableCam);
void handleDisableCam() {
  Camera = false;
  server.send(200, "text/plain", "Camera disabled");
}
void handleEnableCam() {
  Camera = true;
  server.send(200, "text/plain", "Camera enabled");
}

void handleStream() {
  WiFiClient client = server.client();
  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n"
  );

  while (client.connected()) {
    if (!Camera) {
      // Serve the static PNG when Camera is off
      File file = SD_MMC.open("/assets/cameradisabled.png", "r");
      if (!file || file.isDirectory()) {
        file.close();
        break;
      }
      size_t len = file.size();
      client.printf(
        "--frame\r\n"
        "Content-Type: image/png\r\n"
        "Content-Length: %u\r\n\r\n",
        len
      );
      // Stream the file in chunks
      uint8_t buf[256];
      while (file.available()) {
        size_t chunk = file.read(buf, sizeof(buf));
        client.write(buf, chunk);
      }
      file.close();
      client.print("\r\n");
      delay(1000 / fpsValue);
      continue;
    }

    // Original live camera stream
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;
    client.printf(
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n\r\n",
      fb->len
    );
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    delay(1000 / fpsValue);
  }
}


void handleFileManagerPage() {
  if (SD_MMC.exists("/filemanager.html")) {
    File f = SD_MMC.open("/filemanager.html", FILE_READ);
    server.streamFile(f, "text/html"); f.close();
  } else server.send(404, "text/plain", "filemanager.html not found");
}

void handleLogsPage() {
  if (SD_MMC.exists("/logs.html")) {
    File f = SD_MMC.open("/logs.html", FILE_READ);
    server.streamFile(f, "text/html"); f.close();
  } else server.send(404, "text/plain", "logs.html not found");
}

void handleLogs(){
  if (SD_MMC.exists("/logs.txt")) {
    File f = SD_MMC.open("/logs.txt", FILE_READ);
    server.streamFile(f, "text/plain"); f.close();
  } else server.send(404, "text/plain", "logs.txt not found");
}

void handleList() {
  String dir = server.hasArg("dir") ? server.arg("dir") : "/";
  File root = SD_MMC.open(dir);
  if (!root || !root.isDirectory()) { server.send(500, "application/json", "{\"error\":\"open failed\"}"); return; }
  String json = "[";
  File f;
  while ((f = root.openNextFile())) {
    json += String("{\"name\":\"") + f.name() + String("\",\"size\":") + f.size() +
            String(",\"isDir\":") + (f.isDirectory() ? "true" : "false") + String("},");
    f.close();
  }
  if (json.endsWith(",")) json.remove(json.length()-1);
  json += "]";
  server.send(200, "application/json", json);
}
// Recursively delete everything under path (files and sub-dirs)
bool removeRecursive(const String & path) {
  File entry = SD_MMC.open(path);
  if (!entry) return false;

  if (entry.isDirectory()) {
    File child;
    // Iterate and delete children first
    while ((child = entry.openNextFile())) {
      String childPath = String(path) + "/" + child.name();
      child.close();
      if (!removeRecursive(childPath)) {
        entry.close();
        return false;
      }
    }
    entry.close();
    // now remove the (now empty) directory
    return SD_MMC.rmdir(path);
  } else {
    entry.close();
    // simple file
    return SD_MMC.remove(path);
  }
}

void handleDelete() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing 'path'");
    return;
  }
  String path = server.arg("path");
  if (!SD_MMC.exists(path)) {
    server.send(404, "text/plain", "Not Found");
    return;
  }
  bool ok = removeRecursive(path);
  server.send(ok ? 200 : 500,
              "text/plain",
              ok ? "Deleted" : "Delete failed");
}

void handleRename() {
  if (!server.hasArg("from") || !server.hasArg("to")) {
    server.send(400, "text/plain", "Missing 'from' or 'to'");
    return;
  }
  String from = server.arg("from");
  String to   = server.arg("to");
  if (!SD_MMC.exists(from)) {
    server.send(404, "text/plain", "Source not found");
    return;
  }
  // make sure parent of 'to' exists, or create it?
  bool ok = SD_MMC.rename(from, to);
  server.send(ok ? 200 : 500,
              "text/plain",
              ok ? "Renamed" : "Rename failed");
}


void handleOpenFile() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing 'path' parameter");
    return;
  }

  String path = server.arg("path");
  if (!SD_MMC.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }

  String contentType = getContentType(path);
  // Tell browser to render inline instead of download
  server.sendHeader("Content-Disposition", "inline; filename=\"" + path.substring(path.lastIndexOf('/') + 1) + "\"");
  server.streamFile(f, contentType);
  f.close();
}

void handleRestart(){
  logMsg("Reboot Initiated Manually !");
  server.send(200, "text/plain", "Rebooting..");
  delay(500);
  ESP.restart();
}

void handleRestartTermux(){
  server.send(200, "text/plain", "Restarting Temux....");
  logMsg("Stopping Everything!!!!");
  deinitAllExceptBle();
  logMsg("Starting BLE Termux Restart...");
  keyboard.begin(); while(!keyboard.isConnected()) delay(500);
  delay(500); keyboard.write(KEY_ESC); delay(2000);
  keyboard.write(KEY_ESC); delay(2000);
  keyboard.write(KEY_RETURN); delay(2000);
  keyboard.print("./stopter.sh"); delay(2000);
  keyboard.write(KEY_RETURN); delay(3000);
  keyboard.print("ter"); delay(2000);
  keyboard.write(KEY_RETURN); delay(2000);
  keyboard.print("./startbot.sh"); delay(2000);
  keyboard.write(KEY_RETURN); delay(2000);
  logMsg("Restart sequence Completed !");
  ESP.restart();
}

void handleMkdir() {
  if (!server.hasArg("path")) { server.send(400, "text/plain", "Missing 'path'"); return; }
  bool ok = SD_MMC.mkdir(server.arg("path"));
  server.send(ok ? 200 : 500, "text/plain", ok ? "Created" : "Failed");
}

void handleUpload() {
  HTTPUpload& upload = server.upload(); 
  static File file;

  if (upload.status == UPLOAD_FILE_START) {
    String dir = server.arg("path");
    if (dir.length() == 0) dir = "/";
    String path = dir + (dir.endsWith("/") ? "" : "/") + upload.filename;

    if (SD_MMC.exists(path)) SD_MMC.remove(path);
    file = SD_MMC.open(path, FILE_WRITE);
    if (!file) {
      server.send(500, "text/plain", "File open failed");
      return;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (file) file.write(upload.buf, upload.currentSize);

  } else if (upload.status == UPLOAD_FILE_END) {
    if (file) {
      file.close();
      server.send(200, "text/plain", "Upload successful");
    } else {
      server.send(500, "text/plain", "File write failed");
    }
  }
}



void runScript(const String& path) {
  if (scriptRunning) {
    logMsg("runScript: already running");
    return;
  }

  scriptRunning = true;

  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    logMsg("runScript: failed to open");
    scriptRunning = false;
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("//") || line.length() == 0) continue;

    String name;
    ArgList args;
    if (!parseLine(line, name, args)) {
      logMsg(("Parse error: " + line).c_str());
      continue;
    }

    auto it = commands.find(name);
    if (it == commands.end()) {
      logMsg(("Unknown cmd: " + name).c_str());
      continue;
    }

    it->second(args);  // dispatch
  }

  f.close();
  scriptRunning = false;
}


void handleRunScript(){
  if(server.hasArg("script")){
    runScript(server.arg("script"));
    server.send(200, "text/plain", "Script run Triggered check logs for output");
  }else{
    server.send(400, "text/plain", "Missing 'script' parameter");
  }
}

void handleNotFound() {
  String uri = server.uri();  // e.g. "/app3"

  // load the endpoints table and look for a matching URI
  File f = SD_MMC.open(ENDPOINTS_TABLE, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      int comma = line.indexOf(',');
      if (comma < 0) continue;
      String key    = line.substring(0, comma);
      String target = line.substring(comma + 1);
      if (key == uri) {
        // found: serve target (relative to /htmls/ unless it starts with "/")
        String fsPath = target.startsWith("/") ? target : "/htmls/" + target;
        if (SD_MMC.exists(fsPath)) {
          File hf = SD_MMC.open(fsPath, FILE_READ);
          server.sendHeader(
            "Content-Disposition",
            "inline; filename=\"" +
              fsPath.substring(fsPath.lastIndexOf('/') + 1) +
              "\""
          );
          server.streamFile(hf, getContentType(fsPath));
          hf.close();
          f.close();
          return;
        } else {
          server.send(
            500, "text/plain",
            "Mapping for " + uri +
            " → " + target +
            " but file not found on SD"
          );
          f.close();
          return;
        }
      }
    }
    f.close();
  }

  // no mapping → hard 404
  server.send(404, "text/plain", "404: Not Found");
}


//––– Command implementations –––
void cmd_logMsg(const ArgList& a) {
  // strip quotes
  String msg = a[0];
  if (msg.startsWith("\"") && msg.endsWith("\""))
    msg = msg.substring(1, msg.length()-1);
  File f = SD_MMC.open("/logs.txt", FILE_APPEND);
  if (f) { f.println(msg); f.close(); }
}

void cmd_httpGet(const ArgList& a) {
  HTTPClient http;
  http.begin(a[0].substring(1, a[0].length()-1)); // strip quotes
  int code = http.GET();
  logMsg(("GET code=" + String(code)).c_str());
  http.end();
}

void cmd_httpPost(const ArgList& a) {
  HTTPClient http;
  http.begin(a[0].substring(1, a[0].length()-1));
  http.addHeader("Content-Type","application/json");
  int code = http.POST(a[1].substring(1, a[1].length()-1));
  logMsg(("POST code=" + String(code)).c_str());
  http.end();
}

void cmd_setPin(const ArgList& a) {
  int pin   = a[0].toInt();
  int state = (a[1].equals("HIGH") ? HIGH : LOW);
  digitalWrite(pin, state);
}

void cmd_readpin(const ArgList& a) {
  int pin = a[0].toInt();
  int val = digitalRead(pin);
  logMsg(("PIN"+String(pin)+"="+String(val)).c_str());
}

void cmd_delayMs(const ArgList& a) {
  delay(a[0].toInt());
}

void cmd_writeData(const ArgList& a) {
  // strip quotes from arguments
  String path = a[0];
  if (path.startsWith("\"") && path.endsWith("\""))
    path = path.substring(1, path.length() - 1);

  String data = a[1];
  if (data.startsWith("\"") && data.endsWith("\""))
    data = data.substring(1, data.length() - 1);

  // remove existing file so we overwrite instead of append
  if (SD_MMC.exists(path)) {
    SD_MMC.remove(path);
  }

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    logMsg(("writeData: failed to open " + path).c_str());
    return;
  }
  f.print(data);
  f.close();
  logMsg(("writeData: wrote to " + path).c_str());
}

void cmd_listcmds(const ArgList& a) {
  String list;
  for (auto const& kv : commands) {
    if (!list.isEmpty()) list += ", ";
    list += kv.first;
  }
  logMsg(("Available commands: " + list).c_str());
}

void cmd_clearlogs(const ArgList& a) {
    SD_MMC.remove("/logs.txt");
    logMsg("Logs manually cleared");
}

void cmd_pinmode(const ArgList& a) {
  int mainpin = a[0].toInt();
  String pinmode = a[1];

  if (pinmode == "input") {
    pinMode(mainpin, INPUT);
    logMsg(("Setting pin " + String(mainpin) + " as Input").c_str());
  } else if (pinmode == "output") {
    pinMode(mainpin, OUTPUT);
    logMsg(("Setting pin " + String(mainpin) + " as Output").c_str());
  } else {
    logMsg("Invalid Mode");
  }
}


void cmd_serverBind(const ArgList& a) {
  String uri    = a[0];  // e.g. "/app3"
  String target = a[1];  // e.g. "file3.html"

  // strip quotes
  if (uri.startsWith("\"") && uri.endsWith("\""))
    uri = uri.substring(1, uri.length() - 1);
  if (target.startsWith("\"") && target.endsWith("\""))
    target = target.substring(1, target.length() - 1);

  // ensure leading slash
  if (!uri.startsWith("/")) uri = "/" + uri;

  // read existing to avoid duplicates
  bool exists = false;
  File f = SD_MMC.open(ENDPOINTS_TABLE, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (line.startsWith(uri + ",")) { exists = true; break; }
    }
    f.close();
  } else {
    // create parent folder if needed
    SD_MMC.mkdir("/server");
  }

  if (!exists) {
    File out = SD_MMC.open(ENDPOINTS_TABLE, FILE_APPEND);
    if (out) {
      out.printf("%s,%s\n", uri.c_str(), target.c_str());
      out.close();
      logMsg(("Bound: " + uri + " → " + target).c_str());
    } else {
      logMsg("serverbind: failed to open endpoints file");
    }
  } else {
    logMsg(("serverbind: mapping already exists for " + uri).c_str());
  }
}

void cmd_serverUnbind(const ArgList& a) {
  String uri = a[0];  // e.g. "/app3"
  if (uri.startsWith("\"") && uri.endsWith("\""))
    uri = uri.substring(1, uri.length() - 1);
  if (!uri.startsWith("/")) uri = "/" + uri;

  // read all lines, keep those that don't match
  std::vector<String> lines;
  File f = SD_MMC.open(ENDPOINTS_TABLE, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (!line.startsWith(uri + ",")) {
        lines.push_back(line);
      }
    }
    f.close();

    // overwrite with filtered list
    File out = SD_MMC.open(ENDPOINTS_TABLE, FILE_WRITE);
    if (out) {
      for (auto &L : lines) {
        out.println(L);
      }
      out.close();
      logMsg(("Unbound: " + uri).c_str());
    } else {
      logMsg("serverunbind: failed to open endpoints file for write");
    }
  } else {
    logMsg("serverunbind: no endpoints file to modify");
  }
}

void cmd_rm(const ArgList& a) {
  String file=a[0];
    if (!file) {
    logMsg("Invalid Argument");
    return;
  }
  if (!SD_MMC.exists(file)) {
    logMsg("File not found !");
    return;
  }
  bool ok = removeRecursive(file);
  logMsg(ok ? "Deleted" : "Delete failed");
}


//end script