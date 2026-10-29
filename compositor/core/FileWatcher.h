#pragma once

#include "core/EventLoop.h"
#include "core/Signal.h"
#include "core/UniqueFd.h"

#include <filesystem>

namespace dragon {

// Notices when a file is written, replaced or created (inotify). It watches
// the containing directory, since editors usually save by renaming a new file
// over the old one. Bursts of events are collapsed into one `changed`.
class FileWatcher {
public:
    FileWatcher(EventLoop& loop, std::filesystem::path file);
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // False if the directory doesn't exist (yet) or inotify failed.
    bool watching() const { return m_watch >= 0; }

    Signal<> changed;

private:
    void onReadable();

    std::filesystem::path m_file;
    UniqueFd m_fd;
    int m_watch = -1;
    EventSource m_source;
    EventSource m_settle;
};

} // namespace dragon
