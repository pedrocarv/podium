#include "render.h"
#include "geometry.h"
#include "richtext.h"

#include <QBuffer>
#include <QCache>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QPainter>
#include <QPdfWriter>
#include <QtMath>

namespace {

QSizeF deviceSize(const QPainter &p, QRectF r)
{
    return p.combinedTransform().mapRect(r).size() * p.device()->devicePixelRatioF();
}

QTransform flipTransform(const Element &e)
{
    QSizeF s = e.rect.size();
    return QTransform(e.flipH ? -1 : 1, 0, 0, e.flipV ? -1 : 1, e.flipH ? s.width() : 0, e.flipV ? s.height() : 0);
}

void drawLabel(QPainter &p, QRectF r, const QString &label)
{
    p.save();
    p.setPen(QPen(QColor(128, 128, 128), 0.75, Qt::DashLine));
    p.setBrush(QColor(128, 128, 128, 24));
    p.drawRect(r);
    QFont f("Sans");
    f.setPointSizeF(12);
    p.setFont(QFont(f, p.device()));
    p.setPen(QColor(110, 110, 110));
    p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, label);
    p.restore();
}

void drawArrowhead(QPainter &p, const LineEnd &end, QPointF tip, QPointF from, const QPen &pen)
{
    if (end.type == "none" || end.type.isEmpty()) return;
    auto scale = [](const QString &s) { return s == "sm" ? 2.0 : s == "lg" ? 5.0 : 3.0; };
    double lw = std::max(pen.widthF(), 1.0), len = scale(end.len) * lw, half = scale(end.w) * lw / 2;
    QPointF d = tip - from;
    double n = std::hypot(d.x(), d.y());
    if (n < 1e-6) return;
    d /= n;
    QPointF side(-d.y(), d.x()), base = tip - d * len;
    p.save();
    p.setPen(QPen(pen.color(), pen.widthF(), Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    p.setBrush(pen.color());
    QPolygonF tri{tip, base + side * half, base - side * half};
    if (end.type == "arrow") {
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF{base + side * half, tip, base - side * half});
    } else if (end.type == "stealth") {
        p.drawPolygon(QPolygonF{tip, base + side * half, tip - d * len * 0.6, base - side * half});
    } else if (end.type == "oval") {
        p.drawEllipse(tip - d * len / 2, len / 2, half);
    } else if (end.type == "diamond") {
        QPointF m = tip - d * len / 2;
        p.drawPolygon(QPolygonF{tip, m + side * half, tip - d * len, m - side * half});
    } else {
        p.drawPolygon(tri);
    }
    p.restore();
}

richtext::Options textOptions(const Element &e, const RenderOptions &o)
{
    richtext::Options t;
    t.slideNumber = o.slideNumber;
    if (o.opacity) t.opacity = [&e, &o](int paragraph) { return o.opacity(e, paragraph); };
    return t;
}

void paintText(QPainter &p, const Element &e, const TextBody &text, QSizeF size, const RenderOptions &o)
{
    if (e.id == o.skipText && o.skipText >= 0) return;
    p.save();
    if (e.flipV) {
        p.translate(size.width(), size.height());
        p.rotate(180);
    }
    if (o.editing && e.ph && text.isEmpty() && !e.ph.prompt.isEmpty()) {
        TextBody prompt = text;
        CharStyle style = text.paragraphs.isEmpty() ? text.levels[0].chr : text.paragraphs.first().endStyle;
        style.color = QColor(120, 120, 120);
        prompt.paragraphs = {Paragraph{text.paragraphs.isEmpty() ? text.levels[0].para : text.paragraphs.first().style,
                                       {Run{e.ph.prompt, style, {}}}, style}};
        prompt.paragraphs.first().style.bullet.kind = Bullet::None;
        richtext::draw(p, prompt, QRectF(QPointF(), size));
    } else {
        richtext::draw(p, text, QRectF(QPointF(), size), textOptions(e, o));
    }
    p.restore();
}

void paintShape(QPainter &p, const Deck &deck, const Element &e, const ShapeEl &s, const RenderOptions &o)
{
    QSizeF size = e.rect.size();
    geometry::Outline g = geometry::of(s, size);
    p.save();
    p.setTransform(flipTransform(e), true);
    if (!g.fill.isEmpty() && s.fill.kind != Fill::None) p.fillPath(g.fill, brush(s.fill, QRectF(QPointF(), size), deck, p));
    QPen linePen = pen(s.line);
    if (linePen.style() != Qt::NoPen) {
        p.strokePath(g.stroke, linePen);
        if (g.fill.isEmpty() && !g.stroke.isEmpty()) {
            drawArrowhead(p, s.line.head, g.stroke.pointAtPercent(0), g.stroke.pointAtPercent(0.01), linePen);
            drawArrowhead(p, s.line.tail, g.stroke.pointAtPercent(1), g.stroke.pointAtPercent(0.99), linePen);
        }
    }
    if (o.editing && e.ph && s.text.isEmpty() && s.fill.kind == Fill::None && linePen.style() == Qt::NoPen) {
        QPen hint(QColor(150, 150, 150), 0, Qt::DashLine);
        hint.setCosmetic(true);
        p.setPen(hint);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(QPointF(), size));
    }
    p.restore();
    paintText(p, e, s.text, size, o);
}

void paintPicture(QPainter &p, const Deck &deck, const Element &e, const PictureEl &pic, const RenderOptions &o)
{
    QRectF r(QPointF(), e.rect.size());
    QPainterPath clip = geometry::preset(pic.preset, r.size()).fill;
    p.save();
    p.setTransform(flipTransform(e), true);
    if (pic.preset != "rect") p.setClipPath(clip, Qt::IntersectClip);
    QImage frame = o.videoFrame ? o.videoFrame(e) : QImage();
    if (!frame.isNull()) {
        p.drawImage(r, frame);
    } else if (deck.package) {
        double cw = 1 - pic.crop.left() - pic.crop.right(), ch = 1 - pic.crop.top() - pic.crop.bottom();
        QSizeF want = deviceSize(p, r);
        QImage img = loadImage(*deck.package, pic.image, QSizeF(want.width() / std::max(cw, 0.01), want.height() / std::max(ch, 0.01)));
        if (img.isNull()) {
            if (o.editing) drawLabel(p, r, QFileInfo(pic.image).suffix().toUpper() + " " + QObject::tr("image"));
        } else {
            QRectF src(img.width() * pic.crop.left(), img.height() * pic.crop.top(), img.width() * cw, img.height() * ch);
            p.drawImage(r, img, src);
        }
    }
    if (o.editing && !pic.video.isEmpty()) {
        double d = std::min({r.width(), r.height(), 48.0});
        QRectF badge(r.center() - QPointF(d / 2, d / 2), QSizeF(d, d));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawEllipse(badge);
        p.setBrush(Qt::white);
        QPointF c = badge.center();
        p.drawPolygon(QPolygonF{c + QPointF(-d * 0.15, -d * 0.22), c + QPointF(d * 0.25, 0), c + QPointF(-d * 0.15, d * 0.22)});
    }
    if (QPen lp = pen(pic.line); lp.style() != Qt::NoPen) p.strokePath(clip, lp);
    p.restore();
}

void paintTable(QPainter &p, const Deck &deck, const Element &e, const TableEl &t, const RenderOptions &o)
{
    TableGeometry g = tableGeometry(t);
    auto each = [&](auto &&fn) {
        for (int r = 0; r < g.rows() && r < t.cells.size(); ++r)
            for (int c = 0; c < g.cols() && c < t.cells[r].size(); ++c)
                if (!t.cells[r][c].merged) fn(t.cells[r][c], g.cell(t, r, c));
    };
    each([&](const Cell &cell, QRectF rect) {
        if (cell.fill.kind != Fill::None) p.fillRect(rect, brush(cell.fill, rect, deck, p));
        p.save();
        p.translate(rect.topLeft());
        richtext::draw(p, cell.text, QRectF(QPointF(), rect.size()), textOptions(e, o));
        p.restore();
    });
    each([&](const Cell &cell, QRectF rect) {
        auto line = [&](const Line &l, QPointF a, QPointF b) {
            if (QPen lp = pen(l); lp.style() != Qt::NoPen) {
                p.setPen(lp);
                p.drawLine(a, b);
            }
        };
        line(cell.top, rect.topLeft(), rect.topRight());
        line(cell.bottom, rect.bottomLeft(), rect.bottomRight());
        line(cell.left, rect.topLeft(), rect.bottomLeft());
        line(cell.right, rect.topRight(), rect.bottomRight());
    });
}

} // namespace

QTransform elementTransform(const Element &e)
{
    QSizeF s = e.rect.size();
    QTransform t;
    t.translate(e.rect.center().x(), e.rect.center().y());
    t.rotate(e.rotation);
    t.translate(-s.width() / 2, -s.height() / 2);
    return t;
}

QRectF TableGeometry::cell(const TableEl &t, int r, int c) const
{
    const Cell &cell = t.cells[r][c];
    int c2 = std::min(c + cell.colSpan, cols()), r2 = std::min(r + cell.rowSpan, rows());
    return QRectF(QPointF(xs[c], ys[r]), QPointF(xs[c2], ys[r2]));
}

TableGeometry tableGeometry(const TableEl &t)
{
    TableGeometry g{{0}, {0}};
    for (double w : t.cols) g.xs << g.xs.last() + w;
    QList<double> heights = t.rows;
    for (int r = 0; r < t.cells.size() && r < heights.size(); ++r)
        for (int c = 0; c < t.cells[r].size() && c < t.cols.size(); ++c) {
            const Cell &cell = t.cells[r][c];
            if (cell.merged || cell.rowSpan > 1) continue;
            double width = g.xs[std::min<qsizetype>(c + cell.colSpan, t.cols.size())] - g.xs[c];
            heights[r] = std::max(heights[r], richtext::height(cell.text, width));
        }
    for (double h : heights) g.ys << g.ys.last() + h;
    return g;
}

QImage loadImage(const Package &pkg, const QString &part, QSizeF want)
{
    if (part.isEmpty()) return {};
    static QMutex mutex;
    static QCache<QString, QImage> cache(512 * 1024); // KiB
    int edge = int(std::max(want.width(), want.height()));
    int bucket = 64;
    while (bucket < edge && bucket < 8192) bucket *= 2;
    QString key = QString("%1|%2|%3").arg(pkg.uid()).arg(part).arg(bucket);
    {
        QMutexLocker lock(&mutex);
        if (QImage *img = cache.object(key)) return *img;
    }
    QByteArray data = pkg.read(part);
    QBuffer buffer(&data);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    QSize original = reader.size();
    if (original.isValid() && std::max(original.width(), original.height()) > bucket)
        reader.setScaledSize(original.scaled(bucket, bucket, Qt::KeepAspectRatio));
    QImage img = reader.read();
    if (!img.isNull()) img.convertTo(QImage::Format_ARGB32_Premultiplied);
    QMutexLocker lock(&mutex);
    cache.insert(key, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
    return img;
}

QBrush brush(const Fill &f, QRectF bounds, const Deck &deck, const QPainter &p)
{
    switch (f.kind) {
    case Fill::Solid:
        return f.color;
    case Fill::Gradient: {
        double a = qDegreesToRadians(f.angle);
        QPointF d(std::cos(a), std::sin(a)), c = bounds.center();
        double half = (std::abs(bounds.width() * d.x()) + std::abs(bounds.height() * d.y())) / 2;
        QLinearGradient g(c - d * half, c + d * half);
        g.setStops(f.stops);
        return g;
    }
    case Fill::Image: {
        if (!deck.package) return Qt::NoBrush;
        QImage img = loadImage(*deck.package, f.image, deviceSize(p, bounds));
        if (img.isNull()) return Qt::NoBrush;
        QBrush b(img);
        b.setTransform(QTransform::fromTranslate(bounds.x(), bounds.y())
                           .scale(bounds.width() / img.width(), bounds.height() / img.height()));
        return b;
    }
    default:
        return Qt::NoBrush;
    }
}

QPen pen(const Line &l)
{
    if (l.fill.kind == Fill::None) return Qt::NoPen;
    QColor color = l.fill.kind == Fill::Gradient ? l.fill.stops.first().second : l.fill.color;
    QPen pen(color, l.width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    static const QHash<QString, QList<qreal>> dashes{
        {"dot", {1, 3}},           {"dash", {4, 3}},          {"lgDash", {8, 3}},
        {"dashDot", {4, 3, 1, 3}}, {"lgDashDot", {8, 3, 1, 3}}, {"lgDashDotDot", {8, 3, 1, 3, 1, 3}},
        {"sysDash", {3, 1}},       {"sysDot", {1, 1}},        {"sysDashDot", {3, 1, 1, 1}},
        {"sysDashDotDot", {3, 1, 1, 1, 1, 1}}};
    if (auto d = dashes.constFind(l.dash); d != dashes.cend()) pen.setDashPattern(*d);
    return pen;
}

void paintElement(QPainter &p, const Deck &deck, const Element &e, const RenderOptions &o)
{
    if (e.hidden) return;
    qreal alpha = o.opacity ? o.opacity(e, -1) : 1;
    if (alpha <= 0) return;
    p.save();
    p.setOpacity(p.opacity() * alpha);
    if (auto raw = e.as<RawEl>(); raw && !raw->preview.empty()) {
        for (const Element &c : raw->preview) paintElement(p, deck, c, o);
        p.restore();
        return;
    }
    p.setTransform(elementTransform(e), true);
    QSizeF size = e.rect.size();
    if (auto s = e.as<ShapeEl>()) {
        paintShape(p, deck, e, *s, o);
    } else if (auto pic = e.as<PictureEl>()) {
        paintPicture(p, deck, e, *pic, o);
    } else if (auto t = e.as<TableEl>()) {
        paintTable(p, deck, e, *t, o);
    } else if (auto g = e.as<GroupEl>()) {
        p.setTransform(flipTransform(e), true);
        if (!g->childRect.isEmpty()) {
            p.scale(size.width() / g->childRect.width(), size.height() / g->childRect.height());
            p.translate(-g->childRect.topLeft());
        }
        for (const Element &c : g->children) paintElement(p, deck, c, o);
    } else if (auto raw = e.as<RawEl>()) {
        QImage img = deck.package ? loadImage(*deck.package, raw->fallback, deviceSize(p, QRectF(QPointF(), size))) : QImage();
        if (img.isNull()) drawLabel(p, QRectF(QPointF(), size), raw->label);
        else p.drawImage(QRectF(QPointF(), size), img);
    }
    p.restore();
}

void paintSlide(QPainter &p, const Deck &deck, const Slide &slide, const RenderOptions &o)
{
    QRectF page(QPointF(), deck.size);
    p.save();
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    p.fillRect(page, Qt::white);
    if (slide.background.kind != Fill::None) p.fillRect(page, brush(slide.background, page, deck, p));
    if (slide.showMaster && slide.layout < deck.layouts.size()) {
        RenderOptions decor;
        decor.slideNumber = o.slideNumber;
        for (const Element &e : deck.layouts[slide.layout].decor) paintElement(p, deck, e, decor);
    }
    for (const Element &e : slide.elements) paintElement(p, deck, e, o);
    p.restore();
}

QImage renderSlide(const Deck &deck, int index, QSize size, RenderOptions o)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    p.scale(size.width() / deck.size.width(), size.height() / deck.size.height());
    if (o.slideNumber == 0) o.slideNumber = index + 1;
    paintSlide(p, deck, deck.slides[index], o);
    return img;
}

bool exportPdf(const Deck &deck, const QString &path, QString *error)
{
    constexpr int dpi = 300;
    QPdfWriter pdf(path);
    pdf.setResolution(dpi);
    pdf.setCreator("Podium");
    QSizeF portrait(std::min(deck.size.width(), deck.size.height()), std::max(deck.size.width(), deck.size.height()));
    pdf.setPageLayout(QPageLayout(QPageSize(portrait, QPageSize::Point, {}, QPageSize::ExactMatch),
                                  deck.size.width() > deck.size.height() ? QPageLayout::Landscape : QPageLayout::Portrait,
                                  QMarginsF()));
    QPainter p;
    if (!p.begin(&pdf)) {
        if (error) *error = QObject::tr("Cannot write %1").arg(path);
        return false;
    }
    bool first = true;
    for (int i = 0; i < deck.slides.size(); ++i) {
        if (deck.slides[i].hidden) continue;
        if (!first) pdf.newPage();
        first = false;
        p.save();
        p.scale(dpi / 72.0, dpi / 72.0);
        RenderOptions o;
        o.slideNumber = i + 1;
        paintSlide(p, deck, deck.slides[i], o);
        p.restore();
    }
    return p.end();
}
