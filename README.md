# watcher

Cross-platform filesystem watcher for modern C++.

`watcher` provides deterministic directory monitoring with built-in debounce,
designed for hot reload workflows and development tools.

It supports recursive watching, extension filtering, and batched event delivery.

Header-only. Zero external dependencies.

---

## Download

https://vixcpp.com/registry/pkg/gaspardkirira/watcher

## Why watcher?

Filesystem monitoring appears everywhere:

- Hot reload development servers
- Build systems
- Compilers and bundlers
- Static analyzers
- Asset pipelines
- Live configuration reload

Using raw OS APIs directly often leads to:

- Platform-specific code
- Complex event handling
- Event spam during rebuilds
- Inconsistent behavior
- Missing debounce logic

This library provides:

- Cross-platform backend
- Recursive directory watching
- Built-in debounce batching
- Extension allowlist filtering
- Deterministic event ordering
- Clean callback-based API

No heavy framework.
No async runtime dependency.
No event loop required.

Just explicit filesystem watching primitives.

## Installation

### Using Vix Registry

```bash
vix add gaspardkirira/watcher
vix deps
```

### Manual

```bash
git clone https://github.com/GaspardKirira/watcher.git
```

Add the `include/` directory to your project.

## Dependency

Requires C++17 or newer.

Uses:

- `<filesystem>`
- OS native APIs:
  - Linux: inotify
  - Windows: ReadDirectoryChangesW
  - macOS: kqueue

No external libraries.

## Quick Examples

### Basic Watch

```cpp
#include <watcher/watcher.hpp>
#include <iostream>

int main()
{
    watcher::watcher w;

    watcher::options opts;
    opts.recursive = true;
    opts.debounce = std::chrono::milliseconds(150);

    w.start(".", [](const std::vector<watcher::event>& evs)
    {
        for (const auto& e : evs)
            std::cout << e.path << "\n";
    }, opts);

    std::cin.get();
    w.stop();
}
```

### Hot Reload Trigger

```cpp
#include <watcher/watcher.hpp>
#include <iostream>

int main()
{
    watcher::watcher w;

    watcher::options opts;
    opts.recursive = true;
    opts.debounce = std::chrono::milliseconds(250);
    opts.extensions_allowlist = {".cpp", ".hpp"};

    w.start(".", [](const std::vector<watcher::event>&)
    {
        std::cout << "Rebuild triggered\n";
    }, opts);

    std::cin.get();
    w.stop();
}
```

## API Overview

```cpp
watcher::watcher w;

w.start(root, callback, options);
w.stop();

w.running();
```

## Event

```cpp
struct event
{
    event_type type;
    std::string path; // relative to root
};
```

## Event Types

```cpp
enum class event_type
{
    created,
    modified,
    removed,
    renamed
};
```

## Options

```cpp
struct options
{
    bool recursive = true;
    bool include_directories = false;
    std::chrono::milliseconds debounce{150};
    std::vector<std::string> extensions_allowlist{};
};
```

## Debounce Semantics

Filesystem APIs can generate multiple events for a single action
(for example save operations in editors).

`watcher` batches events within a debounce window:

- Events collected for the debounce duration
- Delivered as a single batch
- Duplicate `(type, path)` events removed
- Deterministic sorted output

This makes it ideal for:

- Hot reload servers
- Rebuild triggers
- Dev tool pipelines

## Platform Notes

### Linux

- Uses inotify
- Efficient and recursive

### Windows

- Uses ReadDirectoryChangesW
- Supports recursive monitoring

### macOS

- Uses kqueue (coarse-grained)
- Emits directory-level signals
- Suitable for hot reload workflows

## Complexity

Let:

- E = number of filesystem events
- B = number of events per batch

| Operation            | Time Complexity |
|---------------------|----------------|
| OS event handling   | O(E)           |
| Batch deduplication | O(B log B)     |
| Callback dispatch   | O(B)           |

Memory usage is proportional to batch size.

## Semantics

- Paths are relative to `root`.
- Output is lexicographically sorted.
- Duplicate events removed per batch.
- `stop()` is safe and joins the internal thread.
- Destructor stops watcher automatically.

## Design Principles

- Deterministic batching
- Minimal API surface
- Explicit lifecycle control
- Cross-platform consistency
- Zero runtime dependencies

This library provides primitives only.

If you need:

- Async integration
- Event loop integration
- Advanced gitignore matching
- File content diffing
- Distributed file watching

Build them on top of this layer.

## Tests

```bash
vix build
vix tests
```

Tests verify:

- Watcher start/stop lifecycle
- File creation detection
- Debounce batching
- Extension filtering

## License

MIT License\
Copyright (c) Gaspard Kirira

