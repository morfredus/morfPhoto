#!/usr/bin/env bash
#
# new-service.sh — Amorce un nouveau service morfSystem a partir de ce template.
#
# Copie tout le projet dans un nouveau dossier en remplacant les noms :
#     morfPhoto  -> <NomCamel>   (projet / bibliotheque / cible CMake)
#     morfphoto         -> <nom>        (namespace, dossier include, binaire,
#                                           unite systemd, fichier de config)
#     MORFPHOTO         -> <NOM>        (prefixe de macro)
#
# Usage :
#     scripts/new-service.sh <nom> <NomCamel> [dossier_destination]
# Exemple :
#     scripts/new-service.sh morfwatch morfWatch
#       -> cree ../morfWatch_travail, pret a compiler, ou coder le metier.

set -euo pipefail

LOWER="${1:-}"; CAMEL="${2:-}"
if [[ -z "$LOWER" || -z "$CAMEL" ]]; then
    echo "Usage : $0 <nom-minuscule> <NomCamel> [dossier_destination]" >&2
    echo "Ex.   : $0 morfwatch morfWatch" >&2
    exit 1
fi
UPPER="$(echo "$LOWER" | tr '[:lower:]' '[:upper:]')"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="${3:-$(cd "$ROOT/.." && pwd)/${CAMEL}_travail}"

if [[ -e "$DEST" ]]; then
    echo "Destination deja existante : $DEST" >&2
    exit 1
fi

echo "Template : $ROOT"
echo "Nouveau  : $DEST  ($CAMEL / $LOWER / $UPPER)"

# --- 1. Copier l'arbre (sans .git ni artefacts de build) -----------------
mkdir -p "$DEST"
if command -v rsync >/dev/null; then
    rsync -a --exclude='.git' --exclude='build' --exclude='build-*' "$ROOT"/ "$DEST"/
else
    cp -a "$ROOT"/. "$DEST"/
    rm -rf "$DEST/.git" "$DEST"/build "$DEST"/build-* 2>/dev/null || true
fi

# --- 2. Remplacer les jetons dans tous les fichiers texte ----------------
# (on ne touche pas a third_party/, code vendore de morfBeacon)
grep -rlZ --binary-files=without-match \
     -e 'morfPhoto' -e 'morfphoto' -e 'MORFPHOTO' \
     "$DEST" 2>/dev/null | while IFS= read -r -d '' f; do
    case "$f" in */third_party/morf/beacon/*) continue;; esac  # ne pas toucher au code vendore
    sed -i -e "s/morfPhoto/$CAMEL/g" \
           -e "s/morfphoto/$LOWER/g" \
           -e "s/MORFPHOTO/$UPPER/g" "$f"
done

# --- 3. Renommer les fichiers/dossiers portant l'ancien nom --------------
mv "$DEST/include/morfphoto"                 "$DEST/include/$LOWER"
mv "$DEST/config/morfphoto.example.json"     "$DEST/config/$LOWER.example.json"
mv "$DEST/scripts/linux/morfphoto.service"   "$DEST/scripts/linux/$LOWER.service"

echo "Termine."
echo "  cd \"$DEST\""
echo "  cmake --preset mingw && cmake --build --preset mingw   # doit compiler tel quel"
echo "  puis : coder le metier dans src/ExampleModule.* et adapter ModuleFactory / CMakeLists."
echo
echo "AVANT TOUT DEPLOIEMENT : attribuer un port."
echo "  Le projet genere herite du port 8901, reserve aux gabarits et jamais"
echo "  utilise en production. Deux endroits le portent : config/$LOWER.example.json"
echo "  et le defaut compile dans include/$LOWER/ServiceConfig.h."

# Suggere un port concret plutot que « choisis-en un » : lire le registre a
# l'oeil pour trouver un trou est justement ce qui met deux projets sur le meme
# numero. Best-effort : si le registre est introuvable (parc partiel), on
# retombe sur l'instruction generique.
SUGGESTED=""
PARC="$(cd "$ROOT/.." && pwd)"   # dossier contenant tous les projets du parc
for base in "$PARC/morfTools" "$PARC/morfTools_travail"; do
    if [ -f "$base/ecosystem.json" ]; then
        SUGGESTED="$(python3 "$base/scripts/ecosystem-check.py" "$base/.." "$base/ecosystem.json" next-port 2>/dev/null || true)"
        MANIFEST="$base/ecosystem.json"
        break
    fi
done

if [ -n "$SUGGESTED" ]; then
    echo "  Prochain port libre du bloc service : >>> $SUGGESTED <<<"
    echo "  1. Ajouter ce projet a 'ports.allocations' de $MANIFEST avec http=$SUGGESTED."
    echo "  2. Reporter $SUGGESTED dans config/$LOWER.example.json et ServiceConfig.h."
else
    echo "  1. Reserver un port libre du bloc 8787-8799 dans 'ports.allocations'"
    echo "     de morfTools/ecosystem.json (y ajouter aussi ce projet)."
    echo "  2. Le reporter dans les DEUX emplacements ci-dessus."
fi
echo "  3. Verifier avec : morfTools/doctor.sh"
echo "  'morf doctor' echoue tant que le registre et la configuration divergent,"
echo "  et refuse tout port de la plage template en production."
