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
    const int fid = repo.addFolder(photos, photos, QVariant(), true, nowIso());
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
    const int fidBad = repo.addFolder(base + QStringLiteral("/autre"), photos, QVariant(), true, nowIso());
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

    repo.close();
    QDir(base).removeRecursively();

    std::printf(failures ? "\n%d test(s) en echec\n" : "\nTous les tests passent\n", failures);
    return failures ? 1 : 0;
}
