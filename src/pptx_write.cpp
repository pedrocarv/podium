// Writes the model back to .pptx. Slides, notes and presentation.xml are regenerated; every other part
// (masters, layouts, themes, media, charts, …) is copied from the source package untouched. Each element
// starts from its source XML and only the modeled properties are rewritten, in schema order.

#include "pptx.h"
#include "xml.h"

#include <QFileInfo>
#include <QSet>
#include <QUuid>

#include <functional>

using pugi::xml_attribute;
using pugi::xml_node;

namespace pptx {
namespace {

const char *nsA = "http://schemas.openxmlformats.org/drawingml/2006/main";
const char *nsR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char *nsP = "http://schemas.openxmlformats.org/presentationml/2006/main";
const QString ctPml = "application/vnd.openxmlformats-officedocument.presentationml.";

using Order = std::initializer_list<const char *>;
const Order spOrder{"p:nvSpPr", "p:spPr", "p:style", "p:txBody", "p:extLst"};
const Order picOrder{"p:nvPicPr", "p:blipFill", "p:spPr", "p:style", "p:extLst"};
const Order spPrOrder{"a:xfrm", "a:custGeom", "a:prstGeom", "a:noFill", "a:solidFill", "a:gradFill", "a:blipFill", "a:pattFill",
                      "a:grpFill", "a:ln", "a:effectLst", "a:effectDag", "a:scene3d", "a:sp3d", "a:extLst"};
const Order lnOrder{"a:noFill", "a:solidFill", "a:gradFill", "a:pattFill", "a:prstDash", "a:custDash", "a:round", "a:bevel",
                    "a:miter", "a:headEnd", "a:tailEnd", "a:extLst"};
const Order bodyPrOrder{"a:prstTxWarp", "a:noAutofit", "a:normAutofit", "a:spAutoFit", "a:scene3d", "a:sp3d", "a:flatTx", "a:extLst"};
const Order sldOrder{"p:cSld", "p:clrMapOvr", "p:transition", "mc:AlternateContent", "p:timing", "p:extLst"};
const Order fills{"a:noFill", "a:solidFill", "a:gradFill", "a:blipFill", "a:pattFill", "a:grpFill"};

qint64 emu(double pt) { return qRound64(pt * emuPerPt); }

bool is(xml_node n, const char *name) { return qstrcmp(n.name(), name) == 0; }

void attr(xml_node n, const char *name, const QString &value)
{
    xml_attribute a = n.attribute(name);
    if (!a) a = n.append_attribute(name);
    a.set_value(utf8(value).c_str());
}

void attr(xml_node n, const char *name, qint64 value) { attr(n, name, QString::number(value)); }

void flagAttr(xml_node n, const char *name, bool on)
{
    if (on) attr(n, name, QString("1"));
    else n.remove_attribute(name);
}

// The child `name`, created at its schema position (per `order`) if missing.
xml_node child(xml_node parent, const char *name, Order order)
{
    if (xml_node c = parent.child(name)) return c;
    bool after = false;
    for (const char *o : order) {
        if (qstrcmp(o, name) == 0) {
            after = true;
            continue;
        }
        if (after)
            if (xml_node next = parent.child(o)) return parent.insert_child_before(name, next);
    }
    return parent.append_child(name);
}

// A fresh, empty child `name` replacing any existing one, at its schema position.
xml_node fresh(xml_node parent, const char *name, Order order)
{
    xml_node old = parent.child(name);
    xml_node n = old ? parent.insert_child_before(name, old) : child(parent, name, order);
    if (old) parent.remove_child(old);
    return n;
}

void removeAll(xml_node parent, Order names)
{
    for (const char *name : names)
        while (xml_node c = parent.child(name)) parent.remove_child(c);
}

QString hex(QColor c) { return c.name(QColor::HexRgb).mid(1).toUpper(); }

// Relationships of one part being written; identical targets share an id.
struct RelSet {
    QList<Rel> list;
    int next = 1;
    QString add(const QString &type, const QString &target, bool external = false)
    {
        for (const Rel &r : list)
            if (r.type == type && r.target == target && r.external == external) return r.id;
        Rel r{QString("rId%1").arg(next++), type, target, external};
        list << r;
        return r.id;
    }
    QString add(const Rel &r) { return add(r.type, r.target, r.external); }
};

// Points every r:* attribute under `n` at `rels`, translating from the ids in `old`.
void renumber(xml_node n, const QList<Rel> &old, RelSet &rels)
{
    for (xml_attribute a : n.attributes()) {
        if (!QByteArray(a.name()).startsWith("r:") || QString(a.value()).isEmpty()) continue;
        for (const Rel &r : old)
            if (r.id == str(a)) {
                a.set_value(utf8(rels.add(r)).c_str());
                break;
            }
    }
    for (xml_node c : n.children()) renumber(c, old, rels);
}

void color(xml_node parent, QColor c)
{
    xml_node clr = parent.append_child("a:srgbClr");
    attr(clr, "val", hex(c));
    if (c.alpha() < 255) attr(clr.append_child("a:alpha"), "val", qRound64(c.alphaF() * 100000));
}

class Writer {
public:
    Writer(const Deck &deck) : deck(deck), pkg(*deck.package) {}
    bool write(const QString &path, QString *error);

private:
    QByteArray slide(const Slide &s, int index, RelSet &rels);
    QByteArray notes(const QString &text, const QString &slidePart, RelSet &rels);
    void element(xml_node parent, const Element &e, RelSet &rels);
    void shape(xml_node n, const Element &e, const ShapeEl &s, RelSet &rels);
    void picture(xml_node n, const Element &e, const PictureEl &p, RelSet &rels);
    void table(xml_node n, const Element &e, const TableEl &t, RelSet &rels);
    void group(xml_node n, const Element &e, const GroupEl &g, RelSet &rels);
    void nonVisual(xml_node nv, const Element &e, const char *kind);
    void xfrm(xml_node x, const Element &e);
    void fill(xml_node parent, const Fill &f, RelSet &rels, Order order);
    void line(xml_node ln, const Line &l, RelSet &rels);
    void text(xml_node txBody, const TextBody &b, RelSet &rels, bool cell = false);
    void paragraphs(xml_node txBody, const TextBody &b, RelSet &rels);
    void paraProps(xml_node pPr, const ParaStyle &s);
    void runProps(xml_node rPr, const CharStyle &c, RelSet &rels);
    void timing(xml_node sld, const Slide &s);
    void transition(xml_node sld, const Transition &t);

    const Deck &deck;
    const Package &pkg;
    QString presentationPart, notesMaster;
};

void Writer::xfrm(xml_node x, const Element &e)
{
    qint64 rot = qRound64(std::fmod(e.rotation + 360, 360) * 60000);
    if (rot) attr(x, "rot", rot);
    else x.remove_attribute("rot");
    flagAttr(x, "flipH", e.flipH);
    flagAttr(x, "flipV", e.flipV);
    xml_node off = child(x, "a:off", {"a:off", "a:ext", "a:chOff", "a:chExt"});
    attr(off, "x", emu(e.rect.x()));
    attr(off, "y", emu(e.rect.y()));
    xml_node ext = child(x, "a:ext", {"a:off", "a:ext", "a:chOff", "a:chExt"});
    attr(ext, "cx", emu(e.rect.width()));
    attr(ext, "cy", emu(e.rect.height()));
}

void Writer::nonVisual(xml_node nv, const Element &e, const char *kind)
{
    xml_node c = child(nv, "p:cNvPr", {"p:cNvPr", kind, "p:nvPr"});
    attr(c, "id", e.id);
    attr(c, "name", e.name);
    flagAttr(c, "hidden", e.hidden);
    child(nv, kind, {"p:cNvPr", kind, "p:nvPr"});
    xml_node nvPr = child(nv, "p:nvPr", {"p:cNvPr", kind, "p:nvPr"});
    if (e.ph && !nvPr.child("p:ph")) {
        xml_node ph = nvPr.prepend_child("p:ph");
        if (e.ph.type != "obj") attr(ph, "type", e.ph.type);
        if (e.ph.idx) attr(ph, "idx", e.ph.idx);
        if (qstrcmp(kind, "p:cNvSpPr") == 0) attr(child(nv.child(kind), "a:spLocks", {}), "noGrp", QString("1"));
    }
}

void Writer::fill(xml_node parent, const Fill &f, RelSet &rels, Order order)
{
    const char *tag = f.kind == Fill::Solid      ? "a:solidFill"
                      : f.kind == Fill::Gradient ? "a:gradFill"
                      : f.kind == Fill::Image    ? "a:blipFill"
                                                 : "a:noFill";
    xml_node old; // replace the existing fill in place
    for (xml_node c : parent.children())
        for (const char *name : fills)
            if (is(c, name)) old = c;
    xml_node n = old ? parent.insert_child_before(tag, old) : child(parent, tag, order);
    if (old) parent.remove_child(old);
    switch (f.kind) {
    case Fill::Solid:
        color(n, f.color);
        break;
    case Fill::Gradient: {
        attr(n, "rotWithShape", QString("1"));
        xml_node list = n.append_child("a:gsLst");
        for (const QGradientStop &s : f.stops) {
            xml_node gs = list.append_child("a:gs");
            attr(gs, "pos", qRound64(s.first * 100000));
            color(gs, s.second);
        }
        xml_node lin = n.append_child("a:lin");
        attr(lin, "ang", qRound64(f.angle * 60000));
        attr(lin, "scaled", QString("0"));
        break;
    }
    case Fill::Image:
        attr(n.append_child("a:blip"), "r:embed", rels.add(rel::image, f.image));
        n.append_child("a:stretch").append_child("a:fillRect");
        break;
    case Fill::None:
        break;
    }
}

void Writer::line(xml_node ln, const Line &l, RelSet &rels)
{
    attr(ln, "w", emu(l.width));
    fill(ln, l.fill, rels, lnOrder);
    attr(fresh(ln, "a:prstDash", lnOrder), "val", l.dash);
    auto end = [&](const char *tag, const LineEnd &e) {
        if (e.type == "none") return removeAll(ln, {tag});
        xml_node n = fresh(ln, tag, lnOrder);
        attr(n, "type", e.type);
        if (e.w != "med") attr(n, "w", e.w);
        if (e.len != "med") attr(n, "len", e.len);
    };
    end("a:headEnd", l.head);
    end("a:tailEnd", l.tail);
}

void Writer::runProps(xml_node rPr, const CharStyle &c, RelSet &rels)
{
    attr(rPr, "sz", qRound64(c.size * 100));
    attr(rPr, "b", QString(c.bold ? "1" : "0"));
    attr(rPr, "i", QString(c.italic ? "1" : "0"));
    attr(rPr, "u", QString(c.underline ? "sng" : "none"));
    attr(rPr, "strike", QString(c.strike ? "sngStrike" : "noStrike"));
    if (c.baseline) attr(rPr, "baseline", qint64(c.baseline) * 1000);
    attr(rPr, "dirty", QString("0"));
    if (c.color.alpha() == 0) rPr.append_child("a:noFill");
    else color(rPr.append_child("a:solidFill"), c.color);
    attr(rPr.append_child("a:latin"), "typeface", c.font);
    attr(rPr.append_child("a:cs"), "typeface", c.font);
    if (!c.link.isEmpty()) {
        xml_node h = rPr.append_child("a:hlinkClick");
        bool slideJump = c.link.startsWith("ppt/");
        attr(h, "r:id", rels.add(slideJump ? rel::slide : rel::hyperlink, c.link, !slideJump));
        if (slideJump) attr(h, "action", QString("ppaction://hlinksldjump"));
    }
}

void Writer::text(xml_node txBody, const TextBody &b, RelSet &rels, bool cell)
{
    xml_node bodyPr = child(txBody, "a:bodyPr", {"a:bodyPr", "a:lstStyle", "a:p"});
    if (cell) {
        child(txBody, "a:lstStyle", {"a:bodyPr", "a:lstStyle", "a:p"});
        return paragraphs(txBody, b, rels);
    }
    attr(bodyPr, "lIns", emu(b.insets.left()));
    attr(bodyPr, "tIns", emu(b.insets.top()));
    attr(bodyPr, "rIns", emu(b.insets.right()));
    attr(bodyPr, "bIns", emu(b.insets.bottom()));
    attr(bodyPr, "anchor", QString(b.anchor == TextBody::Middle ? "ctr" : b.anchor == TextBody::Bottom ? "b" : "t"));
    attr(bodyPr, "wrap", QString(b.wrap ? "square" : "none"));
    if (b.vert != "horz") attr(bodyPr, "vert", b.vert);
    else bodyPr.remove_attribute("vert");
    removeAll(bodyPr, {"a:noAutofit", "a:normAutofit", "a:spAutoFit"});
    if (b.autofit == TextBody::Shrink) {
        xml_node fit = child(bodyPr, "a:normAutofit", bodyPrOrder);
        if (b.fontScale < 1) attr(fit, "fontScale", qRound64(b.fontScale * 100000));
        if (b.lineReduction > 0) attr(fit, "lnSpcReduction", qRound64(b.lineReduction * 100000));
    } else {
        child(bodyPr, b.autofit == TextBody::Resize ? "a:spAutoFit" : "a:noAutofit", bodyPrOrder);
    }
    paragraphs(txBody, b, rels);
}

void Writer::paraProps(xml_node pPr, const ParaStyle &s)
{
    attr(pPr, "marL", emu(s.marL));
    attr(pPr, "indent", emu(s.indent));
    if (s.level) attr(pPr, "lvl", qint64(s.level));
    attr(pPr, "algn", QString(s.align & Qt::AlignHCenter ? "ctr" : s.align & Qt::AlignRight ? "r" : s.align & Qt::AlignJustify ? "just" : "l"));
    auto spacing = [&](const char *tag, const Spacing &sp) {
        xml_node n = pPr.append_child(tag);
        if (sp.percent) attr(n.append_child("a:spcPct"), "val", qRound64(sp.value * 100000));
        else attr(n.append_child("a:spcPts"), "val", qRound64(sp.value * 100));
    };
    spacing("a:lnSpc", s.lineSpacing);
    spacing("a:spcBef", s.spaceBefore);
    spacing("a:spcAft", s.spaceAfter);
    const Bullet &bu = s.bullet;
    if (bu.kind == Bullet::None) {
        pPr.append_child("a:buNone");
    } else {
        if (bu.color.isValid()) color(pPr.append_child("a:buClr"), bu.color);
        if (bu.scale != 1) attr(pPr.append_child("a:buSzPct"), "val", qRound64(bu.scale * 100000));
        if (!bu.font.isEmpty()) attr(pPr.append_child("a:buFont"), "typeface", bu.font);
        if (bu.kind == Bullet::Char) {
            attr(pPr.append_child("a:buChar"), "char", bu.chr);
        } else {
            xml_node n = pPr.append_child("a:buAutoNum");
            attr(n, "type", bu.scheme);
            if (bu.startAt != 1) attr(n, "startAt", qint64(bu.startAt));
        }
    }
}

void Writer::paragraphs(xml_node txBody, const TextBody &b, RelSet &rels)
{
    if (!txBody.child("a:lstStyle")) {
        // New text: record its per-level defaults, which PowerPoint uses for newly typed text.
        xml_node lst = child(txBody, "a:lstStyle", {"a:bodyPr", "a:lstStyle", "a:p"});
        for (int i = 0; i < 9; ++i) {
            xml_node lvl = lst.append_child(QByteArray("a:lvl" + QByteArray::number(i + 1) + "pPr").constData());
            paraProps(lvl, b.levels[i].para);
            lvl.remove_attribute("lvl");
            runProps(lvl.append_child("a:defRPr"), b.levels[i].chr, rels);
        }
    }
    removeAll(txBody, {"a:p"});
    QList<Paragraph> list = b.paragraphs;
    if (list.isEmpty()) list << Paragraph{b.levels[0].para, {}, b.levels[0].chr};
    for (const Paragraph &p : list) {
        xml_node ap = txBody.append_child("a:p");
        paraProps(ap.append_child("a:pPr"), p.style);
        for (const Run &r : p.runs) {
            if (!r.field.isEmpty()) {
                xml_node fld = ap.append_child("a:fld");
                attr(fld, "id", QUuid::createUuid().toString(QUuid::WithBraces).toUpper());
                attr(fld, "type", r.field);
                runProps(fld.append_child("a:rPr"), r.style, rels);
                fld.append_child("a:t").text().set(utf8(r.text).c_str());
                continue;
            }
            QStringList pieces = r.text.split(QChar::LineSeparator);
            for (int i = 0; i < pieces.size(); ++i) {
                if (i > 0) runProps(ap.append_child("a:br").append_child("a:rPr"), r.style, rels);
                if (pieces[i].isEmpty()) continue;
                xml_node run = ap.append_child("a:r");
                runProps(run.append_child("a:rPr"), r.style, rels);
                run.append_child("a:t").text().set(utf8(pieces[i]).c_str());
            }
        }
        xml_node end = ap.append_child("a:endParaRPr");
        runProps(end, p.endStyle, rels);
        end.remove_child("a:hlinkClick");
    }
}

void Writer::shape(xml_node n, const Element &e, const ShapeEl &s, RelSet &rels)
{
    const char *nvName = s.connector ? "p:nvCxnSpPr" : "p:nvSpPr";
    nonVisual(child(n, nvName, spOrder), e, s.connector ? "p:cNvCxnSpPr" : "p:cNvSpPr");
    if (s.textBox && !s.connector) attr(n.child(nvName).child("p:cNvSpPr"), "txBox", QString("1"));
    xml_node spPr = child(n, "p:spPr", {nvName, "p:spPr", "p:style", "p:txBody", "p:extLst"});
    xfrm(child(spPr, "a:xfrm", spPrOrder), e);
    if (!s.preset.isEmpty()) {
        removeAll(spPr, {"a:custGeom"});
        xml_node g = fresh(spPr, "a:prstGeom", spPrOrder);
        attr(g, "prst", s.preset);
        xml_node av = g.append_child("a:avLst");
        for (auto [name, value] : s.adjust.asKeyValueRange()) {
            xml_node gd = av.append_child("a:gd");
            attr(gd, "name", name);
            attr(gd, "fmla", "val " + QString::number(qRound64(value)));
        }
    }
    if (!s.connector) fill(spPr, s.fill, rels, spPrOrder);
    line(child(spPr, "a:ln", spPrOrder), s.line, rels);
    if (s.connector) return;
    if (!s.text.paragraphs.isEmpty() || n.child("p:txBody"))
        text(child(n, "p:txBody", {nvName, "p:spPr", "p:style", "p:txBody", "p:extLst"}), s.text, rels);
}

void Writer::picture(xml_node n, const Element &e, const PictureEl &p, RelSet &rels)
{
    nonVisual(child(n, "p:nvPicPr", picOrder), e, "p:cNvPicPr");
    xml_node blipFill = child(n, "p:blipFill", picOrder);
    xml_node blip = child(blipFill, "a:blip", {"a:blip", "a:srcRect", "a:tile", "a:stretch"});
    attr(blip, "r:embed", rels.add(rel::image, p.image));
    if (p.crop.isNull()) {
        removeAll(blipFill, {"a:srcRect"});
    } else {
        xml_node r = fresh(blipFill, "a:srcRect", {"a:blip", "a:srcRect", "a:tile", "a:stretch"});
        attr(r, "l", qRound64(p.crop.left() * 100000));
        attr(r, "t", qRound64(p.crop.top() * 100000));
        attr(r, "r", qRound64(p.crop.right() * 100000));
        attr(r, "b", qRound64(p.crop.bottom() * 100000));
    }
    if (!blipFill.child("a:stretch") && !blipFill.child("a:tile"))
        child(blipFill, "a:stretch", {"a:blip", "a:srcRect", "a:tile", "a:stretch"}).append_child("a:fillRect");
    xml_node spPr = child(n, "p:spPr", picOrder);
    xfrm(child(spPr, "a:xfrm", spPrOrder), e);
    if (!spPr.child("a:custGeom")) {
        xml_node g = fresh(spPr, "a:prstGeom", spPrOrder);
        attr(g, "prst", p.preset);
        g.append_child("a:avLst");
    }
    if (p.line.fill.kind != Fill::None || spPr.child("a:ln")) line(child(spPr, "a:ln", spPrOrder), p.line, rels);
}

void Writer::table(xml_node n, const Element &e, const TableEl &t, RelSet &rels)
{
    const Order order{"p:nvGraphicFramePr", "p:xfrm", "a:graphic", "p:extLst"};
    nonVisual(child(n, "p:nvGraphicFramePr", order), e, "p:cNvGraphicFramePr");
    xml_node x = fresh(n, "p:xfrm", order);
    attr(x.append_child("a:off"), "x", emu(e.rect.x()));
    attr(x.child("a:off"), "y", emu(e.rect.y()));
    attr(x.append_child("a:ext"), "cx", emu(e.rect.width()));
    attr(x.child("a:ext"), "cy", emu(e.rect.height()));
    xml_node data = child(child(n, "a:graphic", order), "a:graphicData", {});
    attr(data, "uri", QString("http://schemas.openxmlformats.org/drawingml/2006/table"));
    xml_node tbl = child(data, "a:tbl", {});
    if (!tbl.child("a:tblPr")) {
        xml_node pr = tbl.prepend_child("a:tblPr");
        attr(pr, "firstRow", QString("1"));
        attr(pr, "bandRow", QString("1"));
    }
    xml_node grid = fresh(tbl, "a:tblGrid", {"a:tblPr", "a:tblGrid", "a:tr", "a:extLst"});
    for (double w : t.cols) attr(grid.append_child("a:gridCol"), "w", emu(w));
    removeAll(tbl, {"a:tr"});

    QList<QList<QPoint>> cover(t.cells.size()); // per covered cell: (dx, dy) from its anchor
    for (int r = 0; r < t.cells.size(); ++r) cover[r].fill(QPoint(0, 0), t.cells[r].size());
    for (int r = 0; r < t.cells.size(); ++r)
        for (int c = 0; c < t.cells[r].size(); ++c) {
            const Cell &cell = t.cells[r][c];
            if (cell.merged) continue;
            for (int dr = 0; dr < cell.rowSpan; ++dr)
                for (int dc = 0; dc < cell.colSpan; ++dc)
                    if ((dr || dc) && r + dr < cover.size() && c + dc < cover[r + dr].size()) cover[r + dr][c + dc] = QPoint(dc, dr);
        }
    for (int r = 0; r < t.cells.size(); ++r) {
        xml_node tr = tbl.append_child("a:tr");
        attr(tr, "h", emu(t.rows.value(r)));
        for (int c = 0; c < t.cells[r].size(); ++c) {
            const Cell &cell = t.cells[r][c];
            xml_node tc = tr.append_child("a:tc");
            if (cell.colSpan > 1) attr(tc, "gridSpan", qint64(cell.colSpan));
            if (cell.rowSpan > 1) attr(tc, "rowSpan", qint64(cell.rowSpan));
            if (cell.merged && cover[r][c].x()) attr(tc, "hMerge", QString("1"));
            if (cell.merged && cover[r][c].y()) attr(tc, "vMerge", QString("1"));
            text(tc.append_child("a:txBody"), cell.text, rels, true);
            xml_node pr = tc.append_child("a:tcPr");
            attr(pr, "marL", emu(cell.text.insets.left()));
            attr(pr, "marR", emu(cell.text.insets.right()));
            attr(pr, "marT", emu(cell.text.insets.top()));
            attr(pr, "marB", emu(cell.text.insets.bottom()));
            attr(pr, "anchor", QString(cell.text.anchor == TextBody::Middle ? "ctr" : cell.text.anchor == TextBody::Bottom ? "b" : "t"));
            for (auto [tag, l] : {std::pair{"a:lnL", &cell.left}, {"a:lnR", &cell.right}, {"a:lnT", &cell.top}, {"a:lnB", &cell.bottom}})
                if (l->fill.kind != Fill::None) line(pr.append_child(tag), *l, rels);
            if (cell.fill.kind != Fill::None) fill(pr, cell.fill, rels, {"a:lnL", "a:lnR", "a:lnT", "a:lnB", "a:lnTlToBr", "a:lnBlToTr", "a:cell3D", "a:noFill", "a:solidFill", "a:gradFill", "a:blipFill", "a:pattFill", "a:grpFill", "a:headers", "a:extLst"});
        }
    }
}

void Writer::group(xml_node n, const Element &e, const GroupEl &g, RelSet &rels)
{
    const Order order{"p:nvGrpSpPr", "p:grpSpPr"};
    nonVisual(child(n, "p:nvGrpSpPr", order), e, "p:cNvGrpSpPr");
    xml_node x = fresh(child(n, "p:grpSpPr", order), "a:xfrm", spPrOrder);
    xfrm(x, e);
    attr(x.append_child("a:chOff"), "x", emu(g.childRect.x()));
    attr(x.child("a:chOff"), "y", emu(g.childRect.y()));
    attr(x.append_child("a:chExt"), "cx", emu(g.childRect.width()));
    attr(x.child("a:chExt"), "cy", emu(g.childRect.height()));
    for (xml_node c = n.first_child(); c;) {
        xml_node next = c.next_sibling();
        if (!is(c, "p:nvGrpSpPr") && !is(c, "p:grpSpPr") && !is(c, "p:extLst")) n.remove_child(c);
        c = next;
    }
    for (const Element &c : g.children) element(n, c, rels);
    if (xml_node ext = n.child("p:extLst")) n.append_move(ext);
}

void Writer::element(xml_node parent, const Element &e, RelSet &rels)
{
    pugi::xml_document src;
    xml_node n;
    if (!e.source.isEmpty() && loadXml(src, e.source)) {
        n = parent.append_copy(src.document_element());
        renumber(n, e.rels, rels);
        if (e.as<RawEl>()) return;
    } else {
        const char *tag = e.as<PictureEl>() ? "p:pic" : e.as<TableEl>() ? "p:graphicFrame" : e.as<GroupEl>() ? "p:grpSp"
                          : e.as<ShapeEl>() && e.as<ShapeEl>()->connector ? "p:cxnSp" : "p:sp";
        if (e.as<RawEl>()) return; // nothing to write without source
        n = parent.append_child(tag);
    }
    if (auto s = e.as<ShapeEl>()) shape(n, e, *s, rels);
    else if (auto p = e.as<PictureEl>()) picture(n, e, *p, rels);
    else if (auto t = e.as<TableEl>()) table(n, e, *t, rels);
    else if (auto g = e.as<GroupEl>()) group(n, e, *g, rels);
}

void Writer::transition(xml_node sld, const Transition &t)
{
    for (xml_node c = sld.first_child(); c;) {
        xml_node next = c.next_sibling();
        if (is(c, "p:transition") || (is(c, "mc:AlternateContent") && c.find_node([](xml_node x) { return is(x, "p:transition"); })))
            sld.remove_child(c);
        c = next;
    }
    if (t.type.isEmpty() && t.after < 0 && t.onClick) return;
    auto setup = [&](xml_node tr, bool p14) {
        attr(tr, "spd", QString(t.ms <= 500 ? "fast" : t.ms <= 750 ? "med" : "slow"));
        if (p14) attr(tr, "p14:dur", qint64(t.ms));
        if (!t.onClick) attr(tr, "advClick", QString("0"));
        if (t.after >= 0) attr(tr, "advTm", qint64(t.after));
        if (!t.type.isEmpty()) {
            xml_node k = tr.append_child(utf8("p:" + t.type).c_str());
            if (!t.dir.isEmpty() && t.type != "fade" && t.type != "cut") attr(k, "dir", t.dir);
        }
    };
    xml_node ac = child(sld, "mc:AlternateContent", sldOrder);
    attr(ac, "xmlns:mc", QString("http://schemas.openxmlformats.org/markup-compatibility/2006"));
    xml_node choice = ac.append_child("mc:Choice");
    attr(choice, "xmlns:p14", QString("http://schemas.microsoft.com/office/powerpoint/2010/main"));
    attr(choice, "Requires", QString("p14"));
    setup(choice.append_child("p:transition"), true);
    setup(ac.append_child("mc:Fallback").append_child("p:transition"), false);
}

void Writer::timing(xml_node sld, const Slide &s)
{
    QSet<int> ids;
    std::function<void(const Element &)> collect = [&](const Element &e) {
        ids.insert(e.id);
        if (auto g = e.as<GroupEl>())
            for (const Element &c : g->children) collect(c);
    };
    for (const Element &e : s.elements) collect(e);

    xml_node old = sld.child("p:timing");
    bool referencesValid = true;
    std::function<void(xml_node)> check = [&](xml_node n) {
        if (is(n, "p:spTgt")) referencesValid &= ids.contains(n.attribute("spid").as_int());
        for (xml_node c : n.children()) check(c);
    };
    check(old);
    if (old && referencesValid && readBuilds(sld) == s.builds) return;
    if (old) sld.remove_child(old);

    QList<Build> builds;
    for (const Build &b : s.builds)
        if (ids.contains(b.element)) builds << b;
    if (builds.isEmpty()) return;

    xml_node timing = child(sld, "p:timing", sldOrder);
    int nextId = 1;
    auto ctn = [&](xml_node parent) {
        xml_node n = parent.append_child("p:cTn");
        attr(n, "id", qint64(nextId++));
        return n;
    };
    auto cond = [](xml_node ctnNode, const QString &delay) {
        attr(child(ctnNode, "p:stCondLst", {}).append_child("p:cond"), "delay", delay);
    };
    auto target = [](xml_node parent, const Build &b) {
        xml_node sp = parent.append_child("p:tgtEl").append_child("p:spTgt");
        attr(sp, "spid", qint64(b.element));
        if (b.paragraph >= 0) {
            xml_node rg = sp.append_child("p:txEl").append_child("p:pRg");
            attr(rg, "st", qint64(b.paragraph));
            attr(rg, "end", qint64(b.paragraph));
        }
    };
    xml_node root = ctn(timing.append_child("p:tnLst").append_child("p:par"));
    attr(root, "dur", QString("indefinite"));
    attr(root, "restart", QString("never"));
    attr(root, "nodeType", QString("tmRoot"));
    xml_node seq = root.append_child("p:childTnLst").append_child("p:seq");
    attr(seq, "concurrent", QString("1"));
    attr(seq, "nextAc", QString("seek"));
    xml_node main = ctn(seq);
    attr(main, "dur", QString("indefinite"));
    attr(main, "nodeType", QString("mainSeq"));
    xml_node clicks = main.append_child("p:childTnLst");

    xml_node clickList, groupList;
    qint64 groupStart = 0, lastEnd = 0;
    for (int i = 0; i < builds.size(); ++i) {
        const Build &b = builds[i];
        if (i == 0 || b.trigger == "click") {
            xml_node c = ctn(clicks.append_child("p:par"));
            attr(c, "fill", QString("hold"));
            cond(c, "indefinite");
            if (i == 0 && b.trigger != "click") {
                xml_node begin = c.child("p:stCondLst").append_child("p:cond");
                attr(begin, "evt", QString("onBegin"));
                attr(begin, "delay", QString("0"));
                attr(begin.append_child("p:tn"), "val", qint64(2));
            }
            clickList = c.append_child("p:childTnLst");
            groupList = {};
            groupStart = lastEnd = 0;
        }
        if (!groupList || b.trigger == "after") {
            if (b.trigger == "after") groupStart = lastEnd;
            xml_node g = ctn(clickList.append_child("p:par"));
            attr(g, "fill", QString("hold"));
            cond(g, QString::number(groupStart));
            groupList = g.append_child("p:childTnLst");
        }
        xml_node eff = ctn(groupList.append_child("p:par"));
        attr(eff, "presetID", qint64(b.preset));
        attr(eff, "presetClass", QString(b.kind == "media" ? "mediacall" : "entr"));
        attr(eff, "presetSubtype", qint64(b.subtype));
        attr(eff, "fill", QString("hold"));
        if (b.kind == "entr") attr(eff, "grpId", QString("0"));
        attr(eff, "nodeType", QString(b.trigger == "with" ? "withEffect" : b.trigger == "after" ? "afterEffect" : "clickEffect"));
        cond(eff, QString::number(b.delay));
        xml_node list = eff.append_child("p:childTnLst");
        if (b.kind == "media") {
            xml_node cmd = list.append_child("p:cmd");
            attr(cmd, "type", QString("call"));
            attr(cmd, "cmd", QString("playFrom(0.0)"));
            xml_node bhvr = cmd.append_child("p:cBhvr");
            xml_node t = ctn(bhvr);
            attr(t, "dur", QString("1"));
            attr(t, "fill", QString("hold"));
            target(bhvr, b);
        } else {
            xml_node set = list.append_child("p:set");
            xml_node bhvr = set.append_child("p:cBhvr");
            xml_node t = ctn(bhvr);
            attr(t, "dur", QString("1"));
            attr(t, "fill", QString("hold"));
            cond(t, "0");
            target(bhvr, b);
            bhvr.append_child("p:attrNameLst").append_child("p:attrName").text().set("style.visibility");
            attr(set.append_child("p:to").append_child("p:strVal"), "val", QString("visible"));
            if (b.preset != 1) {
                xml_node anim = list.append_child("p:animEffect");
                attr(anim, "transition", QString("in"));
                attr(anim, "filter", QString(b.preset == 9 ? "dissolve" : "fade"));
                xml_node ab = anim.append_child("p:cBhvr");
                attr(ctn(ab), "dur", qint64(std::max(b.ms, 1)));
                target(ab, b);
            }
        }
        lastEnd = std::max(lastEnd, groupStart + b.delay + (b.kind == "media" || b.preset == 1 ? 0 : b.ms));
    }
    xml_node prev = seq.append_child("p:prevCondLst").append_child("p:cond");
    attr(prev, "evt", QString("onPrev"));
    attr(prev, "delay", QString("0"));
    prev.append_child("p:tgtEl").append_child("p:sldTgt");
    xml_node next = seq.append_child("p:nextCondLst").append_child("p:cond");
    attr(next, "evt", QString("onNext"));
    attr(next, "delay", QString("0"));
    next.append_child("p:tgtEl").append_child("p:sldTgt");

    xml_node childList = root.child("p:childTnLst");
    for (const Element &e : s.elements) {
        auto pic = e.as<PictureEl>();
        if (!pic || pic->video.isEmpty()) continue;
        xml_node node = childList.append_child("p:video").append_child("p:cMediaNode");
        attr(node, "vol", qint64(80000));
        xml_node t = ctn(node);
        attr(t, "fill", QString("hold"));
        attr(t, "display", QString("0"));
        cond(t, "indefinite");
        attr(node.append_child("p:tgtEl").append_child("p:spTgt"), "spid", qint64(e.id));
    }

    QSet<int> listed;
    xml_node bldLst;
    for (const Build &b : builds) {
        int idx = s.indexOf(b.element);
        if (b.kind != "entr" || listed.contains(b.element) || idx < 0 || !s.elements[idx].as<ShapeEl>() ||
            s.elements[idx].as<ShapeEl>()->text.isEmpty())
            continue;
        listed.insert(b.element);
        if (!bldLst) bldLst = timing.append_child("p:bldLst");
        xml_node p = bldLst.append_child("p:bldP");
        attr(p, "spid", qint64(b.element));
        attr(p, "grpId", QString("0"));
        if (b.paragraph >= 0) attr(p, "build", QString("p"));
    }
}

QByteArray Writer::slide(const Slide &s, int, RelSet &rels)
{
    pugi::xml_document doc;
    if (s.source.isEmpty() || !loadXml(doc, s.source)) {
        xml_node root = doc.append_child("p:sld");
        attr(root, "xmlns:a", QString(nsA));
        attr(root, "xmlns:r", QString(nsR));
        attr(root, "xmlns:p", QString(nsP));
        root.append_child("p:cSld").append_child("p:spTree");
        root.append_child("p:clrMapOvr").append_child("a:masterClrMapping");
    }
    xml_node sld = doc.document_element();
    xml_node cSld = child(sld, "p:cSld", sldOrder);
    xml_node tree = child(cSld, "p:spTree", {"p:bg", "p:spTree", "p:custDataLst", "p:controls", "p:extLst"});
    tree.parent().remove_child(tree);
    renumber(sld, s.rels, rels); // slide-level references (tags, controls, …), before the tree is rebuilt
    tree = child(cSld, "p:spTree", {"p:bg", "p:spTree", "p:custDataLst", "p:controls", "p:extLst"});

    removeAll(cSld, {"p:bg"});
    if (s.layout >= deck.layouts.size() || s.background != deck.layouts[s.layout].background) {
        xml_node pr = cSld.prepend_child("p:bg").append_child("p:bgPr");
        fill(pr, s.background, rels, {});
        pr.append_child("a:effectLst");
    }
    xml_node nv = tree.append_child("p:nvGrpSpPr");
    attr(nv.append_child("p:cNvPr"), "id", qint64(1));
    attr(nv.child("p:cNvPr"), "name", QString(""));
    nv.append_child("p:cNvGrpSpPr");
    nv.append_child("p:nvPr");
    xml_node x = tree.append_child("p:grpSpPr").append_child("a:xfrm");
    for (auto [tag, a, b] : {std::tuple{"a:off", "x", "y"}, {"a:ext", "cx", "cy"}, {"a:chOff", "x", "y"}, {"a:chExt", "cx", "cy"}}) {
        xml_node n = x.append_child(tag);
        attr(n, a, qint64(0));
        attr(n, b, qint64(0));
    }
    for (const Element &e : s.elements) element(tree, e, rels);

    if (s.hidden) attr(sld, "show", QString("0"));
    else sld.remove_attribute("show");
    if (!s.showMaster) attr(sld, "showMasterSp", QString("0"));
    else sld.remove_attribute("showMasterSp");
    if (!(readTransition(sld) == s.transition)) transition(sld, s.transition);
    timing(sld, s);

    rels.list.removeIf([](const Rel &r) { return r.type == rel::slideLayout || r.type == rel::notesSlide; });
    rels.add(rel::slideLayout, deck.layouts.value(s.layout).part);
    // Drop relationships nothing references anymore (e.g. a replaced image).
    QSet<QString> used;
    std::function<void(xml_node)> scan = [&](xml_node n) {
        for (xml_attribute a : n.attributes())
            if (QByteArray(a.name()).startsWith("r:")) used.insert(str(a));
        for (xml_node c : n.children()) scan(c);
    };
    scan(sld);
    rels.list.removeIf([&](const Rel &r) { return r.type != rel::slideLayout && !used.contains(r.id); });
    return serialize(doc);
}

QByteArray Writer::notes(const QString &text, const QString &slidePart, RelSet &rels)
{
    pugi::xml_document doc;
    xml_node root = doc.append_child("p:notes");
    attr(root, "xmlns:a", QString(nsA));
    attr(root, "xmlns:r", QString(nsR));
    attr(root, "xmlns:p", QString(nsP));
    xml_node tree = root.append_child("p:cSld").append_child("p:spTree");
    xml_node nv = tree.append_child("p:nvGrpSpPr");
    attr(nv.append_child("p:cNvPr"), "id", qint64(1));
    attr(nv.child("p:cNvPr"), "name", QString(""));
    nv.append_child("p:cNvGrpSpPr");
    nv.append_child("p:nvPr");
    tree.append_child("p:grpSpPr");
    auto placeholder = [&](int id, const QString &name, const QString &type, int idx) {
        xml_node sp = tree.append_child("p:sp");
        xml_node nvSp = sp.append_child("p:nvSpPr");
        xml_node c = nvSp.append_child("p:cNvPr");
        attr(c, "id", qint64(id));
        attr(c, "name", name);
        attr(nvSp.append_child("p:cNvSpPr").append_child("a:spLocks"), "noGrp", QString("1"));
        xml_node ph = nvSp.append_child("p:nvPr").append_child("p:ph");
        attr(ph, "type", type);
        if (idx) attr(ph, "idx", qint64(idx));
        sp.append_child("p:spPr");
        return sp;
    };
    placeholder(2, "Slide Image Placeholder 1", "sldImg", 0);
    xml_node body = placeholder(3, "Notes Placeholder 2", "body", 1).append_child("p:txBody");
    body.append_child("a:bodyPr");
    body.append_child("a:lstStyle");
    for (const QString &line : text.split('\n')) {
        xml_node p = body.append_child("a:p");
        if (!line.isEmpty()) {
            xml_node r = p.append_child("a:r");
            attr(r.append_child("a:rPr"), "lang", QString("en-US"));
            r.append_child("a:t").text().set(utf8(line).c_str());
        }
    }
    root.append_child("p:clrMapOvr").append_child("a:masterClrMapping");
    rels.add(rel::notesMaster, notesMaster);
    rels.add(rel::slide, slidePart);
    return serialize(doc);
}

bool Writer::write(const QString &path, QString *error)
{
    for (const Rel &r : pkg.rels(""))
        if (r.type == rel::officeDocument) presentationPart = r.target;
    pugi::xml_document pres;
    if (!loadXml(pres, pkg.read(presentationPart))) {
        if (error) *error = QObject::tr("The source presentation is unreadable.");
        return false;
    }
    QList<Rel> presRels = pkg.rels(presentationPart);
    for (const Rel &r : presRels)
        if (r.type == rel::notesMaster) notesMaster = r.target;

    QList<std::pair<QString, QByteArray>> written;
    QHash<QString, QList<Rel>> writtenRels; // rels of regenerated parts, for reachability
    QHash<QString, QString> types;          // content types of regenerated parts
    auto emitPart = [&](const QString &name, const QByteArray &xml, const QList<Rel> &rels, const QString &type) {
        written << std::pair{name, xml};
        if (!rels.isEmpty()) written << std::pair{Package::relsName(name), Package::relsXml(rels, name)};
        writtenRels.insert(name, rels);
        types.insert(name, type);
    };

    // A notes master is required for notes; borrow the template's when the source has none.
    bool anyNotes = std::any_of(deck.slides.begin(), deck.slides.end(), [](const Slide &s) { return !s.notes.isEmpty(); });
    std::shared_ptr<Package> templ;
    if (anyNotes && notesMaster.isEmpty()) {
        templ = Package::openDir(":/template");
        notesMaster = "ppt/notesMasters/notesMaster1.xml";
        QString theme = "ppt/theme/theme-notes.xml";
        emitPart(theme, templ->read("ppt/theme/theme2.xml"), {}, "application/vnd.openxmlformats-officedocument.theme+xml");
        emitPart(notesMaster, templ->read(notesMaster), {Rel{"rId1", rel::theme, theme, false}}, ctPml + "notesMaster+xml");
    }

    // Slides and notes.
    QList<Rel> keptPresRels;
    for (const Rel &r : presRels)
        if (r.type != rel::slide && !(r.type == rel::notesMaster && templ)) keptPresRels << r;
    int nextRel = 1;
    for (const Rel &r : keptPresRels) nextRel = std::max(nextRel, r.id.mid(3).toInt() + 1);
    xml_node root = pres.document_element();
    xml_node idList = root.child("p:sldIdLst");
    if (!idList) idList = root.insert_child_after("p:sldIdLst", root.child("p:notesMasterIdLst") ? root.child("p:notesMasterIdLst") : root.child("p:sldMasterIdLst"));
    removeAll(idList, {"p:sldId"});
    removeAll(root, {"p:custShowLst"});
    for (int i = 0; i < deck.slides.size(); ++i) {
        const Slide &s = deck.slides[i];
        QString name = QString("ppt/slides/slide%1.xml").arg(i + 1);
        RelSet rels;
        QByteArray xml = slide(s, i, rels);
        if (!s.notes.isEmpty()) {
            QString notesName = QString("ppt/notesSlides/notesSlide%1.xml").arg(i + 1);
            RelSet notesRels;
            emitPart(notesName, notes(s.notes, name, notesRels), notesRels.list, ctPml + "notesSlide+xml");
            rels.add(rel::notesSlide, notesName);
        }
        emitPart(name, xml, rels.list, ctPml + "slide+xml");
        Rel r{QString("rId%1").arg(nextRel++), rel::slide, name, false};
        keptPresRels << r;
        xml_node id = idList.append_child("p:sldId");
        attr(id, "id", qint64(256 + i));
        attr(id, "r:id", r.id);
    }
    if (templ) {
        Rel r{QString("rId%1").arg(nextRel++), rel::notesMaster, notesMaster, false};
        keptPresRels << r;
        removeAll(root, {"p:notesMasterIdLst"});
        attr(root.insert_child_after("p:notesMasterIdLst", root.child("p:sldMasterIdLst")).append_child("p:notesMasterId"), "r:id", r.id);
    }
    QString presType = pkg.contentType(presentationPart);
    written.prepend(std::pair{presentationPart, serialize(pres)});
    written.insert(1, std::pair{Package::relsName(presentationPart), Package::relsXml(keptPresRels, presentationPart)});
    writtenRels.insert(presentationPart, keptPresRels);
    types.insert(presentationPart, presType.isEmpty() ? ctPml + "presentation.main+xml" : presType);

    // Everything reachable from the root that we didn't regenerate is copied as-is.
    QSet<QString> reached{presentationPart};
    QStringList queue{presentationPart}, copied{"_rels/.rels"};
    for (const Rel &r : pkg.rels(""))
        if (!r.external && !reached.contains(r.target)) reached.insert(r.target), queue << r.target;
    while (!queue.isEmpty()) {
        QString part = queue.takeFirst();
        bool generated = writtenRels.contains(part);
        if (!generated && pkg.contains(part)) {
            copied << part;
            if (pkg.contains(Package::relsName(part))) copied << Package::relsName(part);
        }
        for (const Rel &r : generated ? writtenRels[part] : pkg.rels(part))
            if (!r.external && !reached.contains(r.target)) reached.insert(r.target), queue << r.target;
    }

    // Content types.
    static const QHash<QString, QString> known{
        {"rels", "application/vnd.openxmlformats-package.relationships+xml"}, {"xml", "application/xml"},
        {"png", "image/png"}, {"jpeg", "image/jpeg"}, {"jpg", "image/jpeg"}, {"gif", "image/gif"}, {"bmp", "image/bmp"},
        {"tif", "image/tiff"}, {"tiff", "image/tiff"}, {"svg", "image/svg+xml"}, {"emf", "image/x-emf"},
        {"wmf", "image/x-wmf"}, {"wdp", "image/vnd.ms-photo"}, {"webp", "image/webp"}, {"mp4", "video/mp4"},
        {"m4v", "video/mp4"}, {"mov", "video/quicktime"}, {"wmv", "video/x-ms-wmv"}, {"avi", "video/x-msvideo"},
        {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"wav", "audio/wav"},
        {"bin", "application/vnd.openxmlformats-officedocument.oleObject"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}, {"fntdata", "application/x-fontdata"}};
    pugi::xml_document ct;
    xml_node typesNode = ct.append_child("Types");
    attr(typesNode, "xmlns", QString("http://schemas.openxmlformats.org/package/2006/content-types"));
    QStringList all = copied;
    for (const auto &[name, data] : written) all << name;
    QSet<QString> defaults;
    for (const QString &part : all) {
        QString ext = QFileInfo(part).suffix().toLower();
        if (defaults.contains(ext)) continue;
        QString type = known.value(ext);
        if (type.isEmpty()) type = types.contains(part) ? types[part] : pkg.contentType(part);
        if (type.isEmpty()) type = "application/octet-stream";
        xml_node d = typesNode.append_child("Default");
        attr(d, "Extension", ext);
        attr(d, "ContentType", type);
        defaults.insert(ext);
    }
    QHash<QString, QString> defaultOf;
    for (xml_node d : typesNode.children("Default")) defaultOf.insert(str(d.attribute("Extension")), str(d.attribute("ContentType")));
    for (const QString &part : all) {
        QString type = types.contains(part) ? types[part] : pkg.contentType(part);
        if (type.isEmpty() || type == defaultOf.value(QFileInfo(part).suffix().toLower())) continue;
        xml_node o = typesNode.append_child("Override");
        attr(o, "PartName", "/" + part);
        attr(o, "ContentType", type);
    }
    written.prepend(std::pair{QString("[Content_Types].xml"), serialize(ct)});

    return pkg.write(path, written, copied, error);
}

} // namespace

bool save(const Deck &deck, const QString &path, QString *error)
{
    if (!deck.package) {
        if (error) *error = QObject::tr("Nothing to save.");
        return false;
    }
    return Writer(deck).write(path, error);
}

} // namespace pptx
