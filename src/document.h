#pragma once

// The open presentation: the Deck plus its undo history. Every change goes through change(), which
// snapshots the deck (cheap: copy-on-write) so undo/redo is just swapping values.

#include "model.h"

#include <QObject>
#include <QUndoStack>

#include <functional>

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(Deck deck, QString path = {}, QObject *parent = nullptr);

    const Deck &deck() const { return current; }
    QUndoStack *undoStack() { return &undo; }
    QString path() const { return file; }
    QString title() const;
    bool isModified() const { return !undo.isClean(); }

    // Applies `edit` as one undoable step. Steps with the same non-zero `mergeId` and text merge.
    void change(const QString &text, const std::function<void(Deck &)> &edit, int mergeId = 0);
    void markSaved(const QString &path);

signals:
    void changed();

private:
    friend class SnapshotCommand;
    void set(const Deck &deck);
    Deck current;
    QString file;
    QUndoStack undo;
};
