#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

struct ICoreWebView2;
struct ICoreWebView2Controller;

namespace veloplay {

/// A small browser window for signing in at a web page that answers by
/// sending the browser to a known address. The window watches for that
/// address, hands it over and closes. It is the WebView2 control that ships
/// with Windows 11 and with Edge on Windows 10.
class LoginWindow {
public:
    /// The address the page ended on, or nothing if the user closed the window.
    using Done = std::function<void(std::optional<std::string> redirect)>;
    /// Called instead if this PC has no WebView2. The window is gone by then.
    using Unavailable = std::function<void()>;

    LoginWindow() = default;
    ~LoginWindow();
    LoginWindow(const LoginWindow&) = delete;
    LoginWindow& operator=(const LoginWindow&) = delete;

    /// Opens the window and starts loading `url`. Returns false if not even
    /// the window could be created. `done` and `unavailable` are called on
    /// the thread that runs the message loop, at most one of them, once.
    bool open(HWND owner, const std::string& url, std::string_view redirectPrefix,
              const std::filesystem::path& dataDirectory, const std::wstring& title, Done done,
              Unavailable unavailable);

    /// Closes the window without reporting anything.
    void close();

    [[nodiscard]] bool isOpen() const { return window_ != nullptr; }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void startBrowser(const std::filesystem::path& dataDirectory);
    void resizeBrowser();
    void finish(std::optional<std::string> redirect);
    void giveUp();

    HWND window_ = nullptr;
    // Owned references, released in close(). Plain pointers keep WebView2.h
    // out of this header.
    ICoreWebView2Controller* controller_ = nullptr;
    ICoreWebView2* view_ = nullptr;
    std::wstring url_;
    std::wstring redirectPrefix_;
    std::wstring captured_;
    Done done_;
    Unavailable unavailable_;
    // Raised whenever the window closes, so that answers still under way from
    // an earlier attempt are recognised and ignored.
    unsigned generation_ = 0;
};

} // namespace veloplay
