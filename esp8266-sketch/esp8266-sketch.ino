#include <ESP8266WiFi.h>
#include <Wire.h>  
#include <Adafruit_BME280.h>
#include <math.h>
#include <Scheduler.h>
#include <Task.h>
#include <LeanTask.h>

static const uint8_t D0 = 16;
static const uint8_t D1 = 5;
static const uint8_t D2 = 4;
static const uint8_t D3 = 0;
static const uint8_t D4 = 2;
static const uint8_t D5 = 14;
static const uint8_t D6 = 12;
static const uint8_t D7 = 13;
static const uint8_t D8 = 15;
static const uint8_t D9 = 3;
static const uint8_t D10 = 1;

// Wi-Fi Network
const char* ssid = "OnePlus Nord 4";
const char* password = "darkside";
WiFiServer server(80);

// Sensors
Adafruit_BME280 bme;
uint8_t bme280SDA = D2;
uint8_t bme280SCL = D1;
uint8_t rainDigital = D3;
uint8_t lightAnalog = A0;

// Main variables
const float SEALEVELPRESSURE_HPA = 1057.00; // for SPb
const float ELEVATION_M = 20.0;
const float ALPHA_P = 0.1f;                               // EMA
const unsigned long TREND_WINDOW_MS = 30UL*60UL*1000UL;   // 30 mins
float seaLevelPressure = 0;     
static float p0_ema = NAN;      
static float p0_mark = NAN; 
static unsigned long lastTrendMark = 0;
const int LIGHT_NIGHT  = 200;
const int LIGHT_DAY    = 400;
const int LIGHT_SUNNY  = 700;
float humidity = 0, temperature = 0, light = 0, rain = 0, pressure = 0, altitude = 0;
bool isRainy = false;
String weather = "";

static inline float toSeaLevelPressure(float p_hPa, float elev_m) {
  return p_hPa / powf(1.0f - elev_m / 44330.0f, 5.255f);
}

static inline float dewPointC(float T, float RH) {
  const float a = 17.62f, b = 243.12f;
  float gamma = (a * T) / (b + T) + logf(RH / 100.0f);
  return (b * gamma) / (a - gamma);
}

static inline const char* trendStr(float dP) {
  if (dP > 1.0f)  return "increase";
  if (dP < -1.0f) return "decrease";
  return "stable";
}

// Task для чтения BME280 (температура и влажность)
class BME280Task : public LeanTask {
public:
  void loop() {
    temperature = bme.readTemperature();
    humidity = bme.readHumidity();
    Serial.println("BME280 temp/humidity updated");
    delay(1000); 
  }
} bme280_task;

class PressureTask : public Task {
public:
  void loop() {
    Serial.println("Reading pressure...");
    delay(10000); // задержва 10 с
    
    pressure = bme.readPressure() / 100.0F;
    altitude = bme.readAltitude(SEALEVELPRESSURE_HPA);
    
    seaLevelPressure = toSeaLevelPressure(pressure, ELEVATION_M);
    if (isnan(p0_ema)) p0_ema = seaLevelPressure;
    p0_ema += ALPHA_P * (seaLevelPressure - p0_ema);

    if (isnan(p0_mark) || (millis() - lastTrendMark) > TREND_WINDOW_MS) {
      p0_mark = p0_ema;
      lastTrendMark = millis();
    }
    
    Serial.println("Pressure updated");
    delay(5000); 
  }
} pressure_task;

class LightTask : public LeanTask {
public:
  void loop() {
    light = 1024 - analogRead(lightAnalog);
    Serial.println("Light sensor updated");
    delay(500);
  }
} light_task;

// Task для чтения датчика дождя
class RainTask : public LeanTask {
public:
  void loop() {
    rain = digitalRead(rainDigital);
    isRainy = rain < 1.00;
    Serial.println("Rain sensor updated");
    delay(200);
  }
} rain_task;

class WeatherTask : public LeanTask {
public:
  void loop() {
    float dP = p0_ema - p0_mark;
    float Td = dewPointC(temperature, humidity);
    float spread = temperature - Td;

    if (isRainy) {
      if (temperature < 0.0f) {
        weather = (light < LIGHT_NIGHT) ? "Snow (night)" : "Snow";
      } else {
        weather = (light < LIGHT_NIGHT) ? "Rain (night)" : "Rain";
      }
      if (p0_ema < 1000.0f || dP < -1.0f) weather += " • low pressure";
    }
    else if ((humidity >= 95.0f || spread < 2.0f) && light < 600) {
      weather = "Fog";
    }
    else {
      if (light >= LIGHT_SUNNY && p0_ema >= 1018.0f) {
        weather = "Sunny";
      } else if (light >= LIGHT_DAY) {
        weather = (p0_ema >= 1010.0f ? "Partly cloudy" : "Cloudy");
      } else if (light < LIGHT_NIGHT) {
        weather = "Night";
      } else {
        weather = (p0_ema < 1005.0f ? "Dull" : "Cloudy");
      }
    }
    
    Serial.println("Weather calculated: " + weather);
    delay(2000); // Пересчитываем каждые 2 секунды
  }
} weather_task;

class WebServerTask : public LeanTask {
public:
  void loop() {
    WiFiClient client = server.available();
    if (client) {
      String request = client.readStringUntil('\r');
      Serial.print("request: ");
      Serial.println(request);
      client.flush();
      client.print(getResponse());
    }
    delay(50); 
  }
} webserver_task;

void setup() {
  Serial.begin(115200);
  
  delay(100);
  Wire.begin(bme280SDA, bme280SCL);
  Wire.setClock(400000);
  bme.begin(0x76);
  pinMode(rainDigital, INPUT);
  
  WiFi.disconnect();
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Successfully connected.");
  Serial.println("\nGot IP: ");
  Serial.print(WiFi.localIP());
  
  server.begin();

  Scheduler.start(&bme280_task);
  Scheduler.start(&pressure_task);
  Scheduler.start(&light_task);
  Scheduler.start(&rain_task);
  Scheduler.start(&weather_task);
  Scheduler.start(&webserver_task);

  Scheduler.begin();
}

void loop() {
}

String getResponse() {
  String ptr = "<!DOCTYPE html>";
  ptr += "<html>";
  ptr += "<head>";
  ptr += "<title>ESP8266 Weather Station</title>";
  ptr += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  ptr += "<link href='https://fonts.googleapis.com/css?family=Open+Sans:300,400,600' rel='stylesheet'>";
  ptr += "<style>";
  ptr += "html { font-family: 'Open Sans', sans-serif; display: block; margin: 0px auto; text-align: center;color: #444444;}";
  ptr += "body{margin: 0px;} ";
  ptr += "h1 {margin: 50px auto 30px;} ";
  ptr += ".side-by-side{display: table-cell;vertical-align: middle;position: relative;}";
  ptr += ".text{font-weight: 600;font-size: 19px;width: 200px;}";
  ptr += ".reading{font-weight: 300;font-size: 50px;padding-right: 25px;}";
  ptr += ".temperature .reading{color: #F29C1F;}";
  ptr += ".humidity .reading{color: #3B97D3;}";
  ptr += ".pressure .reading{color: #26B99A;}";
  ptr += ".altitude .reading{color: #955BA5;}";
  ptr += ".light .reading{color: #ebc034;}";
  ptr += ".rain .reading{color: #949494;}";
  ptr += ".superscript{font-size: 17px;font-weight: 600;position: absolute;top: 10px;}";
  ptr += ".data{padding: 10px;}";
  ptr += ".container{display: table;margin: 0 auto;}";
  ptr += ".icon{width:65px}";
  ptr += "</style>";
  ptr += "<script>\n";
  ptr += "setInterval(loadDoc,1000);\n";
  ptr += "function loadDoc() {\n";
  ptr += "var xhttp = new XMLHttpRequest();\n";
  ptr += "xhttp.onreadystatechange = function() {\n";
  ptr += "if (this.readyState == 4 && this.status == 200) {\n";
  ptr += "document.body.innerHTML =this.responseText}\n";
  ptr += "};\n";
  ptr += "xhttp.open(\"GET\", \"/\", true);\n";
  ptr += "xhttp.send();\n";
  ptr += "}\n";
  ptr += "</script>\n";
  ptr += "</head>";
  ptr += "<body>";
  ptr += "<h1>";
  ptr += weather;
  ptr += "</h1>";
  ptr += "<div class='container'>";
  ptr += "<div class='data temperature'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64px\" height=\"64px\" viewBox=\"0 0 1024 1024\" class=\"icon\" version=\"1.1\"><path d=\"M570.8 742c0.5-2.9 0.9-5.8 0.9-8.8V418.5H458.4v314.6c0 3 0.4 5.9 0.9 8.8-34.3 19.5-57.5 56.3-57.5 98.5 0 62.6 50.7 113.3 113.3 113.3 62.6 0 113.3-50.7 113.3-113.3-0.1-42.2-23.3-79-57.6-98.4z\" fill=\"#F59558\"/><path d=\"M594.3 730.3V194.8c0-43.7-35.6-79.3-79.3-79.3s-79.3 35.6-79.3 79.3v535.4c-35.2 25.4-56.6 66.5-56.6 110.2 0 75 61 135.9 135.9 135.9s136-60.8 136-135.8c0-43.7-21.4-84.8-56.7-110.2zM515 931.1c-50 0-90.6-40.6-90.6-90.6 0-32.1 17.4-62.1 45.3-78.4 7-4 11.3-11.5 11.3-19.6v-40.8h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481v-45.3h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481v-45.3h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481V498h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481v-45.3h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481v-45.3h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481v-45.3h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481V226h11.3c6.3 0 11.3-5.1 11.3-11.3 0-6.3-5.1-11.3-11.3-11.3H481v-8.5c0-18.7 15.2-34 34-34 18.7 0 34 15.2 34 34v547.7c0 8.1 4.3 15.6 11.3 19.6 28 16.2 45.3 46.2 45.3 78.4 0.1 49.9-40.6 90.5-90.6 90.5z\" fill=\"#211F1E\"/></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Temperature</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)temperature;
  ptr += "<span class='superscript'>&deg;C</span></div>";
  ptr += "</div>";
  ptr += "<div class='data humidity'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64px\" height=\"64px\" viewBox=\"0 0 32 32\" enable-background=\"new 0 0 32 32\" version=\"1.1\" xml:space=\"preserve\">\n\n<g id=\"Energy20\"/>\n\n<g id=\"Energy19\"/>\n\n<g id=\"Energy18\"/>\n\n<g id=\"Energy17\"/>\n\n<g id=\"Energy16\"/>\n\n<g id=\"Energy15\"/>\n\n<g id=\"Energy14\"/>\n\n<g id=\"Energy13\"/>\n\n<g id=\"Energy12\"/>\n\n<g id=\"Energy11\">\n\n<g>\n\n<path d=\"M28,19c0,6.62-5.38,12-12,12S4,25.62,4,19C4,12.58,14.83,1.75,15.3,1.29c0.39-0.39,1.01-0.39,1.4,0    C17.17,1.75,28,12.58,28,19z\" fill=\"#34B0C0\"/>\n\n</g>\n\n<g>\n\n<path d=\"M14,26c-3.3086,0-6-2.6914-6-6c0-0.5527,0.4478-1,1-1s1,0.4473,1,1c0,2.2061,1.7944,4,4,4    c0.5522,0,1,0.4473,1,1S14.5522,26,14,26z\" fill=\"#FFFFFF\"/>\n\n</g>\n\n</g>\n\n<g id=\"Energy10\"/>\n\n<g id=\"Energy09\"/>\n\n<g id=\"Energy08\"/>\n\n<g id=\"Energy07\"/>\n\n<g id=\"Energy06\"/>\n\n<g id=\"Energy05\"/>\n\n<g id=\"Energy04\"/>\n\n<g id=\"Energy03\"/>\n\n<g id=\"Energy02\"/>\n\n<g id=\"Energy01\"/>\n\n</svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Humidity</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)humidity;
  ptr += "<span class='superscript'>%</span></div>";
  ptr += "</div>";
  ptr += "<div class='data pressure'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" height=\"64px\" width=\"64px\" version=\"1.1\" id=\"Layer_1\" viewBox=\"0 0 508 508\" xml:space=\"preserve\">\n<circle style=\"fill:#FD8469;\" cx=\"254\" cy=\"254\" r=\"254\"/>\n<path style=\"fill:#324A5E;\" d=\"M254,446.8c-106.4,0-192.8-86.4-192.8-192.8S147.6,61.2,254,61.2S446.8,147.6,446.8,254  S360.4,446.8,254,446.8z\"/>\n<path style=\"fill:#E6E9EE;\" d=\"M406.8,262.8c0-2.8,0.4-6,0.4-8.8c0-84.4-68.4-153.2-153.2-153.2c-84.4,0-153.2,68.4-153.2,153.2  c0,2.8,0,6,0.4,8.8H406.8z\"/>\n<path style=\"fill:#FF7058;\" d=\"M204.8,166.4c-0.8-1.2-2-1.6-3.2-0.8c-1.2,0.4-2,2-1.2,3.2l42,101.2c0.4,0.4,0.4,1.2,0.8,1.6  c3.2,5.2,10,6.8,15.2,3.6c5.2-3.2,6.8-10,3.6-15.2L204.8,166.4z\"/>\n<circle style=\"fill:#FFD05B;\" cx=\"254\" cy=\"262.8\" r=\"27.6\"/>\n<g>\n\t<path style=\"fill:#ACB3BA;\" d=\"M254,136L254,136c-2.8,0-5.2-2.4-5.2-5.2V120c0-2.8,2.4-5.2,5.2-5.2l0,0c2.8,0,5.2,2.4,5.2,5.2v10.8   C259.2,133.6,256.8,136,254,136z\"/>\n\t<path style=\"fill:#ACB3BA;\" d=\"M337.6,170.4L337.6,170.4c-2-2-2-5.2,0-7.2l7.6-7.6c2-2,5.2-2,7.2,0l0,0c2,2,2,5.2,0,7.2l-7.6,7.6   C342.4,172.4,339.2,172.4,337.6,170.4z\"/>\n\t<path style=\"fill:#ACB3BA;\" d=\"M156,156L156,156c2-2,5.2-2,7.2,0l7.6,7.6c2,2,2,5.2,0,7.2l0,0c-2,2-5.2,2-7.2,0l-7.6-7.6   C154,161.2,154,158,156,156z\"/>\n\t<path style=\"fill:#ACB3BA;\" d=\"M208.8,144.8L208.8,144.8c-2.4,1.2-5.6,0-6.4-2.8l-4-10c-1.2-2.4,0-5.6,2.8-6.4l0,0   c2.4-1.2,5.6,0,6.4,2.8l4,10C212.8,140.8,211.6,144,208.8,144.8z\"/>\n\t<path style=\"fill:#ACB3BA;\" d=\"M363.2,208.8L363.2,208.8c-1.2-2.4,0-5.6,2.8-6.4l10-4c2.4-1.2,5.6,0,6.4,2.8l0,0   c1.2,2.4,0,5.6-2.8,6.4l-10,4C367.2,212.8,364,211.6,363.2,208.8z\"/>\n\t<path style=\"fill:#ACB3BA;\" d=\"M299.2,144.8L299.2,144.8c-2.4-1.2-3.6-4-2.8-6.4l4-10c1.2-2.4,4-3.6,6.4-2.8l0,0   c2.4,1.2,3.6,4,2.8,6.4l-4,10C304.8,144.8,301.6,146,299.2,144.8z\"/>\n\t<path style=\"fill:#ACB3BA;\" d=\"M125.6,200.8L125.6,200.8c1.2-2.4,4-3.6,6.4-2.8l10,4c2.4,1.2,3.6,4,2.8,6.4l0,0   c-1.2,2.4-4,3.6-6.4,2.8l-10-4C126,206.4,124.8,203.6,125.6,200.8z\"/>\n</g>\n<g>\n\t<circle style=\"fill:#2B3B4E;\" cx=\"254\" cy=\"82.4\" r=\"10\"/>\n\t<circle style=\"fill:#2B3B4E;\" cx=\"254\" cy=\"425.6\" r=\"10\"/>\n\t<circle style=\"fill:#2B3B4E;\" cx=\"425.6\" cy=\"254\" r=\"10\"/>\n\t<circle style=\"fill:#2B3B4E;\" cx=\"82.4\" cy=\"254\" r=\"10\"/>\n</g>\n</svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Pressure</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)pressure;
  ptr += "<span class='superscript'>hPa</span></div>";
  ptr += "</div>";
  ptr += "<div class='data altitude'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" height=\"64px\" width=\"64px\" version=\"1.1\" id=\"Layer_1\" viewBox=\"0 0 512 512\" xml:space=\"preserve\">\n<path style=\"fill:#00384E;\" d=\"M384,184l-59.2,113.6L200,40L0,472h512L384,184z M400,288l-16,6.4l-17.6-6.4l17.6-35.2L400,288z   M240,193.6l-40,20.8l-40-20.8l40-84.8L240,193.6z M48,440l97.6-217.6l54.4,27.2l54.4-27.2l104,217.6H48z M342.4,332.8l8-17.6  l32,12.8l30.4-11.2L464,440h-70.4L342.4,332.8z\"/>\n<g>\n\t<polygon style=\"fill:#FFFFFF;\" points=\"400,288 384,294.4 366.4,288 384,252.8  \"/>\n\t<polygon style=\"fill:#FFFFFF;\" points=\"240,193.6 200,214.4 160,193.6 200,108.8  \"/>\n</g>\n<g>\n\t<polygon style=\"fill:#FAA85F;\" points=\"48,440 145.6,222.4 200,249.6 254.4,222.4 358.4,440  \"/>\n\t<polygon style=\"fill:#FAA85F;\" points=\"342.4,332.8 352,316.8 384,329.6 414.4,316.8 464,440 393.6,440  \"/>\n</g>\n</svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Altitude</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)altitude;
  ptr += "<span class='superscript'>m</span></div>";
  ptr += "</div>";
  ptr += "<div class='data light'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64px\" height=\"64px\" viewBox=\"0 0 1024 1024\" class=\"icon\" version=\"1.1\"><path d=\"M513.311597 95.397443c-156.598141 0-253.554962 137.570256-253.554962 265.040908 0 105.370087 44.014782 155.726815 82.912186 200.192106l7.377094 8.41429c20.519686 23.753116 25.437407 101.224375 22.39442 158.053082a25.298159 25.298159 0 0 0 6.79655 18.445294c4.725231 4.920793 11.19721 7.767194 18.057242 7.767194h8.122482v115.854673c0 13.722111 11.134753 24.853792 24.853793 24.853792h58.187403v9.967524c0 13.719039 11.130657 24.853792 24.853792 24.853792s24.853792-11.134753 24.853792-24.853792v-9.967524h58.283649c13.722111 0 24.853792-11.131681 24.853792-24.853792V753.310317h8.155247c6.860032 0 13.332011-2.846401 18.057241-7.767194a25.293039 25.293039 0 0 0 6.796551-18.445294c-3.041963-56.828707 1.875758-134.299966 22.39442-158.053082l7.442622-8.546371c38.831875-44.398739 82.845633-94.752396 82.845634-200.060025 0.001024-127.470651-96.954773-265.040907-253.682948-265.040908z\" fill=\"#27323A\"/><path d=\"M571.595245 844.311197H455.124194v-91.00088h116.471051v91.00088z\" fill=\"#79CCBF\"/><path d=\"M646.740237 527.812885l-7.638184 8.705073c-30.355128 35.176604-35.404931 104.432208-35.404931 155.239445 0 4.467212 0.12901 7.896204 0.195562 11.843282H422.827779c0.066553-3.947078 0.195562-7.37607 0.195562-11.843282 0-50.87379-5.049802-120.063865-35.372166-155.239445 0-0.032764-7.571632-8.639544-7.571632-8.639544-36.310045-41.552338-70.614299-80.774313-70.614299-167.441087 0-105.85336 76.244645-215.333323 203.847377-215.333323 127.731742 0 203.976387 109.478938 203.976387 215.333323-0.001024 86.601245-34.238725 125.824244-70.548771 167.375558z\" fill=\"#F4CE73\"/><path d=\"M460.237477 205.622794c3.496568 8.476747-0.517062 18.186251-8.993808 21.68282-42.134929 17.473627-61.196602 49.383013-69.739901 73.069576-13.917673 38.642456-10.163086 84.691699 10.160014 123.168285 4.27165 8.09279 1.166205 18.123794-6.925561 22.39442-8.088694 4.27165-18.123794 1.166205-22.39442-6.92556-24.916249-47.311693-29.317932-101.907306-12.035771-149.897836 15.336778-42.361208 46.665621-75.207449 88.247652-92.426128 8.479818-3.492473 18.189323 0.520134 21.681795 8.934423z\" fill=\"#FFFFFF\"/></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Light</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)light;
  ptr += "<span class='superscript'>lux</span></div>";
  ptr += "</div>";
  ptr += "<div class='data rain'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64px\" height=\"64px\" viewBox=\"0 0 512 512\" id=\"Layer_1\" version=\"1.1\" xml:space=\"preserve\">\n\n<style type=\"text/css\">\n\t.st0{fill:#3B4652;}\n\t.st1{fill:#2B79C2;}\n</style>\n\n<g>\n\n<path class=\"st1\" d=\"M327.2,187.5c0-41.6-33.7-75.3-75.3-75.3c-4.6,0-9.1,0.4-13.4,1.2c2.9-7.1,4.5-14.9,4.5-23   c0-33.5-27.2-60.7-60.7-60.7c-29.6,0-54.2,21.1-59.6,49.1c-0.4,0-0.7,0-1.1,0c-50.8,0-92.1,41.2-92.1,92.1s41.2,92.1,92.1,92.1   c27.6,0,105.1,0,130.3,0C293.5,262.8,327.2,229.1,327.2,187.5z\"/>\n\n<path class=\"st0\" d=\"M128.5,330.6c0,19,15.5,34.5,34.5,34.5c19,0,34.5-15.5,34.5-34.5c0-19-15.5-34.5-34.5-34.5   C143.9,296.1,128.5,311.6,128.5,330.6z M183.5,330.6c0,11.3-9.2,20.5-20.5,20.5c-11.3,0-20.5-9.2-20.5-20.5s9.2-20.5,20.5-20.5   C174.3,310.1,183.5,319.3,183.5,330.6z\"/>\n\n<path class=\"st0\" d=\"M287,307.1c-13,0-23.5,10.5-23.5,23.5s10.5,23.5,23.5,23.5c13,0,23.5-10.5,23.5-23.5S299.9,307.1,287,307.1z    M287,340.1c-5.2,0-9.5-4.3-9.5-9.5c0-5.2,4.3-9.5,9.5-9.5c5.2,0,9.5,4.3,9.5,9.5C296.5,335.9,292.2,340.1,287,340.1z\"/>\n\n<path class=\"st0\" d=\"M359.9,359.3c-19,0-34.5,15.5-34.5,34.5s15.5,34.5,34.5,34.5c19,0,34.5-15.5,34.5-34.5   S378.9,359.3,359.9,359.3z M359.9,414.3c-11.3,0-20.5-9.2-20.5-20.5c0-11.3,9.2-20.5,20.5-20.5s20.5,9.2,20.5,20.5   C380.4,405.1,371.2,414.3,359.9,414.3z\"/>\n\n<path class=\"st0\" d=\"M395.6,278.9c19,0,34.5-15.5,34.5-34.5c0-19-15.5-34.5-34.5-34.5c-19,0-34.5,15.5-34.5,34.5   C361.1,263.4,376.6,278.9,395.6,278.9z M395.6,223.9c11.3,0,20.5,9.2,20.5,20.5c0,11.3-9.2,20.5-20.5,20.5s-20.5-9.2-20.5-20.5   C375.1,233,384.3,223.9,395.6,223.9z\"/>\n\n<path class=\"st0\" d=\"M284.9,89.9c3.6,2.7,8,4.1,12.4,4.1c1,0,2.1-0.1,3.1-0.2c5.5-0.8,10.4-3.7,13.7-8.2c3.3-4.5,4.7-10,3.9-15.5v0   c-1.7-11.4-12.4-19.3-23.8-17.6c-11.4,1.7-19.3,12.4-17.6,23.8C277.5,81.7,280.4,86.6,284.9,89.9z M296.4,66.2   c0.3-0.1,0.7-0.1,1-0.1c3.4,0,6.3,2.5,6.9,5.9v0c0.3,1.8-0.2,3.7-1.3,5.1c-1.1,1.5-2.7,2.5-4.6,2.7c-1.8,0.3-3.7-0.2-5.1-1.3   c-1.5-1.1-2.5-2.7-2.7-4.6C290,70.3,292.6,66.8,296.4,66.2z\"/>\n\n<path class=\"st0\" d=\"M456,278.3c-14.6,0-26.4,11.9-26.4,26.4s11.9,26.4,26.4,26.4c14.6,0,26.4-11.9,26.4-26.4   S470.6,278.3,456,278.3z M456,317.2c-6.9,0-12.4-5.6-12.4-12.4s5.6-12.4,12.4-12.4c6.9,0,12.4,5.6,12.4,12.4S462.9,317.2,456,317.2   z\"/>\n\n<path class=\"st0\" d=\"M249.7,382.9c-19,0-34.5,15.5-34.5,34.5c0,19,15.5,34.5,34.5,34.5c19,0,34.5-15.5,34.5-34.5   C284.2,398.4,268.8,382.9,249.7,382.9z M249.7,437.9c-11.3,0-20.5-9.2-20.5-20.5c0-11.3,9.2-20.5,20.5-20.5   c11.3,0,20.5,9.2,20.5,20.5C270.2,428.7,261.1,437.9,249.7,437.9z\"/>\n\n<path class=\"st0\" d=\"M411.4,184.4c1.4,1.4,3.2,2.1,5,2.1s3.6-0.7,5-2.1c2.7-2.7,2.7-7.2,0-9.9l-37.7-37.7c-2.7-2.7-7.2-2.7-9.9,0   c-2.7,2.7-2.7,7.2,0,9.9L411.4,184.4z\"/>\n\n<path class=\"st0\" d=\"M440.7,213.1c1.4,1.4,3.2,2.1,5,2.1c1.8,0,3.6-0.7,5-2.1c2.7-2.7,2.7-7.2,0-9.9l-9.1-9.1   c-2.7-2.7-7.2-2.7-9.9,0c-2.7,2.7-2.7,7.2,0,9.9L440.7,213.1z\"/>\n\n<path class=\"st0\" d=\"M429.2,365.8c2.7-2.7,2.7-7.2,0-9.9l-55.3-55.3c-2.7-2.7-7.2-2.7-9.9,0c-2.7,2.7-2.7,7.2,0,9.9l55.3,55.3   c1.4,1.4,3.2,2,5,2C426.1,367.9,427.9,367.2,429.2,365.8z\"/>\n\n<path class=\"st0\" d=\"M458.2,384.2c-2.7-2.7-7.2-2.7-9.9,0c-2.7,2.7-2.7,7.2,0,9.9l14,14c1.4,1.4,3.2,2,5,2c1.8,0,3.6-0.7,5-2   c2.7-2.7,2.7-7.2,0-9.9L458.2,384.2z\"/>\n\n<path class=\"st0\" d=\"M140.4,397.2c-2.7-2.7-7.2-2.7-9.9,0c-2.7,2.7-2.7,7.2,0,9.9l69.6,69.6c1.4,1.4,3.2,2,5,2c1.8,0,3.6-0.7,5-2   c2.7-2.7,2.7-7.2,0-9.9L140.4,397.2z\"/>\n\n<path class=\"st0\" d=\"M332.2,441c-2.7-2.7-7.2-2.7-9.9,0c-2.7,2.7-2.7,7.2,0,9.9l17.1,17.1c1.4,1.4,3.2,2,5,2c1.8,0,3.6-0.7,5-2   c2.7-2.7,2.7-7.2,0-9.9L332.2,441z\"/>\n\n<path class=\"st0\" d=\"M434.9,449.5c-2.7-2.7-7.2-2.7-9.9,0c-2.7,2.7-2.7,7.2,0,9.9l21,21c1.4,1.4,3.2,2.1,5,2.1c1.8,0,3.6-0.7,4.9-2   c2.7-2.7,2.7-7.2,0-9.9L434.9,449.5z\"/>\n\n</g>\n\n</svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Is rainy</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += isRainy ? "+" : "-";
  ptr += "</div>";
  ptr += "</div>";

  ptr += "</div>";
  ptr += "</body>";
  ptr += "</html>";
  return ptr;
}