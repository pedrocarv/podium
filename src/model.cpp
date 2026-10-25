#include "model.h"

#include <algorithm>
#include <functional>

bool GroupEl::operator==(const GroupEl &o) const { return children == o.children && childRect == o.childRect; }

bool RawEl::operator==(const RawEl &o) const
{
    return label == o.label && fallback == o.fallback && preview == o.preview;
}

QString Paragraph::text() const
{
    QString s;
    for (const Run &r : runs) s += r.text;
    return s;
}

bool TextBody::isEmpty() const
{
    return std::all_of(paragraphs.begin(), paragraphs.end(), [](const Paragraph &p) { return p.text().isEmpty(); });
}

QString TextBody::text() const
{
    QStringList lines;
    for (const Paragraph &p : paragraphs) lines << p.text();
    return lines.join('\n');
}

TextBody *Element::text()
{
    if (auto s = as<ShapeEl>()) return &s->text;
    return nullptr;
}

const TextBody *Element::text() const { return const_cast<Element *>(this)->text(); }

int Slide::nextId() const
{
    int id = 1;
    std::function<void(const Element &)> visit = [&](const Element &e) {
        id = std::max(id, e.id);
        if (auto g = e.as<GroupEl>())
            for (const Element &c : g->children) visit(c);
    };
    for (const Element &e : elements) visit(e);
    return id + 1;
}

int Slide::indexOf(int id) const
{
    for (int i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return i;
    return -1;
}
