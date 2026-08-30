# Guide d'Installation et de Flashage

Ce guide détaille les étapes nécessaires pour configurer, compiler et flasher le micrologiciel **minimessenger** sur une carte ESP32.

---

## Étape 1 : Configuration des données personnelles

Pour des raisons de sécurité, les informations de connexion WiFi, d'identité de l'appareil et les identifiants du broker MQTT sont stockés dans un fichier privé nommé `personal-data.h`. Ce fichier est ignoré par Git via le fichier `.gitignore`.

1. **Copier le modèle :**
   Dans le dossier `minimessenger/`, copiez le fichier template pour créer votre fichier de configuration :
   ```bash
   cp minimessenger/personal-data.h.template minimessenger/personal-data.h
   ```

2. **Éditer `minimessenger/personal-data.h` :**
   Ouvrez le fichier et configurez les trois sections principales :

   * **Tableau d'identité des appareils (`COMPILED_DEVICE_DATA_ENTRIES`) :**
     Déclarez chaque appareil physique de votre réseau avec sa MAC address, son ID unique (1 à 254), son pseudo et son type d'écran.
     ```cpp
     const DeviceDataEntry COMPILED_DEVICE_DATA_ENTRIES[] = {
         { "AA:BB:CC:DD:EE:01", 1, "Alice", "D1M", ST7789 },
         { "60:01:94:0F:9E:71", 2, "Bob",   "E32", ST7789 },
     };
     ```
     *Note : Si l'adresse MAC de l'appareil n'est pas déclarée, un ID aléatoire et le pseudo "JohnDoe" lui seront attribués au démarrage.*

   * **Réseaux WiFi par défaut (`COMPILED_WIFI_DEFAULTS`) :**
     Ajoutez les réseaux WiFi pré-configurés (jusqu'à 5). Si la liste est vide, l'appareil démarrera directement en mode portail captif pour vous permettre de configurer le WiFi à la volée.
     ```cpp
     const CompiledWifiEntry COMPILED_WIFI_DEFAULTS[] = {
         { "MonWiFiMaison", "motdepasse123" },
     };
     ```

   * **Broker MQTT (`g_mqttServerInfo` et `HIVEMQ_ROOT_CA`) :**
     Renseignez l'adresse de votre serveur MQTT (ex: HiveMQ Cloud), le port (généralement `8883` pour MQTT over TLS), ainsi que l'identifiant et le mot de passe du client.
     
     Si vous utilisez HiveMQ Cloud, collez son certificat racine TLS (généralement *ISRG Root X1* de Let's Encrypt) dans `HIVEMQ_ROOT_CA`. Pour un broker local sans vérification stricte, définissez `.rootCA = nullptr` dans `g_mqttServerInfo` pour désactiver la vérification (l'appareil utilisera alors `setInsecure()`).

---

## Étape 2 : Préparation de l'environnement (Arduino IDE)

Le projet est conçu pour être compilé directement avec l'**Arduino IDE**.

### 1. Installation des cartes (Board Managers)
* **Pour l'ESP32 :**
  Ajoutez l'URL des cartes ESP32 dans vos préférences Arduino IDE, puis installez le paquet `esp32` via le gestionnaire de cartes (recommandé : versions stables de la branche 2.x ou 3.x).
* **Pour l'ESP8266 :**
  Installez le paquet `esp8266` via le gestionnaire de cartes.

### 2. Installation des bibliothèques requises
Ouvrez le **Gestionnaire de bibliothèques** (Library Manager) de l'Arduino IDE et installez les versions exactes suivantes :

* **NimBLE-Arduino** (par *h2zero*) : **v2.5.0**
  * *Crucial pour l'ESP32 :* NimBLE remplace la pile Bluedroid d'origine et permet d'économiser environ 50 Ko de mémoire RAM (Heap). Sans cette économie, la négociation TLS (handshake) avec HiveMQ échouera systématiquement par manque de mémoire contiguë.
* **PubSubClient** (par *Nick O'Leary*) : **v2.8**
  * Gère la communication MQTT.
* **Adafruit ST7735 and ST7789 Library** (par *Adafruit*) : **v1.11.0**
  * Pilote d'affichage pour l'écran TFT ST7789 240x320. Cette bibliothèque installe automatiquement sa dépendance `Adafruit_GFX`.

---

## Étape 3 : Compilation et Flashage

1. **Ouvrir le projet :**
   Lancez l'Arduino IDE et ouvrez le fichier principal `minimessenger/minimessenger.ino`.

2. **Sélection de la cible :**
   Vérifiez que la bonne directive est active tout en haut du fichier `minimessenger.ino` :
   * `#define PAC_ON_ESP32` pour compiler pour un ESP32 (actif par défaut).
   * `#define PAC_ON_D1MINI` pour compiler pour un ESP8266 (D1 mini).

3. **Configuration des paramètres de la carte dans l'IDE :**

   * **Si vous compilez pour ESP32 :**
     * Allez dans **Outils** (Tools) → **Type de carte** (Board) et sélectionnez votre module ESP32 (ex: *ESP32 Dev Module*).
     * **TRÈS IMPORTANT :** Allez dans **Outils** (Tools) → **Partition Scheme** et sélectionnez **"Huge App" (1.9MB No OTA/0.3MB SPIFFS)**. Le schéma de partitionnement standard est trop petit pour l'application avec la pile BLE et TLS.
   
   * **Si vous compilez pour ESP8266 (D1 mini) :**
     * Allez dans **Outils** (Tools) → **Type de carte** (Board) et sélectionnez **"LOLIN(WEMOS) D1 R2 & mini"**.

4. **Flashage :**
   * Connectez votre carte en USB à votre ordinateur.
   * Sélectionnez le bon port série dans **Outils** → **Port**.
   * Ouvrez le **Moniteur Série** (Tools → Serial Monitor) et réglez-le sur **115200 baud**.
   * Cliquez sur le bouton **Téléverser** (Upload / flèche vers la droite) pour compiler et flasher.

Une fois le flashage terminé, l'ESP redémarrera et vous devriez voir les messages de démarrage s'afficher sur l'écran TFT ainsi que sur le Moniteur Série.
