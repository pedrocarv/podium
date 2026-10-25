#pragma once

// The slide editor: draws the slide through the shared renderer and handles selection, moving,
// resizing, rotating, drawing new objects and in-place rich text editing (also inside table cells).

#include "document.h"
#include "ops.h"

#include <QGraphicsTextItem>
#include <QGraphicsView>

class QAction;

class TextEditor : public QGraphicsTextItem {
    Q_OBJECT
public:
    using QGraphicsTextItem::QGraphicsTextItem;

signals:
    void escape();
    void tab(bool backwards);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool sceneEvent(QEvent *event) override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *option, QWidget *widget) override;
};

class Canvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit Canvas(Document *doc, QWidget *parent = nullptr);

    int slideIndex() const { return index; }
    void setSlideIndex(int index);
    QList<int> selection() const { return selected; }
    void setSelection(const QList<int> &ids);
    void selectAll();

    bool isEditingText() const { return editor; }
    TextEditor *textEditor() const { return editor; }
    void beginEdit(int id, int row = -1, int col = -1);
    void stopEditing();

    // Formatting applies to the text selection while editing, otherwise to the selected objects.
    void applyChar(const ops::CharEdit &edit);
    void applyPara(const ops::ParaEdit &edit);
    CharStyle charStyle() const;
    ParaStyle paraStyle() const;

    // The next drag on the canvas draws an object made by `factory` (from the dragged line).
    void beginInsert(std::function<Element(QLineF)> factory);
    void insert(const Element &e);
    void insertImageFile(const QString &file, std::optional<QPointF> at = {});
    void removeSelection();
    void arrange(int how); // 2 to front, 1 forward, -1 backward, -2 to back
    void group();
    void ungroup();
    void copy();
    void cut();
    void paste();

    void tableInsert(bool row, bool after);
    void tableRemove(bool row);
    bool tableSelected() const;

    double zoom() const;
    void setZoom(double zoom); // <= 0: fit
    void setContextActions(const QList<QAction *> &actions) { contextActions = actions; }

signals:
    void selectionChanged();
    void formatChanged();
    void zoomChanged(double zoom);
    void stepSlide(int delta);

protected:
    void drawBackground(QPainter *p, const QRectF &rect) override;
    void drawForeground(QPainter *p, const QRectF &rect) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool focusNextPrevChild(bool) override { return false; } // Tab belongs to the canvas (cells, levels, objects)
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    struct Drag {
        enum Mode { None, Pending, Move, Resize, Rotate, Rubber, Insert } mode = None;
        QPointF start, pressView;
        Slide original, preview;
        int handle = -1;
        bool editOnRelease = false;
        QRectF rubber;
        QList<QLineF> guides;
        QLineF insert;
    };
    struct EditTarget {
        int id = -1, row = -1, col = -1;
    };

    const Deck &deck() const { return doc->deck(); }
    const Slide &slide() const;
    const Slide &shown() const;
    void sync();
    int elementAt(QPointF scenePos) const;
    QList<std::pair<QPointF, int>> handles() const;
    int handleAt(QPointF viewPos) const;
    void moveTo(QPointF pos, Qt::KeyboardModifiers mods);
    void resizeTo(QPointF pos, Qt::KeyboardModifiers mods);
    void rotateTo(QPointF pos, Qt::KeyboardModifiers mods);
    QPointF snap(QRectF box);
    void updateCursor(QPointF viewPos, QPointF scenePos);
    void beginEditAt(int id, QPointF scenePos);
    void layoutEditor();
    TextBody editBody() const;
    QPoint cellAt(const Element &e, QPointF scenePos) const;
    void nudge(QPointF delta);
    void editSlide(const QString &text, const std::function<void(Slide &)> &fn, int mergeId = 0);

    Document *doc;
    int index = 0;
    QList<int> selected;
    Drag drag;
    EditTarget edit;
    TextEditor *editor = nullptr;
    bool fitting = true;
    QPoint tableCell{0, 0};
    std::function<Element(QLineF)> inserting;
    QList<QAction *> contextActions;
};
