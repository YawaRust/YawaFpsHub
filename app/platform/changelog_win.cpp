#include "platform/changelog_win.h"
#include "resources/changelog_ids.h"

#include <algorithm>
#include <string>

namespace yawa {
namespace {

    constexpr wchar_t kChangelogClass[] = L"YawaFpsHubChangelog";
    constexpr int kTextControl = 1;

    LRESULT CALLBACK changelogWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_CREATE: {
            const auto* created = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            const auto* text = static_cast<const std::wstring*>(created->lpCreateParams);
            const HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text->c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 12, 1,
                1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTextControl)), created->hInstance,
                nullptr);
            if (edit == nullptr)
                return -1;
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessageW(edit, EM_SETSEL, 0, 0);
            return 0;
        }
        case WM_SIZE: {
            const HWND edit = GetDlgItem(window, kTextControl);
            if (edit != nullptr) {
                MoveWindow(edit, 12, 12, std::max(0, static_cast<int>(LOWORD(lParam)) - 24),
                    std::max(0, static_cast<int>(HIWORD(lParam)) - 24), TRUE);
            }
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    std::wstring embeddedChangelog()
    {
        const HMODULE instance = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_CHANGELOG), RT_RCDATA);
        const DWORD bytes = resource != nullptr ? SizeofResource(instance, resource) : 0;
        if (bytes == 0 || bytes > 128u * 1024u)
            return { };
        const void* source = LockResource(LoadResource(instance, resource));
        if (source == nullptr)
            return { };
        const auto* utf8 = static_cast<const char*>(source);
        const int count
            = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, static_cast<int>(bytes), nullptr, 0);
        if (count <= 0)
            return { };
        std::wstring decoded(static_cast<std::size_t>(count), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8, static_cast<int>(bytes), decoded.data(), count)
            != count)
            return { };
        std::wstring lines;
        lines.reserve(decoded.size() + decoded.size() / 20);
        for (const wchar_t symbol : decoded) {
            if (symbol == L'\n' && (lines.empty() || lines.back() != L'\r'))
                lines.push_back(L'\r');
            lines.push_back(symbol);
        }
        return lines;
    }

} // namespace

bool openEmbeddedChangelog(HWND owner)
{
    std::wstring text = embeddedChangelog();
    if (text.empty())
        return false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass { };
    windowClass.lpfnWndProc = changelogWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kChangelogClass;
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    const HWND window = CreateWindowExW(0, kChangelogClass, L"Что нового · YawaFpsHub", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 620, owner, nullptr, instance, &text);
    if (window == nullptr)
        return false;
    ShowWindow(window, SW_SHOW);
    SetFocus(GetDlgItem(window, kTextControl));
    UpdateWindow(window);
    return true;
}

} // namespace yawa
