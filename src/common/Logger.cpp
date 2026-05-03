#include "Logger.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace aerochord {

namespace {

// Mutex global protege a escrita em stderr (sink padrão)
std::mutex g_logMutex;

constexpr std::array<const char*, 5> kLevelNames{
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

std::string currentTimestamp() {
    using namespace std::chrono;
    const auto now    = system_clock::now();
    const auto micros = duration_cast<microseconds>(now.time_since_epoch()) % 1'000'000;
    const std::time_t tt = system_clock::to_time_t(now);

    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&tt));

    char full[48]{};
    std::snprintf(full, sizeof(full), "%s.%06lld", buf,
                  static_cast<long long>(micros.count()));
    return full;
}

} // namespace

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::setLevel(LogLevel level) {
    minLevel_ = level;
}

LogLevel Logger::level() const {
    return minLevel_;
}

void Logger::log(LogLevel level,
                 std::string_view module,
                 std::string_view message) const {
    if (level < minLevel_)
        return;

    const std::string ts      = currentTimestamp();
    const auto        lvlIdx  = static_cast<size_t>(level);
    const char*       lvlName = (lvlIdx < kLevelNames.size()) ? kLevelNames[lvlIdx] : "?";

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::fprintf(stderr, "[%s] [%-5s] [%-20.*s] %.*s\n",
                 ts.c_str(),
                 lvlName,
                 static_cast<int>(module.size()),  module.data(),
                 static_cast<int>(message.size()), message.data());
}

void Logger::dbg    (std::string_view m, std::string_view msg) const { log(LogLevel::DBG,     m, msg); }
void Logger::info   (std::string_view m, std::string_view msg) const { log(LogLevel::INFO,    m, msg); }
void Logger::warning(std::string_view m, std::string_view msg) const { log(LogLevel::WARNING, m, msg); }
void Logger::err    (std::string_view m, std::string_view msg) const { log(LogLevel::ERR,     m, msg); }
void Logger::fatal  (std::string_view m, std::string_view msg) const { log(LogLevel::FATAL,   m, msg); }

} // namespace aerochord
