// Drives the real editor offscreen: mouse and keyboard through QTest, checks the model.
// Set PODIUM_SNAPSHOTS to a directory to also save screenshots of the window.

#include "canvas.h"
#include "inspector.h"
#include "document.h"
#include "mainwindow.h"
#include "pptx.h"
#include "show.h"
#include "slidepanel.h"

#include <QDoubleSpinBox>
#include <QStandardPaths>
#include <QtTest>

namespace {

void snapshot(QWidget *w, const QString &name)
{
    QString dir = qEnvironmentVariable("PODIUM_SNAPSHOTS");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(QDir(dir).filePath(name + ".png"));
}

QAction *findAction(QWidget *w, const QString &text)
{
    for (QAction *a : w->findChildren<QAction *>())
        if (a->text().remove('&') == text) return a;
    return nullptr;
}

} // namespace

class UiTest : public QObject {
    Q_OBJECT

    MainWindow *window = nullptr;
    Canvas *canvas() { return window->findChild<Canvas *>(); }
    const Deck &deck() { return window->findChild<Document *>()->deck(); }
    QPoint at(QPointF scene) { return canvas()->mapFromScene(scene); }
    const Element &element(int index) { return deck().slides[canvas()->slideIndex()].elements[index]; }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        window = new MainWindow;
        window->newDeck();
        window->resize(1500, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }

    void cleanupTestCase() { delete window; }

    void typeIntoPlaceholder()
    {
        const Element &title = element(0);
        QVERIFY(title.ph);
        QTest::mouseClick(canvas()->viewport(), Qt::LeftButton, {}, at(title.rect.center()));
        QVERIFY(canvas()->isEditingText());
        QTest::keyClicks(canvas()->viewport(), "Quarterly Review");
        QTest::keyClick(canvas()->viewport(), Qt::Key_Escape);
        QVERIFY(!canvas()->isEditingText());
        QCOMPARE(element(0).text()->text(), QString("Quarterly Review"));
        QCOMPARE(element(0).text()->paragraphs[0].runs[0].style.size, 60.0);
        snapshot(window, "01-title");
    }

    void boldSelection()
    {
        canvas()->setSelection({element(0).id});
        findAction(window, "Bold")->trigger();
        QVERIFY(element(0).text()->paragraphs[0].runs[0].style.bold);
        findAction(window, "Undo")->trigger();
        QVERIFY(!element(0).text()->paragraphs[0].runs[0].style.bold);
    }

    void drawShapeMoveResize()
    {
        int before = int(deck().slides[0].elements.size());
        canvas()->beginInsert([this](QLineF l) {
            return ops::shape(deck(), deck().slides[0].nextId(), "ellipse", QRectF(l.p1(), l.p2()).normalized());
        });
        QTest::mousePress(canvas()->viewport(), Qt::LeftButton, {}, at({100, 100}));
        QTest::mouseMove(canvas()->viewport(), at({200, 160}));
        QTest::mouseRelease(canvas()->viewport(), Qt::LeftButton, {}, at({300, 200}));
        QCOMPARE(int(deck().slides[0].elements.size()), before + 1);
        const Element &e = deck().slides[0].elements.last();
        QVERIFY(std::abs(e.rect.width() - 200) < 3 && std::abs(e.rect.height() - 100) < 3);
        QCOMPARE(canvas()->selection(), QList<int>{e.id});

        QPointF start = e.rect.center();
        QTest::mousePress(canvas()->viewport(), Qt::LeftButton, {}, at(start));
        QTest::mouseMove(canvas()->viewport(), at(start + QPointF(20, 10)));
        QTest::mouseMove(canvas()->viewport(), at(start + QPointF(40, 20)));
        QTest::mouseRelease(canvas()->viewport(), Qt::LeftButton, {}, at(start + QPointF(40, 20)));
        QPointF moved = deck().slides[0].elements.last().rect.center();
        QVERIFY2(QLineF(moved, start + QPointF(40, 20)).length() < 8, "shape follows the drag (allowing snapping)");

        QRectF r = deck().slides[0].elements.last().rect;
        QTest::mousePress(canvas()->viewport(), Qt::LeftButton, {}, at(r.bottomRight()));
        QTest::mouseMove(canvas()->viewport(), at(r.bottomRight() + QPointF(30, 30)));
        QTest::mouseRelease(canvas()->viewport(), Qt::LeftButton, {}, at(r.bottomRight() + QPointF(30, 30)));
        QRectF resized = deck().slides[0].elements.last().rect;
        QVERIFY(resized.width() > r.width() + 20 && resized.height() > r.height() + 20);
        QCOMPARE(resized.topLeft(), r.topLeft());

        QTest::keyClick(canvas()->viewport(), Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(deck().slides[0].elements.last().rect.left(), resized.left() + 10);
        QTest::keyClick(canvas()->viewport(), Qt::Key_Delete);
        QCOMPARE(int(deck().slides[0].elements.size()), before);
        snapshot(window, "02-shapes");
    }

    void typeIntoShapeAndGroup()
    {
        Element a = ops::shape(deck(), deck().slides[0].nextId(), "rect", QRectF(600, 380, 120, 60));
        canvas()->insert(a);
        QTest::keyClicks(canvas()->viewport(), "Go");
        QVERIFY(canvas()->isEditingText());
        canvas()->stopEditing();
        QCOMPARE(deck().slides[0].elements.last().text()->text(), QString("Go"));
        Element b = ops::shape(deck(), deck().slides[0].nextId(), "ellipse", QRectF(760, 380, 80, 60));
        canvas()->insert(b);
        canvas()->setSelection({a.id, b.id});
        canvas()->group();
        QCOMPARE(canvas()->selection().size(), 1);
        QVERIFY(deck().slides[0].elements.last().as<GroupEl>());
        canvas()->ungroup();
        QCOMPARE(canvas()->selection().size(), 2);
        QCOMPARE(deck().slides[0].elements[deck().slides[0].indexOf(a.id)].rect, a.rect);
    }

    void clipboardAndInspector()
    {
        Element e = ops::shape(deck(), deck().slides[0].nextId(), "star5", QRectF(50, 50, 80, 80));
        canvas()->insert(e);
        int count = int(deck().slides[0].elements.size());
        canvas()->copy();
        canvas()->paste();
        QCOMPARE(int(deck().slides[0].elements.size()), count + 1);
        const Element &copy = deck().slides[0].elements.last();
        QVERIFY(copy.id != e.id);
        QCOMPARE(copy.rect, e.rect.translated(12, 12));
        QCOMPARE(canvas()->selection(), QList<int>{copy.id});

        QDoubleSpinBox *x = nullptr; // the Position group's first field
        for (QDoubleSpinBox *spin : window->findChild<Inspector *>()->findChildren<QDoubleSpinBox *>())
            if (!x && spin->suffix() == " pt") x = spin;
        QCOMPARE(x->value(), copy.rect.x());
        x->setValue(300);
        QCOMPARE(deck().slides[0].elements.last().rect.x(), 300.0);
    }

    void tableCells()
    {
        canvas()->insert(ops::table(deck(), deck().slides[0].nextId(), 2, 2, QRectF(100, 380, 300, 80)));
        int id = canvas()->selection().first();
        canvas()->beginEdit(id, 0, 0);
        QTest::keyClicks(canvas()->viewport(), "A1");
        QTest::keyClick(canvas()->viewport(), Qt::Key_Tab);
        QTest::keyClicks(canvas()->viewport(), "B1");
        canvas()->stopEditing();
        const TableEl &t = *deck().slides[0].elements.last().as<TableEl>();
        QCOMPARE(t.cells[0][0].text.text(), QString("A1"));
        QCOMPARE(t.cells[0][1].text.text(), QString("B1"));
        canvas()->setSelection({id});
        canvas()->tableInsert(true, true);
        QCOMPARE(deck().slides[0].elements.last().as<TableEl>()->cells.size(), 3);
        snapshot(window, "03-table");
    }

    void slides()
    {
        for (QAction *a : window->findChildren<QAction *>())
            if (a->shortcut() == QKeySequence("Ctrl+M")) a->trigger();
        QCOMPARE(deck().slides.size(), 2);
        QCOMPARE(window->findChild<SlidePanel *>()->current(), 1);
        QCOMPARE(deck().slides[1].layout, 1);
        findAction(window, "Duplicate Slide")->trigger();
        QCOMPARE(deck().slides.size(), 3);
        findAction(window, "Undo")->trigger();
        QCOMPARE(deck().slides.size(), 2);
    }

    void showBuilds()
    {
        Deck d = deck();
        Slide &s = d.slides[0];
        s.builds = {Build{s.elements[0].id, -1, "entr", 10, 0, "click", 300, 0}, Build{s.elements[1].id, -1, "entr", 1, 0, "after", 0, 0}};
        ShowState state(d, 0);
        QCOMPARE(state.steps(0), 1);
        RenderOptions hidden = state.options(0, 0, -1);
        QCOMPARE(hidden.opacity(s.elements[0], -1), 0.0);
        RenderOptions shown = state.options(0, 1, -1);
        QCOMPARE(shown.opacity(s.elements[0], -1), 1.0);
        RenderOptions halfway = state.options(0, 1, 150);
        QVERIFY(halfway.opacity(s.elements[0], -1) > 0 && halfway.opacity(s.elements[0], -1) < 1);
        QCOMPARE(halfway.opacity(s.elements[1], -1), 0.0); // starts after the fade
        state.next();
        QCOMPARE(state.step(), 1);
        state.next();
        QCOMPARE(state.slide(), 1);
        state.previous();
        QCOMPARE(state.slide(), 0);
        QCOMPARE(state.step(), 1);
    }

    void dualScreenShow()
    {
        if (QGuiApplication::screens().size() < 2) QSKIP("run with two screens, e.g. QT_QPA_PLATFORM=offscreen:configfile=…");
        window->startShow(0, false);
        QTest::qWait(200);
        QPointer<SlideShow> show;
        QPointer<PresenterView> presenter;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto s = qobject_cast<SlideShow *>(w)) show = s;
            if (auto p = qobject_cast<PresenterView *>(w)) presenter = p;
        }
        QVERIFY(show && presenter);
        QVERIFY(show->screen() != window->screen());
        QCOMPARE(presenter->screen(), window->screen());
        QVERIFY(show->isFullScreen());
        snapshot(presenter, "30-dual-presenter");
        window->findChild<ShowState *>()->finish();
        QTest::qWait(200);
        QVERIFY(show.isNull() && presenter.isNull());
        QVERIFY(!window->findChild<ShowState *>());
    }

    void showSnapshots()
    {
        QString sample = qEnvironmentVariable("PODIUM_SAMPLE");
        if (sample.isEmpty() || qEnvironmentVariable("PODIUM_SNAPSHOTS").isEmpty()) QSKIP("needs PODIUM_SAMPLE and PODIUM_SNAPSHOTS");
        std::optional<Deck> d = pptx::load(sample);
        QVERIFY(d);
        d->slides[2].transition = Transition{"push", "l", 1000, true, -1};
        auto *state = new ShowState(*d, 9);
        auto *show = new SlideShow(state);
        show->resize(1280, 800);
        show->show();
        QVERIFY(QTest::qWaitForWindowExposed(show));
        QTest::qWait(200);
        snapshot(show, "20-show-builds-hidden");
        state->next();
        QTest::qWait(1500);
        snapshot(show, "21-show-builds-done");
        state->go(1);
        QTest::qWait(300);
        state->next(); // the slide's click step plays the video
        QTest::qWait(2500);
        snapshot(show, "22-show-video");
        state->next();
        QTest::qWait(450);
        snapshot(show, "23-show-push-transition");
        auto *presenter = new PresenterView(state);
        presenter->resize(1400, 850);
        presenter->show();
        QVERIFY(QTest::qWaitForWindowExposed(presenter));
        QTest::qWait(1200);
        snapshot(presenter, "24-presenter");
        state->finish();
        QTest::qWait(100);
        delete state;
    }

    void sampleScreenshots()
    {
        QString sample = qEnvironmentVariable("PODIUM_SAMPLE");
        if (sample.isEmpty() || qEnvironmentVariable("PODIUM_SNAPSHOTS").isEmpty()) QSKIP("needs PODIUM_SAMPLE and PODIUM_SNAPSHOTS");
        auto *w = new MainWindow;
        QVERIFY(w->open(sample));
        w->resize(1600, 1000);
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        QTest::qWait(1500); // thumbnails render incrementally
        snapshot(w, "10-sample");
        w->findChild<SlidePanel *>()->setCurrent(9);
        QTest::qWait(300);
        Canvas *c = w->findChild<Canvas *>();
        c->setSelection({c->selection().isEmpty() ? w->findChild<Document *>()->deck().slides[9].elements[2].id : c->selection().first()});
        QTest::qWait(100);
        snapshot(w, "11-selected");
        w->findChild<Document *>()->undoStack()->clear();
        delete w;
    }
};

QTEST_MAIN(UiTest)
#include "tst_ui.moc"
