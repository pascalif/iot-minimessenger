# Features à faire

Physique:
-allumé
- probleme
- contact 1+
-
-
- bouton reset ?
- bouton reveil ?
- bouton bonus ?

- ecran test fonts sizes


## Pour une éventuelle V2 si 3+ devices sont fabriqués:
- pas d'accents (ï) sur pseudo (petite font par defaut non générée avec latin1) : changer de font
- selectionner le destinataire pour faire de l'unicast
- sketch.yaml dans le projet (fix les libs et indique la partition)


# Features à modifier



=============

section debug avec tous les logs

147 +    g_wifiManager.setDebugOutput(false);

Si tu chasses une instabilité actuelle, je regarderais d'abord :
1. Quelle est la nature de l'instabilité ? reboot ESP, freeze écran, MQTT qui décroche, BLE qui se déconnecte, ESP qui panic ? Le profil de symptôme oriente la cause.
2. Active esp_log_set_level("*", ESP_LOG_VERBOSE) un moment et regarde le dernier log avant l'événement.
3. Active CONFIG_HEAP_POISONING_COMPREHENSIVE (menuconfig / sdkconfig) pour détecter les use-after-free.


    return g_kb.setup(false,  // clear bonds — flip to true once if a stale bond is


================

Extraire fichiers de constantes

================

❯ a quoi sert dans loop() le timer pour faire un redrawStatusBar ?  N'est ce pas dessiné à la demande qd un toggle change ou qu'on revient sur l'écran de conversations?
⎿  ⧉ Selected 1 lines from minimessenger.ino in IntelliJ IDEA

Read 1 file (ctrl+o to expand)

● Bonne question — réponse honnête : aujourd'hui ce timer est nécessaire parce que les changements d'état réseau/BT ne lèvent aucun flag dirty. Le polling sert de filet :

- WiFi : WiFi.status() peut passer de WL_CONNECTED à autre chose à tout moment (perte/reconnexion auto par le driver ESP), et aucun WiFi.onEvent() n'est enregistré dans
  le sketch → on ne saurait pas qu'il faut repeindre sans relire l'état.
- MQTT : PubSubClient::connected() est un getter, pas un callback. Une déconnexion réseau passive (broker timeout, TCP RST) est détectée par PubSubClient en interne mais
  n'appelle rien chez nous.
- BT : NimBLE expose bien onConnect/onDisconnect dans mm_blekb.cpp, mais ces callbacks ne touchent pas g_statusBarDirty aujourd'hui.
- CapsLock : maintenant 100 % event-driven (le toggle qu'on vient d'ajouter dirty le flag). Pas besoin du polling pour celui-là.
- Retour à l'écran conversation : déjà couvert — hwScrollReset() met g_statusBarDirty = true.

Donc oui, en principe on pourrait supprimer le timer si on poussait g_statusBarDirty = true aux trois endroits manquants :
1. Dans les callbacks NimBLE onConnect/onDisconnect du clavier (mm_blekb.cpp).
2. Aux points où MQTT (re)connecte/échoue (autour de mqttReconnect / dans onMqttIncomingMessage quand "dis" déclenche un disconnect).
3. Enregistrer un WiFi.onEvent() pour ARDUINO_EVENT_WIFI_STA_GOT_IP / ..._DISCONNECTED.

Tant que ces trois branchements n'existent pas, le polling 500 ms est le seul mécanisme qui rattrape les pertes silencieuses. Et comme redrawStatusBar court-circuite
immédiatement quand rien n'a changé, le coût est négligeable (4 lectures de bool + comparaisons toutes les 500 ms).

Tu veux qu'on bascule en full event-driven en branchant les 3 endroits ci-dessus, ou on laisse le polling tel quel pour la robustesse ?

=================


# Tools à suivre

## .clang-format 1

Une option désactivé car clang-format pas encore compatible


## .clang-format 2

REgarde ce commentaire reformaté:
#define KEY_ERR_OVF                                                                                                                                            \
      0x01  //  Keyboard Error Roll Over - used for all slots if too many keys are                                                                               \
            //  pressed ("Phantom key")

Comment empecher l'ajout de "\" en fin de ligne qd c'est un commentaire ?

Claude responded: C'est un bug connu de clang-format : il traite les commentaires dans un #define multiligne comme du code
et ajoute des \ inutiles.C'est un bug connu de clang-format : il traite les commentaires dans un #define multiligne comme du code et ajoute des \ inutiles.

La solution est d'ajouter dans ton .clang-format :
yamlAlignEscapedNewlines: DontAlign

Mais ça ne résout pas totalement le problème des commentaires dans les macros. La vraie solution est de mettre le commentaire avant ou après le #define :
cpp// Keyboard Error Roll Over - used for all slots if too many keys are
// pressed ("Phantom key")
#define KEY_ERR_OVF 0x01

Clang-format n'a pas d'option pour distinguer un commentaire de fin de ligne dans une macro — c'est une limitation connue et non résolue du tool.
