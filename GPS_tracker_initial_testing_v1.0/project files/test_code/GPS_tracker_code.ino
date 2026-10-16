
#include <TinyGPSPlus.h>
#include <math.h>

TinyGPSPlus gps;

#define GPS_RX 16
#define GPS_TX 17

#define MODEM_RX 26
#define MODEM_TX 27

HardwareSerial gpsSerial(2);
HardwareSerial modem(1);

const char PHONE_NUMBER[] = "+918500750375";

// GPS quality settings
const int REQUIRED_SAMPLES = 10;
const int MIN_SATELLITES = 6;
const double MAX_HDOP = 2.5;
const unsigned long GPS_TIMEOUT_MS = 180000UL;

// Store accepted GPS samples
double latSamples[REQUIRED_SAMPLES];
double lonSamples[REQUIRED_SAMPLES];
double altSamples[REQUIRED_SAMPLES];

int sampleCount = 0;

String readResponse(unsigned long timeout) {
  String response;
  unsigned long start = millis();

  while (millis() - start < timeout) {
    while (modem.available()) {
      char c = modem.read();
      Serial.write(c);
      response += c;
    }

    if (response.indexOf("\nOK\r") >= 0 ||
        response.indexOf("\nERROR\r") >= 0 ||
        response.indexOf("+CMS ERROR:") >= 0) {
      break;
    }

    delay(1);
  }

  return response;
}

bool sendAT(const String& cmd, const char* expected,
            unsigned long timeout = 5000) {
  while (modem.available()) modem.read();

  Serial.print("\n>> ");
  Serial.println(cmd);

  modem.print(cmd);
  modem.print("\r\n");

  String response = readResponse(timeout);

  return response.indexOf(expected) >= 0 &&
         response.indexOf("ERROR") < 0;
}

bool waitForPrompt(unsigned long timeout) {
  String response;
  unsigned long start = millis();

  while (millis() - start < timeout) {
    while (modem.available()) {
      char c = modem.read();
      Serial.write(c);
      response += c;

      if (c == '>') return true;

      if (response.indexOf("+CMS ERROR:") >= 0 ||
          response.indexOf("\nERROR\r") >= 0) {
        return false;
      }
    }
    delay(1);
  }

  return false;
}

// Sort a copy and return its median
double medianOf(const double* values, int count) {
  double sorted[REQUIRED_SAMPLES];

  for (int i = 0; i < count; i++) {
    sorted[i] = values[i];
  }

  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (sorted[j] < sorted[i]) {
        double temp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = temp;
      }
    }
  }

  if (count % 2 == 1) {
    return sorted[count / 2];
  }

  return (sorted[count / 2 - 1] +
          sorted[count / 2]) / 2.0;
}

bool gpsQualityGood() {
  return gps.location.isValid() &&
         gps.location.age() < 2000 &&
         gps.satellites.isValid() &&
         gps.satellites.value() >= MIN_SATELLITES &&
         gps.hdop.isValid() &&
         gps.hdop.hdop() <= MAX_HDOP &&
         gps.altitude.isValid() &&
         gps.date.isValid() &&
         gps.time.isValid();
}

bool acquireStableGPS() {
  unsigned long start = millis();
  unsigned long lastSample = 0;

  sampleCount = 0;

  Serial.println("\nAcquiring GPS position...");
  Serial.println("Keep the antenna still under open sky.");

  while (millis() - start < GPS_TIMEOUT_MS &&
         sampleCount < REQUIRED_SAMPLES) {

    while (gpsSerial.available()) {
      gps.encode(gpsSerial.read());
    }

    // NEO-6M normally updates at about 1 Hz.
    // Take at most one sample per second.
    if (millis() - lastSample >= 1000) {
      lastSample = millis();

      if (gpsQualityGood()) {
        latSamples[sampleCount] = gps.location.lat();
        lonSamples[sampleCount] = gps.location.lng();
        altSamples[sampleCount] = gps.altitude.meters();

        sampleCount++;

        Serial.print("Accepted sample ");
        Serial.print(sampleCount);
        Serial.print("/");
        Serial.print(REQUIRED_SAMPLES);
        Serial.print(" | Satellites: ");
        Serial.print(gps.satellites.value());
        Serial.print(" | HDOP: ");
        Serial.println(gps.hdop.hdop(), 2);
      } else {
        Serial.print("Waiting for quality fix | Satellites: ");

        if (gps.satellites.isValid())
          Serial.print(gps.satellites.value());
        else
          Serial.print("unknown");

        Serial.print(" | HDOP: ");

        if (gps.hdop.isValid())
          Serial.println(gps.hdop.hdop(), 2);
        else
          Serial.println("unknown");
      }
    }

    delay(5);
  }

  if (sampleCount < REQUIRED_SAMPLES) {
    Serial.println("\nGPS quality timeout. No SMS will be sent.");
    return false;
  }

  return true;
}

bool sendLocationSMS(double lat, double lon, double altitude) {
  String mapsLink =
      "https://www.google.com/maps?q=" +
      String(lat, 6) + "," + String(lon, 6);

  char dateText[16];
  char timeText[16];

  snprintf(dateText, sizeof(dateText), "%02d-%02d-%04d",
           gps.date.day(), gps.date.month(), gps.date.year());

  snprintf(timeText, sizeof(timeText), "%02d:%02d:%02d UTC",
           gps.time.hour(), gps.time.minute(), gps.time.second());

  String message =
      "Vehicle location:\n" + mapsLink +
      "\nDate: " + String(dateText) +
      "\nTime: " + String(timeText) +
      "\nAltitude: " + String(altitude, 1) + " m" +
      "\nSatellites: " + String(gps.satellites.value()) +
      "\nHDOP: " + String(gps.hdop.hdop(), 1);

  Serial.println("\nFinal filtered position:");
  Serial.println(mapsLink);
  Serial.println("Date: " + String(dateText));
  Serial.println("Time: " + String(timeText));
  Serial.print("Altitude: ");
  Serial.print(altitude, 1);
  Serial.println(" m");

  if (!sendAT("AT+CPIN?", "+CPIN: READY")) {
    Serial.println("SIM not ready.");
    return false;
  }

  // Check home or roaming registration
  while (modem.available()) modem.read();

  modem.print("AT+CREG?\r\n");
  String reg = readResponse(5000);

  bool registered =
      reg.indexOf("+CREG: 0,1") >= 0 ||
      reg.indexOf("+CREG: 0,5") >= 0 ||
      reg.indexOf("+CREG: 1,1") >= 0 ||
      reg.indexOf("+CREG: 1,5") >= 0;

  if (!registered) {
    Serial.println("Network registration not confirmed.");
    return false;
  }

  if (!sendAT("AT+CMGF=1", "OK")) {
    Serial.println("Could not enable SMS text mode.");
    return false;
  }

  while (modem.available()) modem.read();

  modem.print("AT+CMGS=\"");
  modem.print(PHONE_NUMBER);
  modem.print("\"\r\n");

  if (!waitForPrompt(10000)) {
    Serial.println("No SMS prompt received.");
    return false;
  }

  modem.print(message);
  modem.write((uint8_t)26); // Ctrl+Z submits SMS

  String response = readResponse(60000);

  if (response.indexOf("+CMGS:") >= 0 &&
      response.indexOf("\nOK\r") >= 0) {
    Serial.println("\nSMS accepted for submission.");
    return true;
  }

  Serial.println("\nSMS failed or timed out.");
  return false;
}

void setup() {
  Serial.begin(115200);

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  modem.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);

  delay(3000);

  Serial.println("\n=== FILTERED GPS LOCATION TRACKER ===");

  if (!sendAT("AT", "OK")) {
    Serial.println("A7670C not responding.");
    return;
  }

  if (!acquireStableGPS()) {
    return;
  }

  // Median rejects occasional outlier samples better than a mean.
  double latitude = medianOf(latSamples, sampleCount);
  double longitude = medianOf(lonSamples, sampleCount);
  double altitude = medianOf(altSamples, sampleCount);

  Serial.println("\n=== GPS ACQUISITION COMPLETE ===");
  Serial.print("Accepted samples: ");
  Serial.println(sampleCount);
  Serial.print("Latitude: ");
  Serial.println(latitude, 6);
  Serial.print("Longitude: ");
  Serial.println(longitude, 6);

  sendLocationSMS(latitude, longitude, altitude);
}

void loop() {
  // One location SMS per startup.
}
