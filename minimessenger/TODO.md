# Features à faire


# Prompts à passer plus tard

Mettre en cohérence la status bar et les leds sachant que les leds sont structurées differement:
- une led de status LED_STATUS pour indiquer visuellement quand on est loin du device s'il est opérationnel à 100% ou si il y a un probleme
- une (pour l'instant) led d'ami pour indiquer que son device est connecté au réseau.
Règles:
- LED_STATUS

================


Quel est le cout CPU et memoire d'encoder et décoder des messages MQTT au format json ?


- ================


logs
-encore du nimble

================


autre clavier BT ?



================
const int MAX_LINES = 40;   // nombre max de lignes gardées en mémoire
quid si trop ?

❯ redrawAllConversations ne sert vraiment plus a rien, ni le buffer de MAX_LINES ?
⎿  ⧉ Selected 1 lines from minimessenger.ino in IntelliJ IDEA

Searched for 2 patterns (ctrl+o to expand)

● Vérifié au grep. Tu as raison, tout le truc « ring buffer de TextLine » est mort depuis qu'on est passé en HW scroll.
Voici l'inventaire précis.

Dead code identifié

┌───────────────────────────────────┬─────────────────────────────┬───────────────────────────────────────────────┐
│              Élément              │            Ligne            │                    Statut                     │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ redrawAllConversations() (~70     │ minimessenger.ino:1307      │ Jamais appelée                                │
│ lignes)                           │                             │                                               │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ TextLine lines[MAX_LINES]         │ minimessenger.ino:384       │ Écrit jamais lu (sauf dans                    │
│                                   │                             │ redrawAllConversations morte)                 │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ g_lineHead, g_lineCount           │ minimessenger.ino:385-386   │ Maintenus pour rien — ne servent qu'à indexer │
│                                   │                             │  lines[] qui ne se lit pas                    │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ MAX_LINES = 3                     │ minimessenger.ino:378       │ Ne sert qu'à dimensionner le tableau mort     │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ Boucle while (g_lineCount >=      │ minimessenger.ino:1470-1473 │ Évince l'ancienne ligne d'un tableau qu'on ne │
│ MAX_LINES) {...}                  │                             │  lit pas                                      │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ lines[writeIdx] = TextLine(...)   │ minimessenger.ino:1475      │ Construit un TextLine jeté immédiatement      │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ class TextLine dans display.h     │ display.h:10-50             │ Plus aucune utilité — instancié dans la ligne │
│                                   │                             │  morte ci-dessus uniquement                   │
├───────────────────────────────────┼─────────────────────────────┼───────────────────────────────────────────────┤
│ CONVO_TS_MAX_LEN = 20             │ display.h:7                 │ Ne sert qu'à dimensionner TextLine::ts[]      │
└───────────────────────────────────┴─────────────────────────────┴───────────────────────────────────────────────┘

Ce qui DOIT rester (faux positifs)

- CONVO_MSG_MAX_LEN = 128 (display.h:8) — utilisé aussi à minimessenger.ino:1388 pour dimensionner le buffer local
  char msgBuf[CONVO_MSG_MAX_LEN] (la conversion UTF-8 → Latin-1 avant l'affichage). Donc cette constante reste utile
  indépendamment de TextLine.
- tsBox, msgBox, tsX, msgX, tsBlockHWithMargin, msgBlockHWithMargin — variables locales dans addConversationBlock qui
  calculent les positions de dessin direct dans le framebuffer. Rien à voir avec le ring buffer.
- BOX_X / BOX_Y / BOX_W / BOX_H (minimessenger.ino:291-294) — indices dans les arrays bounds. Utilisés partout.

Pourquoi le ring buffer était nécessaire avant

Le pattern initial était : addConversationBlock mettait juste à jour le ring, puis redrawAllConversations repaintait
tout en parcourant lines[]. La source de vérité était le ring. Avec le HW scroll, on dessine directement dans le
framebuffer du contrôleur ST7789, qui devient à son tour la source de vérité (jusqu'à 320 lignes en mémoire vidéo,
suffisant pour scroller). On n'a plus besoin d'une copie applicative.

Ce qu'on gagnerait à nettoyer

- ~80 lignes de code supprimées dans le .ino (la fonction redrawAllConversations + le ring buffer ops)
- display.h peut disparaître complètement si on déplace CONVO_MSG_MAX_LEN dans le .ino (~50 lignes en moins, un
  include en moins)
- 3 × ~170 octets BSS libérés (3 TextLine pour rien dans le tableau actuel, peanuts)
- Simplification cognitive : plus de double système (ring + framebuffer) à mentaliser

Coût de garder

- Zéro fonctionnel — c'est juste du code mort qui compile
- Petite confusion possible pour quelqu'un qui découvre le code et se demande à quoi sert le ring
- Une "réserve" si tu voulais un jour réintroduire un mode où on doit reconstruire l'écran depuis l'historique (theme
  switch, re-render après orientation change…). Mais à ce moment-là tu repartirais probablement d'une stratégie
  différente, donc ce n'est pas vraiment un investissement.

Recommandation

Si tu veux que je nettoie, je peux faire en une passe :
1. Supprimer redrawAllConversations()
2. Supprimer lines[], g_lineHead, g_lineCount, MAX_LINES, la boucle drop-oldest et la construction de TextLine dans
   addConversationBlock
3. Migrer CONVO_MSG_MAX_LEN de display.h vers le .ino
4. Supprimer display.h et son #include

Dis-moi go/no-go.

=================