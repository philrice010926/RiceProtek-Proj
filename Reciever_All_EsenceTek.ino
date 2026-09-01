#include <WiFi.h>
#include <HTTPClient.h>
#include <vector>  // For SMS queue

// WiFi credentials
const char* ssid = "";
const char* password = "";

// Google Sheets URL
String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbyjewCVsj0IYu4FOyi_UkSnHYKeEbah6eCdcPvdCWqqa64uu03JEyyFctOyldPTv-Ip/exec";

// A7670E Module pins
#define MODEM_RX_PIN 27
#define MODEM_TX_PIN 26

// SMS Queue
std::vector<String> smsQueue;
bool processingSMS = false;
int successCount = 0;

// Buffer for incoming direct SMS
String directSMSBuffer = "";

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  
  delay(15000);
  
  Serial.println("\n\n=================================");
  Serial.println("TTGO T-SIM A7670E R2 SMS to Google Sheets");
  Serial.println("=================================\n");
  
  connectToWiFi();
  initModem();
  setupSMS();
  
  Serial.println("\n📱 READY! Waiting for SMS messages...");
  Serial.println("Format: D1 M=84.2 ST=26.5 EC=1144 PH=6.3 N=200 P=508 K=504 AT=26.5 H=88.9 WD=20.2 WS=0 DIR=N");
  Serial.println("Multiple SMS will be queued and processed one by one\n");
}

void loop() {
  // Read all available data from modem
  while (Serial1.available()) {
    String data = Serial1.readString();
    directSMSBuffer += data;
  }
  
  // Process buffer for complete SMS messages
  if (directSMSBuffer.length() > 0) {
    processDirectSMSBuffer();
  }
  
  // Process queue if not already processing
  if (!processingSMS && smsQueue.size() > 0) {
    processNextSMS();
  }
  
  // Manual test input
  if (Serial.available()) {
    String testData = Serial.readStringUntil('\n');
    testData.trim();
    if (testData.length() > 0) {
      Serial.println("\n📨 Test: " + testData);
      parseAndSendData(testData);
    }
  }
  
  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("📡 WiFi reconnecting...");
    WiFi.reconnect();
  }
  
  delay(100);
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected");
  } else {
    Serial.println("\n❌ WiFi Connection Failed!");
  }
}

void initModem() {
  Serial.println("\n🔧 Initializing A7670E module...");
  
  sendATCommand("AT", "OK", 2000);
  sendATCommand("AT+CPIN?", "READY", 5000);
  sendATCommand("AT+CSQ", "+CSQ:", 2000);
  
  Serial.println("✅ Modem ready");
}

void setupSMS() {
  Serial.println("\n📱 Setting up SMS...");
  
  sendATCommand("AT+CMGF=1", "OK", 5000);  // Text mode
  
  // Use mode 2,1 for both notifications AND direct SMS
  sendATCommand("AT+CNMI=2,1,0,0,0", "OK", 5000);
  
  // Clear all old messages
  Serial.println("🧹 Clearing old messages...");
  sendATCommand("AT+CMGD=1,4", "OK", 5000);
  
  Serial.println("✅ SMS ready");
}

bool sendATCommand(String command, String expectedResponse, int timeout) {
  Serial1.println(command);
  
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    if (Serial1.available()) {
      String response = Serial1.readString();
      if (response.indexOf(expectedResponse) >= 0) {
        return true;
      }
    }
    delay(100);
  }
  return false;
}

void processDirectSMSBuffer() {
  // Look for complete SMS in buffer
  // Format: +CMT: "+639761778023",,"24/03/17,12:34:56+08"\nMESSAGE
  
  int startPos = 0;
  
  while (true) {
    // Find +CMT: header
    int cmtPos = directSMSBuffer.indexOf("+CMT:", startPos);
    if (cmtPos < 0) break;
    
    // Find the end of this SMS (next +CMT: or end of buffer)
    int nextCmt = directSMSBuffer.indexOf("+CMT:", cmtPos + 5);
    int endPos;
    
    if (nextCmt > cmtPos) {
      endPos = nextCmt;
    } else {
      endPos = directSMSBuffer.length();
    }
    
    // Extract this SMS
    String smsBlock = directSMSBuffer.substring(cmtPos, endPos);
    
    // Parse the SMS
    parseDirectSMS(smsBlock);
    
    startPos = endPos;
  }
  
  // Keep any incomplete data at the end
  if (startPos < directSMSBuffer.length()) {
    directSMSBuffer = directSMSBuffer.substring(startPos);
  } else {
    directSMSBuffer = "";
  }
}

void parseDirectSMS(String smsBlock) {
  Serial.println("\n📨 Processing direct SMS...");
  
  // Extract sender (between quotes)
  int firstQuote = smsBlock.indexOf('"');
  int secondQuote = smsBlock.indexOf('"', firstQuote + 1);
  String sender = smsBlock.substring(firstQuote + 1, secondQuote);
  
  // Find message (after the newline)
  int newlinePos = smsBlock.indexOf('\n', secondQuote);
  if (newlinePos > 0) {
    String message = smsBlock.substring(newlinePos + 1);
    
    // Clean message
    message.trim();
    message.replace("\r", "");
    message.replace("\n", " ");
    
    Serial.println("  From: " + sender);
    Serial.println("  Msg: " + message);
    
    if (message.startsWith("D")) {
      Serial.println("✅ Valid format!");
      parseAndSendData(message);
    } else {
      Serial.println("⚠️ Not in expected format");
    }
  }
}

void checkForNewSMS() {
  if (Serial1.available()) {
    String data = Serial1.readString();
    
    // Check for notifications
    if (data.indexOf("+CMTI:") >= 0) {
      int pos = 0;
      while (true) {
        int cmtiPos = data.indexOf("+CMTI:", pos);
        if (cmtiPos < 0) break;
        
        int commaPos = data.indexOf(',', cmtiPos);
        if (commaPos > 0) {
          String smsIndex = data.substring(commaPos + 1);
          
          // Clean up the index
          smsIndex.trim();
          smsIndex.replace("\r", "");
          smsIndex.replace("\n", "");
          
          // Add to queue
          if (smsIndex.length() > 0 && isDigit(smsIndex.charAt(0))) {
            bool alreadyQueued = false;
            for (int i = 0; i < smsQueue.size(); i++) {
              if (smsQueue[i] == smsIndex) {
                alreadyQueued = true;
                break;
              }
            }
            
            if (!alreadyQueued) {
              smsQueue.push_back(smsIndex);
              Serial.println("📨 Added SMS " + smsIndex + " to queue. Queue size: " + String(smsQueue.size()));
            }
          }
        }
        
        pos = cmtiPos + 6;
      }
    }
  }
}

void processNextSMS() {
  if (smsQueue.size() == 0) return;
  
  processingSMS = true;
  
  // Get next SMS index
  String smsIndex = smsQueue[0];
  smsQueue.erase(smsQueue.begin());
  
  Serial.println("\n📤 Processing SMS " + smsIndex + " (" + String(smsQueue.size()) + " remaining)");
  
  // Read and process
  readAndProcessSMS(smsIndex);
  
  processingSMS = false;
}

void readAndProcessSMS(String index) {
  Serial.println("📖 Reading SMS at index: " + index);
  
  // Clear buffer
  while (Serial1.available()) {
    Serial1.read();
  }
  
  // Read SMS
  Serial1.println("AT+CMGR=" + index);
  delay(3000);
  
  String response = "";
  unsigned long timeout = millis() + 8000;
  
  while (millis() < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      response += c;
    }
    delay(50);
  }
  
  // Parse the response
  if (response.indexOf("+CMGR:") >= 0) {
    String message = extractMessage(response);
    
    Serial.println("  Message: " + message);
    
    if (message.length() > 0 && message.startsWith("D")) {
      Serial.println("✅ Valid format!");
      parseAndSendData(message);
    }
  }
  
  // Delete after processing
  deleteSMS(index);
}

String extractMessage(String response) {
  String message = "";
  
  // Find message start (after second newline)
  int newlineCount = 0;
  int msgStart = 0;
  for (int i = 0; i < response.length(); i++) {
    if (response[i] == '\n') {
      newlineCount++;
      if (newlineCount == 2) {
        msgStart = i + 1;
        break;
      }
    }
  }
  
  if (msgStart > 0) {
    int msgEnd = response.indexOf("OK", msgStart);
    if (msgEnd > msgStart) {
      message = response.substring(msgStart, msgEnd);
    } else {
      message = response.substring(msgStart);
    }
    
    // Clean message
    message.trim();
    message.replace("\r", "");
    message.replace("\n", " ");
    message.replace("@", "");
  }
  
  return message;
}

void deleteSMS(String index) {
  Serial.println("🗑️ Deleting SMS at index: " + index);
  Serial1.println("AT+CMGD=" + index);
  delay(500);
  while (Serial1.available()) {
    Serial1.read();
  }
}

void parseAndSendData(String message) {
  Serial.println("🔍 Parsing data...");
  
  // Initialize variables
  String device = "1";
  float moisture = 0;
  float soilTemp = 0;
  float ec = 0;
  float ph = 0;
  int nitrogen = 0;
  int phosphorus = 0;
  int potassium = 0;
  float airTemp = 0;
  float humidity = 0;
  float waterDistance = 0;
  float windSpeed = 0;
  String windDirection = "N";
  String timestamp = "";
  
  // Parse short format
  if (message.startsWith("D")) {
    int spaceAfterD = message.indexOf(" ");
    if (spaceAfterD > 1) {
      device = message.substring(1, spaceAfterD);
    }
  }
  
  // Extract parameters
  moisture = extractValueShort(message, "M=");
  soilTemp = extractValueShort(message, "ST=");
  ec = extractValueShort(message, "EC=");
  ph = extractValueShort(message, "PH=");
  nitrogen = extractIntValueShort(message, "N=");
  phosphorus = extractIntValueShort(message, "P=");
  potassium = extractIntValueShort(message, "K=");
  airTemp = extractValueShort(message, "AT=");
  humidity = extractValueShort(message, "H=");
  waterDistance = extractValueShort(message, "WD=");
  windSpeed = extractValueShort(message, "WS=");
  windDirection = extractStringValueShort(message, "DIR=");
  timestamp = extractStringValueShort(message, "TS=");
  
  // Display parsed values
  Serial.println("✅ Parsed:");
  Serial.print("  D" + device);
  Serial.print(" M=" + String(moisture));
  Serial.print(" ST=" + String(soilTemp));
  Serial.print(" EC=" + String(ec));
  Serial.print(" PH=" + String(ph));
  Serial.print(" N=" + String(nitrogen));
  Serial.print(" P=" + String(phosphorus));
  Serial.print(" K=" + String(potassium));
  Serial.print(" AT=" + String(airTemp));
  Serial.print(" H=" + String(humidity));
  Serial.print(" WD=" + String(waterDistance));
  Serial.print(" WS=" + String(windSpeed));
  Serial.print(" DIR=" + windDirection);
  Serial.println(" TS=" + timestamp);
  
  // Send to Google Sheets
  sendToGoogleSheets(device, moisture, soilTemp, ec, ph, nitrogen, 
                     phosphorus, potassium, airTemp, humidity, 
                     waterDistance, windSpeed, windDirection, timestamp);
}

// Parsers
float extractValueShort(String message, String key) {
  int keyIndex = message.indexOf(key);
  if (keyIndex >= 0) {
    int start = keyIndex + key.length();
    int end = message.indexOf(" ", start);
    if (end < 0) end = message.length();
    
    String valueStr = message.substring(start, end);
    valueStr.trim();
    return valueStr.toFloat();
  }
  return 0;
}

int extractIntValueShort(String message, String key) {
  int keyIndex = message.indexOf(key);
  if (keyIndex >= 0) {
    int start = keyIndex + key.length();
    int end = message.indexOf(" ", start);
    if (end < 0) end = message.length();
    
    String valueStr = message.substring(start, end);
    valueStr.trim();
    return valueStr.toInt();
  }
  return 0;
}

String extractStringValueShort(String message, String key) {
  int keyIndex = message.indexOf(key);
  if (keyIndex >= 0) {
    int start = keyIndex + key.length();
    int end = message.indexOf(" ", start);
    if (end < 0) end = message.length();
    
    String valueStr = message.substring(start, end);
    valueStr.trim();
    return valueStr;
  }
  return "";
}

void sendToGoogleSheets(String device, float moisture, float soilTemp, float ec, float ph,
                        int nitrogen, int phosphorus, int potassium, float airTemp,
                        float humidity, float waterDistance, float windSpeed,
                        String windDirection, String timestamp) {
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected.");
    return;
  }
  
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  
  http.begin(GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/json");
  
  String json = "{";
  json += "\"device\":\"" + device + "\",";
  json += "\"moisture\":" + String(moisture) + ",";
  json += "\"soilTemp\":" + String(soilTemp) + ",";
  json += "\"ec\":" + String(ec) + ",";
  json += "\"ph\":" + String(ph) + ",";
  json += "\"nitrogen\":" + String(nitrogen) + ",";
  json += "\"phosphorus\":" + String(phosphorus) + ",";
  json += "\"potassium\":" + String(potassium) + ",";
  json += "\"airTemp\":" + String(airTemp) + ",";
  json += "\"humidity\":" + String(humidity) + ",";
  json += "\"waterDistance\":" + String(waterDistance) + ",";
  json += "\"windSpeed\":" + String(windSpeed) + ",";
  json += "\"windDirection\":\"" + windDirection + "\",";
  json += "\"original_timestamp\":\"" + timestamp + "\"";
  json += "}";
  
  int httpResponseCode = http.POST(json);
  
  if (httpResponseCode == 200 || httpResponseCode == 302) {
    successCount++;
    Serial.println("✅ Sent to Sheets! (Total: " + String(successCount) + ")");
  }
  
  http.end();
}
