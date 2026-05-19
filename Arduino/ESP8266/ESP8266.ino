#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <SoftwareSerial.h>

const char* ssid = "ElBromas_2.4G";
const char* password = "90_kakashi_26";

const char* serverConfig = "https://cyrrotinqlcnfemozcza.supabase.co/rest/v1/config_actual?id=eq.1&select=*";
const char* serverData = "https://cyrrotinqlcnfemozcza.supabase.co/rest/v1/live_data?id=eq.1";
const char* serverHistorico = "https://cyrrotinqlcnfemozcza.supabase.co/rest/v1/datos";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImN5cnJvdGlucWxjbmZlbW96Y3phIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzczMjEwMDIsImV4cCI6MjA5Mjg5NzAwMn0._wl5C8exO7A1mZxFOiXyx61RBjE2HbhjoSenegDjBww";

// =========================
// BUFFER SERIAL
// =========================
String buffer = "";
SoftwareSerial arduinoSerial(D5, D6); // RX, TX

// =========================
// CONTROL DE TIEMPO
// =========================
unsigned long lastRequest = 0;

int intervalo = 2000;  // reposo


// =========================
// VARIABLES PARA CAMBIOS
// =========================
int estado_prev = -1;

int estado_actual = 0;
int experimentoActivo = 0;


bool esperandoACK = false;
unsigned long tiempoCFG = 0;
int reintentosCFG = 0;

bool arduinoReconectado = false;

// =========================
// SETUP
// =========================
void setup() {
  Serial.begin(9600);
  arduinoSerial.begin(9600);

  WiFi.mode(WIFI_STA);

  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();

  while (
    WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {

    ESP.restart();
  }
}

// =========================
// ENVIAR DATOS A API
// =========================
void enviarDatos(String linea) {

  linea.trim();
  if (linea.length() == 0) return;

  String v[14];
  int i = 0, start = 0;

  for (int j = 0; j < linea.length(); j++) {
    if (linea[j] == ',') {
      if (i < 14) v[i++] = linea.substring(start, j);
      start = j + 1;
    }
  }
  v[i] = linea.substring(start);

  // validar estructura
  if (i != 13) return;
  // =========================
  // VALIDAR RANGOS
  // =========================
  float tExt = v[0].toFloat() / 100.0;
  float hExt = v[1].toFloat() / 100.0;

  float tInt = v[2].toFloat() / 100.0;
  float hInt = v[3].toFloat() / 100.0;

  float c1 = v[4].toFloat() / 100.0;
  float c2 = v[5].toFloat() / 100.0;
  float c12 = v[6].toFloat() / 100.0;

  float rocioRX = v[7].toFloat() / 100.0;
  float errorRX = v[8].toFloat() / 100.0;

  int pwmRX = v[9].toInt();

  int velARX = v[10].toInt();
  int velBRX = v[11].toInt();

  float corrienteRX = v[12].toFloat() / 100.0;

  int estadoRX = v[13].toInt();

  if (

    // =========================
    // TEMPERATURAS
    // =========================
    tExt < -20 || tExt > 80 || tInt < -20 || tInt > 80 ||

    c1 < -20 || c1 > 80 || c2 < -20 || c2 > 80 || c12 < -20 || c12 > 80 ||

    // =========================
    // HUMEDAD
    // =========================
    hExt < 0 || hExt > 100 || hInt < 0 || hInt > 100 ||

    // =========================
    // ROCÍO
    // =========================
    rocioRX < -30 || rocioRX > 80 ||

    // =========================
    // ERROR
    // =========================
    errorRX < -100 || errorRX > 100 ||

    // =========================
    // PWM
    // =========================
    pwmRX < 0 || pwmRX > 255 ||

    // =========================
    // VENTILADORES
    // =========================
    velARX < 0 || velARX > 255 || velBRX < 0 || velBRX > 255 ||

    // =========================
    // CORRIENTE
    // =========================
    corrienteRX < 0 || corrienteRX > 30 ||

    // =========================
    // ESTADO
    // =========================
    estadoRX < 0 || estadoRX > 4

  ) {
    return;
  }

  StaticJsonDocument<512> doc;

  doc["text"] = v[0].toFloat() / 100.0;
  doc["hext"] = v[1].toFloat() / 100.0;
  doc["tint"] = v[2].toFloat() / 100.0;
  doc["hint"] = v[3].toFloat() / 100.0;

  doc["c1"] = v[4].toFloat() / 100.0;
  doc["c2"] = v[5].toFloat() / 100.0;
  doc["c12"] = v[6].toFloat() / 100.0;

  doc["puntorocio"] = v[7].toFloat() / 100.0;

  doc["error"] = v[8].toFloat() / 100.0;

  doc["pwm"] = v[9].toInt();

  doc["vela"] = v[10].toInt();
  doc["velb"] = v[11].toInt();

  doc["corriente"] = v[12].toFloat() / 100.0;
  doc["estado"] = v[13].toInt();


  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;


  http.begin(client, serverData);

  http.addHeader("apikey", supabaseKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseKey);
  http.addHeader("Prefer", "return=minimal");

  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");


  // =========================
  // HISTÓRICO
  // =========================
  if (
    estado_actual == 1 && experimentoActivo > 0) {

    StaticJsonDocument<512> hist;

    hist["experiment_id"] = experimentoActivo;

    hist["text"] = tExt;
    hist["hext"] = hExt;

    hist["tint"] = tInt;
    hist["hint"] = hInt;

    hist["c1"] = c1;
    hist["c2"] = c2;
    hist["c12"] = c12;

    hist["puntorocio"] = rocioRX;

    hist["error"] = errorRX;

    hist["pwm"] = pwmRX;

    hist["vela"] = velARX;
    hist["velb"] = velBRX;

    hist["corriente"] = corrienteRX;

    hist["estado"] = estadoRX;

    String jsonHist;
    serializeJson(hist, jsonHist);

    WiFiClientSecure clientHist;
    clientHist.setInsecure();

    HTTPClient httpHist;

    httpHist.begin(clientHist, serverHistorico);

    httpHist.addHeader("apikey", supabaseKey);
    httpHist.addHeader("Authorization", String("Bearer ") + supabaseKey);

    httpHist.addHeader("Content-Type", "application/json");

    httpHist.addHeader("Prefer", "return=minimal");

    httpHist.POST(jsonHist);

    httpHist.end();
  }


  String json;
  serializeJson(doc, json);

  http.PATCH(json);

  http.end();
}

// =========================
// LOOP
// =========================
void loop() {

  // =========================
  // 1. RECIBIR DATOS DEL ARDUINO
  // =========================
  while (arduinoSerial.available()) {
    char c = arduinoSerial.read();

    if (c == '\n') {

      buffer.trim();

      // =========================
      // ACK CFG
      // =========================
      if (
        buffer == "ACK" && buffer.length() == 3) {

        buffer = "";
        esperandoACK = false;
        reintentosCFG = 0;

        estado_prev = estado_actual;
      } else {

        enviarDatos(buffer);
        arduinoReconectado = true;
      }

      buffer = "";
    } else {
      buffer += c;

      if (buffer.length() > 120) {
        buffer = "";
      }
    }
  }

  // =========================
  // RESINCRONIZAR CFG
  // =========================
  if (
    arduinoReconectado && !esperandoACK) {

    estado_prev = -1;

    arduinoReconectado = false;
  }

  // =========================
  // RECONEXIÓN WIFI
  // =========================
  static unsigned long ultimoIntentoWiFi = 0;

  if (WiFi.status() != WL_CONNECTED) {

    if (millis() - ultimoIntentoWiFi > 10000) {

      ultimoIntentoWiFi = millis();

      WiFi.disconnect(true);

      delay(1000);

      WiFi.mode(WIFI_STA);

      WiFi.begin(ssid, password);
    }

    return;
  }

  // =========================
  // 2. CONSULTAR CONFIG
  // =========================
  if (WiFi.status() == WL_CONNECTED) {

    if (millis() - lastRequest > intervalo) {

      lastRequest = millis();

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;

      http.begin(client, serverConfig);
      http.addHeader("apikey", supabaseKey);
      http.addHeader("Authorization", String("Bearer ") + supabaseKey);

      int httpCode = http.GET();

      if (httpCode == 200) {

        String payload = http.getString();

        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
          http.end();
          return;
        }

        JsonArray arr = doc.as<JsonArray>();

        if (arr.size() == 0) {
          http.end();
          return;
        }

        JsonObject obj = arr[0];
        estado_actual = obj["estado_control"];

        experimentoActivo = obj["experimento_id"];

        int estado = estado_actual;

        int modo = obj["modomanual"];
        int pwm = obj["pwm"];

        float kp = obj["kp"];
        float ki = obj["ki"];

        int velA = obj["vela"];
        int velB = obj["velb"];

        float humObj = obj["humedad_objetivo"];
        float tserial = obj["tserial"];

        if (
          pwm < 0 || pwm > 255 || velA < 0 || velA > 255 || velB < 0 || velB > 255 || kp < 0 || kp > 100 || ki < 0 || ki > 100 || estado < 0 || estado > 1) {
          http.end();
          return;
        }

        // =========================
        // CAMBIOS DE ESTADO
        // =========================

        // =========================
        // ENVIAR CONFIG
        // =========================
        if (
          estado != estado_prev && !esperandoACK) {

          //if (true) {

          String cfg = "CFG,";

          cfg += String(modo) + ",";
          cfg += String(pwm) + ",";
          cfg += String((int)(kp * 100)) + ",";
          cfg += String((int)(ki * 100)) + ",";
          cfg += String(velA) + ",";
          cfg += String(velB) + ",";
          cfg += String(humObj, 2) + ",";
          cfg += String(tserial, 0) + ",";
          cfg += String(estado);

          if (!esperandoACK) {
            reintentosCFG = 0;
          }

          arduinoSerial.println(cfg);

          esperandoACK = true;
          tiempoCFG = millis();
          reintentosCFG++;
        }

        // =========================
        // AJUSTAR FRECUENCIA
        // =========================

        intervalo = 2000;
      }

      http.end();
      // =========================
      // REINTENTO CFG
      // =========================
      if (
        esperandoACK && millis() - tiempoCFG > 3000) {

        esperandoACK = false;
        tiempoCFG = millis();
      }
      if (reintentosCFG >= 3) {

        esperandoACK = false;
        reintentosCFG = 0;
      }
    }
  }
}