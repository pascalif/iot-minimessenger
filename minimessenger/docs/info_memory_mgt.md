# Info — Gestion mémoire sur ESP32

> Vue d'ensemble des types de stockage que minimessenger utilise sur ESP32 :
> flash, DRAM (`.bss` / `.data` / heap / stack), IRAM, NVS, eFuse. Ce qui
> est figé par le silicon, ce qui se règle au build, ce qui se choisit au
> moment d'écrire le code. Inclut un exemple concret avec le buffer
> `g_mqttOutgoingMsg`.

## TL;DR

- **`static char buf[500]` dans une fonction n'est PAS sur le heap** — il
  vit dans `.bss`, exactement comme une global. C'est la confusion #1.
- Les buffers volumineux (≥ 100 B) sont en global / static (`.bss`) dans ce
  projet pour préserver la stack et le heap. Les petits (≤ 64 B) sont sur la
  stack par défaut.
- Le **heap** est dynamique mais fragmente avec le temps — critique pour
  TLS qui demande ~38 KB contigus. C'est pourquoi le code évite les
  String temporaires dans les chemins fréquents.
- La **stack** par tâche est limitée (8 KB pour `loopTask` par défaut).
  Mettre un `char buf[500]` dans une fonction appelée depuis MQTT/BLE/TLS
  ronge cette marge.
- Le partitionnement flash est figé par le **partition scheme** choisi
  dans l'IDE Arduino — ce projet utilise impérativement **"Huge App"**
  car le binaire dépasse la partition `default`.

## Hiérarchie physique du stockage

```
┌───────────────────────────────────────────────────────────────────┐
│  ESP32 — module typique 4 MB flash, ~520 KB SRAM, 8 KB RTC slow   │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  Flash externe (SPI, 4 MB sur la plupart des modules)             │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │ Bootloader        ~28 KB                                   │   │
│  │ Partition table   3 KB                                     │   │
│  │ NVS               24 KB    ← WiFi creds, BLE bonds         │   │
│  │ App (Huge)        ~3 MB    ← .text + .rodata + .data init  │   │
│  │ SPIFFS            ~960 KB  ← non utilisé par ce projet     │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  SRAM interne (~520 KB total, ~320 KB DRAM utilisable)            │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │ DRAM (~320 KB partagés)                                    │   │
│  │   .data       ← globals initialisées (copiées de flash)    │   │
│  │   .bss        ← globals zéro-initialisées + static locals  │   │
│  │   Heap        ← malloc, new, String, mbedtls TLS buffers   │   │
│  │   Stack ×N    ← une par task FreeRTOS (loopTask 8 KB)      │   │
│  │ IRAM (~128 KB)                                             │   │
│  │   .iram       ← code marqué IRAM_ATTR (ISR, time-critical) │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  RTC slow memory (8 KB)                                           │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │ Variables RTC_DATA_ATTR — survivent au deep sleep          │   │
│  │ Non utilisé par minimessenger.                             │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  eFuse (1024 bits OTP)                                            │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │ MAC factory, secure boot keys, flash encryption — fixé une │   │
│  │ fois pour toutes à la fab. Lecture seule à l'usage.        │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

### État du build actuel (snapshot)

Au moment où ce doc est écrit, la compilation du sketch sous Arduino IDE
avec partition scheme **"Huge App"** rapporte :

```
Sketch uses          1 631 933 bytes  (51%) of program storage space.   Maximum is 3 145 728 bytes.
Global variables use    64 384 bytes  (19%) of dynamic memory, leaving  263 296 bytes for local variables.
                                                                       Maximum is 327 680 bytes.
```

Décomposition :

- **Sketch (program storage)** = `.text + .rodata + .data initiale`,
  flashé dans la partition app. 1 631 933 B ≈ **1.56 MB** sur 3 MB
  alloués par "Huge App" → 51 % occupé, 1.44 MB libres.
- **Global variables** = `.bss + .data` en DRAM. 64 384 B ≈ **63 KB**
  sur 320 KB de DRAM utilisable → 19 % occupé, 263 KB qu'on peut se
  partager entre heap + stacks de toutes les tasks. Le report Arduino
  appelle ce dernier chiffre "local variables" mais c'est trompeur :
  c'est la **somme** heap + stacks de toutes les tâches FreeRTOS, dont
  les stacks WiFi (~4 KB), tcpip lwIP (~3 KB), NimBLE (~5 KB), etc.,
  s'allouent au boot. Ce qui reste pour notre heap utilisable
  approche plutôt les ~180 KB une fois tout démarré (cf. la mesure
  runtime via `/dbg mem` qui rapporte le `getFreeHeap` réel).

### Conséquence — pourquoi l'OTA n'est pas possible aujourd'hui

Avec le binaire actuel à **1.56 MB**, l'OTA "classique" (deux
partitions app pour basculer entre l'image courante et la nouvelle)
n'est **pas faisable avec le partition scheme par défaut Arduino**, qui
alloue 2× **1.25 MB** pour OTA_0 / OTA_1 (la valeur exacte est
1 310 720 B). 1.56 MB ne tient pas dans 1.25 MB → confirmation que ton
intuition est juste.

**Mais ce n'est pas un mur définitif** :

- Le partition scheme **"Minimal SPIFFS (1.9MB APP with OTA / 190KB
  SPIFFS)"** alloue 2× **1.9 MB** pour les partitions OTA. Le binaire
  de 1.56 MB y tient à l'aise (16 % de marge). Comme ce projet
  n'utilise pas SPIFFS, perdre 1.5 MB → 190 KB de SPIFFS est gratuit.
- Un `partitions.csv` custom (à côté du sketch) permet d'aller au
  millimètre — par exemple deux partitions OTA de 1.7 MB chacune, plus
  ce qu'on veut en NVS / autre.
- Le partition scheme **"Huge App"** actuel ne permet PAS d'OTA par
  construction : il y a une seule partition app de 3 MB (pas de
  binaire B vers lequel basculer). Bien adapté tant qu'on accepte de
  flasher via USB pour chaque mise à jour.

Donc : **OTA possible en passant à "Minimal SPIFFS"** (zéro modif code,
juste un changement dans `Tools → Partition Scheme`), au prix
d'abandonner SPIFFS (déjà inutilisé). Tant que le binaire reste sous
~1.85 MB ça tient sans casse-tête.

## Flash externe — où va le code et les constantes

La flash de 4 MB est divisée en **partitions** définies par un fichier CSV
(le *partition table*) flashé séparément. Le partition scheme choisi dans
Arduino IDE (`Tools → Partition Scheme`) sélectionne ce CSV parmi des
presets bundlés. Ce projet utilise **"Huge App (3 MB no OTA / 1 MB
SPIFFS)"** parce que le binaire compilé (WiFi + BLE NimBLE + Adafruit_GFX +
PubSubClient + mbedTLS + sketch) dépasse la partition `default` de 1.3 MB.

### Ce qu'on trouve dans la partition app

- **`.text`** : code exécutable. Pas chargé en RAM ; le CPU exécute
  directement depuis la flash via le cache MMU (typiquement transparent).
- **`.rodata`** : données read-only. Chaînes littérales (`"hello"`),
  tableaux `const`, lookup tables (`keymapLower[]`, `keymapUpper[]` dans
  `hid_keys.h`). Mappées en lecture via le même cache.
- **`.data` (image)** : valeur initiale des globals initialisées.
  Copiées en DRAM au boot par le startup code (`startup_app_main`).

L'image flashée par `esptool.py` contient ces trois sections concaténées
avec des headers de segment. La partition table indique où elle commence
dans la flash.

### Partitions configurables au build

Le partition scheme change tout :

| Preset (Arduino IDE)        | App     | OTA      | NVS    | SPIFFS / FFat |
|-----------------------------|---------|----------|--------|---------------|
| Default                     | 1.3 MB  | 2× 1.3 MB | 16 KB  | ~1.5 MB       |
| No OTA (Large APP)          | 2 MB    | aucun    | 16 KB  | ~1.9 MB       |
| **Huge App** (utilisé ici)  | 3 MB    | aucun    | 24 KB  | 960 KB        |
| Minimal SPIFFS              | 1.9 MB  | 1.9 MB   | 16 KB  | 192 KB        |

Pour changer ces valeurs au-delà des presets, il faut fournir un
`partitions.csv` custom (placé à côté du sketch) — voir section
suivante.

### `partitions.csv` — verrouiller le layout dans le repo

Pour qu'un clone du projet ait une garantie sur le layout flash sans
dépendre du choix manuel dans `Tools → Partition Scheme`, on peut
poser un fichier `partitions.csv` à côté du `.ino` principal.
arduino-esp32 le ramasse automatiquement quand on sélectionne
`PartitionScheme=custom` dans le menu Tools (ou dans le FQBN
arduino-cli). Le fichier est versionné dans git → reproductibilité.

#### Format

Une ligne par partition, séparateurs virgule, commentaires `#`. Cinq
colonnes obligatoires plus une optionnelle :

```
# Name,    Type, SubType,  Offset,    Size,     Flags
```

- **Name** : étiquette interne (apparaît dans les logs ESP-IDF, sans
  contrainte d'unicité côté firmware mais pratique unique).
- **Type** : `app` (partition contenant un firmware exécutable) ou
  `data` (tout le reste : NVS, SPIFFS, OTA metadata, coredump…).
- **SubType** : précise le rôle.
  - Pour `app` : `factory` (slot unique, jamais OTA), `ota_0` /
    `ota_1` / … / `ota_15` (slots OTA), `test`.
  - Pour `data` : `nvs`, `ota` (le 8 KB qui dit quel `ota_X` booter),
    `spiffs`, `littlefs`, `fat`, `coredump`, `phy`, `nvs_keys`,
    `efuse`.
- **Offset** : adresse de début dans la flash (hex). Doit être aligné
  sur 0x10000 (64 KB) pour les partitions `app`, sur 0x1000 (4 KB)
  pour les `data`. Peut être laissé vide → calculé automatiquement.
- **Size** : taille (hex). Pour `app`, multiple de 0x10000.
- **Flags** (optionnel) : `encrypted` si flash encryption activé. La
  plupart du temps laissé vide.

Le total doit tenir dans la taille de la flash (4 MB = 0x400000 sur
les modules ESP32 typiques).

#### Exemple 1 — layout actuel (Huge App, pas d'OTA)

```csv
# Name,     Type, SubType,  Offset,    Size,     Flags
nvs,        data, nvs,      0x9000,    0x5000,
otadata,    data, ota,      0xe000,    0x2000,
app0,       app,  ota_0,    0x10000,   0x300000,
spiffs,     data, spiffs,   0x310000,  0xE0000,
coredump,   data, coredump, 0x3F0000,  0x10000,
```

Bilan : 20 KB NVS + 8 KB otadata + **3 MB app** + 896 KB spiffs +
64 KB coredump = 4 MB. Un seul slot app → pas d'OTA possible (rien
vers quoi basculer), mais 3 MB pour le binaire (qui consomme ~1.56 MB
aujourd'hui = 51 % occupé).

Curiosité : la sous-partition s'appelle `ota_0` malgré l'absence
d'OTA. C'est juste la convention arduino-esp32 ; le bootloader IDF
sait booter un seul `ota_0` sans `ota_1` (il regarde `otadata`,
trouve "boot ota_0", boote).

#### Exemple 2 — double OTA sans SPIFFS

Pour activer l'OTA tout en laissant assez de place pour notre binaire
de 1.56 MB **et** se débarrasser du SPIFFS qu'on n'utilise pas :

```csv
# Name,     Type, SubType,  Offset,    Size,     Flags
nvs,        data, nvs,      0x9000,    0x5000,
otadata,    data, ota,      0xe000,    0x2000,
app0,       app,  ota_0,    0x10000,   0x1F0000,
app1,       app,  ota_1,    0x200000,  0x1F0000,
coredump,   data, coredump, 0x3F0000,  0x10000,
```

Bilan : 20 KB NVS + 8 KB otadata + 2× **1.94 MB** app + 64 KB
coredump = 4 MB. 0x1F0000 = 2 031 616 B par slot → ~470 KB de
headroom au-delà du binaire actuel (1.94 MB − 1.56 MB). Aucun SPIFFS,
mais ce projet ne s'en sert pas.

Le bootloader basculera entre app0 et app1 à chaque OTA réussie ; en
cas d'OTA boguée, il sait rollback vers l'autre slot (mécanique gérée
par ESP-IDF + l'API `Update` côté Arduino).

#### Comment activer un `partitions.csv` custom

Trois voies :

1. **Arduino IDE GUI** : `Tools → Partition Scheme → Custom`. À ce
   moment-là, arduino-esp32 utilise le `partitions.csv` du dossier
   sketch. Inconvénient : facile à oublier après un changement de
   préférence — d'où l'intérêt du `sketch.yaml` ci-dessous.
2. **arduino-cli + sketch.yaml** : le fichier `sketch.yaml`
   (versionné dans le repo) fixe le FQBN à
   `esp32:esp32:esp32:PartitionScheme=custom,...`. Aucune action
   manuelle côté contributeur, le build est reproductible.
3. **PlatformIO** : variable `board_build.partitions =
   partitions.csv` dans `platformio.ini`. Hors scope d'un projet
   Arduino IDE classique.

## DRAM — où vivent les variables au runtime

Le `.bss` et le `.data` sont posés en début de DRAM. Le reste est partagé
entre **heap** (qui grandit depuis une extrémité) et **stack** (chaque
tâche a la sienne, posée à des adresses fixes par FreeRTOS au moment du
`xTaskCreate`).

### `.bss` — globals zero-init + static locals

```cpp
// minimessenger.ino — exemples
PubSubClient  g_mqttClient(g_wifiClient);          // → .bss (objet)
char          g_mqttOutgoingMsg[MSG_BUFFER_SIZE];  // → .bss, 500 octets
unsigned int  g_mqttOutputMsgNextId = 0;           // → .bss
```

Toutes ces lignes réservent leur place **à la compilation**. Le binaire
ne contient PAS les 500 octets de `g_mqttOutgoingMsg` ; il contient juste
l'instruction "réserve 500 octets en `.bss` au boot et zéro-initialise".

Le `static` dans une fonction stocke **au même endroit** :

```cpp
void foo() {
    static char myBuf[500];   // ← AUSSI en .bss, pas sur le heap !
    char        stackBuf[16]; // ← sur la stack de la task courante
}
```

La seule différence entre `static char buf` dans une fonction et un
global, c'est le *scope* (qui peut le voir). La mémoire occupée et le
moment d'allocation sont identiques.

### `.data` — globals initialisées

```cpp
const char* mqtt_server = "xxxxxx.s1.eu.hivemq.cloud";
// → la chaîne littérale est en .rodata (flash)
// → le pointeur lui-même est en .data (DRAM), 4 octets, initialisé à
//   l'adresse flash de la chaîne au boot
```

Quand le programme démarre, le startup code copie `.data` depuis la flash
vers la DRAM. Coût : flash size (initial value) + DRAM size (writable
copy).

### Vases communicants : `.bss` ↔ heap

Le linker pose `.data` et `.bss` en début de DRAM avec une **taille
figée au build**. Le heap démarre juste après et grandit dynamiquement
vers les adresses hautes :

```
adresse basse                                              adresse haute
┌────────────┬────────────┬────────────────────────────────────────────┐
│   .data    │    .bss    │              heap (le reste)               │
│ (taille    │ (taille    │  ← se remplit par malloc() et par les      │
│  fixe ⟸   │  fixe ⟸   │     stacks des tasks créées dynamiquement   │
│  build)    │  build)    │     (xTaskCreate)                          │
└────────────┴────────────┴────────────────────────────────────────────┘
                          ↑
                       _heap_start (déterminé au link, fonction du
                                    cumul .data + .bss)
```

**Chaque octet ajouté en `.bss` est un octet de moins dans le heap.**
Ce n'est pas une métaphore : `ESP.getHeapSize()` au runtime vaut
`DRAM_totale − .bss − .data − runtime_ESP_IDF − stacks_initiales`.
Ajouter une global de 20 KB fait littéralement chuter le heap dispo de
20 KB au boot.

Sur un build typique de minimessenger :

```
Global variables use ~60 KB of dynamic memory.    (build report)
ESP.getFreeHeap()      ~150–170 KB                (runtime, post-WiFi/BLE)
ESP.getMaxAllocHeap()  ~90–110 KB                 (plus gros bloc contigu)
```

Le `MaxAllocHeap` est le plus critique : mbedTLS demande ~38 KB
**contigus** pour la handshake. Si on rajoutait un buffer global de
20 KB sans réfléchir, on resterait largement au-dessus de zéro free
heap, mais le `MaxAllocHeap` pourrait descendre sous les 38 KB et
**MQTT casserait** silencieusement au prochain reconnect (rc=-2).
D'où le pré-check `MQTT_TLS_MIN_FREE_HEAP_B` dans
`mqttReconnectAttempt()`.

#### Ce qui ne joue PAS dans l'équation

Le code (`.text`) et les constantes en flash (`.rodata` — par exemple
le cert TLS `g_hiveMQRootCA`, ~1880 octets) **ne touchent pas au
heap** : ces sections vivent en flash, accédées via cache MMU. Tu peux
empiler des `const char[]` énormes, le seul coût est en taille de la
partition app (auditée par le `Sketch uses X bytes` du build report).
La DRAM est intacte.

#### Tradeoff design pratique

**`.bss` (global / static)**

- ✅ Pas de fragmentation, prévisible.
- ✅ Auditable au build report (`Global variables use ...`).
- ❌ Empiète directement sur le heap dispo (et sur le `MaxAllocHeap`).
- ❌ Permanent, même quand le buffer n'est pas utilisé.

**Stack ou heap dynamique**

- ✅ RAM non figée — disponible pour le heap quand le buffer ne sert pas.
- ✅ Préserve le `MaxAllocHeap` critique pour mbedTLS.
- ❌ Allocation runtime + risque de fragmentation (heap) ou stack overflow (stack).
- ❌ Coût runtime (allocateur, copies) en plus de la place mémoire.

C'est pourquoi la règle informelle du projet ("≥ 100 octets → global,
< 100 octets → stack") n'est pas naïve : pour les gros buffers, tu
paies soit en heap (global = `.bss` qui réduit le pool), soit en
risque runtime (stack overflow / fragmentation). Le bon choix dépend
de la fréquence d'usage :

- Utilisé en permanence (chaque message MQTT) → global ; aucun gain à
  le rendre éphémère.
- Utilisé une seule fois au boot puis jamais → potentiellement
  candidat à libérer après usage, mais sur 320 KB de DRAM c'est
  rarement le hot path.
- Buffer gigantesque (framebuffer 240×320×2 = 153 KB, par exemple) →
  ne PEUT pas être global sans réduire le heap à zéro ; il faut une
  stratégie spécifique (allocation unique contrôlée, ou skip le
  double-buffer).

### Heap — allocation dynamique

Tout ce qui passe par `malloc()`, `new`, `calloc()`, ou indirectement
par les classes qui font ça en interne :

- `String("hello")` → buffer de chars `malloc`-é (pas de Small String
  Optimization dans Arduino).
- `new MyClass()` → bloc heap.
- mbedTLS pendant la handshake → ~38 KB temporaires.
- Wi-Fi / BLE drivers → blocs internes (de l'ordre de 50 KB cumulés).

Le heap ESP-IDF est segmenté en plusieurs sous-heaps (DRAM, IRAM,
PSRAM si présent). L'allocation par défaut va dans le DRAM heap.

Visible dans le projet via :
- `ESP.getFreeHeap()` — total libre.
- `ESP.getMaxAllocHeap()` — plus gros bloc contigu libre (le critique
  pour TLS).
- `ESP.getMinFreeHeap()` — watermark le plus bas depuis le boot.

**Risque** : la fragmentation. Beaucoup de petites allocs/free, surtout
entremêlées, finissent par éparpiller le heap → impossible de trouver
38 KB contigus pour TLS, même si 100 KB sont libres au total. C'est
pourquoi `mqttReconnectAttempt()` pré-vérifie
`largest >= MQTT_TLS_MIN_FREE_HEAP_B` avant d'oser un connect.

### Stack — une par tâche FreeRTOS

Chaque tâche créée via `xTaskCreate(..., stackSize, ...)` reçoit une
stack distincte allouée sur le heap au moment de sa création. Une fois
allouée, c'est une zone fixe gérée par le compilateur (push/pop des
frames de fonction).

Tasks notables dans ce projet :

| Task          | Stack par défaut | Source                                   |
|---------------|------------------|------------------------------------------|
| `loopTask`    | 8 KB             | Arduino core ; contient setup() + loop() |
| `wifi`        | ~4 KB            | esp_wifi                                 |
| `tcpip`       | ~3 KB            | lwIP                                     |
| `ble_host`    | ~5 KB            | NimBLE                                   |
| Tasks système | divers           | FreeRTOS / IDF                           |

Le `loopTask` est celui qui exécute tout notre code : `setup()`, `loop()`,
tous les callbacks (MQTT, BLE, Serial). Une variable locale `char
buf[500]` dans `mqttPushFormattedMessage` ronge sa marge :

```
loopTask stack 8192 B
  └─ loop()                    : ~200 B   (locals + frame overhead)
     └─ mqttClient.loop()      : ~300 B   (PubSubClient internal)
        └─ onMqttIncomingMsg() : ~600 B   (String message etc.)
           └─ routeMessage()   : ~200 B
              └─ mqttPushFormattedMessage : 500 B + ~100 B
                 └─ mbedtls publish      : ~1500 B
   ─────────────────────────────────
   ≈ 3400 B utilisés au pic → 4800 B de marge
```

C'est pour ça qu'on inspecte `uxTaskGetStackHighWaterMark(NULL)` dans
`/dbg mem` — si la marge descend trop, ajouter un gros `char[]` local
finira par crasher la task (stack overflow → reboot).

## IRAM — code time-critical

Quelques fonctions ESP-IDF (interrupt handlers, certaines parties du
driver SPI haute fréquence) sont marquées `IRAM_ATTR` pour garantir
qu'elles s'exécutent depuis IRAM, sans dépendre du cache flash (qui
peut être indisponible quand on écrit dans la flash, par exemple).

Le projet minimessenger ne déclare aucune fonction `IRAM_ATTR`. L'IRAM
est consommée par le core ESP-IDF, le WiFi driver, le BLE stack. On n'a
pas grand-chose à dire dessus côté sketch.

## NVS — clé-valeur persistant

`Preferences` côté Arduino expose le NVS d'ESP-IDF (key-value store
non-volatile, contenu en flash, mais avec wear leveling et atomicité
garantie par le driver).

Usage dans ce projet :

- **Namespace `"wifi"`** : liste des SSID/passwords ajoutés via le
  portail captif (`/wifi forget`, `/wifi list`, `/wifi portal`).
- **Namespace NimBLE** : bonds BLE persistés par la lib (lié au clavier
  Bluetooth pour ne pas re-pair à chaque boot).

Caractéristiques NVS :

- Taille = celle de la partition NVS dans le partition table (24 KB
  dans "Huge App").
- Lecture rapide (~ms), écriture lente (~10-100 ms) car ça écrit en
  flash.
- API key-value typée : `putString`, `getString`, `putUInt`, etc.
- Atomicité par clé (pas par batch).

**Pas le bon endroit pour** :
- Logs (écriture trop fréquente → use SPIFFS / LittleFS, ou rien).
- Gros blobs structurés (> ~1 KB) — la limite NVS par valeur tape
  vite, et la sérialisation devient pénible.
- Identités device permanentes (préférer hardcoder dans
  `personal-data.h` indexé par MAC → la table compilée est dans
  `.rodata` et n'a pas le coût d'écriture).

## SPIFFS / LittleFS — système de fichiers embarqué (non utilisé ici)

Mentionné en passant dans les sections précédentes : **SPIFFS** (*SPI
Flash File System*) est un mini-FS qui vit dans une partition flash
dédiée et expose une API POSIX-like (`open`, `read`, `write`,
`close`). Sur arduino-esp32 on l'utilise via le global `SPIFFS` (ou
`LittleFS`, son successeur recommandé) :

```cpp
File f = SPIFFS.open("/config.json", "r");
String content = f.readString();
f.close();
```

### À quoi ça sert

C'est le bon endroit pour des données plus volumineuses ou plus
structurées que ce que NVS gère bien :

- **Pages web** pour un serveur HTTP embarqué (HTML/CSS/JS qu'on
  flashe avec l'app et qu'on sert au navigateur).
- **Assets binaires** : images bitmap, échantillons audio TTS, glyphes
  de fontes custom non bakés en `.rodata`.
- **Logs persistants** : append-only de quelques KB par fichier — ce
  pour quoi NVS est mal adapté (écritures à chaque ligne usent la
  flash trop vite, valeurs limitées en taille).
- **Configurations exportables** : JSON / YAML lisibles à l'œil nu,
  remplaçables sans reflasher tout le firmware.

### Pourquoi ce projet ne s'en sert pas

- **Identités peers** → table `COMPILED_DEVICE_DATA_ENTRIES` en dur
  dans `personal-data.h`, gitignored par déploiement, compilée en
  `.rodata`. Zéro écriture flash, lecture instantanée.
- **Credentials WiFi** → NVS namespace `wifi` (faible volume,
  écriture rare via le portail captif, atomicité gratuite).
- **Bonds BLE** → NVS sous le namespace de NimBLE.
- **Logs** → sortis sur le port série, pas persistés.
- **Pages web** → pas de serveur HTTP embarqué.
- **Assets graphiques** → les fontes `FreeSans*_latin1.h` et le logo
  `splash.h` sont des tableaux C `const` → en flash via `.rodata`,
  accédés via cache MMU, pas besoin d'un FS pour ça.

Résultat : on n'a aucun besoin actif → on supprime la partition pour
récupérer de la place pour l'OTA (cf. Exemple 2 du `partitions.csv`).

### SPIFFS vs LittleFS

Si un jour on en a besoin :

| Critère                | SPIFFS                  | LittleFS (recommandé)         |
|------------------------|-------------------------|-------------------------------|
| Sous-dossiers          | Non (flat)              | Oui                           |
| Wear leveling          | Basique                 | Meilleur, moins de power-fail |
| Maturité ESP32         | Long historique         | Plus récent, activement maintenu |
| API arduino-esp32      | `SPIFFS.open(...)`      | `LittleFS.open(...)` (même shape) |

Pour un projet neuf en 2026 on choisirait LittleFS. SPIFFS reste là
pour la compatibilité ascendante.

## eFuse — bits OTP en silicon

1024 bits programmés une seule fois (la plupart à la fab), comprend :

- L'adresse MAC factory (lue par `WiFi.macAddress()` après l'init
  driver, ou par `esp_read_mac()` directement depuis `<esp_mac.h>`).
- Les clés de secure boot et flash encryption (non utilisées ici).
- Quelques flags de configuration silicon (CPU frequency limits,
  etc.).

Read-only à l'usage. Pas une "mémoire" au sens classique : on ne s'en
sert pas pour stocker ses propres données (sauf cas industriels précis).

## Ce qui est hardcoded vs ce qui est configurable

| Couche                         | Exemples                                            | Modifiable ?                                     |
|--------------------------------|-----------------------------------------------------|--------------------------------------------------|
| Silicon                        | Taille SRAM (520 KB), split IRAM/DRAM, eFuse layout | Non                                              |
| Partition table                | Tailles app / NVS / SPIFFS                          | Oui — choix du preset ou `partitions.csv` custom |
| sdkconfig (ESP-IDF)            | Stack default tasks, mbedTLS buffer sizes, FREERTOS_HZ | Oui mais hors sketch Arduino — recompiler le core |
| Arduino Tools menu             | Partition Scheme, Flash Size, CPU Frequency         | Oui, choix dans l'IDE                            |
| Attributs C++                  | `IRAM_ATTR`, `DRAM_ATTR`, `EXT_RAM_ATTR`, `RTC_DATA_ATTR` | Oui, sur chaque variable / fonction        |
| Runtime API                    | `setLoopStackSize()` (avant `setup()`), `heap_caps_malloc()`, `Preferences::putString()` | Oui                                              |

## Exemple concret : `g_mqttOutgoingMsg[500]`

Le buffer scratch utilisé par `mqttPushFormattedMessage()` pour
assembler `<payload> ### ts:... deviceId:... msgId:...` avant le
`publish()`. 500 octets — décision déjà prise dans le projet, mais
revisitons les options pour comprendre :

### Option A — global (état actuel)

```cpp
char g_mqttOutgoingMsg[MSG_BUFFER_SIZE];  // → .bss
```

- 500 octets en `.bss`, alloués au boot, permanents.
- Visibles dans le `Sketch uses X bytes` du build report.
- Aucun coût runtime (pas d'allocation, pas de free).
- Pas de fragmentation possible.

### Option B — `static` local

```cpp
void mqttPushFormattedMessage(...) {
    static char buf[MSG_BUFFER_SIZE];  // → .bss aussi !
}
```

- **Strictement la même conso mémoire que A.** Le `static` ne change
  rien à l'emplacement physique : `.bss`. Il restreint juste la
  visibilité au scope de la fonction.
- Cosmétique pure, pas une "économie".

### Option C — stack local

```cpp
void mqttPushFormattedMessage(...) {
    char buf[MSG_BUFFER_SIZE];  // → stack de loopTask
}
```

- 500 octets sur la stack pendant l'appel.
- **Risqué** : le call stack au moment d'un publish MQTT est déjà
  profond (loop → mqttClient.loop → callback → routeMessage → ici →
  mbedtls). Ajouter 500 octets fixes en cours de route mange la
  marge.
- Économise les 500 octets dans `.bss` quand MQTT est inactif (effet
  négligeable sur 320 KB).
- En cas de débordement → stack overflow → crash silencieux (souvent
  un reboot avec `Guru Meditation Error: Core 0 panic'ed (Unhandled
  debug exception)`).

### Option D — heap dynamique

```cpp
void mqttPushFormattedMessage(...) {
    char* buf = (char*)malloc(MSG_BUFFER_SIZE);
    // ... use ...
    free(buf);
}
```

- Allocation/free à chaque appel.
- Risque de fragmentation au fil des messages.
- Aucun bénéfice par rapport à A pour un buffer de taille fixe.

### Décision pour ce projet

**Global (option A)** pour les raisons suivantes :

1. **Auditabilité** : le `Sketch uses ... bytes for global variables`
   du build report inclut nos 500 octets. On voit exactement notre
   empreinte mémoire.
2. **Cohérence avec les autres buffers MQTT** :
   `g_mqttOutgoingRecipientTopic[30]`, etc. — tous globaux.
3. **Protection de la stack** : avec mbedTLS qui ronge ~1500 octets
   pendant un publish, on a tout intérêt à garder le buffer hors stack.
4. **Pas de fragmentation** : aucune raison de faire allouer/free 500
   octets dynamiquement.

Pour des **petits** buffers (≤ 64 octets, typiquement les arguments
d'une commande `/wifi forget <ssid>`, ou un format epoch), la stack est
le bon choix — voir `mqttSendLiveness()` :

```cpp
char topic[MQTT_TOPIC_SIZE];     // 30 B stack
char payload[32];                // 32 B stack
snprintf(payload, sizeof(payload), "%s %ld", ...);
```

Le seuil informel dans ce projet : **≥ 100 octets → global ; < 100 octets
→ stack**.

## Outils pour inspecter la conso mémoire

### Au build (compile-time)

Le build report Arduino IDE indique :

```
Sketch uses 1240312 bytes (39%) of program storage space.   ← .text + .rodata + .data (image)
Global variables use 51284 bytes (15%) of dynamic memory.   ← .bss + .data (RAM)
```

→ Permet de surveiller l'empreinte mémoire au fil des modifs.

### Au runtime

- `ESP.getHeapSize()` — taille totale du heap (constant après boot).
- `ESP.getFreeHeap()` — bytes libres actuellement.
- `ESP.getMaxAllocHeap()` — plus gros bloc contigu libre.
- `ESP.getMinFreeHeap()` — watermark le plus bas depuis le boot.
- `uxTaskGetStackHighWaterMark(NULL)` — stack libre minimum (en
  *words*, multiplier × 4 pour octets) pour la tâche appelante.
- `ESP.getSketchSize()` / `ESP.getFreeSketchSpace()` — taille de la
  partition app + ce qui reste dispo.

Le `/dbg mem` du projet (`dumpMemInfo()` dans `minimessenger.ino`)
rapporte toutes ces métriques en KB sur l'écran TFT.

### Détection de fuites

Faire un boot, capturer `getMinFreeHeap`, laisser tourner X heures,
relire. Si la valeur a baissé sans qu'on s'y attende → fuite quelque
part. Le watermark ne peut que descendre (jamais remonter — c'est un
minimum historique).

## Références

- ESP-IDF Memory Types :
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/mem_alloc.html>
- ESP32 Partition Tables :
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html>
- Arduino ESP32 — `Preferences` (NVS wrapper) :
  <https://github.com/espressif/arduino-esp32/tree/master/libraries/Preferences>
- FreeRTOS task stack guidance :
  <https://www.freertos.org/Stacks-and-stack-overflow-checking.html>
- mbedTLS memory profile (utile pour comprendre les ~38 KB
  contigus) :
  <https://tls.mbed.org/kb/how-to/reduce-mbedtls-memory-and-storage-footprint>
