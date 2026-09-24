#include "ui/IndexFileWatcher.h"

#include <QFileInfo>

#include <utility>

namespace indexed {

IndexFileWatcher::IndexFileWatcher(QString path, QObject* parent)
    : QObject(parent), path_(std::move(path)) {
    watcher_.addPath(QFileInfo(path_).absolutePath());
    if (QFileInfo::exists(path_)) {
        watcher_.addPath(path_);
    }
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &IndexFileWatcher::OnFileChanged);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            &IndexFileWatcher::OnDirectoryChanged);
}

bool IndexFileWatcher::Rearm() {
    if (watcher_.files().contains(path_) || !QFileInfo::exists(path_)) {
        return false;
    }
    watcher_.addPath(path_);
    return true;
}

void IndexFileWatcher::OnFileChanged(const QString& /*path*/) {
    Rearm();
    emit Changed();
}

void IndexFileWatcher::OnDirectoryChanged(const QString& /*path*/) {
    // Only a file that wasn't being watched is news here; a replacement of a
    // watched file is already reported by OnFileChanged.
    if (Rearm()) {
        emit Changed();
    }
}

}  // namespace indexed
