#pragma once

#include "model.h"

#include <optional>

namespace pugi {
class xml_node;
}

namespace pptx {

std::optional<Deck> load(const QString &path, QString *error = nullptr);
Deck blank(); // a new deck from the built-in template
bool save(const Deck &deck, const QString &path, QString *error = nullptr);

constexpr double emuPerPt = 12700.0;

// Shared by reader and writer: the writer keeps a slide's original <p:transition>/<p:timing> XML
// whenever the model still equals what these return for it.
Transition readTransition(pugi::xml_node sld);
QList<Build> readBuilds(pugi::xml_node sld);

} // namespace pptx
