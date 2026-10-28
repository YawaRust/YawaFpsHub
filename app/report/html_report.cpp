#include "report/html_report.h"
#include "report/package_internal.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace yawa {
namespace {

    constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    void appendBase64(std::string& target, std::string_view bytes)
    {
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const unsigned a = static_cast<unsigned char>(bytes[i]);
            const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
            const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
            target += kBase64[a >> 2];
            target += kBase64[((a & 3u) << 4) | (b >> 4)];
            target += i + 1 < bytes.size() ? kBase64[((b & 15u) << 2) | (c >> 6)] : '=';
            target += i + 2 < bytes.size() ? kBase64[c & 63u] : '=';
        }
    }

    void appendEscaped(std::string& target, std::string_view text)
    {
        for (char symbol : text) {
            switch (symbol) {
            case '&':
                target += "&amp;";
                break;
            case '<':
                target += "&lt;";
                break;
            case '>':
                target += "&gt;";
                break;
            case '"':
                target += "&quot;";
                break;
            case '\'':
                target += "&#39;";
                break;
            default:
                target += symbol;
            }
        }
    }

} // namespace

bool buildHtmlReport(const ReportPackageContents& contents, std::string& html, std::string& error)
{
    error.clear();
    if (contents.cardPng.size() > 32u * 1024u * 1024u || contents.sessionJson.size() > 64u * 1024u
        || !report_internal::validPng(contents.cardPng)) {
        error = "HTML-отчёт: карточка или сессия недоступна";
        return false;
    }
    std::string output;
    output.reserve((contents.cardPng.size() + 2) / 3 * 4 + contents.sessionJson.size() * 5 + 2048);
    output
        = "<!doctype html>\n<html lang=\"ru\"><head><meta charset=\"utf-8\">"
          "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
          "img-src data:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>YawaFpsHub · локальный отчёт</title>"
          "<style>body{margin:0;background:#101820;color:#e8f2f3;font:16px/1.5 system-ui,sans-serif}"
          "main{max-width:1200px;margin:auto;padding:24px}img{display:block;width:100%;height:auto}"
          "figure{margin:24px "
          "0}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#192830;padding:16px}"
          "small{color:#abc0c7}</style></head><body><main>"
          "<h1>Диагностический отчёт YawaFpsHub</h1>"
          "<p>Файл создан из локального пакета. Проверка структуры не подтверждает "
          "достоверность показаний или содержимого кадра. Перед пересылкой проверьте "
          "картинку и личные данные.</p><figure><img alt=\"Карточка из пакета\" src=\"data:image/png;base64,";
    appendBase64(output, contents.cardPng);
    output += "\"></figure><h2>Данные пакета</h2><p>Кадр игры заявлен как ";
    output += contents.gameFrameIncluded ? "включённый" : "отсутствующий";
    output += ". Это поле не подтверждает происхождение изображения.</p><h3>Сессия</h3><pre>";
    appendEscaped(output, contents.sessionJson);
    output += "</pre><small>Снимок офлайн · без сетевых ресурсов и скриптов</small></main></body></html>\n";
    html = std::move(output);
    return true;
}

} // namespace yawa
