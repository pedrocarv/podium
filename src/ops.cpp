#include "ops.h"
#include "richtext.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>

namespace ops {
namespace {

// Model colors are 8 bits per channel, as in the file.
QColor rgb8(QColor c) { return QColor::fromRgba(c.rgba()); }

QColor mix(QColor a, QColor b, double t)
{
    return rgb8(QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                                 float(a.blueF() * (1 - t) + b.blueF() * t)));
}

Line solidLine(QColor color, double width)
{
    Line l;
    l.fill = Fill::solid(color);
    l.width = width;
    return l;
}

TextBody emptyBody(const std::array<LevelStyle, 9> &levels)
{
    TextBody b;
    b.levels = levels;
    b.paragraphs = {Paragraph{levels[0].para, {}, levels[0].chr}};
    return b;
}

} // namespace

void CharEdit::apply(CharStyle &s) const
{
    if (font) s.font = *font;
    if (size) s.size = *size;
    if (grow) s.size = std::clamp(std::round(s.size * *grow * 2) / 2, 1.0, 400.0);
    if (bold) s.bold = *bold;
    if (italic) s.italic = *italic;
    if (underline) s.underline = *underline;
    if (strike) s.strike = *strike;
    if (color) s.color = *color;
}

QTextCharFormat CharEdit::format(double scale) const
{
    QTextCharFormat f;
    if (font) f.setFontFamilies(QStringList{*font});
    if (size) f.setFontPointSize(*size * scale);
    if (bold) f.setFontWeight(*bold ? QFont::Bold : QFont::Normal);
    if (italic) f.setFontItalic(*italic);
    if (underline) f.setFontUnderline(*underline);
    if (strike) f.setFontStrikeOut(*strike);
    if (color) f.setForeground(*color);
    return f;
}

void ParaEdit::apply(Paragraph &p, const TextBody &body) const
{
    if (align) p.style.align = *align;
    if (level) {
        int from = p.style.level, to = std::clamp(from + level, 0, 8);
        const LevelStyle &a = body.levels[from], &b = body.levels[to];
        p.style.level = to;
        p.style.marL = b.para.marL;
        p.style.indent = b.para.indent;
        if (p.style.bullet == a.para.bullet) p.style.bullet = b.para.bullet;
        auto follow = [&](CharStyle &c) {
            if (c.size == a.chr.size) c.size = b.chr.size;
        };
        for (Run &r : p.runs) follow(r.style);
        follow(p.endStyle);
    }
    if (bullet) {
        Bullet &b = p.style.bullet;
        if (*bullet == Bullet::None) {
            b.kind = Bullet::None;
            if (p.style.indent < 0) p.style.marL += p.style.indent, p.style.indent = 0;
        } else {
            if (b.kind == Bullet::None && p.style.indent >= 0) p.style.marL += 18, p.style.indent = -18;
            b.kind = *bullet;
            if (*bullet == Bullet::Char && b.chr.isEmpty()) b.chr = QStringLiteral("•");
        }
    }
}

void forEachText(Element &e, const std::function<void(TextBody &)> &fn)
{
    if (auto s = e.as<ShapeEl>()) fn(s->text);
    if (auto t = e.as<TableEl>())
        for (auto &row : t->cells)
            for (Cell &c : row) fn(c.text);
}

void apply(TextBody &body, const CharEdit &edit)
{
    for (Paragraph &p : body.paragraphs) {
        for (Run &r : p.runs) edit.apply(r.style);
        edit.apply(p.endStyle);
    }
}

void apply(TextBody &body, const ParaEdit &edit)
{
    for (Paragraph &p : body.paragraphs) edit.apply(p, body);
}

void fitText(Element &e)
{
    auto s = e.as<ShapeEl>();
    if (!s) return;
    TextBody &t = s->text;
    if (t.autofit == TextBody::Shrink) {
        t.fontScale = 1;
        t.lineReduction = 0;
        t.fontScale = richtext::fitScale(t, e.rect.size());
        if (t.fontScale < 1) t.lineReduction = std::min(0.2, (1 - t.fontScale) / 2);
    } else if (t.autofit == TextBody::Resize && t.wrap) {
        e.rect.setHeight(richtext::height(t, e.rect.width()));
    }
}

std::array<LevelStyle, 9> plainLevels(const Deck &deck, double size, QColor color)
{
    std::array<LevelStyle, 9> levels;
    for (int i = 0; i < 9; ++i) {
        levels[i].para.level = i;
        levels[i].para.marL = i * 36;
        levels[i].chr.font = deck.theme.minor;
        levels[i].chr.size = size;
        levels[i].chr.color = color.isValid() ? color : deck.theme.colors.value("tx1", Qt::black);
    }
    return levels;
}

Element textBox(const Deck &deck, int id, QRectF rect)
{
    Element e;
    e.id = id;
    e.name = QString("TextBox %1").arg(id);
    e.rect = rect;
    ShapeEl s;
    s.textBox = true;
    s.text = emptyBody(plainLevels(deck));
    s.text.autofit = TextBody::Resize;
    e.body = s;
    fitText(e);
    return e;
}

Element shape(const Deck &deck, int id, const QString &preset, QRectF rect)
{
    QColor accent = deck.theme.colors.value("accent1", QColor(0x2563EB));
    Element e;
    e.id = id;
    e.name = QString("%1 %2").arg(presetName(preset)).arg(id);
    e.rect = rect;
    ShapeEl s;
    s.preset = preset;
    if (preset == "line" || preset.contains("Connector")) {
        s.connector = true;
        s.text.levels = plainLevels(deck);
        s.line = solidLine(accent, 1.5);
        if (preset == "straightConnector1") s.line.tail.type = "triangle";
    } else {
        s.fill = Fill::solid(accent);
        s.line = solidLine(rgb8(accent.darker(150)), 1);
        s.text = emptyBody(plainLevels(deck, 18, Qt::white));
        s.text.anchor = TextBody::Middle;
        for (LevelStyle &l : s.text.levels) l.para.align = Qt::AlignHCenter;
        s.text.paragraphs.first().style.align = Qt::AlignHCenter;
    }
    e.body = s;
    return e;
}

Element table(const Deck &deck, int id, int rows, int cols, QRectF rect)
{
    QColor accent = deck.theme.colors.value("accent1", QColor(0x2563EB));
    Element e;
    e.id = id;
    e.name = QString("Table %1").arg(id);
    e.rect = rect;
    TableEl t;
    for (int c = 0; c < cols; ++c) t.cols << rect.width() / cols;
    for (int r = 0; r < rows; ++r) {
        t.rows << std::max(rect.height() / rows, 26.0);
        QList<Cell> row;
        for (int c = 0; c < cols; ++c) {
            Cell cell;
            bool header = r == 0;
            cell.text = emptyBody(plainLevels(deck));
            if (header) {
                cell.text.paragraphs.first().endStyle.bold = true;
                cell.text.paragraphs.first().endStyle.color = Qt::white;
            }
            cell.text.insets = QMarginsF(7.2, 3.6, 7.2, 3.6);
            cell.fill = Fill::solid(header ? accent : mix(accent, Qt::white, r % 2 ? 0.8 : 0.9));
            cell.left = cell.right = cell.top = cell.bottom = solidLine(Qt::white, 1);
            row << cell;
        }
        t.cells << row;
    }
    e.body = t;
    return e;
}

std::optional<Element> picture(Deck &deck, int id, const QByteArray &data, const QString &suffix, QPointF center)
{
    QBuffer buffer(const_cast<QByteArray *>(&data));
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    QSize px = reader.size();
    if (!px.isValid() || !deck.package) return std::nullopt;
    QString ext = suffix.toLower() == "jpg" ? "jpeg" : suffix.toLower();
    QString hash = QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(12);
    QString part = deck.package->add(QString("ppt/media/image-%1.%2").arg(hash, ext), data);

    QSizeF size = QSizeF(px) * 0.75; // 96 dpi pixels → points
    QSizeF max = deck.size * 0.8;
    if (size.width() > max.width() || size.height() > max.height()) size.scale(max, Qt::KeepAspectRatio);
    Element e;
    e.id = id;
    e.name = QString("Picture %1").arg(id);
    e.rect = QRectF(center - QPointF(size.width() / 2, size.height() / 2), size);
    PictureEl p;
    p.image = part;
    e.body = p;
    return e;
}

Slide slideFromLayout(const Deck &deck, int layout)
{
    Slide s;
    s.layout = layout;
    s.background = deck.layouts[layout].background;
    int id = 2;
    for (Element e : deck.layouts[layout].placeholders) {
        if (e.ph.type == "dt" || e.ph.type == "ftr" || e.ph.type == "sldNum") continue;
        e.id = id++;
        s.elements << e;
    }
    return s;
}

void scaleTable(TableEl &t, QSizeF size)
{
    double w = 0, h = 0;
    for (double c : t.cols) w += c;
    for (double r : t.rows) h += r;
    for (double &c : t.cols) c *= w > 0 ? size.width() / w : 1;
    for (double &r : t.rows) r *= h > 0 ? size.height() / h : 1;
}

QStringList referencedParts(const Element &e)
{
    QStringList parts;
    if (auto p = e.as<PictureEl>()) parts << p->image << p->video;
    if (auto s = e.as<ShapeEl>(); s && s->fill.kind == Fill::Image) parts << s->fill.image;
    if (auto r = e.as<RawEl>()) parts << r->fallback;
    if (auto g = e.as<GroupEl>())
        for (const Element &c : g->children) parts << referencedParts(c);
    for (const Rel &r : e.rels)
        if (!r.external) parts << r.target;
    parts.removeAll(QString());
    return parts;
}

void remapParts(Element &e, const QHash<QString, QString> &renamed)
{
    if (renamed.isEmpty()) return;
    auto map = [&](QString &part) { part = renamed.value(part, part); };
    if (auto p = e.as<PictureEl>()) map(p->image), map(p->video);
    if (auto s = e.as<ShapeEl>()) map(s->fill.image);
    if (auto r = e.as<RawEl>()) {
        map(r->fallback);
        for (Element &c : r->preview) remapParts(c, renamed);
    }
    if (auto g = e.as<GroupEl>())
        for (Element &c : g->children) remapParts(c, renamed);
    for (Rel &r : e.rels)
        if (!r.external) map(r.target);
}

void renumber(Element &e, int &nextId)
{
    e.id = nextId++;
    if (auto g = e.as<GroupEl>())
        for (Element &c : g->children) renumber(c, nextId);
}

QString presetName(const QString &preset)
{
    static const QHash<QString, QString> names{
        {"rect", "Rectangle"},       {"roundRect", "Rounded Rectangle"}, {"ellipse", "Oval"},
        {"triangle", "Triangle"},    {"rtTriangle", "Right Triangle"},   {"diamond", "Diamond"},
        {"parallelogram", "Parallelogram"}, {"trapezoid", "Trapezoid"},  {"pentagon", "Pentagon"},
        {"hexagon", "Hexagon"},      {"octagon", "Octagon"},             {"plus", "Cross"},
        {"star5", "Star"},           {"star8", "8-Point Star"},          {"rightArrow", "Right Arrow"},
        {"leftArrow", "Left Arrow"}, {"upArrow", "Up Arrow"},            {"downArrow", "Down Arrow"},
        {"leftRightArrow", "Left-Right Arrow"}, {"chevron", "Chevron"},  {"homePlate", "Pentagon Arrow"},
        {"heart", "Heart"},          {"lightningBolt", "Lightning Bolt"}, {"donut", "Donut"},
        {"can", "Cylinder"},         {"frame", "Frame"},                 {"wedgeRectCallout", "Callout"},
        {"wedgeEllipseCallout", "Oval Callout"}, {"leftBrace", "Left Brace"}, {"rightBrace", "Right Brace"},
        {"line", "Line"},            {"straightConnector1", "Arrow"}};
    return names.value(preset, preset);
}

} // namespace ops
