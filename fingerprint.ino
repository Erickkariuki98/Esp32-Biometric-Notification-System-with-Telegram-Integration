#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// -------- CONFIG --------
const char* TELEGRAM_BOT_TOKEN = "your bot token";
const char* TELEGRAM_CHAT_ID     ="your chat id or a telegram channel chat id";
const char* SHEETS_WEBAPP_URL   = "link to the google api script linked to your google sheets";
const char* SHEETS_SECRET       = "secret phrase for securing your google sheets";

WiFiMulti wifiMulti;
WebServer server(80);

const long GMT_OFFSET_SEC = 3 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

#define BUZZER_PIN 18
#define FP_RX_PIN 16
#define FP_TX_PIN 17

#define MAX_EMPLOYEES 50
#define MAX_LOGS 50

String employeeNames[MAX_EMPLOYEES];

// Log storage
struct LogEntry {
  String name;
  String timestamp;
  String action;
};
LogEntry recentLogs[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

// Enrollment state
bool enrollmentMode = false;
int enrollmentID = 0;
String enrollmentName = "";
int enrollmentStep = 0;

Preferences prefs;
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);

LiquidCrystal_I2C *lcd = nullptr;
uint8_t lcdAddr = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", GMT_OFFSET_SEC, 60000);

// ---------- UTILITIES ----------
uint8_t scanI2C() {
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0;
}

void shortBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}

void longBeep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(700);
  digitalWrite(BUZZER_PIN, LOW);
}

void doubleBeep() {
  shortBeep();
  delay(100);
  shortBeep();
}

String urlencode(const String &str) {
  String out;
  char c;
  for (size_t i = 0; i < str.length(); i++) {
    c = str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[5];
      sprintf(buf, "%%%02X", (uint8_t)c);
      out += buf;
    }
  }
  return out;
}

String escapeJson(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' ) o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

String escapeHtml(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String getTimeString() {
  if (!timeClient.update()) timeClient.forceUpdate();
  time_t epoch = timeClient.getEpochTime();
  struct tm timeinfo;
  localtime_r(&epoch, &timeinfo);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void addLog(const String &name, const String &action, const String &timestamp) {
  recentLogs[logIndex].name = name;
  recentLogs[logIndex].action = action;
  recentLogs[logIndex].timestamp = timestamp;
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
}

void updateLCD(const String &line1, const String &line2 = "") {
  if (!lcd) return;
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(line1.substring(0, 16));
  if (line2.length() > 0) {
    lcd->setCursor(0, 1);
    lcd->print(line2.substring(0, 16));
  }
}

// ---------- NETWORK ----------
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  for (int i=0; i<6; ++i) {
    if (wifiMulti.run() == WL_CONNECTED) return true;
    delay(500);
  }
  return false;
}

bool sendTelegram(const String &name, const String &timestamp, bool accessGranted = true) {
  if (!ensureWiFi()) return false;

  String status = accessGranted ? "✅ *Status:* Access Granted" : "❌ *Status:* Access Denied";
  String message = "👤 *User:* " + name + "\n⏰ *Time:* " + timestamp + "\n" + status;

  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendMessage";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return false;

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String payload = "chat_id=" + String(TELEGRAM_CHAT_ID) + "&parse_mode=Markdown&text=" + urlencode(message);

  int code = https.POST(payload);
  https.end();
  return (code > 0 && code < 400);
}

bool sendToSheet(const String &name, const String &action, const String &timestamp) {
  if (!ensureWiFi()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, SHEETS_WEBAPP_URL)) return false;
  https.addHeader("Content-Type", "application/json");
  String body = "{\"secret\":\"" + String(SHEETS_SECRET) + "\",\"name\":\"" + escapeJson(name) + "\",\"action\":\"" + escapeJson(action) + "\",\"timestamp\":\"" + escapeJson(timestamp) + "\"}";
  int code = https.POST(body);
  https.end();
  return (code > 0 && code < 400);
}

// ---------- FINGERPRINT ----------
int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

// ---------- PERSISTENCE ----------
void saveName(uint8_t id, const String &name) {
  prefs.begin("employees", false);
  prefs.putString(String(id).c_str(), name);
  prefs.end();
}

void removeName(uint8_t id) {
  prefs.begin("employees", false);
  prefs.remove(String(id).c_str());
  prefs.end();
}

void loadNames() {
  prefs.begin("employees", true);
  for (int i = 1; i < MAX_EMPLOYEES; i++) {
    String s = prefs.getString(String(i).c_str(), "");
    if (s.length()) {
      employeeNames[i] = s;
      Serial.printf("Loaded %d -> %s\n", i, s.c_str());
    }
  }
  prefs.end();
}

// ---------- WEB SERVER HANDLERS ----------

String getHTMLHeader(const String &title) {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>" + title + "</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;max-width:900px;margin:20px auto;padding:20px;background:#f5f5f5;}";
  html += "h1{color:#333;border-bottom:3px solid #4CAF50;padding-bottom:10px;}";
  html += "h2{color:#555;margin-top:30px;}";
  html += ".nav{background:#4CAF50;padding:15px;border-radius:5px;margin-bottom:20px;}";
  html += ".nav a{color:white;text-decoration:none;margin-right:20px;font-weight:bold;}";
  html += ".nav a:hover{text-decoration:underline;}";
  html += ".card{background:white;padding:20px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  html += ".status-good{color:#4CAF50;font-weight:bold;}";
  html += ".status-bad{color:#f44336;font-weight:bold;}";
  html += "table{width:100%;border-collapse:collapse;margin-top:10px;}";
  html += "th,td{padding:12px;text-align:left;border-bottom:1px solid #ddd;}";
  html += "th{background:#4CAF50;color:white;}";
  html += "tr:hover{background:#f5f5f5;}";
  html += "input,select,button{padding:10px;margin:5px 0;font-size:16px;border:1px solid #ddd;border-radius:4px;}";
  html += "button{background:#4CAF50;color:white;border:none;cursor:pointer;font-weight:bold;}";
  html += "button:hover{background:#45a049;}";
  html += ".delete-btn{background:#f44336;}";
  html += ".delete-btn:hover{background:#da190b;}";
  html += ".alert{padding:15px;margin:10px 0;border-radius:5px;}";
  html += ".alert-success{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}";
  html += ".alert-error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;}";
  html += ".alert-info{background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb;}";
  html += ".enroll-step{font-size:18px;font-weight:bold;color:#4CAF50;margin:20px 0;}";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='nav'>";
  html += "<a href='/'>Dashboard</a>";
  html += "<a href='/enroll'>Enroll Fingerprint</a>";
  html += "<a href='/employees'>Manage Employees</a>";
  html += "<a href='/logs'>Recent Logs</a>";
  html += "</div>";
  
  html += "<h1>" + title + "</h1>";
  return html;
}

String getHTMLFooter() {
  return "</body></html>";
}

void handleRoot() {
  String html = getHTMLHeader("Fingerprint Attendance System");
  
  html += "<div class='card'>";
  html += "<h2>System Status</h2>";
  html += "<p><strong>WiFi:</strong> <span class='status-good'>" + String(WiFi.SSID()) + "</span></p>";
  html += "<p><strong>IP Address:</strong> " + WiFi.localIP().toString() + "</p>";
  html += "<p><strong>Current Time:</strong> " + getTimeString() + "</p>";
  
  finger.getTemplateCount();
  html += "<p><strong>Enrolled Fingerprints:</strong> " + String(finger.templateCount) + "</p>";
  html += "<p><strong>Fingerprint Sensor:</strong> <span class='status-good'>Connected</span></p>";
  html += "</div>";
  
  html += "<div class='card'>";
  html += "<h2>Recent Activity</h2>";
  if (logCount == 0) {
    html += "<p>No recent activity</p>";
  } else {
    html += "<table><tr><th>Name</th><th>Action</th><th>Time</th></tr>";
    int start = (logIndex - logCount + MAX_LOGS) % MAX_LOGS;
    for (int i = 0; i < min(10, logCount); i++) {
      int idx = (start + logCount - 1 - i + MAX_LOGS) % MAX_LOGS;
      html += "<tr>";
      html += "<td>" + escapeHtml(recentLogs[idx].name) + "</td>";
      html += "<td>" + escapeHtml(recentLogs[idx].action) + "</td>";
      html += "<td>" + escapeHtml(recentLogs[idx].timestamp) + "</td>";
      html += "</tr>";
    }
    html += "</table>";
    html += "<p style='margin-top:15px;'><a href='/logs'>View all logs →</a></p>";
  }
  html += "</div>";
  
  html += getHTMLFooter();
  server.send(200, "text/html", html);
}

void handleEnroll() {
  String html = getHTMLHeader("Enroll Fingerprint");
  
  if (enrollmentMode) {
    html += "<div class='alert alert-info'>";
    html += "<div class='enroll-step'>Enrollment in Progress...</div>";
    html += "<p><strong>ID:</strong> " + String(enrollmentID) + "</p>";
    html += "<p><strong>Name:</strong> " + escapeHtml(enrollmentName) + "</p>";
    
    if (enrollmentStep == 0) {
      html += "<p>⏳ Waiting for first fingerprint scan...</p>";
    } else if (enrollmentStep == 1) {
      html += "<p>✓ First scan complete! Remove finger and scan again...</p>";
    } else if (enrollmentStep == 2) {
      html += "<p>✓ Enrollment successful!</p>";
      html += "<script>setTimeout(function(){window.location.href='/enroll';}, 2000);</script>";
    }
    
    html += "<form action='/cancel-enroll' method='POST'>";
    html += "<button type='submit' class='delete-btn'>Cancel Enrollment</button>";
    html += "</form>";
    html += "</div>";
  } else {
    html += "<div class='card'>";
    html += "<h2>Start New Enrollment</h2>";
    html += "<form action='/start-enroll' method='POST'>";
    html += "<p><label>Employee ID (1-49):</label><br>";
    html += "<input type='number' name='id' min='1' max='49' required></p>";
    html += "<p><label>Employee Name:</label><br>";
    html += "<input type='text' name='name' required></p>";
    html += "<button type='submit'>Start Enrollment</button>";
    html += "</form>";
    html += "</div>";
  }
  
  html += "<script>";
  if (enrollmentMode) {
    html += "setTimeout(function(){window.location.reload();}, 2000);";
  }
  html += "</script>";
  
  html += getHTMLFooter();
  server.send(200, "text/html", html);
}

void handleStartEnroll() {
  if (server.hasArg("id") && server.hasArg("name")) {
    enrollmentID = server.arg("id").toInt();
    enrollmentName = server.arg("name");
    
    if (enrollmentID < 1 || enrollmentID >= MAX_EMPLOYEES) {
      server.send(400, "text/html", getHTMLHeader("Error") + "<div class='alert alert-error'>Invalid ID</div>" + getHTMLFooter());
      return;
    }
    
    enrollmentMode = true;
    enrollmentStep = 0;
    updateLCD("Enroll ID:" + String(enrollmentID), enrollmentName);
    server.sendHeader("Location", "/enroll");
    server.send(303);
  } else {
    server.send(400, "text/html", getHTMLHeader("Error") + "<div class='alert alert-error'>Missing parameters</div>" + getHTMLFooter());
  }
}

void handleCancelEnroll() {
  enrollmentMode = false;
  enrollmentStep = 0;
  updateLCD("Ready", "");
  server.sendHeader("Location", "/enroll");
  server.send(303);
}

void handleEmployees() {
  String html = getHTMLHeader("Manage Employees");
  
  html += "<div class='card'>";
  html += "<h2>Enrolled Employees</h2>";
  
  bool hasEmployees = false;
  html += "<table><tr><th>ID</th><th>Name</th><th>Action</th></tr>";
  
  for (int i = 1; i < MAX_EMPLOYEES; i++) {
    if (employeeNames[i].length() > 0) {
      hasEmployees = true;
      html += "<tr>";
      html += "<td>" + String(i) + "</td>";
      html += "<td>" + escapeHtml(employeeNames[i]) + "</td>";
      html += "<td><form action='/delete-employee' method='POST' style='margin:0;'>";
      html += "<input type='hidden' name='id' value='" + String(i) + "'>";
      html += "<button type='submit' class='delete-btn' onclick='return confirm(\"Delete " + escapeHtml(employeeNames[i]) + "?\")'>Delete</button>";
      html += "</form></td>";
      html += "</tr>";
    }
  }
  
  if (!hasEmployees) {
    html += "<tr><td colspan='3'>No employees enrolled</td></tr>";
  }
  
  html += "</table>";
  html += "</div>";
  
  html += getHTMLFooter();
  server.send(200, "text/html", html);
}

void handleDeleteEmployee() {
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    if (id > 0 && id < MAX_EMPLOYEES) {
      int p = finger.deleteModel(id);
      if (p == FINGERPRINT_OK) {
        employeeNames[id] = "";
        removeName(id);
      }
    }
  }
  server.sendHeader("Location", "/employees");
  server.send(303);
}

void handleLogs() {
  String html = getHTMLHeader("Recent Logs");
  
  html += "<div class='card'>";
  html += "<h2>All Recent Activity</h2>";
  
  if (logCount == 0) {
    html += "<p>No logs available</p>";
  } else {
    html += "<table><tr><th>Name</th><th>Action</th><th>Timestamp</th></tr>";
    int start = (logIndex - logCount + MAX_LOGS) % MAX_LOGS;
    for (int i = 0; i < logCount; i++) {
      int idx = (start + logCount - 1 - i + MAX_LOGS) % MAX_LOGS;
      html += "<tr>";
      html += "<td>" + escapeHtml(recentLogs[idx].name) + "</td>";
      html += "<td>" + escapeHtml(recentLogs[idx].action) + "</td>";
      html += "<td>" + escapeHtml(recentLogs[idx].timestamp) + "</td>";
      html += "</tr>";
    }
    html += "</table>";
    html += "<p style='margin-top:15px;'>Showing " + String(logCount) + " most recent entries</p>";
  }
  html += "</div>";
  
  html += getHTMLFooter();
  server.send(200, "text/html", html);
}

void handleNotFound() {
  server.send(404, "text/html", getHTMLHeader("404 Not Found") + "<div class='alert alert-error'>Page not found</div>" + getHTMLFooter());
}

// ---------- ENROLLMENT PROCESS ----------
void processEnrollment() {
  if (!enrollmentMode) return;
  
  if (enrollmentStep == 0) {
    // First scan
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        enrollmentStep = 1;
        updateLCD("Remove finger", "Scan again");
        shortBeep();
        delay(1000);
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(100);
      }
    }
  } else if (enrollmentStep == 1) {
    // Second scan
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz(2);
      if (p == FINGERPRINT_OK) {
        p = finger.createModel();
        if (p == FINGERPRINT_OK) {
          p = finger.storeModel(enrollmentID);
          if (p == FINGERPRINT_OK) {
            employeeNames[enrollmentID] = enrollmentName;
            saveName(enrollmentID, enrollmentName);
            enrollmentStep = 2;
            updateLCD("Success!", enrollmentName);
            doubleBeep();
            delay(2000);
            enrollmentMode = false;
            enrollmentStep = 0;
            updateLCD("Ready", "");
          } else {
            longBeep();
            enrollmentMode = false;
            updateLCD("Error!", "Try again");
            delay(2000);
            updateLCD("Ready", "");
          }
        }
      }
    }
  }
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  Wire.begin();
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  lcdAddr = scanI2C();
  if (lcdAddr) {
    lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
    lcd->init();
    lcd->backlight();
    updateLCD("Starting...", "");
    shortBeep();
  }

  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  if (!finger.verifyPassword()) {
    Serial.println("Fingerprint sensor not found - HALT");
    updateLCD("FP Error!", "");
    longBeep();
    while (1) delay(1);
  }

  loadNames();

  wifiMulti.addAP("wifi ssid", "wifi password");
 // you can add more wifi networks here

  Serial.println("Connecting WiFi...");
  updateLCD("Connecting WiFi", "");
  unsigned long start = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: " + WiFi.SSID());
    Serial.println("IP: " + WiFi.localIP().toString());
    
    // Setup mDNS
    if (MDNS.begin("fingerprint")) {
      Serial.println("mDNS: http://fingerprint.local");
    }
    
    // Setup web server routes
    server.on("/", handleRoot);
    server.on("/enroll", handleEnroll);
    server.on("/start-enroll", HTTP_POST, handleStartEnroll);
    server.on("/cancel-enroll", HTTP_POST, handleCancelEnroll);
    server.on("/employees", handleEmployees);
    server.on("/delete-employee", HTTP_POST, handleDeleteEmployee);
    server.on("/logs", handleLogs);
    server.onNotFound(handleNotFound);
    
    server.begin();
    Serial.println("Web server started");
    
    updateLCD("Ready", WiFi.localIP().toString());
  } else {
    Serial.println("WiFi not connected");
    updateLCD("WiFi Failed", "");
  }

  timeClient.begin();
  timeClient.update();
  Serial.println("Time: " + getTimeString());
  Serial.println("System ready");
  doubleBeep();
}

// ---------- LOOP ----------
void loop() {
  if (WiFi.status() != WL_CONNECTED) wifiMulti.run();
  
  server.handleClient();
  
  // Handle enrollment if in progress
  if (enrollmentMode) {
    processEnrollment();
    delay(200);
    return;
  }
  
  // Normal fingerprint scanning
  int id = getFingerprintID();
  if (id >= 0) {
    String name = employeeNames[id].length() ? employeeNames[id] : ("ID " + String(id));
    String ts = getTimeString();
    
    Serial.printf("Matched %s at %s\n", name.c_str(), ts.c_str());
    updateLCD("Welcome!", name);
    
    bool telegramSent = sendTelegram(name, ts, true);
    bool sheetsSent = sendToSheet(name, "Entered", ts);
    
    addLog(name, "Entered", ts);
    
    doubleBeep();
    delay(2000);
    updateLCD("Ready", WiFi.localIP().toString());
    delay(500);
  }
  
  delay(100);
}