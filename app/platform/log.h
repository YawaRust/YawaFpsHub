#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace yawa {

enum class LogLevel { Info, Warning, Error };

// Лог рядом с exe: портативность означает, что ничего не пишем в профиль пользователя.
// Файл режется по размеру, чтобы долгая сессия не съела диск.
class Log {
public:
    static constexpr std::uintmax_t kMaxFileBytes = 1024 * 1024;

    bool open(const std::string& path);
    void write(LogLevel level, std::string_view message);
    void info(std::string_view message) { write(LogLevel::Info, message); }
    void warning(std::string_view message) { write(LogLevel::Warning, message); }
    void error(std::string_view message) { write(LogLevel::Error, message); }

private:
    void rotateIfNeeded();

    std::mutex m_mutex;
    std::ofstream m_file;
    std::string m_path;
};

} // namespace yawa
