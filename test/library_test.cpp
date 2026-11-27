/*
 * morfPhoto - test de la bibliotheque (contrat morfphoto-library/1), headless.
 *
 * Verifie la lecture des sidecars XMP (darktable, XMP generique), le rattachement par le
 * nom, la fusion champ par champ et l'empreinte. Fonctions pures : aucun disque, aucun
 * reseau, aucun ExifTool. Retourne 0 si tout passe.
 */

#include <cstdio>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include "morfphoto/LibraryMetadata.h"

using namespace morfphoto;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } \
                              else { std::printf("ok  : %s\n", msg); } } while (0)

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QByteArray dtXmp = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
 <rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/"
   xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:lr="http://ns.adobe.com/lightroom/1.0/"
   xmlns:darktable="http://darktable.sf.net/"
   xmp:Rating="4" darktable:xmp_version="5" darktable:version_name="noir et blanc"
   darktable:history_current_hash="AAA" darktable:history_basic_hash="BBB" darktable:history_auto_hash="CCC">
  <darktable:colorlabels><rdf:Seq><rdf:li>0</rdf:li><rdf:li>2</rdf:li></rdf:Seq></darktable:colorlabels>
  <dc:subject><rdf:Bag><rdf:li>Bretagne</rdf:li><rdf:li>darktable|format|raw</rdf:li></rdf:Bag></dc:subject>
  <lr:hierarchicalSubject><rdf:Bag><rdf:li>Lieux|France|Bretagne</rdf:li><rdf:li>darktable|format|raw</rdf:li></rdf:Bag></lr:hierarchicalSubject>
  <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Port</rdf:li></rdf:Alt></dc:title>
 </rdf:Description></rdf:RDF></x:xmpmeta>)";
    const ProviderRead dt = readSidecarXmp(parseXmp(dtXmp), 0);
    CHECK(dt.source == QLatin1String("darktable"), "sidecar darktable reconnu");
    CHECK(dt.rating.toInt() == 4 && dt.rejected.toBool() == false, "darktable : 4 etoiles, non rejetee");
    CHECK(dt.edited.toBool() == true, "darktable : retouchee (hash courant different)");
    CHECK(labelNames(dt.labels) == QStringList({"red", "green"}), "darktable : etiquettes rouge et vert");
    CHECK(dt.keywords == QStringList({"Lieux | France | Bretagne"}),
          "darktable : mots-cles hierarchiques, tags internes retires");
    CHECK(dt.title.toString() == QLatin1String("Port") && dt.name.toString() == QLatin1String("noir et blanc"),
          "darktable : titre et nom de version");

    const QByteArray lrXmp = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
 <rdf:Description xmlns:xap="http://ns.adobe.com/xap/1.0/" xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
   xmlns:dc="http://purl.org/dc/elements/1.1/" xap:Rating="-1" xap:Label="Blue" crs:HasSettings="True">
  <dc:subject><rdf:Bag><rdf:li>voyage</rdf:li></rdf:Bag></dc:subject>
 </rdf:Description></rdf:RDF></x:xmpmeta>)";
    const ProviderRead lr = readSidecarXmp(parseXmp(lrXmp), 0);
    CHECK(lr.source == QLatin1String("xmp"), "sidecar XMP generique reconnu (prefixe xap tolere)");
    CHECK(lr.rejected.toBool() == true && lr.rating.isNull(), "note -1 : rejetee, etoiles inconnues");
    CHECK(lr.labels == LabelBlue && lr.edited.toBool() == true, "etiquette bleue, retouchee (crs:HasSettings)");
    CHECK(lr.keywords == QStringList({"voyage"}), "mots-cles a plat");

    CHECK(!parseXmp("pas du xml").ok, "document illisible : ignore");
    CHECK(readSidecarXmp(parseXmp("<a/>"), 0).source.isEmpty(), "XML sans rdf:Description : aucun fournisseur");

    // Fusion : darktable prioritaire, XMP generique ensuite, file en dernier.
    ProviderRead embedded;
    embedded.source = QStringLiteral("file");
    embedded.rating = 1;
    embedded.hasKeywords = true;
    embedded.keywords = {"ancien"};
    embedded.title = QStringLiteral("Titre embarque");
    const LibraryResult merged = mergeReads({lr, embedded, dt}, {"darktable", "xmp"});
    CHECK(merged.versions.size() == 1, "une seule version");
    const LibraryVersion& v = merged.versions.first();
    CHECK(v.rating.toInt() == 4 && v.sources.value("rating").toString() == QLatin1String("darktable"),
          "note : darktable l'emporte sur xmp et file");
    CHECK(v.rejected.toBool() == true && v.sources.value("rejected").toString() == QLatin1String("xmp"),
          "rejet : vrai si UN fournisseur le dit (le XMP generique, note -1), meme si darktable dit non rejetee");
    CHECK(v.keywords == QStringList({"Lieux | France | Bretagne"}),
          "mots-cles : darktable fait foi, ceux de file ne reviennent pas");
    CHECK(v.edited.toBool() == true, "retouchee : vrai si un fournisseur le dit");
    CHECK(v.title.toString() == QLatin1String("Port"), "titre : darktable avant file");

    // Valeur connue mais vide : un sidecar darktable sans mot-cle ne recoit pas ceux de l'image.
    ProviderRead emptyDt;
    emptyDt.source = QStringLiteral("darktable");
    emptyDt.hasKeywords = true;
    const LibraryResult kept = mergeReads({emptyDt, embedded}, {"darktable"});
    CHECK(kept.versions.first().keywords.isEmpty(), "liste vide d'un sidecar connu non recompletee");
    CHECK(kept.versions.first().rating.toInt() == 1, "la note vient de file, faute de mieux");

    // Plusieurs versions : numerotees par le logiciel, fusionnees par numero.
    ProviderRead copy = dt;
    copy.version = 2;
    const LibraryResult two = mergeReads({dt, copy}, {"darktable"});
    CHECK(two.versions.size() == 2 && two.versions[1].version == 2 && two.versions[1].isCopy
          && !two.versions[0].isCopy, "deux versions : l'original et la copie 2");

    // Rattachement par le nom.
    const QVector<SidecarFile> files{
        {"/p", "IMG_1.ARW.xmp", 10, 1}, {"/p", "IMG_1.xmp", 10, 1},
        {"/p", "IMG_1_02.ARW.xmp", 10, 1}, {"/p", "IMG_1_03.ARW.xmp", 10, 1},
        {"/p", "IMG_2.xmp", 10, 1}};
    const QSet<QString> images{"img_1.arw", "img_1.jpg", "img_1_03.arw", "img_2.jpg"};
    const QVector<SidecarRef> arw = matchSidecars("IMG_1.ARW", "/p", files, images);
    CHECK(arw.size() == 2 && arw[0].name == QLatin1String("IMG_1.ARW.xmp") && arw[1].version == 2,
          "IMG_1.ARW : sa version 0 et la copie 2 (IMG_1_03.ARW.xmp est le sidecar de IMG_1_03.ARW)");
    const QVector<SidecarRef> jpg = matchSidecars("IMG_1.JPG", "/p", files, images);
    CHECK(jpg.size() == 1 && jpg[0].name == QLatin1String("IMG_1.xmp"), "IMG_1.JPG : sidecar partage IMG_1.xmp");
    CHECK(matchSidecars("IMG_2.JPG", "/p", files, images).size() == 1, "IMG_2.JPG : IMG_2.xmp");
    CHECK(matchSidecars("IMG_3.JPG", "/p", files, images).isEmpty(), "sans sidecar : rien");

    // Empreinte : change si un sidecar change ou si la configuration change.
    const QString f1 = libraryFingerprint(arw, {"darktable"});
    CHECK(f1 == libraryFingerprint(arw, {"darktable"}), "empreinte stable");
    QVector<SidecarRef> touched = arw;
    touched[0].mtime = 99;
    CHECK(f1 != libraryFingerprint(touched, {"darktable"}), "empreinte change avec le sidecar");
    CHECK(f1 != libraryFingerprint(arw, {"darktable", "xmp"}), "empreinte change avec les fournisseurs");

    // XMP embarque lu dans les tags ExifTool.
    const ProviderRead emb = readEmbedded(QJsonObject{{"Rating", 3}, {"Subject", QJsonArray{"a", "b"}}, {"Label", "Red"}});
    CHECK(emb.source == QLatin1String("file") && emb.rating.toInt() == 3 && emb.labels == LabelRed
          && emb.keywords == QStringList({"a", "b"}), "XMP embarque : note, etiquette, mots-cles");
    CHECK(readEmbedded(QJsonObject{{"Make", "X"}}).source.isEmpty(), "image sans XMP : aucun fournisseur");
    CHECK(normalizeKeyword(" a/ b /c ", QLatin1Char('/')) == QLatin1String("a | b | c"), "mot-cle normalise");

    // --- DxO PhotoLab (.dop) : fichier reel de DxO PhotoLab 8.23, REDUIT et ANONYMISE (la position, les
    // droits et la plupart des reglages sont remplaces ; la structure et la syntaxe sont celles du vrai fichier).
    const QByteArray dop = R"(Sidecar = {
Date = "2026-01-01T09:51:05.4292219Z",
Software = "DxO PhotoLab 8.23",
Source = {
CafId = "C64145b",
Items = {
{
Albums = "",
GPSAltitude = 22.6,
GPSLatitude = 48.5,
GPSLongitude = -2.25,
IPTC = {
statusCopyrightNotice = "Auteur Test",
}
,
Keywords = {
{
"darktable",
"format",
"dng",
}
,
{
"Lieux",
"France",
"Bretagne",
}
,
{
"Famille",
}
,
}
,
Name = "IMG_0001.DNG",
ProcessingStatus = 1,
Rating = 2,
Settings = {
AppliedPresetDisplayName = "1 - DxO Style - Naturel",
Base = {
ChannelMixerActive = false,
CropRect = {
0,
0,
1,
1,
}
,
DehazingValue = 9.727627,
HSLHueSlices = {
{
FadeInStart = 325.07,
Label = "Red",
}
,
}
,
}
,
Overrides = {
ExposureActive = true,
}
,
Version = "19.8",
}
,
ShotDate = "2026-10-27T15:00:32.0000000",
}
,
}
,
Uuid = "9D61F308-51F9-4179-8003-083E2B7FF39E",
}
,
Version = "19.0",
}
)";
    const ProviderRead dx = readSidecarDop(dop, 0);
    CHECK(dx.source == QLatin1String("dxo"), "DxO : .dop reconnu");
    CHECK(dx.rating.toInt() == 2 && dx.rejected.isNull(), "DxO : 2 etoiles, rejet inconnu");
    CHECK(dx.keywords == QStringList({"Lieux | France | Bretagne", "Famille"}) && dx.hasKeywords,
          "DxO : mots-cles hierarchiques lus, etiquette interne darktable retiree");
    CHECK(qAbs(dx.latitude.toDouble() - 48.5) < 1e-9 && qAbs(dx.longitude.toDouble() + 2.25) < 1e-9, "DxO : position decimale");
    CHECK(dx.rights.toString() == QLatin1String("Auteur Test"), "DxO : mention de droits (IPTC)");
    CHECK(dx.edited.isNull(), "DxO : retouchee INCONNU (un .dop existe des l'ouverture de l'image)");
    CHECK(readSidecarDop("Sidecar = { Source = { Items = { } } }", 0).source.isEmpty(), "DxO : sans element d'image, ignore");
    CHECK(readSidecarDop("n'importe quoi", 0).source.isEmpty(), "DxO : fichier illisible ignore");
    CHECK(readSidecarDop(QByteArray("Sidecar = { A = ") + QByteArray(100, '{'), 0).source.isEmpty(), "DxO : imbrication excessive ignoree");
    const QVector<SidecarFile> dopFiles{{"/p", "IMG_0001.DNG.dop", 10, 1}, {"/p", "IMG_0001.DNG.xmp", 10, 1}};
    const QVector<SidecarRef> dopRefs = matchSidecars("IMG_0001.DNG", "/p", dopFiles, {"img_0001.dng"});
    CHECK(dopRefs.size() == 2 && dopRefs[0].version == 0 && dopRefs[1].version == 0, "DxO : photo.ext.dop rattache a la version 0, avec le .xmp");

    // XMP mixte reel (darktable ET un second editeur, les proprietes en ELEMENTS dans un second
    // rdf:Description), reduit et anonymise : la lecture existante doit s'en sortir telle quelle.
    const QByteArray mixed = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="XMP Core 4.4.0-Exiv2">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:exif="http://ns.adobe.com/exif/1.0/" xmlns:lc="http://ns.lightcraft.app/lc/1.0/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmlns:darktable="http://darktable.sf.net/"
    xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:lr="http://ns.adobe.com/lightroom/1.0/"
    exif:GPSAltitude="226/10" darktable:xmp_version="5" darktable:history_end="12"
    darktable:history_auto_hash="32de3adb55b9734753332a77b131d46e"
    darktable:history_current_hash="1ee75e4e4f6297f33fa5c994b45b6891">
   <darktable:history><rdf:Seq><rdf:li darktable:num="0" darktable:operation="rawprepare"/></rdf:Seq></darktable:history>
  </rdf:Description>
  <rdf:Description rdf:about="" xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
    xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:exif="http://ns.adobe.com/exif/1.0/" xmlns:lr="http://ns.adobe.com/lightroom/1.0/"
    xmlns:lc="http://ns.lightcraft.app/lc/1.0/">
   <xmp:Rating>2</xmp:Rating>
   <exif:GPSLatitude>48,30.000N</exif:GPSLatitude>
   <exif:GPSLongitude>2,15.000W</exif:GPSLongitude>
   <lc:flag>none</lc:flag>
   <dc:rights><rdf:Alt><rdf:li xml:lang="x-default">Auteur Test</rdf:li></rdf:Alt></dc:rights>
   <dc:subject><rdf:Bag><rdf:li>darktable</rdf:li><rdf:li>format</rdf:li><rdf:li>dng</rdf:li></rdf:Bag></dc:subject>
   <lr:hierarchicalSubject><rdf:Bag><rdf:li>darktable|format|dng</rdf:li></rdf:Bag></lr:hierarchicalSubject>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>)";
    const ProviderRead mx = readSidecarXmp(parseXmp(mixed), 0);
    CHECK(mx.source == QLatin1String("darktable"), "XMP mixte : reconnu comme darktable");
    CHECK(mx.rating.toInt() == 2, "XMP mixte : note ecrite en ELEMENT dans le second rdf:Description");
    CHECK(mx.edited.toBool() == true, "XMP mixte : retouchee, meme sans hash de base (courant different de l'auto)");
    CHECK(mx.hasKeywords && mx.keywords.isEmpty(), "XMP mixte : seuls des mots-cles internes darktable, tous retires");
    CHECK(qAbs(mx.latitude.toDouble() - 48.5) < 1e-9 && qAbs(mx.longitude.toDouble() + 2.25) < 1e-9, "XMP mixte : position DDD,MM.mmmN / W");
    CHECK(mx.rights.toString() == QLatin1String("Auteur Test"), "XMP mixte : droits (Alt)");
    // DxO et XMP sur la meme photo : meme position, meme note, la priorite tranche.
    const LibraryResult both = mergeReads({dx, mx}, {"darktable", "xmp", "dxo"});
    CHECK(both.versions.size() == 1 && both.versions[0].rating.toInt() == 2, "DxO + darktable : une version, note commune");
    CHECK(both.versions[0].sources.value("rating").toString() == QLatin1String("darktable"), "priorite : darktable avant DxO");
    CHECK(both.versions[0].keywords.isEmpty(), "darktable connait ses mots-cles (vides) : ceux de DxO ne reviennent pas");
    const LibraryResult dxFirst = mergeReads({dx, mx}, {"dxo", "darktable"});
    CHECK(dxFirst.versions[0].keywords == QStringList({"Lieux | France | Bretagne", "Famille"}), "priorite DxO d'abord : ses mots-cles l'emportent");

    // --- Lightcraft : `lc:flag` dans le meme .xmp que darktable (drapeau « retenue »). Confirme sur un vrai
    // fichier : « none » avant, « pick » apres que le drapeau a ete pose. « reject » n'est PAS confirme.
    auto withFlag = [&mixed](const QByteArray& flag) {
        QByteArray x = mixed;
        x.replace("<lc:flag>none</lc:flag>", "<lc:flag>" + flag + "</lc:flag>");
        return x;
    };
    const ProviderRead pick = readSidecarLightcraft(parseXmp(withFlag("pick")), 0);
    CHECK(pick.source == QLatin1String("lightcraft") && pick.picked.toBool() == true, "Lightcraft : lc:flag pick = retenue");
    const ProviderRead none = readSidecarLightcraft(parseXmp(withFlag("none")), 0);
    CHECK(none.source == QLatin1String("lightcraft") && none.picked.isValid() && none.picked.toBool() == false,
          "Lightcraft : lc:flag none = pas de drapeau (faux, pas inconnu)");
    const ProviderRead rej = readSidecarLightcraft(parseXmp(withFlag("reject")), 0);
    CHECK(rej.source == QLatin1String("lightcraft") && rej.rejected.toBool() == true && rej.picked.toBool() == false,
          "Lightcraft : lc:flag reject = rejetee, pas retenue");
    CHECK(pick.rejected.toBool() == false && none.rejected.toBool() == false, "Lightcraft : pick et none = non rejetee");
    CHECK(readSidecarLightcraft(parseXmp(withFlag("autre")), 0).source.isEmpty(), "Lightcraft : valeur inconnue = rien de devine");
    // Rejet : darktable dit « non rejetee » (note >= 0), Lightcraft dit « rejetee » : le rejet l'emporte, attribue a lightcraft.
    const LibraryResult rejMerged = mergeReads({mx, rej}, {"darktable", "lightcraft"});
    CHECK(rejMerged.versions[0].rejected.toBool() == true && rejMerged.versions[0].sources.value("rejected").toString() == QLatin1String("lightcraft"),
          "rejet : vrai si un fournisseur le dit, meme apres darktable (non rejetee)");
    const LibraryResult notRej = mergeReads({mx, pick}, {"darktable", "lightcraft"});
    CHECK(notRej.versions[0].rejected.toBool() == false && notRej.versions[0].sources.value("rejected").toString() == QLatin1String("darktable"),
          "rejet : faux quand personne ne le dit, attribue au premier qui le sait");
    CHECK(readSidecarLightcraft(parseXmp(dtXmp), 0).source.isEmpty(), "XMP sans lc:flag : pas de fournisseur lightcraft");
    // Le drapeau est attribue a lightcraft, la note reste a darktable, dans la meme version.
    const LibraryResult lcMerged = mergeReads({mx, pick}, {"darktable", "lightcraft"});
    CHECK(lcMerged.versions.size() == 1 && lcMerged.versions[0].picked.toBool() == true
          && lcMerged.versions[0].sources.value("picked").toString() == QLatin1String("lightcraft")
          && lcMerged.versions[0].sources.value("rating").toString() == QLatin1String("darktable"),
          "Lightcraft + darktable : drapeau de lightcraft, note de darktable");
    CHECK(mergeReads({mx}, {"darktable"}).versions[0].picked.isNull(), "sans lightcraft : retenue inconnue (null)");

    std::printf(failures ? "\n%d test(s) en echec\n" : "\nTous les tests passent\n", failures);
    return failures ? 1 : 0;
}
