#include <Wire.h>
#include <ModbusMaster.h>
#include <TimeLib.h>
#include <Ticker.h>
#include <math.h>
#include <OneWire.h>
#include "DFRobot_SHT20.h"

// Soil Sensor
#define RS485_DE 18
#define RS485_RE 19
#define UART2_TX 33
#define UART2_RX 32

// For A7670C - Using direct AT commands (No TinyGSM)
#define MODEM_RST 5
#define MODEM_TX 17
#define MODEM_RX 16
#define MODEM_PWR 14  // Add power control pin if connected

// Temp and Humid
#define I2C_SDA 21
#define I2C_SCL 22

DFRobot_SHT20 sht20;

#define SOIL_SENSOR_MOSFET 27
#define All_5V_SENSOR 26

// Ultrasonic Sensor
const int TRIG_PIN = 13;
const int ECHO_PIN = 4;

float Airtemp = 0.0;
float Humidity = 0.0;

// Serial
#define SerialMon Serial
#define SerialAT Serial1

const char* SMS_TARGETS[] = {"+639554132471","+639057496813", "+639761778023"};
const int NUM_TARGETS = sizeof(SMS_TARGETS) / sizeof(SMS_TARGETS[0]);
#define SMS_DEVICE "D8"

ModbusMaster soilNode;
ModbusMaster windNode;

void preTransmission() {
  digitalWrite(RS485_RE, HIGH);
  digitalWrite(RS485_DE, HIGH);
}

void postTransmission() {
  digitalWrite(RS485_RE, LOW);
  digitalWrite(RS485_DE, LOW);
}

#define MAX_RETRIES 3
#define RETRY_DELAY 5000

// Direct AT command functions
bool sendATCommand(String command, String expectedResponse, int timeout) {
  SerialAT.println(command);
  
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    if (SerialAT.available()) {
      String response = SerialAT.readString();
      if (response.indexOf(expectedResponse) >= 0) {
        return true;
      }
    }
    delay(100);
  }
  return false;
}

bool initModem() {
  SerialMon.println("Initializing A7670C...");
  
  // Hardware reset with longer delay
  digitalWrite(MODEM_RST, LOW);
  //delay(1000);original
  delay(2500);
  digitalWrite(MODEM_RST, HIGH);
  //delay(5000);  // Increased from 3000ms
  delay(8000);
  
  // Flush any pending data
  while(SerialAT.available()) {
    SerialAT.read();
  }
  
  // Test AT commands with multiple attempts
  bool atOK = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (sendATCommand("AT", "OK", 3000)) {
      atOK = true;
      break;
    }
    SerialMon.printf("AT command attempt %d failed\n", attempt + 1);
    delay(1000);
  }
  
  if (!atOK) {
    SerialMon.println("Modem not responding");
    return false;
  }
  
  // Check SIM with retry
  bool simOK = false;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (sendATCommand("AT+CPIN?", "READY", 5000)) {
      simOK = true;
      break;
    }
    SerialMon.printf("SIM check attempt %d failed\n", attempt + 1);
    delay(1000);
  }
  
  if (!simOK) {
    SerialMon.println("SIM not ready");
    return false;
  }
  
  // Check signal
  sendATCommand("AT+CSQ", "+CSQ:", 2000);
  
  // Set SMS text mode
  if (!sendATCommand("AT+CMGF=1", "OK", 5000)) {
    SerialMon.println("Failed to set SMS mode");
    return false;
  }
  
  // Set SMS character set
  sendATCommand("AT+CSCS=\"GSM\"", "OK", 3000);
  
  // Check network registration
  sendATCommand("AT+CREG?", "+CREG:", 3000);
  sendATCommand("AT+CGREG?", "+CGREG:", 3000);
  
  SerialMon.println("Modem initialized successfully");
  return true;
}

bool sendSMS(String number, String message) {
  SerialMon.println("Sending SMS to: " + number);
  SerialMon.println("Message: " + message);
  
  // Ensure SerialAT is active
  if (!SerialAT) {
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(1000);
  }
  
  // Clear any pending data
  while(SerialAT.available()) {
    SerialAT.read();
  }
  
  // Send AT command to set recipient
  SerialAT.println("AT+CMGS=\"" + number + "\"");
  delay(1000);
  
  // Send message content
  SerialAT.print(message);
  delay(500);
  
  // Send Ctrl+Z (ASCII 26) to send SMS
  SerialAT.write(26);
  
  // Wait for response with extended timeout
  String response = "";
  unsigned long startTime = millis();
  while (millis() - startTime < 15000) {  // Increased from 8000ms
    while (SerialAT.available()) {
      char c = SerialAT.read();
      response += c;
      // Check for OK or error
      if (response.indexOf("OK") >= 0) {
        SerialMon.println("SMS sent successfully!");
        return true;
      }
      if (response.indexOf("ERROR") >= 0 || response.indexOf("CMS ERROR") >= 0) {
        SerialMon.println("SMS error: " + response);
        return false;
      }
    }
    delay(100);
  }
  
  SerialMon.println("SMS timeout - no response");
  return false;
}

bool sendSmsWithRetry(const char* target, const String& message) {
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    SerialMon.printf("Sending SMS to %s (Attempt %d/%d)...\n", target, attempt, MAX_RETRIES);
    
    if (sendSMS(target, message)) {
      return true;
    }
    
    if (attempt < MAX_RETRIES) {
      SerialMon.printf("Retrying in %d seconds...\n", RETRY_DELAY/1000);
      delay(RETRY_DELAY);
      
      // Re-initialize modem before retry
      if (!initModem()) {
        SerialMon.println("Modem re-init failed");
        return false;
      }
    }
  }
  return false;
}

bool sendSmsToAll(const String& message) {
  bool allSuccess = true;
  
  for (int i = 0; i < NUM_TARGETS; i++) {
    if (!sendSmsWithRetry(SMS_TARGETS[i], message)) {
      SerialMon.println("Failed to send to one recipient.");
      allSuccess = false;
    }
    delay(2000);  // Delay between recipients
  }
  
  return allSuccess;
}

void handleModbusData(float WindSpeed, float Airtemp, float Humidity) {
  
  float moisture = 0.0, temperature = 0.0, ec = 0.0, ph = 0.0;
  int nitrogen = 0, phosphorus = 0, potassium = 0;
  float distance = 0.00;
  
  bool modbusSuccess = false;
  
  // Turn on soil sensor
  digitalWrite(SOIL_SENSOR_MOSFET, HIGH);
  delay(2000);  // Give sensor time to stabilize
  
  // Read soil sensor (slave ID 1)
  uint8_t soilResult = soilNode.readHoldingRegisters(0x0000, 7);
  
  if (soilResult == soilNode.ku8MBSuccess) {
    modbusSuccess = true;
    moisture = soilNode.getResponseBuffer(0) / 10.0;
    temperature = soilNode.getResponseBuffer(1) / 10.0;
    ec = soilNode.getResponseBuffer(2);
    ph = soilNode.getResponseBuffer(3) / 10.0;
    nitrogen = soilNode.getResponseBuffer(4);
    phosphorus = soilNode.getResponseBuffer(5);
    potassium = soilNode.getResponseBuffer(6);
    
    SerialMon.println("Soil Modbus read successful.");
  } else {
    SerialMon.println("Soil Modbus read failed.");
    modbusSuccess = false;
  }
  
  // 3.5 second delay before reading wind sensor
  delay(3500);
  
  // Read wind sensor (slave ID 3)
  uint8_t windResult = windNode.readHoldingRegisters(0x0000, 1);
  
  if (windResult == windNode.ku8MBSuccess) {
    uint16_t rawSpeed = windNode.getResponseBuffer(0);
    WindSpeed = rawSpeed / 10.0;
    SerialMon.printf("Wind speed: %.2f m/s\n", WindSpeed);
  } else {
    SerialMon.println("Wind Modbus read failed.");
    WindSpeed = 0; // Error value
  }
  
  // Turn off soil sensor
  digitalWrite(SOIL_SENSOR_MOSFET, LOW);
  
  // ================= ULTRASONIC - TWO READINGS (USE SECOND) =================
  delay(500);
  
  float distance1 = 0.00;
  float distance2 = 0.00;

  // --- FIRST ULTRASONIC READING ---
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration1 = pulseIn(ECHO_PIN, HIGH, 30000);
  
  if (duration1 > 0) {
    distance1 = (duration1 / 2.0) / 29.1;
  }

  // Small delay between readings
  delay(500);

  // --- SECOND ULTRASONIC READING (THIS IS THE ONE WE WILL USE) ---
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration2 = pulseIn(ECHO_PIN, HIGH, 30000);
  
  if (duration2 > 0) {
    distance2 = (duration2 / 2.0) / 29.1;
  }

  // Use the SECOND reading if valid, otherwise fallback to first
  if (distance2 > 0) {
    distance = distance2;
  } else if (distance1 > 0) {
    distance = distance1;
  } else {
    distance = 0; // Both readings failed
  }

  // Debug output
  SerialMon.print("Ultrasonic R1: ");
  SerialMon.print(distance1);
  SerialMon.print(" cm, R2: ");
  SerialMon.print(distance2);
  SerialMon.print(" cm, Used: ");
  SerialMon.print(distance);
  SerialMon.println(" cm");
  
  delay(500);
  
  // Timestamp
  String timestamp = String(year()) + "-" + String(month()) + "-" + String(day()) + " " +
                     String(hour()) + ":" + String(minute()) + ":" + String(second());
  
  // Build SMS message (short format) - UNCHANGED
  String smsMessage = String(SMS_DEVICE) + " M=" + String(moisture) + 
                      " ST=" + String(temperature) +
                      " EC=" + String(ec) +
                      " PH=" + String(ph) +
                      " N=" + String(nitrogen) +
                      " P=" + String(phosphorus) +
                      " K=" + String(potassium) +
                      " AT=" + String(Airtemp) +
                      " RH=" + String(Humidity) +
                      " WD=" + String(distance) +
                      " WS=" + String(WindSpeed) +
                      " DIR=N" +
                      " TS=" + timestamp;
  
  if (!modbusSuccess) {
    smsMessage += " ALERT:Soil sensor offline";
  }
  
  SerialMon.println("SMS Message: " + smsMessage);
  
  // Send SMS
  bool smsOK = sendSmsToAll(smsMessage);
  
  if (!smsOK) {
    SerialMon.println("SMS sending failed after all retries");
  } else {
    SerialMon.println("All SMS sent successfully!");
  }
}

void setup() {
  SerialMon.begin(115200);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  
  SerialMon.println("DEVICE Starting...");
  delay(100);
  
  pinMode(SOIL_SENSOR_MOSFET, OUTPUT);
  digitalWrite(SOIL_SENSOR_MOSFET, HIGH);
  
  pinMode(All_5V_SENSOR, OUTPUT);
  digitalWrite(All_5V_SENSOR, HIGH);
  
  delay(10000);
  
  Wire.begin(I2C_SDA, I2C_SCL);
  sht20.initSHT20();
  delay(100);
  
  pinMode(RS485_RE, OUTPUT);
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_RE, LOW);
  digitalWrite(RS485_DE, LOW);
  
  Serial2.begin(4800, SERIAL_8N1, UART2_RX, UART2_TX);
  
  // Initialize soil sensor (slave ID 1)
  soilNode.begin(1, Serial2);
  soilNode.preTransmission(preTransmission);
  soilNode.postTransmission(postTransmission);
  
  // Initialize wind sensor (slave ID 3)
  windNode.begin(3, Serial2);
  windNode.preTransmission(preTransmission);
  windNode.postTransmission(postTransmission);
  
  pinMode(MODEM_RST, OUTPUT);
  digitalWrite(MODEM_RST, HIGH);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  // Initialize modem
  if (!initModem()) {
    SerialMon.println("Modem initialization failed, rebooting...");
    delay(1000);
    ESP.restart();
  }
  
  esp_sleep_enable_timer_wakeup(600 * 1000000);  // 10 MINUTES
  //esp_sleep_enable_timer_wakeup(1800 * 1000000);  // 30 MINUTES
  
}

void loop() {
  // Read sensors
  Airtemp = sht20.readTemperature();
  Humidity = sht20.readHumidity();
  
  delay(100);
  
  // Wind speed is now read inside handleModbusData via RS485
  float WindSpeed = 0.0;  // Will be updated in handleModbusData
  
  delay(1000);
  
  // Handle data and send SMS
  handleModbusData(WindSpeed, Airtemp, Humidity);
  
  // Power down modem before sleep
  SerialMon.println("Powering down modem...");
  digitalWrite(MODEM_RST, LOW);
  delay(1000);
  
  // Turn off all sensors
  digitalWrite(All_5V_SENSOR, LOW);
  
  SerialMon.println("Entering deep sleep for 10 minutes...");
  delay(1000);
  
  // Clear Serial buffers
  while(SerialAT.available()) SerialAT.read();
  while(SerialMon.available()) SerialMon.read();
  
  // Force serial flush
  SerialAT.flush();
  SerialMon.flush();
  
  // Enter deep sleep
  esp_deep_sleep_start();
}
