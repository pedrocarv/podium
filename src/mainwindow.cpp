#include "mainwindow.h"
#include "canvas.h"
#include "document.h"
#include "geometry.h"
#include "inspector.h"
#include "pptx.h"
#include "render.h"
#include "show.h"
#include "slidepanel.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWindow>

namespace {

QIcon drawn(const std::function<void(QPainter &, QRectF)> &draw)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QApplication::palette().color(QPalette::WindowText), 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    draw(p, QRectF(10, 10, 44, 44));
    return QIcon(pm);
}

QIcon shapeIcon(const QString &preset)
{
    return drawn([preset](QPainter &p, QRectF r) {
        geometry::Outline o = geometry::preset(preset, geometry::isLine(preset) ? r.size() : QSizeF(r.width(), r.height() * 0.8));
        p.translate(r.left(), r.top() + (geometry::isLine(preset) ? 0 : r.height() * 0.1));
        p.drawPath(o.stroke);
    });
}

QIcon icon(const QString &name, const std::function<void(QPainter &, QRectF)> &fallback = {})
{
    if (QIcon::hasThemeIcon(name)) return QIcon::fromTheme(name);
    return fallback ? drawn(fallback) : QIcon();
}

const QString pptxFilter = QObject::tr("PowerPoint Presentation (*.pptx)");

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowIcon(QIcon(":/podium.svg"));
    status = new QLabel;
    statusBar()->addWidget(status);
    zoomBox = new QComboBox;
    zoomBox->setEditable(true);
    zoomBox->setInsertPolicy(QComboBox::NoInsert);
    zoomBox->addItem(tr("Fit"), 0.0);
    for (int z : {50, 75, 100, 150, 200, 300}) zoomBox->addItem(QString("%1%").arg(z), z / 100.0);
    zoomBox->setMinimumContentsLength(5);
    statusBar()->addPermanentWidget(zoomBox);
    connect(zoomBox, &QComboBox::activated, this, [this](int i) {
        if (canvas) canvas->setZoom(zoomBox->itemData(i).toDouble() * (logicalDpiX() / 72.0));
    });

    inspectorDock = new QDockWidget(tr("Format"), this);
    inspectorDock->setObjectName("format");
    inspectorDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);

    buildActions();
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("state").toByteArray());
    if (!settings.contains("geometry")) resize(1400, 860);
}

MainWindow::~MainWindow()
{
    // Tear the document and its views down while the toolbar they talk to still exists.
    if (doc) doc->disconnect(), doc->undoStack()->disconnect();
    delete split;
    delete inspector;
}

QAction *MainWindow::action(const QString &text, const QString &iconName, const QKeySequence &key, std::function<void()> fn)
{
    auto *a = new QAction(text, this);
    if (!iconName.isEmpty()) a->setIcon(icon(iconName));
    if (!key.isEmpty()) a->setShortcut(key);
    if (fn) connect(a, &QAction::triggered, this, fn);
    return a;
}

void MainWindow::buildActions()
{
    // File
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(action(tr("&New"), "document-new", QKeySequence::New, [this] { newDeck(); }));
    file->addAction(action(tr("&Open…"), "document-open", QKeySequence::Open, [this] {
        if (!maybeSave()) return;
        QString path = QFileDialog::getOpenFileName(this, tr("Open Presentation"), QSettings().value("dir").toString(), pptxFilter);
        if (!path.isEmpty()) open(path);
    }));
    recentMenu = file->addMenu(tr("Open &Recent"));
    connect(recentMenu, &QMenu::aboutToShow, this, [this] {
        recentMenu->clear();
        for (const QString &path : QSettings().value("recent").toStringList())
            recentMenu->addAction(QFileInfo(path).fileName(), this, [this, path] {
                if (maybeSave()) open(path);
            })->setToolTip(path);
    });
    file->addSeparator();
    file->addAction(action(tr("&Save"), "document-save", QKeySequence::Save, [this] { save(); }));
    file->addAction(action(tr("Save &As…"), "document-save-as", QKeySequence::SaveAs, [this] { saveAs(); }));
    file->addAction(action(tr("&Export as PDF…"), "", QKeySequence(tr("Ctrl+Shift+E")), [this] { exportPdf(); }));
    file->addSeparator();
    file->addAction(action(tr("&Quit"), "application-exit", QKeySequence::Quit, [this] { close(); }));

    // Edit
    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    undoAction = action(tr("&Undo"), "edit-undo", QKeySequence::Undo, [this] { undo(); });
    redoAction = action(tr("&Redo"), "edit-redo", QKeySequence::Redo, [this] { redo(); });
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(tr("Ctrl+Y"))});
    edit->addAction(undoAction);
    edit->addAction(redoAction);
    edit->addSeparator();
    QAction *cut = action(tr("Cu&t"), "edit-cut", QKeySequence::Cut, [this] { canvas->cut(); });
    QAction *copy = action(tr("&Copy"), "edit-copy", QKeySequence::Copy, [this] { canvas->copy(); });
    QAction *paste = action(tr("&Paste"), "edit-paste", QKeySequence::Paste, [this] { canvas->paste(); });
    QAction *duplicate = action(tr("&Duplicate"), "", QKeySequence(tr("Ctrl+D")), [this] {
        if (panel->hasFocus()) return duplicateSlides();
        canvas->copy();
        canvas->paste();
    });
    QAction *remove = action(tr("De&lete"), "edit-delete", {}, [this] {
        if (panel->hasFocus()) deleteSlides();
        else canvas->removeSelection();
    });
    QAction *selectAll = action(tr("Select &All"), "edit-select-all", QKeySequence::SelectAll, [this] {
        if (canvas->isEditingText()) {
            QTextCursor c = canvas->textEditor()->textCursor();
            c.select(QTextCursor::Document);
            canvas->textEditor()->setTextCursor(c);
        } else {
            canvas->selectAll();
        }
    });
    edit->addActions({cut, copy, paste, duplicate, remove});
    edit->addSeparator();
    edit->addAction(selectAll);

    // View
    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(action(tr("Zoom &In"), "zoom-in", QKeySequence::ZoomIn, [this] { canvas->setZoom(canvas->zoom() * 1.25); }));
    view->addAction(action(tr("Zoom &Out"), "zoom-out", QKeySequence::ZoomOut, [this] { canvas->setZoom(canvas->zoom() / 1.25); }));
    view->addAction(action(tr("&Fit Slide"), "zoom-fit-best", QKeySequence(tr("Ctrl+0")), [this] { canvas->setZoom(0); }));
    view->addSeparator();
    view->addAction(inspectorDock->toggleViewAction());

    // Insert
    QMenu *insert = menuBar()->addMenu(tr("&Insert"));
    layoutMenu = insert->addMenu(icon("list-add", [](QPainter &p, QRectF r) {
                                     p.drawRect(r.adjusted(0, 6, 0, -6));
                                     p.drawLine(r.center() - QPointF(8, 0), r.center() + QPointF(8, 0));
                                     p.drawLine(r.center() - QPointF(0, 8), r.center() + QPointF(0, 8));
                                 }),
                                 tr("New &Slide"));
    connect(layoutMenu, &QMenu::aboutToShow, this, [this] {
        layoutMenu->clear();
        const Deck &deck = doc->deck();
        for (int i = 0; i < deck.layouts.size(); ++i) {
            QSize size(160, int(160 * deck.size.height() / deck.size.width()));
            Deck preview = deck;
            preview.slides = {ops::slideFromLayout(deck, i)};
            RenderOptions o;
            o.editing = true;
            QImage img(size, QImage::Format_ARGB32_Premultiplied);
            QPainter p(&img);
            p.scale(size.width() / deck.size.width(), size.height() / deck.size.height());
            paintSlide(p, preview, preview.slides.first(), o);
            p.end();
            layoutMenu->addAction(QIcon(QPixmap::fromImage(img)), deck.layouts[i].name, this, [this, i] { newSlide(i); });
        }
    });
    QAction *newSlideAction = action(tr("New Slide"), "", QKeySequence(tr("Ctrl+M")), [this] {
        int current = doc->deck().slides.value(panel->current()).layout;
        newSlide(current == 0 && doc->deck().layouts.size() > 1 ? 1 : current);
    });
    newSlideAction->setIcon(layoutMenu->icon());
    addAction(newSlideAction);
    insert->addSeparator();
    QAction *textBox = action(tr("&Text Box"), "insert-text", {}, [this] {
        canvas->beginInsert([this](QLineF l) {
            QRectF r = QRectF(l.p1(), l.p2()).normalized();
            if (r.width() < 40) r.setWidth(300);
            return ops::textBox(doc->deck(), doc->deck().slides[canvas->slideIndex()].nextId(), r);
        });
    });
    textBox->setIcon(icon("insert-text", [](QPainter &p, QRectF r) {
        p.drawRect(r);
        QFont f = p.font();
        f.setPixelSize(30);
        f.setBold(true);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, "A");
    }));
    insert->addAction(textBox);
    QMenu *shapes = insert->addMenu(shapeIcon("ellipse"), tr("S&hape"));
    for (const QString &preset : geometry::insertable())
        shapes->addAction(shapeIcon(preset), ops::presetName(preset), this, [this, preset] {
            canvas->beginInsert([this, preset](QLineF l) {
                QRectF r = QRectF(l.p1(), l.p2()).normalized();
                Element e = ops::shape(doc->deck(), doc->deck().slides[canvas->slideIndex()].nextId(), preset, r);
                if (geometry::isLine(preset)) {
                    e.flipH = l.p1().x() > l.p2().x();
                    e.flipV = l.p1().y() > l.p2().y();
                }
                return e;
            });
        });
    QAction *image = action(tr("&Image…"), "insert-image", {}, [this] {
        QStringList formats;
        for (const QByteArray &f : QImageReader::supportedImageFormats()) formats << "*." + f;
        QString path = QFileDialog::getOpenFileName(this, tr("Insert Image"), QSettings().value("imageDir").toString(),
                                                    tr("Images (%1)").arg(formats.join(' ')));
        if (path.isEmpty()) return;
        QSettings().setValue("imageDir", QFileInfo(path).path());
        canvas->insertImageFile(path);
    });
    insert->addAction(image);
    QAction *table = action(tr("T&able…"), "insert-table", {}, [this] { insertTable(); });
    table->setIcon(icon("insert-table", [](QPainter &p, QRectF r) {
        p.drawRect(r);
        for (int i = 1; i < 3; ++i) {
            p.drawLine(QPointF(r.left() + r.width() * i / 3, r.top()), QPointF(r.left() + r.width() * i / 3, r.bottom()));
            p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 3), QPointF(r.right(), r.top() + r.height() * i / 3));
        }
    }));
    insert->addAction(table);

    // Format
    QMenu *format = menuBar()->addMenu(tr("F&ormat"));
    auto charAction = [this](const QString &text, const QString &iconName, QKeySequence key, auto setter) {
        QAction *a = action(text, iconName, key, {});
        a->setCheckable(true);
        connect(a, &QAction::triggered, this, [this, setter](bool on) {
            ops::CharEdit e;
            setter(e, on);
            canvas->applyChar(e);
        });
        return a;
    };
    bold = charAction(tr("&Bold"), "format-text-bold", QKeySequence::Bold, [](ops::CharEdit &e, bool on) { e.bold = on; });
    italic = charAction(tr("&Italic"), "format-text-italic", QKeySequence::Italic, [](ops::CharEdit &e, bool on) { e.italic = on; });
    underline = charAction(tr("&Underline"), "format-text-underline", QKeySequence::Underline, [](ops::CharEdit &e, bool on) { e.underline = on; });
    QAction *bigger = action(tr("&Larger Text"), "format-font-size-more", QKeySequence(tr("Ctrl+]")), [this] {
        ops::CharEdit e;
        e.grow = 1.1;
        canvas->applyChar(e);
    });
    QAction *smaller = action(tr("S&maller Text"), "format-font-size-less", QKeySequence(tr("Ctrl+[")), [this] {
        ops::CharEdit e;
        e.grow = 1 / 1.1;
        canvas->applyChar(e);
    });
    format->addActions({bold, italic, underline, bigger, smaller});
    format->addSeparator();
    auto *alignGroup = new QActionGroup(this);
    for (auto [text, iconName, key, align] : {std::tuple{tr("Align &Left"), "format-justify-left", tr("Ctrl+L"), Qt::AlignLeft},
                                              {tr("&Center"), "format-justify-center", tr("Ctrl+E"), Qt::AlignHCenter},
                                              {tr("Align &Right"), "format-justify-right", tr("Ctrl+R"), Qt::AlignRight},
                                              {tr("&Justify"), "format-justify-fill", tr("Ctrl+J"), Qt::AlignJustify}}) {
        Qt::Alignment value = align;
        QAction *a = action(text, iconName, QKeySequence(key), [this, value] {
            ops::ParaEdit e;
            e.align = value;
            canvas->applyPara(e);
        });
        a->setCheckable(true);
        a->setData(int(value));
        alignGroup->addAction(a);
        alignActions << a;
    }
    format->addActions(alignActions);
    format->addSeparator();
    auto listIcon = [](bool numbers) {
        return drawn([numbers](QPainter &p, QRectF r) {
            QFont f = p.font();
            f.setPixelSize(15);
            f.setBold(true);
            p.setFont(f);
            for (int i = 0; i < 3; ++i) {
                double y = r.top() + 6 + i * 16;
                if (numbers) p.drawText(QRectF(r.left() - 4, y - 9, 14, 18), Qt::AlignCenter, QString::number(i + 1));
                else p.drawEllipse(QPointF(r.left() + 3, y), 2.5, 2.5);
                p.drawLine(QPointF(r.left() + 16, y), QPointF(r.right(), y));
            }
        });
    };
    bullets = action(tr("&Bullets"), "view-list-bullet", {}, [this] {
        ops::ParaEdit e;
        e.bullet = canvas->paraStyle().bullet.kind == Bullet::Char ? Bullet::None : Bullet::Char;
        canvas->applyPara(e);
    });
    numbering = action(tr("&Numbering"), "view-list-ordered", {}, [this] {
        ops::ParaEdit e;
        e.bullet = canvas->paraStyle().bullet.kind == Bullet::Number ? Bullet::None : Bullet::Number;
        canvas->applyPara(e);
    });
    for (QAction *a : {bullets, numbering}) a->setCheckable(true);
    if (bullets->icon().isNull()) bullets->setIcon(listIcon(false));
    if (numbering->icon().isNull()) numbering->setIcon(listIcon(true));
    QAction *indent = action(tr("Increase List &Level"), "format-indent-more", {}, [this] {
        ops::ParaEdit e;
        e.level = 1;
        canvas->applyPara(e);
    });
    QAction *outdent = action(tr("Decrease List Le&vel"), "format-indent-less", {}, [this] {
        ops::ParaEdit e;
        e.level = -1;
        canvas->applyPara(e);
    });
    format->addActions({bullets, numbering, outdent, indent});

    // Arrange
    QMenu *arrange = menuBar()->addMenu(tr("&Arrange"));
    QAction *front = action(tr("Bring to &Front"), "go-top", QKeySequence(tr("Ctrl+Shift+]")), [this] { canvas->arrange(2); });
    QAction *forward = action(tr("Bring F&orward"), "go-up", {}, [this] { canvas->arrange(1); });
    QAction *backward = action(tr("Send &Backward"), "go-down", {}, [this] { canvas->arrange(-1); });
    QAction *back = action(tr("Send to Bac&k"), "go-bottom", QKeySequence(tr("Ctrl+Shift+[")), [this] { canvas->arrange(-2); });
    QAction *group = action(tr("&Group"), "", QKeySequence(tr("Ctrl+G")), [this] { canvas->group(); });
    QAction *ungroup = action(tr("&Ungroup"), "", QKeySequence(tr("Ctrl+Shift+G")), [this] { canvas->ungroup(); });
    arrange->addActions({front, forward, backward, back});
    arrange->addSeparator();
    arrange->addActions({group, ungroup});

    // Slide
    QMenu *slide = menuBar()->addMenu(tr("&Slide"));
    slide->addAction(newSlideAction);
    slide->addMenu(layoutMenu);
    QAction *duplicateSlide = action(tr("&Duplicate Slide"), "", {}, [this] { duplicateSlides(); });
    QAction *deleteSlide = action(tr("De&lete Slide"), "edit-delete", {}, [this] { deleteSlides(); });
    hideSlide = action(tr("&Hide Slide"), "", {}, [this] { toggleHidden(); });
    hideSlide->setCheckable(true);
    slide->addActions({duplicateSlide, deleteSlide, hideSlide});

    // Slide Show
    QMenu *show = menuBar()->addMenu(tr("Sli&de Show"));
    QAction *play = action(tr("From &Beginning"), "media-playback-start", QKeySequence(Qt::Key_F5), [this] { startShow(0, false); });
    QAction *playHere = action(tr("From &Current Slide"), "", QKeySequence(Qt::SHIFT | Qt::Key_F5), [this] { startShow(panel->current(), false); });
    QAction *rehearse = action(tr("&Presenter View"), "", QKeySequence(Qt::ALT | Qt::Key_F5), [this] { startShow(panel->current(), true); });
    show->addActions({play, playHere, rehearse});

    // Toolbar
    QToolBar *bar = addToolBar(tr("Toolbar"));
    bar->setObjectName("toolbar");
    bar->setMovable(false);
    bar->setIconSize(QSize(20, 20));
    auto *newSlideButton = new QToolButton;
    newSlideButton->setDefaultAction(newSlideAction);
    newSlideButton->setMenu(layoutMenu);
    newSlideButton->setPopupMode(QToolButton::MenuButtonPopup);
    bar->addWidget(newSlideButton);
    bar->addSeparator();
    bar->addActions({undoAction, redoAction});
    bar->addSeparator();
    bar->addAction(textBox);
    auto *shapeButton = new QToolButton;
    shapeButton->setMenu(shapes);
    shapeButton->setIcon(shapes->icon());
    shapeButton->setToolTip(tr("Shapes"));
    shapeButton->setPopupMode(QToolButton::InstantPopup);
    bar->addWidget(shapeButton);
    bar->addActions({image, table});
    bar->addSeparator();
    fontBox = new QFontComboBox;
    fontBox->setMaximumWidth(180);
    connect(fontBox, &QFontComboBox::activated, this, [this] {
        ops::CharEdit e;
        e.font = fontBox->currentFont().family();
        canvas->applyChar(e);
        canvas->setFocus();
    });
    bar->addWidget(fontBox);
    sizeBox = new QComboBox;
    sizeBox->setEditable(true);
    for (double s : {8.0, 9.0, 10.0, 10.5, 11.0, 12.0, 14.0, 16.0, 18.0, 20.0, 24.0, 28.0, 32.0, 36.0, 40.0, 44.0, 48.0, 54.0, 60.0, 66.0, 72.0, 80.0, 88.0, 96.0})
        sizeBox->addItem(QString::number(s));
    sizeBox->setMinimumContentsLength(3);
    connect(sizeBox, &QComboBox::textActivated, this, [this](const QString &text) {
        bool ok = false;
        double size = text.toDouble(&ok);
        if (!ok || size <= 0) return;
        ops::CharEdit e;
        e.size = size;
        canvas->applyChar(e);
        canvas->setFocus();
    });
    bar->addWidget(sizeBox);
    bar->addActions({bold, italic, underline});
    textColor = new ColorButton(tr("Text Color"), false);
    textColor->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(textColor, &ColorButton::picked, this, [this](QColor c) {
        ops::CharEdit e;
        e.color = c;
        canvas->applyChar(e);
    });
    bar->addWidget(textColor);
    bar->addSeparator();
    bar->addActions(alignActions);
    bar->addActions({bullets, numbering, outdent, indent});
    QWidget *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    play->setToolTip(tr("Start the slide show (F5)"));
    if (play->icon().isNull())
        play->setIcon(drawn([](QPainter &p, QRectF r) {
            p.setBrush(p.pen().color());
            p.drawPolygon(QPolygonF{r.topLeft() + QPointF(8, 2), QPointF(r.right() - 4, r.center().y()), r.bottomLeft() + QPointF(8, -2)});
        }));
    bar->addAction(play);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    objectActions = {cut, copy, paste, duplicate, remove};
    objectActions << new QAction(this);
    objectActions.last()->setSeparator(true);
    objectActions << front << back << group << ungroup;
}

void MainWindow::newDeck()
{
    if (!maybeSave()) return;
    setDocument(new Document(pptx::blank()));
}

bool MainWindow::open(const QString &path)
{
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::optional<Deck> deck = pptx::load(path, &error);
    QApplication::restoreOverrideCursor();
    if (!deck) {
        QMessageBox::warning(this, tr("Open Presentation"), tr("Could not open %1:\n%2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    setDocument(new Document(*deck, QFileInfo(path).absoluteFilePath()));
    addRecent(QFileInfo(path).absoluteFilePath());
    return true;
}

void MainWindow::setDocument(Document *d)
{
    delete split;
    delete inspector;
    if (doc) {
        doc->disconnect();
        doc->undoStack()->disconnect();
        doc->deleteLater();
    }
    doc = d;
    doc->setParent(this);

    canvas = new Canvas(doc);
    panel = new SlidePanel(doc);
    notes = new QPlainTextEdit;
    notes->setPlaceholderText(tr("Click to add speaker notes"));
    notes->setFrameShape(QFrame::NoFrame);
    auto *right = new QSplitter(Qt::Vertical);
    right->addWidget(canvas);
    right->addWidget(notes);
    right->setStretchFactor(0, 5);
    right->setStretchFactor(1, 1);
    right->setSizes({700, 120});
    split = new QSplitter;
    split->addWidget(panel);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({220, 1000});
    setCentralWidget(split);
    inspector = new Inspector(doc, canvas);
    inspectorDock->setWidget(inspector);

    canvas->setContextActions(objectActions);
    QList<QAction *> slideActions;
    slideActions << action(tr("New Slide"), "", {}, [this] { newSlide(doc->deck().slides.value(panel->current()).layout); })
                 << action(tr("Duplicate Slide"), "", {}, [this] { duplicateSlides(); })
                 << action(tr("Delete Slide"), "", {}, [this] { deleteSlides(); }) << hideSlide;
    panel->setContextActions(slideActions);

    connect(panel, &SlidePanel::currentSlideChanged, this, [this](int i) {
        canvas->setSlideIndex(i);
        syncNotes();
        updateActions();
        inspector->refresh();
    });
    connect(panel, &SlidePanel::deleteRequested, this, &MainWindow::deleteSlides);
    connect(canvas, &Canvas::stepSlide, this, [this](int delta) {
        panel->setCurrent(std::clamp(panel->current() + delta, 0, int(doc->deck().slides.size()) - 1));
    });
    connect(canvas, &Canvas::formatChanged, this, &MainWindow::updateFormat);
    connect(canvas, &Canvas::selectionChanged, this, &MainWindow::updateActions);
    connect(canvas, &Canvas::zoomChanged, this, [this](double z) {
        zoomBox->setEditText(QString("%1%").arg(qRound(z / (logicalDpiX() / 72.0) * 100)));
    });
    connect(doc, &Document::changed, this, [this] {
        updateTitle();
        updateActions();
        syncNotes();
    });
    auto *notesTimer = new QTimer(notes);
    notesTimer->setSingleShot(true);
    notesTimer->setInterval(400);
    connect(notes, &QPlainTextEdit::textChanged, notesTimer, qOverload<>(&QTimer::start));
    connect(notesTimer, &QTimer::timeout, this, [this] {
        int i = panel->current();
        QString text = notes->toPlainText();
        if (i < 0 || doc->deck().slides[i].notes == text) return;
        doc->change(tr("Edit Notes"), [&](Deck &deck) { deck.slides[i].notes = text; }, 30);
    });
    connect(doc->undoStack(), &QUndoStack::canUndoChanged, undoAction, &QAction::setEnabled);
    connect(doc->undoStack(), &QUndoStack::canRedoChanged, redoAction, &QAction::setEnabled);

    panel->setCurrent(0);
    canvas->setSlideIndex(0);
    syncNotes();
    updateTitle();
    updateActions();
    updateFormat();
    canvas->setFocus();
}

void MainWindow::syncNotes()
{
    int i = panel->current();
    QString text = i >= 0 && i < doc->deck().slides.size() ? doc->deck().slides[i].notes : QString();
    if (notes->toPlainText() == text) return;
    QSignalBlocker block(notes);
    notes->setPlainText(text);
}

void MainWindow::updateTitle()
{
    setWindowTitle(QString("%1[*] — Podium").arg(doc->title()));
    setWindowModified(doc->isModified());
}

void MainWindow::updateActions()
{
    int i = panel->current();
    int count = int(doc->deck().slides.size());
    status->setText(tr("Slide %1 of %2").arg(i + 1).arg(count));
    hideSlide->setChecked(i >= 0 && i < count && doc->deck().slides[i].hidden);
    undoAction->setEnabled(doc->undoStack()->canUndo() || canvas->isEditingText());
    redoAction->setEnabled(doc->undoStack()->canRedo() || canvas->isEditingText());
    updateFormat();
}

void MainWindow::updateFormat()
{
    CharStyle c = canvas->charStyle();
    ParaStyle p = canvas->paraStyle();
    QSignalBlocker b1(fontBox), b2(sizeBox);
    fontBox->setCurrentFont(QFont(c.font));
    sizeBox->setEditText(QString::number(c.size));
    bold->setChecked(c.bold);
    italic->setChecked(c.italic);
    underline->setChecked(c.underline);
    textColor->setColor(c.color);
    for (QAction *a : alignActions) a->setChecked(Qt::Alignment(a->data().toInt()) == p.align);
    bullets->setChecked(p.bullet.kind == Bullet::Char);
    numbering->setChecked(p.bullet.kind == Bullet::Number);
    QList<QColor> theme;
    for (QString name : {"tx1", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6"})
        theme << doc->deck().theme.colors.value(name);
    textColor->setThemeColors(theme);
}

void MainWindow::undo()
{
    if (canvas->isEditingText()) {
        QTextDocument *d = canvas->textEditor()->document();
        if (d->isUndoAvailable()) return d->undo();
        canvas->stopEditing();
    }
    doc->undoStack()->undo();
}

void MainWindow::redo()
{
    if (canvas->isEditingText()) {
        QTextDocument *d = canvas->textEditor()->document();
        if (d->isRedoAvailable()) return d->redo();
        canvas->stopEditing();
    }
    doc->undoStack()->redo();
}

void MainWindow::newSlide(int layout)
{
    canvas->stopEditing();
    int at = panel->current() + 1;
    doc->change(tr("New Slide"), [&](Deck &d) { d.slides.insert(at, ops::slideFromLayout(d, std::clamp(layout, 0, int(d.layouts.size()) - 1))); });
    panel->setCurrent(at);
}

void MainWindow::duplicateSlides()
{
    canvas->stopEditing();
    QList<int> rows = panel->selectedSlides();
    if (rows.isEmpty()) return;
    doc->change(tr("Duplicate Slide"), [&](Deck &d) {
        QList<Slide> copies;
        for (int r : rows) copies << d.slides[r];
        for (int i = 0; i < copies.size(); ++i) d.slides.insert(rows.last() + 1 + i, copies[i]);
    });
    panel->setCurrent(rows.last() + 1);
}

void MainWindow::deleteSlides()
{
    canvas->stopEditing();
    QList<int> rows = panel->selectedSlides();
    if (rows.isEmpty() || rows.size() >= doc->deck().slides.size()) return;
    doc->change(rows.size() > 1 ? tr("Delete Slides") : tr("Delete Slide"), [&](Deck &d) {
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) d.slides.removeAt(*it);
    });
    panel->setCurrent(std::min(rows.first(), int(doc->deck().slides.size()) - 1));
}

void MainWindow::toggleHidden()
{
    QList<int> rows = panel->selectedSlides();
    bool hide = !doc->deck().slides.value(panel->current()).hidden;
    doc->change(hide ? tr("Hide Slide") : tr("Show Slide"), [&](Deck &d) {
        for (int r : rows) d.slides[r].hidden = hide;
    });
}

void MainWindow::insertTable()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Insert Table"));
    auto *rows = new QSpinBox, *cols = new QSpinBox;
    rows->setRange(1, 50);
    cols->setRange(1, 20);
    rows->setValue(3);
    cols->setValue(3);
    auto *form = new QFormLayout(&dialog);
    form->addRow(tr("Columns"), cols);
    form->addRow(tr("Rows"), rows);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    const Deck &deck = doc->deck();
    double w = deck.size.width() * 0.8, h = 32.0 * rows->value();
    QRectF rect((deck.size.width() - w) / 2, (deck.size.height() - h) / 2, w, h);
    canvas->insert(ops::table(deck, deck.slides[canvas->slideIndex()].nextId(), rows->value(), cols->value(), rect));
}

bool MainWindow::maybeSave()
{
    if (!doc || !doc->isModified()) return true;
    if (canvas) canvas->stopEditing();
    auto answer = QMessageBox::question(this, tr("Save Changes"), tr("Save changes to “%1” before closing?").arg(doc->title()),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

bool MainWindow::save()
{
    canvas->stopEditing();
    return doc->path().isEmpty() ? saveAs() : saveTo(doc->path());
}

bool MainWindow::saveAs()
{
    canvas->stopEditing();
    QString start = doc->path().isEmpty() ? QSettings().value("dir").toString() + "/" + doc->title() + ".pptx" : doc->path();
    QString path = QFileDialog::getSaveFileName(this, tr("Save Presentation"), start, pptxFilter);
    if (path.isEmpty()) return false;
    if (!path.endsWith(".pptx", Qt::CaseInsensitive)) path += ".pptx";
    if (!saveTo(path)) return false;
    addRecent(path);
    return true;
}

bool MainWindow::saveTo(const QString &path)
{
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = pptx::save(doc->deck(), path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
        return false;
    }
    doc->markSaved(path);
    statusBar()->showMessage(tr("Saved"), 2000);
    return true;
}

void MainWindow::exportPdf()
{
    canvas->stopEditing();
    QString start = (doc->path().isEmpty() ? QSettings().value("dir").toString() + "/" + doc->title() : doc->path().chopped(5)) + ".pdf";
    QString path = QFileDialog::getSaveFileName(this, tr("Export PDF"), start, tr("PDF (*.pdf)"));
    if (path.isEmpty()) return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = ::exportPdf(doc->deck(), path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) QMessageBox::warning(this, tr("Export PDF"), error);
    else statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3000);
}

void MainWindow::addRecent(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value("recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    settings.setValue("recent", recent.mid(0, 10));
    settings.setValue("dir", QFileInfo(path).path());
}

void MainWindow::startShow(int from, bool rehearse)
{
    if (showing) return;
    canvas->stopEditing();
    QScreen *here = screen(), *projector = nullptr;
    for (QScreen *s : QGuiApplication::screens())
        if (s != here) projector = s;
    auto *state = new ShowState(doc->deck(), std::max(from, 0), this);
    QList<QPointer<QWidget>> windows;
    auto place = [](QWidget *w, QScreen *screen, bool full) {
        w->setGeometry(screen->availableGeometry());
        w->winId();
        w->windowHandle()->setScreen(screen);
        full ? w->showFullScreen() : w->showMaximized();
    };
    if (!rehearse || projector) {
        auto *show = new SlideShow(state);
        place(show, projector ? projector : here, true);
        windows << show;
    }
    if (projector || rehearse) {
        auto *view = new PresenterView(state);
        place(view, here, bool(projector));
        windows << view;
    }
    showing = true;
    for (QWidget *w : windows)
        connect(w, &QObject::destroyed, this, [this, state, windows](QObject *gone) {
            // A widget's QPointers are cleared only after destroyed() is emitted, so skip the sender.
            for (const QPointer<QWidget> &other : windows)
                if (other && other != gone) return;
            panel->setCurrent(state->slide());
            state->deleteLater();
            showing = false;
            activateWindow();
        });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) return event->ignore();
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("state", saveState());
    event->accept();
}
