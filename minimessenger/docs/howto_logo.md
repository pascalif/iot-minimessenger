# Howto — Remplacer le logo de splash screen

## Contexte

Aujourd'hui le splash screen affiche `logo16_glcd_bmp` (`minimessenger.ino:402`), un bitmap **monochrome 16×16** (32 octets en PROGMEM) dessiné via `drawBitmap()` au centre de l'écran (`minimessenger.ino:1396`).

Ce document décrit deux pistes pour passer à une image **en couleur** plus grande :

| Option | Source de l'image | Lib supplémentaire | Coût flash | Heap décodage | Re-déployable sans reflasher l'app |
|--------|-------------------|--------------------|------------|---------------|------------------------------------|
| 1 — PROGMEM RGB565 | Tableau C statique dans le firmware | Aucune (Adafruit_GFX déjà là) | `w × h × 2` octets | 0 | Non |
| 2 — JPG/PNG depuis LittleFS | Fichier dans la partition de données | `TJpg_Decoder` ou `PNGdec` | ~20-30 Ko (la lib) + image | Quelques Ko transitoires | Oui (flash de la partition data seule) |

**Recommandation par défaut : option 1.** Plus simple, pas de lib supplémentaire, dessin instantané, aucun risque d'interférer avec le heap déjà tendu par mbedtls (cf. note NimBLE dans `CLAUDE.md`). L'option 2 n'a d'intérêt que si on veut pouvoir changer le logo sans recompiler/reflasher l'app, ou afficher plusieurs images volumineuses.

---

## Option 1 — PROGMEM RGB565

### Principe

Adafruit_GFX expose `drawRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h)`. On lui passe un tableau `const uint16_t PROGMEM[]` où chaque entrée est un pixel RGB565 (5 bits rouge, 6 bits vert, 5 bits bleu, big-endian).

On va :
1. Redimensionner et convertir l'image (JPG/PNG/…) en raw RGB888 avec Pillow.
2. Encoder chaque pixel en RGB565 et écrire un `splash.h` prêt à `#include`r en haut de `minimessenger.ino` (un `.ino` ne marcherait pas — cf. encadré « Pourquoi `.h` et pas `.ino` ? » plus bas).

### Pré-requis

```bash
# Pillow (Python Imaging Library)
pip3 install --user Pillow
# (Ubuntu/Debian alternative: sudo apt install python3-pil)
```

### Pipeline complet (un seul script)

Adapter les 5 variables en haut, puis exécuter :

```bash
SRC=~/Pictures/mon_logo.jpg
DST=/home/pascal/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/splash.h
W=64                # largeur cible en pixels
H=64                # hauteur cible en pixels
NAME=splash_bmp     # nom du symbole C généré

python3 - "$SRC" "$W" "$H" "$NAME" "$(basename "$DST")" > "$DST" <<'PY'
import sys
from PIL import Image

src, w, h, name, hdr_basename = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                                 sys.argv[4], sys.argv[5])

# Conversion RGB + redimensionnement (LANCZOS = filtre de bonne qualite pour reduction)
img = Image.open(src).convert("RGB").resize((w, h), Image.LANCZOS)
data = img.tobytes()  # octets bruts R,G,B,R,G,B,... (evite getdata() deprecated en Pillow 14)

# Oneliner pret a coller dans showSplashScreen()
oneliner = (f"pDisp->drawRGBBitmap((FB_WIDTH - {name}_w) / 2, (FB_HEIGHT - {name}_h) / 2, "
            f"{name}, {name}_w, {name}_h);")

# Header
print(f"#pragma once")
print(f"// auto-generated from {src} ({w}x{h}, RGB565)")
print(f"// regenerate via docs/howto_logo.md")
print(f"// paste in showSplashScreen(): {oneliner}")
print(f"static const uint16_t {name}_w = {w};")
print(f"static const uint16_t {name}_h = {h};")
print(f"static const uint16_t PROGMEM {name}[] = {{")

# Encodage RGB888 -> RGB565: R5 = bits 11..15, G6 = bits 5..10, B5 = bits 0..4
for i in range(w * h):
    r, g, b = data[i*3], data[i*3 + 1], data[i*3 + 2]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    end = "\n" if i % 12 == 11 else " "
    print(f"  0x{v:04X},", end=end)

print("\n};")

# Rappel sur stderr (stdout est redirige vers le .h) pour copier-coller direct dans l'IDE
print(f"\n--> ajouter en haut de minimessenger.ino (avec les autres includes projet):"
      f"\n  #include \"{hdr_basename}\"", file=sys.stderr)
print(f"\n--> a coller dans showSplashScreen():\n  {oneliner}\n", file=sys.stderr)
PY
```

Sortie attendue dans `splash.h` :

```cpp
#pragma once
// auto-generated from /home/pascal/Pictures/mon_logo.jpg (64x64, RGB565)
// regenerate via docs/howto_logo.md
// paste in showSplashScreen(): pDisp->drawRGBBitmap((FB_WIDTH - splash_bmp_w) / 2, (FB_HEIGHT - splash_bmp_h) / 2, splash_bmp, splash_bmp_w, splash_bmp_h);
static const uint16_t splash_bmp_w = 64;
static const uint16_t splash_bmp_h = 64;
static const uint16_t PROGMEM splash_bmp[] = {
  0xFFFF, 0xFFDF, 0xF7BE, 0xEF5D, ...
  ...
};
```

Et sur la console (stderr, pas redirigé) :

```
--> ajouter en haut de minimessenger.ino (avec les autres includes projet):
  #include "splash.h"

--> a coller dans showSplashScreen():
  pDisp->drawRGBBitmap((FB_WIDTH - splash_bmp_w) / 2, (FB_HEIGHT - splash_bmp_h) / 2, splash_bmp, splash_bmp_w, splash_bmp_h);
```

> **Pourquoi `.h` et pas `.ino` ?** Arduino IDE concatène tous les `.ino` du sketch dans l'ordre : le sketch principal (`minimessenger.ino`) d'abord, puis les autres par ordre alphabétique. Du coup `splash.ino` serait collé **après** le code qui l'utilise — l'auto-prototype généré par l'IDE ne couvre que les fonctions, pas les `static const`, donc le compilateur dit `splash_bmp was not declared in this scope`. En sortant le tableau dans un `.h` qu'on `#include` en haut de `minimessenger.ino`, on garantit que la déclaration est vue avant son utilisation.

### Câblage dans `showSplashScreen()`

1. Ajouter l'include en haut de `minimessenger.ino`, à côté des autres headers projet (`display.h`, `wifi_state.h`, …) :

```cpp
#include "splash.h"
```

2. Remplacer le bloc actuel autour de `minimessenger.ino:1396` :

```cpp
// Avant:
pDisp->drawBitmap((FB_WIDTH - 16) / 2, (FB_HEIGHT - 16) / 2, logo16_glcd_bmp, 16, 16, ST77XX_WHITE);

// Apres:
pDisp->drawRGBBitmap((FB_WIDTH - splash_bmp_w) / 2, (FB_HEIGHT - splash_bmp_h) / 2, splash_bmp, splash_bmp_w, splash_bmp_h);
```

`drawRGBBitmap` écrase tous les pixels du rectangle (pas de notion de couleur de fond/transparence) — utile à savoir si on superposait l'image sur un texte.

### Coût flash et tailles raisonnables

| Taille image | RAM/Flash | Commentaire |
|--------------|-----------|-------------|
| 32×32        | 2 Ko      | Mini-logo, encore monochrome-isant |
| 64×64        | 8 Ko      | Bon compromis splash centré |
| 128×128      | 32 Ko     | Splash bien visible, large mais OK ESP32 « Huge App » (1,9 Mo) |
| 240×135      | 64,8 Ko   | Plein écran ST7789 portrait |
| 240×320      | 153,6 Ko  | Splash plein cadre vertical complet |

Sur ESP8266 D1 mini (1 Mo flash, ~600 Ko libres dépendant des libs), rester sous ~150×150. Sur ESP32 partition « Huge App », même 240×320 passe sans souci.

### Gotchas

- **Endianness** : Adafruit_GFX attend du big-endian (octet de poids fort en premier). Le script ci-dessus le fait naturellement via le format `0xFFFF` (l'entier 16 bits est stocké en mémoire selon l'endian de la cible, et le compilateur s'aligne — ça marche tel quel sur ESP32 et ESP8266). En cas de couleurs inversées (rouge ↔ bleu visible), ajouter `pDisp->setEndianness(...)` ou byte-swap dans le script.
- **Couleurs un peu fades** : RGB565 = 65 K couleurs vs 16 M en RGB888, normal qu'un dégradé subtil perde du peps. Pour un logo plat ça ne se voit pas.
- **Image plus grande que l'écran** : `drawRGBBitmap` ne clippe pas proprement ; redimensionner en amont (c'est le rôle de `resize((W, H), ...)`).

### Variantes N/B (1bpp ou 8bpp gris)

Si l'image source est déjà en niveaux de gris ou qu'on accepte de la convertir, Adafruit_GFX propose deux formats plus compacts :

| Format           | Bytes/pixel | Fonction GFX             | 64×64  | 128×128 | 240×320 |
|------------------|-------------|--------------------------|--------|---------|---------|
| RGB565 (couleur) | 2           | `drawRGBBitmap()`        | 8 Ko   | 32 Ko   | 154 Ko  |
| Gris 8bpp        | 1           | `drawGrayscaleBitmap()`  | 4 Ko   | 16 Ko   | 77 Ko   |
| Mono 1bpp        | 1/8         | `drawBitmap(..., color)` | 512 o  | 2 Ko    | 9,6 Ko  |

- **Mono 1bpp** : c'est le format historique de `logo16_glcd_bmp`. Un bit par pixel, 8 pixels packés par octet (MSB-first), couleur passée en argument à la fonction de dessin (peut être n'importe quel RGB565, pas que blanc — `pDisp->drawBitmap(..., ST77XX_CYAN)` est parfaitement légal). Variante à deux couleurs : `drawBitmap(x, y, bmp, w, h, fg, bg)` peint aussi les pixels « 0 » avec une couleur de fond, utile sur un écran déjà peint.
- **Gris 8bpp** : un octet par pixel (0=noir, 255=blanc). Permet des dégradés et de l'anti-aliasing tout en restant 2× plus compact que RGB565. La fonction GFX convertit chaque octet en gris RGB565 (`r=g=b=v`) à l'envoi.

#### Pipeline unifié (3 modes)

Mêmes variables qu'au-dessus + une `MODE` à choisir parmi `rgb565` / `gray8` / `mono1` :

```bash
SRC=~/Pictures/mon_logo.jpg
DST=/home/pascal/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/splash.h
W=128
H=128
NAME=splash_bmp
MODE=mono1                # rgb565 | gray8 | mono1

python3 - "$SRC" "$W" "$H" "$NAME" "$MODE" "$(basename "$DST")" > "$DST" <<'PY'
import sys
from PIL import Image

src, w, h, name, mode, hdr_basename = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                                       sys.argv[4], sys.argv[5], sys.argv[6])

# Oneliner pret a coller dans showSplashScreen() selon le mode
xy = f"(FB_WIDTH - {name}_w) / 2, (FB_HEIGHT - {name}_h) / 2"
if mode == "rgb565":
    oneliner = f"pDisp->drawRGBBitmap({xy}, {name}, {name}_w, {name}_h);"
    label = "RGB565"
    c_type = "uint16_t"
elif mode == "gray8":
    oneliner = f"pDisp->drawGrayscaleBitmap({xy}, {name}, {name}_w, {name}_h);"
    label = "grayscale 8bpp"
    c_type = "uint8_t"
elif mode == "mono1":
    oneliner = f"pDisp->drawBitmap({xy}, {name}, {name}_w, {name}_h, ST77XX_WHITE);"
    label = "monochrome 1bpp, MSB-first"
    c_type = "uint8_t"
else:
    sys.exit(f"MODE inconnu: {mode} (attendu: rgb565 | gray8 | mono1)")

# Header commun aux 3 modes
print(f"#pragma once")
print(f"// auto-generated from {src} ({w}x{h}, {label})")
print(f"// regenerate via docs/howto_logo.md")
print(f"// paste in showSplashScreen(): {oneliner}")
print(f"static const uint16_t {name}_w = {w};")
print(f"static const uint16_t {name}_h = {h};")
print(f"static const {c_type} PROGMEM {name}[] = {{")

if mode == "rgb565":
    img = Image.open(src).convert("RGB").resize((w, h), Image.LANCZOS)
    data = img.tobytes()  # R,G,B,R,G,B,... (evite getdata() deprecated en Pillow 14)
    for i in range(w * h):
        r, g, b = data[i*3], data[i*3 + 1], data[i*3 + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        end = "\n" if i % 12 == 11 else " "
        print(f"  0x{v:04X},", end=end)

elif mode == "gray8":
    # Resize en luminance, puis 1 octet par pixel (0=noir, 255=blanc).
    img = Image.open(src).convert("L").resize((w, h), Image.LANCZOS)
    data = img.tobytes()
    for i, byte in enumerate(data):
        end = "\n" if i % 16 == 15 else " "
        print(f"  0x{byte:02X},", end=end)

elif mode == "mono1":
    # Resize en luminance (smooth), puis threshold a 1bit. tobytes() retourne MSB-first packed, format attendu par drawBitmap.
    img = Image.open(src).convert("L").resize((w, h), Image.LANCZOS).convert("1")
    data = img.tobytes()
    for i, byte in enumerate(data):
        end = "\n" if i % 16 == 15 else " "
        print(f"  0x{byte:02X},", end=end)

print("\n};")

# Rappel sur stderr (stdout est redirige vers le .h) pour copier-coller direct dans l'IDE
print(f"\n--> ajouter en haut de minimessenger.ino (avec les autres includes projet):"
      f"\n  #include \"{hdr_basename}\"", file=sys.stderr)
print(f"\n--> a coller dans showSplashScreen():\n  {oneliner}\n", file=sys.stderr)
PY
```

Note sur `convert("L") -> resize -> convert("1")` : on **resize avant de seuiller**. Faire l'inverse (resize d'une image déjà en 1bpp) produit du moiré et des escaliers ; resizer en luminance puis seuiller à la fin laisse Pillow appliquer son tramage Floyd-Steinberg par défaut, ce qui donne un rendu beaucoup plus propre sur les courbes et les zones de transition.

#### Câblage dans `showSplashScreen()` selon le mode

```cpp
// MODE=rgb565
pDisp->drawRGBBitmap((FB_WIDTH - splash_bmp_w) / 2, (FB_HEIGHT - splash_bmp_h) / 2,
                     splash_bmp, splash_bmp_w, splash_bmp_h);

// MODE=gray8
pDisp->drawGrayscaleBitmap((FB_WIDTH - splash_bmp_w) / 2, (FB_HEIGHT - splash_bmp_h) / 2,
                           splash_bmp, splash_bmp_w, splash_bmp_h);

// MODE=mono1 (la couleur peut etre n'importe quel RGB565)
pDisp->drawBitmap((FB_WIDTH - splash_bmp_w) / 2, (FB_HEIGHT - splash_bmp_h) / 2,
                  splash_bmp, splash_bmp_w, splash_bmp_h, ST77XX_WHITE);
```

#### Quand choisir quoi

- **Logo plat type pictogramme, contours nets** → `mono1`. C'est le sweet spot : un 128×128 ne pèse que 2 Ko, donc on peut même se permettre un splash plein écran 240×320 pour ~10 Ko de flash.
- **Photo N&B, dégradés importants** → `gray8`. Compromis raisonnable, garde l'anti-aliasing.
- **Image couleur** → `rgb565` (cf. section principale).

---

## Option 2 — JPG/PNG depuis LittleFS

### SPIFFS vs LittleFS — c'est quoi ?

Les deux sont des **systèmes de fichiers** embarqués qui vivent dans une zone de la flash NOR de l'ESP32/ESP8266 distincte du code applicatif. La flash est partitionnée en plusieurs zones (cf. « Tools → Partition Scheme » dans l'IDE) : bootloader, partition app(s), NVS, et une zone « data » dont la taille et le format dépendent du schéma choisi. Pour notre projet « Huge App (1.9 MB / 320 KB SPIFFS) » : ~320 Ko sont réservés à cette partition data.

- **SPIFFS** (SPI Flash File System) — l'ancêtre. Pas de vrais dossiers (les `/` dans les noms sont décoratifs), pas de tolérance aux coupures de courant en cours d'écriture, wear-leveling sommaire. **Déprécié depuis arduino-esp32 2.x** mais toujours fonctionnel — c'est ce que le nom de partition « SPIFFS » désigne historiquement.
- **LittleFS** — successeur recommandé. Vrais dossiers, écritures atomiques (résiste à un reset au milieu d'un `write`), wear-leveling correct, format ouvert. Sur ESP32 récent on l'utilise dans la même partition qui s'appelle toujours « SPIFFS » dans le tableau de partitions (le nom du schéma de partition n'a pas changé, mais on y monte LittleFS).

**On utilise LittleFS** dans la pratique aujourd'hui. SPIFFS n'a plus aucun intérêt sauf pour de la compatibilité legacy.

### Pourquoi c'est intéressant ici

La partition data est flashée **séparément** de l'app. Workflow :

1. On compile et flashe l'app une fois (avec le code qui sait lire un `splash.jpg` depuis LittleFS).
2. Pour mettre à jour le logo : on remplace `data/splash.jpg`, on flashe **seulement la partition data**, l'app reste intacte.
3. Possibilité d'aller plus loin : exposer une page web servie par l'ESP qui accepte un upload, ou de l'OTA-data via MQTT.

### Comment y accéder depuis l'extérieur du firmware

Trois méthodes, par ordre de simplicité :

#### a. Plugin Arduino IDE 2.x — « Arduino LittleFS Upload »

C'est le plus pratique pour du dev iteratif :

1. Installer le plugin via Arduino IDE 2.x : `Ctrl+Shift+P` → « Install Plugin » → chercher « LittleFS » (ou suivre <https://github.com/earlephilhower/arduino-littlefs-upload>).
2. Créer un dossier `data/` à côté de `minimessenger.ino` et y mettre les fichiers à embarquer (ex: `data/splash.jpg`).
3. `Ctrl+Shift+P` → « Upload LittleFS to Pico/ESP32 ». Le plugin construit une image LittleFS à partir du dossier `data/` et la flashe dans la partition data **sans toucher au code applicatif**.

#### b. CLI bas niveau : `mklittlefs` + `esptool.py`

Utile pour scripter / CI. Pseudo-commande :

```bash
# 1. Construire une image LittleFS de 320 Ko (taille = celle de la partition SPIFFS du schema choisi)
mklittlefs -c ./data -s 0x50000 littlefs.bin

# 2. Flasher uniquement la partition data (offset = celui defini dans partitions.csv,
#    typiquement 0x290000 pour le schema "Huge App" mais a verifier avec esptool.py read_flash)
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
           write_flash 0x290000 littlefs.bin
```

Le binaire `mklittlefs` est packagé avec arduino-esp32 (cherche dans `~/.arduino15/packages/esp32/tools/mklittlefs/`).

#### c. À l'exécution depuis l'app — page web d'upload

Une fois l'ESP en WiFi, on peut héberger un endpoint HTTP `POST /upload` qui reçoit le fichier et le sauve via `LittleFS.open("/splash.jpg", "w").write(...)`. Permet de mettre à jour le logo sans même brancher l'USB. Hors scope ici mais c'est le pattern utilisé par `ElegantOTA` et compagnie pour les firmwares OTA.

### Code minimal pour afficher un JPG depuis LittleFS

Librairie : **TJpg_Decoder** (Bodmer) via Library Manager.

```cpp
#include <LittleFS.h>
#include <TJpg_Decoder.h>

// Callback appele par le decodeur pour chaque bloc 16x16 decode. C'est ici qu'on pousse les pixels vers le TFT.
bool tftOutputCb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= FB_HEIGHT) return 0;  // stop si on depasse l'ecran
  ((Adafruit_ST7789*)g_disp)->drawRGBBitmap(x, y, bitmap, w, h);
  return 1;  // continue
}

void setupSplashFromFS() {
  if (!LittleFS.begin()) {
    log("LittleFS mount failed");
    return;
  }
  TJpgDec.setJpgScale(1);          // 1, 2, 4 ou 8 (downscale a la volee)
  TJpgDec.setSwapBytes(true);      // selon endianness du TFT, a ajuster si couleurs inversees
  TJpgDec.setCallback(tftOutputCb);
  TJpgDec.drawFsJpg(centerX, centerY, "/splash.jpg", LittleFS);
}
```

À mettre dans `showSplashScreen()` à la place du `drawBitmap` actuel.

### Coûts à garder en tête

- La lib TJpg_Decoder ajoute ~25-30 Ko de flash.
- Le décodage JPG alloue un buffer de travail (~ quelques Ko) — surveiller le heap juste avant le handshake mbedtls vers HiveMQ (~38 Ko contigus nécessaires, cf. `CLAUDE.md`). En pratique, faire le splash **avant** de monter MQTT évite tout conflit, et libérer le décodeur (`TJpgDec.setCallback(nullptr)`) en sortie.
- Un JPG de qualité 80 pour un logo 240×320 pèse typiquement 10-30 Ko — vs 154 Ko en RGB565 PROGMEM. C'est le seul vrai gain de l'option 2 côté place.

---

## Récap décisionnel

- **Logo unique, taille raisonnable (≤ 128×128), pas besoin de le changer souvent** → Option 1, basta.
- **Plusieurs images / grandes images / changement fréquent du visuel sans reflash app** → Option 2 (LittleFS + TJpg_Decoder).
- **Splash plein écran 240×320 en RGB565 PROGMEM** → ça passe sur ESP32 mais c'est 154 Ko de flash gaspillés pour un fichier qui ferait 20 Ko en JPG ; à ce stade l'option 2 devient justifiée.