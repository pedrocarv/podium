#pragma once

// The Format panel: properties of the selected object (or of the slide when nothing is selected).

#include "document.h"

#include <QScrollArea>
#include <QToolButton>

class Canvas;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QSpinBox;

// A color picker button: theme swatches, "None" (optional) and a full color dialog.
class ColorButton : public QToolButton {
    Q_OBJECT
public:
    ColorButton(const QString &label, bool allowNone, QWidget *parent = nullptr);
    void setColor(QColor color);
    void setThemeColors(const QList<QColor> &colors);

signals:
    void picked(QColor color); // invalid = none

private:
    void rebuildMenu();
    QString label;
    bool allowNone;
    QColor current;
    QList<QColor> theme;
};

class Inspector : public QScrollArea {
    Q_OBJECT
public:
    Inspector(Document *doc, Canvas *canvas, QWidget *parent = nullptr);
    void refresh();

private:
    void editElement(const QString &text, const std::function<void(Element &)> &fn, int mergeId = 0);
    void editSlide(const QString &text, const std::function<void(Slide &)> &fn, int mergeId = 0);
    void setBuild();

    Document *doc;
    Canvas *canvas;
    bool updating = false;
    QGroupBox *slideBox, *arrangeBox, *shapeBox, *textBox, *animBox;
    QComboBox *layout, *transition, *anchor, *autofit, *dash, *effect, *trigger;
    QDoubleSpinBox *x, *y, *w, *h, *rotation, *lineWidth, *duration, *transitionTime, *advance;
    QCheckBox *hidden, *advanceOn, *wrap, *byParagraph;
    ColorButton *background, *fill, *line;
};
