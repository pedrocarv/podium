#pragma once

#include <QMainWindow>

class Canvas;
class ColorButton;
class Document;
class Inspector;
class QComboBox;
class QFontComboBox;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class SlidePanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    bool open(const QString &path);
    void newDeck();
    void startShow(int from, bool rehearse); // rehearse: presenter view even without a second screen

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildActions();
    void setDocument(Document *doc);
    bool maybeSave();
    bool save();
    bool saveAs();
    bool saveTo(const QString &path);
    void exportPdf();
    void addRecent(const QString &path);
    void updateTitle();
    void updateFormat();
    void updateActions();
    void syncNotes();
    void newSlide(int layout);
    void duplicateSlides();
    void deleteSlides();
    void toggleHidden();
    void insertTable();
    void undo();
    void redo();
    QAction *action(const QString &text, const QString &icon, const QKeySequence &key, std::function<void()> fn);

    Document *doc = nullptr;
    QSplitter *split = nullptr;
    Canvas *canvas = nullptr;
    SlidePanel *panel = nullptr;
    QPlainTextEdit *notes = nullptr;
    Inspector *inspector = nullptr;
    QDockWidget *inspectorDock = nullptr;
    QFontComboBox *fontBox = nullptr;
    QComboBox *sizeBox = nullptr, *zoomBox = nullptr;
    ColorButton *textColor = nullptr;
    QLabel *status = nullptr;
    QMenu *recentMenu = nullptr, *layoutMenu = nullptr;
    QAction *undoAction, *redoAction, *bold, *italic, *underline, *bullets, *numbering, *hideSlide;
    QList<QAction *> alignActions, objectActions;
    bool showing = false;
};
