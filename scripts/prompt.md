Merge toutes les directives données pour écrire ce script python et son fichier de configuration et redonne les moi de facon cohérente et comprehensive sous forme d'un prompte à donner à une IA


ton prompt est trop détaillé. Je ne veux pas les détails techniques propres à python (hormis la répartition en 2 fichiers), pas de librairies, pas de point d'implem. Je veux juste les specs fonctionnelles.


===============

Écris un script Python de chat MQTT avec son fichier de configuration séparé config.py.

Configuration (config.py)
Externaliser dans ce fichier : host, port, user, password du broker MQTT, et l'identifiant de l'appareil local (DEVICE_ID_ME = 1). Le broker est HiveMQ cloud, connexion TLS sur port 8883.

Comportement général
Le script tourne jusqu'à Ctrl-C. Il se termine immédiatement avec un message d'erreur lisible si la connexion MQTT échoue (réseau inaccessible, timeout, identifiants incorrects, etc.).

Format des messages reçus
<txt> [### [...] [deviceId:<id>] [...]]
Le trailer ### est facultatif, et deviceId l'est aussi à l'intérieur. Si l'identifiant est absent ou non décodable, afficher unk.

Topics écoutés
msg/broadcast — afficher en jaune <id> - <heure> : <txt>, sauf si le message provient de moi-même (même deviceId), auquel cas l'ignorer silencieusement.
msg/unicast/<DEVICE_ID_ME> — afficher en orange <id> - <heure> : <txt>.
admin/liveness/<id> — payload <status> <ts> — afficher en rose <id> - <ts> : <status>. Une constante SHOW_LIVE_STATUS (true/false) contrôle l'affichage des lignes dont le status vaut LIVE.

Envoi de messages
La dernière ligne de l'écran est réservée à la saisie. Sur appui d'Entrée, le message est publié sur msg/broadcast avec le format ci-dessus (trailer incluant deviceId et timestamp), affiché en blanc dans la zone de messages, et la zone de saisie est vidée.

Interface
Terminal en mode plein écran. Les messages reçus et envoyés s'accumulent du haut vers le bas, les plus récents en bas, au-dessus de la ligne de saisie.