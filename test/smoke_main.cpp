/*
 * morfPhoto — test de fumée headless (aucun réseau, aucun ExifTool).
 *
 * Vérifie les invariants du domaine : classification des types, garde-fou de
 * périmètre, souveraineté du mapping EXIF (sur un JSON figé), et tout le cycle
 * d'indexation (verdict incrémental via (path,size,mtime), disparu marqué puis
 * ravivé, sélection hors racine refusée, retrait doux préservant l'historique).
 *
 * L'indexeur est éprouvé avec un extracteur NUL : la logique de réconciliation ne
 * dépend pas des valeurs EXIF, ce qui rend le test portable et déterministe.
 * Compilé via l'option CMake MORFPHOTO_BUILD_SMOKE. Retourne 0 si tout passe.
 */

#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QJsonObject>
#include <QVariantMap>

#include "morfphoto/PhotoScanner.h"
#include "morfphoto/PhotoRepository.h"
#include "morfphoto/ExifExtractor.h"
#include "morfphoto/Indexer.h"

using namespace morfphoto;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } \
                              else { std::printf("ok  : %s\n", msg); } } while (0)

static QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }

static void writeBytes(const QString& path, int n) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QByteArray(n, 'x'));
        f.close();
    }
}

static int lastRunInt(PhotoRepository& repo, const char* key) {
    return repo.latestRun(nullptr).value(QLatin1String(key)).toInt();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QString base = QDir::tempPath() + QStringLiteral("/morfphoto_smoke_%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(base + QStringLiteral("/photos/2024"));
    const QString photos = base + QStringLiteral("/photos");

    // --- 1) Scanner : classification et périmètre ---
    CHECK(fileTypeFor(QStringLiteral(".ARW")) == QLatin1String("raw"), "type .ARW -> raw (insensible casse)");
    CHECK(fileTypeFor(QStringLiteral(".jpg")) == QLatin1String("jpeg"), "type .jpg -> jpeg");
    CHECK(fileTypeFor(QStringLiteral(".xyz")) == QLatin1String("other"), "type inconnu -> other");
    CHECK(isWithinRoots(photos + QStringLiteral("/2024/x.jpg"), {photos}), "sous racine -> autorise");
    CHECK(!isWithinRoots(base + QStringLiteral("/autre/x.jpg"), {photos}), "hors racine -> refuse");
    CHECK(!isWithinRoots(photos + QStringLiteral("/../evasion.jpg"), {photos}), "'..' ne s'echappe pas");

    // Sonde d'accessibilité bornée : un dossier présent répond, un chemin absent est
    // déclaré indisponible sans attendre (déterministe, hors réseau).
    CHECK(probeAccessible(photos, 1000), "probe: dossier existant -> accessible");
    CHECK(!probeAccessible(base + QStringLiteral("/inexistant"), 200),
          "probe: chemin absent -> indisponible");

    // --- 2) Mapping EXIF (souveraineté), sur un JSON figé ---
    const QString json = QStringLiteral(
        "[{\"SourceFile\":\"x\",\"DateTimeOriginal\":\"2017-02-11 22:18:38\","
        "\"Make\":\"NIKON\",\"Model\":\"NIKON D7200\",\"LensModel\":\"18-105\","
        "\"FocalLength\":30.0,\"FNumber\":7.1,\"ExposureTime\":0.0166,"
        "\"ShutterSpeed\":\"1/60\",\"ISO\":3200}]");
    QString mapErr;
    const ExifData d = ExifExtractor::mapTags(json, &mapErr);
    CHECK(mapErr.isEmpty() && d.ok, "mapTags: succes");
    CHECK(d.focalLength.toDouble() == 30.0, "focale brute preservee (30)");
    CHECK(d.shutterSpeed.toString() == QLatin1String("1/60"), "vitesse lisible conservee");
    CHECK(d.shutterSpeedS.toDouble() == 0.0166, "vitesse numerique conservee");
    CHECK(d.cameraModel.toString() == QLatin1String("NIKON D7200"), "boitier mappe");
    CHECK(!d.raw.contains(QStringLiteral("SourceFile")), "raw_exif exclut SourceFile");
    QString errCase;
    const ExifData de = ExifExtractor::mapTags(
        QStringLiteral("[{\"SourceFile\":\"x\",\"Error\":\"boom\"}]"), &errCase);
    CHECK(!de.ok && !errCase.isEmpty(), "mapTags: Error -> ok=false, message");

    // --- 3) Cycle d'indexation (extracteur nul) ---
    writeBytes(photos + QStringLiteral("/2024/a.jpg"), 100);
    writeBytes(photos + QStringLiteral("/2024/b.arw"), 200);
    writeBytes(photos + QStringLiteral("/2024/notes.txt"), 50);

    PhotoRepository repo(QStringLiteral("smoke:main"));
    CHECK(repo.open(base + QStringLiteral("/db.sqlite")), "ouverture base + migrations");
    const int fid = repo.addFolder(photos, photos, QVariant(), true, false, QVariant(), nowIso());
    CHECK(fid > 0, "addFolder");

    Indexer idx(&repo, nullptr, {photos});
    CHECK(idx.run(IndexMode::Full, {}, QStringLiteral("cli")) > 0, "passe full");
    CHECK(repo.summary().value(QStringLiteral("files_present")).toInt() == 2,
          "2 fichiers indexes (notes.txt ignore)");

    idx.run(IndexMode::Incremental, {}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "files_new") == 0 && lastRunInt(repo, "files_updated") == 0,
          "incremental inchange: rien a faire");

    writeBytes(photos + QStringLiteral("/2024/a.jpg"), 250);   // taille differente
    idx.run(IndexMode::Incremental, {}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "files_updated") == 1, "fichier modifie -> 1 update");

    QFile::remove(photos + QStringLiteral("/2024/b.arw"));
    idx.run(IndexMode::Incremental, {}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "files_missing") == 1, "fichier disparu -> missing");
    QVariantMap fMissing; fMissing["state"] = QStringLiteral("missing");
    CHECK(repo.listPhotos(fMissing, 1, 10).value(QStringLiteral("total")).toInt() == 1,
          "un fichier en etat missing (jamais supprime)");

    writeBytes(photos + QStringLiteral("/2024/b.arw"), 200);   // reapparait a l'identique
    idx.run(IndexMode::Incremental, {}, QStringLiteral("cli"));
    CHECK(repo.summary().value(QStringLiteral("files_present")).toInt() == 2,
          "fichier revenu -> ravive present");

    // Sélection hors racine : refusée et journalisée, rien indexé.
    QDir().mkpath(base + QStringLiteral("/autre"));
    writeBytes(base + QStringLiteral("/autre/secret.jpg"), 100);
    const int fidBad = repo.addFolder(base + QStringLiteral("/autre"), photos, QVariant(), true, false, QVariant(), nowIso());
    idx.run(IndexMode::Full, {fidBad}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "errors_count") >= 1, "selection hors racine -> erreur journalisee");
    CHECK(repo.summary().value(QStringLiteral("files_present")).toInt() == 2,
          "rien indexe hors racine");

    // Retrait doux : les fichiers passent 'deleted', l'historique reste.
    repo.softDeleteFolder(fid, nowIso());
    CHECK(repo.summary().value(QStringLiteral("files_present")).toInt() == 0,
          "retrait doux -> plus aucun present");
    CHECK(repo.summary().value(QStringLiteral("files_total")).toInt() >= 2,
          "retrait doux -> historique conserve (lignes toujours la)");

    // deleted_at classe le dossier comme retiré (visible dans le détail JSON).
    bool wasDeleted = false;
    (void)repo.folderIdByPath(photos, &wasDeleted);
    CHECK(wasDeleted, "retrait doux -> deleted_at renseigne");

    // Restauration : le dossier redevient surveillé, une passe ravive les fichiers.
    repo.restoreFolder(fid);
    bool stillDeleted = true;
    (void)repo.folderIdByPath(photos, &stillDeleted);
    CHECK(!stillDeleted, "restauration -> deleted_at efface");
    idx.run(IndexMode::Incremental, {}, QStringLiteral("cli"));
    CHECK(repo.summary().value(QStringLiteral("files_present")).toInt() == 2,
          "restauration -> fichiers ravives (present)");

    // --- 3b) Export compact pour l'analyse (colonnaire + dictionnaires) ---
    {
        const QJsonObject ds = repo.photoDataset();
        CHECK(ds.value(QStringLiteral("count")).toInt() == 2,
              "dataset: 2 photos presentes exportees");
        const QJsonObject cols = ds.value(QStringLiteral("columns")).toObject();
        CHECK(cols.value(QStringLiteral("taken_at")).toArray().size() == 2,
              "dataset: colonne taken_at alignee sur count");
        CHECK(cols.value(QStringLiteral("folder_id")).toArray().size() == 2,
              "dataset: colonne folder_id alignee sur count");
        // Extracteur nul : aucune donnee EXIF => les colonnes EXIF sont NULL (jamais 0),
        // et les dictionnaires de chaines restent vides (pas d'index bidon).
        CHECK(cols.value(QStringLiteral("iso")).toArray().at(0).isNull(),
              "dataset: EXIF absent -> null preserve (pas de 0 trompeur)");
        CHECK(cols.value(QStringLiteral("camera")).toArray().at(0).isNull(),
              "dataset: boitier absent -> index null");
        CHECK(ds.value(QStringLiteral("dictionaries")).toObject()
                .value(QStringLiteral("camera")).toArray().isEmpty(),
              "dataset: dictionnaire boitiers vide sans EXIF");
        // file_type est connu par le scan (pas besoin d'EXIF) : il doit s'interner.
        CHECK(!cols.value(QStringLiteral("file_type")).toArray().at(0).isNull(),
              "dataset: file_type present -> index non null");
        // Table des dossiers : id -> libelle (pour filtrer/etiqueter par dossier).
        const QJsonObject folders = ds.value(QStringLiteral("folders")).toObject();
        CHECK(folders.value(QString::number(fid)).toString() == photos,
              "dataset: folders mappe l'id du dossier vers son chemin");
    }

    // --- 4) Source distante perdue en cours de vie : ne pas confondre indisponible
    //         et supprimé. Une racine à part (montage réseau simulé) est indexée,
    //         puis rendue inaccessible : la passe suivante doit interrompre proprement
    //         SANS marquer aucun fichier disparu.
    const QString net = base + QStringLiteral("/net");   // racine « réseau » simulée
    QDir().mkpath(net);
    writeBytes(net + QStringLiteral("/x.jpg"), 100);
    writeBytes(net + QStringLiteral("/y.jpg"), 100);
    const int netFid = repo.addFolder(net, net, QVariant(), true, false, QVariant(), nowIso());
    // Indexeur dédié à cette racine ; timeout de sonde court (test déterministe).
    Indexer netIdx(&repo, nullptr, {net}, 300);
    netIdx.run(IndexMode::Full, {netFid}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "files_new") == 2, "source: 2 fichiers indexes au depart");
    CHECK(repo.latestRun(nullptr).value(QStringLiteral("state")).toString()
              == QLatin1String("done"), "source: premiere passe terminee (done)");

    // La racine disparaît (l'hote du partage s'endort / le montage tombe).
    QDir(net).removeRecursively();
    netIdx.run(IndexMode::Incremental, {netFid}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "files_missing") == 0,
          "source perdue -> AUCUN fichier marque disparu");
    CHECK(lastRunInt(repo, "files_unavailable") >= 1,
          "source perdue -> selection comptee indisponible");
    CHECK(repo.latestRun(nullptr).value(QStringLiteral("state")).toString()
              == QLatin1String("interrupted"),
          "source perdue -> passe interrompue (interrupted != done)");
    QVariantMap fNet; fNet["folder"] = netFid; fNet["state"] = QStringLiteral("present");
    CHECK(repo.listPhotos(fNet, 1, 10).value(QStringLiteral("total")).toInt() == 2,
          "source perdue -> fichiers deja acquis restent present");

    // Retour de la source : une passe ultérieure reprend normalement.
    QDir().mkpath(net);
    writeBytes(net + QStringLiteral("/x.jpg"), 100);
    writeBytes(net + QStringLiteral("/y.jpg"), 100);
    netIdx.run(IndexMode::Incremental, {netFid}, QStringLiteral("cli"));
    CHECK(repo.latestRun(nullptr).value(QStringLiteral("state")).toString()
              == QLatin1String("done"), "source revenue -> passe de nouveau normale (done)");

    // --- 5) Support AMOVIBLE (CD/DVD, archive) : le retrait du support ne vaut
    //         JAMAIS suppression. On indexe un « CD », puis on le « retire » en
    //         laissant le point de montage présent mais VIDE (cas qui, sans la
    //         protection removable, ferait marquer disparue toute l'archive et la
    //         sortirait de morfAnalytics). Les fichiers doivent rester 'present'.
    const QString cd = base + QStringLiteral("/cd");   // « point de montage » du CD
    QDir().mkpath(cd);
    writeBytes(cd + QStringLiteral("/p1.jpg"), 100);
    writeBytes(cd + QStringLiteral("/p2.arw"), 100);
    // Racine du CD = le point de montage lui-même ; sélection déclarée AMOVIBLE.
    const int cdFid = repo.addFolder(cd, cd, QVariant(QStringLiteral("Archive CD")),
                                     true, true, QVariant(QStringLiteral("PHOTOS-2015")), nowIso());
    CHECK(cdFid > 0, "addFolder amovible (CD)");
    Indexer cdIdx(&repo, nullptr, {cd}, 300);
    cdIdx.run(IndexMode::Full, {cdFid}, QStringLiteral("cli"));
    QVariantMap fCdPresent; fCdPresent["folder"] = cdFid; fCdPresent["state"] = QStringLiteral("present");
    CHECK(repo.listPhotos(fCdPresent, 1, 10).value(QStringLiteral("total")).toInt() == 2,
          "CD present -> 2 photos indexees");

    // Le libellé de volume est bien porté par la sélection (retrouver le disque).
    {
        bool foundCd = false;
        const QJsonArray fdet = repo.listFoldersDetail();
        for (const QJsonValue& v : fdet) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toInt() == cdFid) {
                foundCd = o.value(QStringLiteral("removable")).toInt() == 1 &&
                          o.value(QStringLiteral("volume_label")).toString() == QLatin1String("PHOTOS-2015");
            }
        }
        CHECK(foundCd, "CD: detail expose removable=1 et volume_label");
    }

    // ÉJECTION : le point de montage reste là mais devient VIDE (fichiers retirés).
    QFile::remove(cd + QStringLiteral("/p1.jpg"));
    QFile::remove(cd + QStringLiteral("/p2.arw"));
    cdIdx.run(IndexMode::Incremental, {cdFid}, QStringLiteral("cli"));
    CHECK(lastRunInt(repo, "files_missing") == 0,
          "CD retire (montage vide) -> AUCUN fichier marque disparu");
    CHECK(repo.listPhotos(fCdPresent, 1, 10).value(QStringLiteral("total")).toInt() == 2,
          "CD retire -> les 2 photos restent 'present' (persistance morfAnalytics)");

    // Persistance à travers un « reboot » : fermer puis rouvrir la base (le fichier
    // SQLite survit ; morfAnalytics re-lira ces photos). Support toujours absent.
    repo.close();
    PhotoRepository repo2(QStringLiteral("smoke:reboot"));
    CHECK(repo2.open(base + QStringLiteral("/db.sqlite")), "reouverture base (simulation reboot)");
    CHECK(repo2.listPhotos(fCdPresent, 1, 10).value(QStringLiteral("total")).toInt() == 2,
          "apres reboot -> photos du CD toujours presentes en base");

    // --- 6) Exclusion des ANALYSES sans effacer les donnees ---
    const int dsBefore = repo2.photoDataset().value(QStringLiteral("count")).toInt();
    repo2.setFolderAnalyticsExcluded(cdFid, true);
    const int dsExcluded = repo2.photoDataset().value(QStringLiteral("count")).toInt();
    CHECK(dsExcluded == dsBefore - 2,
          "exclusion analytique -> dataset perd les 2 photos du CD");
    CHECK(repo2.listPhotos(fCdPresent, 1, 10).value(QStringLiteral("total")).toInt() == 2,
          "exclusion analytique -> donnees CONSERVEES en base (non destructif)");
    repo2.setFolderAnalyticsExcluded(cdFid, false);
    CHECK(repo2.photoDataset().value(QStringLiteral("count")).toInt() == dsBefore,
          "reintegration analytique -> dataset revenu a l'identique");

    // --- 7) Purge DEFINITIVE (irreversible) ---
    const int purged = repo2.purgeFolder(cdFid);
    CHECK(purged == 2, "purge du dossier CD -> 2 fichiers effaces");
    CHECK(repo2.listPhotos(fCdPresent, 1, 10).value(QStringLiteral("total")).toInt() == 0,
          "purge -> plus aucune ligne pour ce dossier (suppression reelle)");
    {
        bool cdGone = true;
        (void)repo2.folderIdByPath(cd, nullptr);
        for (const QJsonValue& v : repo2.listFoldersDetail())
            if (v.toObject().value(QStringLiteral("id")).toInt() == cdFid) cdGone = false;
        CHECK(cdGone, "purge du dossier -> la selection elle-meme est retiree");
    }
    // Purge totale : remise a zero complete du domaine.
    repo2.purgeAll();
    CHECK(repo2.summary().value(QStringLiteral("files_total")).toInt() == 0 &&
          repo2.listFoldersDetail().isEmpty(),
          "purge totale -> base videe (fichiers + selections)");

    repo2.close();
    QDir(base).removeRecursively();

    std::printf(failures ? "\n%d test(s) en echec\n" : "\nTous les tests passent\n", failures);
    return failures ? 1 : 0;
}
