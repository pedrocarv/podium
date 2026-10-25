#pragma once

// Small helpers shared by the OPC/pptx code on top of pugixml.

#include <QByteArray>
#include <QString>
#include <pugixml.hpp>

inline QString str(const char *s) { return QString::fromUtf8(s); }
inline QString str(pugi::xml_attribute a) { return QString::fromUtf8(a.value()); }
inline std::string utf8(const QString &s) { return s.toStdString(); }

inline bool loadXml(pugi::xml_document &doc, const QByteArray &data)
{
    return doc.load_buffer(data.constData(), size_t(data.size()),
                           pugi::parse_default | pugi::parse_ws_pcdata_single);
}

struct ByteWriter : pugi::xml_writer {
    QByteArray out;
    void write(const void *data, size_t size) override { out.append(static_cast<const char *>(data), qsizetype(size)); }
};

inline QByteArray serialize(const pugi::xml_document &doc)
{
    ByteWriter w;
    w.out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
    doc.save(w, "", pugi::format_raw | pugi::format_no_declaration);
    return w.out;
}

inline QByteArray serialize(pugi::xml_node node)
{
    ByteWriter w;
    node.print(w, "", pugi::format_raw);
    return w.out;
}
