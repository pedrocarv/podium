#include "package.h"
#include "xml.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <atomic>

#include <zip.h>

namespace {

QString zipError(int code)
{
    zip_error_t e;
    zip_error_init_with_code(&e, code);
    QString message = str(zip_error_strerror(&e));
    zip_error_fini(&e);
    return message;
}

bool alreadyCompressed(const QString &part)
{
    static const QSet<QString> exts{"png", "jpg", "jpeg", "gif", "webp", "wdp", "mp4", "m4v", "mov", "mp3", "m4a"};
    return exts.contains(QFileInfo(part).suffix().toLower());
}

} // namespace

std::shared_ptr<Package> Package::openZip(const QString &path, QString *error)
{
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &code);
    if (!archive) {
        if (error) *error = zipError(code);
        return nullptr;
    }
    std::shared_ptr<Package> p(new Package);
    p->archive = archive;
    for (zip_int64_t i = 0, n = zip_get_num_entries(archive, 0); i < n; ++i) {
        QString name = str(zip_get_name(archive, zip_uint64_t(i), ZIP_FL_ENC_GUESS));
        if (!name.isEmpty() && !name.endsWith('/')) p->index.insert(name, i);
    }
    p->readContentTypes();
    return p;
}

std::shared_ptr<Package> Package::openDir(const QString &dir)
{
    std::shared_ptr<Package> p(new Package);
    QDir root(dir);
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (f.open(QIODevice::ReadOnly)) p->memory.insert(root.relativeFilePath(f.fileName()), f.readAll());
    }
    p->readContentTypes();
    return p;
}

Package::Package() : id([] {
    static std::atomic<quint64> next{1};
    return next++;
}())
{
}

Package::~Package()
{
    if (archive) zip_discard(archive);
}

void Package::readContentTypes()
{
    pugi::xml_document doc;
    if (!loadXml(doc, read("[Content_Types].xml"))) return;
    for (auto n : doc.document_element().children()) {
        if (str(n.name()) == "Default")
            defaults.insert(str(n.attribute("Extension")).toLower(), str(n.attribute("ContentType")));
        else if (str(n.name()) == "Override")
            overrides.insert(str(n.attribute("PartName")).mid(1), str(n.attribute("ContentType")));
    }
}

bool Package::contains(const QString &part) const
{
    QMutexLocker lock(&mutex);
    return memory.contains(part) || index.contains(part);
}

QByteArray Package::read(const QString &part) const
{
    QMutexLocker lock(&mutex);
    if (auto m = memory.constFind(part); m != memory.cend()) return *m;
    auto i = index.constFind(part);
    if (i == index.cend()) return {};
    zip_stat_t st;
    if (zip_stat_index(archive, zip_uint64_t(*i), 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE)) return {};
    zip_file_t *f = zip_fopen_index(archive, zip_uint64_t(*i), 0);
    if (!f) return {};
    QByteArray data(qsizetype(st.size), Qt::Uninitialized);
    zip_int64_t got = zip_fread(f, data.data(), st.size);
    zip_fclose(f);
    return got == zip_int64_t(st.size) ? data : QByteArray();
}

QStringList Package::parts() const
{
    QMutexLocker lock(&mutex);
    QStringList all = index.keys() + memory.keys();
    all.removeDuplicates();
    return all;
}

QList<Rel> Package::rels(const QString &part) const
{
    QList<Rel> out;
    pugi::xml_document doc;
    if (!loadXml(doc, read(relsName(part)))) return out;
    for (auto r : doc.document_element().children("Relationship")) {
        Rel rel{str(r.attribute("Id")), str(r.attribute("Type")), str(r.attribute("Target")),
                str(r.attribute("TargetMode")) == "External"};
        if (!rel.external) rel.target = resolve(part, rel.target);
        out.append(rel);
    }
    return out;
}

QString Package::contentType(const QString &part) const
{
    if (auto o = overrides.constFind(part); o != overrides.cend()) return *o;
    return defaults.value(QFileInfo(part).suffix().toLower());
}

QString Package::add(const QString &hint, const QByteArray &data)
{
    QMutexLocker lock(&mutex);
    QFileInfo info(hint);
    QString name = hint;
    for (int n = 2; index.contains(name) || (memory.contains(name) && memory[name] != data); ++n)
        name = QString("%1/%2-%3.%4").arg(info.path(), info.completeBaseName()).arg(n).arg(info.suffix());
    memory.insert(name, data);
    return name;
}

bool Package::write(const QString &path, const QList<std::pair<QString, QByteArray>> &written,
                    const QStringList &copied, QString *error) const
{
    QMutexLocker lock(&mutex);
    int code = 0;
    zip_t *out = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &code);
    if (!out) {
        if (error) *error = zipError(code);
        return false;
    }
    auto add = [&](const QString &name, zip_source_t *src, zip_int32_t method) {
        if (!src) return false;
        zip_int64_t i = zip_file_add(out, name.toUtf8().constData(), src, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (i < 0) {
            zip_source_free(src);
            return false;
        }
        zip_set_file_compression(out, zip_uint64_t(i), method, 0);
        return true;
    };
    bool ok = true;
    for (const auto &[name, data] : written)
        ok = ok && add(name, zip_source_buffer(out, data.constData(), zip_uint64_t(data.size()), 0), ZIP_CM_DEFLATE);
    for (const QString &name : copied) {
        if (auto m = memory.constFind(name); m != memory.cend()) {
            ok = ok && add(name, zip_source_buffer(out, m->constData(), zip_uint64_t(m->size()), 0),
                           alreadyCompressed(name) ? ZIP_CM_STORE : ZIP_CM_DEFLATE);
        } else if (auto i = index.constFind(name); i != index.cend()) {
            // Keep the entry's compression so libzip copies the compressed bytes instead of recompressing.
            zip_stat_t st;
            zip_stat_index(archive, zip_uint64_t(*i), 0, &st);
            ok = ok && add(name, zip_source_zip_file(out, archive, zip_uint64_t(*i), ZIP_FL_COMPRESSED, 0, -1, nullptr),
                           st.comp_method);
        }
    }
    if (!ok || zip_close(out) < 0) {
        if (error) *error = str(zip_strerror(out));
        zip_discard(out);
        return false;
    }
    return true;
}

QString Package::relsName(const QString &part)
{
    int slash = part.lastIndexOf('/');
    return part.left(slash + 1) + "_rels/" + part.mid(slash + 1) + ".rels";
}

QString Package::resolve(const QString &fromPart, const QString &target)
{
    if (target.startsWith('/')) return target.mid(1);
    QString dir = fromPart.left(fromPart.lastIndexOf('/') + 1);
    return QDir::cleanPath(dir + target);
}

QString Package::relative(const QString &fromPart, const QString &toPart)
{
    QStringList from = fromPart.split('/'), to = toPart.split('/');
    from.removeLast();
    qsizetype common = 0;
    while (common < from.size() && common < to.size() - 1 && from[common] == to[common]) ++common;
    QStringList out;
    for (qsizetype i = common; i < from.size(); ++i) out << "..";
    out << to.mid(common);
    return out.join('/');
}

QByteArray Package::relsXml(const QList<Rel> &rels, const QString &fromPart)
{
    pugi::xml_document doc;
    auto root = doc.append_child("Relationships");
    root.append_attribute("xmlns") = "http://schemas.openxmlformats.org/package/2006/relationships";
    for (const Rel &r : rels) {
        auto n = root.append_child("Relationship");
        n.append_attribute("Id") = utf8(r.id).c_str();
        n.append_attribute("Type") = utf8(r.type).c_str();
        n.append_attribute("Target") = utf8(r.external ? r.target : relative(fromPart, r.target)).c_str();
        if (r.external) n.append_attribute("TargetMode") = "External";
    }
    return serialize(doc);
}
