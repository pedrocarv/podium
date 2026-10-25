#pragma once

// Slideshow: a shared ShowState (slide, click step, timing of builds) drives the full-screen
// SlideShow and the PresenterView.

#include "model.h"
#include "render.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QKeyEvent;
class QLabel;
class QMediaPlayer;
class QTemporaryFile;
class QTextBrowser;

class ShowState : public QObject {
    Q_OBJECT
public:
    ShowState(Deck deck, int start, QObject *parent = nullptr);

    const Deck &deck() const { return d; }
    int slide() const { return current; }
    int step() const { return currentStep; }
    int steps(int slide) const;
    qint64 elapsed() const { return settled ? -1 : clock.elapsed(); } // since the step started; -1 = settled
    qint64 showTime() const { return started.elapsed(); }
    QColor blank() const { return blankColor; }

    void next();
    void previous();
    void go(int slide);
    void setBlank(QColor color);
    void finish();
    void restartTimer() { started.restart(); }

    // Render options for `slide` at `step`, `ms` into that step (build visibility and fades).
    RenderOptions options(int slide, int step, qint64 ms) const;
    bool animating() const;
    QList<std::pair<int, qint64>> mediaStarts() const; // videos the current step plays, with start times
    std::pair<int, int> following() const;             // the slide/step after the current one
    bool handleKey(QKeyEvent *event);                  // shared keyboard control

signals:
    void changed(int previousSlide);
    void blankChanged();
    void finished();

private:
    struct Timed {
        Build build;
        int step = 0;
        qint64 start = 0;
    };
    const QList<Timed> &timeline(int slide) const;
    int visible(int from, int dir) const;

    Deck d;
    int current = 0, currentStep = 0;
    QElapsedTimer clock, started;
    bool settled = false; // stepped backwards: show the end state without animating
    bool done = false;
    QColor blankColor;
    QString typed;
    mutable QHash<int, QList<Timed>> timelines;
};

class SlideShow : public QWidget {
    Q_OBJECT
public:
    explicit SlideShow(ShowState *state, QWidget *parent = nullptr);
    ~SlideShow() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct Video {
        QMediaPlayer *player = nullptr;
        QTemporaryFile *file = nullptr;
        QImage frame;
    };
    QRectF target() const;
    QImage render(int slide, int step, qint64 ms) const;
    void stateChanged(int previousSlide);
    void tick();
    void prepareVideos();
    void stopVideos();
    Video *videoAt(QPointF pos);
    void toggle(int id);

    ShowState *state;
    QImage frame, previousFrame;
    QElapsedTimer transitionClock;
    Transition transition;
    bool inTransition = false;
    QTimer timer, cursorTimer, advanceTimer;
    QHash<int, Video> videos;
    QList<int> startedMedia;
};

class PresenterView : public QWidget {
    Q_OBJECT
public:
    explicit PresenterView(ShowState *state, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void refresh();

    ShowState *state;
    QLabel *current, *next, *clock, *timerLabel, *counter;
    QTextBrowser *notes;
    QTimer ticker;
};
