/*
 * VicTech ESP32-WROOM-32D v5.0
 * MQTT sobre WebSocket SSL manual (HTTP/1.1 forzado)
 *
 * Librerías:
 *   - ArduinoJson by Benoit Blanchon
 *   (NO necesita librería WebSocket externa)
 *
 * Pines:
 *   Arduino #1 (entrada): TX=GPIO17, RX=GPIO16
 *   Arduino #2 (salida):  TX=GPIO25, RX=GPIO26
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

#define SERIAL1_TX 17
#define SERIAL1_RX 16
#define SERIAL2_TX 25
#define SERIAL2_RX 26
#define PIN_RESET_BTN 0
#define RESET_HOLD_MS 5000

#define MQTT_HOST "mqtt.victech.mx"
#define MQTT_PORT 443
#define MQTT_USER "esp32"
#define MQTT_PASS "victech01"
#define API_REGISTER "https://api.victech.mx/api/devices/register"
#define AP_SSID_PREFIX "VicTech-Setup-"

Preferences      prefs;
WebServer        server(80);
WiFiClientSecure sslClient;

HardwareSerial ArduinoA(1);
HardwareSerial ArduinoB(2);

String installationId="", topicCmdA, topicCmdB, topicStatA, topicStatB, topicOnline, topicReset;

bool wsConnected  = false;
bool mqttConnected = false;
unsigned long lastOnline    = 0;
unsigned long lastReconnect = 0;
unsigned long lastPing      = 0;

// ── HTML provisioning ────────────────────────────────────
const char PROVISION_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>VicTech Setup</title>
<style>body{font-family:sans-serif;background:#f0ede6;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0}.card{background:#fff;padding:2rem;border-radius:12px;box-shadow:0 4px 16px rgba(0,0,0,.12);width:320px}h2{color:#534AB7;margin:0 0 1.5rem;text-align:center}label{font-size:.85rem;color:#555;display:block;margin-bottom:.25rem}input{width:100%;padding:.6rem .8rem;border:1px solid #ddd;border-radius:8px;margin-bottom:1rem;box-sizing:border-box;font-size:1rem}button{width:100%;padding:.75rem;background:#534AB7;color:#fff;border:none;border-radius:8px;font-size:1rem;cursor:pointer}#msg{margin-top:1rem;text-align:center;font-size:.9rem;color:#333}</style>
</head><body><div class="card"><h2>VicTech Setup</h2>
<label>Red WiFi</label><input id="s" type="text" placeholder="Nombre de tu red">
<label>Contrasena</label><input id="p" type="password" placeholder="Contrasena WiFi">
<label>Token</label><input id="t" type="text" placeholder="Token de instalacion">
<button onclick="g()">Guardar y conectar</button><div id="m"></div></div>
<script>function g(){const s=document.getElementById('s').value.trim(),p=document.getElementById('p').value,t=document.getElementById('t').value.trim();if(!s||!t){document.getElementById('m').innerText='Completa todos los campos';return}document.getElementById('m').innerText='Guardando...';fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,pass:p,token:t})}).then(r=>r.json()).then(d=>{document.getElementById('m').innerText=d.ok?'Listo! Reiniciando...':'Error: '+d.error}).catch(()=>{document.getElementById('m').innerText='Error'})}</script>
</body></html>
)rawliteral";

String apSSID() {
  uint8_t m[6]; WiFi.macAddress(m);
  char s[5]; sprintf(s,"%02X%02X",m[4],m[5]);
  return String(AP_SSID_PREFIX)+s;
}

void checkResetButton() {
  if(digitalRead(PIN_RESET_BTN)==LOW) {
    unsigned long t=millis();
    while(digitalRead(PIN_RESET_BTN)==LOW) {
      if(millis()-t>RESET_HOLD_MS){prefs.begin("victech",false);prefs.clear();prefs.end();delay(300);ESP.restart();}
      delay(50);
    }
  }
}

void iniciarProvisioningAP() {
  WiFi.mode(WIFI_AP); WiFi.softAP(apSSID().c_str());
  Serial.print("[AP] "); Serial.println(apSSID());
  server.on("/",HTTP_GET,[](){server.send(200,"text/html",PROVISION_HTML);});
  server.on("/save",HTTP_POST,[](){
    StaticJsonDocument<256> doc;
    if(!server.hasArg("plain")||deserializeJson(doc,server.arg("plain"))){server.send(400,"application/json","{\"ok\":false}");return;}
    prefs.begin("victech",false);
    prefs.putString("wifi_ssid",doc["ssid"].as<String>());
    prefs.putString("wifi_pass",doc["pass"].as<String>());
    prefs.putString("reg_token",doc["token"].as<String>());
    prefs.end();
    server.send(200,"application/json","{\"ok\":true}");
    delay(1500); ESP.restart();
  });
  server.begin();
}

bool conectarWiFi(const String& ssid,const String& pass) {
  Serial.print("[WiFi] "); Serial.println(ssid);
  WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(),pass.c_str());
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<15000){delay(300);Serial.print(".");}
  if(WiFi.status()==WL_CONNECTED){Serial.print("\n[WiFi] IP: ");Serial.println(WiFi.localIP());return true;}
  Serial.println("\n[WiFi] Fallo"); return false;
}

bool registrarDispositivo(const String& token) {
  prefs.begin("victech",true); String sid=prefs.getString("installation_id",""); prefs.end();
  if(!sid.isEmpty()){installationId=sid;return true;}
  HTTPClient http; http.begin(API_REGISTER); http.addHeader("Content-Type","application/json");
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ms[18]; sprintf(ms,"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  StaticJsonDocument<256> b; b["esp32_serial"]=ms; b["token"]=token; b["mqtt_client_id"]=String("esp32-")+ms;
  String bs; serializeJson(b,bs);
  int code=http.POST(bs);
  if(code==200||code==201){
    StaticJsonDocument<512> r;
    if(!deserializeJson(r,http.getString())){
      installationId=r["installation_id"].as<String>();
      prefs.begin("victech",false); prefs.putString("installation_id",installationId); prefs.end();
      Serial.print("[API] "); Serial.println(installationId);
      http.end(); return true;
    }
  }
  Serial.print("[API] Error "); Serial.println(code); http.end(); return false;
}

void buildTopics() {
  String b="victech/"+installationId+"/gate/";
  topicCmdA=b+"entrada/cmd"; topicCmdB=b+"salida/cmd";
  topicStatA=b+"entrada/status"; topicStatB=b+"salida/status";
  topicOnline="victech/"+installationId+"/device/online";
  topicReset="victech/"+installationId+"/device/reset";
}

// ── WebSocket frame: enviar datos binarios enmascarados ───
void wsSendBinary(const uint8_t* data, size_t len) {
  if(!wsConnected||!sslClient.connected()) return;
  uint8_t hdr[10]; int hi=0;
  hdr[hi++]=0x82; // FIN + binary
  uint8_t mask[4]={0x12,0x34,0x56,0x78};
  if(len<126) {
    hdr[hi++]=(uint8_t)(len|0x80);
  } else {
    hdr[hi++]=0xFE; hdr[hi++]=(len>>8)&0xFF; hdr[hi++]=len&0xFF;
  }
  hdr[hi++]=mask[0]; hdr[hi++]=mask[1]; hdr[hi++]=mask[2]; hdr[hi++]=mask[3];
  sslClient.write(hdr,hi);
  // Enmascarar y enviar payload
  uint8_t buf[256]; size_t sent=0;
  while(sent<len) {
    size_t chunk=min((size_t)256, len-sent);
    for(size_t i=0;i<chunk;i++) buf[i]=data[sent+i]^mask[(sent+i)%4];
    sslClient.write(buf,chunk);
    sent+=chunk;
  }
}

// ── Leer frame WebSocket del servidor ────────────────────
int wsReadFrame(uint8_t* buf, size_t maxLen) {
  if(!sslClient.available()) return 0;
  int b0=sslClient.read(); if(b0<0) return -1;
  int b1=sslClient.read(); if(b1<0) return -1;
  uint8_t opcode=b0&0x0F;
  if(opcode==0x08){wsConnected=false;mqttConnected=false;Serial.println("[WS] CLOSE");return -1;}
  if(opcode==0x09){sslClient.write(0x8A);sslClient.write(0x00);return 0;} // PONG
  int payLen=b1&0x7F;
  if(payLen==126){int h=sslClient.read(),l=sslClient.read();payLen=(h<<8)|l;}
  if(payLen<=0||payLen>(int)maxLen) return 0;
  int got=0;
  unsigned long t=millis();
  while(got<payLen&&millis()-t<2000){
    if(sslClient.available()) buf[got++]=sslClient.read();
    else delay(1);
  }
  return got;
}

// ── MQTT: enviar paquete ──────────────────────────────────
void mqttSend(const uint8_t* pkt, size_t len) {
  wsSendBinary(pkt, len);
}

void mqttPublish(const String& topic, const String& payload) {
  if(!mqttConnected) return;
  int tLen=topic.length(), pLen=payload.length();
  uint8_t pkt[512]; int i=0;
  pkt[i++]=0x30; pkt[i++]=2+tLen+pLen;
  pkt[i++]=(tLen>>8)&0xFF; pkt[i++]=tLen&0xFF;
  for(int j=0;j<tLen;j++) pkt[i++]=topic[j];
  for(int j=0;j<pLen;j++) pkt[i++]=payload[j];
  mqttSend(pkt,i);
}

void mqttSubscribe(const String& topic) {
  int tLen=topic.length();
  uint8_t pkt[128]; int i=0;
  pkt[i++]=0x82; pkt[i++]=2+2+tLen+1;
  pkt[i++]=0x00; pkt[i++]=0x01;
  pkt[i++]=(tLen>>8)&0xFF; pkt[i++]=tLen&0xFF;
  for(int j=0;j<tLen;j++) pkt[i++]=topic[j];
  pkt[i++]=0x00;
  mqttSend(pkt,i);
  Serial.print("[MQTT sub] "); Serial.println(topic);
}

void mqttPing() {
  uint8_t pkt[]={0xC0,0x00};
  mqttSend(pkt,2);
}

// ── Conectar WebSocket manualmente con HTTP/1.1 ───────────
bool conectarWS() {
  wsConnected=false; mqttConnected=false;
  sslClient.stop();
  Serial.print("[WS] Conectando a "); Serial.println(MQTT_HOST);
  sslClient.setInsecure();
  if(!sslClient.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.println("[WS] SSL connect fallo"); return false;
  }
  // Enviar WebSocket upgrade HTTP/1.1 manualmente
  sslClient.print("GET / HTTP/1.1\r\n");
  sslClient.print("Host: mqtt.victech.mx\r\n");
  sslClient.print("Upgrade: websocket\r\n");
  sslClient.print("Connection: Upgrade\r\n");
  sslClient.print("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
  sslClient.print("Sec-WebSocket-Version: 13\r\n");
  sslClient.print("Sec-WebSocket-Protocol: mqtt\r\n");
  sslClient.print("\r\n");

  // Leer respuesta HTTP
  String response=""; unsigned long t=millis();
  while(millis()-t<5000) {
    while(sslClient.available()) response+=(char)sslClient.read();
    if(response.indexOf("\r\n\r\n")>=0) break;
    delay(10);
  }
  Serial.println("[WS] Respuesta: "+response.substring(0,50));
  if(response.indexOf("101")<0) {
    Serial.println("[WS] No recibio 101"); sslClient.stop(); return false;
  }
  wsConnected=true;
  Serial.println("[WS] Conectado! Enviando MQTT CONNECT...");

  // Enviar MQTT CONNECT
  uint8_t mac[6]; WiFi.macAddress(mac);
  char cid[32]; sprintf(cid,"esp32-%02X%02X%02X",mac[3],mac[4],mac[5]);
  String clientId=cid, user=MQTT_USER, pass=MQTT_PASS;
  int cidLen=clientId.length(), userLen=user.length(), passLen=pass.length();
  int remLen=10+2+cidLen+2+userLen+2+passLen;
  uint8_t pkt[256]; int i=0;
  pkt[i++]=0x10; pkt[i++]=remLen;
  pkt[i++]=0x00; pkt[i++]=0x04; pkt[i++]='M'; pkt[i++]='Q'; pkt[i++]='T'; pkt[i++]='T';
  pkt[i++]=0x04; pkt[i++]=0xC2; pkt[i++]=0x00; pkt[i++]=0x3C;
  pkt[i++]=(cidLen>>8)&0xFF; pkt[i++]=cidLen&0xFF;
  for(int j=0;j<cidLen;j++) pkt[i++]=clientId[j];
  pkt[i++]=(userLen>>8)&0xFF; pkt[i++]=userLen&0xFF;
  for(int j=0;j<userLen;j++) pkt[i++]=user[j];
  pkt[i++]=(passLen>>8)&0xFF; pkt[i++]=passLen&0xFF;
  for(int j=0;j<passLen;j++) pkt[i++]=pass[j];
  wsSendBinary(pkt,i);

  // Esperar CONNACK
  t=millis(); uint8_t buf[16];
  while(millis()-t<5000) {
    if(sslClient.available()>=2) {
      int r=wsReadFrame(buf,16);
      if(r>=4&&buf[0]==0x20) {
        if(buf[3]==0x00) {
          mqttConnected=true;
          Serial.println("[MQTT] CONNACK OK!");
          mqttSubscribe(topicCmdA); delay(50);
          mqttSubscribe(topicCmdB); delay(50);
          mqttSubscribe(topicReset); delay(50);
          mqttPublish(topicOnline,"{\"online\":true}");
          return true;
        } else {
          Serial.print("[MQTT] CONNACK error: "); Serial.println((int)buf[3]);
          return false;
        }
      }
    }
    delay(50);
  }
  Serial.println("[MQTT] Timeout esperando CONNACK");
  return false;
}

// ── Procesar mensaje MQTT recibido ────────────────────────
void procesarMQTT(const uint8_t* data, int len) {
  if(len<2) return;
  uint8_t type=data[0]&0xF0;
  if(type!=0x30) return; // Solo PUBLISH

  int idx=1;
  int remLen=0,mult=1; uint8_t b;
  do{b=data[idx++];remLen+=(b&0x7F)*mult;mult*=128;}while(b&0x80&&idx<len);
  if(idx+2>len) return;
  int tLen=((uint8_t)data[idx]<<8)|(uint8_t)data[idx+1]; idx+=2;
  if(idx+tLen>len) return;
  String topic=String((const char*)data+idx,tLen); idx+=tLen;
  String payload=String((const char*)data+idx,len-idx);
  Serial.print("[MQTT] "); Serial.print(topic); Serial.print(" -> "); Serial.println(payload);

  StaticJsonDocument<256> doc;
  if(deserializeJson(doc,payload)) return;
  String action=doc["action"].as<String>();

  if(action=="reset"){prefs.begin("victech",false);prefs.clear();prefs.end();delay(300);ESP.restart();return;}

  String cmd="";
  if(action=="open") cmd="OPEN\n";
  else if(action=="close") cmd="CLOSE\n";
  else if(action=="status") cmd="STATUS\n";
  else return;

  if(topic==topicCmdA){ArduinoA.print(cmd);Serial.print("[->A] ");Serial.print(cmd);}
  else if(topic==topicCmdB){ArduinoB.print(cmd);Serial.print("[->B] ");Serial.print(cmd);}
}

void leerArduino(HardwareSerial& serial, const String& topicStat, const String& side) {
  if(!serial.available()) return;
  String line=serial.readStringUntil('\n'); line.trim();
  if(!line.startsWith("STATUS:")) return;
  String sv=line.substring(7); sv.toLowerCase();
  StaticJsonDocument<128> doc; doc["status"]=sv; doc["side"]=side; doc["ts"]=millis();
  String out; serializeJson(doc,out);
  mqttPublish(topicStat,out);
  Serial.print("[<-"); Serial.print(side); Serial.print("] "); Serial.println(line);
}

// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[VicTech] v5.0 WS-Manual");
  pinMode(PIN_RESET_BTN,INPUT_PULLUP);
  ArduinoA.begin(9600,SERIAL_8N1,SERIAL1_RX,SERIAL1_TX);
  ArduinoB.begin(9600,SERIAL_8N1,SERIAL2_RX,SERIAL2_TX);

  prefs.begin("victech",true);
  String ssid=prefs.getString("wifi_ssid","");
  String pass=prefs.getString("wifi_pass","");
  String token=prefs.getString("reg_token","");
  prefs.end();

  if(ssid.isEmpty()){iniciarProvisioningAP();return;}
  if(!conectarWiFi(ssid,pass)){iniciarProvisioningAP();return;}
  if(!registrarDispositivo(token)) Serial.println("[ERROR] Registro fallido");
  buildTopics();
  conectarWS();
}

// ══════════════════════════════════════════════════════════
void loop() {
  checkResetButton();
  if(WiFi.getMode()==WIFI_AP){server.handleClient();return;}

  if(WiFi.status()!=WL_CONNECTED){
    prefs.begin("victech",true);
    String ssid=prefs.getString("wifi_ssid","");
    String pass=prefs.getString("wifi_pass","");
    prefs.end();
    conectarWiFi(ssid,pass);
  }

  // Leer frames WebSocket entrantes
  if(wsConnected && sslClient.connected()) {
    if(sslClient.available()) {
      uint8_t buf[512];
      int r=wsReadFrame(buf,512);
      if(r>0) procesarMQTT(buf,r);
      else if(r<0){wsConnected=false;mqttConnected=false;}
    }
    // Ping MQTT cada 45s
    if(mqttConnected && millis()-lastPing>45000) {
      lastPing=millis(); mqttPing();
    }
  } else if(millis()-lastReconnect>8000) {
    lastReconnect=millis();
    Serial.println("[WS] Reconectando...");
    conectarWS();
  }

  leerArduino(ArduinoA,topicStatA,"entrada");
  leerArduino(ArduinoB,topicStatB,"salida");

  // Heartbeat cada 30s
  if(mqttConnected&&!installationId.isEmpty()&&millis()-lastOnline>30000){
    lastOnline=millis();
    StaticJsonDocument<64> doc; doc["online"]=true; doc["ts"]=millis();
    String out; serializeJson(doc,out);
    mqttPublish(topicOnline,out);
  }
}
