#include <WiFi.h>
#include "esp_wifi.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <Keypad.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <time.h>
#include "SPIFFS.h"

#define SDA_LCD 21
#define SCL_LCD 22
#define SDA_RTC 18
#define SCL_RTC 19
#define LED_PIN 16

#define FIRMWARE_VERSION "2.0.0"
#define SYNC_INTERVAL_MS 300000  // 5 minutes
#define WIFI_CONNECT_TIMEOUT 15000
#define CONFIG_FILE "/wifi_config.json"

// --- OBJETOS ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;
TwoWire I2C_RTC = TwoWire(1);
WebServer server(80);

// --- TECLADO ---
const byte FILAS = 4;
const byte COLUMNAS = 4;

char teclas[FILAS][COLUMNAS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte pinesFilas[FILAS] = { 13, 12, 14, 27 };
byte pinesColumnas[COLUMNAS] = { 26, 25, 33, 32 };
Keypad keypad = Keypad(makeKeymap(teclas), pinesFilas, pinesColumnas, FILAS, COLUMNAS);

// --- WIFI CONFIG (loaded from SPIFFS) ---
String cfgSsid = "";
String cfgPassword = "";
String cfgApiKey = "";
String cfgServerUrl = "";
bool modoConfiguracion = false;
bool wifiConectado = false;
bool internetDisponible = false;
unsigned long lastInternetCheck = 0;
#define INTERNET_CHECK_INTERVAL 60000  // 60 seconds
unsigned long lastRtcInternetSync = 0;

// --- VARIABLES ---
String codigo = "";
String peso = "";
bool ingresandoPeso = false;
bool menuDVisible = false;
bool viendoRegistros = false;
bool modoPairing = false;
bool modoFecha = false;
int fechaCampo = 0;
int fechaDia = 1, fechaMes = 1, fechaAnio = 2026, fechaHora = 0, fechaMin = 0, fechaSeg = 0;
String pinInput = "";

// Visor de registros del día
static const int MAX_REGISTROS_DIA = 50;
String regCodigos[MAX_REGISTROS_DIA];
String regPesos[MAX_REGISTROS_DIA];
String regHoras[MAX_REGISTROS_DIA];
int regIds[MAX_REGISTROS_DIA];
int regTotal = 0;
int regIndice = 0;

bool editandoCodigo = false;
String editCodigo = "";
bool editandoPeso = false;
String editPeso = "";

unsigned long lastUpdate = 0;
unsigned long lastSync = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long idRegistro = 1;
int pendingCount = 0;
String lastSyncTime = "--:--";

String archivo = "/registros.json";

// Forward declarations
int contarPendientes();
void limpiarRegistrosAntiguos();
bool sincronizarRTCInternet();
void cargarFechaDesdeRTC();
void mostrarModoFecha();
void cambiarCampoFecha(int delta);
void iniciarModoFecha();

// --------------------------------------------------
// CONFIG: Load / Save / Check
// --------------------------------------------------

bool cargarConfig() {
  if (!SPIFFS.exists(CONFIG_FILE)) return false;
  File file = SPIFFS.open(CONFIG_FILE, FILE_READ);
  if (!file) return false;

  String contenido = file.readString();
  file.close();

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, contenido)) return false;

  cfgSsid = String(doc["ssid"] | "");
  cfgPassword = String(doc["password"] | "");
  cfgApiKey = String(doc["api_key"] | "");
  cfgServerUrl = String(doc["server_url"] | "");

  return cfgSsid.length() > 0 && cfgServerUrl.length() > 0;
}

void guardarConfig(String ssid, String pass, String apiKey, String serverUrl) {
  StaticJsonDocument<512> doc;
  doc["ssid"] = ssid;
  doc["password"] = pass;
  doc["api_key"] = apiKey;
  doc["server_url"] = serverUrl;

  File file = SPIFFS.open(CONFIG_FILE, FILE_WRITE);
  serializeJson(doc, file);
  file.close();
}

void borrarConfig() {
  SPIFFS.remove(CONFIG_FILE);
}

// --------------------------------------------------
// WIFI: Connect STA
// --------------------------------------------------

bool conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfgSsid.c_str(), cfgPassword.c_str());

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Conectando WiFi...");
  lcd.setCursor(0, 1);
  lcd.print(cfgSsid.substring(0, 20));

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    lcd.setCursor(0, 2);
    lcd.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Conectado!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(1500);
    lcd.clear();
    return true;
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi FALLO");
  lcd.setCursor(0, 1);
  lcd.print("Continua offline");
  delay(2000);
  return false;
}

// --------------------------------------------------
// INTERNET: Verify connectivity
// --------------------------------------------------

bool verificarInternet() {
  if (!wifiConectado || WiFi.status() != WL_CONNECTED) {
    internetDisponible = false;
    return false;
  }

  HTTPClient http;
  if (cfgServerUrl.length() > 0) {
    http.begin(cfgServerUrl + "/health");
    http.addHeader("X-Device-Key", cfgApiKey);
    http.setTimeout(3000);
    int code = http.GET();
    http.end();
    if (code >= 200 && code < 500) {
      internetDisponible = true;
      return true;
    }
  }

  http.begin("http://clients3.google.com/generate_204");
  http.setTimeout(3000);
  int code = http.GET();
  http.end();

  internetDisponible = (code == 204 || code == 200);
  return internetDisponible;
}

// --------------------------------------------------
// CAPTIVE PORTAL: AP mode config page
// --------------------------------------------------

String generarConfigHTML() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  int n = WiFi.scanNetworks();
  WiFi.mode(WIFI_AP);

  String options = "";
  String seen[20];
  int seenCount = 0;
  for (int i = 0; i < n && seenCount < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < seenCount; j++) {
      if (seen[j] == ssid) { dup = true;
        break; }
    }
    if (dup) continue;
    seen[seenCount++] = ssid;
    int rssi = WiFi.RSSI(i);
    String signal = rssi > -50 ? "&#9679;&#9679;&#9679;" : rssi > -70 ? "&#9679;&#9679;&#9675;" : "&#9679;&#9675;&#9675;";
    options += "<option value=\"" + ssid + "\">" + ssid + " " + signal + "</option>";
  }
  WiFi.scanDelete();
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>Balanza - Config</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;max-width:400px;margin:20px auto;padding:0 15px;background:#f5f5f5}";
  html += "h2{color:#333;text-align:center}";
  html += "label{display:block;margin-top:12px;font-weight:bold;color:#555}";
  html += "input,select{width:100%;padding:10px;margin-top:4px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:16px}";
  html += "button{width:100%;padding:12px;margin-top:20px;background:#2563eb;color:#fff;border:none;border-radius:6px;font-size:16px;cursor:pointer}";
  html += "button:hover{background:#1d4ed8}";
  html += ".info{background:#dbeafe;padding:10px;border-radius:6px;margin-top:15px;font-size:13px;color:#1e40af}";
  html += ".scan{text-align:right;margin-top:4px}";
  html += ".scan a{font-size:13px;color:#2563eb}";
  html += "</style></head><body>";
  html += "<h2>Configurar Balanza</h2>";
  html += "<form method=\"POST\" action=\"/save\">";
  html += "<label>Red WiFi</label>";
  if (n > 0) {
    html += "<select name=\"ssid\" required>" + options + "</select>";
    html += "<div class=\"scan\"><a href=\"/\">Escanear de nuevo</a></div>";
  } else {
    html += "<input name=\"ssid\" required placeholder=\"Nombre de la red WiFi\">";
    html += "<div class=\"scan\"><a href=\"/\">Escanear redes</a></div>";
  }
  html += "<label>Contrasena WiFi</label>";
  html += "<input name=\"password\" type=\"password\" placeholder=\"Contrasena\">";
  String preApiKey = cfgApiKey.length() > 0 ?
    cfgApiKey : "";
  String preServerUrl = cfgServerUrl.length() > 0 ? cfgServerUrl : "https://lechefacil-api.gcobena.dev/api/v1";
  if (preApiKey.length() > 0) {
    html += "<input type=\"hidden\" name=\"api_key\" value=\"" + preApiKey + "\">";
    html += "<input type=\"hidden\" name=\"server_url\" value=\"" + preServerUrl + "\">";
  } else {
    html += "<label>API Key del dispositivo</label>";
    html += "<input name=\"api_key\" placeholder=\"Clave de LecheFacil\">";
    html += "<div class=\"info\">Opcional: puedes emparejar con PIN desde el teclado</div>";
    html += "<label>URL del servidor</label>";
    html += "<input name=\"server_url\" required value=\"" + preServerUrl + "\">";
  }
  html += "<button type=\"submit\">Guardar y Conectar</button>";
  html += "</form>";
  html += "<div class=\"info\">Obtiene la API Key desde LecheFacil &gt; Configuracion &gt; pestana Dispositivos.</div>";
  html += "</body></html>";
  return html;
}

void iniciarModoConfig() {
  modoConfiguracion = true;
  wifiConectado = false;

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gw(192, 168, 4, 1);
  IPAddress sn(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gw, sn);
  WiFi.softAP("Balanza-Setup", "12345678");
  esp_wifi_set_max_tx_power(78);
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", generarConfigHTML());
  });
  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("password");
    String apiKey = server.arg("api_key");
    String serverUrl = server.arg("server_url");

    if (ssid.length() == 0 || serverUrl.length() == 0) {
      server.send(400, "text/html", "<h2>Datos incompletos</h2><a href='/'>Volver</a>");
      return;
    }

    if (serverUrl.endsWith("/")) serverUrl.remove(serverUrl.length() - 1);

    guardarConfig(ssid, pass, apiKey, serverUrl);
    server.send(200, "text/html", "<h2>Guardado! Reiniciando...</h2>");
    delay(1500);
    
    ESP.restart();
  });

  server.begin();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Config WiFi");
  lcd.setCursor(0, 1);
  lcd.print("WiFi: Balanza-Setup");
  lcd.setCursor(0, 2);
  lcd.print("IP: 192.168.4.1");
  lcd.setCursor(0, 3);
  lcd.print("C=Salir sin guardar");
}

void salirModoConfig() {
  modoConfiguracion = false;
  WiFi.softAPdisconnect(true);
  if (cargarConfig()) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(cfgSsid.c_str(), cfgPassword.c_str());
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("Reconectando...");
    delay(500);
  }

  lcd.clear();
}

// --------------------------------------------------
// RECORDS: File operations
// --------------------------------------------------

void mostrarOK() {
  lcd.clear();
  lcd.setCursor(4, 1);
  lcd.print("OK GUARDADO");
  digitalWrite(LED_PIN, HIGH);
  delay(1300);
  digitalWrite(LED_PIN, LOW);
  lcd.clear();
}

void guardarEnArchivo(String nuevoRegistro) {
  File file = SPIFFS.open(archivo, FILE_READ);
  String contenido = "[]";

  if (file) {
    contenido = file.readString();
    file.close();
  }

  if (contenido == "[]" || contenido.length() <= 2) {
    contenido = "[" + nuevoRegistro + "]";
  } else {
    contenido.remove(contenido.length() - 1);
    contenido += "," + nuevoRegistro + "]";
  }

  file = SPIFFS.open(archivo, FILE_WRITE);
  file.print(contenido);
  file.close();
}

void guardarRegistro(String cod, String pes) {
  DateTime now = rtc.now();
  char fecha[11];
  sprintf(fecha, "%02d/%02d/%04d", now.day(), now.month(), now.year());

  char hora[9];
  sprintf(hora, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  String turno = (now.hour() >= 12) ? "PM" : "AM";

  StaticJsonDocument<256> doc;
  doc["id"] = idRegistro++;
  doc["codigo"] = cod;
  doc["peso"] = pes;
  doc["fecha"] = fecha;
  doc["hora"] = hora;
  doc["turno"] = turno;

  String salida;
  serializeJson(doc, salida);
  guardarEnArchivo(salida);
  contarPendientes();
}

int contarPendientes() {
  File file = SPIFFS.open(archivo, FILE_READ);
  if (!file) { pendingCount = 0; return 0;
  }

  String contenido = file.readString();
  file.close();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, contenido)) { pendingCount = 0; return 0;
  }

  int count = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (!(obj["s"] | false)) count++;
  }
  pendingCount = count;
  return pendingCount;
}

// --------------------------------------------------
// CLOUD SYNC
// --------------------------------------------------

bool sincronizarNube() {
  if (cfgApiKey.length() == 0) return false;
  if (!wifiConectado || WiFi.status() != WL_CONNECTED) return false;

  File file = SPIFFS.open(archivo, FILE_READ);
  if (!file) return false;
  String contenido = file.readString();
  file.close();

  if (contenido == "[]" || contenido.length() <= 2) return true;

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, contenido)) return false;
  DynamicJsonDocument pendingDoc(8192);
  JsonArray pendingArr = pendingDoc.to<JsonArray>();
  JsonArray arr = doc.as<JsonArray>();
  bool hayPendientes = false;
  for (JsonObject obj : arr) {
    if (!(obj["s"] | false)) {
      pendingArr.add(obj);
      hayPendientes = true;
    }
  }

  if (!hayPendientes) return true;

  String pendingStr;
  serializeJson(pendingDoc, pendingStr);
  String body = "{\"records\":" + pendingStr + ",\"firmware_version\":\"" + FIRMWARE_VERSION + "\"}";

  HTTPClient http;
  String url = cfgServerUrl + "/scale/sync";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", cfgApiKey);
  http.setTimeout(15000);

  int code = http.POST(body);
  String response = http.getString();
  http.end();
  if (code == 200) {
    for (JsonObject obj : arr) {
      if (!(obj["s"] | false)) {
        obj["s"] = true;
      }
    }
    File out = SPIFFS.open(archivo, FILE_WRITE);
    serializeJson(doc, out);
    out.close();

    contarPendientes();
    DateTime now = rtc.now();
    char buf[6];
    sprintf(buf, "%02d:%02d", now.hour(), now.minute());
    lastSyncTime = String(buf);

    return true;
  }

  Serial.printf("[SYNC] Error HTTP %d: %s\n", code, response.c_str());
  return false;
}

// --------------------------------------------------
// CLEANUP: Remove old synced records to free SPIFFS
// --------------------------------------------------

void limpiarRegistrosAntiguos() {
  File file = SPIFFS.open(archivo, FILE_READ);
  if (!file) return;

  String contenido = file.readString();
  file.close();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, contenido)) return;

  DateTime now = rtc.now();
  char hoy[11];
  sprintf(hoy, "%02d/%02d/%04d", now.day(), now.month(), now.year());

  JsonArray arr = doc.as<JsonArray>();
  bool cambio = false;
  for (int i = (int)arr.size() - 1; i >= 0; i--) {
    const char* fecha = arr[i]["fecha"] | "";
    bool synced = arr[i]["s"] | false;
    if (synced && strcmp(fecha, hoy) != 0) {
      arr.remove(i);
      cambio = true;
    }
  }

  if (cambio) {
    File out = SPIFFS.open(archivo, FILE_WRITE);
    serializeJson(doc, out);
    out.close();
  }
}

// --------------------------------------------------
// VISOR REGISTROS
// --------------------------------------------------

void cargarRegistrosHoy() {
  regTotal = 0;

  DateTime now = rtc.now();
  char hoy[11];
  sprintf(hoy, "%02d/%02d/%04d", now.day(), now.month(), now.year());

  File file = SPIFFS.open(archivo, FILE_READ);
  if (!file) return;

  String contenido = file.readString();
  file.close();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, contenido)) return;

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (regTotal >= MAX_REGISTROS_DIA) break;
    const char* fecha = obj["fecha"] | "";
    if (strcmp(fecha, hoy) == 0) {
      regCodigos[regTotal] = String(obj["codigo"] | "");
      regPesos[regTotal] = String(obj["peso"] | "");
      regHoras[regTotal] = String(obj["hora"] | "");
      regIds[regTotal] = obj["id"] | 0;
      regTotal++;
    }
  }
}

void mostrarRegistro() {
  lcd.clear();
  if (regTotal == 0) {
    lcd.setCursor(0, 1);
    lcd.print("SIN REGISTROS HOY");
    lcd.setCursor(0, 3);
    lcd.print("C=Salir");
    return;
  }
  lcd.setCursor(0, 0);
  char encabezado[21];
  sprintf(encabezado, "Registro %d/%d", regIndice + 1, regTotal);
  lcd.print(encabezado);

  lcd.setCursor(0, 1);
  lcd.print("Cod: " + regCodigos[regIndice]);

  lcd.setCursor(0, 2);
  lcd.print("Peso: " + regPesos[regIndice] + " kg");

  lcd.setCursor(0, 3);
  lcd.print(regHoras[regIndice]);
  lcd.print(" A>B<D=XC=#=Ed");
}

// --------------------------------------------------
// ELIMINAR / EDITAR REGISTRO INDIVIDUAL
// --------------------------------------------------

void eliminarRegistroPorId(int id) {
  File file = SPIFFS.open(archivo, FILE_READ);
  if (!file) return;
  String contenido = file.readString();
  file.close();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, contenido)) return;

  JsonArray arr = doc.as<JsonArray>();
  for (int i = 0; i < (int)arr.size(); i++) {
    if ((arr[i]["id"] | 0) == id) {
      arr.remove(i);
      break;
    }
  }

  File out = SPIFFS.open(archivo, FILE_WRITE);
  serializeJson(doc, out);
  out.close();
  contarPendientes();
}

void editarRegistroPorId(int id, String nuevoCodigo, String nuevoPeso) {
  File file = SPIFFS.open(archivo, FILE_READ);
  if (!file) return;

  String contenido = file.readString();
  file.close();
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, contenido)) return;

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if ((obj["id"] | 0) == id) {
      obj["codigo"] = nuevoCodigo;
      obj["peso"] = nuevoPeso;
      break;
    }
  }

  File out = SPIFFS.open(archivo, FILE_WRITE);
  serializeJson(doc, out);
  out.close();
}

// --------------------------------------------------
// BORRAR REGISTROS
// --------------------------------------------------

void borrarRegistros() {
  SPIFFS.remove(archivo);

  File file = SPIFFS.open(archivo, FILE_WRITE);
  file.print("[]");
  file.close();

  pendingCount = 0;

  lcd.clear();
  lcd.setCursor(0, 1);
  lcd.print("REGISTROS BORRADOS");
  digitalWrite(LED_PIN, HIGH);
  delay(1500);
  digitalWrite(LED_PIN, LOW);
  lcd.clear();
}

// --------------------------------------------------
// SERIAL COMMANDS
// --------------------------------------------------

void procesarSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "DATA") {
    File file = SPIFFS.open(archivo, FILE_READ);
    if (!file) Serial.println("[]");
    else {
      Serial.println(file.readString());
      file.close();
    }
  } else if (cmd == "PING") {
    Serial.println("PONG");
  } else if (cmd.startsWith("SET_CONFIG ")) {
    String jsonStr = cmd.substring(11);
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, jsonStr)) {
      guardarConfig(
        String(doc["ssid"] | ""),
        String(doc["password"] | ""),
        String(doc["api_key"] | ""),
        String(doc["server_url"] | "")
      );
      Serial.println("CONFIG_SAVED");
      delay(500);
      ESP.restart();
    } else {
      Serial.println("CONFIG_ERROR: invalid JSON");
    }
  } else if (cmd == "GET_CONFIG") {
    if (SPIFFS.exists(CONFIG_FILE)) {
      File f = SPIFFS.open(CONFIG_FILE, FILE_READ);
      Serial.println(f.readString());
      f.close();
    } else {
      Serial.println("NO_CONFIG");
    }
  } else if (cmd == "RESET_CONFIG") {
    borrarConfig();
    Serial.println("CONFIG_RESET");
    delay(500);
    ESP.restart();
  }
}

// --------------------------------------------------
// FECHA / HORA
// --------------------------------------------------

int diasEnMes(int mes, int anio) {
  switch (mes) {
    case 1: case 3: case 5: case 7: case 8: case 10: case 12: return 31;
    case 4: case 6: case 9: case 11: return 30;
    case 2:
      return ((anio % 4 == 0 && anio % 100 != 0) || (anio % 400 == 0)) ? 29 : 28;
    default:
      return 31;
  }
}

void ajustarDiaFecha() {
  int maxDia = diasEnMes(fechaMes, fechaAnio);
  if (fechaDia < 1) fechaDia = 1;
  if (fechaDia > maxDia) fechaDia = maxDia;
}

void cargarFechaDesdeRTC() {
  DateTime now = rtc.now();
  fechaDia = now.day();
  fechaMes = now.month();
  fechaAnio = now.year();
  fechaHora = now.hour();
  fechaMin = now.minute();
  fechaSeg = now.second();
  ajustarDiaFecha();
}

void cambiarCampoFecha(int delta) {
  switch (fechaCampo) {
    case 0:
      fechaDia += delta;
      if (fechaDia > diasEnMes(fechaMes, fechaAnio)) fechaDia = 1;
      if (fechaDia < 1) fechaDia = diasEnMes(fechaMes, fechaAnio);
      break;
    case 1:
      fechaMes += delta;
      if (fechaMes > 12) fechaMes = 1;
      if (fechaMes < 1) fechaMes = 12;
      ajustarDiaFecha();
      break;
    case 2:
      fechaAnio += delta;
      if (fechaAnio > 2099) fechaAnio = 2000;
      if (fechaAnio < 2000) fechaAnio = 2099;
      ajustarDiaFecha();
      break;
    case 3:
      fechaHora += delta;
      if (fechaHora > 23) fechaHora = 0;
      if (fechaHora < 0) fechaHora = 23;
      break;
    case 4:
      fechaMin += delta;
      if (fechaMin > 59) fechaMin = 0;
      if (fechaMin < 0) fechaMin = 59;
      break;
    case 5:
      fechaSeg += delta;
      if (fechaSeg > 59) fechaSeg = 0;
      if (fechaSeg < 0) fechaSeg = 59;
      break;
  }
}

void mostrarModoFecha() {
  char linea0[21];
  char linea1[21];
  char linea2[21];
  const char* campos[] = {"DIA", "MES", "ANIO", "HORA", "MIN", "SEG"};

  sprintf(linea0, "FECHA MANUAL %s", campos[fechaCampo]);
  switch (fechaCampo) {
    case 0: sprintf(linea1, "[%02d]/%02d/%04d", fechaDia, fechaMes, fechaAnio); break;
    case 1: sprintf(linea1, "%02d/[%02d]/%04d", fechaDia, fechaMes, fechaAnio); break;
    case 2: sprintf(linea1, "%02d/%02d/[%04d]", fechaDia, fechaMes, fechaAnio); break;
    default: sprintf(linea1, "%02d/%02d/%04d", fechaDia, fechaMes, fechaAnio); break;
  }

  switch (fechaCampo) {
    case 3: sprintf(linea2, "[%02d]:%02d:%02d", fechaHora, fechaMin, fechaSeg); break;
    case 4: sprintf(linea2, "%02d:[%02d]:%02d", fechaHora, fechaMin, fechaSeg); break;
    case 5: sprintf(linea2, "%02d:%02d:[%02d]", fechaHora, fechaMin, fechaSeg); break;
    default: sprintf(linea2, "%02d:%02d:%02d", fechaHora, fechaMin, fechaSeg); break;
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(linea0);
  lcd.setCursor(0, 1);
  lcd.print(linea1);
  lcd.setCursor(0, 2);
  lcd.print(linea2);
  lcd.setCursor(0, 3);
  lcd.print("A/B +/- D=Sig #=OK");
}

bool sincronizarRTCInternet() {
  if (!wifiConectado || WiFi.status() != WL_CONNECTED) return false;
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  struct tm timeinfo;
  unsigned long inicio = millis();
  while (!getLocalTime(&timeinfo) && millis() - inicio < 10000) {
    delay(200);
  }

  if (!getLocalTime(&timeinfo)) return false;
  rtc.adjust(DateTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  ));
  lastRtcInternetSync = millis();
  internetDisponible = true;
  return true;
}

void iniciarModoFecha() {
  if (internetDisponible) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("FECHA POR INTERNET");
    if (sincronizarRTCInternet()) {
      lcd.setCursor(0, 1);
      lcd.print("ACTUALIZADA");
    } else {
      lcd.setCursor(0, 1);
      lcd.print("NO SE PUDO");
    }
    delay(2000);
    lcd.clear();
    return;
  }

  modoFecha = true;
  fechaCampo = 0;
  cargarFechaDesdeRTC();
  mostrarModoFecha();
}

// --------------------------------------------------
// PAIRING: PIN-based device pairing
// --------------------------------------------------

void iniciarModoPairing() {
  modoPairing = true;
  pinInput = "";
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("--- VINCULAR ---");
  lcd.setCursor(0, 1);
  lcd.print("Ingrese PIN:");
  lcd.setCursor(0, 2);
  lcd.print("______");
  lcd.setCursor(0, 3);
  lcd.print("A=OK  C=Cancelar");
}

void enviarPIN(String pin) {
  if (!wifiConectado || WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("SIN CONEXION WiFi");
    delay(2000);
    return;
  }

  lcd.clear();
  lcd.setCursor(0, 1);
  lcd.print("Enviando PIN...");

  HTTPClient http;
  String url = cfgServerUrl + "/scale/pair";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  String body = "{\"pin\":\"" + pin + "\"}";
  int code = http.POST(body);
  String response = http.getString();
  http.end();

  if (code == 200) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, response)) {
      String apiKey = String(doc["api_key"] | "");
      String deviceName = String(doc["device_name"] | "");
      if (apiKey.length() > 0) {
        guardarConfig(cfgSsid, cfgPassword, apiKey, cfgServerUrl);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("EMPAREJADO!");
        lcd.setCursor(0, 1);
        lcd.print(deviceName.substring(0, 20));
        delay(2500);
        ESP.restart();
        return;
      }
    }
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("ERROR DE RESPUESTA");
    delay(2000);
    return;
  } else if (code == 401) {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("PIN INVALIDO");
    delay(2000);
    return;
  } else {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("ERROR DE RED");
    delay(2000);
    return;
  }
}

// --------------------------------------------------
// SETUP
// --------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(SDA_LCD, SCL_LCD);
  I2C_RTC.begin(SDA_RTC, SCL_RTC, 100000);

  lcd.init();
  lcd.backlight();

  rtc.begin(&I2C_RTC);
  if (!SPIFFS.begin(true)) {
    Serial.println("Error SPIFFS");
  }

  if (!SPIFFS.exists(archivo)) {
    File file = SPIFFS.open(archivo, FILE_WRITE);
    file.print("[]");
    file.close();
  }

  limpiarRegistrosAntiguos();
  contarPendientes();

  if (cargarConfig()) {
    if (conectarWiFi()) {
      server.on("/data", HTTP_GET, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        File file = SPIFFS.open(archivo, FILE_READ);
        if (!file) { server.send(200, "application/json", "[]"); return; }
        server.send(200, "application/json", file.readString());
        file.close();
      });
      server.begin();
      if (cfgApiKey.length() == 0) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Sin API Key");
        lcd.setCursor(0, 1);
        lcd.print("Presione D > 1/2");
        lcd.setCursor(0, 2);
        lcd.print("para vincular");
        delay(3000);
      } else {
        verificarInternet();
        if (internetDisponible) sincronizarRTCInternet();
        sincronizarNube();
      }
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("WiFi no disponible");
      lcd.setCursor(0, 1);
      lcd.print("Modo offline");
      delay(2000);
    }
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sin config WiFi");
    lcd.setCursor(0, 1);
    lcd.print("Use D > Config WiFi");
    delay(2000);
  }

  lcd.clear();
}

// --------------------------------------------------
// LOOP
// --------------------------------------------------

void loop() {
  server.handleClient();
  procesarSerial();

  if (modoConfiguracion) {
    char tecla = keypad.getKey();
    if (tecla == 'C') {
      salirModoConfig();
    }
    return;
  }

  if (modoFecha) {
    char tecla = keypad.getKey();
    if (tecla) {
      if (tecla == 'C') {
        modoFecha = false;
        lcd.clear();
      } else if (tecla == 'A') {
        cambiarCampoFecha(-1);
        mostrarModoFecha();
      } else if (tecla == 'B') {
        cambiarCampoFecha(1);
        mostrarModoFecha();
      } else if (tecla == 'D') {
        fechaCampo = (fechaCampo + 1) % 6;
        mostrarModoFecha();
      } else if (tecla == '#') {
        ajustarDiaFecha();
        rtc.adjust(DateTime(fechaAnio, fechaMes, fechaDia, fechaHora, fechaMin, fechaSeg));
        modoFecha = false;
        lcd.clear();
        lcd.setCursor(0, 1);
        lcd.print("FECHA GUARDADA");
        digitalWrite(LED_PIN, HIGH);
        delay(1500);
        digitalWrite(LED_PIN, LOW);
        lcd.clear();
      }
    }
    return;
  }

  if (modoPairing) {
    char tecla = keypad.getKey();
    if (tecla) {
      if (tecla == 'C') {
        modoPairing = false;
        pinInput = "";
        lcd.clear();
      } else if (tecla == 'B') {
        if (pinInput.length() > 0) {
          pinInput.remove(pinInput.length() - 1);
          String display = pinInput;
          for (int i = display.length(); i < 6; i++) display += "_";
          lcd.setCursor(0, 2);
          lcd.print(display + "              ");
        }
      } else if (isdigit(tecla)) {
        if (pinInput.length() < 6) {
          pinInput += tecla;
          String display = pinInput;
          for (int i = display.length(); i < 6; i++) display += "_";
          lcd.setCursor(0, 2);
          lcd.print(display + "              ");
        }
      } else if (tecla == 'A') {
        if (pinInput.length() == 6) {
          enviarPIN(pinInput);
          if (modoPairing) {
            pinInput = "";
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("--- VINCULAR ---");
            lcd.setCursor(0, 1);
            lcd.print("Ingrese PIN:");
            lcd.setCursor(0, 2);
            lcd.print("______");
            lcd.setCursor(0, 3);
            lcd.print("A=OK  C=Cancelar");
          }
        }
      }
    }
    return;
  }

  if (wifiConectado && WiFi.status() != WL_CONNECTED) {
    wifiConectado = false;
    internetDisponible = false;
  }
  if (!wifiConectado && WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
  }
  if (!wifiConectado && cfgSsid.length() > 0 && millis() - lastReconnectAttempt > 30000) {
    lastReconnectAttempt = millis();
    WiFi.reconnect();
  }

  if (wifiConectado && millis() - lastInternetCheck >= INTERNET_CHECK_INTERVAL) {
    lastInternetCheck = millis();
    verificarInternet();
    if (internetDisponible && (lastRtcInternetSync == 0 || millis() - lastRtcInternetSync >= 21600000UL)) {
      sincronizarRTCInternet();
    }
  }

  if (wifiConectado && pendingCount > 0 && millis() - lastSync >= SYNC_INTERVAL_MS) {
    lastSync = millis();
    if (sincronizarNube()) {
      internetDisponible = true;
    }
  }

  if (millis() - lastUpdate >= 1000 && !viendoRegistros && !editandoPeso && !editandoCodigo && !menuDVisible && !modoConfiguracion && !modoPairing) {
    lastUpdate = millis();
    DateTime now = rtc.now();

    char linea0[21];
    sprintf(linea0, "%02d/%02d/%04d %02d:%02d:%02d",
            now.day(), now.month(), now.year(),
            now.hour(), now.minute(), now.second());
    lcd.setCursor(0, 0);
    lcd.print(linea0);

    if (!ingresandoPeso && codigo.length() == 0) {
      const char* estado = "SinCfg";
      if (cfgSsid.length() > 0) {
        if (!wifiConectado) estado = "SinRed";
        else if (!internetDisponible) estado = "SinNet";
        else estado = "Online";
      }
      char linea3[21];
      sprintf(linea3, "P:%-3d %s %s",
              pendingCount, estado, lastSyncTime.c_str());
      lcd.setCursor(0, 3);
      lcd.print(linea3);
      lcd.print("    ");
    }
  }

  // --- KEYPAD ---
  char tecla = keypad.getKey();
  if (tecla) {
    
    // CANCEL GLOBAL (Cancela todo el flujo y regresa al estado inicial)
    if (tecla == 'C') {
      if (viendoRegistros) {
        viendoRegistros = false;
        editandoCodigo = false;
        editandoPeso = false;
        lcd.clear();
        return;
      }
      if (menuDVisible) {
        menuDVisible = false;
        lcd.clear();
        return;
      }
      codigo = "";
      peso = "";
      ingresandoPeso = false;
      lcd.clear();
      lcd.setCursor(5, 2);
      lcd.print("CANCELADO");
      digitalWrite(LED_PIN, HIGH);
      delay(800);
      digitalWrite(LED_PIN, LOW);
      lcd.clear();
      return;
    }

    // MENU D.
    if (tecla == 'D' && !menuDVisible && !viendoRegistros) {
      menuDVisible = true;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("---- MENU ----");
      lcd.setCursor(0, 1);
      lcd.print("A=Borrar registros");
      lcd.setCursor(0, 2);
      lcd.print("B=Config WiFi");
      lcd.setCursor(0, 3);
      lcd.print("1=Vincular 2=Fecha");
      return;
    }

    if (menuDVisible) {
      if (tecla == 'A') {
        menuDVisible = false;
        borrarRegistros();
      } else if (tecla == 'B') {
        menuDVisible = false;
        iniciarModoConfig();
      } else if (tecla == '1') {
        menuDVisible = false;
        iniciarModoPairing();
      } else if (tecla == '2') {
        menuDVisible = false;
        iniciarModoFecha();
      } else if (tecla == 'D') {
        menuDVisible = false;
        borrarConfig();
        WiFi.disconnect();
        wifiConectado = false;
        internetDisponible = false;
        cfgSsid = "";
        cfgPassword = "";
        lcd.clear();
        lcd.setCursor(0, 1);
        lcd.print("RED OLVIDADA");
        lcd.setCursor(0, 2);
        lcd.print("Use D > Config WiFi");
        digitalWrite(LED_PIN, HIGH);
        delay(1500);
        digitalWrite(LED_PIN, LOW);
        lcd.clear();
      } else {
        menuDVisible = false;
        lcd.clear();
      }
      return;
    }

    // RECORD VIEWER
    if (viendoRegistros) {
      
      // EDICION DE CODIGO
      if (editandoCodigo) {
        if (isdigit(tecla)) {
          editCodigo += tecla;
        } else if (tecla == '#' || tecla == 'B') {
          if (editCodigo.length() > 0) editCodigo.remove(editCodigo.length() - 1);
        } else if (tecla == 'A') {
          // Si deja el código vacío, mantiene el anterior
          if (editCodigo.length() == 0) editCodigo = regCodigos[regIndice]; 
          
          editandoCodigo = false;
          editandoPeso = true;
          editPeso = "";
          
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Editar Peso:");
          lcd.setCursor(0, 1);
          lcd.print("Act: " + regPesos[regIndice] + " kg");
          lcd.setCursor(0, 2);
          lcd.print("Nvo: ");
          lcd.setCursor(0, 3);
          lcd.print("A=OK  C=Cancelar");
          return;
        }
        
        if (editandoCodigo) {
          lcd.setCursor(0, 2);
          lcd.print("Nvo: " + editCodigo + "          ");
        }
        return;
      }

      // EDICION DE PESO
      if (editandoPeso) {
        if (isdigit(tecla)) {
          editPeso += tecla;
        } else if (tecla == '*' && editPeso.indexOf('.') == -1) {
          editPeso += '.';
        } else if (tecla == '#' || tecla == 'B') {
          if (editPeso.length() > 0) editPeso.remove(editPeso.length() - 1);
        } else if (tecla == 'A') {
          // Si deja el peso vacío, mantiene el anterior
          if (editPeso.length() == 0) editPeso = regPesos[regIndice]; 
          
          // Guardar ambos cambios
          editarRegistroPorId(regIds[regIndice], editCodigo, editPeso);
          regCodigos[regIndice] = editCodigo;
          regPesos[regIndice] = editPeso;
          
          editandoPeso = false;
          editCodigo = "";
          editPeso = "";
          
          lcd.clear();
          lcd.setCursor(2, 1);
          lcd.print("REG GUARDADO");
          digitalWrite(LED_PIN, HIGH);
          delay(1200);
          digitalWrite(LED_PIN, LOW);
          mostrarRegistro();
        }
        
        if (editandoPeso) {
          lcd.setCursor(0, 2);
          lcd.print("Nvo: " + editPeso + "          ");
        }
        return;
      }

      if (tecla == 'A' && regTotal > 0) {
        regIndice = (regIndice + 1) % regTotal;
        mostrarRegistro();
      } else if (tecla == 'B' && regTotal > 0) {
        regIndice = (regIndice - 1 + regTotal) % regTotal;
        mostrarRegistro();
      } else if (tecla == 'D' && regTotal > 0) {
        int id = regIds[regIndice];
        eliminarRegistroPorId(id);
        cargarRegistrosHoy();
        if (regTotal == 0) {
          mostrarRegistro();
        } else {
          if (regIndice >= regTotal) regIndice = regTotal - 1;
          lcd.clear();
          lcd.setCursor(3, 1);
          lcd.print("REGISTRO BORRADO");
          digitalWrite(LED_PIN, HIGH);
          delay(1200);
          digitalWrite(LED_PIN, LOW);
          mostrarRegistro();
        }
      } else if (tecla == '#' && regTotal > 0) {
        // Iniciar flujo de edición: Primero código, luego peso
        editandoCodigo = true;
        editandoPeso = false;
        editCodigo = "";
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Editar Codigo:");
        lcd.setCursor(0, 1);
        lcd.print("Act: " + regCodigos[regIndice]);
        lcd.setCursor(0, 2);
        lcd.print("Nvo: ");
        lcd.setCursor(0, 3);
        lcd.print("A=Sig  C=Cancelar");
      }
      return;
    }

    // MANUAL SYNC with # (when no input active)
    if (tecla == '#' && codigo.length() == 0 && !ingresandoPeso) {
      lcd.clear();
      lcd.setCursor(0, 1);
      lcd.print("Sincronizando...");
      bool ok = sincronizarNube();
      lastSync = millis();
      lcd.clear();
      lcd.setCursor(0, 1);
      lcd.print(ok ? "Sync OK!" : "Sync FALLO");
      digitalWrite(LED_PIN, HIGH);
      delay(1200);
      digitalWrite(LED_PIN, LOW);
      lcd.clear();
      return;
    }

    // CODE INPUT MODE
    if (!ingresandoPeso) {
      if (tecla == '*' && codigo.length() == 0) {
        viendoRegistros = true;
        regIndice = 0;
        cargarRegistrosHoy();
        mostrarRegistro();
        return;
      } else if (isdigit(tecla)) {
        codigo += tecla;
      } else if (tecla == '#' || tecla == 'B') {
        if (codigo.length() > 0) codigo.remove(codigo.length() - 1);
      } else if (tecla == 'A' && codigo.length() > 0) {
        ingresandoPeso = true;
      }

    // WEIGHT INPUT MODE
    } else {
      if (isdigit(tecla)) {
        peso += tecla;
      } else if (tecla == '*' && peso.indexOf('.') == -1) {
        peso += '.';
      } else if (tecla == '#' || tecla == 'B') {
        if (peso.length() > 0) peso.remove(peso.length() - 1);
      } else if (tecla == 'A' && peso.length() > 0) {
        guardarRegistro(codigo, peso);
        codigo = "";
        peso = "";
        ingresandoPeso = false;
        mostrarOK();
        return;
      }
    }

    // Update LCD
    lcd.setCursor(0, 1);
    lcd.print("Codigo: " + codigo + "      ");

    lcd.setCursor(0, 2);
    lcd.print("Peso: " + peso + "          ");

    lcd.setCursor(0, 3);
    if (!ingresandoPeso) lcd.print("Ingrese Codigo   ");
    else lcd.print("Ingrese Peso     ");
  }
}