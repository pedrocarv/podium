#include "richtext.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>

namespace richtext {
namespace {

QString roman(int n)
{
    static const std::pair<int, const char *> table[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"},
                                                         {90, "XC"},  {50, "L"},   {40, "XL"}, {10, "X"},   {9, "IX"},
                                                         {5, "V"},    {4, "IV"},   {1, "I"}};
    QString s;
    for (auto [value, digits] : table)
        for (; n >= value; n -= value) s += digits;
    return s;
}

QString letters(int n)
{
    QString s;
    for (; n > 0; n = (n - 1) / 26) s.prepend(QChar('A' + (n - 1) % 26));
    return s;
}

// Symbol fonts aren't available on Linux; map their common bullet glyphs to Unicode.
QString unicodeBullet(const Bullet &b)
{
    static const QHash<QString, QString> wingdings{{"§", "▪"}, {"Ø", "➢"}, {"ü", "✓"}, {"q", "❑"}, {"v", "❖"},
                                                   {"n", "■"}, {"l", "●"}, {"Ÿ", "•"}, {"o", "□"}, {"w", "⬥"},
                                                   {"è", "➔"}, {"à", "➔"}, {"ð", "⇨"}, {"Ø", "➢"}};
    if (b.font.startsWith("Wingdings")) return wingdings.value(b.chr, "•");
    if (b.font == "Symbol" && b.chr == "·") return "•";
    return b.chr;
}

QFont bulletFont(const Bullet &b, const QFont &text)
{
    QFont f = text;
    if (!b.font.isEmpty() && !b.font.startsWith("Wingdings") && b.font != "Symbol") f.setFamilies({b.font});
    f.setPointSizeF(std::max(0.5, text.pointSizeF() * b.scale));
    f.setBold(false);
    f.setItalic(false);
    f.setUnderline(false);
    return f;
}

double spacing(const Spacing &s, double size) { return s.percent ? s.value * size * 1.2 : s.value; }

} // namespace

QPaintDevice *referenceDevice()
{
    static QImage device = [] {
        QImage img(1, 1, QImage::Format_ARGB32_Premultiplied);
        img.setDotsPerMeterX(2835); // 72 dpi
        img.setDotsPerMeterY(2835);
        return img;
    }();
    return &device;
}

QString bulletText(const Bullet &b, int n)
{
    if (b.kind == Bullet::Char) return unicodeBullet(b);
    if (b.kind != Bullet::Number) return {};
    QString core = b.scheme.startsWith("alphaLc")   ? letters(n).toLower()
                   : b.scheme.startsWith("alphaUc") ? letters(n)
                   : b.scheme.startsWith("romanLc") ? roman(n).toLower()
                   : b.scheme.startsWith("romanUc") ? roman(n)
                                                    : QString::number(n);
    if (b.scheme.endsWith("ParenBoth")) return "(" + core + ")";
    if (b.scheme.endsWith("ParenR")) return core + ")";
    if (b.scheme.endsWith("Period")) return core + ".";
    return core;
}

QFont font(const CharStyle &s, double scale)
{
    QFont f;
    f.setFamilies({s.font});
    f.setPointSizeF(std::max(0.5, s.size * scale));
    f.setBold(s.bold);
    f.setItalic(s.italic);
    f.setUnderline(s.underline);
    f.setStrikeOut(s.strike);
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

QTextCharFormat charFormat(const CharStyle &s, double scale, qreal alpha)
{
    QTextCharFormat f;
    f.setFont(font(s, scale), QTextCharFormat::FontPropertiesAll);
    QColor c = s.color;
    c.setAlphaF(float(c.alphaF() * alpha));
    f.setForeground(c);
    if (s.baseline > 0) f.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (s.baseline < 0) f.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    if (!s.link.isEmpty()) {
        f.setAnchor(true);
        f.setAnchorHref(s.link);
    }
    f.setProperty(CharStyleProperty, QVariant::fromValue(s));
    return f;
}

CharStyle charStyle(const QTextCharFormat &f, double scale)
{
    CharStyle s = f.property(CharStyleProperty).value<CharStyle>();
    QFont font = f.font();
    if (!font.families().isEmpty()) s.font = font.families().first();
    s.size = std::round(font.pointSizeF() / scale * 100) / 100;
    s.bold = font.bold();
    s.italic = font.italic();
    s.underline = font.underline();
    s.strike = font.strikeOut();
    if (f.foreground().style() != Qt::NoBrush) s.color = f.foreground().color();
    auto va = f.verticalAlignment();
    s.baseline = va == QTextCharFormat::AlignSuperScript ? (s.baseline > 0 ? s.baseline : 30)
                 : va == QTextCharFormat::AlignSubScript ? (s.baseline < 0 ? s.baseline : -25)
                                                         : 0;
    s.link = f.isAnchor() ? f.anchorHref() : QString();
    return s;
}

QTextBlockFormat blockFormat(const ParaStyle &s, const CharStyle &first, bool firstBlock, double scale,
                             double lineReduction)
{
    QTextBlockFormat f;
    f.setAlignment(s.align);
    f.setLeftMargin(s.marL);
    double indent = s.indent;
    if (s.bullet.kind != Bullet::None) {
        QFont bf = bulletFont(s.bullet, font(first, scale));
        double width = QFontMetricsF(bf, referenceDevice()).horizontalAdvance(bulletText(s.bullet, 8) + " ");
        indent = std::max(0.0, s.indent + width);
    }
    f.setTextIndent(indent);
    if (s.lineSpacing.percent) {
        double pct = s.lineSpacing.value * (1 - lineReduction);
        if (std::abs(pct - 1) > 0.001) f.setLineHeight(pct * 100, QTextBlockFormat::ProportionalHeight);
    } else {
        f.setLineHeight(s.lineSpacing.value * scale, QTextBlockFormat::FixedHeight);
    }
    double size = first.size * scale;
    if (!firstBlock) f.setTopMargin(spacing(s.spaceBefore, size));
    f.setBottomMargin(spacing(s.spaceAfter, size));
    f.setProperty(ParaStyleProperty, QVariant::fromValue(s));
    return f;
}

std::unique_ptr<QTextDocument> document(const TextBody &body, const Options &o)
{
    auto doc = std::make_unique<QTextDocument>();
    doc->setUndoRedoEnabled(false);
    doc->setUseDesignMetrics(true);
    doc->setDocumentMargin(0);
    doc->documentLayout()->setPaintDevice(referenceDevice());
    double scale = o.scale > 0 ? o.scale : body.fontScale;
    QTextCursor cur(doc.get());
    for (int i = 0; i < body.paragraphs.size(); ++i) {
        const Paragraph &p = body.paragraphs[i];
        qreal alpha = o.opacity ? o.opacity(i) : 1;
        const CharStyle &first = p.runs.isEmpty() ? p.endStyle : p.runs.first().style;
        QTextBlockFormat bf = blockFormat(p.style, first, i == 0, scale, body.lineReduction);
        QTextCharFormat ef = charFormat(p.endStyle, scale, alpha);
        if (i == 0) {
            cur.setBlockFormat(bf);
            cur.setBlockCharFormat(ef);
        } else {
            cur.insertBlock(bf, ef);
        }
        for (const Run &r : p.runs) {
            QTextCharFormat f = charFormat(r.style, scale, alpha);
            if (!r.field.isEmpty()) f.setProperty(FieldProperty, r.field);
            bool number = r.field == "slidenum" && o.slideNumber > 0;
            cur.insertText(number ? QString::number(o.slideNumber) : r.text, f);
        }
    }
    return doc;
}

QList<Paragraph> paragraphs(const QTextDocument &doc, double scale)
{
    QList<Paragraph> out;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        Paragraph p;
        QTextBlockFormat bf = b.blockFormat();
        p.style = bf.property(ParaStyleProperty).value<ParaStyle>();
        p.style.align = bf.alignment() & Qt::AlignHorizontal_Mask;
        p.endStyle = charStyle(b.charFormat(), scale);
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.text().isEmpty()) continue;
            p.runs << Run{frag.text(), charStyle(frag.charFormat(), scale),
                          frag.charFormat().property(FieldProperty).toString()};
        }
        if (!p.runs.isEmpty() && p.endStyle.size <= 0) p.endStyle = p.runs.last().style;
        out << p;
    }
    return out;
}

QTransform place(QTextDocument &doc, const TextBody &body, QSizeF box)
{
    bool vertical = body.vert != "horz" && !body.vert.isEmpty();
    QRectF area = QRectF(QPointF(), vertical ? box.transposed() : box).marginsRemoved(body.insets);
    double width = std::max(area.width(), 1.0), dx = 0;
    if (body.wrap) {
        doc.setTextWidth(width);
    } else {
        doc.setTextWidth(-1);
        double ideal = doc.idealWidth();
        doc.setTextWidth(std::max(width, ideal));
        Qt::Alignment align = body.paragraphs.isEmpty() ? Qt::AlignLeft : body.paragraphs.first().style.align;
        if (ideal > width) dx = align & Qt::AlignHCenter ? (width - ideal) / 2 : align & Qt::AlignRight ? width - ideal : 0;
    }
    double h = doc.size().height();
    double dy = body.anchor == TextBody::Middle ? (area.height() - h) / 2
                : body.anchor == TextBody::Bottom ? area.height() - h
                                                  : 0;
    QTransform t = QTransform::fromTranslate(area.left() + dx, area.top() + dy);
    if (body.vert == "vert270") t *= QTransform(0, -1, 1, 0, 0, box.height());
    else if (vertical) t *= QTransform(0, 1, -1, 0, box.width(), 0);
    return t;
}

void drawDocument(QPainter &p, const QTextDocument &doc)
{
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, Qt::black);
    doc.documentLayout()->draw(&p, ctx);
    drawBullets(p, doc);
}

void drawBullets(QPainter &p, const QTextDocument &doc)
{
    std::array<int, 9> counters{};
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        ParaStyle s = b.blockFormat().property(ParaStyleProperty).value<ParaStyle>();
        int level = std::clamp(s.level, 0, 8);
        std::fill(counters.begin() + level + 1, counters.end(), 0);
        if (s.bullet.kind != Bullet::Number) counters[level] = 0;
        QTextLayout *layout = b.layout();
        if (s.bullet.kind == Bullet::None || b.text().isEmpty() || !layout || layout->lineCount() == 0) continue;

        int n = s.bullet.kind == Bullet::Number ? s.bullet.startAt + counters[level]++ : 0;
        QTextCharFormat cf = b.begin().fragment().charFormat();
        QColor color = s.bullet.color.isValid() ? s.bullet.color : cf.foreground().color();
        color.setAlphaF(std::min(color.alphaF(), cf.foreground().color().alphaF()));
        QTextLayout bullet(bulletText(s.bullet, n), bulletFont(s.bullet, cf.font()), referenceDevice());
        bullet.beginLayout();
        QTextLine bl = bullet.createLine();
        bullet.endLayout();
        QTextLine first = layout->lineAt(0);
        QPointF origin = layout->position();
        p.save();
        p.setPen(color);
        bullet.draw(&p, QPointF(origin.x() + s.marL + s.indent, origin.y() + first.y() + first.ascent() - bl.ascent()));
        p.restore();
    }
}

void draw(QPainter &p, const TextBody &body, QRectF box, const Options &o)
{
    if (body.paragraphs.isEmpty()) return;
    auto doc = document(body, o);
    QTransform t = place(*doc, body, box.size());
    p.save();
    p.translate(box.topLeft());
    p.setTransform(t, true);
    drawDocument(p, *doc);
    p.restore();
}

double height(const TextBody &body, double width)
{
    auto doc = document(body);
    doc->setTextWidth(std::max(1.0, width - body.insets.left() - body.insets.right()));
    return doc->size().height() + body.insets.top() + body.insets.bottom();
}

double fitScale(const TextBody &body, QSizeF box)
{
    double available = box.height() - body.insets.top() - body.insets.bottom();
    auto fits = [&](double scale) {
        Options o;
        o.scale = scale;
        auto doc = document(body, o);
        place(*doc, body, box);
        return doc->size().height() <= available;
    };
    if (fits(1)) return 1;
    double lo = 0.25, hi = 1;
    for (int i = 0; i < 8; ++i) (fits((lo + hi) / 2) ? lo : hi) = (lo + hi) / 2;
    return std::floor(lo * 40) / 40;
}

} // namespace richtext
