#include "show.h"

#include <QAudioOutput>
#include <QBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QPainter>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTime>
#include <QVideoFrame>
#include <QVideoSink>

namespace {

qreal ease(qreal t) { return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2; }

QRectF letterbox(QSizeF slide, QRectF area)
{
    QSizeF s = slide.scaled(area.size(), Qt::KeepAspectRatio);
    return QRectF(area.center() - QPointF(s.width() / 2, s.height() / 2), s);
}

} // namespace

// --- ShowState ---

ShowState::ShowState(Deck deck, int start, QObject *parent) : QObject(parent), d(std::move(deck))
{
    current = std::clamp(start, 0, int(d.slides.size()) - 1);
    if (d.slides[current].hidden) current = visible(current, 1);
    if (current < 0) current = start;
    clock.start();
    started.start();
}

const QList<ShowState::Timed> &ShowState::timeline(int slide) const
{
    auto it = timelines.find(slide);
    if (it != timelines.end()) return *it;
    QList<Timed> out;
    int step = 0;
    qint64 groupStart = 0, lastStart = 0, lastEnd = 0;
    for (const Build &b : d.slides[slide].builds) {
        if (b.trigger == "click") {
            ++step;
            groupStart = lastStart = lastEnd = 0;
        }
        qint64 start = b.trigger == "after" ? lastEnd : b.trigger == "with" ? lastStart : groupStart;
        start += b.delay;
        int ms = b.kind == "media" ? 0 : (b.preset == 1 ? 0 : std::max(b.ms, 1));
        out << Timed{b, step, start};
        lastStart = start;
        lastEnd = std::max(lastEnd, start + ms);
    }
    return *timelines.insert(slide, out);
}

int ShowState::steps(int slide) const
{
    int n = 0;
    for (const Timed &t : timeline(slide)) n = std::max(n, t.step);
    return n;
}

int ShowState::visible(int from, int dir) const
{
    for (int i = from + dir; i >= 0 && i < d.slides.size(); i += dir)
        if (!d.slides[i].hidden) return i;
    return -1;
}

void ShowState::next()
{
    int previousSlide = current;
    if (currentStep < steps(current)) {
        ++currentStep;
    } else {
        int n = visible(current, 1);
        if (n < 0) return finish();
        current = n;
        currentStep = 0;
    }
    settled = false;
    clock.restart();
    emit changed(previousSlide);
}

void ShowState::previous()
{
    int previousSlide = current;
    if (currentStep > 0) {
        --currentStep;
    } else {
        int p = visible(current, -1);
        if (p < 0) return;
        current = p;
        currentStep = steps(p);
    }
    settled = true;
    emit changed(previousSlide);
}

void ShowState::go(int slide)
{
    if (slide < 0 || slide >= d.slides.size()) return;
    int previousSlide = current;
    current = slide;
    currentStep = 0;
    settled = false;
    clock.restart();
    emit changed(previousSlide);
}

void ShowState::finish()
{
    if (std::exchange(done, true)) return;
    emit finished();
}

void ShowState::setBlank(QColor color)
{
    blankColor = blankColor == color ? QColor() : color;
    emit blankChanged();
}

RenderOptions ShowState::options(int slide, int step, qint64 ms) const
{
    RenderOptions o;
    o.slideNumber = slide + 1;
    QList<Timed> line = timeline(slide);
    if (line.isEmpty()) return o;
    o.opacity = [line, step, ms](const Element &e, int paragraph) -> qreal {
        qreal alpha = 1;
        bool seen = false;
        for (const Timed &t : line) {
            if (t.build.kind != "entr" || t.build.element != e.id || t.build.paragraph != paragraph) continue;
            if (seen) continue; // the first entrance decides when it appears
            seen = true;
            if (t.step > step) alpha = 0;
            else if (t.step == step && ms >= 0) {
                if (ms < t.start) alpha = 0;
                else if (t.build.preset != 1) alpha = ease(std::clamp(qreal(ms - t.start) / std::max(t.build.ms, 1), 0.0, 1.0));
            }
        }
        return alpha;
    };
    return o;
}

bool ShowState::animating() const
{
    if (elapsed() < 0) return false;
    for (const Timed &t : timeline(current))
        if (t.step == currentStep && elapsed() < t.start + t.build.ms) return true;
    return false;
}

QList<std::pair<int, qint64>> ShowState::mediaStarts() const
{
    QList<std::pair<int, qint64>> out;
    for (const Timed &t : timeline(current))
        if (t.build.kind == "media" && t.step == currentStep) out << std::pair{t.build.element, t.start};
    return out;
}

std::pair<int, int> ShowState::following() const
{
    if (currentStep < steps(current)) return {current, currentStep + 1};
    return {visible(current, 1), 0};
}

bool ShowState::handleKey(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_Space:
    case Qt::Key_PageDown:
    case Qt::Key_N:
        next();
        break;
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_PageUp:
    case Qt::Key_Backspace:
    case Qt::Key_P:
        previous();
        break;
    case Qt::Key_Home:
        go(visible(-1, 1));
        break;
    case Qt::Key_End:
        go(visible(int(d.slides.size()), -1));
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!typed.isEmpty()) go(typed.toInt() - 1);
        else next();
        typed.clear();
        break;
    case Qt::Key_B:
    case Qt::Key_Period:
        setBlank(Qt::black);
        break;
    case Qt::Key_W:
    case Qt::Key_Comma:
        setBlank(Qt::white);
        break;
    case Qt::Key_Escape:
        finish();
        break;
    default:
        if (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
            typed += event->text();
            return true;
        }
        return false;
    }
    if (event->key() < Qt::Key_0 || event->key() > Qt::Key_9) typed.clear();
    return true;
}

// --- SlideShow ---

SlideShow::SlideShow(ShowState *state, QWidget *parent) : QWidget(parent), state(state)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setWindowTitle(tr("Slide Show"));
    setMouseTracking(true);
    setCursor(Qt::BlankCursor);
    timer.setInterval(16);
    cursorTimer.setSingleShot(true);
    cursorTimer.setInterval(2000);
    advanceTimer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, this, &SlideShow::tick);
    connect(&cursorTimer, &QTimer::timeout, this, [this] { setCursor(Qt::BlankCursor); });
    connect(&advanceTimer, &QTimer::timeout, state, &ShowState::next);
    connect(state, &ShowState::changed, this, &SlideShow::stateChanged);
    connect(state, &ShowState::blankChanged, this, qOverload<>(&QWidget::update));
    connect(state, &ShowState::finished, this, &QWidget::close);
    stateChanged(state->slide());
}

SlideShow::~SlideShow() { stopVideos(); }

QRectF SlideShow::target() const { return letterbox(state->deck().size, rect()); }

QImage SlideShow::render(int slide, int step, qint64 ms) const
{
    if (slide < 0) return {};
    QSizeF size = target().size() * devicePixelRatioF();
    QImage img(size.toSize(), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(devicePixelRatioF());
    img.fill(Qt::black);
    QPainter p(&img);
    p.scale(size.width() / state->deck().size.width() / devicePixelRatioF(),
            size.height() / state->deck().size.height() / devicePixelRatioF());
    RenderOptions o = state->options(slide, step, ms);
    if (slide == state->slide() && !videos.isEmpty())
        o.videoFrame = [this](const Element &e) { return videos.value(e.id).frame; };
    paintSlide(p, state->deck(), state->deck().slides[slide], o);
    return img;
}

void SlideShow::stateChanged(int previousSlide)
{
    advanceTimer.stop();
    bool newSlide = previousSlide != state->slide() || frame.isNull();
    if (newSlide) {
        stopVideos();
        previousFrame = frame;
        transition = state->deck().slides[state->slide()].transition;
        inTransition = !previousFrame.isNull() && !transition.type.isEmpty() && transition.type != "cut" &&
                       previousSlide != state->slide();
        transitionClock.start();
        prepareVideos();
    }
    startedMedia.clear();
    tick();
    timer.start();
}

void SlideShow::tick()
{
    qint64 ms = state->elapsed();
    frame = render(state->slide(), state->step(), ms);
    if (inTransition && transitionClock.elapsed() >= std::max(transition.ms, 1)) inTransition = false;
    for (auto [id, start] : state->mediaStarts())
        if (!startedMedia.contains(id) && ms >= start) {
            startedMedia << id;
            if (videos.contains(id)) videos[id].player->play();
        }
    bool busy = inTransition || state->animating();
    if (!busy) timer.stop();
    const Slide &s = state->deck().slides[state->slide()];
    if (!busy && s.transition.after >= 0 && state->step() == state->steps(state->slide()) && !advanceTimer.isActive())
        advanceTimer.start(s.transition.after);
    update();
}

void SlideShow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (state->blank().isValid()) {
        p.fillRect(rect(), state->blank());
        return;
    }
    QRectF t = target();
    if (inTransition) {
        qreal k = ease(std::clamp(qreal(transitionClock.elapsed()) / std::max(transition.ms, 1), 0.0, 1.0));
        QString dir = transition.dir.isEmpty() ? "l" : transition.dir;
        QPointF axis = dir == "r" ? QPointF(-1, 0) : dir == "u" ? QPointF(0, 1) : dir == "d" ? QPointF(0, -1) : QPointF(1, 0);
        QPointF span(axis.x() * t.width(), axis.y() * t.height());
        p.setClipRect(t);
        if (transition.type == "push") {
            p.drawImage(t.translated(-span * k), previousFrame);
            p.drawImage(t.translated(span * (1 - k)), frame);
        } else if (transition.type == "cover") {
            p.drawImage(t, previousFrame);
            p.drawImage(t.translated(span * (1 - k)), frame);
        } else if (transition.type == "wipe") {
            p.drawImage(t, previousFrame);
            QRectF reveal = t;
            if (axis.x() > 0) reveal.setLeft(t.right() - t.width() * k);
            else if (axis.x() < 0) reveal.setRight(t.left() + t.width() * k);
            else if (axis.y() > 0) reveal.setTop(t.bottom() - t.height() * k);
            else reveal.setBottom(t.top() + t.height() * k);
            p.setClipRect(reveal);
            p.drawImage(t, frame);
        } else {
            p.drawImage(t, previousFrame);
            p.setOpacity(k);
            p.drawImage(t, frame);
        }
        return;
    }
    p.drawImage(t, frame);
}

void SlideShow::prepareVideos()
{
    const Slide &s = state->deck().slides[state->slide()];
    for (const Element &e : s.elements) {
        auto pic = e.as<PictureEl>();
        if (!pic || pic->video.isEmpty() || !state->deck().package) continue;
        Video v;
        v.file = new QTemporaryFile(QDir::tempPath() + "/podium-XXXXXX." + QFileInfo(pic->video).suffix(), this);
        if (!v.file->open()) {
            delete v.file;
            continue;
        }
        v.file->write(state->deck().package->read(pic->video));
        v.file->flush();
        v.player = new QMediaPlayer(this);
        auto *audio = new QAudioOutput(v.player);
        v.player->setAudioOutput(audio);
        auto *sink = new QVideoSink(v.player);
        v.player->setVideoSink(sink);
        int id = e.id;
        connect(sink, &QVideoSink::videoFrameChanged, this, [this, id](const QVideoFrame &f) {
            if (!videos.contains(id)) return;
            videos[id].frame = f.toImage();
            if (!timer.isActive()) tick();
        });
        v.player->setSource(QUrl::fromLocalFile(v.file->fileName()));
        videos.insert(id, v);
    }
}

void SlideShow::stopVideos()
{
    for (Video &v : videos) {
        v.player->stop();
        delete v.player;
        delete v.file;
    }
    videos.clear();
}

SlideShow::Video *SlideShow::videoAt(QPointF pos)
{
    QRectF t = target();
    QPointF p((pos.x() - t.left()) * state->deck().size.width() / t.width(),
              (pos.y() - t.top()) * state->deck().size.height() / t.height());
    const QList<Element> &elements = state->deck().slides[state->slide()].elements;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it)
        if (videos.contains(it->id) && QRectF(QPointF(), it->rect.size()).contains(elementTransform(*it).inverted().map(p)))
            return &videos[it->id];
    return nullptr;
}

void SlideShow::keyPressEvent(QKeyEvent *event)
{
    if (!state->handleKey(event)) QWidget::keyPressEvent(event);
}

void SlideShow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) return state->previous();
    if (Video *v = videoAt(event->position())) {
        if (v->player->playbackState() == QMediaPlayer::PlayingState) v->player->pause();
        else v->player->play();
        timer.start();
        return;
    }
    state->next();
}

void SlideShow::mouseMoveEvent(QMouseEvent *)
{
    unsetCursor();
    cursorTimer.start();
}

void SlideShow::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() < 0) state->next();
    else if (event->angleDelta().y() > 0) state->previous();
}

void SlideShow::resizeEvent(QResizeEvent *)
{
    tick();
}

void SlideShow::closeEvent(QCloseEvent *event)
{
    stopVideos();
    timer.stop();
    state->finish();
    QWidget::closeEvent(event);
}

// --- PresenterView ---

PresenterView::PresenterView(ShowState *state, QWidget *parent) : QWidget(parent), state(state)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Presenter View"));
    setStyleSheet("PresenterView { background: #111; } QLabel { color: #ddd; } "
                  "QTextBrowser { background: #1b1b1b; color: #eee; border: none; font-size: 20px; }");
    current = new QLabel;
    next = new QLabel;
    clock = new QLabel;
    timerLabel = new QLabel;
    counter = new QLabel;
    notes = new QTextBrowser;
    for (QLabel *l : {current, next}) {
        l->setAlignment(Qt::AlignCenter);
        l->setMinimumSize(160, 90);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }
    QFont big = font();
    big.setPointSize(22);
    big.setBold(true);
    for (QLabel *l : {clock, timerLabel, counter}) l->setFont(big);

    auto *reset = new QPushButton(tr("Restart Timer"));
    auto *end = new QPushButton(tr("End Show"));
    connect(reset, &QPushButton::clicked, state, &ShowState::restartTimer);
    connect(end, &QPushButton::clicked, state, &ShowState::finish);

    auto *side = new QVBoxLayout;
    auto *nextTitle = new QLabel(tr("Next"));
    side->addWidget(nextTitle);
    side->addWidget(next, 2);
    auto *notesTitle = new QLabel(tr("Notes"));
    side->addWidget(notesTitle);
    side->addWidget(notes, 3);
    auto *top = new QHBoxLayout;
    top->addWidget(current, 3);
    top->addLayout(side, 2);
    auto *bar = new QHBoxLayout;
    bar->addWidget(timerLabel);
    bar->addSpacing(24);
    bar->addWidget(clock);
    bar->addStretch();
    bar->addWidget(counter);
    bar->addSpacing(24);
    bar->addWidget(reset);
    bar->addWidget(end);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top, 1);
    layout->addLayout(bar);

    ticker.setInterval(500);
    connect(&ticker, &QTimer::timeout, this, [this] {
        qint64 s = this->state->showTime() / 1000;
        timerLabel->setText(QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')));
        clock->setText(QTime::currentTime().toString("HH:mm"));
    });
    ticker.start();
    connect(state, &ShowState::changed, this, &PresenterView::refresh);
    connect(state, &ShowState::finished, this, &QWidget::close);
    refresh();
}

void PresenterView::refresh()
{
    auto image = [this](QLabel *label, int slide, int step) {
        if (slide < 0) {
            label->setPixmap({});
            label->setText(tr("End of show"));
            return;
        }
        QSizeF deck = state->deck().size;
        QSizeF size = deck.scaled(label->size(), Qt::KeepAspectRatio) * devicePixelRatioF();
        if (size.isEmpty()) return;
        RenderOptions o = state->options(slide, step, -1);
        QImage img = renderSlide(state->deck(), slide, size.toSize(), o);
        img.setDevicePixelRatio(devicePixelRatioF());
        label->setPixmap(QPixmap::fromImage(img));
    };
    image(current, state->slide(), state->step());
    auto [ns, nstep] = state->following();
    image(next, ns, nstep);
    notes->setPlainText(state->deck().slides[state->slide()].notes);
    int total = int(state->deck().slides.size());
    QString steps = state->steps(state->slide()) ? QString("  ·  %1/%2").arg(state->step()).arg(state->steps(state->slide())) : QString();
    counter->setText(tr("Slide %1 of %2").arg(state->slide() + 1).arg(total) + steps);
}

void PresenterView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::ZoomIn) || event->matches(QKeySequence::ZoomOut)) {
        QFont f = notes->font();
        f.setPointSizeF(std::max(8.0, f.pointSizeF() + (event->matches(QKeySequence::ZoomIn) ? 2 : -2)));
        notes->setFont(f);
        return;
    }
    if (!state->handleKey(event)) QWidget::keyPressEvent(event);
}

void PresenterView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refresh();
}

void PresenterView::closeEvent(QCloseEvent *event)
{
    ticker.stop();
    state->finish();
    QWidget::closeEvent(event);
}
