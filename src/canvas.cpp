#include "canvas.h"
#include "geometry.h"
#include "render.h"
#include "richtext.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QUuid>
#include <QtMath>

namespace {

constexpr int handleSize = 8;
const char *clipboardMime = "application/x-podium-elements";

struct Clipboard {
    QList<Element> elements;
    std::shared_ptr<Package> package;
    QByteArray token;
};

Clipboard &clipboard()
{
    static Clipboard c;
    return c;
}

Element *byId(Slide &s, int id)
{
    for (Element &e : s.elements)
        if (e.id == id) return &e;
    return nullptr;
}

const Element *byId(const Slide &s, int id) { return byId(const_cast<Slide &>(s), id); }

bool isLine(const Element &e)
{
    auto s = e.as<ShapeEl>();
    return s && (s->connector || geometry::isLine(s->preset));
}

bool textual(const Element &e) { return e.as<ShapeEl>() || e.as<TableEl>(); }

QRectF bounds(const Element &e) { return elementTransform(e).mapRect(QRectF(QPointF(), e.rect.size())); }

QPointF lineStart(const Element &e)
{
    return {e.flipH ? e.rect.width() : 0, e.flipV ? e.rect.height() : 0};
}

QPointF lineEnd(const Element &e)
{
    return {e.flipH ? 0 : e.rect.width(), e.flipV ? 0 : e.rect.height()};
}

TextBody &textOf(Element &e, int row, int col)
{
    if (row >= 0) return e.as<TableEl>()->cells[row][col].text;
    return *e.text();
}

// Applies `fn` to the character styles in the cursor's selection, fragment by fragment.
void mergeEach(QTextCursor sel, double scale, const std::function<void(CharStyle &)> &fn)
{
    QTextDocument *doc = sel.document();
    int a = sel.selectionStart(), b = sel.selectionEnd();
    for (QTextBlock blk = doc->findBlock(a); blk.isValid() && blk.position() <= b; blk = blk.next()) {
        for (auto it = blk.begin(); !it.atEnd(); ++it) {
            QTextFragment f = it.fragment();
            int s = std::max(a, f.position()), e = std::min(b, f.position() + f.length());
            if (s >= e) continue;
            QTextCursor c(doc);
            c.setPosition(s);
            c.setPosition(e, QTextCursor::KeepAnchor);
            CharStyle style = richtext::charStyle(f.charFormat(), scale);
            fn(style);
            QTextCharFormat format = richtext::charFormat(style, scale);
            if (f.charFormat().hasProperty(richtext::FieldProperty))
                format.setProperty(richtext::FieldProperty, f.charFormat().property(richtext::FieldProperty));
            c.setCharFormat(format);
        }
    }
}

} // namespace

// --- TextEditor ---

void TextEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) return emit escape();
    QGraphicsTextItem::keyPressEvent(event);
}

bool TextEditor::sceneEvent(QEvent *event)
{
    // Graphics items hand Tab to focus navigation before keyPressEvent; we use it for cells and list levels.
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            emit tab(key->key() == Qt::Key_Backtab || key->modifiers() & Qt::ShiftModifier);
            return true;
        }
    }
    return QGraphicsTextItem::sceneEvent(event);
}

void TextEditor::paint(QPainter *p, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    QGraphicsTextItem::paint(p, option, widget);
    richtext::drawBullets(*p, *document());
}

// --- Canvas ---

Canvas::Canvas(Document *doc, QWidget *parent) : QGraphicsView(parent), doc(doc)
{
    setScene(new QGraphicsScene(this));
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    setViewportUpdateMode(FullViewportUpdate);
    setTransformationAnchor(AnchorUnderMouse);
    setFrameShape(NoFrame);
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(doc, &Document::changed, this, &Canvas::sync);
    sync();
}

const Slide &Canvas::slide() const { return deck().slides[index]; }

const Slide &Canvas::shown() const
{
    bool live = drag.mode == Drag::Move || drag.mode == Drag::Resize || drag.mode == Drag::Rotate;
    return live ? drag.preview : slide();
}

void Canvas::sync()
{
    QSizeF size = deck().size;
    double m = size.width() * 0.04;
    scene()->setSceneRect(QRectF(-m, -m, size.width() + 2 * m, size.height() + 2 * m));
    index = std::clamp(index, 0, int(deck().slides.size()) - 1);
    if (editor && !byId(slide(), edit.id)) {
        delete std::exchange(editor, nullptr);
        edit = {};
    }
    QList<int> kept;
    for (int id : selected)
        if (byId(slide(), id)) kept << id;
    if (kept != selected) {
        selected = kept;
        emit selectionChanged();
    }
    if (fitting) setZoom(0);
    viewport()->update();
}

void Canvas::setSlideIndex(int i)
{
    if (i == index || i < 0 || i >= deck().slides.size()) return;
    stopEditing();
    index = i;
    drag = {};
    selected.clear();
    emit selectionChanged();
    viewport()->update();
}

void Canvas::setSelection(const QList<int> &ids)
{
    if (ids == selected) return;
    selected = ids;
    emit selectionChanged();
    emit formatChanged();
    viewport()->update();
}

void Canvas::selectAll()
{
    QList<int> ids;
    for (const Element &e : slide().elements) ids << e.id;
    setSelection(ids);
}

double Canvas::zoom() const { return transform().m11(); }

void Canvas::setZoom(double z)
{
    fitting = z <= 0;
    Qt::ScrollBarPolicy bars = fitting ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
    setHorizontalScrollBarPolicy(bars);
    setVerticalScrollBarPolicy(bars);
    if (fitting) {
        QSizeF size = deck().size;
        double m = size.width() * 0.03;
        fitInView(QRectF(-m, -m, size.width() + 2 * m, size.height() + 2 * m), Qt::KeepAspectRatio);
    } else {
        setTransform(QTransform::fromScale(std::clamp(z, 0.1, 8.0), std::clamp(z, 0.1, 8.0)));
    }
    emit zoomChanged(zoom());
    if (editor) layoutEditor();
}

// --- painting ---

void Canvas::drawBackground(QPainter *p, const QRectF &rect)
{
    p->fillRect(rect, palette().color(QPalette::Window).darker(115));
    QRectF page(QPointF(), deck().size);
    p->fillRect(page.translated(0, 2 / zoom()), QColor(0, 0, 0, 70));
    RenderOptions o;
    o.slideNumber = index + 1;
    o.editing = true;
    Slide s = shown();
    if (editor && edit.row < 0) o.skipText = edit.id;
    if (editor && edit.row >= 0)
        if (Element *e = byId(s, edit.id)) textOf(*e, edit.row, edit.col).paragraphs.clear();
    paintSlide(*p, deck(), s, o);
}

void Canvas::drawForeground(QPainter *p, const QRectF &)
{
    const Slide &s = shown();
    QColor accent = palette().color(QPalette::Highlight);
    p->save();
    QPen outline(accent, 1.5);
    outline.setCosmetic(true);
    p->setPen(outline);
    p->setBrush(Qt::NoBrush);
    for (int id : selected)
        if (const Element *e = byId(s, id)) {
            if (isLine(*e)) continue;
            p->drawPolygon(elementTransform(*e).map(QRectF(QPointF(), e->rect.size())));
        }
    if (editor && edit.row >= 0)
        if (const Element *e = byId(s, edit.id))
            p->drawPolygon(elementTransform(*e).map(tableGeometry(*e->as<TableEl>()).cell(*e->as<TableEl>(), edit.row, edit.col)));

    QPen guide(QColor(255, 64, 160), 1, Qt::DashLine);
    guide.setCosmetic(true);
    p->setPen(guide);
    for (const QLineF &l : drag.guides) p->drawLine(l);
    if (drag.mode == Drag::Rubber && !drag.rubber.isEmpty()) {
        p->setPen(outline);
        QColor fill = accent;
        fill.setAlpha(40);
        p->setBrush(fill);
        p->drawRect(drag.rubber);
    }
    if (drag.mode == Drag::Insert && !drag.insert.isNull()) {
        p->setPen(outline);
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(drag.insert.p1(), drag.insert.p2()).normalized());
    }

    p->resetTransform();
    for (auto [pos, kind] : handles()) {
        QPointF v = mapFromScene(pos);
        p->setPen(QPen(accent, 1.5));
        p->setBrush(Qt::white);
        QRectF r(v - QPointF(handleSize / 2.0, handleSize / 2.0), QSizeF(handleSize, handleSize));
        if (kind == 8) {
            const Element *e = byId(s, selected.first());
            p->drawLine(v, mapFromScene(elementTransform(*e).map(QPointF(e->rect.width() / 2, 0))));
            p->drawEllipse(r.adjusted(-1, -1, 1, 1));
        } else if (kind >= 9) {
            p->drawEllipse(r);
        } else {
            p->drawRect(r);
        }
    }
    p->restore();
}

QList<std::pair<QPointF, int>> Canvas::handles() const
{
    QList<std::pair<QPointF, int>> out;
    if (selected.size() != 1 || editor || drag.mode == Drag::Rubber) return out;
    const Element *e = byId(shown(), selected.first());
    if (!e) return out;
    QTransform t = elementTransform(*e);
    if (isLine(*e)) return {{t.map(lineStart(*e)), 9}, {t.map(lineEnd(*e)), 10}};
    double w = e->rect.width(), h = e->rect.height();
    QPointF pts[] = {{0, 0}, {w / 2, 0}, {w, 0}, {w, h / 2}, {w, h}, {w / 2, h}, {0, h}, {0, h / 2}};
    for (int i = 0; i < 8; ++i) out << std::pair{t.map(pts[i]), i};
    if (!e->as<TableEl>()) out << std::pair{t.map(QPointF(w / 2, -24 / zoom())), 8};
    return out;
}

int Canvas::handleAt(QPointF viewPos) const
{
    for (auto [pos, kind] : handles())
        if (QLineF(mapFromScene(pos), viewPos).length() <= handleSize) return kind;
    return -1;
}

int Canvas::elementAt(QPointF pos) const
{
    const Slide &s = slide();
    double tol = 4 / zoom();
    for (qsizetype i = s.elements.size() - 1; i >= 0; --i) {
        const Element &e = s.elements[i];
        if (e.hidden) continue;
        QPointF local = elementTransform(e).inverted().map(pos);
        if (QRectF(QPointF(), e.rect.size()).adjusted(-tol, -tol, tol, tol).contains(local)) return e.id;
    }
    return -1;
}

void Canvas::updateCursor(QPointF viewPos, QPointF scenePos)
{
    if (inserting) return setCursor(Qt::CrossCursor);
    int h = handleAt(viewPos);
    static const Qt::CursorShape shapes[] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                             Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
    if (h >= 0 && h < 8) return setCursor(shapes[h]);
    if (h >= 8) return setCursor(Qt::PointingHandCursor);
    int hit = elementAt(scenePos);
    if (hit < 0) return unsetCursor();
    const Element *e = byId(slide(), hit);
    setCursor(selected == QList{hit} && textual(*e) ? Qt::IBeamCursor : Qt::SizeAllCursor);
}

// --- mouse ---

void Canvas::mousePressEvent(QMouseEvent *event)
{
    QPointF pos = mapToScene(event->position().toPoint());
    if (editor) {
        const Element *e = byId(slide(), edit.id);
        QRectF area = edit.row >= 0 ? tableGeometry(*e->as<TableEl>()).cell(*e->as<TableEl>(), edit.row, edit.col)
                                    : QRectF(QPointF(), e->rect.size());
        if (area.contains(elementTransform(*e).inverted().map(pos))) {
            QGraphicsView::mousePressEvent(event);
            return;
        }
        stopEditing();
    }
    setFocus();
    drag = {};
    drag.start = pos;
    drag.pressView = event->position();
    drag.original = drag.preview = slide();
    if (inserting && event->button() == Qt::LeftButton) {
        drag.mode = Drag::Insert;
        drag.insert = QLineF(pos, pos);
        return;
    }
    int hit = elementAt(pos);
    if (event->button() == Qt::RightButton) {
        if (hit < 0) setSelection({});
        else if (!selected.contains(hit)) setSelection({hit});
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (int h = handleAt(event->position()); h >= 0) {
        drag.mode = h == 8 ? Drag::Rotate : Drag::Resize;
        drag.handle = h;
        return;
    }
    if (hit < 0) {
        if (!(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) setSelection({});
        drag.mode = Drag::Rubber;
        return;
    }
    if (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) {
        QList<int> ids = selected;
        if (ids.contains(hit)) ids.removeAll(hit);
        else ids << hit;
        setSelection(ids);
        return;
    }
    const Element *e = byId(slide(), hit);
    if (e->as<TableEl>()) tableCell = cellAt(*e, pos);
    bool emptyPlaceholder = e->ph && e->text() && e->text()->isEmpty();
    drag.editOnRelease = textual(*e) && (selected == QList{hit} || emptyPlaceholder);
    if (!selected.contains(hit)) setSelection({hit});
    drag.mode = Drag::Pending;
}

void Canvas::mouseMoveEvent(QMouseEvent *event)
{
    QPointF pos = mapToScene(event->position().toPoint());
    if (drag.mode == Drag::None) {
        if (editor) QGraphicsView::mouseMoveEvent(event);
        else updateCursor(event->position(), pos);
        return;
    }
    switch (drag.mode) {
    case Drag::Pending:
        if ((event->position() - drag.pressView).manhattanLength() < QApplication::startDragDistance()) return;
        drag.mode = Drag::Move;
        [[fallthrough]];
    case Drag::Move:
        moveTo(pos, event->modifiers());
        break;
    case Drag::Resize:
        resizeTo(pos, event->modifiers());
        break;
    case Drag::Rotate:
        rotateTo(pos, event->modifiers());
        break;
    case Drag::Rubber: {
        drag.rubber = QRectF(drag.start, pos).normalized();
        QList<int> ids;
        for (const Element &e : slide().elements)
            if (!e.hidden && drag.rubber.contains(bounds(e))) ids << e.id;
        setSelection(ids);
        break;
    }
    case Drag::Insert:
        drag.insert.setP2(pos);
        break;
    default:
        break;
    }
    viewport()->update();
}

void Canvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (editor && drag.mode == Drag::None) {
        QGraphicsView::mouseReleaseEvent(event);
        emit formatChanged();
        return;
    }
    Drag d = std::exchange(drag, {});
    if (d.mode == Drag::Insert) {
        QLineF line(d.insert.p1(), mapToScene(event->position().toPoint()));
        if (line.length() < 4 / zoom()) line = QLineF(line.p1() - QPointF(72, 72), line.p1() + QPointF(72, 72));
        Element e = std::exchange(inserting, nullptr)(line);
        unsetCursor();
        insert(e);
        if (textual(e) && e.as<ShapeEl>() && e.as<ShapeEl>()->textBox) beginEdit(e.id);
    } else if ((d.mode == Drag::Move || d.mode == Drag::Resize || d.mode == Drag::Rotate) && d.preview != d.original) {
        QString text = d.mode == Drag::Move ? tr("Move") : d.mode == Drag::Resize ? tr("Resize") : tr("Rotate");
        Slide preview = d.preview;
        editSlide(text, [&](Slide &s) { s = preview; });
    } else if (d.mode == Drag::Pending && d.editOnRelease && selected.size() == 1) {
        beginEditAt(selected.first(), d.start);
    }
    viewport()->update();
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    QPointF pos = mapToScene(event->position().toPoint());
    if (editor) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }
    int hit = elementAt(pos);
    if (hit < 0) return;
    const Element *e = byId(slide(), hit);
    if (!textual(*e)) return;
    setSelection({hit});
    beginEditAt(hit, pos);
    if (editor) {
        QTextCursor c = editor->textCursor();
        c.select(QTextCursor::WordUnderCursor);
        editor->setTextCursor(c);
    }
}

void Canvas::moveTo(QPointF pos, Qt::KeyboardModifiers mods)
{
    QPointF delta = pos - drag.start;
    if (mods & Qt::ShiftModifier) (std::abs(delta.x()) > std::abs(delta.y()) ? delta.ry() : delta.rx()) = 0;
    QRectF box;
    for (int id : selected)
        if (const Element *e = byId(drag.original, id)) box |= bounds(*e);
    drag.guides.clear();
    if (!(mods & Qt::AltModifier)) delta += snap(box.translated(delta));
    drag.preview = drag.original;
    for (int id : selected)
        if (Element *e = byId(drag.preview, id)) e->rect.translate(delta);
}

QPointF Canvas::snap(QRectF box)
{
    double tol = 6 / zoom();
    QSizeF size = deck().size;
    QList<double> xs{0, size.width() / 2, size.width()}, ys{0, size.height() / 2, size.height()};
    for (const Element &e : drag.original.elements) {
        if (selected.contains(e.id) || e.hidden) continue;
        QRectF b = bounds(e);
        if (b.width() >= size.width() * 0.98 && b.height() >= size.height() * 0.98) continue;
        xs << b.left() << b.center().x() << b.right();
        ys << b.top() << b.center().y() << b.bottom();
    }
    auto best = [tol](const QList<double> &targets, std::initializer_list<double> values) {
        std::optional<std::pair<double, double>> found; // offset, guide position
        for (double v : values)
            for (double t : targets)
                if (std::abs(t - v) < (found ? std::abs(found->first) : tol)) found = std::pair{t - v, t};
        return found;
    };
    QRectF area = sceneRect();
    double dx = 0, dy = 0;
    if (auto x = best(xs, {box.left(), box.center().x(), box.right()})) {
        dx = x->first;
        drag.guides << QLineF(x->second, area.top(), x->second, area.bottom());
    }
    if (auto y = best(ys, {box.top(), box.center().y(), box.bottom()})) {
        dy = y->first;
        drag.guides << QLineF(area.left(), y->second, area.right(), y->second);
    }
    return {dx, dy};
}

void Canvas::resizeTo(QPointF pos, Qt::KeyboardModifiers mods)
{
    int id = selected.first();
    const Element &o = *byId(drag.original, id);
    Element &e = *byId(drag.preview, id);
    QTransform t = elementTransform(o);
    if (drag.handle >= 9) {
        QPointF start = t.map(lineStart(o)), end = t.map(lineEnd(o));
        QPointF &moving = drag.handle == 9 ? start : end, fixed = drag.handle == 9 ? end : start;
        moving = pos;
        if (mods & Qt::ShiftModifier) {
            QLineF l(fixed, pos);
            l.setAngle(std::round(l.angle() / 45) * 45);
            moving = l.p2();
        }
        e.rotation = 0;
        e.flipH = start.x() > end.x();
        e.flipV = start.y() > end.y();
        e.rect = QRectF(start, end).normalized();
        return;
    }
    QPointF p = t.inverted().map(pos);
    double w = o.rect.width(), h = o.rect.height(), l = 0, top = 0, r = w, b = h;
    int k = drag.handle;
    if (k == 0 || k == 6 || k == 7) l = p.x();
    if (k == 2 || k == 3 || k == 4) r = p.x();
    if (k == 0 || k == 1 || k == 2) top = p.y();
    if (k == 4 || k == 5 || k == 6) b = p.y();
    bool keep = k % 2 == 0 && (bool(o.as<PictureEl>()) != bool(mods & Qt::ShiftModifier));
    if (keep && w > 0 && h > 0) {
        double s = std::max(std::abs(r - l) / w, std::abs(b - top) / h);
        if (k == 0 || k == 6) l = w - w * s;
        else r = w * s;
        if (k == 0 || k == 2) top = h - h * s;
        else b = h * s;
    }
    QRectF nr = QRectF(QPointF(l, top), QPointF(r, b)).normalized();
    nr.setSize(nr.size().expandedTo(QSizeF(1, 1)));
    QPointF c = t.map(nr.center());
    e.rect = QRectF(c - QPointF(nr.width() / 2, nr.height() / 2), nr.size());
    if (auto table = e.as<TableEl>()) ops::scaleTable(*table, e.rect.size());
    ops::fitText(e);
}

void Canvas::rotateTo(QPointF pos, Qt::KeyboardModifiers mods)
{
    const Element &o = *byId(drag.original, selected.first());
    QPointF c = o.rect.center();
    double angle = qRadiansToDegrees(std::atan2(pos.y() - c.y(), pos.x() - c.x())) + 90;
    if (mods & Qt::ShiftModifier) angle = std::round(angle / 15) * 15;
    byId(drag.preview, o.id)->rotation = std::fmod(angle + 720, 360);
}

// --- keyboard, wheel, menus, drops ---

void Canvas::keyPressEvent(QKeyEvent *event)
{
    if (editor) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    double step = event->modifiers() & Qt::ShiftModifier ? 10 : 1;
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        return removeSelection();
    case Qt::Key_Escape:
        inserting = nullptr;
        unsetCursor();
        return setSelection({});
    case Qt::Key_Left:
        return nudge({-step, 0});
    case Qt::Key_Right:
        return nudge({step, 0});
    case Qt::Key_Up:
        return nudge({0, -step});
    case Qt::Key_Down:
        return nudge({0, step});
    case Qt::Key_Tab:
    case Qt::Key_Backtab: {
        const QList<Element> &all = slide().elements;
        if (all.isEmpty()) return;
        int i = selected.isEmpty() ? -1 : slide().indexOf(selected.first());
        i = (i + (event->key() == Qt::Key_Tab ? 1 : int(all.size()) - 1)) % int(all.size());
        return setSelection({all[i].id});
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_F2:
        if (selected.size() == 1 && textual(*byId(slide(), selected.first()))) beginEdit(selected.first());
        return;
    default:
        break;
    }
    QString text = event->text();
    if (selected.size() == 1 && !text.isEmpty() && text.at(0).isPrint() && !(event->modifiers() & Qt::ControlModifier)) {
        const Element *e = byId(slide(), selected.first());
        if (e->as<ShapeEl>()) {
            beginEdit(e->id);
            QTextCursor c = editor->textCursor();
            c.select(QTextCursor::Document);
            editor->setTextCursor(c);
            QGraphicsView::keyPressEvent(event);
            return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

void Canvas::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        setZoom(zoom() * (event->angleDelta().y() > 0 ? 1.15 : 1 / 1.15));
        return;
    }
    if (fitting && !editor) {
        static int accumulated = 0;
        accumulated += event->angleDelta().y();
        if (std::abs(accumulated) >= 120) {
            emit stepSlide(accumulated > 0 ? -1 : 1);
            accumulated = 0;
        }
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void Canvas::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (fitting) setZoom(0);
}

void Canvas::contextMenuEvent(QContextMenuEvent *event)
{
    if (editor) {
        QGraphicsView::contextMenuEvent(event);
        return;
    }
    QMenu menu(this);
    menu.addActions(contextActions);
    if (tableSelected()) {
        menu.addSeparator();
        menu.addAction(tr("Insert Row Above"), this, [this] { tableInsert(true, false); });
        menu.addAction(tr("Insert Row Below"), this, [this] { tableInsert(true, true); });
        menu.addAction(tr("Insert Column Left"), this, [this] { tableInsert(false, false); });
        menu.addAction(tr("Insert Column Right"), this, [this] { tableInsert(false, true); });
        menu.addAction(tr("Delete Row"), this, [this] { tableRemove(true); });
        menu.addAction(tr("Delete Column"), this, [this] { tableRemove(false); });
    }
    menu.exec(event->globalPos());
}

void Canvas::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) event->acceptProposedAction();
}

void Canvas::dragMoveEvent(QDragMoveEvent *event) { event->acceptProposedAction(); }

void Canvas::dropEvent(QDropEvent *event)
{
    QPointF pos = mapToScene(event->position().toPoint());
    for (const QUrl &url : event->mimeData()->urls())
        if (url.isLocalFile()) insertImageFile(url.toLocalFile(), pos);
    event->acceptProposedAction();
}

// --- text editing ---

QPoint Canvas::cellAt(const Element &e, QPointF scenePos) const
{
    const TableEl &t = *e.as<TableEl>();
    TableGeometry g = tableGeometry(t);
    QPointF p = elementTransform(e).inverted().map(scenePos);
    for (int r = 0; r < g.rows() && r < t.cells.size(); ++r)
        for (int c = 0; c < g.cols() && c < t.cells[r].size(); ++c)
            if (!t.cells[r][c].merged && g.cell(t, r, c).contains(p)) return {c, r};
    return {0, 0};
}

TextBody Canvas::editBody() const
{
    Element e = *byId(slide(), edit.id);
    return textOf(e, edit.row, edit.col);
}

void Canvas::beginEditAt(int id, QPointF scenePos)
{
    const Element *e = byId(slide(), id);
    if (!e) return;
    if (e->as<TableEl>()) {
        QPoint cell = cellAt(*e, scenePos);
        beginEdit(id, cell.y(), cell.x());
    } else {
        beginEdit(id);
    }
    if (!editor) return;
    QPointF local = editor->mapFromScene(scenePos);
    int pos = editor->document()->documentLayout()->hitTest(local, Qt::FuzzyHit);
    if (pos >= 0) {
        QTextCursor c = editor->textCursor();
        c.setPosition(pos);
        editor->setTextCursor(c);
    }
}

void Canvas::beginEdit(int id, int row, int col)
{
    stopEditing();
    const Element *e = byId(slide(), id);
    if (!e || !textual(*e)) return;
    if (e->as<TableEl>() && row < 0) row = tableCell.y(), col = tableCell.x();
    edit = {id, row, col};
    TextBody body = editBody();
    if (body.paragraphs.isEmpty()) body.paragraphs = {Paragraph{body.levels[0].para, {}, body.levels[0].chr}};

    editor = new TextEditor;
    QTextDocument *text = richtext::document(body).release();
    text->setParent(editor);
    text->setUndoRedoEnabled(true);
    editor->setDocument(text);
    editor->setTextInteractionFlags(Qt::TextEditorInteraction);
    scene()->addItem(editor);
    QTextCursor c(text);
    c.movePosition(QTextCursor::End);
    editor->setTextCursor(c);
    connect(text, &QTextDocument::contentsChanged, this, &Canvas::layoutEditor);
    connect(text, &QTextDocument::cursorPositionChanged, this, &Canvas::formatChanged);
    connect(editor, &TextEditor::escape, this, &Canvas::stopEditing);
    connect(editor, &TextEditor::tab, this, [this](bool back) {
        const Element *e = byId(slide(), edit.id);
        if (auto t = e ? e->as<TableEl>() : nullptr) {
            int n = int(t->cols.size()), i = edit.row * n + edit.col + (back ? -1 : 1);
            int count = int(t->rows.size()) * n;
            i = (i + count) % count;
            int id = edit.id;
            stopEditing();
            beginEdit(id, i / n, i % n);
            if (editor) {
                QTextCursor c = editor->textCursor();
                c.select(QTextCursor::Document);
                editor->setTextCursor(c);
            }
            return;
        }
        QTextCursor c = editor->textCursor();
        bool atStart = c.position() == c.block().position() || c.hasSelection();
        if (atStart) {
            ops::ParaEdit p;
            p.level = back ? -1 : 1;
            applyPara(p);
        } else if (!back) {
            c.insertText("\t");
        }
    });
    layoutEditor();
    setFocus();
    editor->setFocus();
    emit formatChanged();
    viewport()->update();
}

void Canvas::layoutEditor()
{
    if (!editor) return;
    const Element *e = byId(slide(), edit.id);
    if (!e) return;
    TextBody body = editBody();
    QRectF box(QPointF(), e->rect.size());
    if (edit.row >= 0) box = tableGeometry(*e->as<TableEl>()).cell(*e->as<TableEl>(), edit.row, edit.col);
    QTransform t = richtext::place(*editor->document(), body, box.size());
    t *= QTransform::fromTranslate(box.left(), box.top());
    if (e->flipV) t *= QTransform(-1, 0, 0, -1, e->rect.width(), e->rect.height());
    editor->setTransform(t * elementTransform(*e));
    editor->update();
}

void Canvas::stopEditing()
{
    if (!editor) return;
    EditTarget target = std::exchange(edit, {});
    TextEditor *item = std::exchange(editor, nullptr);
    double scale = 1;
    if (const Element *e = byId(slide(), target.id)) {
        Element copy = *e;
        scale = textOf(copy, target.row, target.col).fontScale;
    }
    QList<Paragraph> paragraphs = richtext::paragraphs(*item->document(), scale);
    item->deleteLater();
    scene()->removeItem(item);
    const Element *e = byId(slide(), target.id);
    if (e) {
        Element copy = *e;
        if (textOf(copy, target.row, target.col).paragraphs != paragraphs)
            editSlide(tr("Edit Text"), [&](Slide &s) {
                Element *el = byId(s, target.id);
                TextBody &body = textOf(*el, target.row, target.col);
                body.paragraphs = paragraphs;
                if (target.row < 0) ops::fitText(*el);
            });
    }
    emit formatChanged();
    viewport()->update();
}

// --- formatting ---

void Canvas::applyChar(const ops::CharEdit &edit)
{
    if (editor) {
        double scale = editBody().fontScale;
        QTextCursor c = editor->textCursor();
        if (!c.hasSelection()) c.select(QTextCursor::WordUnderCursor);
        if (c.hasSelection()) {
            mergeEach(c, scale, [&](CharStyle &s) { edit.apply(s); });
        } else {
            CharStyle s = richtext::charStyle(c.charFormat(), scale);
            edit.apply(s);
            QTextCursor typing = editor->textCursor();
            typing.setCharFormat(richtext::charFormat(s, scale));
            if (typing.block().text().isEmpty()) typing.setBlockCharFormat(richtext::charFormat(s, scale));
            editor->setTextCursor(typing);
        }
        layoutEditor();
        emit formatChanged();
        return;
    }
    if (selected.isEmpty()) return;
    editSlide(tr("Format Text"), [&](Slide &s) {
        for (int id : selected)
            if (Element *e = byId(s, id)) {
                ops::forEachText(*e, [&](TextBody &b) { ops::apply(b, edit); });
                ops::fitText(*e);
            }
    });
    emit formatChanged();
}

void Canvas::applyPara(const ops::ParaEdit &edit)
{
    if (editor) {
        QTextDocument *d = editor->document();
        QTextCursor c = editor->textCursor();
        TextBody body = editBody();
        body.paragraphs = richtext::paragraphs(*d, body.fontScale);
        int first = d->findBlock(c.selectionStart()).blockNumber(), last = d->findBlock(c.selectionEnd()).blockNumber();
        for (int i = first; i <= last && i < body.paragraphs.size(); ++i) {
            Paragraph &p = body.paragraphs[i];
            edit.apply(p, body);
            QTextBlock block = d->findBlockByNumber(i);
            QTextCursor bc(block);
            const CharStyle &lead = p.runs.isEmpty() ? p.endStyle : p.runs.first().style;
            bc.setBlockFormat(richtext::blockFormat(p.style, lead, i == 0, body.fontScale, body.lineReduction));
            bc.setBlockCharFormat(richtext::charFormat(p.endStyle, body.fontScale));
            int run = 0;
            for (auto it = block.begin(); !it.atEnd() && run < p.runs.size(); ++it) {
                QTextFragment f = it.fragment();
                if (f.text().isEmpty()) continue;
                QTextCursor fc(d);
                fc.setPosition(f.position());
                fc.setPosition(f.position() + f.length(), QTextCursor::KeepAnchor);
                QTextCharFormat format = richtext::charFormat(p.runs[run++].style, body.fontScale);
                if (f.charFormat().hasProperty(richtext::FieldProperty))
                    format.setProperty(richtext::FieldProperty, f.charFormat().property(richtext::FieldProperty));
                fc.setCharFormat(format);
            }
        }
        layoutEditor();
        emit formatChanged();
        return;
    }
    if (selected.isEmpty()) return;
    editSlide(tr("Format Paragraph"), [&](Slide &s) {
        for (int id : selected)
            if (Element *e = byId(s, id)) {
                ops::forEachText(*e, [&](TextBody &b) { ops::apply(b, edit); });
                ops::fitText(*e);
            }
    });
    emit formatChanged();
}

CharStyle Canvas::charStyle() const
{
    if (editor) return richtext::charStyle(editor->textCursor().charFormat(), editBody().fontScale);
    for (int id : selected) {
        Element e = *byId(slide(), id);
        std::optional<CharStyle> style;
        ops::forEachText(e, [&](TextBody &b) {
            if (style || b.paragraphs.isEmpty()) return;
            const Paragraph &p = b.paragraphs.first();
            style = p.runs.isEmpty() ? p.endStyle : p.runs.first().style;
        });
        if (style) return *style;
    }
    return {};
}

ParaStyle Canvas::paraStyle() const
{
    if (editor) {
        ParaStyle s = editor->textCursor().blockFormat().property(richtext::ParaStyleProperty).value<ParaStyle>();
        s.align = editor->textCursor().blockFormat().alignment() & Qt::AlignHorizontal_Mask;
        return s;
    }
    for (int id : selected)
        if (const TextBody *b = byId(slide(), id)->text(); b && !b->paragraphs.isEmpty()) return b->paragraphs.first().style;
    return {};
}

// --- object operations ---

void Canvas::editSlide(const QString &text, const std::function<void(Slide &)> &fn, int mergeId)
{
    int i = index;
    doc->change(text, [&](Deck &d) { fn(d.slides[i]); }, mergeId);
}

void Canvas::beginInsert(std::function<Element(QLineF)> factory)
{
    stopEditing();
    setSelection({});
    inserting = std::move(factory);
    setCursor(Qt::CrossCursor);
    setFocus();
}

void Canvas::insert(const Element &e)
{
    editSlide(tr("Insert %1").arg(e.name.section(' ', 0, 0)), [&](Slide &s) { s.elements << e; });
    setSelection({e.id});
}

void Canvas::insertImageFile(const QString &file, std::optional<QPointF> at)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;
    Deck copy = deck();
    QPointF center = at.value_or(QPointF(copy.size.width() / 2, copy.size.height() / 2));
    if (auto e = ops::picture(copy, slide().nextId(), f.readAll(), QFileInfo(file).suffix(), center)) insert(*e);
}

void Canvas::removeSelection()
{
    if (selected.isEmpty()) return;
    QList<int> ids = selected;
    editSlide(tr("Delete"), [&](Slide &s) {
        s.elements.removeIf([&](const Element &e) { return ids.contains(e.id); });
    });
    setSelection({});
}

void Canvas::nudge(QPointF delta)
{
    if (selected.isEmpty()) return;
    editSlide(tr("Nudge"), [&](Slide &s) {
        for (int id : selected)
            if (Element *e = byId(s, id)) e->rect.translate(delta);
    }, 1);
}

void Canvas::arrange(int how)
{
    if (selected.isEmpty()) return;
    editSlide(tr("Arrange"), [&](Slide &s) {
        QList<Element> &all = s.elements;
        QList<Element> picked;
        for (const Element &e : all)
            if (selected.contains(e.id)) picked << e;
        if (how == 2 || how == -2) {
            all.removeIf([&](const Element &e) { return selected.contains(e.id); });
            if (how == 2) all << picked;
            else all = picked + all;
            return;
        }
        auto step = [&](qsizetype i, qsizetype j) {
            if (j >= 0 && j < all.size() && !selected.contains(all[j].id)) all.swapItemsAt(i, j);
        };
        if (how > 0)
            for (qsizetype i = all.size() - 1; i >= 0; --i) {
                if (selected.contains(all[i].id)) step(i, i + 1);
            }
        else
            for (qsizetype i = 0; i < all.size(); ++i)
                if (selected.contains(all[i].id)) step(i, i - 1);
    });
}

void Canvas::group()
{
    if (selected.size() < 2) return;
    int id = slide().nextId();
    editSlide(tr("Group"), [&](Slide &s) {
        GroupEl g;
        qsizetype at = -1;
        for (qsizetype i = 0; i < s.elements.size(); ++i)
            if (selected.contains(s.elements[i].id)) {
                g.children.push_back(s.elements[i]);
                g.childRect |= s.elements[i].rect;
                at = i;
            }
        Element e;
        e.id = id;
        e.name = QString("Group %1").arg(id);
        e.rect = g.childRect;
        e.body = std::move(g);
        s.elements.insert(at + 1, e);
        s.elements.removeIf([&](const Element &x) { return selected.contains(x.id); });
    });
    setSelection({id});
}

void Canvas::ungroup()
{
    QList<int> ids;
    editSlide(tr("Ungroup"), [&](Slide &s) {
        for (qsizetype i = 0; i < s.elements.size(); ++i) {
            const Element g = s.elements[i];
            auto group = g.as<GroupEl>();
            if (!group || !selected.contains(g.id)) continue;
            QRectF from = group->childRect.isEmpty() ? g.rect : group->childRect;
            double sx = g.rect.width() / std::max(from.width(), 1e-6), sy = g.rect.height() / std::max(from.height(), 1e-6);
            QTransform toSlide = QTransform::fromTranslate(-from.left(), -from.top()) * QTransform::fromScale(sx, sy) *
                                 elementTransform(g);
            s.elements.removeAt(i);
            for (Element c : group->children) {
                QSizeF size(c.rect.width() * sx, c.rect.height() * sy);
                QPointF center = toSlide.map(c.rect.center());
                c.rect = QRectF(center - QPointF(size.width() / 2, size.height() / 2), size);
                c.rotation = std::fmod(c.rotation + g.rotation, 360);
                ids << c.id;
                s.elements.insert(i++, c);
            }
            --i;
        }
    });
    if (!ids.isEmpty()) setSelection(ids);
}

void Canvas::copy()
{
    if (editor) {
        QTextCursor c = editor->textCursor();
        if (!c.hasSelection()) return;
        auto *m = new QMimeData;
        m->setText(c.selectedText().replace(QChar::ParagraphSeparator, '\n').replace(QChar::LineSeparator, '\n'));
        m->setHtml(c.selection().toHtml());
        QApplication::clipboard()->setMimeData(m);
        return;
    }
    if (selected.isEmpty()) return;
    Clipboard &clip = clipboard();
    clip.elements.clear();
    QStringList texts;
    for (const Element &e : slide().elements)
        if (selected.contains(e.id)) {
            clip.elements << e;
            if (const TextBody *t = e.text()) texts << t->text();
        }
    clip.package = deck().package;
    clip.token = QUuid::createUuid().toByteArray();
    auto *m = new QMimeData;
    m->setData(clipboardMime, clip.token);
    if (!texts.join("").isEmpty()) m->setText(texts.join('\n'));
    QRectF box;
    for (const Element &e : clip.elements) box |= bounds(e);
    if (!box.isEmpty()) {
        QImage img((box.size() * 2).toSize(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        p.scale(2, 2);
        p.translate(-box.topLeft());
        for (const Element &e : clip.elements) paintElement(p, deck(), e);
        p.end();
        m->setImageData(img);
    }
    QApplication::clipboard()->setMimeData(m);
}

void Canvas::cut()
{
    copy();
    if (editor) {
        QTextCursor c = editor->textCursor();
        c.removeSelectedText();
        editor->setTextCursor(c);
    } else {
        removeSelection();
    }
}

void Canvas::paste()
{
    const QMimeData *m = QApplication::clipboard()->mimeData();
    if (!m) return;
    if (editor) {
        if (m->hasText()) {
            QTextCursor c = editor->textCursor();
            c.insertText(m->text().replace('\n', QChar::ParagraphSeparator));
            editor->setTextCursor(c);
        }
        return;
    }
    Clipboard &clip = clipboard();
    if (m->data(clipboardMime) == clip.token && !clip.token.isEmpty()) {
        QList<Element> elements = clip.elements;
        QHash<QString, QString> renamed;
        if (clip.package != deck().package && deck().package && clip.package)
            for (const Element &e : elements)
                for (const QString &part : ops::referencedParts(e))
                    if (!renamed.contains(part)) renamed[part] = deck().package->add(part, clip.package->read(part));
        int id = slide().nextId();
        QList<int> ids;
        for (Element &e : elements) {
            ops::remapParts(e, renamed);
            bool overlap = std::any_of(slide().elements.begin(), slide().elements.end(),
                                       [&](const Element &o) { return o.rect == e.rect; });
            if (overlap) e.rect.translate(12, 12);
            ops::renumber(e, id);
            ids << e.id;
        }
        editSlide(tr("Paste"), [&](Slide &s) { s.elements << elements; });
        clip.elements = elements;
        setSelection(ids);
        return;
    }
    QPointF center(deck().size.width() / 2, deck().size.height() / 2);
    if (m->hasImage()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        qvariant_cast<QImage>(m->imageData()).save(&buffer, "PNG");
        Deck copy = deck();
        if (auto e = ops::picture(copy, slide().nextId(), png, "png", center)) insert(*e);
    } else if (m->hasUrls()) {
        for (const QUrl &url : m->urls())
            if (url.isLocalFile()) insertImageFile(url.toLocalFile());
    } else if (m->hasText()) {
        Element e = ops::textBox(deck(), slide().nextId(), QRectF(center.x() - 200, center.y() - 30, 400, 60));
        TextBody &body = *e.text();
        body.paragraphs.clear();
        for (const QString &line : m->text().split('\n'))
            body.paragraphs << Paragraph{body.levels[0].para, {Run{line, body.levels[0].chr, {}}}, body.levels[0].chr};
        ops::fitText(e);
        insert(e);
    }
}

// --- tables ---

bool Canvas::tableSelected() const
{
    return selected.size() == 1 && byId(slide(), selected.first())->as<TableEl>();
}

void Canvas::tableInsert(bool row, bool after)
{
    if (!tableSelected()) return;
    int id = selected.first();
    QPoint cell = editor && edit.id == id ? QPoint(edit.col, edit.row) : tableCell;
    stopEditing();
    editSlide(row ? tr("Insert Row") : tr("Insert Column"), [&](Slide &s) {
        Element &e = *byId(s, id);
        TableEl &t = *e.as<TableEl>();
        auto blank = [](Cell c) {
            c.colSpan = c.rowSpan = 1;
            c.merged = false;
            for (Paragraph &p : c.text.paragraphs) p.runs.clear();
            c.text.paragraphs = c.text.paragraphs.mid(0, 1);
            return c;
        };
        if (row) {
            int at = std::clamp(cell.y() + (after ? 1 : 0), 0, int(t.rows.size()));
            int src = std::clamp(cell.y() == 0 && after && t.cells.size() > 1 ? 1 : cell.y(), 0, int(t.cells.size()) - 1);
            QList<Cell> cells;
            for (const Cell &c : t.cells[src]) cells << blank(c);
            t.cells.insert(at, cells);
            t.rows.insert(at, t.rows[src]);
            e.rect.setHeight(e.rect.height() + t.rows[src]);
        } else {
            int at = std::clamp(cell.x() + (after ? 1 : 0), 0, int(t.cols.size()));
            int src = std::clamp(cell.x(), 0, int(t.cols.size()) - 1);
            for (QList<Cell> &r : t.cells) r.insert(at, blank(r[std::min<qsizetype>(src, r.size() - 1)]));
            t.cols.insert(at, t.cols[src]);
            e.rect.setWidth(e.rect.width() + t.cols[src]);
        }
    });
}

void Canvas::tableRemove(bool row)
{
    if (!tableSelected()) return;
    int id = selected.first();
    QPoint cell = editor && edit.id == id ? QPoint(edit.col, edit.row) : tableCell;
    stopEditing();
    const TableEl &table = *byId(slide(), id)->as<TableEl>();
    if ((row ? table.rows.size() : table.cols.size()) <= 1) return removeSelection();
    editSlide(row ? tr("Delete Row") : tr("Delete Column"), [&](Slide &s) {
        Element &e = *byId(s, id);
        TableEl &t = *e.as<TableEl>();
        if (row) {
            int r = std::clamp(cell.y(), 0, int(t.rows.size()) - 1);
            e.rect.setHeight(e.rect.height() - t.rows[r]);
            t.rows.removeAt(r);
            t.cells.removeAt(r);
        } else {
            int c = std::clamp(cell.x(), 0, int(t.cols.size()) - 1);
            e.rect.setWidth(e.rect.width() - t.cols[c]);
            t.cols.removeAt(c);
            for (QList<Cell> &r : t.cells)
                if (c < r.size()) r.removeAt(c);
        }
    });
    tableCell = {0, 0};
}
