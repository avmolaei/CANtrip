#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QCoreApplication>
#include <QObject>
#include <QStringList>

#include "HeadlessRunner.h"
#include "MainWindow.h"

int main(int argc, char** argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
            break;
        }
    }

    if (headless) {
        // QCoreApplication, not QApplication - genuinely no widget
        // dependency anywhere in the headless path (TsharkCapture,
        // MessageSender, ILogWriter, DbcDecoder are all plain QObjects),
        // matching the whole point of --headless.
        QCoreApplication app(argc, argv);

        QStringList args = QCoreApplication::arguments();
        args.removeFirst(); // argv[0]
        args.removeOne("--headless");

        cantrip::HeadlessRunner runner;
        QString error;
        if (!runner.start(args, &error)) {
            fprintf(stderr, "cantrip: %s\n", qUtf8Printable(error));
            return 1;
        }

        int exitCode = 0;
        QObject::connect(&runner, &cantrip::HeadlessRunner::finished, &app, [&](int code) {
            exitCode = code;
            app.quit();
        });
        app.exec();
        return exitCode;
    }

    QApplication app(argc, argv);
    cantrip::MainWindow window;
    window.show();
    return app.exec();
}
