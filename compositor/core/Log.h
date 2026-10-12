#pragma once

#include <format>
#include <string_view>

namespace dragon {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };

namespace log {

// Reads DRAGON_LOG (trace|debug|info|warn|error|off) and routes libwayland's
// own log output through us. Call once at startup.
void init();

void setLevel(LogLevel level);
LogLevel level();
bool enabled(LogLevel level);

void write(LogLevel level, std::string_view category, std::string_view message);

template <typename... Args>
void print(LogLevel lvl, std::string_view category, std::format_string<Args...> fmt, Args&&... args)
{
    if (enabled(lvl))
        write(lvl, category, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void trace(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
{
    print(LogLevel::Trace, cat, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
{
    print(LogLevel::Debug, cat, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void info(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
{
    print(LogLevel::Info, cat, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
{
    print(LogLevel::Warn, cat, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void error(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
{
    print(LogLevel::Error, cat, fmt, std::forward<Args>(args)...);
}

} // namespace log
} // namespace dragon
