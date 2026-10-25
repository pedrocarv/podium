#pragma once

// The document model: plain values on Qt's copy-on-write containers, so copying a whole Deck is cheap
// (undo snapshots). Geometry is in points, colors are resolved RGBA, and enumerations keep DrawingML
// vocabulary (preset names, dash names, …) so pptx I/O maps them one-to-one.

#include "package.h"

#include <QColor>
#include <QGradient>
#include <QMap>
#include <QMarginsF>
#include <QMetaType>
#include <QPainterPath>
#include <QRectF>

#include <array>
#include <memory>
#include <variant>
#include <vector>

struct Fill {
    enum Kind { None, Solid, Gradient, Image };
    Kind kind = None;
    QColor color;
    QGradientStops stops;
    double angle = 0; // gradient direction, degrees clockwise from left→right
    QString image;    // part name
    bool operator==(const Fill &) const = default;
    static Fill solid(QColor c)
    {
        Fill f;
        f.kind = Solid;
        f.color = c;
        return f;
    }
};

struct LineEnd {
    QString type = "none", w = "med", len = "med";
    bool operator==(const LineEnd &) const = default;
};

struct Line {
    Fill fill;
    double width = 0.75;
    QString dash = "solid";
    LineEnd head, tail;
    bool operator==(const Line &) const = default;
};

struct CharStyle {
    QString font = "Calibri";
    double size = 18;
    QColor color = Qt::black;
    bool bold = false, italic = false, underline = false, strike = false;
    int baseline = 0; // percent: >0 superscript, <0 subscript
    QString link;
    bool operator==(const CharStyle &) const = default;
};

struct Spacing {
    double value = 0;
    bool percent = false; // value is a fraction of the line height (1.0 = single) instead of points
    bool operator==(const Spacing &) const = default;
};

struct Bullet {
    enum Kind { None, Char, Number };
    Kind kind = None;
    QString chr = QStringLiteral("•");
    QString scheme = "arabicPeriod";
    int startAt = 1;
    QString font;   // empty: follow text
    double scale = 1;
    QColor color;   // invalid: follow text
    bool operator==(const Bullet &) const = default;
};

struct ParaStyle {
    Qt::Alignment align = Qt::AlignLeft;
    int level = 0;
    double marL = 0, indent = 0;
    Spacing lineSpacing{1, true}, spaceBefore, spaceAfter;
    Bullet bullet;
    bool operator==(const ParaStyle &) const = default;
};

struct Run {
    QString text; // U+2028 marks a line break
    CharStyle style;
    QString field; // "slidenum", "datetime1", … (text is the cached value)
    bool operator==(const Run &) const = default;
};

struct Paragraph {
    ParaStyle style;
    QList<Run> runs;
    CharStyle endStyle;
    bool operator==(const Paragraph &) const = default;
    QString text() const;
};

struct LevelStyle {
    ParaStyle para;
    CharStyle chr;
    bool operator==(const LevelStyle &) const = default;
};

struct TextBody {
    enum Anchor { Top, Middle, Bottom };
    enum Autofit { NoAutofit, Shrink, Resize };
    QList<Paragraph> paragraphs;
    QMarginsF insets{7.2, 3.6, 7.2, 3.6};
    Anchor anchor = Top;
    bool wrap = true;
    Autofit autofit = NoAutofit;
    double fontScale = 1, lineReduction = 0;
    QString vert = "horz";
    std::array<LevelStyle, 9> levels; // resolved defaults per outline level
    bool operator==(const TextBody &) const = default;
    bool isEmpty() const;
    QString text() const;
};

struct CustomPath {
    QPainterPath path; // in the path's own coordinate space `size`
    QSizeF size;
    bool fill = true, stroke = true;
    bool operator==(const CustomPath &) const = default;
};

struct ShapeEl {
    QString preset = "rect"; // empty when `custom` geometry is used
    QMap<QString, double> adjust;
    QList<CustomPath> custom;
    Fill fill;
    Line line;
    TextBody text;
    bool textBox = false, connector = false;
    bool operator==(const ShapeEl &) const = default;
};

struct PictureEl {
    QString image;   // part name
    QMarginsF crop;  // fractions of the image cut from each side
    QString video;   // part name of embedded media, if any
    QString preset = "rect";
    Line line;
    bool operator==(const PictureEl &) const = default;
};

struct Cell {
    TextBody text;
    Fill fill;
    Line left, right, top, bottom;
    int colSpan = 1, rowSpan = 1;
    bool merged = false; // covered by another cell's span
    bool operator==(const Cell &) const = default;
};

struct TableEl {
    QList<double> cols, rows;
    QList<QList<Cell>> cells;
    bool operator==(const TableEl &) const = default;
};

struct Element;

struct GroupEl {
    std::vector<Element> children;
    QRectF childRect;
    bool operator==(const GroupEl &) const;
};

// An object we render but don't model (chart, SmartArt, OLE, ink, …); written back verbatim.
struct RawEl {
    QString label;
    QString fallback;              // image part to draw, if any
    std::vector<Element> preview;  // drawable fallback content (from mc:Fallback), in slide coordinates
    bool operator==(const RawEl &) const;
};

struct Placeholder {
    QString type; // "" = not a placeholder; "obj" for index-only placeholders
    int idx = 0;
    QString prompt;
    bool operator==(const Placeholder &) const = default;
    explicit operator bool() const { return !type.isEmpty(); }
};

struct Element {
    int id = 0;
    QString name;
    QRectF rect;
    double rotation = 0; // degrees clockwise
    bool flipH = false, flipV = false, hidden = false;
    Placeholder ph;
    std::variant<ShapeEl, PictureEl, TableEl, GroupEl, RawEl> body;
    QByteArray source; // original XML, patched on save
    QList<Rel> rels;   // relationships referenced from `source`
    bool operator==(const Element &) const = default;

    template <class T> T *as() { return std::get_if<T>(&body); }
    template <class T> const T *as() const { return std::get_if<T>(&body); }
    TextBody *text();
    const TextBody *text() const;
};

struct Build {
    int element = 0;
    int paragraph = -1;        // -1: the whole element
    QString kind = "entr";     // "entr" (entrance effect) or "media" (play video)
    int preset = 10, subtype = 0; // presetID: 1 appear, 10 fade, 9 dissolve, …
    QString trigger = "click"; // click | with | after
    int ms = 500, delay = 0;
    bool operator==(const Build &) const = default;
};

struct Transition {
    QString type; // "" none, else the DrawingML element name: fade, push, wipe, cover, cut, …
    QString dir;
    int ms = 0;
    bool onClick = true;
    int after = -1; // auto-advance after ms
    bool operator==(const Transition &) const = default;
};

struct Theme {
    QString major = "Calibri", minor = "Calibri";
    QMap<QString, QColor> colors; // tx1, bg1, tx2, bg2, accent1…6, hlink, folHlink
    bool operator==(const Theme &) const = default;
};

struct Layout {
    QString part, name;
    Fill background;
    QList<Element> decor;        // master + layout artwork drawn behind slides
    QList<Element> placeholders; // resolved, with empty text
    bool operator==(const Layout &) const = default;
};

struct Slide {
    int layout = 0;
    Fill background;
    bool showMaster = true, hidden = false;
    QList<Element> elements;
    QString notes;
    Transition transition;
    QList<Build> builds;
    QByteArray source; // original slide XML
    QList<Rel> rels;   // original slide relationships
    bool operator==(const Slide &) const = default;

    int nextId() const;
    int indexOf(int id) const;
};

struct Deck {
    QSizeF size{960, 540};
    Theme theme;
    QList<Layout> layouts;
    QList<Slide> slides;
    std::shared_ptr<Package> package;
};

Q_DECLARE_METATYPE(ParaStyle)
Q_DECLARE_METATYPE(CharStyle)
