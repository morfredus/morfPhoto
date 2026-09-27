# Roadmap - morfPhoto

morfPhoto a une seule responsabilité : être la source de vérité des métadonnées
d'une photothèque locale (EXIF brut, présence des fichiers, contexte photographique
par dossier) et les servir en HTTP. Cette feuille de route se limite à ce périmètre.

## Pistes

Aucune piste propre au service n'est ouverte pour l'instant. Les évolutions qui le
traversent (contrat de contexte photographique `morfphoto-context/2`, page Photo de
morfAnalytics, dialogue avec PhotoHub) sont suivies dans les notes d'évolution du
parc, car elles engagent plusieurs dépôts à la fois.

## Non-objectifs

- **Aucune analyse métier** : regroupement, déduplication et interprétation vivent
  dans morfAnalytics, qui lit l'API de morfPhoto.
- **Aucune écriture dans les photos** : les fichiers et leur EXIF ne sont jamais
  modifiés. Le seul fichier que morfPhoto écrit dans une source est le
  `.morfphoto.json` d'un dossier ; une source réseau n'est d'ailleurs montée en
  écriture que si elle est déclarée `writable`.
- **Aucune suppression implicite** : un fichier disparu est marqué absent, sa purge
  reste un geste explicite.
