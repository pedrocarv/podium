#include "document.h"

#include <QFileInfo>

class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Document *doc, Deck before, Deck after, const QString &text, int merge)
        : QUndoCommand(text), doc(doc), before(std::move(before)), after(std::move(after)), merge(merge)
    {
    }
    void undo() override { doc->set(before); }
    void redo() override { doc->set(after); }
    int id() const override { return merge ? merge : -1; }
    bool mergeWith(const QUndoCommand *other) override
    {
        if (other->text() != text()) return false;
        after = static_cast<const SnapshotCommand *>(other)->after;
        return true;
    }

private:
    Document *doc;
    Deck before, after;
    int merge;
};

Document::Document(Deck deck, QString path, QObject *parent) : QObject(parent), current(std::move(deck)), file(std::move(path))
{
    connect(&undo, &QUndoStack::cleanChanged, this, &Document::changed);
}

QString Document::title() const
{
    return file.isEmpty() ? tr("Untitled") : QFileInfo(file).completeBaseName();
}

void Document::change(const QString &text, const std::function<void(Deck &)> &edit, int mergeId)
{
    Deck after = current;
    edit(after);
    undo.push(new SnapshotCommand(this, current, std::move(after), text, mergeId));
}

void Document::markSaved(const QString &path)
{
    file = path;
    undo.setClean();
    emit changed();
}

void Document::set(const Deck &deck)
{
    current = deck;
    emit changed();
}
