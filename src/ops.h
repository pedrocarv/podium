#pragma once

// Pure operations on the model used by the editor UI.

#include "model.h"

#include <QTextCharFormat>

#include <functional>
#include <optional>

namespace ops {

struct CharEdit {
    std::optional<QString> font;
    std::optional<double> size, grow; // grow: multiply the size
    std::optional<bool> bold, italic, underline, strike;
    std::optional<QColor> color;
    void apply(CharStyle &s) const;
    QTextCharFormat format(double scale = 1) const; // for a live QTextDocument (sizes need the current style)
};

struct ParaEdit {
    std::optional<Qt::Alignment> align;
    std::optional<Bullet::Kind> bullet;
    int level = 0; // relative change
    void apply(Paragraph &p, const TextBody &body) const;
};

void forEachText(Element &e, const std::function<void(TextBody &)> &fn);
void apply(TextBody &body, const CharEdit &edit);
void apply(TextBody &body, const ParaEdit &edit);
void fitText(Element &e); // autofit: shrink fonts or grow the box after text changes

std::array<LevelStyle, 9> plainLevels(const Deck &deck, double size = 18, QColor color = {});
Element textBox(const Deck &deck, int id, QRectF rect);
Element shape(const Deck &deck, int id, const QString &preset, QRectF rect);
Element table(const Deck &deck, int id, int rows, int cols, QRectF rect);
std::optional<Element> picture(Deck &deck, int id, const QByteArray &data, const QString &suffix, QPointF center);
Slide slideFromLayout(const Deck &deck, int layout);
void scaleTable(TableEl &t, QSizeF size);
QStringList referencedParts(const Element &e); // media parts used by an element
void remapParts(Element &e, const QHash<QString, QString> &renamed);
void renumber(Element &e, int &nextId); // fresh ids for e and its group children
QString presetName(const QString &preset);

} // namespace ops
