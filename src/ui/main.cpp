#include <QApplication>
#include <QMainWindow>

#include "Version.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle(
        QStringLiteral("indexed v%1").arg(QString::fromUtf8(indexed::GetVersionString())));
    window.resize(900, 600);
    window.show();

    return app.exec();
}
