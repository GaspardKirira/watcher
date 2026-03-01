#include <watcher/watcher.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static bool is_source_file(const std::string &p)
{
  auto ends_with = [&](const char *ext)
  {
    const std::string e(ext);
    return p.size() >= e.size() && p.compare(p.size() - e.size(), e.size(), e) == 0;
  };

  return ends_with(".cpp") || ends_with(".hpp") || ends_with(".c") || ends_with(".h");
}

int main(int argc, char **argv)
{
  const std::string root = (argc >= 2) ? std::string(argv[1]) : std::string(".");

  watcher::watcher w;
  watcher::options opts;
  opts.debounce = std::chrono::milliseconds(250);
  opts.recursive = true;

  std::mutex mu;
  std::atomic<bool> pending{false};

  w.start(root, [&](const std::vector<watcher::event> &evs)
          {
      bool relevant = false;
      for (const auto& e : evs)
      {
        if (is_source_file(e.path))
        {
          relevant = true;
          break;
        }
      }
      if (relevant)
      {
        pending.store(true);
      } }, opts);

  std::cout << "Hot reload watcher running on: " << root << "\n";
  std::cout << "Press Enter to stop.\n";

  std::thread runner([&]()
                     {
    while (w.running())
    {
      if (pending.exchange(false))
      {
        // Here you can call your build tool.
        // Keep it simple in this example: print only.
        std::lock_guard<std::mutex> lk(mu);
        std::cout << "Change detected -> rebuild trigger\n";
        std::cout.flush();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } });

  std::string line;
  std::getline(std::cin, line);

  w.stop();
  runner.join();
  return 0;
}
