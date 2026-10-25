// Round-trip and package-integrity tests for the .pptx reader and writer.

#include "ops.h"
#include "pptx.h"
#include "render.h"

#include <QBuffer>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void strip(Element &e)
{
    e.source.clear();
    e.rels.clear();
    if (auto g = e.as<GroupEl>())
        for (Element &c : g->children) strip(c);
    if (auto r = e.as<RawEl>())
        for (Element &c : r->preview) strip(c);
}

// The model without the bookkeeping that legitimately changes on save (original XML, relationship ids).
Deck comparable(Deck d)
{
    for (Slide &s : d.slides) {
        s.source.clear();
        s.rels.clear();
        for (Element &e : s.elements) strip(e);
    }
    for (Layout &l : d.layouts) {
        for (Element &e : l.decor) strip(e);
        for (Element &e : l.placeholders) strip(e);
    }
    d.package.reset();
    return d;
}

QString describe(const Slide &a, const Slide &b)
{
    if (a.elements.size() != b.elements.size()) return QString("element count %1 vs %2").arg(a.elements.size()).arg(b.elements.size());
    for (int i = 0; i < a.elements.size(); ++i) {
        const Element &x = a.elements[i], &y = b.elements[i];
        if (x == y) continue;
        QString what = x.rect != y.rect                                                   ? "rect"
                       : x.rotation != y.rotation || x.flipH != y.flipH || x.flipV != y.flipV ? "transform"
                       : x.body.index() != y.body.index()                                 ? "kind"
                       : x.ph != y.ph                                                     ? "placeholder"
                                                                                          : "body";
        if (what == "body" && x.as<ShapeEl>() && y.as<ShapeEl>()) {
            const ShapeEl &p = *x.as<ShapeEl>(), &q = *y.as<ShapeEl>();
            if (p.fill != q.fill) what = "fill";
            else if (p.line != q.line) what = QString("line (%1 %2 %3 | %4 %5 %6)").arg(p.line.width).arg(p.line.fill.kind).arg(p.line.dash).arg(q.line.width).arg(q.line.fill.kind).arg(q.line.dash);
            else if (p.preset != q.preset || p.adjust != q.adjust || p.custom != q.custom) what = "geometry";
            else if (p.textBox != q.textBox || p.connector != q.connector) what = "shape kind";
            else if (p.text.insets != q.text.insets || p.text.anchor != q.text.anchor || p.text.wrap != q.text.wrap)
                what = "insets/anchor/wrap";
            else if (p.text.autofit != q.text.autofit || p.text.fontScale != q.text.fontScale || p.text.lineReduction != q.text.lineReduction)
                what = QString("autofit %1 %2 | %3 %4").arg(p.text.autofit).arg(p.text.fontScale).arg(q.text.autofit).arg(q.text.fontScale);
        }
        if (what == "body" && x.text() && y.text()) {
            const TextBody &t = *x.text(), &u = *y.text();
            what = t.paragraphs != u.paragraphs ? "paragraphs: " + t.text() + " | " + u.text()
                   : t.levels != u.levels       ? "levels"
                                                : "text box properties";
            for (int p = 0; p < std::min(t.paragraphs.size(), u.paragraphs.size()); ++p) {
                if (t.paragraphs[p].style != u.paragraphs[p].style) what += QString(" [para %1 style]").arg(p);
                if (t.paragraphs[p].endStyle != u.paragraphs[p].endStyle) what += QString(" [para %1 end style]").arg(p);
                for (int r = 0; r < std::min(t.paragraphs[p].runs.size(), u.paragraphs[p].runs.size()); ++r)
                    if (t.paragraphs[p].runs[r] != u.paragraphs[p].runs[r]) what += QString(" [para %1 run %2]").arg(p).arg(r);
            }
        }
        return QString("element %1 (%2): %3").arg(x.id).arg(x.name, what);
    }
    if (a.builds != b.builds) return "builds";
    if (a.transition != b.transition) return "transition";
    if (a.notes != b.notes) return "notes: " + a.notes + " | " + b.notes;
    if (a.background != b.background) return "background";
    return "slide properties";
}

void compareDecks(const Deck &original, const Deck &reloaded)
{
    Deck a = comparable(original), b = comparable(reloaded);
    QCOMPARE(b.size, a.size);
    QCOMPARE(b.slides.size(), a.slides.size());
    QCOMPARE(b.layouts.size(), a.layouts.size());
    for (int i = 0; i < a.slides.size(); ++i)
        QVERIFY2(a.slides[i] == b.slides[i], qPrintable(QString("slide %1: %2").arg(i + 1).arg(describe(a.slides[i], b.slides[i]))));
}

// Every relationship resolves, every part has a content type, slides are listed in order.
void checkPackage(const QString &path)
{
    QString error;
    std::shared_ptr<Package> pkg = Package::openZip(path, &error);
    QVERIFY2(pkg, qPrintable(error));
    QStringList parts = pkg->parts();
    for (const QString &part : parts) {
        if (part.endsWith(".rels") || part == "[Content_Types].xml") continue;
        QVERIFY2(!pkg->contentType(part).isEmpty(), qPrintable("no content type: " + part));
        for (const Rel &r : pkg->rels(part))
            QVERIFY2(r.external || pkg->contains(r.target), qPrintable(QString("%1 → %2 (%3) is missing").arg(part, r.target, r.type)));
    }
    for (const Rel &r : pkg->rels("")) QVERIFY2(r.external || pkg->contains(r.target), qPrintable(r.target));
}

QByteArray png(QColor color)
{
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return bytes;
}

} // namespace

class PptxTest : public QObject {
    Q_OBJECT

private slots:
    void blankTemplate()
    {
        Deck d = pptx::blank();
        QCOMPARE(d.slides.size(), 1);
        QCOMPARE(d.layouts.size(), 6);
        QCOMPARE(d.size, QSizeF(960, 540));
        QCOMPARE(d.slides[0].elements.size(), 2);
        QCOMPARE(d.slides[0].elements[0].ph.type, QString("ctrTitle"));
        QCOMPARE(d.slides[0].elements[0].text()->levels[0].chr.size, 60.0);
        QCOMPARE(d.layouts[1].name, QString("Title and Content"));
    }

    void roundTripEdited()
    {
        Deck d = pptx::blank();
        Slide &title = d.slides[0];
        title.elements[0].text()->paragraphs[0].runs << Run{"Hello, Podium", title.elements[0].text()->paragraphs[0].endStyle, {}};
        title.notes = "First line\nSecond line";

        Slide content = ops::slideFromLayout(d, 1);
        TextBody &body = *content.elements[1].text();
        body.paragraphs.clear();
        for (int i = 0; i < 3; ++i) {
            Paragraph p{body.levels[i].para, {Run{QString("Level %1").arg(i), body.levels[i].chr, {}}}, body.levels[i].chr};
            if (i == 1) p.runs << Run{QString(QChar::LineSeparator) + "bold", body.levels[i].chr, {}}, p.runs.last().style.bold = true;
            body.paragraphs << p;
        }
        int id = content.nextId();
        content.elements << ops::shape(d, id++, "roundRect", QRectF(100, 100, 200, 120));
        content.elements << ops::shape(d, id++, "straightConnector1", QRectF(300, 300, 200, 0));
        content.elements << ops::table(d, id++, 3, 2, QRectF(80, 260, 400, 90));
        content.elements.last().as<TableEl>()->cells[1][0].text.paragraphs[0].runs << Run{"cell", CharStyle{}, {}};
        std::optional<Element> pic = ops::picture(d, id++, png(Qt::red), "png", QPointF(700, 200));
        QVERIFY(pic);
        content.elements << *pic;
        content.builds << Build{content.elements[1].id, 0, "entr", 10, 0, "click", 500, 0}
                       << Build{content.elements[1].id, 1, "entr", 10, 0, "click", 500, 0}
                       << Build{pic->id, -1, "entr", 1, 0, "after", 0, 0};
        content.transition = Transition{"push", "l", 800, true, -1};
        content.hidden = true;
        d.slides << content;

        Slide grouped = ops::slideFromLayout(d, 5);
        int gid = grouped.nextId();
        GroupEl g;
        g.children.push_back(ops::shape(d, gid + 1, "ellipse", QRectF(100, 100, 50, 50)));
        g.children.push_back(ops::shape(d, gid + 2, "rect", QRectF(200, 150, 50, 50)));
        g.childRect = QRectF(100, 100, 150, 100);
        Element ge;
        ge.id = gid;
        ge.name = "Group";
        ge.rect = g.childRect;
        ge.body = g;
        grouped.elements << ge;
        grouped.background = Fill::solid(QColor(0x123456));
        d.slides << grouped;

        QTemporaryDir dir;
        QString path = dir.filePath("edited.pptx");
        QString error;
        QVERIFY2(pptx::save(d, path, &error), qPrintable(error));
        if (QString keep = qEnvironmentVariable("PODIUM_OUTPUT"); !keep.isEmpty()) QFile::copy(path, keep + "/edited.pptx");
        checkPackage(path);
        std::optional<Deck> back = pptx::load(path, &error);
        QVERIFY2(back, qPrintable(error));
        compareDecks(d, *back);

        // Saving the reloaded deck again is stable too.
        QString again = dir.filePath("again.pptx");
        QVERIFY2(pptx::save(*back, again, &error), qPrintable(error));
        checkPackage(again);
        std::optional<Deck> third = pptx::load(again, &error);
        QVERIFY(third);
        compareDecks(*back, *third);
    }

    void sampleDeck()
    {
        QString sample = qEnvironmentVariable("PODIUM_SAMPLE");
        if (sample.isEmpty()) QSKIP("set PODIUM_SAMPLE to a .pptx to round-trip it");
        QString error;
        std::optional<Deck> d = pptx::load(sample, &error);
        QVERIFY2(d, qPrintable(error));
        QTemporaryDir dir;
        QString path = dir.filePath("sample.pptx");
        QVERIFY2(pptx::save(*d, path, &error), qPrintable(error));
        checkPackage(path);
        std::optional<Deck> back = pptx::load(path, &error);
        QVERIFY2(back, qPrintable(error));
        compareDecks(*d, *back);
        std::shared_ptr<Package> a = d->package, b = back->package;
        for (const QString &part : a->parts())
            if (part.startsWith("ppt/media/") && b->contains(part)) QVERIFY2(a->read(part) == b->read(part), qPrintable(part));
        for (int i = 0; i < d->slides.size(); ++i)
            QVERIFY2(renderSlide(*d, i, QSize(480, 270)) == renderSlide(*back, i, QSize(480, 270)), qPrintable(QString("slide %1 renders differently").arg(i + 1)));
    }

    void pdfExport()
    {
        QTemporaryDir dir;
        QString path = dir.filePath("deck.pdf");
        QVERIFY(exportPdf(pptx::blank(), path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.read(5) == "%PDF-");
    }
};

QTEST_MAIN(PptxTest)
#include "tst_pptx.moc"
