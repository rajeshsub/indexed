#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

namespace indexed {

// Emits Changed() every time the file at `path` is modified or replaced.
// QFileSystemWatcher alone stops watching a file once it is replaced by
// rename(2) (the inode it watched is gone), which is exactly how
// IndexSerializer::Save and ReplaceFileForRootWrite publish a new index --
// so a bare watcher reports only the first save and misses every later one.
// This re-adds the path after each change so every replacement is seen, and
// watches the containing directory so a file that doesn't exist yet (or
// vanished) is picked up once it appears.
class IndexFileWatcher : public QObject {
    Q_OBJECT

public:
    explicit IndexFileWatcher(QString path, QObject* parent = nullptr);

signals:
    void Changed();

private:
    // Re-adds path_ to the watcher if it exists but isn't watched; true if it did.
    bool Rearm();
    void OnFileChanged(const QString& path);
    void OnDirectoryChanged(const QString& path);

    QString path_;
    QFileSystemWatcher watcher_;
};

}  // namespace indexed
