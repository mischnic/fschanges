#include <string>
#include <sstream>
#include <stack>
#include "../DirTree.hh"
#include "../shared/BruteForceBackend.hh"
#include "./WindowsBackend.hh"
#include "./win_utils.hh"

#define CONVERT_TIME(ft) ULARGE_INTEGER{ft.dwLowDateTime, ft.dwHighDateTime}.QuadPart

void BruteForceBackend::readTree(Watcher &watcher, std::shared_ptr<DirTree> tree) {
  HANDLE hFind = INVALID_HANDLE_VALUE;
  std::stack<std::string> directories;
  
  directories.push(watcher.mDir);

  while (!directories.empty()) {
    std::string path = directories.top();
    std::string spec = path + "\\*";
    directories.pop();

    WIN32_FIND_DATA ffd;
    hFind = FindFirstFile(spec.c_str(), &ffd);

    if (hFind == INVALID_HANDLE_VALUE)  {
      tree->remove(path);
      continue;
    }

    do {
      if (strcmp(ffd.cFileName, ".") != 0 && strcmp(ffd.cFileName, "..") != 0) {
        std::string fullPath = path + "\\" + ffd.cFileName;
        if (watcher.mIgnore.count(fullPath) > 0) {
          continue;
        }

        tree->add(fullPath, CONVERT_TIME(ffd.ftLastWriteTime), ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          directories.push(fullPath);
        }
      }
    } while (FindNextFile(hFind, &ffd) != 0);
  }

  FindClose(hFind);
}

void WindowsBackend::start() {
  mRunning = true;
  notifyStarted();

  while (mRunning) {
    SleepEx(INFINITE, true);
  }
}

WindowsBackend::~WindowsBackend() {
  std::unique_lock<std::mutex> lock(mMutex);

  // Mark as stopped, and queue a noop function in the thread to break the loop
  mRunning = false;
  QueueUserAPC([](__in ULONG_PTR) {}, mThread.native_handle(), (ULONG_PTR)this);
}

class Subscription {
public:
  Subscription(Watcher *watcher, std::shared_ptr<DirTree> tree) {
    mRunning = true;
    mWatcher = watcher;
    mTree = tree;
    ZeroMemory(&mOverlapped, sizeof(OVERLAPPED));
    mOverlapped.hEvent = this;
    mReadBuffer.resize(1024 * 1024);
    mWriteBuffer.resize(1024 * 1024);

    mDirectoryHandle = CreateFileW(
      utf8ToUtf16(watcher->mDir).data(),
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      NULL
    );

    if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
      throw "Invalid handle";
    }
  }

  ~Subscription() {
    mRunning = false;
    CancelIo(mDirectoryHandle);
    CloseHandle(mDirectoryHandle);
  }

  void poll() {
    if (!mRunning) {
      return;
    }

    // Asynchronously wait for changes.
    int success = ReadDirectoryChangesW(
      mDirectoryHandle,
      mWriteBuffer.data(),
      static_cast<DWORD>(mWriteBuffer.size()),
      TRUE, // recursive
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES
        | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
      NULL,
      &mOverlapped,
      [](DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
        auto subscription = reinterpret_cast<Subscription *>(overlapped->hEvent);
        subscription->processEvents(errorCode);
      }
    );

    if (!success) {
      throw "Unexpected shutdown";
    }
  }

  void processEvents(DWORD errorCode) {
    if (!mRunning) {
      return;
    }
    
    // TODO: error handling
    switch (errorCode) {
      case ERROR_OPERATION_ABORTED:
        return;
      case ERROR_INVALID_PARAMETER:
        return;
      case ERROR_NOTIFY_ENUM_DIR:
        return;
      default:
        if (errorCode != ERROR_SUCCESS) {
          throw "Unknown error";
        }
    }

    // Swap read and write buffers, and poll again
    std::swap(mWriteBuffer, mReadBuffer);
    poll();

    // Read change events
    BYTE *base = mReadBuffer.data();
    while (true) {
      PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)base;
      processEvent(info);

      if (info->NextEntryOffset == 0) {
        break;
      }

      base += info->NextEntryOffset;
    }

    mWatcher->notify();
  }

  void processEvent(PFILE_NOTIFY_INFORMATION info) {
    std::string path = mWatcher->mDir + "\\" + utf16ToUtf8(info->FileName, info->FileNameLength / sizeof(WCHAR));
    if (mWatcher->isIgnored(path)) {
      return;
    }

    switch (info->Action) {
      case FILE_ACTION_ADDED:
      case FILE_ACTION_RENAMED_NEW_NAME: {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(utf8ToUtf16(path).data(), GetFileExInfoStandard, &data)) {
          mWatcher->mEvents.create(path);
          mTree->add(path, CONVERT_TIME(data.ftLastWriteTime), data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        }
        break;
      }
      case FILE_ACTION_MODIFIED: {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(utf8ToUtf16(path).data(), GetFileExInfoStandard, &data)) {
          mTree->update(path, CONVERT_TIME(data.ftLastWriteTime));
          if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            mWatcher->mEvents.update(path);
          }
        }
        break;
      }
      case FILE_ACTION_REMOVED:
      case FILE_ACTION_RENAMED_OLD_NAME:
        mWatcher->mEvents.remove(path);
        mTree->remove(path);
        break;
    }
  }

private:
  Watcher *mWatcher;
  std::shared_ptr<DirTree> mTree;
  bool mRunning;
  HANDLE mDirectoryHandle;
  std::vector<BYTE> mReadBuffer;
  std::vector<BYTE> mWriteBuffer;
  OVERLAPPED mOverlapped;
};

void WindowsBackend::subscribe(Watcher &watcher) {
  // Create a subscription for this watcher
  Subscription *sub = new Subscription(&watcher, getTree(watcher, false));
  watcher.state = (void *)sub;

  // Queue polling for this subscription in the correct thread.
  QueueUserAPC([](__in ULONG_PTR ptr) {
    Subscription *sub = (Subscription *)ptr;
    sub->poll();
  }, mThread.native_handle(), (ULONG_PTR)sub);
}

void WindowsBackend::unsubscribe(Watcher &watcher) {
  Subscription *sub = (Subscription *)watcher.state;
  delete sub;
}
