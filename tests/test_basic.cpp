#include <watcher/watcher.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static void write_file(const fs::path &p, const std::string &content)
{
  fs::create_directories(p.parent_path());
  std::ofstream out(p.string(), std::ios::binary);
  out << content;
  out.flush();
}

int main()
{
  fs::remove_all("watcher_test");
  fs::create_directory("watcher_test");

  std::mutex mu;
  std::vector<watcher::event> got;

  watcher::watcher w;

  watcher::options opts;
  opts.debounce = std::chrono::milliseconds(100);
  opts.recursive = true;
  opts.extensions_allowlist = {".txt"};

  w.start("watcher_test", [&](const std::vector<watcher::event> &evs)
          {
            std::lock_guard<std::mutex> lk(mu);
            got.insert(got.end(), evs.begin(), evs.end()); }, opts);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  write_file("watcher_test/a.txt", "hello");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  w.stop();

  {
    std::lock_guard<std::mutex> lk(mu);
    assert(!got.empty());
    bool ok = false;
    for (const auto &e : got)
    {
      if (e.path == "a.txt" && (e.type == watcher::event_type::created || e.type == watcher::event_type::modified))
      {
        ok = true;
        break;
      }
    }
    assert(ok && "expected create/modify event for a.txt");
  }

  fs::remove_all("watcher_test");
  return 0;
}
