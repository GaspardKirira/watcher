#include <watcher/watcher.hpp>

#include <iostream>
#include <string>

int main(int argc, char **argv)
{
  const std::string root = (argc >= 2) ? std::string(argv[1]) : std::string(".");

  watcher::watcher w;
  watcher::options opts;
  opts.debounce = std::chrono::milliseconds(150);
  opts.recursive = false;

  w.start(root, [&](const std::vector<watcher::event> &evs)
          {
      for (const auto& e : evs)
      {
        std::cout << e.path << "\n";
      }
      std::cout.flush(); }, opts);

  std::cout << "Watching (non-recursive): " << root << "\n";
  std::cout << "Press Enter to stop.\n";
  std::string line;
  std::getline(std::cin, line);

  w.stop();
  return 0;
}
