#!/usr/bin/env bash
#
# deploy-config.sh — copie la configuration du depot vers son emplacement
#                    d'installation. C'est LE script de deploiement de config.
#
# morfPhoto n'a qu'UN fichier de configuration :
#
#   config/morfphoto.json  ->  /etc/morfsystem/morfphoto/morfphoto.json
#       Reglages du service : port, adresse d'ecoute, racines autorisees,
#       cadence de la surveillance (watch), ExifTool. Lu par morfPhoto seul.
#
# Pourquoi ce script en plus de `service.py` (morfdeploy) : `service.py update`
# n'AJOUTE que les cles manquantes, il n'ecrase jamais une valeur ni une entree
# de liste deja presente (pour ne pas effacer vos reglages). Quand vous voulez au
# contraire REMPLACER la config deployee par celle du depot -- par exemple apres
# avoir change une racine ou `watch.interval_ms` -- c'est ce script qu'il faut.
#
# La config REELLE du depot (config/morfphoto.json) sert de reference si elle
# existe ; sinon l'exemple (config/morfphoto.example.json). Gardez donc un vrai
# fichier dans votre clone, avec VOS racines, pour qu'il devienne la reference
# deployee -- l'exemple ne porte que des chemins d'exemple.
#
# Le fichier est sauvegarde avant d'etre remplace (.bak-<date> a cote), et les
# differences appliquees sont affichees. Rien n'est perdu.
#
# Ne PAS prefixer par sudo : le script n'eleve que les ecritures systeme. La
# lecture, la comparaison et l'affichage n'ont aucun besoin des droits root.
#
# Usage :
#   ./scripts/linux/deploy-config.sh                # deploie, puis redemarre
#   ./scripts/linux/deploy-config.sh --no-restart   # sans redemarrer
#   ./scripts/linux/deploy-config.sh --if-absent    # ne placer que si absent
#   ./scripts/linux/deploy-config.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# La configuration vit dans /etc, pas a cote du binaire : convention Linux, et
# cela survit a un effacement de /opt pour reinstaller. APP_DIR reste le dossier
# du BINAIRE, utilise ici seulement pour verifier que le service est installe.
APP_DIR="${MORF_APP_DIR:-/opt/morfphoto}"
CONFIG_DIR="${MORF_CONFIG_DIR:-/etc/morfsystem/morfphoto}"
SERVICE_NAME="${MORF_SERVICE_NAME:-morfphoto}"
STATUS_PORT="${MORF_STATUS_PORT:-8793}"

# Commande d'elevation. Vide quand on est deja root, surchargeable par MORF_SUDO
# pour deployer vers un emplacement accessible sans privileges — ce qui rend le
# script VERIFIABLE hors d'une vraie machine.
SUDO="${MORF_SUDO-sudo}"
[[ "${EUID:-1}" -eq 0 ]] && SUDO=""

RESTART=1
# --if-absent : ne rien ecraser, ne placer que le fichier manquant. C'est ce dont
# l'installation a besoin -- produire un systeme qui fonctionne sans jamais
# effacer les reglages d'une installation precedente.
IF_ABSENT=0

for arg in "$@"; do
    case "$arg" in
        --no-restart) RESTART=0 ;;
        --if-absent)  IF_ABSENT=1 ;;
        -h|--help)    sed -n '3,31p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Option inconnue : $arg  (--no-restart | --if-absent | --help)" >&2; exit 2 ;;
    esac
done

# Source : la configuration REELLE du depot si elle existe, l'exemple sinon.
pick_source() {
    local base="$1"
    if [[ -f "$REPO_ROOT/config/$base.json" ]]; then
        printf '%s\n' "$REPO_ROOT/config/$base.json"
    elif [[ -f "$REPO_ROOT/config/$base.example.json" ]]; then
        printf '%s\n' "$REPO_ROOT/config/$base.example.json"
    fi
    return 0
}

# Copie un fichier vers sa destination, en sauvegardant et en montrant ce qui
# change. Un deploiement muet laisse l'utilisateur sans moyen de savoir si son
# geste a fait ce qu'il croyait.
deploy_one() {
    local src="$1" dest="$2" label="$3"

    if (( IF_ABSENT )) && $SUDO test -f "$dest"; then
        echo "── $label"
        echo "   deja present, conserve : $dest"
        echo
        return 0
    fi

    echo "── $label"
    echo "   source      : $src"
    echo "   destination : $dest"

    $SUDO install -d -m 0755 "$(dirname "$dest")"

    if $SUDO test -f "$dest"; then
        local backup="$dest.bak-$(date +%Y%m%d-%H%M%S)"
        $SUDO cp -p "$dest" "$backup"
        echo "   sauvegarde  : $backup"
        if ! $SUDO diff -q "$backup" "$src" >/dev/null 2>&1; then
            local out lines
            out="$($SUDO diff -u "$backup" "$src" | tail -n +3 || true)"
            lines="$(printf '%s\n' "$out" | wc -l)"
            echo
            echo "   differences appliquees :"
            printf '%s\n' "$out" | head -n 25 | sed 's/^/      /'
            (( lines > 25 )) && echo "      ... ($((lines - 25)) lignes de plus)"
            echo
        else
            echo "   (identique : rien ne change)"
        fi
    fi

    $SUDO install -m 0644 "$src" "$dest"
    echo "   copie       : OK"
    echo
}

SRC="$(pick_source morfphoto)"
if [[ -z "$SRC" ]]; then
    echo "Aucune configuration dans $REPO_ROOT/config/ (ni morfphoto.json ni l'exemple)." >&2
    exit 1
elif [[ ! -d "$APP_DIR" ]] && (( ! IF_ABSENT )); then
    echo "Service non installe : $APP_DIR absent." >&2
    echo "Lancer d'abord : sudo ./service.py install" >&2
    exit 1
fi

deploy_one "$SRC" "$CONFIG_DIR/morfphoto.json" "Configuration du service   ->  $CONFIG_DIR"

if (( RESTART )) && command -v systemctl >/dev/null 2>&1; then
    if $SUDO systemctl cat "$SERVICE_NAME" >/dev/null 2>&1; then
        $SUDO systemctl restart "$SERVICE_NAME"
        echo "Redemarre : $SERVICE_NAME"
    fi
    echo
    echo "Verifier :"
    echo "    curl -s http://127.0.0.1:$STATUS_PORT/api/v1/index/status | head -c 200"
    echo "    journalctl -u $SERVICE_NAME -n 20"
else
    echo "Redemarrer pour appliquer :"
    echo "    $SUDO systemctl restart $SERVICE_NAME"
fi
