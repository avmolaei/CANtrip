#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    cantrip::MainWindow window;
    window.show();
    return app.exec();
}
