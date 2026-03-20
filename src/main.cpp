#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>

// ─── CONFIGURATION ────────────────────────────────────────────────────────────

// Broches I2C de l'ESP32-S3-DevKitM-1
#define I2C_SDA 4
#define I2C_SCL 0

// Nombre de capteurs
#define SENSOR_COUNT 4

// Broches XSHUT pour chaque capteur
const uint8_t XSHUT_PINS[SENSOR_COUNT] = {5, 6, 7, 8};

// Adresses I2C uniques à assigner (différentes de 0x29)
const uint8_t SENSOR_ADDRESSES[SENSOR_COUNT] = {0x30, 0x31, 0x32, 0x33};

// Noms affichés dans le moniteur série
const char* SENSOR_NAMES[SENSOR_COUNT] = {"Capteur 1", "Capteur 2", "Capteur 3", "Capteur 4"};

// Timeout de lecture en ms (0 = désactivé)
#define SENSOR_TIMEOUT_MS 500

// ─── OBJETS CAPTEURS ──────────────────────────────────────────────────────────

VL53L0X sensors[SENSOR_COUNT];

// ─── FONCTIONS ────────────────────────────────────────────────────────────────

/**
 * Éteint tous les capteurs via XSHUT (LOW = OFF)
 */
void disableAllSensors() {
    for (int i = 0; i < SENSOR_COUNT; i++) {
        pinMode(XSHUT_PINS[i], OUTPUT);
        digitalWrite(XSHUT_PINS[i], LOW);
    }
    delay(10);
}

/**
 * Initialise les capteurs un par un en leur assignant une adresse unique.
 * Retourne true si tous les capteurs sont initialisés avec succès.
 */
bool initAllSensors() {
    disableAllSensors();

    for (int i = 0; i < SENSOR_COUNT; i++) {
        Serial.printf("Initialisation de %s...\n", SENSOR_NAMES[i]);

        // Allume uniquement ce capteur
        digitalWrite(XSHUT_PINS[i], HIGH);
        delay(10);  // Attendre le démarrage du capteur

        // Configure le bus I2C et le timeout
        sensors[i].setBus(&Wire);
        sensors[i].setTimeout(SENSOR_TIMEOUT_MS);

        // Initialise le capteur (encore à l'adresse par défaut 0x29)
        if (!sensors[i].init()) {
            Serial.printf("  X ERREUR : Impossible d'initialiser %s !\n", SENSOR_NAMES[i]);
            Serial.println("    Vérifiez le câblage et la broche XSHUT.");
            return false;
        }

        // Assigne une adresse unique
        sensors[i].setAddress(SENSOR_ADDRESSES[i]);
        Serial.printf("  ✓ Adresse I2C assignée : 0x%02X\n", SENSOR_ADDRESSES[i]);

        // Mode continu pour des mesures rapides
        sensors[i].startContinuous(50);  // Une mesure toutes les 50 ms
    }

    Serial.println("Tous les capteurs sont initialisés !");
    return true;
}

// ─── SETUP ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Multi VL53L0X sur ESP32-S3 ===\n");

    // Démarre le bus I2C avec les broches personnalisées
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);  // 400 kHz (Fast Mode)

    // Initialise les capteurs
    if (!initAllSensors()) {
        Serial.println("\n  Arrêt du programme. Corrigez les erreurs ci-dessus.");
        while (1) { delay(1000); }
    }

    Serial.println("\nDémarrage des mesures...\n");
    Serial.println("--------------------------------------------");
    Serial.printf("%-12s | %-10s\n", "Capteur", "Distance (mm)");
    Serial.println("--------------------------------------------");
}

// ─── LOOP ─────────────────────────────────────────────────────────────────────

void loop() {
    for (int i = 0; i < SENSOR_COUNT; i++) {
        uint16_t distance = sensors[i].readRangeContinuousMillimeters();

        if (sensors[i].timeoutOccurred()) {
            Serial.printf("%-12s | TIMEOUT\n", SENSOR_NAMES[i]);
        } else if (distance >= 8190) {
            // 8190 mm = hors portée (valeur renvoyée quand rien n'est détecté)
            Serial.printf("%-12s | Hors portée\n", SENSOR_NAMES[i]);
        } else if (distance <= 49) {
            // 50 mm = distance minimale (valeur renvoyée quand l'objet est trop proche)
            Serial.printf("%-12s | Trop proche\n", SENSOR_NAMES[i]);
        } else {
            Serial.printf("%-12s | %d mm\n", SENSOR_NAMES[i], distance);
        }
    }

    Serial.println("--------------------------------------------");
    delay(250);
}