#pragma once

// The slide sorter: thumbnails (rendered incrementally, only for slides that changed), drag to reorder.

#include "document.h"

#include <QFutureWatcher>
#include <QListWidget>

class SlidePanel : public QListWidget {
    Q_OBJECT
public:
    explicit SlidePanel(Document *doc, QWidget *parent = nullptr);

    int current() const { return currentRow(); }
    void setCurrent(int index);
    QList<int> selectedSlides() const;
    void setContextActions(const QList<QAction *> &actions) { contextActions = actions; }

signals:
    void currentSlideChanged(int index);
    void deleteRequested();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void sync();
    void renderNext();

    Document *doc;
    QList<std::optional<Slide>> rendered; // what each thumbnail shows
    QFutureWatcher<QImage> worker;         // renders one thumbnail at a time off the GUI thread
    int pending = -1;
    Slide pendingSlide;
    QList<QAction *> contextActions;
};
