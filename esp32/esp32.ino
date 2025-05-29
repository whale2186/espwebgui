#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <FS.h>

// C++ containers and algorithms for scripting engine
#include <map>
#include <vector>
#include <algorithm>
#include <functional>

// ——— Configuration ———
const char* SSID       = "SSID";
const char* PASSWORD   = "PASSWD";

//Static IP
//IPAddress local_IP(192,168,1,25), gateway(192,168,1,1), subnet(255,255,255,0);

//DNS Config
IPAddress primaryDNS(8,8,8,8), secondaryDNS(8,8,4,4);

#define AUTOSTART_CONFIG "/configs/autostart.txt"
#define ENDPOINTS_TABLE  "/server/endpoints.csv"

WiFiClient   httpClient;
WebServer    server(80);

// ——— Scripting engine types ———
typedef std::vector<String> ArgList;
typedef std::function<void(const ArgList&)> CmdFn;
std::map<String, CmdFn> commands;
volatile bool scriptRunning = false;

// ——— Forward declarations ———
void connectWiFi();
void logMsg(const char* msg);

String getContentType(const String& path);
bool parseLine(const String& line, String& name, ArgList& args);
void initScriptEngine();
void runScript(const String& path);

// HTTP handlers
void handleRoot();
void handleList();
void handleDelete();
void handleUpload();
void handleDownload();
void handleMkdir();
void handleRename();
void handleNotFound();
void handleRunScript();
void handleAutoStartAdd();
void handleAutoStartRemove();
void handleGetAutoStart();
void loadAutoStart();
void handleFileManagerPage();
void handleLogsPage();
void handleLogs();
void handleRunCommand();
void handleListApps();
void handleRestart();

// AutoStart helpers
std::vector<String> readAutoStartList();
bool writeAutoStartList(const std::vector<String>& L);

// ——— Helpers for scripting ———
bool removeRecursive(const String & path) {
  File entry = LittleFS.open(path, "r");
  if (!entry) return false;
  if (entry.isDirectory()) {
    File child;
    while ((child = entry.openNextFile())) {
      String childPath = String(path) + "/" + child.name();
      removeRecursive(childPath);
      child.close();
    }
    entry.close();
    return LittleFS.rmdir(path);
  } else {
    entry.close();
    return LittleFS.remove(path);
  }
}

void appendEndpoint(const String& uri, const String& target) {
  LittleFS.mkdir("/server");
  File f = LittleFS.open(ENDPOINTS_TABLE, "a");
  if (!f) return;
  f.printf("%s,%s\n", uri.c_str(), target.c_str());
  f.close();
}

void removeEndpoint(const String& uri) {
  if (!LittleFS.exists(ENDPOINTS_TABLE)) return;
  File f = LittleFS.open(ENDPOINTS_TABLE, "r");
  std::vector<String> lines;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (!l.startsWith(uri + ",")) lines.push_back(l);
  }
  f.close();
  File fw = LittleFS.open(ENDPOINTS_TABLE, "w");
  for (auto &l: lines) fw.println(l);
  fw.close();
}

// ——— Setup & Loop ———
void setup() {
  Serial.begin(115200);
  delay(500);

  if (!LittleFS.begin()) {
    Serial.println("❌ LittleFS Mount Failed");
    return;
  }
  Serial.println("✅ LittleFS Mounted");

  connectWiFi();

  ArduinoOTA.onStart([]() { logMsg("OTA Start"); });
  ArduinoOTA.onEnd([]()   { logMsg("OTA End");   });
  ArduinoOTA.onError([](ota_error_t error) { logMsg("OTA Error"); });

  server.on("/",                 HTTP_GET, handleRoot);
  server.on("/index.html",       HTTP_GET, handleRoot);
  server.on("/filemanager.html", HTTP_GET, handleFileManagerPage);
  server.on("/logs.html",        HTTP_GET, handleLogsPage);
  server.on("/logs",             HTTP_GET, handleLogs);

  server.on("/list",     HTTP_GET,  handleList);
  server.on("/delete",   HTTP_GET,  handleDelete);
  server.on("/upload",   HTTP_POST, [](){ server.send(200); }, handleUpload);
  server.on("/download", HTTP_GET,  handleDownload);
  server.on("/mkdir",    HTTP_GET,  handleMkdir);
  server.on("/rename",   HTTP_GET,  handleRename);
  server.on("/open",     HTTP_GET,  handleDownload);

  server.on("/runscript",        HTTP_GET, handleRunScript);
  server.on("/autostart/add",    HTTP_GET, handleAutoStartAdd);
  server.on("/autostart/remove", HTTP_GET, handleAutoStartRemove);
  server.on("/runcommand",       HTTP_GET, handleRunCommand);
  server.on("/getautostart",     HTTP_GET, handleGetAutoStart);
  server.on("/listapps",         HTTP_GET, handleListApps);
  server.on("/restart",          HTTP_GET, handleRestart);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("🌐 HTTP server started");

  initScriptEngine();
  loadAutoStart();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
}

// ——— Core ———
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected, IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ WiFi Failed, restarting");
    ESP.restart();
  }
}

void logMsg(const char *msg) {
  File f = LittleFS.open("/logs.txt", "a");
  if (!f) return;
  f.printf("[%10lu] %s\n", millis(), msg);
  f.close();
}

String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".gif"))  return "image/gif";
  return "application/octet-stream";
}

// ——— HTTP Handlers ———
void handleRoot() {
  if (!LittleFS.exists("/index.html"))
    return server.send(404,"text/plain","index.html not found");
  File f = LittleFS.open("/index.html","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleList() {
  String dir = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!dir.endsWith("/")) dir += "/";

  File root = LittleFS.open(dir);
  String json = "[";
  File entry;
  while ((entry = root.openNextFile())) {
    if (json.length()>1) json += ",";
    json += String("{\"name\":\"") + entry.name() +
            String("\",\"size\":")    + entry.size() +
            String(",\"isDir\":")     + (entry.isDirectory()?"true":"false") +
            String("}");
    entry.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleFileManagerPage() {
  if (!LittleFS.exists("/filemanager.html"))
    return server.send(404,"text/plain","filemanager.html not found");
  File f = LittleFS.open("/filemanager.html","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleLogsPage() {
  if (!LittleFS.exists("/logs.html"))
    return server.send(404,"text/plain","logs.html not found");
  File f = LittleFS.open("/logs.html","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleLogs(){
  if (!LittleFS.exists("/logs.txt"))
    return server.send(404,"text/plain","logs.txt not found");
  File f = LittleFS.open("/logs.txt","r");
  server.streamFile(f,"text/html");
  f.close();
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File file;
  if (upload.status == UPLOAD_FILE_START) {
    String dir = server.arg("path");
    if (dir.length() == 0) dir = "/";
    String path = dir + (dir.endsWith("/") ? "" : "/") + upload.filename;
    if (LittleFS.exists(path)) LittleFS.remove(path);
    file = LittleFS.open(path, "w");
    if (!file) { server.send(500, "text/plain", "File open failed"); return; }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (file) file.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (file) { file.close(); server.send(200, "text/plain", "Upload successful"); }
    else     { server.send(500, "text/plain", "File write failed"); }
  }
}

void handleDownload() {
  String path = server.arg("path");
  if (!LittleFS.exists(path)) return server.send(404,"text/plain","Not Found");
  File f = LittleFS.open(path,"r");
  server.streamFile(f, getContentType(path));
  f.close();
}

void handleDelete() {
  String path = server.arg("path");
  if (!LittleFS.exists(path)) return server.send(404,"text/plain","Not Found");
  bool ok = removeRecursive(path);
  server.send(ok?200:500,"text/plain", ok?"Deleted":"Delete failed");
}

void handleMkdir() {
  String path = server.arg("path");
  bool ok = LittleFS.mkdir(path);
  server.send(ok?200:500,"text/plain", ok?"Created":"Failed");
}

void handleRename() {
  String from = server.arg("from"), to = server.arg("to");
  bool ok = LittleFS.rename(from,to);
  server.send(ok?200:500,"text/plain", ok?"Renamed":"Rename failed");
}

void handleNotFound() {
  String uri = server.uri();
  File f = LittleFS.open(ENDPOINTS_TABLE,"r");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int c = line.indexOf(',');
    if (c>0 && line.substring(0,c)==uri) {
      String tgt = line.substring(c+1);
      if (LittleFS.exists(tgt)) {
        File hf = LittleFS.open(tgt,"r");
        server.streamFile(hf,getContentType(tgt));
        hf.close(); f.close();
        return;
      }
    }
  }
  f.close();
  server.send(404,"text/plain","404: Not Found");
}

void handleRunCommand(){
  if (!server.hasArg("command")) { server.send(400); return; }
  runScript(server.arg("command"));
  server.send(200,"text/plain","Command executed");
}

void handleRestart(){
  logMsg("Reboot Initiated Manually !");
  server.send(200, "text/plain", "Rebooting..");
  delay(500);
  ESP.restart();
}

void handleListApps() {
  const char* dirPath = "/apps";

  // 1) Make sure the directory exists
  if (!LittleFS.exists(dirPath)) {
    server.send(500, "application/json", "{\"error\":\"/apps not found\"}");
    return;
  }

  // 2) Open the directory
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    server.send(500, "application/json", "{\"error\":\"/apps open failed\"}");
    return;
  }

  // 3) Build JSON array
  String json = "[";

  File entry;
  while ((entry = dir.openNextFile())) {
    String fname = entry.name();  // e.g. "/apps/foo.run"

    // only include .run and .wx
    if (fname.endsWith(".run") || fname.endsWith(".wx")) {
      String name = "Unknown";
      String icon = "default.png";

      // read up to 5 comment-lines for "// name:" and "// icon:"
      for (int i = 0; i < 5 && entry.available(); ++i) {
        String line = entry.readStringUntil('\n');
        line.trim();
        if (!line.startsWith("//")) break;
        int colon = line.indexOf(':');
        if (colon > 2) {
          String key = line.substring(2, colon);
          String val = line.substring(colon + 1);
          val.trim();
          if      (key == "name") name = val;
          else if (key == "icon") icon = val;
        }
      }

      json += "{";
      json += "\"file\":\"" + fname + "\",";
      json += "\"name\":\"" + name + "\",";
      json += "\"icon\":\"" + icon + "\"";
      json += "},";
    }

    entry.close();
  }

  // remove trailing comma (if any) and close array
  if (json.endsWith(",")) {
    json.remove(json.length() - 1);
  }
  json += "]";

  // 4) Send JSON response
  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", json);
}

void handleRunScript() {
  if (!server.hasArg("script")) return server.send(400);
  runScript(server.arg("script"));
  server.send(200,"text/plain","Script triggered");
}

void handleAutoStartAdd() {
  if (!server.hasArg("script")) return server.send(400);
  String s = server.arg("script");
  std::vector<String> L = readAutoStartList();
  if (std::find(L.begin(),L.end(),s) == L.end()) {
    L.push_back(s);
    writeAutoStartList(L);
  }
  server.send(200,"text/plain","ok");
}

void handleAutoStartRemove() {
  if (!server.hasArg("script")) return server.send(400);
  String s = server.arg("script");
  std::vector<String> L = readAutoStartList();
  L.erase(std::remove(L.begin(),L.end(),s),L.end());
  writeAutoStartList(L);
  server.send(200,"text/plain","ok");
}

void handleGetAutoStart() {
  std::vector<String> L = readAutoStartList();
  String j = "[";
  for (size_t i = 0; i < L.size(); ++i) {
    j += "\"" + L[i] + "\",";
  }
  if (j.endsWith(",")) j.remove(j.length()-1);
  j += "]";
  server.send(200,"application/json",j);
}

// ——— AutoStart helpers ———
std::vector<String> readAutoStartList() {
  std::vector<String> out;
  File f = LittleFS.open(AUTOSTART_CONFIG,"r");
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (l.length() && !l.startsWith("//")) out.push_back(l);
  }
  f.close();
  return out;
}

bool writeAutoStartList(const std::vector<String>& L) {
  File f = LittleFS.open(AUTOSTART_CONFIG,"w");
  if (!f) return false;
  for (auto &s: L) f.println(s);
  f.close();
  return true;
}

void loadAutoStart() {
  if (!LittleFS.exists(AUTOSTART_CONFIG)) return;
  File cfg = LittleFS.open(AUTOSTART_CONFIG, "r");
  while (cfg.available()) {
    String line = cfg.readStringUntil('\n'); line.trim();
    if (line.length()==0 || line.startsWith("//")) continue;
    logMsg(("AutoStarted: "+line).c_str());
    runScript("/apps/" + line);
  }
  cfg.close();
}

bool parseLine(const String& line, String& name, ArgList& args) {
  int p=line.indexOf('('), q=line.lastIndexOf(')');
  if (p<1||q<=p) return false;
  name=line.substring(0,p);
  String body=line.substring(p+1,q);
  args.clear();
  int i=0;
  while (i<body.length()) {
    int c=body.indexOf(',',i);
    String a = c<0 ? body.substring(i) : body.substring(i,c);
    a.trim();
    if (a.length()) args.push_back(a);
    if (c<0) break;
    i=c+1;
  }
  return true;
}

void runScript(const String& path) {
  if (scriptRunning) return;
  scriptRunning = true;
  File f = LittleFS.open(path,"r");
  if (!f) { scriptRunning=false; return; }
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (l.startsWith("//")||l.length()==0) continue;
    String cmd; ArgList A;
    if (!parseLine(l,cmd,A)) continue;
    auto it=commands.find(cmd);
    if (it!=commands.end()) it->second(A);
  }
  f.close();
  scriptRunning = false;
}

void initScriptEngine() {
  commands["logmsg"] = [](const ArgList& A){ logMsg(A[0].c_str()); };
  commands["httpget"] = [](const ArgList& A){
    HTTPClient h; String u=A[0]; u.replace("\"","");
    if (h.begin(httpClient,u)) { int code=h.GET(); logMsg(("GET="+String(code)).c_str()); h.end(); }
    else logMsg("GET: begin() failed");
  };
  commands["httppost"] = [](const ArgList& A){
    HTTPClient h; String u=A[0],b=A[1]; u.replace("\"",""); b.replace("\"","");
    if (h.begin(httpClient,u)) { h.addHeader("Content-Type","application/json"); int code=h.POST(b); logMsg(("POST="+String(code)).c_str()); h.end(); }
    else logMsg("POST: begin() failed");
  };
  commands["setpin"] = [](const ArgList& A){
    int p=A[0].toInt();
    digitalWrite(p, A[1]=="HIGH"?HIGH:LOW);
  };
  commands["readpin"] = [](const ArgList& A){
    int p=A[0].toInt();
    logMsg(("PIN"+String(p)+"="+String(digitalRead(p))).c_str());
  };
  commands["pinmode"] = [](const ArgList& A){
    int p=A[0].toInt();
    pinMode(p, A[1]=="input"?INPUT:OUTPUT);
    logMsg(("PinMode "+String(p)+"="+A[1]).c_str());
  };
  commands["delayms"] = [](const ArgList& A){ delay(A[0].toInt()); };
  commands["writedata"] = [](const ArgList& A){
    String p=A[0],d=A[1]; p.replace("\"",""); d.replace("\"","");
    if (LittleFS.exists(p)) LittleFS.remove(p);
    File f=LittleFS.open(p,"w"); if (f) { f.print(d); f.close(); }
    logMsg(("writedata "+p).c_str());
  };
  commands["listcmds"] = [](const ArgList&){
    String all;
    for (auto &kv: commands) { if (all.length()) all+=", "; all+=kv.first; }
    logMsg(("cmds: "+all).c_str());
  };
  commands["clearlogs"] = [](const ArgList&){
    if (LittleFS.exists("/logs.txt")) LittleFS.remove("/logs.txt");
    logMsg("Logs cleared");
  };
  commands["rm"] = [](const ArgList& A){
    String p=A[0]; p.replace("\"","");
    logMsg(removeRecursive(p)?"rm OK":"rm fail");
  };
  commands["serverbind"] = [](const ArgList& A){
    String u=A[0],t=A[1]; u.replace("\"",""); t.replace("\"","");
    if (!u.startsWith("/")) u="/"+u;
    appendEndpoint(u,t);
    logMsg(("Bind "+u+"->"+t).c_str());
  };
  commands["serverunbind"] = [](const ArgList& A){
    String u=A[0]; u.replace("\"","");
    if (!u.startsWith("/")) u="/"+u;
    removeEndpoint(u);
    logMsg(("Unbind "+u).c_str());
  };
commands["togglepin"] = [](const ArgList& A){
  int p = A[0].toInt();
  int currentstate = digitalRead(p);
  digitalWrite(p, currentstate == LOW ? HIGH : LOW);
};

}
