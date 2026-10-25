#include "slidepanel.h"
#include "render.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QtConcurrentRun>

namespace {

constexpr int thumbWidth = 320; // device pixels

class Delegate : public QStyledItemDelegate {
public:
    explicit Delegate(QListWidget *list, const Document *doc) : QStyledItemDelegate(list), list(list), doc(doc) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        QSizeF slide = doc->deck().size;
        int w = list->viewport()->width() - 40;
        return QSize(list->viewport()->width(), int(w * slide.height() / slide.width()) + 14);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        bool selected = o.state & QStyle::State_Selected, current = index.row() == list->currentRow();
        QRectF r = QRectF(o.rect).adjusted(30, 7, -10, -7);
        QColor accent = o.palette.color(QPalette::Highlight);
        p->setPen(current ? accent : o.palette.color(QPalette::PlaceholderText));
        QFont f = o.font;
        f.setBold(current);
        p->setFont(f);
        p->drawText(QRectF(o.rect.left(), r.top(), 26, 20), Qt::AlignRight | Qt::AlignTop, QString::number(index.row() + 1));
        QImage img = index.data(Qt::DecorationRole).value<QImage>();
        if (img.isNull()) p->fillRect(r, Qt::white);
        else p->drawImage(r, img);
        if (index.data(Qt::UserRole + 1).toBool()) {
            p->fillRect(r, QColor(0, 0, 0, 110));
            p->setPen(QPen(Qt::white, 1.5));
            p->drawLine(r.topLeft(), r.bottomRight());
        }
        p->setPen(selected || current ? QPen(accent, 2.5) : QPen(o.palette.color(QPalette::Mid), 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(r.adjusted(-1.5, -1.5, 1.5, 1.5), 3, 3);
        p->restore();
    }

private:
    QListWidget *list;
    const Document *doc;
};

} // namespace

SlidePanel::SlidePanel(Document *doc, QWidget *parent) : QListWidget(parent), doc(doc)
{
    setItemDelegate(new Delegate(this, doc));
    setSelectionMode(ExtendedSelection);
    setDragDropMode(InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setVerticalScrollMode(ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(NoFrame);
    setMinimumWidth(150);
    connect(&worker, &QFutureWatcher<QImage>::finished, this, [this] {
        if (pending >= 0 && pending < count()) {
            item(pending)->setData(Qt::DecorationRole, worker.result());
            rendered[pending] = pendingSlide;
        }
        pending = -1;
        renderNext();
    });
    connect(this, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) emit currentSlideChanged(row);
        viewport()->update();
    });
    connect(doc, &Document::changed, this, &SlidePanel::sync);
    sync();
}

void SlidePanel::setCurrent(int index)
{
    if (index == currentRow()) return;
    setCurrentRow(index, QItemSelectionModel::ClearAndSelect);
    scrollToItem(item(index));
}

QList<int> SlidePanel::selectedSlides() const
{
    QList<int> rows;
    for (const QModelIndex &i : selectionModel()->selectedRows()) rows << i.row();
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty() && currentRow() >= 0) rows << currentRow();
    return rows;
}

void SlidePanel::sync()
{
    const QList<Slide> &slides = doc->deck().slides;
    int keep = std::clamp(currentRow(), 0, int(slides.size()) - 1);
    {
        QSignalBlocker block(this);
        while (count() > slides.size()) delete takeItem(count() - 1);
        while (count() < slides.size()) addItem(new QListWidgetItem);
        for (int i = 0; i < count(); ++i) {
            item(i)->setData(Qt::UserRole, i);
            item(i)->setData(Qt::UserRole + 1, slides[i].hidden);
        }
        if (currentRow() != keep) setCurrentRow(keep);
    }
    rendered.resize(slides.size());
    renderNext();
    viewport()->update();
}

void SlidePanel::renderNext()
{
    if (worker.isRunning()) return;
    const Deck &deck = doc->deck();
    for (int i = 0; i < deck.slides.size(); ++i) {
        if (rendered[i] && *rendered[i] == deck.slides[i]) continue;
        QSize size(thumbWidth, int(thumbWidth * deck.size.height() / deck.size.width()));
        pending = i;
        pendingSlide = deck.slides[i];
        worker.setFuture(QtConcurrent::run([deck, i, size] { return renderSlide(deck, i, size); }));
        return;
    }
}

void SlidePanel::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addActions(contextActions);
    menu.exec(event->globalPos());
}

void SlidePanel::dropEvent(QDropEvent *event)
{
    QListWidget::dropEvent(event);
    QList<int> order;
    for (int i = 0; i < count(); ++i) order << item(i)->data(Qt::UserRole).toInt();
    if (std::is_sorted(order.begin(), order.end())) return;
    QList<std::optional<Slide>> thumbs;
    for (int i : order) thumbs << rendered.value(i);
    rendered = thumbs; // the items moved with their thumbnails
    doc->change(tr("Move Slides"), [&](Deck &d) {
        QList<Slide> slides;
        for (int i : order) slides << d.slides[i];
        d.slides = slides;
    });
    emit currentSlideChanged(currentRow());
}

void SlidePanel::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) return emit deleteRequested();
    QListWidget::keyPressEvent(event);
}

void SlidePanel::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    doItemsLayout();
}
