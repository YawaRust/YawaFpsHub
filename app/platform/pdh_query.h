#pragma once

#include <string>
#include <utility>
#include <vector>

namespace yawa {

// PDH — тот же источник, что у диспетчера задач, поэтому значения совпадают с ним.
// Имена счётчиков берутся английские: на русской Windows локализованные имена
// PdhAddCounter не понимает, а PdhAddEnglishCounter работает на любой локали.
class PdhCounter {
public:
    PdhCounter() = default;
    ~PdhCounter();

    PdhCounter(const PdhCounter&) = delete;
    PdhCounter& operator=(const PdhCounter&) = delete;

    bool open(const wchar_t* path);
    void close();

    bool collect();
    double value() const { return m_value; }
    bool isOpen() const { return m_counter != nullptr; }

private:
    void* m_query = nullptr;
    void* m_counter = nullptr;
    double m_value = 0.0;
};

class PdhWildcardCounter {
public:
    PdhWildcardCounter() = default;
    ~PdhWildcardCounter();

    PdhWildcardCounter(const PdhWildcardCounter&) = delete;
    PdhWildcardCounter& operator=(const PdhWildcardCounter&) = delete;

    bool open(const wchar_t* path);
    void close();

    bool collect();
    bool isOpen() const { return m_counter != nullptr; }

    // Имя экземпляра и значение: экземпляры вида pid_1234_luid_0x..., по ним и фильтруем.
    const std::vector<std::pair<std::wstring, double>>& values() const { return m_values; }

private:
    void* m_query = nullptr;
    void* m_counter = nullptr;
    std::vector<std::pair<std::wstring, double>> m_values;
};

} // namespace yawa
