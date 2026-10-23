#include "platform/log.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace yawa {
namespace {

std::string_view levelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "INFO";
}

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

} // namespace

bool Log::open(const std::string& path)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    m_path = path;
    m_file.open(path, std::ios::app | std::ios::binary);
    return m_file.is_open();
}

void Log::rotateIfNeeded()
{
    std::error_code ignore;
    const std::uintmax_t size = std::filesystem::file_size(m_path, ignore);
    if (ignore || size < kMaxFileBytes) {
        return;
    }

    m_file.close();
    std::filesystem::rename(m_path, m_path + ".1", ignore);
    m_file.open(m_path, std::ios::app | std::ios::binary);
}

void Log::write(LogLevel level, std::string_view message)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_file.is_open()) {
        return;
    }

    rotateIfNeeded();
    m_file << timestamp() << " [" << levelTag(level) << "] " << message << '\n';
    m_file.flush();
}

} // namespace yawa
