#include <SPI.h>
#include <LoRa.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// LoRa pins for ESP32
#define LORA_SCK 5
#define LORA_MISO 18
#define LORA_MOSI 23
#define LORA_SS 4
#define LORA_RST 14
#define LORA_DIO0 26

// Additional hardware pins
#define STATUS_LED 13
#define POWER_BUTTON 12

// LoRa frequency (in Hz)
#define LORA_FREQUENCY 433E6  // 915 MHz (US/AU), use 868E6 for Europe, 433E6 for Asia

// BLE UUIDs
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define WRITE_CHAR_UUID        "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define READ_CHAR_UUID         "beb5483e-36e1-4688-b7f5-ea07361b26a9"

// Buffer for receiving data
#define MAX_PACKET_SIZE 255
uint8_t loraData[MAX_PACKET_SIZE];
String bleData = "";

// BLE handles
BLEServer* pServer = NULL;
BLECharacteristic* pWriteCharacteristic = NULL;
BLECharacteristic* pReadCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

volatile bool loraDataReceived = false;

void sendLoRaData(String data);

// BLE Server callback
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    digitalWrite(STATUS_LED, HIGH);
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    digitalWrite(STATUS_LED, LOW);
  }
};

// BLE Write callback
class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();  // Avoid std::string
    if (rxValue.length() > 0) {
      Serial.println("Received over BLE: " + rxValue);
      bleData = rxValue;
      sendLoRaData(bleData);
    }
  }
};

// LoRa receive handler
void onLoRaReceive(int packetSize) {
  if (packetSize == 0) return;

  Serial.println("Received packet from LoRa:");
  int i = 0;
  while (LoRa.available() && i < MAX_PACKET_SIZE) {
    loraData[i] = LoRa.read();
    Serial.print((char)loraData[i]);
    i++;
  }
  Serial.println();
  loraData[i] = '\0'; // Null-terminate
  loraDataReceived = true;
}

// LoRa transmission
void sendLoRaData(String data) {
  Serial.println("Sending over LoRa: " + data);
  digitalWrite(STATUS_LED, HIGH);
  LoRa.beginPacket();
  LoRa.print(data);
  LoRa.endPacket();
  digitalWrite(STATUS_LED, LOW);
  
  // Return to receive mode after sending
  LoRa.receive();
}

// Generate dynamic BLE name
String getDeviceName() {
  uint64_t chipid = ESP.getEfuseMac(); // Use ESP32 MAC
  char id[13];
  sprintf(id, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  return "EchoGhat-" + String(id);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 BLE-LoRa Bridge");

  pinMode(STATUS_LED, OUTPUT);
  pinMode(POWER_BUTTON, INPUT_PULLUP);
  digitalWrite(STATUS_LED, LOW);

  // LoRa setup
  Serial.println("Initializing LoRa...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LoRa init failed!");
    while (1);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.onReceive(onLoRaReceive);
  LoRa.receive();
  Serial.println("LoRa ready!");

  // BLE setup
  String deviceName = getDeviceName();
  Serial.println("BLE Name: " + deviceName);
  BLEDevice::init(deviceName.c_str());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // BLE write characteristic
  pWriteCharacteristic = pService->createCharacteristic(
    WRITE_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pWriteCharacteristic->setCallbacks(new MyCallbacks());

  // BLE read/notify characteristic
  pReadCharacteristic = pService->createCharacteristic(
    READ_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_INDICATE
  );
  pReadCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising...");
}

void loop() {
  // Long-press to restart
  static unsigned long pressStart = 0;
  static bool isPressed = false;

  if (digitalRead(POWER_BUTTON) == LOW) {
    if (!isPressed) {
      isPressed = true;
      pressStart = millis();
    } else if (millis() - pressStart > 3000) {
      Serial.println("Power button long press detected. Restarting...");
      digitalWrite(STATUS_LED, LOW);
      delay(1000);
      ESP.restart();
    }
  } else {
    isPressed = false;
  }

  // Send LoRa -> BLE
  if (loraDataReceived && deviceConnected) {
    String loraStr = String((char*)loraData);
    pReadCharacteristic->setValue(loraStr.c_str());
    pReadCharacteristic->notify();
    Serial.println("Sent to BLE: " + loraStr);
    loraDataReceived = false;

    digitalWrite(STATUS_LED, HIGH);
    delay(50);
    digitalWrite(STATUS_LED, LOW);
  }

  // Reconnect handling
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("BLE disconnected. Restarting advertising...");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    Serial.println("BLE client connected!");
    oldDeviceConnected = deviceConnected;
  }

  delay(10);
}
