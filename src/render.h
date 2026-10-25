#pragma once

// The one renderer: canvas, thumbnails, slideshow, presenter view and PDF all paint through here, in
// slide coordinates (points). Callers set up the painter transform.

#include "model.h"

#include <QImage>

#include <functional>

class QPainter;

struct RenderOptions {
    int slideNumber = 0;  // 1-based, for slide-number fields
    bool editing = false; // show empty-placeholder prompts and outlines
    int skipText = -1;    // element whose text is not drawn (being edited)
    std::function<qreal(const Element &, int paragraph)> opacity; // builds; paragraph -1 = whole element
    std::function<QImage(const Element &)> videoFrame;            // playing video, drawn in z-order
};

void paintSlide(QPainter &p, const Deck &deck, const Slide &slide, const RenderOptions &options = {});
void paintElement(QPainter &p, const Deck &deck, const Element &e, const RenderOptions &options = {});
QImage renderSlide(const Deck &deck, int index, QSize size, RenderOptions options = {});
bool exportPdf(const Deck &deck, const QString &path, QString *error = nullptr);

QImage loadImage(const Package &pkg, const QString &part, QSizeF devicePixels = {});
QBrush brush(const Fill &fill, QRectF bounds, const Deck &deck, const QPainter &p);
QPen pen(const Line &line);
QTransform elementTransform(const Element &e); // element-local → slide coordinates (rotation, no flips)

// Column/row edges of a table in element-local coordinates; rows grow to fit their text.
struct TableGeometry {
    QList<double> xs, ys;
    QRectF cell(const TableEl &t, int row, int col) const;
    int rows() const { return int(ys.size()) - 1; }
    int cols() const { return int(xs.size()) - 1; }
};
TableGeometry tableGeometry(const TableEl &t);
