#include <watcher/watcher.hpp>

#include <iostream>
#include <mutex>
#include <string>

static const char *to_string(watcher::event_type t)
{
  switch (t)
  {
  case watcher::event_type::created:
    return "created";
  case watcher::event_type::modified:
    return "modified";
  case watcher::event_type::removed:
    return "removed";
  case watcher::event_type::renamed:
    return "renamed";
  default:
    return "unknown";
  }
}

int main(int argc, char **argv)
{
  const std::string root = (argc >= 2) ? std::string(argv[1]) : std::string(".");

  watcher::watcher w;
  watcher::options opts;
  opts.debounce = std::chrono::milliseconds(150);
  opts.recursive = true;

  std::mutex io;

  w.start(root, [&](const std::vector<watcher::event> &evs)
          {
      std::lock_guard<std::mutex> lk(io);
      for (const auto& e : evs)
      {
        std::cout << "[" << to_string(e.type) << "] " << e.path << "\n";
      }
      std::cout.flush(); }, opts);

  std::cout << "Watching: " << root << "\n";
  std::cout << "Press Enter to stop.\n";
  std::string line;
  std::getline(std::cin, line);

  w.stop();
  return 0;
}
