---
name: feedback-static-naming
description: "Convention `gs_` réservée aux variables `static` *fonction-locales* (cachées dans une méthode mais consommant de la .bss), pour qu'un grep `gs_` retrouve cet état persistant invisible"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 4082b9c2-cb98-45e3-930c-1bcca99a4e93
---

Toute variable C/C++ déclarée avec le mot-clé `static` **à l'intérieur d'une fonction ou méthode** doit être préfixée **`gs_`** — par exemple `static char gs_buf[20];` dans `getCurrentDateTime()`, ou `static uint8_t gs_prevKeys[6];` dans une fonction de lecture HID. La règle s'applique uniquement aux statics *fonction-locales*, pas aux file-scope statics.

**Why:** ce sont exactement les variables qui consomment de la `.bss` (durée de vie programme, état persistant entre appels) tout en étant **invisibles à un audit naïf** — pas de déclaration au top-level d'un fichier, pas de symbole exporté, pas visibles via `grep "^[a-zA-Z].*=.*;"` sur les fichiers. Elles sont aussi un foyer historique de bugs subtils dans ce code : cf. `g_ts[20]` partagé entre `getCurrentTime()` et `getCurrentDateTime()` (race condition résolue en passant les buffers en statics locaux). Le préfixe `gs_` rend ces piégeuses traçables : `grep -rn "gs_" minimessenger/` liste l'intégralité de cet état caché — combien d'octets de `.bss` sont planqués dans les corps de fonctions, où sont les buffers partagés entre appels, etc.

**How to apply:**
- À la création d'un `static` **fonction-locale** (buffer style libc retourné par pointeur, état persistant entre appels, accumulateur, latch) → préfixer `gs_`.
- À la création d'un `static` **file-scope** (déclaration au top-level d'un `.cpp`/`.ino`, ex. `static ContactLastLiveData g_contacts[…];`) → garder le préfixe `g_` classique des globales, **PAS** `gs_`. Une file-scope static a les mêmes propriétés mémoire qu'une globale (durée de vie programme, en `.bss`) ; le `static` ne fait que limiter le linkage à la TU. Conceptuellement c'est une globale interne, donc `g_`.
- Constantes immuables `static const`/`static constexpr` (ex. `HIVEMQ_ROOT_CA`, `splash_bmp`) → exclues, conservent leur propre convention (SCREAMING_SNAKE_CASE ou snake_case selon le cas).
- Statiques fonction-locales existantes à renommer opportunément (audit du 2026-06-04) : `minimessenger.ino:679` `s_prevKeys` → `gs_prevKeys` ; `minimessenger.ino:863` `s_dynamicMacBuf` → `gs_dynamicMacBuf` ; `contacts.h:32` `buf` (dans `DeviceDataEntry::name()`) → `gs_buf`.
