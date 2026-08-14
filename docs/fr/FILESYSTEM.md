# Emplacements des fichiers : config, état, programme

Ce document fixe la doctrine du parc morfSystem sur la séparation entre
configuration, état persistant et programme. Il sert de référence à tous les
services clonés depuis ce modèle.

## Le principe

Un service range ses fichiers dans trois zones distinctes, jamais mélangées.

| Zone | Rôle | Emplacement (Linux) | Propriétaire | Écriture par le service |
|------|------|---------------------|--------------|-------------------------|
| Programme | binaire + assets embarqués | `/opt/<service>` | user du service | non |
| Config admin | réglages posés par l'administrateur, référence durable | `/etc/morfsystem/<service>` | `root` | non |
| État persistant | données générées par le service | `/var/lib/morfsystem/<service>` | user du service | oui |

Les journaux passent par journald (`StandardOutput=journal`) : pas de fichiers
sous `/var/log`.

Sous Windows, tout se replie sous `%ProgramData%` avec la même logique :
`%ProgramData%\<service>` pour le programme, `%ProgramData%\morfsystem\<service>`
pour la config, `%ProgramData%\morfsystem\<service>\state` pour l'état.

## La règle de tri : config ou état ?

> Ce qu'un administrateur pose et qu'on peut perdre à la réinstallation sans
> rien casser relève de la **configuration**. Ce que le service génère lui-même
> et dont la perte casse une continuité relève de l'**état**.

Litmus qui tranche les cas limites : un secret, une clé, une séquence monotone,
un curseur de collecte, un index de déduplication, un UUID généré, une date de
dernière exécution sont **toujours de l'état**. Jamais de la configuration.

Mettre de l'état dans `/etc` est l'erreur à éviter. C'est ce qui rendait le
coffre de morfCollector inécrivable : `vault.key` et `vault.enc` (de l'état
généré) étaient placés dans `/etc/morfsystem/morfcollector`, dossier appartenant
à `root`. Le service, qui tourne sous son propre utilisateur, ne pouvait pas y
créer sa clé ; toutes les sources restaient bloquées en `auth_failed`.

## Un service doit-il modifier sa configuration ?

Par défaut, non. Trois cas distincts, à ne pas confondre.

1. **Config admin immuable** (port, adresse d'écoute, cible, planification par
   défaut). Elle vit dans `/etc`, en lecture seule. Le service ne la réécrit
   jamais.

2. **État runtime** (coffre, curseurs, séquences de synchronisation, index,
   identifiants générés). Il vit dans `/var/lib`. C'est la seule zone où le
   service écrit.

3. **Préférence modifiable à l'exécution** (par exemple un réglage changé depuis
   une interface web). C'est le seul cas où le service « change sa config ».
   Approche retenue : la **superposition**.

## La superposition pour les préférences éditables

La config admin de `/etc` reste la base, en lecture seule. Les préférences
modifiées à l'exécution sont écrites dans un fichier séparé de l'état
(`<service>.overrides.json` sous la racine d'état) et appliquées **par-dessus**
au chargement.

```
config effective = config admin (/etc)  puis  surcharges (/var/lib) par-dessus
```

Ce choix garde `/etc` comme référence administrateur, cantonne toutes les
écritures à la zone accessible en écriture, et rend les opérations propres :

- **Sauvegarde** : sauvegarder `/var/lib` capture tout ce qui est vivant.
- **Réinstallation** : effacer `/opt` et recopier le binaire ne touche ni la
  config ni l'état.
- **Droits** : le service n'a jamais besoin d'écrire dans `/etc`, ce qui
  éviterait de lui donner des droits qui affaibliraient la référence admin.

Ne pas rendre `/etc` accessible en écriture au service, et ne pas déplacer la
config admin vers `/var/lib` : la superposition évite d'avoir à choisir entre
les deux.

## Le mécanisme : StateDirectory de systemd

L'unité systemd déclare :

```ini
[Service]
User=<user>
StateDirectory=morfsystem/<service>
```

`StateDirectory=` fait créer `/var/lib/morfsystem/<service>` par systemd, possédé
par le `User=` du service et avec le bon mode, puis l'expose au processus via la
variable d'environnement `$STATE_DIRECTORY`. Le problème de droits disparaît par
construction, au lieu d'être rattrapé après coup par un `chown`.

Le service résout sa racine d'état ainsi (voir `Paths::stateDir`) :

1. `$STATE_DIRECTORY` s'il est défini (cas systemd) ;
2. sinon, repli conforme à l'OS : `/var/lib/morfsystem/<service>` (Linux) ou
   `%ProgramData%\morfsystem\<service>\state` (Windows).

Le repli couvre l'exécution manuelle (hors service) et Windows, où il n'y a pas
de systemd. Dans les deux cas, le dossier est créé et doit être accessible en
écriture au compte courant, à la différence de `/etc`.

## Côté déploiement (morfdeploy)

Le manifeste `service.json` déclare les trois emplacements par plateforme
(`app_dir`, `config_dir`, `state_dir`). `morfdeploy` :

- copie le binaire dans `app_dir` et la config dans `config_dir` (référence
  admin, jamais `chown` vers le user) ;
- laisse systemd créer et posséder le dossier d'état via `StateDirectory=` ;
- substitue au besoin `__STATE_DIR__` dans l'unité (pour un service qui passe sa
  racine d'état en argument plutôt que de lire `$STATE_DIRECTORY`) ;
- affiche les trois chemins au moment de l'installation.

## Résumé pour un nouveau service

- Réglages posés à la main : `/etc/morfsystem/<service>`, lecture seule.
- Tout ce que le service génère : racine `$STATE_DIRECTORY`
  (`/var/lib/morfsystem/<service>`).
- Préférences éditables : superposées depuis
  `<service>.overrides.json` dans l'état.
- Déclarer `StateDirectory=morfsystem/<service>` dans l'unité et `state_dir`
  dans `service.json`.
- Ne jamais écrire dans `/etc`. Ne jamais mettre d'état dans `/opt` ou `/etc`.
