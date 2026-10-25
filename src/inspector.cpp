#include "inspector.h"
#include "canvas.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QMenu>
#include <QPainter>
#include <QVBoxLayout>

namespace {

QIcon swatch(QColor c)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    if (c.isValid()) {
        p.setBrush(c);
        p.setPen(QPen(QColor(128, 128, 128), 1.5));
        p.drawRoundedRect(QRectF(3, 3, 26, 26), 5, 5);
    } else {
        p.setPen(QPen(QColor(160, 160, 160), 1.5));
        p.drawRoundedRect(QRectF(3, 3, 26, 26), 5, 5);
        p.setPen(QPen(QColor(220, 60, 60), 2));
        p.drawLine(QPointF(6, 26), QPointF(26, 6));
    }
    return QIcon(pm);
}

QDoubleSpinBox *spin(double min, double max, double step, const QString &suffix, int decimals = 1)
{
    auto *s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setAccelerated(true);
    return s;
}

QComboBox *combo(const QList<std::pair<QString, QVariant>> &items)
{
    auto *c = new QComboBox;
    for (const auto &[text, data] : items) c->addItem(text, data);
    return c;
}

const QList<std::pair<QString, QVariant>> transitions{{QObject::tr("None"), ""},       {QObject::tr("Fade"), "fade"},
                                                      {QObject::tr("Push"), "push"},   {QObject::tr("Wipe"), "wipe"},
                                                      {QObject::tr("Cover"), "cover"}, {QObject::tr("Cut"), "cut"}};
const QList<std::pair<QString, QVariant>> effects{{QObject::tr("None"), 0}, {QObject::tr("Appear"), 1}, {QObject::tr("Fade"), 10},
                                                  {QObject::tr("Dissolve"), 9}};

void setCombo(QComboBox *c, const QVariant &value, const QString &fallback = {})
{
    int i = c->findData(value);
    if (i < 0 && !fallback.isEmpty()) {
        c->addItem(fallback, value);
        i = c->count() - 1;
    }
    c->setCurrentIndex(std::max(i, 0));
}

} // namespace

// --- ColorButton ---

ColorButton::ColorButton(const QString &label, bool allowNone, QWidget *parent)
    : QToolButton(parent), label(label), allowNone(allowNone)
{
    setPopupMode(InstantPopup);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setText(label);
    setToolTip(label);
    setMenu(new QMenu(this));
    setColor(Qt::black);
    rebuildMenu();
}

void ColorButton::setColor(QColor color)
{
    current = color;
    setIcon(swatch(color));
}

void ColorButton::setThemeColors(const QList<QColor> &colors)
{
    if (colors == theme) return;
    theme = colors;
    rebuildMenu();
}

void ColorButton::rebuildMenu()
{
    QMenu *m = menu();
    m->clear();
    if (allowNone) m->addAction(swatch({}), tr("None"), this, [this] { emit picked({}); });
    QList<QColor> colors = theme;
    colors << Qt::white << Qt::black << QColor(0x6B7280) << QColor(0xEF4444) << QColor(0xF59E0B) << QColor(0x10B981);
    for (QColor c : colors) m->addAction(swatch(c), c.name().toUpper(), this, [this, c] { emit picked(c); });
    m->addSeparator();
    m->addAction(tr("More Colors…"), this, [this] {
        QColor c = QColorDialog::getColor(current.isValid() ? current : Qt::black, this, label, QColorDialog::ShowAlphaChannel);
        if (c.isValid()) emit picked(c);
    });
}

// --- Inspector ---

Inspector::Inspector(Document *doc, Canvas *canvas, QWidget *parent) : QScrollArea(parent), doc(doc), canvas(canvas)
{
    auto *content = new QWidget;
    auto *col = new QVBoxLayout(content);
    auto group = [&](const QString &title) {
        auto *box = new QGroupBox(title);
        auto *form = new QFormLayout(box);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        col->addWidget(box);
        return std::pair{box, form};
    };

    auto [sb, sf] = group(tr("Slide"));
    slideBox = sb;
    layout = new QComboBox;
    background = new ColorButton(tr("Background"), false);
    hidden = new QCheckBox(tr("Hide in slideshow"));
    transition = combo(transitions);
    transitionTime = spin(0.1, 10, 0.1, " s");
    advanceOn = new QCheckBox(tr("Advance after"));
    advance = spin(0, 3600, 1, " s");
    sf->addRow(tr("Layout"), layout);
    sf->addRow(tr("Fill"), background);
    sf->addRow(hidden);
    sf->addRow(tr("Transition"), transition);
    sf->addRow(tr("Duration"), transitionTime);
    sf->addRow(advanceOn, advance);

    auto [ab, af] = group(tr("Position"));
    arrangeBox = ab;
    x = spin(-5000, 5000, 1, " pt");
    y = spin(-5000, 5000, 1, " pt");
    w = spin(1, 5000, 1, " pt");
    h = spin(0, 5000, 1, " pt");
    rotation = spin(-360, 360, 1, "°");
    af->addRow(tr("X"), x);
    af->addRow(tr("Y"), y);
    af->addRow(tr("Width"), w);
    af->addRow(tr("Height"), h);
    af->addRow(tr("Rotation"), rotation);

    auto [shb, shf] = group(tr("Shape"));
    shapeBox = shb;
    fill = new ColorButton(tr("Fill"), true);
    line = new ColorButton(tr("Line"), true);
    lineWidth = spin(0, 100, 0.25, " pt", 2);
    dash = combo({{tr("Solid"), "solid"}, {tr("Dash"), "dash"}, {tr("Dot"), "sysDot"}, {tr("Dash Dot"), "dashDot"}, {tr("Long Dash"), "lgDash"}});
    shf->addRow(tr("Fill"), fill);
    shf->addRow(tr("Line"), line);
    shf->addRow(tr("Line width"), lineWidth);
    shf->addRow(tr("Dash"), dash);

    auto [tb, tf] = group(tr("Text Box"));
    textBox = tb;
    anchor = combo({{tr("Top"), TextBody::Top}, {tr("Middle"), TextBody::Middle}, {tr("Bottom"), TextBody::Bottom}});
    autofit = combo({{tr("Do not autofit"), TextBody::NoAutofit}, {tr("Shrink text on overflow"), TextBody::Shrink},
                     {tr("Resize shape to fit text"), TextBody::Resize}});
    wrap = new QCheckBox(tr("Wrap text in shape"));
    tf->addRow(tr("Align"), anchor);
    tf->addRow(tr("Autofit"), autofit);
    tf->addRow(wrap);

    auto [anb, anf] = group(tr("Animation"));
    animBox = anb;
    effect = combo(effects);
    trigger = combo({{tr("On click"), "click"}, {tr("With previous"), "with"}, {tr("After previous"), "after"}});
    duration = spin(0.1, 20, 0.1, " s");
    byParagraph = new QCheckBox(tr("By paragraph"));
    anf->addRow(tr("Entrance"), effect);
    anf->addRow(tr("Start"), trigger);
    anf->addRow(tr("Duration"), duration);
    anf->addRow(byParagraph);
    col->addStretch();

    setWidget(content);
    setWidgetResizable(true);
    setFrameShape(NoFrame);
    setMinimumWidth(content->sizeHint().width() + 24);

    auto onSlide = [this](auto *widget, auto signal, const QString &text, auto fn, int merge = 0) {
        connect(widget, signal, this, [=, this] {
            if (!updating) editSlide(text, fn, merge);
        });
    };
    onSlide(layout, &QComboBox::activated, tr("Change Layout"), [this](Slide &s) {
        int l = layout->currentIndex();
        if (s.background == this->doc->deck().layouts[s.layout].background) s.background = this->doc->deck().layouts[l].background;
        s.layout = l;
    });
    connect(background, &ColorButton::picked, this, [this](QColor c) { editSlide(tr("Background"), [c](Slide &s) { s.background = Fill::solid(c); }); });
    onSlide(hidden, &QCheckBox::toggled, tr("Hide Slide"), [this](Slide &s) { s.hidden = hidden->isChecked(); });
    onSlide(transition, &QComboBox::activated, tr("Transition"), [this](Slide &s) {
        s.transition.type = transition->currentData().toString();
        if (s.transition.ms <= 0) s.transition.ms = 700;
    });
    onSlide(transitionTime, &QDoubleSpinBox::valueChanged, tr("Transition Duration"), [this](Slide &s) { s.transition.ms = int(transitionTime->value() * 1000); }, 10);
    auto setAdvance = [this](Slide &s) { s.transition.after = advanceOn->isChecked() ? int(advance->value() * 1000) : -1; };
    onSlide(advanceOn, &QCheckBox::toggled, tr("Advance"), setAdvance);
    onSlide(advance, &QDoubleSpinBox::valueChanged, tr("Advance"), setAdvance, 11);

    auto onElement = [this](auto *widget, auto signal, const QString &text, auto fn, int merge = 0) {
        connect(widget, signal, this, [=, this] {
            if (!updating) editElement(text, fn, merge);
        });
    };
    onElement(x, &QDoubleSpinBox::valueChanged, tr("Move"), [this](Element &e) { e.rect.moveLeft(x->value()); }, 20);
    onElement(y, &QDoubleSpinBox::valueChanged, tr("Move"), [this](Element &e) { e.rect.moveTop(y->value()); }, 20);
    onElement(w, &QDoubleSpinBox::valueChanged, tr("Resize"), [this](Element &e) {
        e.rect.setWidth(w->value());
        if (auto t = e.as<TableEl>()) ops::scaleTable(*t, e.rect.size());
        ops::fitText(e);
    }, 21);
    onElement(h, &QDoubleSpinBox::valueChanged, tr("Resize"), [this](Element &e) {
        e.rect.setHeight(h->value());
        if (auto t = e.as<TableEl>()) ops::scaleTable(*t, e.rect.size());
        ops::fitText(e);
    }, 21);
    onElement(rotation, &QDoubleSpinBox::valueChanged, tr("Rotate"), [this](Element &e) { e.rotation = std::fmod(rotation->value() + 360, 360); }, 22);
    connect(fill, &ColorButton::picked, this, [this](QColor c) {
        editElement(tr("Fill"), [c](Element &e) {
            if (auto s = e.as<ShapeEl>()) s->fill = c.isValid() ? Fill::solid(c) : Fill{};
        });
    });
    connect(line, &ColorButton::picked, this, [this](QColor c) {
        editElement(tr("Line"), [c](Element &e) {
            Line *l = e.as<ShapeEl>() ? &e.as<ShapeEl>()->line : e.as<PictureEl>() ? &e.as<PictureEl>()->line : nullptr;
            if (l) l->fill = c.isValid() ? Fill::solid(c) : Fill{};
        });
    });
    auto lineOf = [](Element &e) -> Line * {
        return e.as<ShapeEl>() ? &e.as<ShapeEl>()->line : e.as<PictureEl>() ? &e.as<PictureEl>()->line : nullptr;
    };
    onElement(lineWidth, &QDoubleSpinBox::valueChanged, tr("Line Width"), [this, lineOf](Element &e) {
        if (Line *l = lineOf(e)) l->width = lineWidth->value();
    }, 23);
    onElement(dash, &QComboBox::activated, tr("Dash"), [this, lineOf](Element &e) {
        if (Line *l = lineOf(e)) l->dash = dash->currentData().toString();
    });
    auto text = [](Element &e, auto fn) {
        if (TextBody *t = e.text()) {
            fn(*t);
            ops::fitText(e);
        }
    };
    onElement(anchor, &QComboBox::activated, tr("Text Alignment"), [this, text](Element &e) {
        text(e, [this](TextBody &t) { t.anchor = TextBody::Anchor(anchor->currentData().toInt()); });
    });
    onElement(autofit, &QComboBox::activated, tr("Autofit"), [this, text](Element &e) {
        text(e, [this](TextBody &t) {
            t.autofit = TextBody::Autofit(autofit->currentData().toInt());
            t.fontScale = 1;
            t.lineReduction = 0;
        });
    });
    onElement(wrap, &QCheckBox::toggled, tr("Wrap Text"), [this, text](Element &e) {
        text(e, [this](TextBody &t) { t.wrap = wrap->isChecked(); });
    });
    connect(effect, &QComboBox::activated, this, &Inspector::setBuild);
    connect(trigger, &QComboBox::activated, this, &Inspector::setBuild);
    connect(duration, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!updating) setBuild();
    });
    connect(byParagraph, &QCheckBox::toggled, this, [this] {
        if (!updating) setBuild();
    });

    connect(doc, &Document::changed, this, &Inspector::refresh);
    connect(canvas, &Canvas::selectionChanged, this, &Inspector::refresh);
    refresh();
}

void Inspector::editElement(const QString &text, const std::function<void(Element &)> &fn, int mergeId)
{
    QList<int> ids = canvas->selection();
    if (ids.isEmpty()) return;
    int slide = canvas->slideIndex();
    doc->change(text, [&](Deck &d) {
        for (Element &e : d.slides[slide].elements)
            if (ids.contains(e.id)) fn(e);
    }, mergeId);
}

void Inspector::editSlide(const QString &text, const std::function<void(Slide &)> &fn, int mergeId)
{
    int slide = canvas->slideIndex();
    doc->change(text, [&](Deck &d) { fn(d.slides[slide]); }, mergeId);
}

void Inspector::setBuild()
{
    if (updating || canvas->selection().size() != 1) return;
    int id = canvas->selection().first(), preset = effect->currentData().toInt();
    QString start = trigger->currentData().toString();
    int ms = preset == 1 ? 0 : int(duration->value() * 1000);
    bool paragraphs = byParagraph->isChecked();
    editSlide(tr("Animation"), [&](Slide &s) {
        qsizetype at = -1;
        for (qsizetype i = 0; i < s.builds.size(); ++i)
            if (s.builds[i].element == id && s.builds[i].kind == "entr" && at < 0) at = i;
        s.builds.removeIf([id](const Build &b) { return b.element == id && b.kind == "entr"; });
        if (preset == 0) return;
        if (at < 0) at = s.builds.size();
        const Element &e = s.elements[s.indexOf(id)];
        QList<Build> added;
        if (paragraphs && e.text() && e.text()->paragraphs.size() > 1) {
            for (int p = 0; p < e.text()->paragraphs.size(); ++p)
                if (!e.text()->paragraphs[p].text().isEmpty())
                    added << Build{id, p, "entr", preset, 0, added.isEmpty() ? start : "click", ms, 0};
        } else {
            added << Build{id, -1, "entr", preset, 0, start, ms, 0};
        }
        for (const Build &b : added) s.builds.insert(at++, b);
    });
}

void Inspector::refresh()
{
    QSignalBlocker blockers[] = {QSignalBlocker(layout), QSignalBlocker(hidden), QSignalBlocker(transition),
                                 QSignalBlocker(transitionTime), QSignalBlocker(advanceOn), QSignalBlocker(advance),
                                 QSignalBlocker(x), QSignalBlocker(y), QSignalBlocker(w), QSignalBlocker(h),
                                 QSignalBlocker(rotation), QSignalBlocker(lineWidth), QSignalBlocker(dash),
                                 QSignalBlocker(anchor), QSignalBlocker(autofit), QSignalBlocker(wrap),
                                 QSignalBlocker(effect), QSignalBlocker(trigger), QSignalBlocker(duration),
                                 QSignalBlocker(byParagraph)};
    updating = true;
    const Deck &deck = doc->deck();
    if (canvas->slideIndex() >= deck.slides.size()) return void(updating = false);
    const Slide &s = deck.slides[canvas->slideIndex()];
    QList<QColor> themeColors;
    for (QString name : {"accent1", "accent2", "accent3", "accent4", "accent5", "accent6", "tx2", "bg2"})
        themeColors << deck.theme.colors.value(name);
    for (ColorButton *b : {background, fill, line}) b->setThemeColors(themeColors);

    layout->clear();
    for (const Layout &l : deck.layouts) layout->addItem(l.name);
    layout->setCurrentIndex(s.layout);
    background->setColor(s.background.kind == Fill::Solid ? s.background.color : QColor(Qt::white));
    hidden->setChecked(s.hidden);
    setCombo(transition, s.transition.type, s.transition.type);
    transitionTime->setValue(std::max(s.transition.ms, 100) / 1000.0);
    advanceOn->setChecked(s.transition.after >= 0);
    advance->setValue(std::max(s.transition.after, 0) / 1000.0);

    QList<int> ids = canvas->selection();
    const Element *e = ids.size() == 1 ? &s.elements[s.indexOf(ids.first())] : nullptr;
    slideBox->setVisible(ids.isEmpty());
    arrangeBox->setVisible(e);
    const Line *ln = e ? (e->as<ShapeEl>() ? &e->as<ShapeEl>()->line : e->as<PictureEl>() ? &e->as<PictureEl>()->line : nullptr) : nullptr;
    shapeBox->setVisible(ln);
    textBox->setVisible(e && e->as<ShapeEl>() && !e->as<ShapeEl>()->connector);
    animBox->setVisible(e);
    if (e) {
        x->setValue(e->rect.x());
        y->setValue(e->rect.y());
        w->setValue(e->rect.width());
        h->setValue(e->rect.height());
        rotation->setValue(e->rotation);
        if (auto sh = e->as<ShapeEl>()) {
            fill->setEnabled(true);
            fill->setColor(sh->fill.kind == Fill::Solid ? sh->fill.color : QColor());
            anchor->setCurrentIndex(anchor->findData(sh->text.anchor));
            autofit->setCurrentIndex(autofit->findData(sh->text.autofit));
            wrap->setChecked(sh->text.wrap);
        } else {
            fill->setEnabled(false);
        }
        if (ln) {
            line->setColor(ln->fill.kind == Fill::Solid ? ln->fill.color : QColor());
            lineWidth->setValue(ln->width);
            setCombo(dash, ln->dash, ln->dash);
        }
        const Build *b = nullptr;
        int paragraphBuilds = 0;
        for (const Build &x : s.builds)
            if (x.element == e->id && x.kind == "entr") {
                if (!b) b = &x;
                paragraphBuilds += x.paragraph >= 0;
            }
        setCombo(effect, b ? b->preset : 0, b ? tr("Effect %1").arg(b->preset) : QString());
        setCombo(trigger, b ? b->trigger : QString("click"));
        duration->setValue(b ? std::max(b->ms, 100) / 1000.0 : 0.5);
        byParagraph->setChecked(paragraphBuilds > 0);
        for (QWidget *wgt : std::initializer_list<QWidget *>{trigger, duration, byParagraph}) wgt->setEnabled(b);
    }
    updating = false;
}
