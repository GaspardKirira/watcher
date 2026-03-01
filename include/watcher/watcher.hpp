/**
 * @file watcher.hpp
 * @brief Cross-platform filesystem watcher with debounce for hot reload workflows.
 *
 * `watcher` provides a small API to watch a directory tree and receive file change
 * events (create/modify/remove/rename). Events are coalesced using a debounce
 * window to avoid spamming during rebuilds.
 *
 * Backends:
 * - Linux: inotify
 * - Windows: ReadDirectoryChangesW
 * - macOS: kqueue (fallback)
 *
 * Header-only. Zero external dependencies.
 *
 * Requirements: C++17+
 */

#ifndef WATCHER_WATCHER_HPP
#define WATCHER_WATCHER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace watcher
{
  namespace fs = std::filesystem;

  enum class event_type
  {
    created,
    modified,
    removed,
    renamed
  };

  struct event
  {
    event_type type{};
    std::string path; // relative to root (generic_string)
  };

  struct options
  {
    bool recursive = true;
    bool include_directories = false;

    // Debounce window for batching events.
    std::chrono::milliseconds debounce{150};

    // If non-empty, only these extensions are reported (".cpp", ".hpp", ...).
    std::vector<std::string> extensions_allowlist{};
  };

  using callback_t = std::function<void(const std::vector<event> &)>;

  namespace detail
  {
    inline std::string to_generic_relative(const fs::path &root, const fs::path &p)
    {
      std::error_code ec;
      fs::path rel = fs::relative(p, root, ec);
      if (ec)
      {
        // fallback: best-effort
        rel = p.filename();
      }
      return rel.generic_string();
    }

    inline bool has_allowed_extension(const std::vector<std::string> &allow, const fs::path &p)
    {
      if (allow.empty())
        return true;

      const std::string ext = p.extension().string();
      for (const auto &a : allow)
      {
        if (ext == a)
          return true;
      }
      return false;
    }

    inline void push_event(std::vector<event> &buf, event_type t, const std::string &rel)
    {
      buf.push_back(event{t, rel});
    }

    inline void sort_and_dedupe(std::vector<event> &events)
    {
      // Dedup by (type,path) to reduce spam.
      std::sort(events.begin(), events.end(),
                [](const event &a, const event &b)
                {
                  if (a.path != b.path)
                    return a.path < b.path;
                  return static_cast<int>(a.type) < static_cast<int>(b.type);
                });

      events.erase(std::unique(events.begin(), events.end(),
                               [](const event &a, const event &b)
                               {
                                 return a.type == b.type && a.path == b.path;
                               }),
                   events.end());
    }
  } // namespace detail

  class watcher
  {
  public:
    watcher() = default;
    watcher(const watcher &) = delete;
    watcher &operator=(const watcher &) = delete;

    watcher(watcher &&other) noexcept
    {
      move_from(std::move(other));
    }

    watcher &operator=(watcher &&other) noexcept
    {
      if (this != &other)
      {
        stop();
        move_from(std::move(other));
      }
      return *this;
    }

    ~watcher()
    {
      stop();
    }

    bool running() const noexcept { return running_.load(); }

    void start(const std::string &root, callback_t cb, options opts = {})
    {
      stop();

      root_ = fs::path(root);
      cb_ = std::move(cb);
      opts_ = std::move(opts);

      if (!fs::exists(root_) || !fs::is_directory(root_))
      {
        throw std::runtime_error("watcher: root is not a directory: " + root);
      }

      running_.store(true);
      worker_ = std::thread([this]()
                            { this->run(); });
    }

    void stop()
    {
      const bool was = running_.exchange(false);
      if (!was)
        return;

      {
        std::lock_guard<std::mutex> lk(mu_);
        wake_.notify_all();
      }

#if defined(_WIN32)
      if (win_stop_event_ != nullptr)
      {
        SetEvent(win_stop_event_);
      }
#endif

      if (worker_.joinable())
      {
        worker_.join();
      }

      cleanup_backend();
    }

  private:
    fs::path root_{};
    callback_t cb_{};
    options opts_{};

    std::atomic<bool> running_{false};
    std::thread worker_{};

    std::mutex mu_{};
    std::condition_variable wake_{};

    // batching
    std::vector<event> batch_{};
    std::chrono::steady_clock::time_point last_event_tp_{};
    bool has_pending_{false};

    void move_from(watcher &&other) noexcept
    {
      other.running_.store(false);

      root_ = std::move(other.root_);
      cb_ = std::move(other.cb_);
      opts_ = std::move(other.opts_);

      if (other.worker_.joinable())
      {
        other.worker_.join();
      }

      running_.store(false);
      has_pending_ = false;
      batch_.clear();

#if defined(__linux__)
      inotify_fd_ = other.inotify_fd_;
      other.inotify_fd_ = -1;
      wd_to_path_ = std::move(other.wd_to_path_);
#elif defined(_WIN32)
      win_dir_ = other.win_dir_;
      other.win_dir_ = INVALID_HANDLE_VALUE;
      win_stop_event_ = other.win_stop_event_;
      other.win_stop_event_ = nullptr;
#elif defined(__APPLE__)
      kq_ = other.kq_;
      other.kq_ = -1;
      fd_to_path_ = std::move(other.fd_to_path_);
#endif
    }

#if defined(__linux__)
    int inotify_fd_ = -1;
    std::unordered_map<int, fs::path> wd_to_path_{};
#elif defined(_WIN32)
    HANDLE win_dir_ = INVALID_HANDLE_VALUE;
    HANDLE win_stop_event_ = nullptr;
#elif defined(__APPLE__)
    int kq_ = -1;
    std::unordered_map<int, fs::path> fd_to_path_{};
#endif

  private:
    void cleanup_backend()
    {
#if defined(__linux__)
      if (inotify_fd_ != -1)
      {
        ::close(inotify_fd_);
        inotify_fd_ = -1;
      }
      wd_to_path_.clear();
#elif defined(_WIN32)
      if (win_dir_ != INVALID_HANDLE_VALUE)
      {
        CloseHandle(win_dir_);
        win_dir_ = INVALID_HANDLE_VALUE;
      }
      if (win_stop_event_ != nullptr)
      {
        CloseHandle(win_stop_event_);
        win_stop_event_ = nullptr;
      }
#elif defined(__APPLE__)
      if (kq_ != -1)
      {
        ::close(kq_);
        kq_ = -1;
      }
      for (auto &kv : fd_to_path_)
      {
        ::close(kv.first);
      }
      fd_to_path_.clear();
#endif
    }

    void emit_batch_locked()
    {
      if (batch_.empty())
      {
        has_pending_ = false;
        return;
      }

      detail::sort_and_dedupe(batch_);
      const auto out = batch_;
      batch_.clear();
      has_pending_ = false;

      // Call user callback outside lock.
      mu_.unlock();
      cb_(out);
      mu_.lock();
    }

    void run()
    {
      init_backend();

      std::unique_lock<std::mutex> lk(mu_);
      while (running_.load())
      {
        // 1) poll backend and push events
        lk.unlock();
        poll_backend();
        lk.lock();

        // 2) debounce flush
        if (has_pending_)
        {
          const auto now = std::chrono::steady_clock::now();
          if (now - last_event_tp_ >= opts_.debounce)
          {
            emit_batch_locked();
          }
        }

        // 3) sleep/wait a bit
        wake_.wait_for(lk, std::chrono::milliseconds(25));
      }

      // flush remaining events
      if (has_pending_)
      {
        emit_batch_locked();
      }
    }

    void init_backend()
    {
#if defined(__linux__)
      inotify_fd_ = ::inotify_init1(IN_NONBLOCK);
      if (inotify_fd_ == -1)
      {
        throw std::runtime_error("watcher: inotify_init1 failed");
      }
      add_linux_watch_recursive(root_);
#elif defined(_WIN32)
      win_dir_ = CreateFileW(
          root_.wstring().c_str(),
          FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          nullptr);

      if (win_dir_ == INVALID_HANDLE_VALUE)
      {
        throw std::runtime_error("watcher: CreateFileW failed");
      }

      win_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!win_stop_event_)
      {
        throw std::runtime_error("watcher: CreateEvent failed");
      }
#elif defined(__APPLE__)
      kq_ = ::kqueue();
      if (kq_ == -1)
      {
        throw std::runtime_error("watcher: kqueue failed");
      }
      add_macos_watch_recursive(root_);
#endif
    }

#if defined(__linux__)
    void add_linux_watch_recursive(const fs::path &dir)
    {
      const uint32_t mask =
          IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
          IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

      const int wd = ::inotify_add_watch(inotify_fd_, dir.c_str(), mask);
      if (wd != -1)
      {
        wd_to_path_[wd] = dir;
      }

      if (!opts_.recursive)
        return;

      for (const auto &e : fs::directory_iterator(dir))
      {
        if (e.is_directory())
        {
          add_linux_watch_recursive(e.path());
        }
      }
    }
#endif

#if defined(__APPLE__)
    void add_macos_watch_recursive(const fs::path &dir)
    {
      const int fd = ::open(dir.c_str(), O_EVTONLY);
      if (fd == -1)
      {
        return;
      }

      struct kevent ev;
      EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
             NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND,
             0, nullptr);

      if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1)
      {
        ::close(fd);
        return;
      }

      fd_to_path_[fd] = dir;

      if (!opts_.recursive)
        return;

      for (const auto &e : fs::directory_iterator(dir))
      {
        if (e.is_directory())
        {
          add_macos_watch_recursive(e.path());
        }
      }
    }
#endif

    void record_event(event_type t, const fs::path &abs_path)
    {
      if (!opts_.include_directories)
      {
        std::error_code ec;
        if (fs::is_directory(abs_path, ec))
          return;
      }

      if (!detail::has_allowed_extension(opts_.extensions_allowlist, abs_path))
      {
        return;
      }

      const std::string rel = detail::to_generic_relative(root_, abs_path);

      std::lock_guard<std::mutex> lk(mu_);
      detail::push_event(batch_, t, rel);
      last_event_tp_ = std::chrono::steady_clock::now();
      has_pending_ = true;
      wake_.notify_all();
    }

    void poll_backend()
    {
#if defined(__linux__)
      poll_linux();
#elif defined(_WIN32)
      poll_windows();
#elif defined(__APPLE__)
      poll_macos();
#else
      // Unsupported platform: do nothing.
#endif
    }

#if defined(__linux__)
    void poll_linux()
    {
      if (inotify_fd_ == -1)
        return;

      alignas(inotify_event) char buf[4096];

      for (;;)
      {
        const ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
        if (n <= 0)
          break;

        std::size_t i = 0;
        while (i < static_cast<std::size_t>(n))
        {
          const auto *ev = reinterpret_cast<const inotify_event *>(buf + i);

          const auto it = wd_to_path_.find(ev->wd);
          fs::path base = (it != wd_to_path_.end()) ? it->second : root_;

          fs::path p = base;
          if (ev->len > 0)
          {
            p /= std::string(ev->name);
          }

          // If a new directory appears, add watch (recursive mode).
          if ((ev->mask & IN_ISDIR) && opts_.recursive && (ev->mask & (IN_CREATE | IN_MOVED_TO)))
          {
            std::error_code ec;
            if (fs::exists(p, ec) && fs::is_directory(p, ec))
            {
              add_linux_watch_recursive(p);
            }
          }

          if (ev->mask & (IN_CREATE | IN_MOVED_TO))
            record_event(event_type::created, p);
          else if (ev->mask & IN_MODIFY)
            record_event(event_type::modified, p);
          else if (ev->mask & IN_DELETE)
            record_event(event_type::removed, p);
          else if (ev->mask & (IN_MOVED_FROM))
            record_event(event_type::renamed, p);

          i += sizeof(inotify_event) + ev->len;
        }
      }
    }
#endif

#if defined(_WIN32)
    void poll_windows()
    {
      if (win_dir_ == INVALID_HANDLE_VALUE)
        return;

      // We poll using ReadDirectoryChangesW in a blocking call with a short timeout strategy:
      // use an overlapped + WaitForMultipleObjects (stop event).
      static constexpr DWORD kBufSize = 64 * 1024;

      std::vector<char> buffer(kBufSize);
      OVERLAPPED ov{};
      ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

      DWORD bytes = 0;

      const DWORD notifyFilter =
          FILE_NOTIFY_CHANGE_FILE_NAME |
          FILE_NOTIFY_CHANGE_DIR_NAME |
          FILE_NOTIFY_CHANGE_LAST_WRITE |
          FILE_NOTIFY_CHANGE_SIZE;

      const BOOL ok = ReadDirectoryChangesW(
          win_dir_,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          opts_.recursive ? TRUE : FALSE,
          notifyFilter,
          &bytes,
          &ov,
          nullptr);

      if (!ok)
      {
        CloseHandle(ov.hEvent);
        return;
      }

      HANDLE handles[2] = {ov.hEvent, win_stop_event_};
      const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 100);

      if (wait == WAIT_OBJECT_0 + 1)
      {
        CancelIoEx(win_dir_, &ov);
        CloseHandle(ov.hEvent);
        return;
      }

      if (wait != WAIT_OBJECT_0)
      {
        // timeout
        CancelIoEx(win_dir_, &ov);
        CloseHandle(ov.hEvent);
        return;
      }

      DWORD transferred = 0;
      if (!GetOverlappedResult(win_dir_, &ov, &transferred, FALSE))
      {
        CloseHandle(ov.hEvent);
        return;
      }

      char *ptr = buffer.data();
      while (true)
      {
        auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(ptr);

        std::wstring wname(info->FileName, info->FileNameLength / sizeof(WCHAR));
        fs::path abs = root_ / fs::path(wname);

        switch (info->Action)
        {
        case FILE_ACTION_ADDED:
          record_event(event_type::created, abs);
          break;
        case FILE_ACTION_REMOVED:
          record_event(event_type::removed, abs);
          break;
        case FILE_ACTION_MODIFIED:
          record_event(event_type::modified, abs);
          break;
        case FILE_ACTION_RENAMED_OLD_NAME:
        case FILE_ACTION_RENAMED_NEW_NAME:
          record_event(event_type::renamed, abs);
          break;
        default:
          break;
        }

        if (info->NextEntryOffset == 0)
          break;

        ptr += info->NextEntryOffset;
      }

      CloseHandle(ov.hEvent);
    }
#endif

#if defined(__APPLE__)
    void poll_macos()
    {
      if (kq_ == -1)
        return;

      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 50 * 1000 * 1000; // 50ms

      struct kevent ev;
      const int n = ::kevent(kq_, nullptr, 0, &ev, 1, &ts);
      if (n <= 0)
        return;

      const int fd = static_cast<int>(ev.ident);
      const auto it = fd_to_path_.find(fd);
      const fs::path base = (it != fd_to_path_.end()) ? it->second : root_;

      // kqueue vnode gives coarse directory-level signals.
      // We emit a "modified" event for the directory, and hot reload can rescan.
      record_event(event_type::modified, base);

      // If recursive, try to refresh watches by discovering new directories.
      if (opts_.recursive)
      {
        for (const auto &e : fs::recursive_directory_iterator(root_))
        {
          if (e.is_directory())
          {
            // If not already watched, add it.
            bool watched = false;
            for (const auto &kv : fd_to_path_)
            {
              if (kv.second == e.path())
              {
                watched = true;
                break;
              }
            }
            if (!watched)
            {
              add_macos_watch_recursive(e.path());
            }
          }
        }
      }
    }
#endif
  };

  /**
   * @brief Convenience helper: start a watcher and return the instance.
   */
  inline watcher watch(const std::string &root, callback_t cb, options opts = {})
  {
    watcher w;
    w.start(root, std::move(cb), std::move(opts));
    return std::move(w);
  }
} // namespace watcher

#endif // WATCHER_WATCHER_HPP
