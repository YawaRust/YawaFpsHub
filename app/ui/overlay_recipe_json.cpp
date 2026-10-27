#include "ui/settings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <utility>

namespace yawa {
namespace {

    class RecipeReader
    {
    public:
        explicit RecipeReader(std::string_view json)
            : m_json(json)
        {
        }

        bool take(char expected)
        {
            skipSpace();
            if (m_pos == m_json.size() || m_json[m_pos] != expected)
                return false;
            ++m_pos;
            return true;
        }

        bool peek(char expected)
        {
            skipSpace();
            return m_pos < m_json.size() && m_json[m_pos] == expected;
        }

        bool finished()
        {
            skipSpace();
            return m_pos == m_json.size();
        }

        bool readNull()
        {
            skipSpace();
            if (m_json.substr(m_pos, 4) != "null")
                return false;
            m_pos += 4;
            return true;
        }

        bool readNumber(int& result)
        {
            skipSpace();
            const std::size_t start = m_pos;
            while (m_pos < m_json.size() && m_json[m_pos] >= '0' && m_json[m_pos] <= '9')
                ++m_pos;
            if (start == m_pos || (m_pos - start > 1 && m_json[start] == '0'))
                return false;
            const auto parsed = std::from_chars(m_json.data() + start, m_json.data() + m_pos, result);
            return parsed.ec == std::errc { } && parsed.ptr == m_json.data() + m_pos;
        }

        bool readString(std::string& result)
        {
            if (!take('"'))
                return false;
            result.clear();
            while (m_pos < m_json.size()) {
                const unsigned char symbol = static_cast<unsigned char>(m_json[m_pos++]);
                if (symbol == '"')
                    return true;
                if (symbol >= 0x20 && symbol <= 0x7e && symbol != '\\') {
                    result.push_back(static_cast<char>(symbol));
                    continue;
                }
                if (symbol != '\\' || m_pos == m_json.size())
                    return false;
                const char escaped = m_json[m_pos++];
                if (escaped == '"' || escaped == '\\' || escaped == '/') {
                    result.push_back(escaped);
                } else if (escaped == 'u') {
                    if (m_json.size() - m_pos < 4)
                        return false;
                    unsigned value = 0;
                    for (int index = 0; index < 4; ++index) {
                        const int digit = hexDigit(m_json[m_pos++]);
                        if (digit < 0)
                            return false;
                        value = (value << 4) | static_cast<unsigned>(digit);
                    }
                    if (value < 0x20 || value > 0x7e)
                        return false;
                    result.push_back(static_cast<char>(value));
                } else {
                    // Ключи и значения этой схемы — печатный ASCII; контрольные символы не нужны.
                    return false;
                }
            }
            return false;
        }

    private:
        static int hexDigit(char digit)
        {
            if (digit >= '0' && digit <= '9')
                return digit - '0';
            if (digit >= 'a' && digit <= 'f')
                return digit - 'a' + 10;
            if (digit >= 'A' && digit <= 'F')
                return digit - 'A' + 10;
            return -1;
        }

        void skipSpace()
        {
            while (m_pos < m_json.size()
                && (m_json[m_pos] == ' ' || m_json[m_pos] == '\r' || m_json[m_pos] == '\n'
                    || m_json[m_pos] == '\t'))
                ++m_pos;
        }

        std::string_view m_json;
        std::size_t m_pos = 0;
    };

    constexpr std::array<std::string_view, 8> kRecipeFields { "schema", "recipe", "palette", "shape",
        "surface", "typeface", "scale", "colors" };
    constexpr std::array<std::string_view, 8> kColorFields { "value", "label", "accent", "fps", "cpu", "gpu",
        "ram", "vram" };

    bool readColors(RecipeReader& reader, std::array<std::optional<std::uint32_t>, 8>& colors)
    {
        if (!reader.take('{'))
            return false;
        bool first = true;
        while (!reader.peek('}')) {
            if (!first && !reader.take(','))
                return false;
            first = false;
            std::string key;
            std::string value;
            if (!reader.readString(key) || !reader.take(':') || !reader.readString(value))
                return false;
            const auto field = std::find(kColorFields.begin(), kColorFields.end(), key);
            if (field == kColorFields.end())
                return false;
            const std::size_t index = static_cast<std::size_t>(field - kColorFields.begin());
            if (colors[index] || value.size() != 9 || value.front() != '#')
                return false;
            std::uint32_t rgba = 0;
            for (std::size_t digit = 1; digit < 9; ++digit) {
                const char symbol = value[digit];
                const int nibble = symbol >= '0' && symbol <= '9' ? symbol - '0'
                    : symbol >= 'a' && symbol <= 'f'              ? symbol - 'a' + 10
                    : symbol >= 'A' && symbol <= 'F'              ? symbol - 'A' + 10
                                                                  : -1;
                if (nibble < 0)
                    return false;
                rgba = (rgba << 4) | static_cast<std::uint32_t>(nibble);
            }
            colors[index] = rgba;
        }
        return reader.take('}');
    }

    bool parseRecipeFields(std::string_view json, OverlaySettings& result)
    {
        RecipeReader reader(json);
        if (!reader.take('{'))
            return false;
        std::array<bool, kRecipeFields.size()> seen { };
        std::array<std::optional<std::uint32_t>, kColorFields.size()> colors { };
        OverlayRecipe recipe = result.recipe;
        OverlayStyle palette = result.style;
        OverlayLayout shape = result.layout;
        OverlaySurface surface = result.surface;
        bool hasSurface = false;
        bool customSurface = false;
        int font = result.fontFamily;
        int scale = result.scalePercent;
        bool first = true;
        while (!reader.peek('}')) {
            if (!first && !reader.take(','))
                return false;
            first = false;
            std::string key;
            if (!reader.readString(key) || !reader.take(':'))
                return false;
            const auto field = std::find(kRecipeFields.begin(), kRecipeFields.end(), key);
            if (field == kRecipeFields.end())
                return false;
            const std::size_t index = static_cast<std::size_t>(field - kRecipeFields.begin());
            if (seen[index])
                return false;
            seen[index] = true;
            if (index == 7) {
                if (!readColors(reader, colors))
                    return false;
                continue;
            }
            if (index == 0 || index == 6) {
                int value = 0;
                if (!reader.readNumber(value))
                    return false;
                if (index == 0 && value != 1)
                    return false;
                if (index == 6)
                    scale = value;
                continue;
            }
            if (index == 4 && reader.readNull())
                continue;
            std::string value;
            if (!reader.readString(value))
                return false;
            switch (index) {
            case 1:
                if (!parseOverlayRecipe(value, recipe))
                    return false;
                break;
            case 2:
                if (!parseOverlayStyle(value, palette) || overlayStyleKey(palette) != value)
                    return false;
                break;
            case 3:
                if (!parseOverlayLayout(value, shape))
                    return false;
                break;
            case 4:
                customSurface = value == "custom";
                hasSurface = !customSurface;
                if (!customSurface && !parseOverlaySurface(value, surface))
                    return false;
                break;
            case 5:
                font = value == "sans"     ? 0
                    : value == "mono"      ? 1
                    : value == "condensed" ? 2
                    : value == "dense"     ? 3
                                           : -1;
                if (font < 0)
                    return false;
                break;
            default:
                return false;
            }
        }
        if (!reader.take('}') || !reader.finished() || scale < 70 || scale > 160
            || !std::all_of(seen.begin(), seen.begin() + 7, [](bool present) { return present; }))
            return false;

        const auto before = result;
        applyOverlayRecipe(result, recipe);
        result.metrics = before.metrics;
        result.metricRgba = before.metricRgba;
        result.style = palette;
        result.layout = shape;
        result.fontFamily = font;
        result.scalePercent = scale;
        if (customSurface) {
            result.surface = before.surface;
            result.surfaceActive = false;
            result.showBackground = before.showBackground;
            result.showBorder = before.showBorder;
            result.showShadow = before.showShadow;
        } else if (hasSurface) {
            setOverlaySurface(result, surface);
        } else {
            result.surfaceActive = false;
            result.showBackground = palette != OverlayStyle::Rtss;
            result.showBorder = palette != OverlayStyle::Rtss && palette != OverlayStyle::Mono;
            result.showShadow = palette != OverlayStyle::Rtss && palette != OverlayStyle::Mono;
        }
        result.valueRgba = colors[0];
        result.labelRgba = colors[1];
        result.accentRgba = colors[2];
        for (std::size_t index = 0; index < kLabelCount; ++index)
            result.rowRgba[index] = colors[index + 3];
        return true;
    }

} // namespace

bool importOverlayRecipeJson(std::string_view json, OverlaySettings& overlay, std::string& error)
{
    if (json.size() > 16384) {
        error = "Файл рецепта слишком большой (предел 16 КБ)";
        return false;
    }
    OverlaySettings candidate = overlay;
    if (!parseRecipeFields(json, candidate)) {
        error = "Неверная схема, неизвестное поле или значение рецепта JSON";
        return false;
    }
    overlay = std::move(candidate);
    error.clear();
    return true;
}

} // namespace yawa
