#include "geometry.h"

#include <QHash>
#include <QPolygonF>
#include <QtMath>

#include <functional>

namespace geometry {
namespace {

struct Ctx {
    double w, h, ss;
    const QMap<QString, double> &adjust;
    // An adjust value as a fraction (DrawingML stores 1/100000ths).
    double a(const QString &name, double fallback) const { return adjust.value(name, fallback) / 100000.0; }
};

QPainterPath polygon(std::initializer_list<QPointF> pts)
{
    QPainterPath p;
    p.addPolygon(QPolygonF(pts));
    p.closeSubpath();
    return p;
}

QPainterPath star(double w, double h, int points, double inner)
{
    QPolygonF poly;
    for (int i = 0; i < points * 2; ++i) {
        double angle = qDegreesToRadians(-90.0 + i * 180.0 / points), r = i % 2 ? inner : 1.0;
        poly << QPointF(w / 2 + std::cos(angle) * w / 2 * r, h / 2 + std::sin(angle) * h / 2 * r);
    }
    QPainterPath p;
    p.addPolygon(poly);
    p.closeSubpath();
    return p;
}

QPainterPath regular(double w, double h, int sides)
{
    QPolygonF poly;
    for (int i = 0; i < sides; ++i) {
        double angle = qDegreesToRadians(-90.0 + i * 360.0 / sides);
        poly << QPointF(w / 2 + std::cos(angle) * w / 2, h / 2 + std::sin(angle) * h / 2);
    }
    QPainterPath p;
    p.addPolygon(poly);
    p.closeSubpath();
    return p;
}

QPainterPath rightArrow(double w, double h, double shaft, double head)
{
    double y1 = h / 2 * (1 - shaft), y2 = h - y1, x = w - head;
    return polygon({{0, y1}, {x, y1}, {x, 0}, {w, h / 2}, {x, h}, {x, y2}, {0, y2}});
}

QPainterPath callout(QPainterPath body, double w, double h, QPointF tip)
{
    // A wedge from the body's center towards the tip.
    QPointF c(w / 2, h / 2), d = tip - c;
    QPointF n = QPointF(-d.y(), d.x()) / std::max(1.0, std::hypot(d.x(), d.y())) * std::min(w, h) * 0.12;
    return body.united(polygon({c + n, tip, c - n}));
}

using Builder = std::function<Outline(const Ctx &)>;

Outline closed(QPainterPath p) { return {p, p}; }
Outline open(QPainterPath p) { return {{}, p}; }

const QHash<QString, Builder> &builders()
{
    static const QHash<QString, Builder> table = [] {
        QHash<QString, Builder> t;
        t["rect"] = [](const Ctx &c) {
            QPainterPath p;
            p.addRect(0, 0, c.w, c.h);
            return closed(p);
        };
        t["roundRect"] = [](const Ctx &c) {
            QPainterPath p;
            double r = c.ss * c.a("adj", 16667);
            p.addRoundedRect(0, 0, c.w, c.h, r, r);
            return closed(p);
        };
        t["ellipse"] = [](const Ctx &c) {
            QPainterPath p;
            p.addEllipse(0, 0, c.w, c.h);
            return closed(p);
        };
        t["triangle"] = [](const Ctx &c) { return closed(polygon({{c.w * c.a("adj", 50000), 0}, {c.w, c.h}, {0, c.h}})); };
        t["rtTriangle"] = [](const Ctx &c) { return closed(polygon({{0, 0}, {c.w, c.h}, {0, c.h}})); };
        t["diamond"] = [](const Ctx &c) {
            return closed(polygon({{c.w / 2, 0}, {c.w, c.h / 2}, {c.w / 2, c.h}, {0, c.h / 2}}));
        };
        t["parallelogram"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 25000);
            return closed(polygon({{x, 0}, {c.w, 0}, {c.w - x, c.h}, {0, c.h}}));
        };
        t["trapezoid"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 25000);
            return closed(polygon({{0, c.h}, {x, 0}, {c.w - x, 0}, {c.w, c.h}}));
        };
        t["pentagon"] = [](const Ctx &c) { return closed(regular(c.w, c.h, 5)); };
        t["heptagon"] = [](const Ctx &c) { return closed(regular(c.w, c.h, 7)); };
        t["decagon"] = [](const Ctx &c) { return closed(regular(c.w, c.h, 10)); };
        t["dodecagon"] = [](const Ctx &c) { return closed(regular(c.w, c.h, 12)); };
        t["hexagon"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 25000);
            return closed(polygon({{x, 0}, {c.w - x, 0}, {c.w, c.h / 2}, {c.w - x, c.h}, {x, c.h}, {0, c.h / 2}}));
        };
        t["octagon"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 29289);
            return closed(polygon({{x, 0}, {c.w - x, 0}, {c.w, x}, {c.w, c.h - x}, {c.w - x, c.h}, {x, c.h},
                                   {0, c.h - x}, {0, x}}));
        };
        t["plus"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 25000), w = c.w, h = c.h;
            return closed(polygon({{x, 0}, {w - x, 0}, {w - x, x}, {w, x}, {w, h - x}, {w - x, h - x}, {w - x, h},
                                   {x, h}, {x, h - x}, {0, h - x}, {0, x}, {x, x}}));
        };
        for (auto [name, points, inner] : {std::tuple{"star4", 4, 12500}, {"star5", 5, 19098}, {"star6", 6, 28868},
                                           {"star7", 7, 34601}, {"star8", 8, 38250}, {"star10", 10, 42533},
                                           {"star12", 12, 37500}, {"star16", 16, 37500}, {"star24", 24, 37500},
                                           {"star32", 32, 37500}})
            t[name] = [points, inner](const Ctx &c) { return closed(star(c.w, c.h, points, c.a("adj", inner) * 2)); };
        t["rightArrow"] = [](const Ctx &c) {
            return closed(rightArrow(c.w, c.h, c.a("adj1", 50000), c.ss * c.a("adj2", 50000)));
        };
        t["leftArrow"] = [](const Ctx &c) {
            return closed(QTransform(-1, 0, 0, 1, c.w, 0).map(rightArrow(c.w, c.h, c.a("adj1", 50000), c.ss * c.a("adj2", 50000))));
        };
        t["downArrow"] = [](const Ctx &c) {
            return closed(QTransform(0, 1, 1, 0, 0, 0).map(rightArrow(c.h, c.w, c.a("adj1", 50000), c.ss * c.a("adj2", 50000))));
        };
        t["upArrow"] = [](const Ctx &c) {
            return closed(QTransform(0, -1, 1, 0, 0, c.h).map(rightArrow(c.h, c.w, c.a("adj1", 50000), c.ss * c.a("adj2", 50000))));
        };
        t["leftRightArrow"] = [](const Ctx &c) {
            double y1 = c.h / 2 * (1 - c.a("adj1", 50000)), y2 = c.h - y1, x = c.ss * c.a("adj2", 50000);
            return closed(polygon({{0, c.h / 2}, {x, 0}, {x, y1}, {c.w - x, y1}, {c.w - x, 0}, {c.w, c.h / 2},
                                   {c.w - x, c.h}, {c.w - x, y2}, {x, y2}, {x, c.h}}));
        };
        t["chevron"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 50000);
            return closed(polygon({{0, 0}, {c.w - x, 0}, {c.w, c.h / 2}, {c.w - x, c.h}, {0, c.h}, {x, c.h / 2}}));
        };
        t["homePlate"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 50000);
            return closed(polygon({{0, 0}, {c.w - x, 0}, {c.w, c.h / 2}, {c.w - x, c.h}, {0, c.h}}));
        };
        t["snip1Rect"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 16667);
            return closed(polygon({{0, 0}, {c.w - x, 0}, {c.w, x}, {c.w, c.h}, {0, c.h}}));
        };
        t["foldedCorner"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 16667);
            Outline o = closed(polygon({{0, 0}, {c.w, 0}, {c.w, c.h - x}, {c.w - x, c.h}, {0, c.h}}));
            o.stroke.addPolygon(QPolygonF({QPointF(c.w, c.h - x), QPointF(c.w - x * 0.8, c.h - x * 0.8), QPointF(c.w - x, c.h)}));
            return o;
        };
        t["frame"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj1", 12500);
            QPainterPath p;
            p.addRect(0, 0, c.w, c.h);
            p.addRect(x, x, c.w - 2 * x, c.h - 2 * x);
            return closed(p);
        };
        t["donut"] = [](const Ctx &c) {
            double x = c.ss * c.a("adj", 25000);
            QPainterPath p;
            p.addEllipse(0, 0, c.w, c.h);
            p.addEllipse(x, x, c.w - 2 * x, c.h - 2 * x);
            return closed(p);
        };
        t["can"] = [](const Ctx &c) {
            double e = c.ss * c.a("adj", 25000);
            QPainterPath p;
            p.moveTo(0, e / 2);
            p.lineTo(0, c.h - e / 2);
            p.arcTo(0, c.h - e, c.w, e, 180, 180);
            p.lineTo(c.w, e / 2);
            p.arcTo(0, 0, c.w, e, 0, 180);
            p.closeSubpath();
            Outline o = closed(p);
            o.stroke.addEllipse(0, 0, c.w, e);
            return o;
        };
        t["heart"] = [](const Ctx &c) {
            QPainterPath p;
            p.moveTo(c.w / 2, c.h / 4);
            p.cubicTo(c.w * 0.65, -c.h / 12, c.w * 1.25, c.h / 6, c.w / 2, c.h);
            p.cubicTo(-c.w / 4, c.h / 6, c.w * 0.35, -c.h / 12, c.w / 2, c.h / 4);
            return closed(p);
        };
        t["lightningBolt"] = [](const Ctx &c) {
            QPolygonF poly{{8458, 0}, {12128, 6146}, {10368, 6915}, {16063, 12176}, {14563, 12934}, {21600, 21600},
                           {10178, 14701}, {12079, 13871}, {4648, 9374}, {6772, 8457}, {0, 3923}};
            QPainterPath p;
            p.addPolygon(QTransform::fromScale(c.w / 21600, c.h / 21600).map(poly));
            p.closeSubpath();
            return closed(p);
        };
        t["wedgeRectCallout"] = [](const Ctx &c) {
            QPainterPath body;
            body.addRect(0, 0, c.w, c.h);
            return closed(callout(body, c.w, c.h, {c.w / 2 + c.w * c.a("adj1", -20833), c.h / 2 + c.h * c.a("adj2", 62500)}));
        };
        t["wedgeRoundRectCallout"] = [](const Ctx &c) {
            QPainterPath body;
            body.addRoundedRect(0, 0, c.w, c.h, c.ss / 6, c.ss / 6);
            return closed(callout(body, c.w, c.h, {c.w / 2 + c.w * c.a("adj1", -20833), c.h / 2 + c.h * c.a("adj2", 62500)}));
        };
        t["wedgeEllipseCallout"] = [](const Ctx &c) {
            QPainterPath body;
            body.addEllipse(0, 0, c.w, c.h);
            return closed(callout(body, c.w, c.h, {c.w / 2 + c.w * c.a("adj1", -20833), c.h / 2 + c.h * c.a("adj2", 62500)}));
        };
        t["line"] = [](const Ctx &c) {
            QPainterPath p;
            p.moveTo(0, 0);
            p.lineTo(c.w, c.h);
            return open(p);
        };
        t["bentConnector2"] = [](const Ctx &c) {
            QPainterPath p;
            p.moveTo(0, 0);
            p.lineTo(c.w, 0);
            p.lineTo(c.w, c.h);
            return open(p);
        };
        t["bentConnector3"] = [](const Ctx &c) {
            double x = c.w * c.a("adj1", 50000);
            QPainterPath p;
            p.moveTo(0, 0);
            p.lineTo(x, 0);
            p.lineTo(x, c.h);
            p.lineTo(c.w, c.h);
            return open(p);
        };
        t["curvedConnector3"] = [](const Ctx &c) {
            double x = c.w * c.a("adj1", 50000);
            QPainterPath p;
            p.moveTo(0, 0);
            p.cubicTo(x, 0, x, c.h, c.w, c.h);
            return open(p);
        };
        t["rightBracket"] = [](const Ctx &c) {
            double r = std::min(c.ss * c.a("adj", 8333), c.h / 2);
            QPainterPath p;
            p.moveTo(0, 0);
            p.quadTo(c.w, 0, c.w, r);
            p.lineTo(c.w, c.h - r);
            p.quadTo(c.w, c.h, 0, c.h);
            return open(p);
        };
        t["rightBrace"] = [](const Ctx &c) {
            double r = std::min(c.ss * c.a("adj1", 8333), c.h / 4), y = c.h * c.a("adj2", 50000), m = c.w / 2;
            QPainterPath p;
            p.moveTo(0, 0);
            p.quadTo(m, 0, m, r);
            p.lineTo(m, y - r);
            p.quadTo(m, y, c.w, y);
            p.quadTo(m, y, m, y + r);
            p.lineTo(m, c.h - r);
            p.quadTo(m, c.h, 0, c.h);
            return open(p);
        };
        t["arc"] = [](const Ctx &c) {
            double st = c.adjust.value("adj1", 16200000) / 60000, end = c.adjust.value("adj2", 0) / 60000;
            double sweep = std::fmod(end - st + 720, 360);
            QPainterPath p;
            p.arcMoveTo(0, 0, c.w, c.h, -st);
            p.arcTo(0, 0, c.w, c.h, -st, -sweep);
            return open(p);
        };
        t["pie"] = [](const Ctx &c) {
            double st = c.adjust.value("adj1", 0) / 60000, end = c.adjust.value("adj2", 16200000) / 60000;
            double sweep = std::fmod(end - st + 720, 360);
            QPainterPath p;
            p.moveTo(c.w / 2, c.h / 2);
            p.arcTo(0, 0, c.w, c.h, -st, -sweep);
            p.closeSubpath();
            return closed(p);
        };
        auto mirror = [&t](const char *from, const char *to) {
            Builder b = t[from];
            t[to] = [b](const Ctx &c) {
                Outline o = b(c);
                QTransform m(-1, 0, 0, 1, c.w, 0);
                return Outline{m.map(o.fill), m.map(o.stroke)};
            };
        };
        mirror("rightBracket", "leftBracket");
        mirror("rightBrace", "leftBrace");
        t["straightConnector1"] = t["line"];
        t["flowChartProcess"] = t["rect"];
        t["flowChartDecision"] = t["diamond"];
        t["flowChartConnector"] = t["ellipse"];
        t["flowChartTerminator"] = [](const Ctx &c) {
            QPainterPath p;
            p.addRoundedRect(0, 0, c.w, c.h, std::min(c.w, c.h) / 2, c.h / 2);
            return closed(p);
        };
        t["flowChartAlternateProcess"] = [](const Ctx &c) {
            QPainterPath p;
            p.addRoundedRect(0, 0, c.w, c.h, c.ss / 6, c.ss / 6);
            return closed(p);
        };
        return t;
    }();
    return table;
}

} // namespace

Outline preset(const QString &name, QSizeF size, const QMap<QString, double> &adjust)
{
    Ctx c{size.width(), size.height(), std::min(size.width(), size.height()), adjust};
    auto it = builders().constFind(name);
    return it != builders().cend() ? (*it)(c) : builders()["rect"](c);
}

Outline custom(const QList<CustomPath> &paths, QSizeF size)
{
    Outline o;
    for (const CustomPath &cp : paths) {
        QTransform t = QTransform::fromScale(cp.size.width() > 0 ? size.width() / cp.size.width() : 1,
                                             cp.size.height() > 0 ? size.height() / cp.size.height() : 1);
        QPainterPath p = t.map(cp.path);
        if (cp.fill) o.fill.addPath(p);
        if (cp.stroke) o.stroke.addPath(p);
    }
    return o;
}

Outline of(const ShapeEl &shape, QSizeF size)
{
    return shape.custom.isEmpty() ? preset(shape.preset, size, shape.adjust) : custom(shape.custom, size);
}

void arcTo(QPainterPath &path, double wR, double hR, double startDeg, double sweepDeg)
{
    double a = qDegreesToRadians(startDeg);
    QPointF center = path.currentPosition() - QPointF(wR * std::cos(a), hR * std::sin(a));
    path.arcTo(QRectF(center.x() - wR, center.y() - hR, 2 * wR, 2 * hR), -startDeg, -sweepDeg);
}

bool isLine(const QString &name)
{
    return name == "line" || name.contains("Connector");
}

QStringList insertable()
{
    return {"rect",       "roundRect",      "ellipse",        "triangle",        "rtTriangle",     "diamond",
            "parallelogram", "trapezoid",   "pentagon",       "hexagon",         "octagon",        "plus",
            "star5",      "star8",          "rightArrow",     "leftArrow",       "upArrow",        "downArrow",
            "leftRightArrow", "chevron",    "homePlate",      "heart",           "lightningBolt",  "donut",
            "can",        "frame",          "wedgeRectCallout", "wedgeEllipseCallout", "leftBrace", "rightBrace",
            "line",       "straightConnector1"};
}

} // namespace geometry
