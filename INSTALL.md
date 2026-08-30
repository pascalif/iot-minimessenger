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
         { "AA:BB:CC:DD:EE:01", 1, "Alice", "E32", ST7789 },
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


3. **Configuration du script de dialogue Python (talk.py) :**
   Un client de chat interactif en ligne de commande (en Python) est disponible dans le dossier `scripts/` pour vous permettre de dialoguer directement avec vos appareils physiques depuis votre ordinateur via le broker MQTT.

   * **Créer le fichier de configuration :**
     Copiez le modèle de configuration dans le dossier `scripts/` :
     ```bash
     cp scripts/config.py.template scripts/config.py
     ```

   * **Éditer `scripts/config.py` :**
     Ouvrez ce fichier et configurez les valeurs de connexion au broker MQTT en recopiant **exactement** les mêmes paramètres que ceux saisis dans votre fichier `minimessenger/personal-data.h` (`g_mqttServerInfo`) :
     * `MQTT_HOST` : L'adresse de votre serveur (hôte DNS du broker).
     * `MQTT_PORT` : Généralement `8883` (port MQTT over TLS).
     * `MQTT_USER` : Le nom d'utilisateur de connexion au broker.
     * `MQTT_PASSWORD` : Le mot de passe de connexion.
     * `DEVICE_ID_ME` : Renseignez un ID numérique (ex: `10` ou `99`) non attribué à l'un de vos appareils physiques, pour vous identifier de manière unique lors des échanges de messages.

   * **Lancement du script :**
     Une fois le fichier enregistré, assurez-vous d'avoir la dépendance requise et lancez le client de chat :
     ```bash
     pip install paho-mqtt
     python3 scripts/talk.py
     ```

---

## Étape 2 : Préparation de l'environnement (Arduino IDE)

Le projet est conçu pour être compilé directement avec l'**Arduino IDE**.

### 1. Installation de la carte (Board Manager)
* **Pour l'ESP32 :**
  Ajoutez l'URL des cartes ESP32 dans vos préférences Arduino IDE, puis installez le paquet `esp32` via le gestionnaire de cartes (recommandé : versions stables de la branche 2.x ou 3.x).

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

2. **Configuration des paramètres de la carte dans l'IDE :**
   * Allez dans **Outils** (Tools) → **Type de carte** (Board) et sélectionnez votre module ESP32 (ex: *ESP32 Dev Module*).
   * **TRÈS IMPORTANT :** Allez dans **Outils** (Tools) → **Partition Scheme** et sélectionnez **"Huge App (3MB No OTA / 1MB SPIFFS)"** (ou **"Huge App (1.9MB No OTA/0.3MB SPIFFS)"** selon les paquets de cartes de votre IDE). Le schéma de partitionnement standard est trop petit pour l'application avec la pile BLE et TLS.

3. **Flashage :**
   * Connectez votre carte en USB à votre ordinateur.
   * Sélectionnez le bon port série dans **Outils** → **Port**.
   * Ouvrez le **Moniteur Série** (Tools → Serial Monitor) et réglez-le sur **115200 baud**.
   * Cliquez sur le bouton **Téléverser** (Upload / flèche vers la droite) pour compiler et flasher.

Une fois le flashage terminé, l'ESP redémarrera et vous devriez voir les messages de démarrage s'afficher sur l'écran TFT ainsi que sur le Moniteur Série.
