#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <memory>
#include <utility>

struct zip;

// An OPC relationship. `target` is an absolute part name (no leading '/') unless external.
struct Rel {
    QString id, type, target;
    bool external = false;
    bool is(QStringView kind) const { return type.endsWith(u'/' + kind.toString()); }
    bool operator==(const Rel &) const = default;
};

namespace rel {
inline const QString ns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
inline const QString slide = ns + "slide", slideLayout = ns + "slideLayout", slideMaster = ns + "slideMaster",
                     notesSlide = ns + "notesSlide", notesMaster = ns + "notesMaster", theme = ns + "theme",
                     image = ns + "image", hyperlink = ns + "hyperlink", video = ns + "video",
                     officeDocument = ns + "officeDocument",
                     media = "http://schemas.microsoft.com/office/2007/relationships/media";
} // namespace rel

// An OPC package (a .pptx zip, or a directory for the built-in template). Parts are read lazily and
// can be read from any thread. Parts added at runtime (inserted images) live in memory.
class Package {
public:
    static std::shared_ptr<Package> openZip(const QString &path, QString *error = nullptr);
    static std::shared_ptr<Package> openDir(const QString &dir);
    ~Package();

    bool contains(const QString &part) const;
    QByteArray read(const QString &part) const;
    QStringList parts() const;
    QList<Rel> rels(const QString &part) const;
    QString contentType(const QString &part) const;

    // Stores data under a fresh part name derived from `hint` (e.g. "ppt/media/image.png") and returns it.
    QString add(const QString &hint, const QByteArray &data);

    // Writes a new zip at `path`: `written` parts first, then `copied` parts from this package
    // (raw-copied without recompression when they come from the source zip).
    bool write(const QString &path, const QList<std::pair<QString, QByteArray>> &written,
               const QStringList &copied, QString *error = nullptr) const;

    quint64 uid() const { return id; } // unique per package instance (cache key)

    static QString relsName(const QString &part);
    static QString resolve(const QString &fromPart, const QString &target);
    static QString relative(const QString &fromPart, const QString &toPart);
    static QByteArray relsXml(const QList<Rel> &rels, const QString &fromPart);

private:
    Package();
    const quint64 id;
    mutable QMutex mutex;
    zip *archive = nullptr;
    QHash<QString, qint64> index;
    QHash<QString, QByteArray> memory;
    QHash<QString, QString> defaults, overrides;
    void readContentTypes();
};
