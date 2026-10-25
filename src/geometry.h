#pragma once

#include "model.h"

namespace geometry {

// What to fill and what to stroke; `fill` is empty for open shapes (lines, brackets, arcs).
struct Outline {
    QPainterPath fill, stroke;
};

Outline preset(const QString &name, QSizeF size, const QMap<QString, double> &adjust = {});
Outline custom(const QList<CustomPath> &paths, QSizeF size);
Outline of(const ShapeEl &shape, QSizeF size);

// DrawingML arcTo: continues `path` along an ellipse with radii wR/hR from startDeg, sweeping sweepDeg
// (degrees, clockwise with y down).
void arcTo(QPainterPath &path, double wR, double hR, double startDeg, double sweepDeg);

bool isLine(const QString &name);
QStringList insertable();

} // namespace geometry
