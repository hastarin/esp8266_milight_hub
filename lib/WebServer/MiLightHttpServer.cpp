#include <fs.h>
#include <WiFiUdp.h>
#include <IntParsing.h>
#include <Settings.h>
#include <MiLightHttpServer.h>
#include <MiLightRadioConfig.h>

void MiLightHttpServer::begin() {
  applySettings(settings);
  
  server.on("/", HTTP_GET, handleServeFile(WEB_INDEX_FILENAME, "text/html"));
  server.on("/settings", HTTP_GET, handleServeFile(SETTINGS_FILE, "application/json"));
  server.on("/settings", HTTP_PUT, [this]() { handleUpdateSettings(); });
  server.on("/settings", HTTP_POST, [this]() { server.send(200, "text/plain", "success"); }, handleUpdateFile(SETTINGS_FILE));
  server.onPattern("/gateway_traffic/:type", HTTP_GET, [this](const UrlTokenBindings* b) { handleListenGateway(b); });
  server.onPattern("/gateways/:device_id/:type/:group_id", HTTP_PUT, [this](const UrlTokenBindings* b) { handleUpdateGroup(b); });
  server.onPattern("/gateways/:device_id/:type", HTTP_PUT, [this](const UrlTokenBindings* b) { handleUpdateGateway(b); });
  server.onPattern("/send_raw/:type", HTTP_PUT, [this](const UrlTokenBindings* b) { handleSendRaw(b); });
  server.on("/web", HTTP_POST, [this]() { server.send(200, "text/plain", "success"); }, handleUpdateFile(WEB_INDEX_FILENAME));
  server.on("/firmware", HTTP_POST, 
    [this](){
      server.sendHeader("Connection", "close");
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
      ESP.restart();
    },
    [this](){
      HTTPUpload& upload = server.upload();
      if(upload.status == UPLOAD_FILE_START){
        WiFiUDP::stopAll();
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if(!Update.begin(maxSketchSpace)){//start with max available size
          Update.printError(Serial);
        }
      } else if(upload.status == UPLOAD_FILE_WRITE){
        if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
          Update.printError(Serial);
        }
      } else if(upload.status == UPLOAD_FILE_END){
        if(Update.end(true)){ //true to set the size to the current progress
        } else {
          Update.printError(Serial);
        }
      }
      yield();
    }
  );
  
  server.begin();
}

void MiLightHttpServer::handleClient() {
  server.handleClient();
}

void MiLightHttpServer::applySettings(Settings& settings) {
  if (server.authenticationRequired() && !settings.hasAuthSettings()) {
    server.disableAuthentication();
  } else {
    server.requireAuthentication(settings.adminUsername, settings.adminPassword);
  }
  
  milightClient->setResendCount(settings.packetRepeats);
}

void MiLightHttpServer::onSettingsSaved(SettingsSavedHandler handler) {
  this->settingsSavedHandler = handler;
}
  
ESP8266WebServer::THandlerFunction MiLightHttpServer::handleServeFile(
  const char* filename, 
  const char* contentType, 
  const char* defaultText) {
    
  return [this, filename, contentType, defaultText]() {
    if (!serveFile(filename)) {
      if (defaultText) {
        server.send(200, contentType, defaultText);
      } else {
        server.send(404);
      }
    }
  };
}

bool MiLightHttpServer::serveFile(const char* file, const char* contentType) {
  if (SPIFFS.exists(file)) {
    File f = SPIFFS.open(file, "r");
    server.send(200, contentType, f.readString());
    f.close();
    return true;
  }
  
  return false;
}

ESP8266WebServer::THandlerFunction MiLightHttpServer::handleUpdateFile(const char* filename) {
  return [this, filename]() {
    HTTPUpload& upload = server.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
      updateFile = SPIFFS.open(filename, "w");
    } else if(upload.status == UPLOAD_FILE_WRITE){
      if (updateFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Serial.println("Error updating web file");
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      updateFile.close();
    }
  };
}

void MiLightHttpServer::handleUpdateSettings() {
  DynamicJsonBuffer buffer;
  const String& rawSettings = server.arg("plain");
  JsonObject& parsedSettings = buffer.parse(rawSettings);
  
  if (parsedSettings.success()) {
    settings.patch(parsedSettings);
    settings.save();
    
    this->applySettings(settings);
    this->settingsSavedHandler();
    
    server.send(200, "application/json", "true");
  } else {
    server.send(400, "application/json", "\"Invalid JSON\"");
  }
}

void MiLightHttpServer::handleListenGateway(const UrlTokenBindings* bindings) {
  bool available = false;
  MiLightRadioConfig config = milightClient->getRadioConfig(bindings->get("type"));
  
  while (!available) {
    if (!server.clientConnected()) {
      return;
    }
    
    if (milightClient->available(config.type)) {
      available = true;
    }
    
    yield();
  }
  
  uint8_t packet[config.packetLength];
  milightClient->read(static_cast<MiLightRadioType>(config.type), packet);
  
  String response = "Packet received (";
  response += String(sizeof(packet)) + " bytes)";
  response += ":\n";
  
  char ppBuffer[200];
  milightClient->formatPacket(config, packet, ppBuffer);
  response += String(ppBuffer);
  
  response += "\n\n";
  
  server.send(200, "text/plain", response);
}

void MiLightHttpServer::handleUpdateGroup(const UrlTokenBindings* urlBindings) {
  DynamicJsonBuffer buffer;
  JsonObject& request = buffer.parse(server.arg("plain"));
  
  if (!request.success()) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  const uint16_t deviceId = parseInt<uint16_t>(urlBindings->get("device_id"));
  const uint8_t groupId = urlBindings->get("group_id").toInt();
  const MiLightRadioType type = MiLightClient::getRadioType(urlBindings->get("type"));
  
  if (type == UNKNOWN) {
    String body = "Unknown device type: ";
    body += urlBindings->get("type");
    
    server.send(400, "text/plain", body);
    return;
  }
  
  milightClient->setResendCount(
    settings.httpRepeatFactor * settings.packetRepeats
  );
  
  if (request.containsKey("status")) {
    const String& statusStr = request.get<String>("status");
    MiLightStatus status = (statusStr == "on" || statusStr == "true") ? ON : OFF;
    milightClient->updateStatus(type, deviceId, groupId, status);
  }
      
  if (request.containsKey("command")) {
    if (request["command"] == "unpair") {
      milightClient->unpair(type, deviceId, groupId);
    }
    
    if (request["command"] == "pair") {
      milightClient->pair(type, deviceId, groupId);
    }
  }
  
  if (type == RGBW) {
    if (request.containsKey("hue")) {
      milightClient->updateHue(deviceId, groupId, request["hue"]);
    }
    
    if (request.containsKey("level")) {
      milightClient->updateBrightness(deviceId, groupId, request["level"]);
    }
    
    if (request.containsKey("command")) {
      if (request["command"] == "set_white") {
        milightClient->updateColorWhite(deviceId, groupId);
      }
    }
  } else if (type == CCT) {
    if (request.containsKey("temperature")) {
      milightClient->updateTemperature(deviceId, groupId, request["temperature"]);
    }
    
    if (request.containsKey("level")) {
      milightClient->updateCctBrightness(deviceId, groupId, request["level"]);
    }
    
    if (request.containsKey("command")) {
      // CCT command work more effectively with a lower number of repeats it seems.
      milightClient->setResendCount(MILIGHT_DEFAULT_RESEND_COUNT);
      
      if (request["command"] == "level_up") {
        milightClient->increaseCctBrightness(deviceId, groupId);
      }
      
      if (request["command"] == "level_down") {
        milightClient->decreaseCctBrightness(deviceId, groupId);
      }
      
      if (request["command"] == "temperature_up") {
        milightClient->increaseTemperature(deviceId, groupId);
      }
      
      if (request["command"] == "temperature_down") {
        milightClient->decreaseTemperature(deviceId, groupId);
      }
  
      milightClient->setResendCount(settings.packetRepeats);
    }
  } 
  
  milightClient->setResendCount(settings.packetRepeats);
  
  server.send(200, "application/json", "true");
}

void MiLightHttpServer::handleUpdateGateway(const UrlTokenBindings* urlBindings) {
  DynamicJsonBuffer buffer;
  JsonObject& request = buffer.parse(server.arg("plain"));
  
  const uint16_t deviceId = parseInt<uint16_t>(urlBindings->get("device_id"));
  const MiLightRadioType type = MiLightClient::getRadioType(urlBindings->get("type"));
  
  if (type == UNKNOWN) {
    String body = "Unknown device type: ";
    body += urlBindings->get("type");
    
    server.send(400, "text/plain", body);
    return;
  }
  
  milightClient->setResendCount(MILIGHT_DEFAULT_RESEND_COUNT);
  
  if (request.containsKey("status")) {
    if (request["status"] == "on") {
      milightClient->allOn(type, deviceId);
    } else if (request["status"] == "off") {
      milightClient->allOff(type, deviceId);
    }
  }
  
  server.send(200, "application/json", "true");
}

void MiLightHttpServer::handleSendRaw(const UrlTokenBindings* bindings) {
  DynamicJsonBuffer buffer;
  JsonObject& request = buffer.parse(server.arg("plain"));
  MiLightRadioConfig config = milightClient->getRadioConfig(bindings->get("type"));
  
  uint8_t packet[config.packetLength];
  const String& hexPacket = request["packet"];
  hexStrToBytes<uint8_t>(hexPacket.c_str(), hexPacket.length(), packet, config.packetLength);
  
  size_t numRepeats = MILIGHT_DEFAULT_RESEND_COUNT;
  if (request.containsKey("num_repeats")) {
    numRepeats = request["num_repeats"];
  }
  
  for (size_t i = 0; i < numRepeats; i++) {
    milightClient->getRadio(config.type)->write(packet, config.packetLength);
  }
  
  server.send(200, "text/plain", "true");
}