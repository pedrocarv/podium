// Reads a .pptx into the model. Inheritance (theme → master → layout → slide) is resolved here so the
// rest of the app only sees explicit properties; each element keeps its source XML for the writer.

#include "geometry.h"
#include "pptx.h"
#include "xml.h"

#include <QObject>
#include <QSet>
#include <QtMath>

#include <functional>
#include <map>

using pugi::xml_attribute;
using pugi::xml_node;

namespace pptx {
namespace {

double emu(xml_attribute a, double fallback = 0) { return a ? double(a.as_llong()) / emuPerPt : fallback; }
double fraction(xml_attribute a, double fallback = 0) { return a ? a.as_double() / 100000.0 : fallback; }

bool flag(xml_attribute a, bool fallback = false)
{
    if (!a) return fallback;
    QString v = str(a);
    return v == "1" || v == "true" || v == "on";
}

bool is(xml_node n, const char *name) { return qstrcmp(n.name(), name) == 0; }

xml_node nth(xml_node parent, int i)
{
    for (xml_node c = parent.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element && i-- == 0) return c;
    return {};
}

xml_node nvProps(xml_node shape)
{
    for (xml_node c : shape.children())
        if (QByteArray(c.name()).startsWith("p:nv")) return c;
    return {};
}

Placeholder placeholderOf(xml_node shape)
{
    xml_node ph = nvProps(shape).child("p:nvPr").child("p:ph");
    if (!ph) return {};
    return {ph.attribute("type") ? str(ph.attribute("type")) : "obj", ph.attribute("idx").as_int(0), {}};
}

QString phClass(const QString &type)
{
    if (type == "title" || type == "ctrTitle") return "title";
    if (type == "dt" || type == "ftr" || type == "sldNum" || type == "hdr") return type;
    return "body";
}

QString defaultPrompt(const QString &type)
{
    if (phClass(type) == "title") return QObject::tr("Click to add title");
    if (type == "subTitle") return QObject::tr("Click to add subtitle");
    if (type == "pic") return QObject::tr("Click to add picture");
    if (phClass(type) == "body") return QObject::tr("Click to add text");
    return {};
}

QString paragraphsText(xml_node txBody)
{
    QStringList lines;
    for (xml_node p : txBody.children("a:p")) {
        QString line;
        for (xml_node r : p.children()) line += is(r, "a:br") ? QString("\n") : str(r.child_value("a:t"));
        lines << line;
    }
    return lines.join('\n');
}

xml_node findPh(xml_node tree, const Placeholder &ph, bool byIndex)
{
    xml_node byType;
    for (xml_node n : tree.children()) {
        Placeholder p = placeholderOf(n);
        if (!p || phClass(p.type) != phClass(ph.type)) continue;
        if (byIndex && p.idx == ph.idx) return n;
        if (!byType) byType = n;
    }
    return byType;
}

void applyXfrm(Element &e, xml_node x)
{
    xml_node off = x.child("a:off"), ext = x.child("a:ext");
    e.rect = QRectF(emu(off.attribute("x")), emu(off.attribute("y")), emu(ext.attribute("cx")),
                    emu(ext.attribute("cy")));
    e.rotation = std::fmod(std::fmod(x.attribute("rot").as_double() / 60000.0, 360) + 360, 360);
    e.flipH = flag(x.attribute("flipH"));
    e.flipV = flag(x.attribute("flipV"));
}

QList<CustomPath> customGeometry(xml_node custGeom)
{
    QList<CustomPath> out;
    auto pt = [](xml_node n) { return QPointF(n.attribute("x").as_double(), n.attribute("y").as_double()); };
    for (xml_node path : custGeom.child("a:pathLst").children("a:path")) {
        CustomPath cp;
        cp.size = QSizeF(path.attribute("w").as_double(), path.attribute("h").as_double());
        cp.fill = str(path.attribute("fill")) != "none";
        cp.stroke = flag(path.attribute("stroke"), true);
        for (xml_node c : path.children()) {
            if (is(c, "a:moveTo")) cp.path.moveTo(pt(c.child("a:pt")));
            else if (is(c, "a:lnTo")) cp.path.lineTo(pt(c.child("a:pt")));
            else if (is(c, "a:cubicBezTo")) cp.path.cubicTo(pt(nth(c, 0)), pt(nth(c, 1)), pt(nth(c, 2)));
            else if (is(c, "a:quadBezTo")) cp.path.quadTo(pt(nth(c, 0)), pt(nth(c, 1)));
            else if (is(c, "a:close")) cp.path.closeSubpath();
            else if (is(c, "a:arcTo"))
                geometry::arcTo(cp.path, c.attribute("wR").as_double(), c.attribute("hR").as_double(),
                                c.attribute("stAng").as_double() / 60000, c.attribute("swAng").as_double() / 60000);
        }
        if (cp.size.isEmpty()) cp.size = cp.path.boundingRect().size();
        out << cp;
    }
    return out;
}

double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double toSrgb(double c) { return std::clamp(c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055, 0.0, 1.0); }

QColor mapLinear(QColor c, auto &&f)
{
    return QColor::fromRgbF(float(toSrgb(f(toLinear(c.redF())))), float(toSrgb(f(toLinear(c.greenF())))),
                            float(toSrgb(f(toLinear(c.blueF())))), c.alphaF());
}

QColor mapHsl(QColor c, double ds, double dl, double ms = 1, double ml = 1)
{
    QColor h = c.toHsl();
    float hue = std::max(0.0f, h.hslHueF());
    return QColor::fromHslF(hue, std::clamp(float(h.hslSaturationF() * ms + ds), 0.0f, 1.0f),
                            std::clamp(float(h.lightnessF() * ml + dl), 0.0f, 1.0f), c.alphaF())
        .toRgb();
}

// Serializes `n` with the namespace declarations it inherits from its ancestors (other than a/r/p,
// which every slide declares), so it stays valid when inserted into another part.
QByteArray selfContained(xml_node n)
{
    pugi::xml_document doc;
    xml_node copy = doc.append_copy(n);
    QSet<QByteArray> prefixes;
    std::function<void(xml_node)> scan = [&](xml_node x) {
        auto add = [&](QByteArray name) {
            int colon = name.indexOf(':');
            if (colon > 0 && !name.startsWith("xmlns")) prefixes.insert(name.left(colon));
        };
        add(x.name());
        for (xml_attribute a : x.attributes()) {
            add(a.name());
            if (qstrcmp(a.name(), "mc:Ignorable") == 0)
                for (const QByteArray &p : QByteArray(a.value()).split(' ')) prefixes.insert(p);
        }
        for (xml_node c : x.children())
            if (c.type() == pugi::node_element) scan(c);
    };
    scan(copy);
    for (const QByteArray &prefix : prefixes) {
        QByteArray decl = "xmlns:" + prefix;
        if (prefix == "a" || prefix == "r" || prefix == "p" || prefix == "xml" || copy.attribute(decl.constData())) continue;
        for (xml_node up = n.parent(); up; up = up.parent())
            if (xml_attribute a = up.attribute(decl.constData())) {
                copy.append_attribute(decl.constData()) = a.value();
                break;
            }
    }
    return serialize(copy);
}

// A parsed XML part with its relationships.
struct Part {
    QString name;
    QByteArray bytes;
    std::shared_ptr<pugi::xml_document> doc = std::make_shared<pugi::xml_document>();
    QHash<QString, Rel> rels;
    xml_node root() const { return doc->document_element(); }
    QString target(xml_attribute rid) const { return rels.value(str(rid)).target; }
    QString targetOf(const QString &type) const
    {
        for (const Rel &r : rels)
            if (r.type == type) return r.target;
        return {};
    }
};

// The nodes a shape inherits from, nearest first: own node, layout placeholder, master placeholder.
using Chain = QList<std::pair<xml_node, const Part *>>;
using Parents = QList<std::pair<xml_node /*spTree*/, const Part *>>;

struct ThemeData {
    Part part;
    QMap<QString, QColor> scheme;
    QString major = "Calibri", minor = "Calibri";
    xml_node fills, lines, bgFills;
};

struct MasterData {
    Part part;
    const ThemeData *theme = nullptr;
    QMap<QString, QString> clrMap;
    xml_node titleStyle, bodyStyle, otherStyle;
};

struct LayoutData {
    Part part;
    const MasterData *master = nullptr;
};

class Reader {
public:
    explicit Reader(std::shared_ptr<Package> pkg) : pkg(std::move(pkg)) {}
    std::optional<Deck> read(QString *error);

private:
    enum Filter { All, Decor, Placeholders };

    std::shared_ptr<Package> pkg;
    Deck deck;
    xml_node defaultTextStyle;
    std::map<QString, ThemeData> themes;
    std::map<QString, MasterData> masters;
    std::map<QString, LayoutData> layouts;
    QHash<QString, int> layoutIndex;
    const MasterData *master = nullptr;
    QMap<QString, QString> clrMap;

    Part load(const QString &name) const;
    const ThemeData &theme(const QString &name);
    MasterData &masterData(const QString &name);
    void activate(const MasterData &m);
    void readLayout(const QString &name, const MasterData &m);
    Slide readSlide(const QString &name);
    QString readNotes(const QString &name) const;

    QColor schemeColor(const QString &name) const;
    QColor color(xml_node c, QColor phClr) const;
    std::optional<QColor> colorIn(xml_node parent, QColor phClr = {}) const;
    std::optional<Fill> fillElement(xml_node e, const Part &part, QColor phClr) const;
    std::optional<Fill> fillIn(xml_node parent, const Part &part, QColor phClr = {}) const;
    std::optional<Fill> styleFill(xml_node style) const;
    void applyLine(Line &line, xml_node ln, const Part &part, QColor phClr = {}) const;
    void styleLine(Line &line, xml_node style) const;
    std::optional<Fill> background(xml_node cSld, const Part &part) const;
    QString font(const QString &typeface) const;
    void applyRPr(CharStyle &c, xml_node rPr, const Part &part) const;
    void applyPPr(ParaStyle &p, xml_node pPr) const;
    void applyBodyPr(TextBody &b, xml_node bodyPr) const;
    TextBody text(xml_node txBody, const QList<xml_node> &inherited, const QString &phType,
                  std::optional<QColor> fontColor, const QString &fontFace, const Part &part) const;

    QList<Element> tree(xml_node container, const Part &part, const Parents &parents, Filter filter) const;
    std::optional<Element> element(xml_node n, const Part &part, const Parents &parents) const;
    Chain chain(xml_node shape, const Part &part, const Placeholder &ph, const Parents &parents) const;
    void base(Element &e, xml_node shape, const Part &part) const;
    QString prompt(const Chain &ch, const Placeholder &ph) const;
    Element shape(xml_node sp, const Part &part, const Parents &parents) const;
    Element picture(xml_node pic, const Part &part, const Parents &parents) const;
    Element group(xml_node grp, const Part &part, const Parents &parents) const;
    Element frame(xml_node frame, const Part &part) const;
    Element raw(xml_node n, const Part &part, const Parents &parents) const;
    TableEl table(xml_node tbl, const Part &part) const;
};

Part Reader::load(const QString &name) const
{
    Part p;
    p.name = name;
    p.bytes = pkg->read(name);
    loadXml(*p.doc, p.bytes);
    for (const Rel &r : pkg->rels(name)) p.rels.insert(r.id, r);
    return p;
}

const ThemeData &Reader::theme(const QString &name)
{
    if (auto it = themes.find(name); it != themes.end()) return it->second;
    ThemeData &t = themes[name];
    t.part = load(name);
    xml_node elements = t.part.root().child("a:themeElements");
    for (xml_node c : elements.child("a:clrScheme").children())
        t.scheme.insert(str(c.name()).mid(2), color(c.first_child(), {}));
    xml_node fonts = elements.child("a:fontScheme");
    t.major = str(fonts.child("a:majorFont").child("a:latin").attribute("typeface"));
    t.minor = str(fonts.child("a:minorFont").child("a:latin").attribute("typeface"));
    xml_node fmt = elements.child("a:fmtScheme");
    t.fills = fmt.child("a:fillStyleLst");
    t.lines = fmt.child("a:lnStyleLst");
    t.bgFills = fmt.child("a:bgFillStyleLst");
    return t;
}

MasterData &Reader::masterData(const QString &name)
{
    if (auto it = masters.find(name); it != masters.end()) return it->second;
    MasterData &m = masters[name];
    m.part = load(name);
    m.theme = &theme(m.part.targetOf(rel::theme));
    for (xml_attribute a : m.part.root().child("p:clrMap").attributes()) m.clrMap.insert(str(a.name()), str(a));
    xml_node styles = m.part.root().child("p:txStyles");
    m.titleStyle = styles.child("p:titleStyle");
    m.bodyStyle = styles.child("p:bodyStyle");
    m.otherStyle = styles.child("p:otherStyle");
    return m;
}

void Reader::activate(const MasterData &m)
{
    master = &m;
    clrMap = m.clrMap;
}

std::optional<Deck> Reader::read(QString *error)
{
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return std::nullopt;
    };
    QString main;
    for (const Rel &r : pkg->rels(""))
        if (r.type == rel::officeDocument) main = r.target;
    Part presentation = load(main);
    xml_node pres = presentation.root();
    if (!is(pres, "p:presentation")) return fail(QObject::tr("Not a PowerPoint presentation."));

    xml_node size = pres.child("p:sldSz");
    deck.size = QSizeF(emu(size.attribute("cx"), 960), emu(size.attribute("cy"), 540));
    defaultTextStyle = pres.child("p:defaultTextStyle");

    for (xml_node id : pres.child("p:sldMasterIdLst").children("p:sldMasterId")) {
        const MasterData &m = masterData(presentation.target(id.attribute("r:id")));
        for (xml_node lid : m.part.root().child("p:sldLayoutIdLst").children("p:sldLayoutId"))
            readLayout(m.part.target(lid.attribute("r:id")), m);
    }
    if (deck.layouts.isEmpty()) return fail(QObject::tr("The presentation has no slide layouts."));

    activate(masters.begin()->second);
    const ThemeData &t = *master->theme;
    deck.theme.major = t.major;
    deck.theme.minor = t.minor;
    for (QString name : {"tx1", "bg1", "tx2", "bg2", "accent1", "accent2", "accent3", "accent4", "accent5",
                         "accent6", "hlink", "folHlink"})
        deck.theme.colors.insert(name, schemeColor(name));

    for (xml_node id : pres.child("p:sldIdLst").children("p:sldId"))
        deck.slides << readSlide(presentation.target(id.attribute("r:id")));
    deck.package = pkg;
    return deck;
}

void Reader::readLayout(const QString &name, const MasterData &m)
{
    if (name.isEmpty() || layouts.count(name)) return;
    activate(m);
    LayoutData &ld = layouts[name];
    ld.part = load(name);
    ld.master = &m;

    xml_node root = ld.part.root(), cSld = root.child("p:cSld");
    xml_node masterTree = m.part.root().child("p:cSld").child("p:spTree");
    Layout layout;
    layout.part = name;
    layout.name = str(cSld.attribute("name"));
    std::optional<Fill> bg = background(cSld, ld.part);
    if (!bg) bg = background(m.part.root().child("p:cSld"), m.part);
    layout.background = bg.value_or(Fill::solid(Qt::white));
    if (flag(root.attribute("showMasterSp"), true)) layout.decor = tree(masterTree, m.part, {}, Decor);
    Parents parents{{masterTree, &m.part}};
    layout.decor += tree(cSld.child("p:spTree"), ld.part, parents, Decor);
    layout.placeholders = tree(cSld.child("p:spTree"), ld.part, parents, Placeholders);
    for (Element &e : layout.placeholders) {
        e.source.clear();
        e.rels.clear();
        if (TextBody *body = e.text()) body->paragraphs = {Paragraph{body->levels[0].para, {}, body->levels[0].chr}};
    }
    layoutIndex.insert(name, int(deck.layouts.size()));
    deck.layouts << layout;
}

Slide Reader::readSlide(const QString &name)
{
    Part part = load(name);
    Slide s;
    s.source = part.bytes;
    s.rels = part.rels.values();
    std::sort(s.rels.begin(), s.rels.end(), [](const Rel &a, const Rel &b) { return a.id < b.id; });

    QString layoutName = part.targetOf(rel::slideLayout);
    if (!layouts.count(layoutName)) layoutName = layouts.begin()->first;
    const LayoutData &ld = layouts.at(layoutName);
    s.layout = layoutIndex.value(layoutName);
    activate(*ld.master);

    xml_node root = part.root(), cSld = root.child("p:cSld");
    if (xml_node o = root.child("p:clrMapOvr").child("a:overrideClrMapping"))
        for (xml_attribute a : o.attributes()) clrMap.insert(str(a.name()), str(a));

    s.background = background(cSld, part).value_or(deck.layouts[s.layout].background);
    s.showMaster = flag(root.attribute("showMasterSp"), true);
    s.hidden = !flag(root.attribute("show"), true);
    Parents parents{{ld.part.root().child("p:cSld").child("p:spTree"), &ld.part},
                    {ld.master->part.root().child("p:cSld").child("p:spTree"), &ld.master->part}};
    s.elements = tree(cSld.child("p:spTree"), part, parents, All);
    s.notes = readNotes(part.targetOf(rel::notesSlide));
    s.transition = readTransition(root);
    s.builds = readBuilds(root);
    return s;
}

QString Reader::readNotes(const QString &name) const
{
    if (name.isEmpty()) return {};
    Part part = load(name);
    for (xml_node sp : part.root().child("p:cSld").child("p:spTree").children("p:sp")) {
        if (placeholderOf(sp).type != "body") continue;
        return paragraphsText(sp.child("p:txBody")).trimmed();
    }
    return {};
}

// --- DrawingML ---

QColor Reader::schemeColor(const QString &name) const
{
    return master->theme->scheme.value(clrMap.value(name, name), Qt::black);
}

QColor Reader::color(xml_node c, QColor phClr) const
{
    QString v = str(c.attribute("val"));
    QColor col;
    if (is(c, "a:srgbClr")) col = QColor("#" + v);
    else if (is(c, "a:schemeClr")) col = v == "phClr" ? phClr : schemeColor(v);
    else if (is(c, "a:sysClr"))
        col = c.attribute("lastClr") ? QColor("#" + str(c.attribute("lastClr"))) : QColor(v == "window" ? Qt::white : Qt::black);
    else if (is(c, "a:prstClr")) col = QColor(v.toLower());
    else if (is(c, "a:scrgbClr"))
        col = QColor::fromRgbF(float(toSrgb(fraction(c.attribute("r")))), float(toSrgb(fraction(c.attribute("g")))),
                               float(toSrgb(fraction(c.attribute("b")))));
    else if (is(c, "a:hslClr"))
        col = QColor::fromHslF(float(c.attribute("hue").as_double() / 60000 / 360), float(fraction(c.attribute("sat"))),
                               float(fraction(c.attribute("lum"))));
    else return {};
    if (!col.isValid()) col = Qt::black;

    for (xml_node t : c.children()) {
        double val = fraction(t.attribute("val"));
        if (is(t, "a:alpha")) col.setAlphaF(float(val));
        else if (is(t, "a:lumMod")) col = mapHsl(col, 0, 0, 1, val);
        else if (is(t, "a:lumOff")) col = mapHsl(col, 0, val);
        else if (is(t, "a:satMod")) col = mapHsl(col, 0, 0, val, 1);
        else if (is(t, "a:tint")) col = mapLinear(col, [val](double x) { return x * val + (1 - val); });
        else if (is(t, "a:shade")) col = mapLinear(col, [val](double x) { return x * val; });
    }
    return QColor(col.red(), col.green(), col.blue(), col.alpha()); // 8 bits per channel, as saved
}

std::optional<QColor> Reader::colorIn(xml_node parent, QColor phClr) const
{
    for (xml_node c : parent.children())
        if (QColor col = color(c, phClr); col.isValid()) return col;
    return std::nullopt;
}

std::optional<Fill> Reader::fillElement(xml_node e, const Part &part, QColor phClr) const
{
    Fill f;
    if (is(e, "a:noFill") || is(e, "a:grpFill")) return f;
    if (is(e, "a:solidFill") || is(e, "a:pattFill")) {
        f.kind = Fill::Solid;
        f.color = colorIn(is(e, "a:pattFill") ? e.child("a:fgClr") : e, phClr).value_or(Qt::black);
        return f;
    }
    if (is(e, "a:gradFill")) {
        f.kind = Fill::Gradient;
        for (xml_node gs : e.child("a:gsLst").children("a:gs"))
            f.stops << QGradientStop{fraction(gs.attribute("pos")), colorIn(gs, phClr).value_or(Qt::black)};
        std::sort(f.stops.begin(), f.stops.end(), [](auto &a, auto &b) { return a.first < b.first; });
        f.angle = e.child("a:lin") ? e.child("a:lin").attribute("ang").as_double() / 60000 : 90;
        if (f.stops.isEmpty()) return Fill{};
        return f;
    }
    if (is(e, "a:blipFill")) {
        f.kind = Fill::Image;
        f.image = part.target(e.child("a:blip").attribute("r:embed"));
        return f;
    }
    return std::nullopt;
}

std::optional<Fill> Reader::fillIn(xml_node parent, const Part &part, QColor phClr) const
{
    for (xml_node c : parent.children())
        if (auto f = fillElement(c, part, phClr)) return f;
    return std::nullopt;
}

std::optional<Fill> Reader::styleFill(xml_node style) const
{
    xml_node ref = style.child("a:fillRef");
    int idx = ref.attribute("idx").as_int();
    if (!ref || idx == 0) return std::nullopt;
    xml_node e = idx >= 1001 ? nth(master->theme->bgFills, idx - 1001) : nth(master->theme->fills, idx - 1);
    return fillElement(e, master->theme->part, colorIn(ref).value_or(QColor()));
}

void Reader::applyLine(Line &line, xml_node ln, const Part &part, QColor phClr) const
{
    if (!ln) return;
    if (xml_attribute w = ln.attribute("w")) line.width = emu(w);
    if (auto f = fillIn(ln, part, phClr)) line.fill = *f;
    if (xml_node d = ln.child("a:prstDash")) line.dash = str(d.attribute("val"));
    auto end = [](LineEnd &e, xml_node n) {
        if (!n) return;
        if (n.attribute("type")) e.type = str(n.attribute("type"));
        if (n.attribute("w")) e.w = str(n.attribute("w"));
        if (n.attribute("len")) e.len = str(n.attribute("len"));
    };
    end(line.head, ln.child("a:headEnd"));
    end(line.tail, ln.child("a:tailEnd"));
}

void Reader::styleLine(Line &line, xml_node style) const
{
    xml_node ref = style.child("a:lnRef");
    int idx = ref.attribute("idx").as_int();
    if (idx > 0) applyLine(line, nth(master->theme->lines, idx - 1), master->theme->part, colorIn(ref).value_or(QColor()));
}

std::optional<Fill> Reader::background(xml_node cSld, const Part &part) const
{
    xml_node bg = cSld.child("p:bg");
    if (xml_node pr = bg.child("p:bgPr")) return fillIn(pr, part);
    if (xml_node ref = bg.child("p:bgRef")) {
        int idx = ref.attribute("idx").as_int();
        xml_node e = idx >= 1001 ? nth(master->theme->bgFills, idx - 1001) : nth(master->theme->fills, idx - 1);
        return fillElement(e, master->theme->part, colorIn(ref).value_or(QColor()));
    }
    return std::nullopt;
}

QString Reader::font(const QString &typeface) const
{
    if (typeface.startsWith("+mj")) return master->theme->major;
    if (typeface.startsWith("+mn")) return master->theme->minor;
    return typeface;
}

void Reader::applyRPr(CharStyle &c, xml_node rPr, const Part &part) const
{
    if (!rPr) return;
    if (xml_attribute a = rPr.attribute("sz")) c.size = a.as_double() / 100;
    if (xml_attribute a = rPr.attribute("b")) c.bold = flag(a);
    if (xml_attribute a = rPr.attribute("i")) c.italic = flag(a);
    if (xml_attribute a = rPr.attribute("u")) c.underline = str(a) != "none";
    if (xml_attribute a = rPr.attribute("strike")) c.strike = str(a) != "noStrike";
    if (xml_attribute a = rPr.attribute("baseline")) c.baseline = a.as_int() / 1000;
    if (auto f = fillIn(rPr, part)) {
        if (f->kind == Fill::Solid) c.color = f->color;
        else if (f->kind == Fill::Gradient) c.color = f->stops.first().second;
        else if (f->kind == Fill::None) c.color = Qt::transparent;
    }
    if (xml_node latin = rPr.child("a:latin"); latin && latin.attribute("typeface"))
        c.font = font(str(latin.attribute("typeface")));
    if (xml_node link = rPr.child("a:hlinkClick")) c.link = part.target(link.attribute("r:id"));
}

void Reader::applyPPr(ParaStyle &p, xml_node pPr) const
{
    if (!pPr) return;
    if (xml_attribute a = pPr.attribute("algn")) {
        QString v = str(a);
        p.align = v == "ctr"                ? Qt::AlignHCenter
                  : v == "r"                ? Qt::AlignRight
                  : v == "just" || v == "dist" ? Qt::AlignJustify
                                            : Qt::AlignLeft;
    }
    if (xml_attribute a = pPr.attribute("marL")) p.marL = emu(a);
    if (xml_attribute a = pPr.attribute("indent")) p.indent = emu(a);
    auto spacing = [](xml_node n, Spacing &s) {
        if (xml_node v = n.child("a:spcPct")) s = {fraction(v.attribute("val")), true};
        else if (xml_node v = n.child("a:spcPts")) s = {v.attribute("val").as_double() / 100, false};
    };
    spacing(pPr.child("a:lnSpc"), p.lineSpacing);
    spacing(pPr.child("a:spcBef"), p.spaceBefore);
    spacing(pPr.child("a:spcAft"), p.spaceAfter);
    Bullet &b = p.bullet;
    for (xml_node c : pPr.children()) {
        if (is(c, "a:buNone")) b.kind = Bullet::None;
        else if (is(c, "a:buChar")) b.kind = Bullet::Char, b.chr = str(c.attribute("char"));
        else if (is(c, "a:buAutoNum"))
            b.kind = Bullet::Number, b.scheme = str(c.attribute("type")), b.startAt = c.attribute("startAt").as_int(1);
        else if (is(c, "a:buFont")) b.font = font(str(c.attribute("typeface")));
        else if (is(c, "a:buFontTx")) b.font.clear();
        else if (is(c, "a:buSzPct")) b.scale = fraction(c.attribute("val"), 1);
        else if (is(c, "a:buSzTx")) b.scale = 1;
        else if (is(c, "a:buClr")) b.color = colorIn(c).value_or(QColor());
        else if (is(c, "a:buClrTx")) b.color = QColor();
    }
}

void Reader::applyBodyPr(TextBody &b, xml_node bp) const
{
    if (!bp) return;
    b.insets = QMarginsF(emu(bp.attribute("lIns"), b.insets.left()), emu(bp.attribute("tIns"), b.insets.top()),
                         emu(bp.attribute("rIns"), b.insets.right()), emu(bp.attribute("bIns"), b.insets.bottom()));
    if (xml_attribute a = bp.attribute("anchor"))
        b.anchor = str(a) == "ctr" ? TextBody::Middle : str(a) == "b" ? TextBody::Bottom : TextBody::Top;
    if (xml_attribute a = bp.attribute("wrap")) b.wrap = str(a) != "none";
    if (xml_attribute a = bp.attribute("vert")) b.vert = str(a);
    if (bp.child("a:noAutofit")) b.autofit = TextBody::NoAutofit, b.fontScale = 1, b.lineReduction = 0;
    if (xml_node n = bp.child("a:normAutofit")) {
        b.autofit = TextBody::Shrink;
        b.fontScale = fraction(n.attribute("fontScale"), 1);
        b.lineReduction = fraction(n.attribute("lnSpcReduction"), 0);
    }
    if (bp.child("a:spAutoFit")) b.autofit = TextBody::Resize, b.fontScale = 1, b.lineReduction = 0;
}

TextBody Reader::text(xml_node txBody, const QList<xml_node> &inherited, const QString &phType,
                      std::optional<QColor> fontColor, const QString &fontFace, const Part &part) const
{
    TextBody b;
    for (xml_node t : inherited) applyBodyPr(b, t.child("a:bodyPr"));
    applyBodyPr(b, txBody.child("a:bodyPr"));

    QString cls = phType.isEmpty() ? QString() : phClass(phType);
    QList<xml_node> lists{defaultTextStyle, cls == "title" ? master->titleStyle
                                            : cls == "body" ? master->bodyStyle
                                                            : master->otherStyle};
    for (xml_node t : inherited) lists << t.child("a:lstStyle");
    for (int l = 0; l < 9; ++l) {
        LevelStyle &level = b.levels[l];
        level.chr.font = master->theme->minor;
        level.chr.color = schemeColor("tx1");
        QByteArray tag = "a:lvl" + QByteArray::number(l + 1) + "pPr";
        auto apply = [&](xml_node list) {
            applyPPr(level.para, list.child(tag.constData()));
            applyRPr(level.chr, list.child(tag.constData()).child("a:defRPr"), part);
        };
        for (xml_node list : lists) apply(list);
        if (fontColor) level.chr.color = *fontColor;
        if (!fontFace.isEmpty()) level.chr.font = fontFace;
        apply(txBody.child("a:lstStyle"));
        level.para.level = l;
    }

    for (xml_node p : txBody.children("a:p")) {
        xml_node pPr = p.child("a:pPr");
        int lvl = std::clamp(pPr.attribute("lvl").as_int(0), 0, 8);
        Paragraph para{b.levels[lvl].para, {}, b.levels[lvl].chr};
        applyPPr(para.style, pPr);
        para.style.level = lvl;
        for (xml_node r : p.children()) {
            if (!is(r, "a:r") && !is(r, "a:fld") && !is(r, "a:br")) continue;
            Run run{is(r, "a:br") ? QString(QChar::LineSeparator) : str(r.child_value("a:t")), para.endStyle,
                    is(r, "a:fld") ? str(r.attribute("type")) : QString()};
            applyRPr(run.style, r.child("a:rPr"), part);
            if (!para.runs.isEmpty() && run.field.isEmpty() && para.runs.last().field.isEmpty() && para.runs.last().style == run.style)
                para.runs.last().text += run.text; // runs split only by markup (spell check, lang, breaks)
            else
                para.runs << run;
        }
        applyRPr(para.endStyle, p.child("a:endParaRPr"), part);
        b.paragraphs << para;
    }
    return b;
}

// --- elements ---

QList<Element> Reader::tree(xml_node container, const Part &part, const Parents &parents, Filter filter) const
{
    QList<Element> out;
    for (xml_node n : container.children()) {
        std::optional<Element> e = element(n, part, parents);
        if (!e || (filter == Decor && e->ph) || (filter == Placeholders && !e->ph)) continue;
        out << *e;
    }
    return out;
}

std::optional<Element> Reader::element(xml_node n, const Part &part, const Parents &parents) const
{
    Element e;
    if (is(n, "p:sp") || is(n, "p:cxnSp")) e = shape(n, part, parents);
    else if (is(n, "p:pic")) e = picture(n, part, parents);
    else if (is(n, "p:grpSp")) e = group(n, part, parents);
    else if (is(n, "p:graphicFrame")) e = frame(n, part);
    else if (n.type() != pugi::node_element || is(n, "p:nvGrpSpPr") || is(n, "p:grpSpPr") || is(n, "p:extLst"))
        return std::nullopt;
    else e = raw(n, part, parents);
    e.source = selfContained(n);
    QSet<QString> ids;
    std::function<void(xml_node)> collect = [&](xml_node x) {
        for (xml_attribute a : x.attributes())
            if (QByteArray(a.name()).startsWith("r:")) ids.insert(str(a));
        for (xml_node c : x.children()) collect(c);
    };
    collect(n);
    for (const QString &id : ids)
        if (part.rels.contains(id)) e.rels << part.rels[id];
    std::sort(e.rels.begin(), e.rels.end(), [](const Rel &a, const Rel &b) { return a.id < b.id; });
    return e;
}

Chain Reader::chain(xml_node shape, const Part &part, const Placeholder &ph, const Parents &parents) const
{
    Chain c{{shape, &part}};
    Placeholder current = ph;
    for (const auto &[spTree, p] : parents) {
        if (!current) break;
        xml_node match = findPh(spTree, current, c.size() == 1);
        if (!match) continue;
        c << std::pair{match, p};
        current = placeholderOf(match);
    }
    return c;
}

void Reader::base(Element &e, xml_node shape, const Part &) const
{
    xml_node c = nvProps(shape).child("p:cNvPr");
    e.id = c.attribute("id").as_int();
    e.name = str(c.attribute("name"));
    e.hidden = flag(c.attribute("hidden"));
    e.ph = placeholderOf(shape);
}

// The placeholder's prompt ("Click to add title"): a custom one from the layout, or the default.
QString Reader::prompt(const Chain &ch, const Placeholder &ph) const
{
    for (const auto &[n, p] : ch)
        if (flag(nvProps(n).child("p:nvPr").child("p:ph").attribute("hasCustomPrompt"))) return paragraphsText(n.child("p:txBody"));
    return defaultPrompt(ph.type);
}

Element Reader::shape(xml_node sp, const Part &part, const Parents &parents) const
{
    Element e;
    base(e, sp, part);
    Chain ch = chain(sp, part, e.ph, parents);
    if (e.ph) e.ph.prompt = prompt(ch, e.ph);
    for (const auto &[n, p] : ch)
        if (xml_node x = n.child("p:spPr").child("a:xfrm")) {
            applyXfrm(e, x);
            break;
        }

    ShapeEl s;
    s.connector = is(sp, "p:cxnSp");
    s.textBox = flag(nvProps(sp).child("p:cNvSpPr").attribute("txBox"));
    for (const auto &[n, p] : ch) {
        xml_node spPr = n.child("p:spPr");
        if (xml_node g = spPr.child("a:prstGeom")) {
            s.preset = str(g.attribute("prst"));
            for (xml_node gd : g.child("a:avLst").children("a:gd"))
                s.adjust.insert(str(gd.attribute("name")), str(gd.attribute("fmla")).section(' ', 1).toDouble());
            break;
        }
        if (xml_node g = spPr.child("a:custGeom")) {
            s.preset.clear();
            s.custom = customGeometry(g);
            break;
        }
    }

    xml_node style = sp.child("p:style");
    std::optional<Fill> fill;
    for (const auto &[n, p] : ch)
        if ((fill = fillIn(n.child("p:spPr"), *p))) break;
    s.fill = fill ? *fill : styleFill(style).value_or(Fill{});
    s.line.fill = Fill{};
    styleLine(s.line, style);
    for (auto it = ch.rbegin(); it != ch.rend(); ++it) applyLine(s.line, it->first.child("p:spPr").child("a:ln"), *it->second);

    QList<xml_node> inherited;
    for (auto it = ch.rbegin(); it != std::prev(ch.rend()); ++it) inherited << it->first.child("p:txBody");
    xml_node fontRef = style.child("a:fontRef");
    QString face = str(fontRef.attribute("idx")) == "major"   ? master->theme->major
                   : str(fontRef.attribute("idx")) == "minor" ? master->theme->minor
                                                              : QString();
    s.text = text(sp.child("p:txBody"), inherited, e.ph.type, colorIn(fontRef), face, part);
    e.body = s;
    return e;
}

Element Reader::picture(xml_node pic, const Part &part, const Parents &parents) const
{
    Element e;
    base(e, pic, part);
    Chain ch = chain(pic, part, e.ph, parents);
    if (e.ph) e.ph.prompt = prompt(ch, e.ph);
    for (const auto &[n, p] : ch)
        if (xml_node x = n.child("p:spPr").child("a:xfrm")) {
            applyXfrm(e, x);
            break;
        }
    PictureEl p;
    xml_node blipFill = pic.child("p:blipFill");
    p.image = part.target(blipFill.child("a:blip").attribute("r:embed"));
    if (xml_node r = blipFill.child("a:srcRect"))
        p.crop = QMarginsF(fraction(r.attribute("l")), fraction(r.attribute("t")), fraction(r.attribute("r")),
                           fraction(r.attribute("b")));
    if (xml_node g = pic.child("p:spPr").child("a:prstGeom")) p.preset = str(g.attribute("prst"));
    styleLine(p.line, pic.child("p:style"));
    applyLine(p.line, pic.child("p:spPr").child("a:ln"), part);

    xml_node nvPr = nvProps(pic).child("p:nvPr");
    if (xml_node media = nvPr.find_node([](xml_node n) { return is(n, "p14:media"); }))
        p.video = part.target(media.attribute("r:embed"));
    else if (xml_node video = nvPr.child("a:videoFile"); video && !part.rels.value(str(video.attribute("r:link"))).external)
        p.video = part.target(video.attribute("r:link"));
    e.body = p;
    return e;
}

Element Reader::group(xml_node grp, const Part &part, const Parents &parents) const
{
    Element e;
    base(e, grp, part);
    xml_node x = grp.child("p:grpSpPr").child("a:xfrm");
    applyXfrm(e, x);
    GroupEl g;
    xml_node off = x.child("a:chOff"), ext = x.child("a:chExt");
    g.childRect = QRectF(emu(off.attribute("x")), emu(off.attribute("y")), emu(ext.attribute("cx")),
                         emu(ext.attribute("cy")));
    for (const Element &c : tree(grp, part, parents, All)) g.children.push_back(c);
    e.body = std::move(g);
    return e;
}

Element Reader::frame(xml_node frame, const Part &part) const
{
    Element e;
    base(e, frame, part);
    applyXfrm(e, frame.child("p:xfrm"));
    xml_node data = frame.child("a:graphic").child("a:graphicData");
    if (xml_node tbl = data.child("a:tbl")) {
        e.body = table(tbl, part);
        return e;
    }
    QString uri = str(data.attribute("uri"));
    RawEl r;
    r.label = uri.contains("chart") ? QObject::tr("Chart") : uri.contains("diagram") ? QObject::tr("SmartArt") : QObject::tr("Object");
    if (xml_node blip = data.find_node([](xml_node n) { return is(n, "a:blip"); }))
        r.fallback = part.target(blip.attribute("r:embed"));
    e.body = r;
    return e;
}

Element Reader::raw(xml_node n, const Part &part, const Parents &parents) const
{
    // mc:AlternateContent, p:contentPart, …: draw the fallback content, keep the XML untouched.
    Element e;
    RawEl r;
    if (is(n, "mc:AlternateContent")) {
        xml_node fallback = n.child("mc:Fallback");
        xml_node inner = fallback.first_child() ? fallback.first_child() : n.child("mc:Choice").first_child();
        if (std::optional<Element> preview = element(inner, part, parents)) {
            e = *preview;
            preview->source.clear();
            preview->rels.clear();
            r.preview.push_back(*preview);
        }
    } else {
        base(e, n, part);
        if (xml_node x = n.find_node([](xml_node c) { return QByteArray(c.name()).endsWith(":xfrm"); }))
            applyXfrm(e, x);
        r.label = QObject::tr("Object");
    }
    e.body = r;
    return e;
}

TableEl Reader::table(xml_node tbl, const Part &part) const
{
    TableEl t;
    for (xml_node col : tbl.child("a:tblGrid").children("a:gridCol")) t.cols << emu(col.attribute("w"));
    for (xml_node tr : tbl.children("a:tr")) {
        t.rows << emu(tr.attribute("h"));
        QList<Cell> row;
        for (xml_node tc : tr.children("a:tc")) {
            Cell c;
            c.colSpan = tc.attribute("gridSpan").as_int(1);
            c.rowSpan = tc.attribute("rowSpan").as_int(1);
            c.merged = flag(tc.attribute("hMerge")) || flag(tc.attribute("vMerge"));
            xml_node pr = tc.child("a:tcPr");
            c.text = text(tc.child("a:txBody"), {}, {}, std::nullopt, {}, part);
            c.text.insets = QMarginsF(emu(pr.attribute("marL"), 7.2), emu(pr.attribute("marT"), 3.6),
                                      emu(pr.attribute("marR"), 7.2), emu(pr.attribute("marB"), 3.6));
            QString anchor = str(pr.attribute("anchor"));
            c.text.anchor = anchor == "ctr" ? TextBody::Middle : anchor == "b" ? TextBody::Bottom : TextBody::Top;
            c.fill = fillIn(pr, part).value_or(Fill{});
            for (auto [side, tag] : {std::pair{&c.left, "a:lnL"}, {&c.right, "a:lnR"}, {&c.top, "a:lnT"}, {&c.bottom, "a:lnB"}}) {
                side->fill = Fill{};
                applyLine(*side, pr.child(tag), part);
            }
            row << c;
        }
        t.cells << row;
    }
    return t;
}

} // namespace

Transition readTransition(xml_node sld)
{
    xml_node t = sld.child("p:transition");
    for (xml_node ac : sld.children("mc:AlternateContent"))
        if (xml_node c = ac.child("mc:Choice").child("p:transition")) t = c;
    Transition tr;
    if (!t) return tr;
    tr.onClick = flag(t.attribute("advClick"), true);
    tr.after = t.attribute("advTm") ? t.attribute("advTm").as_int() : -1;
    QString spd = str(t.attribute("spd"));
    tr.ms = t.attribute("p14:dur") ? t.attribute("p14:dur").as_int() : spd == "slow" ? 1000 : spd == "med" ? 750 : 500;
    for (xml_node k : t.children()) {
        QString name = str(k.name());
        if (name == "p:sndAc" || name == "p:extLst") continue;
        tr.type = name.section(':', 1);
        tr.dir = str(k.attribute("dir"));
        break;
    }
    return tr;
}

QList<Build> readBuilds(xml_node sld)
{
    QList<Build> out;
    xml_node root = sld.child("p:timing").child("p:tnLst").child("p:par").child("p:cTn");
    xml_node main;
    for (xml_node seq : root.child("p:childTnLst").children("p:seq"))
        if (str(seq.child("p:cTn").attribute("nodeType")) == "mainSeq") main = seq.child("p:cTn");
    for (xml_node click : main.child("p:childTnLst").children("p:par"))
        for (xml_node grp : click.child("p:cTn").child("p:childTnLst").children("p:par"))
            for (xml_node eff : grp.child("p:cTn").child("p:childTnLst").children("p:par")) {
                xml_node ctn = eff.child("p:cTn");
                QString cls = str(ctn.attribute("presetClass"));
                int preset = ctn.attribute("presetID").as_int();
                if (cls != "entr" && !(cls == "mediacall" && preset == 1)) continue;
                Build b;
                b.kind = cls == "entr" ? "entr" : "media";
                b.preset = preset;
                b.subtype = ctn.attribute("presetSubtype").as_int();
                QString node = str(ctn.attribute("nodeType"));
                b.trigger = node == "withEffect" ? "with" : node == "afterEffect" ? "after" : "click";
                b.delay = ctn.child("p:stCondLst").child("p:cond").attribute("delay").as_int();
                xml_node target = ctn.find_node([](xml_node n) { return is(n, "p:spTgt"); });
                b.element = target.attribute("spid").as_int();
                if (xml_node range = target.child("p:txEl").child("p:pRg")) b.paragraph = range.attribute("st").as_int();
                int ms = 0;
                std::function<void(xml_node)> longest = [&](xml_node n) {
                    for (xml_node c : n.children()) {
                        if (is(c, "p:cTn") && c != ctn) ms = std::max(ms, c.attribute("dur").as_int());
                        longest(c);
                    }
                };
                longest(ctn);
                b.ms = b.kind == "media" || b.preset == 1 ? 0 : ms; // "appear" is instantaneous
                out << b;
            }
    return out;
}

std::optional<Deck> load(const QString &path, QString *error)
{
    std::shared_ptr<Package> pkg = Package::openZip(path, error);
    if (!pkg) return std::nullopt;
    return Reader(pkg).read(error);
}

Deck blank()
{
    std::optional<Deck> deck = Reader(Package::openDir(":/template")).read(nullptr);
    Q_ASSERT(deck);
    return *deck;
}

} // namespace pptx
