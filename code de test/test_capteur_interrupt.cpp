/**
 * =============================================================
 *  Multi VL53L0X sur ESP32-S3-DevKitM-1
 *  Alerte déclenchée UNIQUEMENT par le Capteur 4
 * =============================================================
 *
 *  CÂBLAGE (4 capteurs) :
 *
 *  ESP32-S3          VL53L0X #1    VL53L0X #2    VL53L0X #3    VL53L0X #4
 *  ---------         ----------    ----------    ----------    ----------
 *  3V3         ----> VIN           VIN           VIN           VIN
 *  GND         ----> GND           GND           GND           GND
 *  GPIO8 (SDA) ----> SDA           SDA           SDA           SDA  (bus partagé)
 *  GPIO9 (SCL) ----> SCL           SCL           SCL           SCL  (bus partagé)
 *  GPIO4       ----> XSHUT
 *  GPIO5       ---->               XSHUT
 *  GPIO6       ---->                             XSHUT
 *  GPIO7       ---->                                           XSHUT
 *  GPIO10      ----> LED / Buzzer / Relais (sortie alarme)
 *
 *  PRINCIPE :
 *  - Les 4 capteurs sont lus en continu (affichage).
 *  - SEUL le Capteur 4 (index 3) peut déclencher l'alerte.
 *  - Quand sa distance dépasse ALERT_THRESHOLD_MM, onDistanceAlert() est appelée.
 *  - Un cooldown évite les déclenchements répétés en rafale.
 * =============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>

// ─── CONFIGURATION ────────────────────────────────────────────────────────────

#define I2C_SDA  4
#define I2C_SCL  0

#define SENSOR_COUNT 4

// Broches XSHUT — une par capteur
const uint8_t XSHUT_PINS[SENSOR_COUNT]       = { 4,     5,     6,     7     };

// Adresses I2C uniques (≠ 0x29)
const uint8_t SENSOR_ADDRESSES[SENSOR_COUNT] = { 0x30,  0x31,  0x32,  0x33  };

// Noms affichés dans la console
const char* SENSOR_NAMES[SENSOR_COUNT]       = { "Capteur 1", "Capteur 2",
                                                  "Capteur 3", "Capteur 4" };

// ── Index du capteur qui déclenche l'alerte ───────────────────────────────────
//    0 = Capteur 1, 1 = Capteur 2, 2 = Capteur 3, 3 = Capteur 4
#define ALERT_SENSOR_INDEX  3   // ← Capteur 4

// ── Seuil de distance pour le capteur d'alerte (mm) ──────────────────────────
//    L'interruption se déclenche quand distance > seuil
#define ALERT_THRESHOLD_MM  300

// ── Broche de sortie physique (LED, buzzer, relais) ───────────────────────────
//    Mettre -1 pour désactiver
#define ALARM_PIN  10

// ── Anti-rebond : délai minimum entre 2 alertes consécutives (ms) ─────────────
#define ALERT_COOLDOWN_MS  1000

// ── Timeout lecture I2C (ms) ──────────────────────────────────────────────────
#define SENSOR_TIMEOUT_MS  500

// ─── VARIABLES GLOBALES ───────────────────────────────────────────────────────

VL53L0X sensors[SENSOR_COUNT];

// Drapeau d'alerte — levé par sensorTask (Core 0), consommé dans loop() (Core 1)
volatile bool     alertFlag         = false;
volatile uint16_t alertDistanceVal  = 0;
volatile uint32_t lastAlertTime     = 0;

// Mutex pour sécuriser le bus I2C partagé entre les deux cores
SemaphoreHandle_t i2cMutex;

// ─── CALLBACK D'ALERTE ────────────────────────────────────────────────────────

/**
 * Appelée dans loop() quand le Capteur 4 dépasse ALERT_THRESHOLD_MM.
 * Ajoutez ici vos actions : MQTT, HTTP, GPIO, relais, etc.
 */
void onDistanceAlert(uint16_t distance) {
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════╗");
    Serial.println("║  🚨  INTERRUPTION — Capteur 4               ║");
    Serial.printf( "║  Distance mesurée : %5d mm               \n", distance);
    Serial.printf( "║  Seuil configuré  : %5d mm               \n", ALERT_THRESHOLD_MM);
    Serial.printf( "║  Dépassement      : +%d mm               \n", distance - ALERT_THRESHOLD_MM);
    Serial.println("╚══════════════════════════════════════════════╝\n");

    // ── Sortie physique (LED / buzzer / relais) ───────────────────────────────
#if ALARM_PIN >= 0
    digitalWrite(ALARM_PIN, HIGH);
    delay(300);
    digitalWrite(ALARM_PIN, LOW);
#endif

    // ── Vos actions personnalisées ────────────────────────────────────────────
    // mqttClient.publish("alerte/capteur4", String(distance).c_str());
    // http.begin("http://mon-serveur/alerte?dist=" + String(distance));
    // digitalWrite(RELAY_PIN, HIGH);
}

// ─── INITIALISATION DES CAPTEURS ──────────────────────────────────────────────

void disableAllSensors() {
    for (int i = 0; i < SENSOR_COUNT; i++) {
        pinMode(XSHUT_PINS[i], OUTPUT);
        digitalWrite(XSHUT_PINS[i], LOW);
    }
    delay(10);
}

bool initAllSensors() {
    disableAllSensors();

    for (int i = 0; i < SENSOR_COUNT; i++) {
        const char* role = (i == ALERT_SENSOR_INDEX) ? " [ALERTE]" : "";
        Serial.printf("Init %s%s... ", SENSOR_NAMES[i], role);

        // Allume ce capteur seul (il répond à 0x29 par défaut)
        digitalWrite(XSHUT_PINS[i], HIGH);
        delay(10);

        sensors[i].setBus(&Wire);
        sensors[i].setTimeout(SENSOR_TIMEOUT_MS);

        if (!sensors[i].init()) {
            Serial.println("✗ ECHEC ! Vérifiez le câblage.");
            return false;
        }

        // Assigne son adresse unique jusqu'au prochain reset
        sensors[i].setAddress(SENSOR_ADDRESSES[i]);
        sensors[i].startContinuous(50);   // mesure toutes les 50 ms

        Serial.printf("✓ → 0x%02X\n", SENSOR_ADDRESSES[i]);
    }
    return true;
}

// ─── TÂCHE FREEERTOS — LECTURE CAPTEURS SUR CORE 0 ───────────────────────────

/**
 * Lit tous les capteurs en continu.
 * Surveille UNIQUEMENT le Capteur 4 pour l'alerte.
 */
void sensorTask(void* pvParameters) {
    for (;;) {
        for (int i = 0; i < SENSOR_COUNT; i++) {

            if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint16_t dist    = sensors[i].readRangeContinuousMillimeters();
                bool     timeout = sensors[i].timeoutOccurred();
                xSemaphoreGive(i2cMutex);

                // ── Alerte : Capteur 4 uniquement ─────────────────────────────
                if (i == ALERT_SENSOR_INDEX && !timeout && dist < 8190) {
                    uint32_t now = millis();
                    if (dist > ALERT_THRESHOLD_MM &&
                        (now - lastAlertTime) > ALERT_COOLDOWN_MS)
                    {
                        lastAlertTime    = now;
                        alertDistanceVal = dist;
                        alertFlag        = true;   // ← traité dans loop()
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── SETUP ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Multi VL53L0X — Alerte Capteur 4 — ESP32-S3 ===\n");

#if ALARM_PIN >= 0
    pinMode(ALARM_PIN, OUTPUT);
    digitalWrite(ALARM_PIN, LOW);
#endif

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);   // I2C Fast Mode 400 kHz

    i2cMutex = xSemaphoreCreateMutex();

    if (!initAllSensors()) {
        Serial.println("\n⚠️  Erreur d'initialisation. Programme arrêté.");
        for (;;) delay(1000);
    }

    Serial.printf("\nSeuil d'alerte (Capteur 4) : %d mm\n", ALERT_THRESHOLD_MM);

    // Lance la tâche de lecture sur le Core 0
    xTaskCreatePinnedToCore(
        sensorTask,    // fonction
        "SensorTask",  // nom debug
        4096,          // pile (octets)
        NULL,          // paramètre
        2,             // priorité
        NULL,          // handle
        0              // Core 0
    );

    Serial.println("\nSurveillance active.\n");
    Serial.println("────────────────────────────────────────────────────────────");
    Serial.printf("%-12s │ %-14s │ Statut\n", "Capteur", "Distance (mm)");
    Serial.println("────────────────────────────────────────────────────────────");
}

// ─── LOOP — ALERTE + AFFICHAGE (Core 1) ──────────────────────────────────────

void loop() {
    // 1) Traite le drapeau levé par sensorTask ────────────────────────────────
    if (alertFlag) {
        alertFlag = false;
        onDistanceAlert(alertDistanceVal);
    }

    // 2) Affichage de l'état de tous les capteurs ─────────────────────────────
    for (int i = 0; i < SENSOR_COUNT; i++) {
        uint16_t dist  = 0;
        bool     tiout = false;

        if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            dist  = sensors[i].readRangeContinuousMillimeters();
            tiout = sensors[i].timeoutOccurred();
            xSemaphoreGive(i2cMutex);
        }

        // Indicateur visuel pour le capteur d'alerte
        const char* tag = (i == ALERT_SENSOR_INDEX) ? " 🔔" : "   ";

        if (tiout) {
            Serial.printf("%-12s%s │ %-14s │ TIMEOUT\n",
                          SENSOR_NAMES[i], tag, "---");
        } else if (dist >= 8190) {
            Serial.printf("%-12s%s │ %-14s │ OK\n",
                          SENSOR_NAMES[i], tag, "Hors portee");
        } else {
            // Statut spécial pour le capteur 4
            const char* statut = "OK";
            if (i == ALERT_SENSOR_INDEX && dist > ALERT_THRESHOLD_MM) {
                statut = "⚠ DEPASSE";
            }
            Serial.printf("%-12s%s │ %-14d │ %s\n",
                          SENSOR_NAMES[i], tag, dist, statut);
        }
    }

    Serial.println("────────────────────────────────────────────────────────────");
    delay(500);
}