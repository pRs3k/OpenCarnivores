// SOURCEPORT: file-change watcher. See HotReload.h.

#include "HotReload.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <sys/stat.h>
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
  #include <sys/types.h>
  typedef struct _stat64 StatT;
  static int stat_file(const char* p, StatT* s) { return _stat64(p, s); }
#else
  typedef struct stat StatT;
  static int stat_file(const char* p, StatT* s) { return ::stat(p, s); }
#endif

namespace {

struct WatchEntry {
    std::string path;
    std::function<void()> cb;
    int64_t lastMtime;   // -1 = not yet observed
};

std::vector<WatchEntry> g_watches;
uint32_t g_lastTickMs = 0;
size_t   g_nextIdx    = 0;

uint32_t NowMs() {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    return 0;
#endif
}

int64_t GetMtime(const char* path) {
    StatT st;
    if (stat_file(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

} // anon

namespace HotReload {

void Watch(const char* path, std::function<void()> onChange) {
    if (!path || !onChange) return;
    WatchEntry w;
    w.path = path;
    w.cb = std::move(onChange);
    w.lastMtime = GetMtime(path);  // don't fire on first registration
    g_watches.push_back(std::move(w));
}

void Tick() {
    // SOURCEPORT: stutter fix. With hundreds of registered textures, stat()'ing
    // every file on each tick produced multi-millisecond hitches every ~333 ms
    // (visible as periodic stutters even at steady 144 Hz). Now amortised:
    // poll a small round-robin slice per tick at a slower 100 ms cadence, so
    // each syscall batch is <1 ms and the full set is swept every few seconds —
    // plenty fast for edit-and-save dev reload.
    uint32_t now = NowMs();
    if (now - g_lastTickMs < 100) return;
    g_lastTickMs = now;

    if (g_watches.empty()) return;
    constexpr size_t kBudgetPerTick = 8;
    size_t n = g_watches.size();
    size_t scan = n < kBudgetPerTick ? n : kBudgetPerTick;

    for (size_t i = 0; i < scan; ++i) {
        if (g_nextIdx >= n) g_nextIdx = 0;
        auto& w = g_watches[g_nextIdx++];
        int64_t m = GetMtime(w.path.c_str());
        if (m < 0) continue;
        if (w.lastMtime < 0) { w.lastMtime = m; continue; }
        if (m != w.lastMtime) {
            w.lastMtime = m;
            if (w.cb) w.cb();
        }
    }
}

void Shutdown() {
    g_watches.clear();
}

} // namespace HotReload
