#include "core/Log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <wayland-server-core.h>

namespace dragon::log {

namespace {

LogLevel g_level = LogLevel::Info;
bool g_color = false;
const auto g_start = std::chrono::steady_clock::now();

struct LevelInfo {
    std::string_view name;
    std::string_view color;
};

LevelInfo levelInfo(LogLevel lvl)
{
    switch (lvl) {
    case LogLevel::Trace: return {"TRACE", "\x1b[90m"};
    case LogLevel::Debug: return {"DEBUG", "\x1b[36m"};
    case LogLevel::Info:  return {"INFO ", "\x1b[32m"};
    case LogLevel::Warn:  return {"WARN ", "\x1b[33m"};
    case LogLevel::Error: return {"ERROR", "\x1b[31m"};
    case LogLevel::Off:   break;
    }
    return {"?????", ""};
}

void waylandLogHandler(const char* fmt, va_list args)
{
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    std::string_view msg(buf);
    if (msg.ends_with('\n'))
        msg.remove_suffix(1);
    write(LogLevel::Warn, "wayland", msg);
}

} // namespace

void init()
{
    g_color = isatty(STDERR_FILENO);

    if (const char* env = std::getenv("DRAGON_LOG")) {
        std::string_view v(env);
        if (v == "trace") g_level = LogLevel::Trace;
        else if (v == "debug") g_level = LogLevel::Debug;
        else if (v == "info") g_level = LogLevel::Info;
        else if (v == "warn") g_level = LogLevel::Warn;
        else if (v == "error") g_level = LogLevel::Error;
        else if (v == "off") g_level = LogLevel::Off;
    }

    wl_log_set_handler_server(waylandLogHandler);
}

void setLevel(LogLevel lvl) { g_level = lvl; }
LogLevel level() { return g_level; }
bool enabled(LogLevel lvl) { return lvl != LogLevel::Off && lvl >= g_level; }

void write(LogLevel lvl, std::string_view category, std::string_view message)
{
    using namespace std::chrono;
    const double secs = duration<double>(steady_clock::now() - g_start).count();
    const auto [name, color] = levelInfo(lvl);

    // One fprintf per line so concurrent writers (threads, child processes) don't interleave.
    std::fprintf(stderr, "[%10.4f] %s%.*s%s %.*s: %.*s\n", secs,
                 g_color ? color.data() : "", int(name.size()), name.data(), g_color ? "\x1b[0m" : "",
                 int(category.size()), category.data(), int(message.size()), message.data());
}

} // namespace dragon::log
