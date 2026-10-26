#include "mainwindow.h"
#include "pptx.h"
#include "render.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>

#include <cstdio>
#include <string_view>

namespace {

// Exports, --help and --version need no display, so they also work over SSH or in CI.
bool headless(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        for (std::string_view flag : {"--export-pdf", "--export-png", "--save-as", "--help", "-h", "--version", "-v"})
            if (arg == flag || (arg.starts_with(flag) && arg.size() > flag.size() && arg[flag.size()] == '=')) return true;
    }
    return false;
}

int fail(const QString &message)
{
    std::fprintf(stderr, "podium: %s\n", qPrintable(message));
    return 1;
}

int exportDeck(const QCommandLineParser &cli, const QString &file)
{
    QElapsedTimer timer;
    timer.start();
    QString error;
    std::optional<Deck> deck = file.isEmpty() ? pptx::blank() : pptx::load(file, &error);
    if (!deck) return fail(QString("%1: %2").arg(file, error));
    std::fprintf(stderr, "loaded %lld slides in %lld ms\n", qlonglong(deck->slides.size()), timer.restart());
    if (cli.isSet("export-pdf") && !exportPdf(*deck, cli.value("export-pdf"), &error)) return fail(error);
    if (cli.isSet("export-png")) {
        QDir dir(cli.value("export-png"));
        if (!dir.mkpath(".")) return fail("cannot create " + dir.path());
        int width = cli.value("width").toInt();
        QSize size(width, int(width * deck->size.height() / deck->size.width()));
        for (int i = 0; i < deck->slides.size(); ++i)
            if (!renderSlide(*deck, i, size).save(dir.filePath(QString("slide%1.png").arg(i + 1, 2, 10, QChar('0')))))
                return fail("cannot write PNG");
    }
    if (cli.isSet("save-as") && !pptx::save(*deck, cli.value("save-as"), &error)) return fail(error);
    std::fprintf(stderr, "done in %lld ms\n", timer.elapsed());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (headless(argc, argv)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("QT_QPA_PLATFORMTHEME"); // desktop themes (e.g. gtk3) want a display
    }
    QApplication app(argc, argv);
    app.setApplicationName("Podium");
    app.setOrganizationName("Podium");
    app.setApplicationVersion(PODIUM_VERSION);
    app.setDesktopFileName("podium");

    QCommandLineParser cli;
    cli.setApplicationDescription("A presentation editor for .pptx files.");
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOption({"show", "Start the slide show."});
    cli.addOption({"start", "Slide to start the show at (1-based).", "n", "1"});
    cli.addOption({"export-pdf", "Export to a PDF file and exit.", "pdf"});
    cli.addOption({"export-png", "Render every slide to PNG files in a directory and exit.", "dir"});
    cli.addOption({"width", "Width of exported PNGs.", "px", "1920"});
    cli.addOption({"save-as", "Save a copy as .pptx and exit.", "pptx"});
    cli.addPositionalArgument("file", "Presentation to open.");
    cli.process(app);
    QString file = cli.positionalArguments().value(0);

    if (cli.isSet("export-pdf") || cli.isSet("export-png") || cli.isSet("save-as")) return exportDeck(cli, file);

    MainWindow window;
    if (file.isEmpty() || !window.open(file)) window.newDeck();
    window.show();
    if (cli.isSet("show")) window.startShow(cli.value("start").toInt() - 1, false);
    return app.exec();
}
