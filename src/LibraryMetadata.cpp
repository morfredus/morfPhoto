/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "morfphoto/LibraryMetadata.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QXmlStreamReader>

#include <algorithm>
#include <vector>

namespace morfphoto {

namespace {

// Un sidecar au-dela de cette taille n'est pas lu : un XMP de photo tient en quelques
// dizaines de Ko (jusqu'a quelques Mo avec l'historique de retouche complet).
constexpr qint64 kMaxXmpBytes = 16 * 1024 * 1024;

// Prefixe CANONIQUE d'un espace de noms XMP. Les documents choisissent leurs prefixes
// librement (« xap » pour « xmp » dans de vieux fichiers) : on se fie a l'URI.
QString canonicalPrefix(const QString& ns) {
    static const QHash<QString, QString> table{
        {QStringLiteral("http://ns.adobe.com/xap/1.0/"), QStringLiteral("xmp")},
        {QStringLiteral("http://purl.org/dc/elements/1.1/"), QStringLiteral("dc")},
        {QStringLiteral("http://ns.adobe.com/lightroom/1.0/"), QStringLiteral("lr")},
        {QStringLiteral("http://darktable.sf.net/"), QStringLiteral("darktable")},
        {QStringLiteral("http://ns.adobe.com/exif/1.0/"), QStringLiteral("exif")},
        {QStringLiteral("http://ns.adobe.com/camera-raw-settings/1.0/"), QStringLiteral("crs")},
        {QStringLiteral("http://www.digikam.org/ns/1.0/"), QStringLiteral("digiKam")},
        {QStringLiteral("http://ns.lightcraft.app/lc/1.0/"), QStringLiteral("lc")},
    };
    return table.value(ns);
}

constexpr auto kRdfNs = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

QString keyFor(const QString& ns, const QString& local) {
    const QString prefix = canonicalPrefix(ns);
    return prefix.isEmpty() ? QString() : prefix + QLatin1Char(':') + local;
}

// Valeur numerique de position XMP : « 48,51.123N », « 48,51,7N » ou decimal.
QVariant parseGps(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return {};
    int sign = 1;
    const QChar last = s.at(s.size() - 1).toUpper();
    if (last == QLatin1Char('N') || last == QLatin1Char('E') || last == QLatin1Char('S')
        || last == QLatin1Char('W')) {
        if (last == QLatin1Char('S') || last == QLatin1Char('W'))
            sign = -1;
        s.chop(1);
    }
    const QStringList parts = s.split(QLatin1Char(','));
    double value = 0;
    double divisor = 1;
    for (const QString& part : parts) {
        bool ok = false;
        const double n = part.trimmed().toDouble(&ok);
        if (!ok)
            return {};
        value += n / divisor;
        divisor *= 60;
    }
    if (parts.isEmpty())
        return {};
    return sign * value;
}

QStringList cleaned(const QStringList& in) {
    QStringList out;
    for (const QString& v : in) {
        const QString t = v.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}

// Les mots-cles internes de darktable (format, changed, auto-applied...) ne sont pas
// des mots-cles de la personne : ils ne doivent jamais ressortir.
bool isDarktableInternalKeyword(const QString& k) {
    // Teste apres normalisation : les niveaux sont separes par « | » avec espaces.
    return k == QLatin1String("darktable") || k.startsWith(QLatin1String("darktable | "));
}

QVariant nonEmpty(const QString& s) {
    const QString t = s.trimmed();
    return t.isEmpty() ? QVariant() : QVariant(t);
}

// Valeur d'un tag ExifTool (JSON) : une chaine ou une liste de chaines.
QStringList tagList(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    QStringList out;
    if (v.isString())
        out << v.toString();
    else if (v.isArray())
        for (const QJsonValue& x : v.toArray())
            out << (x.isString() ? x.toString() : QString::number(x.toDouble()));
    else if (v.isDouble())
        out << QString::number(v.toDouble());
    return out;
}

} // namespace

// --- Etiquettes ---------------------------------------------------------------

QStringList labelNames(int mask) {
    QStringList out;
    if (mask & LabelRed)    out << QStringLiteral("red");
    if (mask & LabelYellow) out << QStringLiteral("yellow");
    if (mask & LabelGreen)  out << QStringLiteral("green");
    if (mask & LabelBlue)   out << QStringLiteral("blue");
    if (mask & LabelPurple) out << QStringLiteral("purple");
    return out;
}

int labelMaskFromName(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("red"))    return LabelRed;
    if (n == QLatin1String("yellow")) return LabelYellow;
    if (n == QLatin1String("green"))  return LabelGreen;
    if (n == QLatin1String("blue"))   return LabelBlue;
    if (n == QLatin1String("purple") || n == QLatin1String("violet")) return LabelPurple;
    return 0;
}

QStringList knownProviders() {
    return {QStringLiteral("darktable"), QStringLiteral("xmp"), QStringLiteral("dxo"), QStringLiteral("lightcraft")};
}

QString normalizeKeyword(const QString& raw, QChar separator) {
    QStringList levels;
    for (const QString& level : raw.split(separator))
        if (!level.trimmed().isEmpty())
            levels << level.trimmed();
    return levels.join(QStringLiteral(" | "));
}

// --- Rattachement des sidecars a une image -------------------------------------

QVector<SidecarRef> matchSidecars(const QString& imageFilename, const QString& directory,
                                  const QVector<SidecarFile>& sidecars,
                                  const QSet<QString>& imagesLower) {
    const int dot = imageFilename.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? imageFilename.left(dot) : imageFilename;
    const QString ext  = dot > 0 ? imageFilename.mid(dot) : QString();   // « .ARW »
    const QString full = imageFilename.toLower() + QStringLiteral(".xmp");   // IMG.arw.xmp
    const QString shared = stem.toLower() + QStringLiteral(".xmp");          // img.xmp
    // Copies darktable : IMG_02.ARW.xmp (le numero est la version). Teste a la main, sans
    // expression reguliere : une par image, sur un dossier de milliers de photos, couterait.
    const QString copyPrefix = stem.toLower() + QLatin1Char('_');
    const QString copySuffix = ext.toLower() + QStringLiteral(".xmp");

    const QString dopName = imageFilename.toLower() + QStringLiteral(".dop");   // IMG.arw.dop (DxO)
    const SidecarFile* dop = nullptr;
    const SidecarFile* own = nullptr;       // IMG.ARW.xmp
    const SidecarFile* common = nullptr;    // IMG.xmp
    QVector<QPair<int, const SidecarFile*>> copies;
    for (const SidecarFile& s : sidecars) {
        const QString lower = s.name.toLower();
        if (lower == dopName) {
            dop = &s;
        } else if (lower == full) {
            own = &s;
        } else if (lower == shared) {
            common = &s;
        } else {
            if (!lower.startsWith(copyPrefix) || !lower.endsWith(copySuffix)
                || lower.size() <= copyPrefix.size() + copySuffix.size())
                continue;
            const QString digits = lower.mid(copyPrefix.size(), lower.size() - copyPrefix.size() - copySuffix.size());
            bool ok = false;
            const int version = digits.toInt(&ok);
            if (!ok || version <= 0 || digits.at(0) == QLatin1Char('+') || digits.at(0) == QLatin1Char('-'))
                continue;
            // `IMG_01.ARW` est elle-meme une image du dossier : ce sidecar est le sien.
            if (imagesLower.contains(copyPrefix + digits + ext.toLower()))
                continue;
            copies.append({version, &s});
        }
    }

    auto ref = [&directory](const SidecarFile& s, int version) {
        SidecarRef r;
        r.path = directory + QLatin1Char('/') + s.name;
        r.name = s.name;
        r.version = version;
        r.size = s.size;
        r.mtime = s.mtime;
        return r;
    };
    QVector<SidecarRef> out;
    if (own)
        out.append(ref(*own, 0));
    else if (common)
        out.append(ref(*common, 0));
    if (dop)
        out.append(ref(*dop, 0));   // DxO : toujours la version 0 (copies non lues, format a confirmer)
    for (const auto& c : copies)
        out.append(ref(*c.second, c.first));
    std::sort(out.begin(), out.end(), [](const SidecarRef& a, const SidecarRef& b) {
        return a.version < b.version;
    });
    return out;
}

// --- Lecture XMP ----------------------------------------------------------------

XmpDocument parseXmp(const QByteArray& bytes) {
    XmpDocument doc;
    if (bytes.isEmpty() || bytes.size() > kMaxXmpBytes)
        return doc;

    QXmlStreamReader r(bytes);
    int depth = 0;
    int descDepth = -1;          // profondeur du premier rdf:Description
    bool inProp = false;
    int propDepth = 0;
    QString propKey;
    QString propText;
    bool propHadItems = false;
    bool inItem = false;
    int itemDepth = 0;
    QString itemText;

    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType tok = r.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            ++depth;
            const QString ns = r.namespaceUri().toString();
            const QString local = r.name().toString();
            if (descDepth < 0 && ns == QLatin1String(kRdfNs) && local == QLatin1String("Description")) {
                descDepth = depth;
                doc.ok = true;
                // Forme abregee : les proprietes sont des attributs de rdf:Description.
                for (const QXmlStreamAttribute& a : r.attributes()) {
                    const QString key = keyFor(a.namespaceUri().toString(), a.name().toString());
                    if (!key.isEmpty())
                        doc.props[key] << a.value().toString();
                }
            } else if (descDepth >= 0 && depth == descDepth + 1) {
                // Forme developpee : un element enfant par propriete.
                propKey = keyFor(ns, local);
                inProp = !propKey.isEmpty();
                propDepth = depth;
                propText.clear();
                propHadItems = false;
            } else if (inProp && ns == QLatin1String(kRdfNs) && local == QLatin1String("li")) {
                propHadItems = true;
                inItem = true;
                itemDepth = depth;
                itemText.clear();
            }
        } else if (tok == QXmlStreamReader::Characters) {
            if (r.isWhitespace())
                continue;
            if (inItem)
                itemText += r.text().toString();
            else if (inProp && depth == propDepth)
                propText += r.text().toString();
        } else if (tok == QXmlStreamReader::EndElement) {
            if (inItem && depth == itemDepth) {
                doc.props[propKey] << itemText.trimmed();
                inItem = false;
            }
            if (inProp && depth == propDepth) {
                if (!propHadItems)
                    doc.props[propKey] << propText.trimmed();
                else
                    doc.props[propKey];   // un sac vide reste « present » (liste vide connue)
                inProp = false;
            }
            --depth;
        }
    }
    // Un document tronque garde ce qui a ete lu : mieux qu'un sidecar ignore en bloc.
    if (!doc.ok)
        doc.props.clear();
    return doc;
}

ProviderRead readSidecarXmp(const XmpDocument& doc, int version) {
    ProviderRead r;
    r.version = version;
    if (!doc.ok)
        return r;
    const bool dt = doc.has(QStringLiteral("darktable:xmp_version"));
    r.source = dt ? QStringLiteral("darktable") : QStringLiteral("xmp");

    // Note : -1 = rejetee (convention darktable et XMP). Un rejet ne dit rien des etoiles.
    bool ratingOk = false;
    const int rating = doc.first(QStringLiteral("xmp:Rating")).toInt(&ratingOk);
    if (ratingOk) {
        if (rating < 0) {
            r.rejected = true;
        } else if (rating <= 5) {
            r.rating = rating;
            if (dt)
                r.rejected = false;   // darktable code le rejet dans la note : 0..5 = non rejetee
        }
    }

    // Etiquettes de couleur : darktable:colorlabels (0..4) et/ou xmp:Label (nom).
    r.hasLabels = dt || doc.has(QStringLiteral("darktable:colorlabels")) || doc.has(QStringLiteral("xmp:Label"));
    for (const QString& v : doc.props.value(QStringLiteral("darktable:colorlabels"))) {
        bool ok = false;
        const int n = v.toInt(&ok);
        if (ok && n >= 0 && n <= 4)
            r.labels |= (1 << n);
    }
    r.labels |= labelMaskFromName(doc.first(QStringLiteral("xmp:Label")));

    // Mots-cles : hierarchiques d'abord, puis digiKam, puis la liste a plat.
    QStringList keywords;
    if (doc.has(QStringLiteral("lr:hierarchicalSubject"))) {
        for (const QString& v : doc.props.value(QStringLiteral("lr:hierarchicalSubject")))
            keywords << normalizeKeyword(v, QLatin1Char('|'));
        r.hasKeywords = true;
    } else if (doc.has(QStringLiteral("digiKam:TagsList"))) {
        for (const QString& v : doc.props.value(QStringLiteral("digiKam:TagsList")))
            keywords << normalizeKeyword(v, QLatin1Char('/'));
        r.hasKeywords = true;
    } else if (doc.has(QStringLiteral("dc:subject"))) {
        for (const QString& v : doc.props.value(QStringLiteral("dc:subject")))
            keywords << normalizeKeyword(v, QLatin1Char('|'));
        r.hasKeywords = true;
    }
    // Un sidecar darktable fait foi sur ses mots-cles, meme vides : un mot-cle retire
    // ne doit pas revenir depuis l'image.
    if (dt)
        r.hasKeywords = true;
    for (const QString& k : cleaned(keywords))
        if (!isDarktableInternalKeyword(k))
            r.keywords << k;

    r.title       = nonEmpty(doc.first(QStringLiteral("dc:title")));
    r.description = nonEmpty(doc.first(QStringLiteral("dc:description")));
    r.creator     = nonEmpty(cleaned(doc.props.value(QStringLiteral("dc:creator"))).join(QStringLiteral(", ")));
    r.rights      = nonEmpty(doc.first(QStringLiteral("dc:rights")));
    r.name        = nonEmpty(doc.first(QStringLiteral("darktable:version_name")));

    // Retouchee ?
    if (dt) {
        const QString current = doc.first(QStringLiteral("darktable:history_current_hash"));
        if (!current.isEmpty())
            r.edited = current != doc.first(QStringLiteral("darktable:history_basic_hash"))
                    && current != doc.first(QStringLiteral("darktable:history_auto_hash"));
    } else if (doc.has(QStringLiteral("crs:HasSettings"))) {
        const QString v = doc.first(QStringLiteral("crs:HasSettings")).toLower();
        if (v == QLatin1String("true"))       r.edited = true;
        else if (v == QLatin1String("false")) r.edited = false;
    }

    r.latitude  = parseGps(doc.first(QStringLiteral("exif:GPSLatitude")));
    r.longitude = parseGps(doc.first(QStringLiteral("exif:GPSLongitude")));
    if (r.latitude.isNull() || r.longitude.isNull()) {
        r.latitude = QVariant();
        r.longitude = QVariant();
    }
    return r;
}

ProviderRead readSidecarLightcraft(const XmpDocument& doc, int version) {
    ProviderRead r;
    r.version = version;
    if (!doc.ok || !doc.has(QStringLiteral("lc:flag")))
        return r;
    const QString flag = doc.first(QStringLiteral("lc:flag")).trimmed().toLower();
    // Le drapeau est a trois etats (pick, none, reject), tous confirmes sur de vrais fichiers : un seul etat a
    // la fois, donc « retenue » et « rejetee » se deduisent l'un de l'autre.
    if (flag == QLatin1String("pick")) {
        r.picked = true;
        r.rejected = false;
    } else if (flag == QLatin1String("none")) {
        r.picked = false;
        r.rejected = false;
    } else if (flag == QLatin1String("reject")) {
        r.picked = false;
        r.rejected = true;
    } else {
        return r;       // autre valeur : on ne devine pas, aucun fournisseur
    }
    r.source = QStringLiteral("lightcraft");
    return r;
}

// --- DxO PhotoLab : tables imbriquees « Nom = { ... }, » ------------------------------------

namespace {

// Noeud d'un .dop : une table (cles + enfants), une chaine, un nombre ou un booleen.
struct DopNode {
    enum Kind { Table, String, Number, Bool } kind = Table;
    QString str;
    double num = 0;
    bool boolean = false;
    std::vector<QString> keys;       // « » pour une entree sans nom
    std::vector<DopNode> children;
    const DopNode* find(const char* key) const {
        const QString k = QLatin1String(key);
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k)
                return &children[i];
        return nullptr;
    }
};

// Analyseur minimal, borne en profondeur et en taille : un fichier corrompu ou hostile ne doit
// jamais faire deborder la pile ni consommer la memoire.
class DopParser {
public:
    explicit DopParser(const QByteArray& bytes) : m_s(bytes) {}
    bool parse(DopNode* root) {
        skipSpace();
        // « Sidecar = { ... } » : on ignore le nom, on garde la table.
        if (!skipIdentifier()) return false;
        skipSpace();
        if (!take('=')) return false;
        return parseValue(root, 0);
    }
private:
    static constexpr int kMaxDepth = 64;
    static constexpr int kMaxNodes = 2000000;
    const QByteArray& m_s;
    int m_pos = 0;
    int m_nodes = 0;

    bool atEnd() const { return m_pos >= m_s.size(); }
    char peek() const { return atEnd() ? '\0' : m_s.at(m_pos); }
    void skipSpace() { while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n')) ++m_pos; }
    bool take(char c) { if (peek() != c) return false; ++m_pos; return true; }
    static bool isIdent(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
    bool skipIdentifier() { const int start = m_pos; while (!atEnd() && isIdent(peek())) ++m_pos; return m_pos > start; }

    bool parseString(QString* out) {
        if (!take('"')) return false;
        QByteArray raw;
        while (!atEnd() && peek() != '"') {
            if (peek() == '\\' && m_pos + 1 < m_s.size()) ++m_pos;   // l'echappement garde le caractere suivant
            raw.append(m_s.at(m_pos++));
        }
        if (!take('"')) return false;
        *out = QString::fromUtf8(raw);
        return true;
    }

    bool parseValue(DopNode* node, int depth) {
        if (depth > kMaxDepth || ++m_nodes > kMaxNodes) return false;
        skipSpace();
        const char c = peek();
        if (c == '{') {
            ++m_pos;
            node->kind = DopNode::Table;
            for (;;) {
                skipSpace();
                if (atEnd()) return false;
                if (take('}')) return true;
                if (take(',')) continue;        // separateur : la virgule peut etre seule sur sa ligne
                // Une entree : « Nom = valeur » ou une valeur seule.
                QString key;
                const int save = m_pos;
                if (isIdent(peek())) {
                    const int start = m_pos;
                    skipIdentifier();
                    const int end = m_pos;
                    skipSpace();
                    if (peek() == '=') {
                        ++m_pos;
                        key = QString::fromLatin1(m_s.mid(start, end - start));
                    } else {
                        m_pos = save;         // c'etait un nombre ou un booleen
                    }
                }
                DopNode child;
                if (!parseValue(&child, depth + 1)) return false;
                node->keys.push_back(key);
                node->children.push_back(std::move(child));
            }
        }
        if (c == '"') {
            node->kind = DopNode::String;
            return parseString(&node->str);
        }
        // nombre ou booleen
        const int start = m_pos;
        while (!atEnd() && (isIdent(peek()) || peek() == '.' || peek() == '-' || peek() == '+')) ++m_pos;
        if (m_pos == start) return false;
        const QByteArray tok = m_s.mid(start, m_pos - start);
        if (tok == "true" || tok == "false") {
            node->kind = DopNode::Bool;
            node->boolean = tok == "true";
            return true;
        }
        bool ok = false;
        node->num = tok.toDouble(&ok);
        node->kind = DopNode::Number;
        return ok;
    }
};

} // namespace

ProviderRead readSidecarDop(const QByteArray& bytes, int version) {
    ProviderRead r;
    r.version = version;
    if (bytes.isEmpty() || bytes.size() > kMaxXmpBytes)
        return r;
    DopNode root;
    DopParser parser(bytes);
    if (!parser.parse(&root))
        return r;
    // Sidecar = { Source = { Items = { { ...image... } } } }
    const DopNode* source = root.find("Source");
    const DopNode* items = source ? source->find("Items") : nullptr;
    if (!items || items->children.empty() || items->children.front().kind != DopNode::Table)
        return r;
    const DopNode& item = items->children.front();
    r.source = QStringLiteral("dxo");

    if (const DopNode* n = item.find("Rating"); n && n->kind == DopNode::Number) {
        const int rating = qRound(n->num);
        if (rating >= 0 && rating <= 5)
            r.rating = rating;
    }
    // Mots-cles : une table de tables, chaque table interne est UN mot-cle hierarchique (un niveau
    // par chaine). Les etiquettes internes de darktable importees par PhotoLab sont retirees.
    if (const DopNode* kw = item.find("Keywords"); kw && kw->kind == DopNode::Table) {
        r.hasKeywords = true;
        QStringList keywords;
        for (const DopNode& k : kw->children) {
            QStringList levels;
            if (k.kind == DopNode::String) {
                levels << k.str;
            } else if (k.kind == DopNode::Table) {
                for (const DopNode& level : k.children)
                    if (level.kind == DopNode::String)
                        levels << level.str;
            }
            const QString joined = normalizeKeyword(levels.join(QLatin1Char('|')), QLatin1Char('|'));
            if (!joined.isEmpty())
                keywords << joined;
        }
        for (const QString& k : cleaned(keywords))
            if (!isDarktableInternalKeyword(k))
                r.keywords << k;
    }
    // Position : decimale, directement.
    const DopNode* lat = item.find("GPSLatitude");
    const DopNode* lon = item.find("GPSLongitude");
    if (lat && lon && lat->kind == DopNode::Number && lon->kind == DopNode::Number
        && lat->num >= -90 && lat->num <= 90 && lon->num >= -180 && lon->num <= 180) {
        r.latitude = lat->num;
        r.longitude = lon->num;
    }
    if (const DopNode* iptc = item.find("IPTC")) {
        if (const DopNode* rights = iptc->find("statusCopyrightNotice"); rights && rights->kind == DopNode::String)
            r.rights = nonEmpty(rights->str);
    }
    return r;
}

ProviderRead readEmbedded(const QJsonObject& raw) {
    ProviderRead r;
    r.version = 0;
    bool any = false;

    bool ratingOk = false;
    const QJsonValue rv = raw.value(QStringLiteral("Rating"));
    if (rv.isDouble()) {
        const int rating = qRound(rv.toDouble());
        ratingOk = true;
        if (rating < 0)           r.rejected = true;
        else if (rating <= 5)     r.rating = rating;
        else                      ratingOk = false;
    }
    any = any || ratingOk;

    const QStringList label = tagList(raw, "Label");
    if (!label.isEmpty()) {
        r.hasLabels = true;
        r.labels = labelMaskFromName(label.first());
        any = true;
    }

    QStringList keywords;
    if (raw.contains(QStringLiteral("HierarchicalSubject"))) {
        for (const QString& v : tagList(raw, "HierarchicalSubject"))
            keywords << normalizeKeyword(v, QLatin1Char('|'));
        r.hasKeywords = true;
    } else if (raw.contains(QStringLiteral("Subject"))) {
        for (const QString& v : tagList(raw, "Subject"))
            keywords << normalizeKeyword(v, QLatin1Char('|'));
        r.hasKeywords = true;
    } else if (raw.contains(QStringLiteral("Keywords"))) {
        for (const QString& v : tagList(raw, "Keywords"))
            keywords << normalizeKeyword(v, QLatin1Char('|'));
        r.hasKeywords = true;
    }
    for (const QString& k : cleaned(keywords))
        if (!isDarktableInternalKeyword(k))
            r.keywords << k;
    any = any || r.hasKeywords;

    r.title       = nonEmpty(tagList(raw, "Title").value(0));
    r.description = nonEmpty(tagList(raw, "Description").value(0));
    r.creator     = nonEmpty(cleaned(tagList(raw, "Creator")).join(QStringLiteral(", ")));
    r.rights      = nonEmpty(tagList(raw, "Rights").value(0));
    any = any || !r.title.isNull() || !r.description.isNull() || !r.creator.isNull()
              || !r.rights.isNull();

    if (any)
        r.source = embeddedProviderId();
    return r;
}

// --- Fusion ---------------------------------------------------------------------

LibraryResult mergeReads(const QVector<ProviderRead>& reads, const QStringList& priority) {
    QStringList order = priority;
    order << embeddedProviderId();
    auto rank = [&order](const QString& source) {
        const int i = order.indexOf(source);
        return i < 0 ? order.size() : i;
    };

    // Lectures triees par priorite (a priorite egale, l'ordre d'arrivee est conserve).
    QVector<ProviderRead> sorted = reads;
    std::stable_sort(sorted.begin(), sorted.end(), [&rank](const ProviderRead& a, const ProviderRead& b) {
        return rank(a.source) < rank(b.source);
    });

    QSet<int> versionNumbers;
    for (const ProviderRead& r : sorted)
        versionNumbers.insert(r.version);
    QList<int> numbers(versionNumbers.begin(), versionNumbers.end());
    std::sort(numbers.begin(), numbers.end());

    LibraryResult out;
    // Fournisseur principal : le plus prioritaire present (`file` seulement s'il est seul).
    for (const ProviderRead& r : sorted) {
        if (r.source != embeddedProviderId()) { out.mainSource = r.source; break; }
    }
    if (out.mainSource.isEmpty() && !sorted.isEmpty())
        out.mainSource = embeddedProviderId();
    for (const int n : numbers) {
        LibraryVersion v;
        v.version = n;
        v.isCopy = n > 0;
        bool labelsDone = false;
        bool keywordsDone = false;
        bool anyEdited = false;
        bool anyNotEdited = false;
        QString editedSource, notEditedSource;
        bool anyRejected = false;
        bool anyNotRejected = false;
        QString rejectedSource, notRejectedSource;

        // Premier fournisseur (par priorite) dont la valeur n'est pas null.
        auto take = [&v](QVariant& target, const QVariant& value, const char* field, const QString& source) {
            if (target.isNull() && !value.isNull()) {
                target = value;
                v.sources.insert(QLatin1String(field), source);
            }
        };

        for (const ProviderRead& r : sorted) {
            if (r.version != n)
                continue;
            take(v.name, r.name, "name", r.source);
            take(v.rating, r.rating, "rating", r.source);
            take(v.picked, r.picked, "picked", r.source);
            // Rejetee : vraie si UN fournisseur la dit rejetee (un rejet, dans un logiciel, est un geste qu'un
            // autre logiciel ne sait pas defaire) ; fausse si au moins un la dit non rejetee sans contradiction.
            if (!r.rejected.isNull()) {
                if (r.rejected.toBool()) {
                    anyRejected = true;
                    if (rejectedSource.isEmpty()) rejectedSource = r.source;          // le premier qui rejette
                } else {
                    anyNotRejected = true;
                    if (notRejectedSource.isEmpty()) notRejectedSource = r.source;    // le premier qui dit non rejetee
                }
            }
            take(v.title, r.title, "title", r.source);
            take(v.description, r.description, "description", r.source);
            take(v.creator, r.creator, "creator", r.source);
            take(v.rights, r.rights, "rights", r.source);
            if (!labelsDone && r.hasLabels) {
                v.labels = r.labels;
                v.sources.insert(QStringLiteral("color_labels"), r.source);
                labelsDone = true;
            }
            if (!keywordsDone && r.hasKeywords) {
                v.keywords = r.keywords;
                v.sources.insert(QStringLiteral("keywords"), r.source);
                keywordsDone = true;
            }
            if (!r.edited.isNull()) {
                if (r.edited.toBool()) {
                    anyEdited = true;
                    if (editedSource.isEmpty()) editedSource = r.source;             // le premier qui dit retouchee
                } else {
                    anyNotEdited = true;
                    if (notEditedSource.isEmpty()) notEditedSource = r.source;       // le premier qui dit non retouchee
                }
            }
            // Position : le premier fournisseur qui en donne une (jamais l'EXIF : il
            // reste dans la table des fichiers, voir PhotoRepository).
            if (n == 0 && out.latitude.isNull() && !r.latitude.isNull() && !r.longitude.isNull()) {
                out.latitude = r.latitude;
                out.longitude = r.longitude;
                out.gpsSource = r.source;
            }
        }
        if (anyRejected) {
            v.rejected = true;
            v.sources.insert(QStringLiteral("rejected"), rejectedSource);
        } else if (anyNotRejected) {
            v.rejected = false;
            v.sources.insert(QStringLiteral("rejected"), notRejectedSource);
        }
        // retouchee : vrai si UN fournisseur le dit ; faux si au moins un le dit sans
        // contradiction ; inconnu sinon.
        if (anyEdited) {
            v.edited = true;
            v.sources.insert(QStringLiteral("edited"), editedSource);
        } else if (anyNotEdited) {
            v.edited = false;
            v.sources.insert(QStringLiteral("edited"), notEditedSource);
        }
        out.versions.append(v);
    }
    return out;
}

QString libraryFingerprint(const QVector<SidecarRef>& sidecars, const QStringList& providers) {
    QStringList parts;
    for (const SidecarRef& s : sidecars)
        parts << QStringLiteral("%1|%2|%3").arg(s.name).arg(s.size).arg(s.mtime);
    parts.sort();
    const QString key = QStringLiteral("lib2;") + providers.join(QLatin1Char(',')) + QLatin1Char(';')
                      + parts.join(QLatin1Char(';'));
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace morfphoto
