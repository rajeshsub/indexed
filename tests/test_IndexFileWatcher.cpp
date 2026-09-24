#include <QSignalSpy>
#include <QTest>

#include "ui/IndexFileWatcher.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using indexed::IndexFileWatcher;

namespace {

// Publishes `contents` at `path` the same way IndexSerializer::Save does:
// write a sibling temp file, then rename it over the target.
void ReplaceAtomically(const std::string& path, const std::string& contents) {
    const std::string tempPath = path + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        out << contents;
    }
    QVERIFY(std::rename(tempPath.c_str(), path.c_str()) == 0);
}

// One replacement can be reported twice (file and directory notifications);
// let those land and forget them so the next wait() only passes on a new
// replacement being noticed.
void SettleAndClear(QSignalSpy& spy) {
    QTest::qWait(200);
    spy.clear();
}

}  // namespace

class TestIndexFileWatcher : public QObject {
    Q_OBJECT

private slots:
    void init() {
        dir_ = std::filesystem::temp_directory_path() /
               ("indexed_test_index_file_watcher_" +
                std::to_string(QCoreApplication::applicationPid()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        path_ = (dir_ / "indexed.idx").string();
    }

    void cleanup() { std::filesystem::remove_all(dir_); }

    void emitsChangedForEveryAtomicReplacement() {
        std::ofstream(path_) << "v0";
        IndexFileWatcher watcher(QString::fromStdString(path_));
        QSignalSpy changed(&watcher, &IndexFileWatcher::Changed);

        ReplaceAtomically(path_, "v1");
        QVERIFY(changed.wait(2000));
        // One save must mean one reload, even though Qt reports it twice.
        QTest::qWait(400);
        QCOMPARE(changed.count(), 1);

        SettleAndClear(changed);
        ReplaceAtomically(path_, "v2");
        QVERIFY(changed.wait(2000));
    }

    void picksUpAFileThatDidNotExistAtConstruction() {
        IndexFileWatcher watcher(QString::fromStdString(path_));
        QSignalSpy changed(&watcher, &IndexFileWatcher::Changed);

        ReplaceAtomically(path_, "v1");
        QVERIFY(changed.wait(2000));

        SettleAndClear(changed);
        ReplaceAtomically(path_, "v2");
        QVERIFY(changed.wait(2000));
    }

private:
    std::filesystem::path dir_;
    std::string path_;
};

QTEST_MAIN(TestIndexFileWatcher)
#include "test_IndexFileWatcher.moc"
