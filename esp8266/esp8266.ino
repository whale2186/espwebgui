#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>

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
//IPAddress primaryDNS(8,8,8,8), secondaryDNS(8,8,4,4);

#define AUTOSTART_CONFIG "/configs/autostart.txt"
#define ENDPOINTS_TABLE  "/server/endpoints.csv"

WiFiClient   httpClient;
ESP8266WebServer server(80);

// ——— Scripting engine types ———
typedef std::vector<String> ArgList;
typedef std::function<void(const ArgList&)> CmdFn;
std::map<String, CmdFn> commands;
volatile bool scriptRunning = false;

const int CYCLE_DELAY_MS = 15000;

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


// ——— Helpers for scripting ———
bool removeRecursive(const String & path) {
  File entry = LittleFS.open(path, "r");
  if (!entry) return false;
  if (entry.isDirectory()) {
    Dir dir = LittleFS.openDir(path);
    while (dir.next()) {
      String childPath = path + "/" + dir.fileName();
      removeRecursive(childPath);
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

ArduinoOTA.setHostname("esp123");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });

    ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

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
  server.on("/open", HTTP_GET, handleOpenFile);


  server.on("/runscript",        HTTP_GET, handleRunScript);
  server.on("/autostart/add",    HTTP_GET, handleAutoStartAdd);
  server.on("/autostart/remove", HTTP_GET, handleAutoStartRemove);
  server.on("/runcommand",       HTTP_GET, handleRunCommand);
  server.on("/getautostart",     HTTP_GET, handleGetAutoStart);
  server.on("/listapps", HTTP_GET, handleListApps);
  server.on("/restart", HTTP_GET, handleRestart);

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

void handleRunCommand(){
  if (!server.hasArg("command")) {
    server.send(400, "text/plain", "command arg required");
    return;
  }
  String cmdLine = server.arg("command");
  runCommand(cmdLine);
  server.send(200, "text/plain", "Command executed");
}

void handleLogs(){
  if (!LittleFS.exists("/logs.txt"))
    return server.send(404,"text/plain","logs.txt not found");
  File f = LittleFS.open("/logs.txt","r");
  server.streamFile(f,"text/html");
  f.close();
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

  Dir d = LittleFS.openDir(dir);
  String json = "[";
  while (d.next()) {
    if (json.length()>1) json += ",";
    json += String("{\"name\":\"") + d.fileName() +
            String("\",\"size\":")    + d.fileSize() +
            String(",\"isDir\":")     + (d.isDirectory()?"true":"false") +
            String("}");
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleOpenFile() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing 'path' parameter");
    return;
  }

  String path = server.arg("path");
  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }

  String contentType = getContentType(path);
  // Tell browser to render inline instead of download
  server.sendHeader("Content-Disposition",
                    "inline; filename=\"" +
                     path.substring(path.lastIndexOf('/') + 1) +
                     "\"");
  server.streamFile(f, contentType);
  f.close();
}

void handleDelete() {
  String path = server.arg("path");
  if (!LittleFS.exists(path)) return server.send(404,"text/plain","Not Found");
  bool ok = removeRecursive(path);
  server.send(ok?200:500,"text/plain", ok?"Deleted":"Delete failed");
}

void handleListApps() {
  const char* dirPath = "/apps";

  // 1) Make sure the directory exists
  if (!LittleFS.exists(dirPath)) {
    server.send(500, "application/json", "{\"error\":\"/apps not found\"}");
    return;
  }

  // 2) Open the directory
  Dir dir = LittleFS.openDir(dirPath);

  // 3) Build JSON array
  String json = "[";

  while (dir.next()) {
    // open the current file for reading
    File f = dir.openFile("r");
    String fname = f.name();  // e.g. "/apps/foo.run"

    // only include .run and .wx
    if (!fname.endsWith(".run") && !fname.endsWith(".wx")) {
      f.close();
      continue;
    }

    // defaults
    String name = "Unknown";
    String icon = "default.png";

    // read up to 5 comment-lines for "// name:" and "// icon:"
    for (int i = 0; i < 5 && f.available(); ++i) {
      String line = f.readStringUntil('\n');
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
    f.close();

    // append this entry
    json += "{";
    json += "\"file\":\"" + fname + "\",";
    json += "\"name\":\"" + name + "\",";
    json += "\"icon\":\"" + icon + "\"";
    json += "},";
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


void handleUpload() {
  HTTPUpload& upload = server.upload(); 
  static File file;

  if (upload.status == UPLOAD_FILE_START) {
    String dir = server.arg("path");
    if (dir.length() == 0) dir = "/";
    String path = dir + (dir.endsWith("/") ? "" : "/") + upload.filename;

    if (LittleFS.exists(path)) LittleFS.remove(path);
    file = LittleFS.open(path, "w");  // Use "w" instead of FILE_WRITE
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

void handleDownload() {
  String path = server.arg("path");
  if (!LittleFS.exists(path)) return server.send(404,"text/plain","Not Found");
  File f = LittleFS.open(path,"r");
  server.streamFile(f, getContentType(path));
  f.close();
}

void handleRestart(){
  logMsg("Reboot Initiated Manually !");
  server.send(200, "text/plain", "Rebooting..");
  delay(500);
  ESP.restart();
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
  while (f && f.available()) {
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
  if (f) f.close();
  server.send(404,"text/plain","404: Not Found");
}

// ——— AutoStart & Scripting ———
std::vector<String> readAutoStartList() {
  std::vector<String> out;
  File f = LittleFS.open(AUTOSTART_CONFIG,"r");
  while (f && f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (l.length() && !l.startsWith("//")) out.push_back(l);
  }
  if (f) f.close();
  return out;
}

bool writeAutoStartList(const std::vector<String>& L) {
  File f = LittleFS.open(AUTOSTART_CONFIG,"w");
  if (!f) return false;
  for (auto &s: L) f.println(s);
  f.close();
  return true;
}

void handleAutoStartAdd() {
  if (!server.hasArg("script")) return server.send(400);
  String s = server.arg("script");
  auto L = readAutoStartList();
  if (std::find(L.begin(),L.end(),s)==L.end()) {
    L.push_back(s);
    writeAutoStartList(L);
  }
  server.send(200,"text/plain","ok");
}

void handleAutoStartRemove() {
  if (!server.hasArg("script")) return server.send(400);
  String s = server.arg("script");
  auto L = readAutoStartList();
  L.erase(std::remove(L.begin(),L.end(),s),L.end());
  writeAutoStartList(L);
  server.send(200,"text/plain","ok");
}

void handleGetAutoStart() {
  auto L = readAutoStartList();
  String j = "[";
  for (auto &s: L) j += "\"" + s + "\",";
  if (j.endsWith(",")) j.remove(j.length()-1);
  j += "]";
  server.send(200,"application/json",j);
}

void handleRunScript() {
  if (!server.hasArg("script")) return server.send(400);
  runScript(server.arg("script"));
  server.send(200,"text/plain","Script triggered");
}

void loadAutoStart() {
  // Check if the autostart configuration exists
  if (!LittleFS.exists(AUTOSTART_CONFIG)) {
    Serial.println("AutoStart config not found");
    return;
  }

  // Open the configuration file for reading
  File cfg = LittleFS.open(AUTOSTART_CONFIG, "r");
  if (!cfg) {
    Serial.println("Failed to open autostart.txt");
    return;
  }

  // Read and process each line
  while (cfg.available()) {
    String line = cfg.readStringUntil('\n');
    line.trim();

    // Skip blank lines or comments
    if (line.length() == 0 || line.startsWith("//")) {
      continue;
    }

    // Log and run each script
    Serial.print("AutoStarted: ");
    Serial.println(line);
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

void runCommand(const String& cmdLine) {
  if (scriptRunning) return;
  String cmd; ArgList args;
  if (!parseLine(cmdLine, cmd, args)) {
    logMsg((String("runCommand: parse error for line: ") + cmdLine).c_str());
    return;
  }
  auto it = commands.find(cmd);
  if (it != commands.end()) {
    it->second(args);
  } else {
    logMsg((String("runCommand: unknown command: ") + cmd).c_str());
  }
}

void initScriptEngine() {
  commands["logmsg"] = [](auto&A){ logMsg(A[0].c_str()); };
  commands["httpget"] = [](auto&A){
    HTTPClient h; String u=A[0]; u.replace("\"","");
    if (h.begin(httpClient,u)) { int code=h.GET(); logMsg(("GET="+String(code)).c_str()); h.end(); }
    else logMsg("GET: begin() failed");
  };
  commands["httppost"] = [](auto&A){
    HTTPClient h; String u=A[0],b=A[1]; u.replace("\"",""); b.replace("\"","");
    if (h.begin(httpClient,u)) { h.addHeader("Content-Type","application/json"); int code=h.POST(b); logMsg(("POST="+String(code)).c_str()); h.end(); }
    else logMsg("POST: begin() failed");
  };
  commands["setpin"] = [](auto&A){
    int p=A[0].toInt();
    digitalWrite(p, A[1]=="HIGH"?HIGH:LOW);
  };
  commands["readpin"] = [](auto&A){
    int p=A[0].toInt();
    logMsg(("PIN"+String(p)+"="+String(digitalRead(p))).c_str());
  };
  commands["pinmode"] = [](auto&A){
    int p=A[0].toInt();
    pinMode(p, A[1]=="input"?INPUT:OUTPUT);
    logMsg(("PinMode "+String(p)+"="+A[1]).c_str());
  };
  commands["delayms"] = [](auto&A){ delay(A[0].toInt()); };
  commands["writedata"] = [](auto&A){
    String p=A[0],d=A[1]; p.replace("\"",""); d.replace("\"","");
    if (LittleFS.exists(p)) LittleFS.remove(p);
    File f=LittleFS.open(p,"w"); if (f) { f.print(d); f.close(); }
    logMsg(("writedata "+p).c_str());
  };
  commands["listcmds"] = [](auto&){
    String all;
    for (auto&kv:commands) { if (all.length()) all+=", "; all+=kv.first; }
    logMsg(("cmds: "+all).c_str());
  };
  commands["clearlogs"] = [](auto&){
    if (LittleFS.exists("/logs.txt")) LittleFS.remove("/logs.txt");
    logMsg("Logs cleared");
  };
  commands["rm"] = [](auto&A){
    String p=A[0]; p.replace("\"","");
    logMsg(removeRecursive(p)?"rm OK":"rm fail");
  };
  commands["serverbind"] = [](auto&A){
    String u=A[0],t=A[1]; u.replace("\"",""); t.replace("\"","");
    if (!u.startsWith("/")) u="/"+u;
    appendEndpoint(u,t);
    logMsg(("Bind "+u+"->"+t).c_str());
  };
  commands["serverunbind"] = [](auto&A){
    String u=A[0]; u.replace("\"","");
    if (!u.startsWith("/")) u="/"+u;
    removeEndpoint(u);
    logMsg(("Unbind "+u).c_str());
  };
    commands["togglepin"] = [](auto&A){
    int p=A[0].toInt();
    int currentstate = digitalRead(p);
    digitalWrite(p, currentstate==0?HIGH:LOW  );
  };
}
