/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

// -----------------------------------------------------------------------------
// Bibliotheque photo (contrat morfphoto-library/1) : la couche note, rejet, etiquettes,
// mots-cles, retouchee, posee par les logiciels photo a cote des images.
//
// Ce module est PUR : octets et noms en entree, structures en sortie, aucun acces a la
// base ni au disque. C'est ce qui le rend testable sans reseau ni ExifTool.
//
// Souverainete : l'EXIF du fichier fait autorite sur la prise de vue ; les sidecars ne
// donnent que ce qui concerne le traitement (voir le contrat). Une valeur inconnue est
// toujours un QVariant vide (null), jamais une valeur par defaut.
// -----------------------------------------------------------------------------
namespace morfphoto {

// Etiquettes de couleur : masque binaire INTERNE (le contrat expose des noms).
enum LabelBit { LabelRed = 1, LabelYellow = 2, LabelGreen = 4, LabelBlue = 8, LabelPurple = 16 };
QStringList labelNames(int mask);                 // 5 -> ("red", "green")
int         labelMaskFromName(const QString& name);   // "Red" / "red" -> 1, inconnu -> 0

// Fournisseurs de sidecars connus. `file` (XMP embarque dans l'image) est implicite,
// toujours le dernier de la liste de priorite.
QStringList knownProviders();                     // darktable, xmp, dxo, lightcraft
inline QString embeddedProviderId() { return QStringLiteral("file"); }

// Un fichier sidecar vu pendant le parcours d'un dossier.
struct SidecarFile {
    QString directory;
    QString name;        // nom de fichier, avec extension (« IMG_1.ARW.xmp »)
    qint64  size  = 0;
    qint64  mtime = 0;
};

// Un sidecar rattache a une image, pret a etre lu.
struct SidecarRef {
    QString path;        // chemin complet (lecture seulement cote serveur)
    QString name;        // nom de fichier (seul champ expose par l'API)
    int     version = 0; // 0 = original, 1... = copies (darktable : IMG_1_01.ARW.xmp)
    qint64  size  = 0;
    qint64  mtime = 0;
};

// Candidats de sidecar d'une image : par le NOM seulement, sans ouvrir le fichier.
//   IMG_1.ARW.xmp     -> version 0 (nommage darktable)
//   IMG_1_02.ARW.xmp  -> version 2 (copie darktable)
//   IMG_1.xmp         -> version 0, partage avec IMG_1.JPG (nommage Lightroom, digiKam...)
// `IMG_1.ARW.xmp` l'emporte toujours sur `IMG_1.xmp` pour la version 0.
// `imagesLower` : noms (en minuscules) des images du meme dossier. Il leve l'ambiguite
// de darktable : `IMG_01.ARW.xmp` est la copie 1 de `IMG.ARW`... sauf si `IMG_01.ARW`
// existe, auquel cas c'est SON sidecar.
QVector<SidecarRef> matchSidecars(const QString& imageFilename, const QString& directory,
                                  const QVector<SidecarFile>& sidecarsOfDirectory,
                                  const QSet<QString>& imagesLower);

// Proprietes d'un document XMP, cle = « prefixe:nom » avec un prefixe CANONIQUE choisi
// par l'espace de noms (xmp:Rating, dc:subject, lr:hierarchicalSubject, darktable:...).
struct XmpDocument {
    bool ok = false;
    QHash<QString, QStringList> props;
    bool has(const QString& key) const { return props.contains(key); }
    QString first(const QString& key) const { return props.value(key).value(0); }
};
XmpDocument parseXmp(const QByteArray& bytes);

// Ce qu'UN fournisseur sait d'UNE version. Tout champ non renseigne reste vide (null).
struct ProviderRead {
    QString  source;                  // darktable | xmp | dxo | lightcraft | file
    int      version = 0;
    QVariant name, rating, rejected, title, description, creator, rights, edited;
    QVariant picked;                  // drapeau « retenue » (Lightcraft) : vrai, faux, ou inconnu
    QVariant latitude, longitude;
    bool     hasLabels = false;       // le fournisseur CONNAIT ce champ (meme vide)
    int      labels = 0;
    bool     hasKeywords = false;
    QStringList keywords;
};

// Lit un sidecar XMP et dit quel logiciel l'a ecrit (darktable ou XMP generique).
// `source` = vide si le document n'est pas du XMP exploitable.
ProviderRead readSidecarXmp(const XmpDocument& doc, int version);

// Lightcraft : un second editeur qui ecrit DANS le meme .xmp que darktable, avec ses proprietes `lc:`.
// Seul `lc:flag` est lu : « pick » = retenue, « none » = pas de drapeau (les deux sont confirmes sur de
// vrais fichiers). Toute autre valeur reste inconnue : le sens de « reject » n'est pas confirme, et
// `lc:settings` (reglages en JSON) n'est pas lu. Les valeurs sont attribuees a `lightcraft`, pas au
// logiciel qui a classe le fichier.
ProviderRead readSidecarLightcraft(const XmpDocument& doc, int version);

// DxO PhotoLab (`photo.ext.dop`) : un fichier texte en tables imbriquees, pas du XML. On n'en lit
// que ce qui est confirme sur un fichier reel (DxO PhotoLab 8.23) : note, mots-cles, position,
// mention de droits. « Retouchee » reste INCONNU : PhotoLab ecrit un .dop des qu'on ouvre une image,
// avec des corrections automatiques deja appliquees, donc sa seule presence ne prouve rien.
// Seul le premier element (`Items`) est lu : le format des copies virtuelles n'est pas encore confirme.
ProviderRead readSidecarDop(const QByteArray& bytes, int version);

// Lit l'XMP EMBARQUE a partir des tags ExifTool deja stockes (raw_exif). Vide
// (source vide) si l'image n'en porte aucun.
ProviderRead readEmbedded(const QJsonObject& rawExif);

// Resultat fusionne d'une version (contrat, §4 et §5).
struct LibraryVersion {
    int      version = 0;
    bool     isCopy = false;
    QVariant name, rating, rejected, title, description, creator, rights, edited;
    QVariant picked;
    int      labels = 0;
    QStringList keywords;
    QJsonObject sources;              // champ -> fournisseur qui l'a donne
};

// Un sidecar tel que l'API le montre : logiciel, nom du fichier (jamais le chemin), et
// s'il a pu etre lu.
struct SidecarEntry {
    QString name;
    QString source;
    bool    readable = true;
};

struct LibraryResult {
    QVector<LibraryVersion> versions;
    QVector<SidecarEntry> sidecars;
    QVariant latitude, longitude;     // position tiree d'un sidecar (a defaut de l'EXIF)
    QString  gpsSource;
    QString  mainSource;              // fournisseur le plus prioritaire present (pour l'export)
};

// Fusionne, champ par champ, les lectures de plusieurs fournisseurs. `priority` donne
// l'ordre (le plus prioritaire d'abord) ; `file` est ajoute en dernier d'office.
LibraryResult mergeReads(const QVector<ProviderRead>& reads, const QStringList& priority);

// Empreinte de ce que la lecture a vu (liste des sidecars + configuration) : tant
// qu'elle ne change pas, la photo n'est pas relue.
QString libraryFingerprint(const QVector<SidecarRef>& sidecars, const QStringList& providers);

// Mot-cle hierarchique normalise : niveaux separes par « | » avec espaces.
QString normalizeKeyword(const QString& raw, QChar separator);

} // namespace morfphoto
