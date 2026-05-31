# WiFi onboarding & runtime — minimessenger

## Vue d'ensemble

Minimessenger gère sa connexion WiFi via une **machine à état non-bloquante** qui combine trois sources de configuration:

1. **Compile-time defaults** dans `compiled_wifi.h` (gitignored, créé à partir de `compiled_wifi.h.template`) — **toujours chargés** dans WiFiMulti à chaque boot, en plus du NVS. Servent de safety net permanent.
2. **NVS (Preferences)** — réseaux ajoutés au runtime (via portail captif), prio sur les defaults compilés en cas de SSID dupliqué (password potentiellement plus frais)
3. **Portail captif WiFiManager** — fallback automatique quand aucun réseau connu ne répond, accessible via téléphone à l'AP `minimessenger-config`

**Règle de fusion**: à chaque boot, `setupWifi()` charge dans `WiFiMulti` d'abord toutes les entrées NVS, puis ajoute les compile defaults en sautant les SSIDs déjà présents dans NVS. Donc:
- SSID uniquement en compile defaults → utilisé tel quel
- SSID uniquement en NVS → utilisé tel quel
- SSID dans les deux → NVS gagne (son password peut avoir été mis à jour via portail)

Le boot ne bloque plus en attendant le WiFi: l'écran d'info, le clavier BLE et l'interface série sont fonctionnels dès le démarrage, et la connexion s'établit en arrière-plan via WiFiMulti. Si rien ne fonctionne, le portail s'ouvre tout seul.

## Architecture (état de connexion)

```
            ┌────────────┐
            │  BOOTING   │   setup() pas encore terminé
            └─────┬──────┘
                  │ setupWifi() (NVS load + state machine kick)
                  ▼
            ┌────────────┐
   ┌────────│ TRYING_KN. │   WiFiMulti.run() tous les 1s, timeout 15s
   │ 15s    └─────┬──────┘
   │      success │
   │              ▼
   │       ┌────────────┐
   │       │ CONNECTED  │   STA up + NTP + MQTT peuvent démarrer
   │       └─────┬──────┘
   │             │ WiFi.status() != WL_CONNECTED
   │             ▼
   │       ┌────────────┐
   │       │   LOST     │   WiFiMulti.run() tous les 5s, timeout 60s
   │       └─────┬──────┘
   │  60s        │
   │             │
   ▼             ▼
┌────────────────┐
│    PORTAL      │   WiFiManager AP "minimessenger-config" sur 192.168.4.1
└────────┬───────┘   Timeout 5 min → repasse TRYING_KNOWN pour un dernier essai
         │
         │ user saisit credentials
         │ → wifiOnPortalSave() → NVS + WiFiMulti
         ▼
    CONNECTED
```

L'état courant est lu par `showUpdatedInfoScreen()` qui adapte les rows affichées. Les bandeaux "WiFi lost" / "WiFi back" en rouge/vert dans la zone de conversation reflètent les transitions LOST ↔ CONNECTED.

## Workflows

### Premier flash sur un device vierge

1. Cloner le repo
2. Copier le template:
   ```bash
   cp compiled_wifi.h.template compiled_wifi.h
   ```
3. Éditer `compiled_wifi.h` pour y mettre tes SSIDs/passwords (jusqu'à 5):
   ```cpp
   static const CompiledWifiEntry COMPILED_WIFI_DEFAULTS[] = {
     { "MyHomeWiFi",     "homepass"    },
     { "MyPhoneHotspot", "hotspotpass" },
   };
   ```
4. Compiler + flasher via Arduino IDE
5. Au boot, NVS est vide → WiFiMulti est nourri uniquement par les compile defaults → connexion immédiate au SSID le plus fort des deux

**Si tu laisses `compiled_wifi.h` vide**: le device boote, WiFiMulti est vide, le portail s'ouvre tout seul. Tu configures depuis ton téléphone sans toucher au code. C'est le mode "distribution à un utilisateur final".

### Configurer un nouveau réseau via le portail

Quand tu déplaces un device dans un lieu sans WiFi connu (vacances, chez les beaux-parents, conf):

1. Au boot, le device essaie pendant 15s ses réseaux connus → aucun ne répond
2. L'écran passe en mode "WiFi setup" — affiche les instructions:
   ```
   WiFi setup
   AP:  minimessenger-config
   URL: 192.168.4.1
   THEN: pick WiFi
   ```
3. Sur ton téléphone:
   - Connecte-toi au WiFi `minimessenger-config` (AP ouvert, pas de password)
   - Ouvre un navigateur, va sur `http://192.168.4.1` (la plupart des téléphones ouvrent automatiquement un captive popup)
   - WiFiManager affiche la liste des WiFi détectés à proximité
   - Choisis le tien, tape le password, valide
4. Le device:
   - Reçoit les credentials, tente la connexion
   - Sur succès: ajoute le nouveau couple en NVS (slot libre ou nouveau slot)
   - Bascule en CONNECTED, le portail se ferme, l'écran reprend son flow normal
5. Au prochain boot dans ce lieu, le device se connecte directement (aucun portail nécessaire) — le réseau est désormais dans la liste NVS

### Le mot de passe d'un réseau a changé

Cas typique: la box maison a été reset, le password est nouveau.

1. Le device passe en LOST quand l'ancien password ne marche plus, retry pendant 60s
2. Après 60s sans succès → bascule auto en PORTAL → tu reconfigures via téléphone
3. Côté NVS, `wifiSaveToNvs()` détecte que le SSID existe déjà et **remplace le password en place** (pas de doublon)
4. À chaque boot suivant, le nouveau password en NVS gagne sur l'ancien dans compile defaults (règle de dédup)

Alternative manuelle: `/wifi forget MyHomeWiFi` puis `/wifi portal` pour ouvrir immédiatement le portail.

**⚠️ Cas particulier**: si tu fais `/wifi forget MyHomeWiFi` et que `MyHomeWiFi` est aussi déclaré dans `compiled_wifi.h`, l'entrée disparaît de NVS mais reste injectée dans WiFiMulti au prochain boot via les compile defaults (avec le password compilé, possiblement périmé). Pour vraiment ne plus s'y connecter du tout, supprime aussi l'entrée de `compiled_wifi.h` et reflashe.

### Déménagement complet / reset du device

```
/wifi clean  → vide NVS uniquement
[reboot]     → WiFiMulti rechargé avec les compile defaults seuls (portail si template vide)
```

`/wifi clean` n'efface PAS la connexion en cours et n'efface PAS les compile defaults — c'est juste un wipe NVS. Le device garde son lien actuel jusqu'au prochain reboot. Au prochain boot, seuls les compile defaults sont disponibles dans WiFiMulti (puisque NVS est vide).

### Reconnexion automatique (panne courte de la box)

1. La box reboote (ou perd le signal momentanément)
2. Le device passe en LOST: bandeau rouge "WiFi lost — Retrying..." dans la conversation
3. `WiFiMulti.run()` est appelé toutes les 5 secondes pour ré-essayer tous les réseaux connus (du plus fort RSSI au plus faible)
4. Quand la box revient: bandeau vert "WiFi back", MQTT se reconnecte sur sa propre boucle de retry
5. Si la box reste down > 60s → fallback PORTAL automatique

### Reconnexion après changement de lieu (sans le savoir)

Cas: tu emportes un device préconfiguré chez Papa, où le SSID est dans la liste NVS.

1. Boot → TRYING_KNOWN
2. WiFiMulti scanne les APs visibles, voit `WifiPapa` (connu), tente d'associer → CONNECTED
3. Aucune interaction utilisateur nécessaire

Si le SSID **n'est pas** dans la liste NVS → après 15s de TRYING_KNOWN → PORTAL → utilisateur le configure.

## Commandes disponibles

Toutes les commandes peuvent venir du **clavier BLE** (Enter pour valider), de **Serial** (via cable USB et console à 115200), ou de **MQTT** (broadcast vers tous les peers — utile pour scripter à distance).

Les commandes sont organisées en 4 commandes orphelines (`/help`, `/status`, `/mqtt-drop`, `/bt-clean`) et 2 groupes (`/wifi *`, `/dbg *`). Taper la racine du groupe seule (ex: `/wifi` sans argument) affiche l'aide partielle du groupe.

| Commande | Effet |
|----------|-------|
| `/help` | Liste les commandes orphelines + les racines de groupes |
| `/status` | Affiche l'écran d'info pendant 10s |
| `/mqtt-drop` | Disconnect MQTT (test de résilience). MQTT se reconnecte automatiquement via la boucle de retry |
| `/bt-clean` | Vide les bonds BLE (clavier doit re-pair après reboot) |
| `/wifi` | Aide partielle: liste uniquement les sous-commandes du groupe `/wifi *` |
| `/wifi drop` | Force un disconnect WiFi (test de résilience). Le device passe en LOST puis tente de se reconnecter |
| `/wifi list` | Affiche en rose les SSIDs sauvés en NVS |
| `/wifi forget <ssid>` | Supprime un SSID précis de NVS (laisse les autres). Ex: `/wifi forget OldWiFi` |
| `/wifi clean` | Vide totalement NVS WiFi. Au prochain reboot, re-seed depuis `compiled_wifi.h` ou portail |
| `/wifi portal` | Force l'ouverture du portail captif immédiatement, sans attendre un échec |
| `/dbg` | Aide partielle: liste uniquement les sous-commandes du groupe `/dbg *` |
| `/dbg redraw` | Repaint complet des 3 zones (status bar + scroll + footer) |
| `/dbg chip` | Diagnostic: chip model, revision, MACs, IDF version, reset reason |
| `/dbg mem` | Diagnostic: heap libre, fragmentation, stack high-water-mark |

## Schéma NVS

Namespace `wifi` (via Arduino `Preferences`). Clés:

- `count` (uint8): nombre de slots **alloués**. Borne supérieure quand on itère; certains slots intermédiaires peuvent être vides (trous laissés par `/wifi forget`, réclamés au prochain `wifiSaveToNvs`).
- `ssid_0`, `pwd_0`, …, `ssid_4`, `pwd_4` (String): jusqu'à `MAX_WIFI_NETWORKS=5` paires. SSID vide = slot libre.

Les `COMPILED_WIFI_DEFAULTS` ne sont **jamais écrits en NVS** — ils sont injectés directement dans WiFiMulti au boot par `wifiLoadNVSAndCompiledIntoMulti()`. Modifier `compiled_wifi.h` + reflasher est donc immédiatement effectif au prochain boot, sans dépendre de l'état NVS. `/wifi list` ne montre que les entrées NVS (les compile defaults sont des constantes du code, pas des données utilisateur).

## Que dit chaque ligne du status screen ?

| Ligne | État | Sens |
|-------|------|------|
| `SSID: (searching)` | TRYING_KNOWN/LOST | Aucun AP associé pour l'instant, scan en cours |
| `SSID: MyWifi` | CONNECTED | Connecté à l'AP `MyWifi` |
| `IP: Connecting...` | TRYING_KNOWN | Tentative d'association |
| `IP: Lost, retrying` | LOST | Connexion tombée, retry en cours |
| `IP: 192.168.x.y` | CONNECTED | DHCP a délivré une adresse |
| `MQTT: OK` | CONNECTED + MQTT up | TLS handshake réussi, broker répond |
| `MQTT: NOT OK` | autre | MQTT pas (encore) connecté |
| `TIME: Paris (UTC+1/2)` | NTP synced | DST géré automatiquement |
| `WiFi setup / AP: ...` | PORTAL | Portail captif actif, voir les instructions |

## Diagnostic

Logs à surveiller (préfixe `WIFI` pour cette zone):

- `Loaded N network(s) from NVS into WiFiMulti` — au boot, montre combien de réseaux connus
- `State: X → Y` — chaque transition d'état, X et Y étant les enum int (0=BOOTING, 1=TRYING_KNOWN, 2=PORTAL, 3=CONNECTED, 4=LOST)
- `TRYING_KNOWN timeout (15000 ms) — no known network responded` — bascule en portail attendue
- `Portal saved credentials: SSID=[...]` — l'utilisateur a soumis le formulaire
- `Connection lost (was CONNECTED)` — falling edge détecté
- `Connected to [...], IP=..., RSSI=...` — rising edge, mesure de signal incluse

Pour ajuster les timeouts, voir les `#define` en tête de `wifi.ino`:
- `WIFI_TRYING_KNOWN_TIMEOUT_MS` (15s par défaut)
- `WIFI_PORTAL_TIMEOUT_MS` (5 min)
- `WIFI_LOST_TO_PORTAL_MS` (60s)
- `WIFI_LOST_RETRY_INTERVAL_MS` (5s)
- `WIFI_TRYING_KNOWN_RETRY_INTERVAL_MS` (1s)

## Limitations connues

- **WiFiManager portal est HTTP only**, pas HTTPS. Acceptable pour un réseau privé domestique, pas pour un usage en environnement non-trust.
- **Pas de WPA Enterprise** supporté côté NVS (juste WPA/WPA2 PSK). Les réseaux Eduroam et co. ne marchent pas via ce système.
- **Pas de detection automatique de "mauvais password"**: si tu changes le password d'un AP côté router sans toucher au device, WiFiMulti continue d'essayer indéfiniment. Workaround: `/wifi forget <ssid>` + portail, ou attendre 60s pour le fallback PORTAL.
- **AP générique `minimessenger-config`**: si plusieurs minimessenger sont en mode portail simultanément dans le même lieu, il n'y aura qu'un seul AP visible — configurer les devices un par un.
- **Portal save callback fragile au stack WiFiManager**: si la lib est upgradée, vérifier que `WiFi.SSID()` / `WiFi.psk()` retournent toujours des valeurs valides à l'appel du callback.

## Pour aller plus loin

- Ajouter un OTA (Over-The-Air update) basé sur la même connexion: hors scope ce PR, mais l'architecture non-bloquante le permet maintenant.
- Hot-reload de `compiled_wifi.h` sans `/wifi clean`: faudrait un hash des defaults stocké en NVS, comparer au boot, re-seeder si différent. Pas implémenté.
- Filtrer les commandes destructives (`/wifi clean`, `/dbg bt-clean`) selon le canal d'entrée: un `/wifi clean` venu de MQTT broadcast efface NVS sur tous les peers. Pour l'instant, accepté tel quel.
