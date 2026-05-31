# JSON pour les payloads MQTT — analyse, alternatives, exemples

## Contexte

Aujourd'hui le projet sérialise les messages MQTT avec un **trailer texte** ajouté en fin de payload par `mqttPushFormattedMessage()`:

```
hello world ### ts:2026-05-30|22:19:48 deviceId:4 msgId:155
```

Le corps du message reste lisible humainement, et les méta-données sont parsées côté réception par `strstr` + `strtol`. Format pragmatique, zéro dépendance, adapté à un schéma figé (5 champs).

Cette note compare ce format à JSON et à des intermédiaires, mesure le coût réel sur ESP32, et propose des exemples de migration si le besoin se présente.

---

## Coût mesuré (ESP32 @ 240 MHz, payloads ~100-200 octets, 5 champs)

### CPU par opération

| Opération | Format `###` actuel | ArduinoJson v7 | cJSON (IDF) |
|-----------|----------------------|-----------------|-------------|
| Sérialisation (100 octets) | ~10-30 µs (`snprintf`) | 80-200 µs | 150-400 µs |
| Désérialisation | ~20-50 µs (`sscanf`/`strstr`) | 150-500 µs | 300-800 µs |
| Ratio vs actuel | **1×** | **3-10×** | **8-20×** |

À 4 msg/min (rythme actuel: keepalive broadcast + traffic utilisateur), le CPU est invisible — quelques ms par seconde au pire. Le CPU n'est pas le facteur limitant.

### Flash (code embarqué une fois linké)

- **Format `###`**: 0 octet de code dédié (juste `snprintf`/`strstr`)
- **ArduinoJson v7**: ~10-15 KB
- **cJSON** (livré avec ESP-IDF): ~5-8 KB

Sur la partition "Huge App 1.9 MB", c'est négligeable dans les deux cas.

### Heap par message

- **Format `###`**: **0 alloc**, manipulation directe de pointeurs sur le buffer MQTT déjà en mémoire
- **ArduinoJson v7** mode `JsonDocument` stack-allocated: **0 alloc heap**, ~256 octets de stack
- **ArduinoJson v7** mode dynamique: 256-768 octets de heap par message, libérés à la sortie du scope
- **cJSON**: 256-768 octets répartis en **plusieurs petits chunks** (chaque clé, chaque valeur = un malloc) → **fragmentation bien plus rapide**

### Le vrai enjeu sur ce projet: la fragmentation heap

Le CLAUDE.md documente déjà:

> Fix memory fragmentation + migration Bluedroid → NimBLE pour libérer ~50 KB heap parce que mbedtls TLS handshake en demande ~38 KB contigus.

Le heap est tendu. Chaque malloc/free de 256-768 octets par message, sur le long terme, fragmente — surtout mélangé avec PubSubClient, NimBLE, mbedtls, et les Strings Arduino. À 4 msg/min c'est gérable. À 60 msg/min (chat actif multi-user), tu peux voir des problèmes après quelques heures de uptime.

---

## Faut-il passer à JSON ?

### Garder le format `###`

- ✅ Schéma fixe (5 champs: body, ts, deviceId, msgId, recipientId implicite via topic)
- ✅ Lisible côté `mosquitto_sub` sans tooling
- ✅ Zéro alloc heap
- ✅ Code de parse trivialement debuggable
- ❌ Évolution du schéma douloureuse (chaque champ optionnel = cas particulier dans le parse)
- ❌ Pas de consommateur tiers possible sans connaître le format custom

### Passer à JSON

Tu y passes **quand** au moins un de ces points devient vrai:

1. Le schéma évolue — ajout de champs (typing indicators, ack délivrance, attachments, signatures), messages structurés (sondages, listes), métadonnées variables selon le type de message
2. Un client tiers consomme tes topics — dashboard web, app mobile, bridge MQTT → autre système
3. Tu ajoutes ≥ 3-4 nouveaux champs et le trailer devient illisible

Tant qu'aucun n'est vrai, garde `###` — la complexité ajoutée ne se rentabilise pas.

### Intermédiaire: JSON à la main

Quand tu veux du JSON-lisible côté wire, mais sans embarquer ArduinoJson, tu fabriques le payload avec `snprintf` au format `{"body":"%s","deviceId":%d,...}` côté sortie, et tu parses avec `strstr` + `strtol` côté entrée. Marche bien pour la sérialisation, moche pour le parse (chaque champ = un `strstr` puis un parsing manuel), 0 dépendance.

---

## Exemples de sérialisation / désérialisation

### A. Format actuel `###` trailer

Le code existant dans `mqttPushFormattedMessage()` (extrait simplifié):

```cpp
char trailer[64];
snprintf(trailer, sizeof(trailer), " ### ts:%s deviceId:%d msgId:%u",
         getCurrentTime().c_str(), g_deviceIdMe, g_mqttOutputMsgId);

char payload[256];
snprintf(payload, sizeof(payload), "%s%s", body.c_str(), trailer);

g_mqttClient.publish(topic, (const uint8_t*) payload, strlen(payload), false);
g_mqttOutputMsgId++;
```

Parse côté réception (extrait):

```cpp
// payload = "hello world ### ts:2026-05-30|22:19:48 deviceId:4 msgId:155"
const char* sep = strstr(payload, " ### ");
if (sep) {
  *((char*) sep) = '\0';                      // tronque le corps
  const char* meta = sep + 5;                 // après " ### "

  const char* tsKey = strstr(meta, "ts:");
  const char* idKey = strstr(meta, "deviceId:");
  // ... etc, extraction par strstr + strtol
}
```

### B. ArduinoJson v7 — recommandé si on migre

**Installation**: Library Manager → "ArduinoJson" (Benoît Blanchon), v7.x.

#### Sérialisation

```cpp
#include <ArduinoJson.h>

void mqttPublishJson(const char* topic, const String& body) {
  JsonDocument doc;                    // v7: stack-allocated par défaut, pas de capacité à pré-réserver
  doc["body"]     = body;
  doc["ts"]       = getCurrentTime();
  doc["deviceId"] = g_deviceIdMe;
  doc["msgId"]    = g_mqttOutputMsgId;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));   // retourne le nombre d'octets écrits
  if (n == 0 || n >= sizeof(buf)) {
    ESP_LOGE(TAG_MQTT, "JSON serialize failed or overflow (n=%u)", (unsigned) n);
    return;
  }

  g_mqttClient.publish(topic, (const uint8_t*) buf, n, false);
  g_mqttOutputMsgId++;
}
```

Wire output:
```json
{"body":"hello","ts":"2026-05-30|22:19:48","deviceId":4,"msgId":155}
```

#### Désérialisation

```cpp
void onMqttIncomingMessage(char* topic, uint8_t* payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    ESP_LOGW(TAG_MQTT, "JSON parse failed: %s — falling back to raw", err.c_str());
    // fallback: traiter comme texte brut si besoin
    return;
  }

  // Accès par clé. Les conversions sont implicites et safe (renvoient une valeur par défaut si la clé manque).
  const char* body     = doc["body"]     | "";        // | "default" = fallback si null/absent
  const char* ts       = doc["ts"]       | "";
  int         deviceId = doc["deviceId"] | -1;
  uint32_t    msgId    = doc["msgId"]    | 0;

  ESP_LOGI(TAG_MQTT, "Received from #%d (msgId=%u): [%s]", deviceId, msgId, body);
  // ... addConversationBlock(...) etc.
}
```

#### Notes ArduinoJson v7

- `JsonDocument` en v7 n'a plus de capacité statique à déclarer — la lib gère un buffer interne croissant (mode "stack-friendly" par défaut tant que le doc reste petit).
- L'opérateur `|` est le pattern v6+/v7 pour fallback: `doc["x"] | default_value`. Evite de tester `containsKey()` ou `isNull()` pour les types simples.
- `serializeJson(doc, char* buf, size_t bufsize)` est non-allouant et borné. Toujours vérifier que `n < sizeof(buf)`.
- Pour produire du JSON **pretty-printed** (debug humain), utiliser `serializeJsonPretty()` à la place — mais le wire MQTT n'en a pas besoin.

### C. JSON à la main (sans ArduinoJson)

#### Sérialisation

```cpp
void mqttPublishJsonManual(const char* topic, const String& body) {
  // Attention à échapper les '"' et '\\' si body peut en contenir. Cf. fonction utilitaire jsonEscape() ci-dessous.
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"body\":\"%s\",\"ts\":\"%s\",\"deviceId\":%d,\"msgId\":%u}",
    body.c_str(),
    getCurrentTime().c_str(),
    g_deviceIdMe,
    g_mqttOutputMsgId);

  if (n < 0 || n >= (int) sizeof(buf)) {
    ESP_LOGE(TAG_MQTT, "JSON-manual serialize overflow (n=%d)", n);
    return;
  }

  g_mqttClient.publish(topic, (const uint8_t*) buf, n, false);
  g_mqttOutputMsgId++;
}
```

**Échappement minimal** des `"` et `\` dans le corps (sinon tu peux casser le JSON):

```cpp
// Copie src dans dst en échappant ", \, et les contrôles. Renvoie le nombre d'octets écrits (sans le \0 final).
size_t jsonEscape(char* dst, size_t dstSize, const char* src) {
  size_t out = 0;
  for (size_t i = 0; src[i] && out + 2 < dstSize; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      dst[out++] = '\\';
      dst[out++] = c;
    } else if ((unsigned char) c < 0x20) {
      // Caractère de contrôle → \uXXXX. Coûte 6 octets.
      if (out + 6 >= dstSize) break;
      snprintf(dst + out, dstSize - out, "\\u%04x", c);
      out += 6;
    } else {
      dst[out++] = c;
    }
  }
  dst[out] = '\0';
  return out;
}
```

#### Désérialisation (parse manuel pour un schéma figé)

```cpp
// Recherche "key":"value" ou "key":number dans le JSON. Renvoie un pointeur vers la valeur, ou nullptr.
const char* findJsonValueAfter(const char* json, const char* keyWithQuotes) {
  // keyWithQuotes doit inclure les guillemets autour de la clé, ex: "\"body\":"
  const char* p = strstr(json, keyWithQuotes);
  if (!p) return nullptr;
  return p + strlen(keyWithQuotes);
}

void onMqttIncomingMessageManual(char* topic, uint8_t* payload, unsigned int length) {
  // payload n'est pas garanti NUL-terminated — on copie dans un buffer.
  char json[256];
  size_t n = length < sizeof(json) - 1 ? length : sizeof(json) - 1;
  memcpy(json, payload, n);
  json[n] = '\0';

  // Body (string)
  char body[128] = {0};
  const char* p = findJsonValueAfter(json, "\"body\":");
  if (p && *p == '"') {
    p++;                                 // après la "
    const char* end = strchr(p, '"');    // attention: ne gère pas les échappements internes !
    if (end) {
      size_t len = end - p;
      if (len >= sizeof(body)) len = sizeof(body) - 1;
      memcpy(body, p, len);
      body[len] = '\0';
    }
  }

  // deviceId (int)
  int deviceId = -1;
  p = findJsonValueAfter(json, "\"deviceId\":");
  if (p) deviceId = strtol(p, nullptr, 10);

  // msgId (uint32)
  uint32_t msgId = 0;
  p = findJsonValueAfter(json, "\"msgId\":");
  if (p) msgId = strtoul(p, nullptr, 10);

  ESP_LOGI(TAG_MQTT, "Received from #%d (msgId=%u): [%s]", deviceId, msgId, body);
}
```

**Limites du parse manuel**:
- Ne gère pas correctement les `\"` échappés dans la valeur (le `strchr(p, '"')` s'arrêterait dessus)
- Ne gère pas les espaces autour des `:` ou les nested objects
- Suffisant pour un schéma 100% sous ton contrôle, à éviter dès qu'un producteur tiers peut envoyer

---

## Recommandation pour minimessenger aujourd'hui

1. **Garder le format `###`** tant que le schéma reste 5 champs fixes et que personne d'externe ne consomme les topics. C'est ce qui est déployé, ça marche, ça ne fragmente pas, ça reste lisible.
2. **Si on ajoute 3-4 nouveaux champs** (typing indicators, attachments, ack délivrance), passer directement à **ArduinoJson v7** en mode stack-document. La migration coûte ~10 KB de flash, négligeable, et la dette technique du parse manuel disparaît.
3. **Ne pas faire l'étape intermédiaire "JSON à la main"** sauf pour un prototype court-termiste. Le coût en bugs (échappement, edge cases) dépasse rapidement le coût d'ArduinoJson.
4. **Ne pas utiliser cJSON** sur ce projet — la fragmentation heap (un malloc par clé et par valeur) est incompatible avec la contrainte mbedtls TLS handshake.

## Liens

- ArduinoJson v7 docs: https://arduinojson.org/v7/
- Benchmark ArduinoJson vs cJSON vs rapidjson: https://arduinojson.org/v7/news/2024/02/07/announcing-version-7/
- ESP-IDF cJSON (si jamais on en a besoin pour interop): https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/cjson.html
