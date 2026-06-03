# Howto — MQTT in minimessenger

> Vue d'ensemble de la pile MQTT côté firmware : QoS (vraiment, surtout
> publish-QoS vs subscribe-QoS), messages **retained** et leur effet sur
> le boot, et clean session. Pour la topologie (qui publie où, quels
> topics, quels callbacks) → voir la section *MQTT topology* du
> `CLAUDE.md` à la racine du projet, et le code dans `mqtt.ino`.

## TL;DR

- **PubSubClient** (Library Manager 2.8) ne sait publier qu'en **QoS 0**
  — peu importe le drapeau que tu mets. Il sait s'abonner en QoS 0 ou
  QoS 1, **mais pas QoS 2**.
- En MQTT, la QoS effective d'une délivrance vaut
  **`min(publish_QoS, subscribe_QoS)`**. S'abonner en QoS 1 à un sujet
  où l'autre côté publie en QoS 0 ne fait rien gagner.
- Un message marqué **retained** est stocké côté broker pour ce topic et
  re-livré à **tout** nouvel abonné, y compris ce même device après un
  reboot. C'est pour ça que les messages de chat réapparaissent à chaque
  démarrage — voir `mqttPushFormattedMessage()` dans `mqtt.ino` qui
  passe actuellement `MQTT_MSG_RETAINED` pour **tous** les topics.
- Le fix : ne retenir que ce dont la sémantique l'exige (`admin/live`),
  pas les messages de conversation.

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
g_mqttClient.subscribe(g_mqttIncomingTopicBroadcast, MQTT_QOS_1);
g_mqttClient.subscribe(myUnicastTopic.c_str(),       MQTT_QOS_1);
g_mqttClient.subscribe(g_mqttOutgoingTopicLive,      MQTT_QOS_0);
g_mqttClient.subscribe(g_mqttOutgoingTopicWill,      MQTT_QOS_0);
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

C'est probablement le comportement le plus surprenant de MQTT pour qui
vient d'autres files de messages.

### Définition

Un message publié avec le flag **retained = true** est **stocké côté
broker** pour ce topic exact. Il y reste jusqu'à ce qu'un autre message
retained écrase le précédent sur le même topic (ou qu'un publish d'un
payload vide avec retained=true le supprime). Et **chaque** nouvel
abonné à ce topic reçoit immédiatement ce dernier message retenu, en
plus des messages futurs.

C'est conçu pour exposer un **état**, pas un **événement** :

- ✅ Bon usage de retained : "device 4 est en ligne", "la température est
  à 21 °C", "le mode est `night`". Un nouvel abonné a besoin de
  connaître l'état actuel sans attendre la prochaine émission.
- ❌ Mauvais usage de retained : "Bob a dit hello", "le bouton a été
  pressé", n'importe quel **événement** qui n'a de sens qu'au moment
  où il se produit.

### Pourquoi le projet rejoue les messages au boot

`mqttPushFormattedMessage()` dans `mqtt.ino` publie **tout** avec
`MQTT_MSG_RETAINED = true` :

```cpp
bool ok = g_mqttClient.publish(topic, g_mqttOutgoingMsg, MQTT_MSG_RETAINED);
```

Résultat :

1. Bob envoie "salut" sur `msg/broadcast` — publié retained.
2. Le broker mémorise ce "salut" comme dernier message retenu sur
   `msg/broadcast`.
3. Alice reboot. Au reconnect, elle re-souscrit à `msg/broadcast`. Le
   broker la salue avec le "salut" mémorisé — qu'elle a déjà reçu et
   affiché.
4. Pareil sur `msg/unicast/<aliceId>` s'il y a un message ciblé.
5. Conclusion : chaque boot, la dernière ligne de conversation
   ressurgit. Sur de multiples reconnects (perte WiFi, /mqtt-drop, …)
   c'est encore pire.

### Le fix : retain par topic, pas par défaut

La sémantique projet par projet :

| Topic                  | Devrait être retained ? | Pourquoi |
|------------------------|-------------------------|----------|
| `admin/live`           | ✅ **Oui**              | C'est un **état** "device X est en ligne". Un device qui boot a besoin de connaître les peers en ligne sans attendre 2 min de keepalive. |
| `admin/dead` (Will)    | Géré par `connect()`    | Le retain de la Last Will est piloté par le 6ᵉ argument de `g_mqttClient.connect(...)` — déjà à `MQTT_MSG_NOT_RETAINED` dans `mqttReconnectAttempt()`. Ne pas retenir : un device "mort" n'est mort qu'au moment où le broker détecte la déco, pas indéfiniment. |
| `msg/broadcast`        | ❌ **Non**              | Événement (un message de chat). Si Alice reboot 3 jours après le dernier "salut" de Bob, elle n'a pas à revoir "salut". |
| `msg/unicast/<id>`     | ❌ **Non**              | Idem — événement, pas état. |
| `admin/logs`           | ❌ **Non**              | Événement aussi, si on s'en sert un jour. |

### Code minimal à changer

`mqttPushFormattedMessage()` doit prendre le drapeau retain en
paramètre, et les callers le fournissent selon la sémantique du topic
qu'ils ciblent :

```cpp
// Dans mqtt.h
bool mqttPushFormattedMessage(const char* topic, const char* payload, bool retained);

// Dans mqtt.ino
bool mqttPushFormattedMessage(const char* topic, const char* payload, bool retained) {
    snprintf(g_mqttOutgoingMsg, MSG_BUFFER_SIZE,
             "%s ### ts:%s deviceId:%d msgId:%d",
             payload, getCurrentDateTime(), g_deviceData.deviceId, g_mqttOutputMsgId);
    bool ok = g_mqttClient.publish(topic, g_mqttOutgoingMsg, retained);
    // … logs identiques
}

// Callers :
//   mqttSendAlive()    →  mqttPushFormattedMessage(g_mqttOutgoingTopicLive, payload, MQTT_MSG_RETAINED);
//   routeMessage()     →  mqttPushFormattedMessage(g_mqttOutoingRecipientTopic, message.c_str(), MQTT_MSG_NOT_RETAINED);
//   broadcast variants →  MQTT_MSG_NOT_RETAINED
```

### Purger les retains existants côté broker

Une fois le code corrigé, le broker garde **toujours** le dernier
message retenu publié par l'ancien code. Pour les effacer, il faut
publier un payload vide retained sur chaque topic concerné :

```bash
# avec mosquitto_pub depuis n'importe quelle machine sur le réseau
mosquitto_pub -h xxxxxx.s1.eu.hivemq.cloud -p 8883 \
  --capath /etc/ssl/certs -u xxxxx -P xxxxxxx \
  -t 'msg/broadcast' -r -n
mosquitto_pub … -t 'msg/unicast/1' -r -n
mosquitto_pub … -t 'msg/unicast/2' -r -n
# … un par deviceId connu
```

`-r` = retained, `-n` = null/empty message. Effet : "publish un message
retained vide" → le broker supprime l'entrée retenue sur ce topic.

Alternative : la console web HiveMQ Cloud expose un "Clear retained
message" par topic.

### Stratégies alternatives (si on tenait à garder retained=true)

Pour mémoire — pas recommandées ici, mais utiles à connaître :

1. **Tracking de `msgId` en NVS.** Chaque payload contient
   `msgId:<n>` dans le trailer. Le receiver mémorise `(deviceId →
   dernier msgId vu)` en NVS et ignore les payloads dont le `msgId`
   est ≤ au dernier vu. Coût : une écriture NVS par message entrant.
2. **Filtre par horodatage.** Le trailer contient `ts:YYYY-MM-DD
   HH:MM:SS`. Au boot, on dropne tout message dont `ts` est antérieur
   à une marge (ex : `now() - 30 s`). Coût : zéro stockage, mais
   nécessite que NTP soit synchro **avant** que le callback MQTT ne
   commence à délivrer — pas garanti dans l'ordre actuel
   `setupWifi()` → MQTT → NTP.
3. **Clean session = `false`.** Le broker conserve les messages QoS ≥ 1
   manqués pendant que le client était déconnecté, et les rejoue **une
   fois**, à la reconnexion suivante. Mais comme PubSubClient publie en
   QoS 0, la queue est toujours vide → cette option ne change rien
   tant qu'on reste sur cette lib.

La fix par drapeau `retained` reste de loin la plus propre : c'est ce
que MQTT a prévu pour distinguer état et événement, autant l'utiliser
comme tel.

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
