#pragma once

// TextBody ↔ QTextDocument. Documents are laid out on a 72-dpi reference device with design metrics,
// so 1 document unit = 1 pt and line breaks are identical on screen, in thumbnails and in PDFs.

#include "model.h"

#include <QTextDocument>
#include <QTextFormat>

#include <functional>
#include <memory>

class QPainter;

namespace richtext {

enum Property { ParaStyleProperty = QTextFormat::UserProperty + 1, CharStyleProperty, FieldProperty };

struct Options {
    double scale = -1;                   // font scale; -1 uses the body's autofit scale
    int slideNumber = 0;                 // value for slide-number fields; 0 keeps the cached text
    std::function<qreal(int)> opacity;   // per paragraph (builds); null = opaque
};

QPaintDevice *referenceDevice();
QFont font(const CharStyle &style, double scale = 1);
QTextCharFormat charFormat(const CharStyle &style, double scale = 1, qreal alpha = 1);
CharStyle charStyle(const QTextCharFormat &format, double scale = 1);
QTextBlockFormat blockFormat(const ParaStyle &style, const CharStyle &first, bool firstBlock, double scale,
                             double lineReduction);

std::unique_ptr<QTextDocument> document(const TextBody &body, const Options &options = {});
QList<Paragraph> paragraphs(const QTextDocument &doc, double scale = 1);

// Sizes `doc` for `body` inside a box and returns the document → box transform (insets, anchor,
// vertical text).
QTransform place(QTextDocument &doc, const TextBody &body, QSizeF box);
void drawDocument(QPainter &p, const QTextDocument &doc);
void drawBullets(QPainter &p, const QTextDocument &doc);
void draw(QPainter &p, const TextBody &body, QRectF box, const Options &options = {});
double height(const TextBody &body, double width);
double fitScale(const TextBody &body, QSizeF box);
QString bulletText(const Bullet &bullet, int number);

} // namespace richtext
