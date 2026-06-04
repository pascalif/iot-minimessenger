# Howto — MQTT in minimessenger

> Vue d'ensemble de la pile MQTT côté firmware : QoS (vraiment, surtout
> publish-QoS vs subscribe-QoS), messages **retained** et leur effet sur
> le boot, et clean session. Pour la topologie (qui publie où, quels
> topics, quels callbacks) → voir la section *MQTT topology* du
> `CLAUDE.md` à la racine du projet, et le code dans `mqtt.ino`.

## TL;DR

- **PubSubClient** (Library Manager 2.8) ne sait publier qu'en **QoS 0**.
  Subscribe en 0 ou 1, **jamais 2**.
- QoS effective = **`min(publish_QoS, subscribe_QoS)`**. Monter en QoS
  d'un seul côté ne sert à rien.
- Un message **retained** est stocké côté broker pour **ce topic
  exact** et re-livré à tout nouvel abonné. Clé de stockage = topic
  (pas couple topic+publisher) — si N devices publient retained sur le
  même topic, seul le dernier survit.
- **Schéma actuel pour la présence des peers** : un seul topic family,
  `admin/liveness/<deviceId>`, en retained. Couvre les quatre
  transitions :
  - `"BOOT <epoch>"`, `"RECO <epoch>"`, `"LIVE <epoch>"` → device
    présent. `<epoch>` = `time(nullptr)` au moment du publish.
  - `"DEAD"` (sans timestamp) → device absent. C'est aussi le payload
    Will, envoyé par le broker quand il détecte une déco.
- Chaque device s'abonne au wildcard `admin/liveness/+`. Au boot, il
  reçoit en une fois un retained par peer — alive ou dead — avec le
  bon `deviceId` lisible dans le suffixe du topic.
- Le dispatcher ignore les retains LIVE/BOOT/RECO dont l'`epoch` est
  plus vieux que `MQTT_KEEPALIVE_INTERVAL_MS + 5 s` : c'est typiquement
  un peer qui a crashé sans déclencher son Will, et dont le retained
  LIVE traîne, figé, sur le broker.
- **Aucun retain sur le chat** (`msg/broadcast`, `msg/unicast/*`). Le
  flag `retained` de `mqttPushFormattedMessage` reste explicite ; les
  callers de chat passent `MQTT_MSG_NOT_RETAINED`.

## Où vit le code MQTT

- `mqtt.ino` — connexion, (re)connexion avec backoff exponentiel,
  publish, callback de réception, dispatch par topic.
- `mqtt.h` — `extern`s + tous les `#define` partagés (`MQTT_QOS_*`,
  `MQTT_MSG_*`, `MQTT_SESSION_*`, timing, buffer sizes, prototypes).
- `minimessenger.ino` — credentials broker (`mqtt_server` /
  `mqtt_port` / `mqtt_user` / `mqtt_password`), la gate de reconnexion
  dans `loop()`, l'indicateur MQTT du status bar, `setRecipient()`,
  `onMQTTReconnected()`.

## Quality of Service (QoS)

MQTT définit trois niveaux de qualité de service par message. Chaque
niveau est un contrat sur la livraison **entre le broker et un client**
(donc applicable des deux côtés indépendamment : publisher → broker, et
broker → subscriber).

| QoS | Nom              | Garantie                         | Coût            |
|-----|------------------|----------------------------------|-----------------|
| 0   | At most once     | Fire & forget. Aucun ACK.        | 1 paquet.       |
| 1   | At least once    | Au moins une livraison. Peut dupliquer. | 2 paquets (PUBLISH + PUBACK). |
| 2   | Exactly once     | Exactement une livraison. Pas de duplicat. | 4 paquets (PUBLISH/PUBREC/PUBREL/PUBCOMP). |

### Publish-QoS vs Subscribe-QoS — la règle du min()

C'est le piège classique. La QoS effective avec laquelle un subscriber
**reçoit** un message vaut :

```
effectiveQoS = min(publishQoS, subscribeQoS)
```

Donc :

- **publish QoS 2 + subscribe QoS 0** → le subscriber reçoit en QoS 0.
  Le broker a tenté de livrer en at-most-once parce que le subscriber
  l'a demandé. Il peut perdre le message.
- **publish QoS 0 + subscribe QoS 2** → le subscriber reçoit en QoS 0.
  S'abonner en QoS 2 ne magique pas un publish fire-and-forget : si le
  publisher a perdu le PUBLISH avant qu'il arrive au broker, le message
  n'existe pas. Le subscribe-QoS ne s'applique que sur le tronçon
  **broker → ce subscriber**, pas en arrière.
- **publish QoS 1 + subscribe QoS 1** → at-least-once de bout en bout.
  Le subscriber peut recevoir des duplicats (et doit dédupliquer par
  packet identifier ou par contenu).
- **publish QoS 1 + subscribe QoS 0** → le subscriber peut perdre le
  message même si le broker a accusé réception au publisher. Le
  publisher pense qu'il est arrivé, le destinataire ne l'a peut-être
  jamais vu.

Conséquence : **les deux côtés doivent monter en QoS pour qu'une
garantie tienne**. Une chaîne de production fiable, c'est publisher
QoS ≥ 1 **et** subscriber QoS ≥ 1, pour chaque message.

### Ce que PubSubClient supporte

- **Publish** : QoS 0 uniquement. La signature `publish(topic, payload,
  retained)` n'a même pas de paramètre QoS. Si tu veux QoS 1/2 en
  publish, il faut une autre lib (AsyncMqttClient, esp-mqtt côté IDF,
  ArduinoMqtt…).
- **Subscribe** : QoS 0 ou QoS 1 via la signature `subscribe(topic,
  qos)`. **QoS 2 n'est pas implémenté** dans la lib — passer 2 est
  silencieusement clampé à 1.

### Conséquences concrètes pour ce projet

Code actuel dans `mqtt.ino::mqttReconnectAttempt()` :

```cpp
g_mqttClient.subscribe(g_mqttIncomingTopicBroadcast, MQTT_QOS_1);   // msg/broadcast
g_mqttClient.subscribe(myUnicastTopic.c_str(),       MQTT_QOS_1);   // msg/unicast/<myId>
g_mqttClient.subscribe(MQTT_LIVENESS_TOPIC_WILDCARD, MQTT_QOS_0);   // admin/liveness/+ — voir section "présence" plus bas
```

Et côté publish (`mqttPushFormattedMessage`) → toujours QoS 0 (limite
PubSubClient).

Donc en pratique :

- **`min(0, 1) = 0`** pour les topics `msg/*` : on s'abonne poliment en
  QoS 1, mais comme tout publisher est aussi sous PubSubClient et publie
  en QoS 0, on reçoit en QoS 0. Aucune retransmission, aucun ACK. Si le
  TLS hiccupe au mauvais moment, le message est perdu.
- **`min(0, 0) = 0`** pour les topics `admin/*` : QoS 0 partout, normal.

S'abonner en QoS 1 reste utile **uniquement** si un futur publisher (un
script Python, un debugger Mosquitto, …) publie en QoS ≥ 1 — alors notre
device bénéficiera des retransmissions broker → device.

### Recommandation pratique

Tant que tous les devices restent sur PubSubClient, **les QoS sont
cosmétiques au-delà de 0**. Ne pas changer le code juste pour passer
des `1` partout : ça donne une fausse impression de fiabilité.

Si la fiabilité de livraison devient un vrai sujet, l'option propre est
de migrer publish + subscribe vers une lib qui supporte vraiment QoS 1
(AsyncMqttClient sur ESP32, par exemple), pas de bidouiller les
constantes ici.

## Messages "retained" et rejouage au boot

Le mécanisme retain est puissant et piégeux : c'est lui qui rend
possible la présence persistante des peers (`admin/liveness/<id>`),
mais c'était aussi la source du bug "chaque reboot ré-affiche la
dernière conversation". Cette section explique pourquoi, et le schéma
de présence qui en résulte.

### Définition

Un message publié avec **retained = true** est stocké côté broker sous
une clé = **le topic exact**. Un seul message retenu par topic. Une
nouvelle publication retained sur ce topic écrase le précédent ; un
publish retained avec payload vide le supprime. Chaque nouvel abonné à
ce topic (ou matchant un wildcard couvrant ce topic) reçoit
immédiatement le dernier retained, en plus des messages futurs.

C'est conçu pour exposer un **état**, pas un **événement** :

- ✅ Bon usage : "device 4 est en ligne", "la température est à 21 °C".
- ❌ Mauvais usage : "Bob a dit hello", "le bouton a été pressé".

### Piège n°1 — la clé de stockage est le topic, pas le publisher

Si plusieurs devices publient retained sur **le même topic**, ils se
piétinent : le broker n'en garde qu'un, le dernier publisher gagne. Si
on avait gardé un `admin/live` partagé entre 4 devices :

```
t=0  device 1 publish (retained) "1 keep ..."  → retained[admin/live] = "1 keep ..."
t=1  device 2 publish (retained) "2 keep ..."  → retained[admin/live] = "2 keep ..." (1 écrasé)
t=2  device 3 publish (retained) "3 keep ..."  → retained[admin/live] = "3 keep ..." (2 écrasé)
t=3  device 4 publish (retained) "4 keep ..."  → retained[admin/live] = "4 keep ..." (3 écrasé)
```

Un nouveau device qui s'abonne ne reçoit **un seul** retained — celui
du dernier publisher. Il n'apprend les autres qu'à leur prochain
keepalive (jusqu'à 120 s d'attente).

**La parade** : un topic par publisher (`admin/liveness/<id>`) +
souscription via wildcard (`admin/liveness/+`). Le broker garde alors
un retained par topic distinct → une entrée par device. Le nouveau
device en reçoit l'ensemble en une fois.

### Piège n°2 — le dernier retained reste, indéfiniment

Si on retient un événement (un message de chat par exemple), le broker
le garde jusqu'à ce qu'il soit explicitement remplacé ou effacé. Donc
chaque nouveau subscriber, chaque reconnect, chaque reboot →
re-livraison.

D'où la règle : **ne retenir que les états**. Dans le code, le drapeau
`retained` de `mqttPushFormattedMessage` est explicite, choisi par le
caller. La règle d'or :

| Topic                          | Retain   | Pourquoi |
|--------------------------------|----------|----------|
| `admin/liveness/<id>`          | ✅ true  | État (présence). Subscribé via wildcard pour panorama immédiat. |
| `msg/broadcast`                | ❌ false | Évènement de chat. |
| `msg/unicast/<id>`             | ❌ false | Évènement de chat. |
| `admin/logs`                   | ❌ false | Évènement (si un jour utilisé). |

## Présence des peers — schéma `admin/liveness/<id>`

C'est l'usage central du retain dans ce projet. Une **seule** famille
de topics pour porter alive ET dead, avec un payload structuré.

### Format du payload

```
"BOOT <epochSeconds>"     ← première publication après un boot frais
"RECO <epochSeconds>"     ← publication post-reconnexion (n-ième connect)
"LIVE <epochSeconds>"     ← keepalive périodique tous les MQTT_KEEPALIVE_INTERVAL_MS
"DEAD"                    ← Will tiré par le broker à la déconnexion. Pas d'epoch.
```

`<epochSeconds>` = `time(nullptr)` au moment du publish (entier
décimal). Format choisi par opposition à un timestamp formaté
(`YYYY-MM-DD HH:MM:SS`) parce que la comparaison côté receiver se
résume à une soustraction d'entiers — pas de `strptime`, pas de parse
complexe.

Les 4 mots-clés sont portés par un `enum class` dans `mqtt.h`, et deux
helpers font le pont avec leur forme wire :

```cpp
// mqtt.h
enum class MQTTLiveness : uint8_t { BOOT, RECO, LIVE, DEAD };

const char* mqttLivenessAsString(MQTTLiveness subtype);
bool        parseMQTTLiveness(const char* payload,
                                        MQTTLiveness& outSubtype);
```

Côté receiver, `parseMQTTLiveness` lit les 4 premiers
caractères du payload, vérifie le délimiteur derrière (' ' ou '\0'
pour éviter qu'un `"DEADBEEF"` matche `DEAD`), et retourne l'enum.
Côté publisher (`mqttSendLiveness()` et le `willMessage` de `connect()`),
`mqttLivenessAsString()` donne la string immuable à coller dans
le payload.

Le dispatcher dans `mqtt.ino` enchaîne ensuite avec la sémantique de
plus haut niveau, l'enum **`ContactLiveness`** (LIVE / DEAD) défini
dans `contacts.h` — `onReceivedContactOnline()` ne reçoit que cette
forme binaire, BOOT/RECO/LIVE s'effondrent en `ContactLiveness::LIVE`.

### Le Will porte un payload DEAD retenu sur le même topic

C'est la pièce centrale qui permet à `admin/liveness/<id>` d'être le
seul topic de présence : la Last Will Testament posée à `connect()` a
pour cible `admin/liveness/<myId>`, payload `"DEAD"`, **retained =
true**. Quand le broker détecte la déconnexion, il publie ce payload
et le retient — il **écrase** le dernier LIVE/BOOT/RECO précédemment
publié. Un futur subscriber au wildcard voit donc le DEAD à la place
de l'ancien LIVE.

Le tombstone se nettoie automatiquement à la prochaine reconnexion du
device : son `mqttSendLiveness(0)` post-connect publie un `BOOT <epoch>`
retenu, qui écrase le DEAD.

```cpp
// mqttReconnectAttempt() — extrait
char willTopic[MQTT_TOPIC_SIZE];
snprintf(willTopic, MQTT_TOPIC_SIZE, MQTT_LIVENESS_TOPIC_PREFIX "%d", g_deviceData.deviceId);

g_mqttClient.connect(g_deviceData.name(),
                     g_mqttServerInfo.user, g_mqttServerInfo.password,
                     willTopic,
                     MQTT_QOS_0,
                     MQTT_MSG_RETAINED,                                          // ← retain du Will = true
                     mqttLivenessAsString(MQTTLiveness::DEAD),   // "DEAD"
                     MQTT_SESSION_VOLATILE);
```

### Dispatch côté receiver

`onMqttIncomingMessage()` applique le contrat en 5 étapes :

1. Le topic match `admin/liveness/<id>` → extraire `<id>` depuis le
   suffixe (via `parseLeadingDeviceId`).
2. Si `<id> == g_deviceData.deviceId` → ignorer (c'est notre propre
   retained rejoué par le broker lors de la souscription au wildcard).
3. Parser le 1er champ du payload via `parseMQTTLiveness()`.
   Échec (TYPE inconnu ou tronqué) → log warn, ignorer.
   - Si `MQTTLiveness::DEAD` →
     `onReceivedContactOnline(id, ContactLiveness::DEAD)`, terminé.
4. Sinon (BOOT/RECO/LIVE), lire le 2ᵉ champ comme `epochSeconds`. Si
   absent ou non numérique → log warn, ignorer.
5. **Staleness check** : si notre horloge locale est synchro
   (`time(nullptr) > 1.7e9`) et que `now - payload.epoch > keepalive
   + 5 s`, le retained est obsolète (peer crashé sans Will, retained
   figé) → ignorer. Sinon →
   `onReceivedContactOnline(id, ContactLiveness::LIVE)`.

L'étape 5 est défensive : en théorie, un peer qui meurt déclenche son
Will → le retained passe à DEAD → l'étape 3 le traite. Mais si le
broker ne détecte jamais la déco (réseau silencieux, keepalive TCP
qui traîne), le LIVE sur ce peer reste retenu pendant des heures avec
un epoch figé. La staleness check évite qu'un nouveau device traite ce
fossile comme un peer actif.

### Pourquoi pas de horodatage dans le Will lui-même ?

Le Will est figé au moment de `connect()` côté broker. Le device est
mort par définition quand le Will est tiré — il ne peut pas
"timestamp" l'événement à l'instant du décès. Un `ts:` baked dans le
Will au moment du connect mentirait : il indiquerait l'heure de
connexion, pas l'heure de mort.

Plutôt que de mentir, on s'en passe : le payload `"DEAD"` est sec, et
l'horodatage de mort se reconstruit côté observateur à partir de
**deux sources** :

1. Si l'observateur était déjà connecté quand le DEAD est arrivé : il
   stampe l'événement avec sa propre `time(nullptr)` au moment de la
   réception (précision = seconde).
2. Si l'observateur arrive plus tard et lit le retained DEAD : il
   peut au mieux dire "mort à un instant antérieur à maintenant".
   Le dernier `LIVE <epoch>` qu'il avait éventuellement vu avant le
   DEAD donne la borne inférieure. Précision ≈ ±
   `MQTT_KEEPALIVE_INTERVAL_MS`.

C'est le maximum accessible en MQTT 3.1.1. Mieux exigerait MQTT 5
(broker-side timestamp via properties) — inatteignable avec
PubSubClient.

### Cycle de vie complet d'un peer (résumé)

```
Device 3 boot
  ├─ setupWifi() → WiFi UP
  ├─ wifiOnConnected() → setupNTP() bloque jusqu'à SNTP sync (max 15 s)
  ├─ mqttReconnectAttempt() — gate "time(nullptr) >= 1.7e9" sinon return false
  │   Tant que NTP pas synchro, MQTT ne se connecte PAS → aucun publish bidon.
  ├─ connect() — Will armé sur admin/liveness/3 = "DEAD" retained
  └─ mqttSendLiveness(BOOT) → publish "BOOT 1717459200" retained sur admin/liveness/3
     [tombstone DEAD éventuel d'un précédent crash écrasé]

Device 3 vie courante
  └─ toutes les 120 s : mqttSendLiveness(LIVE) → publish "LIVE <now>" retained

Device 3 perte réseau temporaire
  ├─ broker détecte la déco → publie le Will : "DEAD" retained sur admin/liveness/3
  └─ retained sur le broker est maintenant DEAD

Device 3 revient
  ├─ connect() — Will armé à nouveau (NTP déjà synchro depuis le 1er boot, gate passe direct)
  └─ mqttSendLiveness(RECO) → publish "RECO <now>" retained
     [DEAD écrasé par RECO, peers déjà connectés reçoivent le RECO]

Device 3 crashe hard (power yank), broker ne détecte rien pendant longtemps
  └─ retained reste "LIVE <epoch_d'il_y_a_des_heures>"
     [staleness check côté nouveau subscriber : ignore]
  ↓ broker finit par détecter (timeout TCP)
  └─ Will tiré : retained passe à DEAD
```

Le pré-check NTP dans `mqttReconnectAttempt()` est la pièce qui garantit
qu'aucun retained `BOOT <epoch>` ne part avec un epoch boot-relatif (du
genre `BOOT 22` quand le device a démarré il y a 22 secondes et que SNTP
n'a pas encore répondu). La gate utilise le même seuil que `setupNTP()`
— `time(nullptr) >= 1'700'000'000` — donc le contrat est cohérent
entre les deux fonctions : NTP synced ⇔ MQTT autorisé à publier.

### Purger les retains historiques côté broker

Si tu modifies la sémantique d'un topic ou que tu veux nettoyer après
un test, le broker garde toujours le dernier retained reçu. Pour les
effacer, publier un payload vide retained :

```bash
mosquitto_pub -h <cluster>.s1.eu.hivemq.cloud -p 8883 --capath /etc/ssl/certs -u <user> -P <pass> -t 'admin/liveness/3' -r -n
mosquitto_pub … -t 'admin/live'    -r -n      # ancien topic d'avant le refactor
mosquitto_pub … -t 'admin/live/3'  -r -n      # idem
mosquitto_pub … -t 'admin/dead'    -r -n      # idem
```

`-r` retained, `-n` null/empty. Alternative : la console web HiveMQ
Cloud expose un "Clear retained message" par topic.

## Clean session

Dans `mqttReconnectAttempt()`, on passe `MQTT_SESSION_VOLATILE = true`
au 7ᵉ argument de `g_mqttClient.connect(...)`. Sens :

- **`true` (volatile / clean)** : le broker oublie tout état de notre
  session à la déconnexion (subscriptions, messages QoS ≥ 1 en attente).
  À chaque reconnect, on doit re-souscrire — c'est exactement ce que
  fait notre code.
- **`false` (persistent)** : le broker garde notre liste de
  subscriptions ET met en queue les messages QoS ≥ 1 reçus pendant
  qu'on était déconnecté, à délivrer au prochain reconnect.

Pourquoi `true` chez nous :

- On publie tout en QoS 0 (PubSubClient), donc rien ne serait queué de
  toute façon.
- Les subscriptions sont peu nombreuses et triviales à recréer.
- Persistent + clean=false signifie aussi que la queue du broker peut
  grossir pendant qu'on est offline — risque de bombe à retardement
  quand on reconnect après une coupure longue.

Avec QoS 0 partout, clean session ≈ no-op fonctionnel. Garder `true`.

## Alternatives à PubSubClient

PubSubClient (Nick O'Leary, 2012) est notre lib actuelle : MQTT 3.1.1,
publish QoS 0 uniquement, subscribe QoS 0/1, API synchrone, ~6 KB de
flash. Battle-tested, stable, mais minimaliste. La question — légitime —
"et si on passait à une lib plus moderne ?" mérite une réponse posée
parce que le coût n'est pas nul.

### Les candidats sérieux (vérifié juin 2026)

| Lib                                  | MQTT 5 | QoS pub | Async | Code flash | Maintenance        | API                  | Stack TLS                      |
|--------------------------------------|--------|---------|-------|------------|--------------------|----------------------|--------------------------------|
| **PubSubClient** (actuel, knolleary) | ❌     | 0 only  | non   | ~6 KB      | calme (stable)     | très simple          | WiFiClientSecure + mbedTLS     |
| **espMqttClient** (bertmelis)        | ❌     | 0/1/2   | sync ou async | ~30 KB | très active   | callbacks lambda     | WiFiClientSecure + mbedTLS     |
| **ArduinoMqttClient** (Arduino off.) | ❌     | 0/1/2   | non   | ~10 KB     | officielle (lente) | proche PubSub        | WiFiClientSecure + mbedTLS     |
| **esp-mqtt** (ESP-IDF)               | ✅     | 0/1/2   | async (event loop) | ~25 KB | Espressif (gold) | C, event-driven      | esp-tls (séparé)               |

⚠ **Important** : dans l'écosystème Arduino-ESP32, **aucune lib
Arduino-friendly ne supporte MQTT 5**. PubSubClient, espMqttClient,
ArduinoMqttClient, AsyncMqttClient sont tous MQTT 3.1.1. **Le seul
chemin vers MQTT 5 sur ESP32 est `esp-mqtt` de l'ESP-IDF**, qui change
de paradigme (API C event-driven plutôt qu'Arduino/C++ classique).

`AsyncMqttClient`, longtemps populaire, est devenu fragile depuis
qu'AsyncTCP_SSL a divergé (dernière release 2021). À éviter
aujourd'hui sur ESP32.

### Ce qu'on gagnerait — deux scénarios distincts

Il y a deux niveaux d'upgrade possibles, à séparer parce que les
bénéfices et coûts ne sont pas comparables :

#### Scénario A — Upgrade MQTT 3.1.1 (espMqttClient ou ArduinoMqttClient)

Le seul gain réel : **QoS 1 en publish**, donc garantie at-least-once
sur les messages chat. Les messages ne sont plus perdus silencieusement
quand TLS hoquette. C'est le bénéfice visible côté utilisateur final.

Bonus secondaires : `MQTT_MAX_PACKET_SIZE 256` disparaît (taille
dynamique), API async possible avec espMqttClient (latence un peu
meilleure pendant les publish).

**Ce qu'on N'a PAS** : pas de No Local subscription, pas de user
properties, pas de Will delay. Tous les nettoyages architecturaux
liés à MQTT 5 restent hors de portée — le trailer
`### deviceId:N msgId:N` reste nécessaire pour le self-filter, etc.

#### Scénario B — Upgrade vers MQTT 5 (esp-mqtt uniquement)

C'est seulement ici qu'on accède aux features MQTT 5 qui changent
l'architecture :

1. **No Local subscription** — le broker ne renvoie pas nos propres
   publish via la wildcard. `extractSenderAndStripTrailer()` et le
   trailer deviennent **inutiles** pour le self-filter. Nettoyage
   architectural significatif.

2. **User Properties** — on déplace `senderId` du payload (trailer)
   vers une property metadata séparée. Le payload chat redevient
   **purement le texte saisi par l'utilisateur**, donc parfaitement
   compatible avec une console web qui tape "salut" sans tricks.

3. **Will Delay** — le broker attend N secondes après détection de
   déconnexion avant de tirer le Will. Évite les "DEAD" intempestifs
   sur reconnexions courtes.

4. **Subscription IDs, Message Expiry, Topic Alias** — features
   périphériques que ce projet n'utiliserait probablement pas.

Et bien sûr QoS 1/2 inclus aussi (donc le bénéfice du scénario A est
embarqué).

### Ce qu'on paierait

Coûts communs aux deux scénarios :

1. **Flash** : +5 à +30 KB selon la lib (ArduinoMqttClient ~10 KB,
   espMqttClient ~30 KB, esp-mqtt ~25 KB). Sur notre binaire de
   1.56 MB sur partition de 3 MB, c'est confortable, mais ça mange la
   marge si on prévoit l'OTA (partitions de 1.9 MB max — cf.
   `info_memory_mgt.md`).

2. **Heap** : +5 à +15 KB de footprint runtime (queues internes,
   callback state, buffers tx/rx séparés). À surveiller : mbedTLS
   réclame déjà 38 KB **contigus** pour la handshake. Une lib qui
   fragmente plus la DRAM peut faire passer `mqttReconnectAttempt()`
   en pré-check fail systématique.

3. **Refactoring** : ~150-200 lignes à réécrire (`mqtt.ino` +
   `mqtt.h` + `routeMessage()`) dans le scénario A,
   significativement plus dans le scénario B parce qu'esp-mqtt change
   le paradigme (event-handler pattern ESP-IDF). Compter 2-3 sessions
   pour A, 4-6 pour B.

4. **Régression sur les paths bien rodés** : la chaîne
   `WiFiClientSecure → setCACert → connect → mbedtls handshake` est
   cadrée après des heures de debug (Bluedroid → NimBLE, pré-check
   heap, rc=-2…). Refaire ce tuning avec une nouvelle lib =
   potentiellement re-débugger les mêmes points. C'est particulièrement
   vrai pour `esp-mqtt` qui n'utilise PAS `WiFiClientSecure` — il a sa
   propre stack TLS (`esp-tls`), donc nouvelle calibration heap +
   gestion du root CA.

5. **Stabilité PubSubClient** : la lib est en maintenance lente parce
   qu'**elle est terminée**. Pas de bug critique connu, API gelée
   depuis 2020. Les alternatives "actively maintained" voient passer
   des breaking changes mineurs entre versions, ce qui complique la
   reproductibilité (et pèse sur le `sketch.yaml` à pinner serré).

6. **Documentation et StackOverflow** : ~10 ans d'historique sur
   PubSubClient. Les libs modernes ont moins de retours d'expérience
   publics, surtout sur les cas tordus (TLS + NimBLE + WiFiManager
   simultanés sur ESP32).

Coût spécifique au scénario B (esp-mqtt) :

7. **Quitter le paradigme Arduino** : esp-mqtt est API ESP-IDF en C,
   event-driven (`esp_mqtt_client_register_event`,
   `esp_mqtt_event_handle_t`, etc.). Le code MQTT du projet devient
   hybride Arduino + IDF, moins consistant. Tu perds l'option
   "n'importe quel hobbyiste Arduino peut lire ce code".

8. **Documentation et examples plus rares** sur les usages mixtes
   Arduino-ESP32 + esp-mqtt (la plupart des exemples Espressif sont
   en pur IDF).

### Recommandation

**Rester sur PubSubClient pour l'instant**, sauf si l'un de ces deux
signaux apparaît :

- **Pertes de messages chat observées** entre devices (typiquement :
  un message envoyé pendant un hiccup WiFi/TLS, jamais reçu en face —
  symptôme classique du QoS 0). À ce moment-là, **scénario A**
  devient justifié → bascule vers **espMqttClient** (actif, le
  meilleur successeur Arduino-friendly de PubSubClient aujourd'hui).

- **Volonté de nettoyer la sémantique MQTT** : payload purement
  utilisateur, plus de trailer pour le self-filter, compatibilité
  totale avec une console web qui tape juste du texte. C'est le
  **scénario B**, et le seul chemin est **`esp-mqtt`** avec MQTT 5.
  Investissement plus important (changement de paradigme), mais c'est
  ce qu'il faut.

Si bascule un jour, classement :

- **`espMqttClient`** : pour QoS 1 + maintenance active, sans MQTT 5.
- **`ArduinoMqttClient`** : option "minimum viable upgrade" Arduino
  officielle, simple, mais maintenance lente.
- **`esp-mqtt`** : la seule porte d'entrée vers MQTT 5 sur ESP32. À
  retenir si tu vises spécifiquement les features MQTT 5 (No Local,
  user properties, will delay) — sinon c'est overkill et invasif.

### Migration incrémentale possible côté broker

HiveMQ Cloud supporte 3.1.1 et 5.0 simultanément sur le même endpoint
TLS. Un device en CONNECT v5 et un device en CONNECT v3.1.1 cohabitent
sur les mêmes topics sans souci — c'est le client qui choisit sa
version à la connexion. Conséquence pratique :

- Tu peux migrer un seul device de test vers esp-mqtt + MQTT 5
  d'abord, les autres restent en PubSubClient (ou en espMqttClient
  3.1.1).
- Le device "v5" lit les messages des "v3.1.1" comme du texte (sans
  user properties → fallback "ext" comme aujourd'hui pour les
  messages externes).
- Les "v3.1.1" lisent les messages du "v5" aussi — le broker leur
  livre le payload sans les properties (qu'ils ignoreraient de toute
  façon).
- Tu flash les autres devices au fil de ta confiance dans la nouvelle
  lib.

Pas de big bang requis — c'est un point fort majeur de MQTT 5 vs
d'autres protocoles.

### Outils pour publier en MQTT 5 manuellement (test / debug)

⚠ Attention : **le client web embarqué dans `console.hivemq.cloud`
(panneau "Web Client") n'expose PAS les controls MQTT 5**. Son
formulaire "Send Message" se limite à Topic / Payload / QoS — pas de
toggle de version protocole, pas de section User Properties. Pour
tester un device MQTT 5, il te faut un outil externe :

#### MQTTX (GUI recommandée, gratuit, multi-plateforme)

<https://mqttx.app/>

Workflow :

1. **New Connection** → Host `<cluster>.s1.eu.hivemq.cloud`, Port
   8883, SSL/TLS ✓, Username/Password tes creds. **MQTT Version : 5.0**
   (le toggle est dans les options avancées de la fenêtre New
   Connection).
2. **Publish** → Topic + Payload + QoS classiques, puis section
   pliable **User Properties** en bas du formulaire : tu cliques "+"
   pour ajouter `senderId=5`, `pseudo=Pac`, etc.
3. **Subscribe** : le panneau de réception déplie les user properties
   reçues dans un sous-panneau, donc tu vois à l'œil nu ce qu'un
   firmware enverrait/lirait.

#### mosquitto_pub / mosquitto_sub (CLI, version ≥ 2.0)

Publish avec user properties :

```bash
mosquitto_pub -h <cluster>.s1.eu.hivemq.cloud -p 8883 \
  --capath /etc/ssl/certs -u <user> -P <pass> \
  -V mqttv5 \
  -D PUBLISH user-property senderId 5 \
  -D PUBLISH user-property pseudo Pac \
  -t msg/broadcast \
  -m "salut tout le monde"
```

Subscribe en imprimant les properties à la réception :

```bash
mosquitto_sub -h <cluster>.s1.eu.hivemq.cloud -p 8883 \
  --capath /etc/ssl/certs -u <user> -P <pass> \
  -V mqttv5 \
  -F '%t | %x | %P' \
  -t 'msg/#'
```

`-V mqttv5` force le CONNECT en version 5. `-D PUBLISH user-property
K V` ajoute une property (répétable). `%P` dans le format de sortie
imprime les properties reçues.

#### Pourquoi pas la web console HiveMQ ?

L'onglet "Web Client" du `console.hivemq.cloud` reste utile pour les
tests basiques en MQTT 3.1.1 (vérifier qu'un device publie, ce que le
broker reçoit, etc.). Pour MQTT 5 il faut payer la version self-hosted
HiveMQ ou utiliser un client externe. C'est une limitation produit, pas
une limite du broker — le broker lui-même gère MQTT 5 sans problème.

## Pour aller plus loin

- Spec MQTT 3.1.1 (PubSubClient l'implémente) :
  <https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html>
- Section officielle QoS :
  <https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718099>
- Section retained messages :
  <https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718038>
- PubSubClient API :
  <https://pubsubclient.knolleary.net/api>
- Console HiveMQ Cloud (gestion des retains) :
  <https://console.hivemq.cloud/>
