#include "app/LoginWindow.hpp"

#include <wrl.h>

#include <WebView2.h>

#include <spdlog/spdlog.h>

namespace veloplay {

using Microsoft::WRL::Callback;

namespace {

constexpr wchar_t kClassName[] = L"VeloPlayLoginWindow";
constexpr UINT kCapturedMessage = WM_APP + 1;
constexpr UINT kUnavailableMessage = WM_APP + 2;

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring toWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

} // namespace

LoginWindow::~LoginWindow() { close(); }

bool LoginWindow::open(HWND owner, const std::string& url, std::string_view redirectPrefix,
                       const std::filesystem::path& dataDirectory, const std::wstring& title, Done done,
                       Unavailable unavailable) {
    close();

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &LoginWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass); // fails harmlessly when it is registered already

    // Centred over the main window, sized for Sony's sign-in form.
    const UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    const int width = MulDiv(520, static_cast<int>(dpi), 96);
    const int height = MulDiv(760, static_cast<int>(dpi), 96);
    RECT ownerRect{0, 0, width, height};
    if (owner) GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    window_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, width, height, owner, nullptr,
                              instance, this);
    if (!window_) return false;

    url_ = toWide(url);
    redirectPrefix_ = toWide(redirectPrefix);
    captured_.clear();
    done_ = std::move(done);
    unavailable_ = std::move(unavailable);
    ShowWindow(window_, SW_SHOW);
    startBrowser(dataDirectory);
    return true;
}

void LoginWindow::startBrowser(const std::filesystem::path& dataDirectory) {
    const unsigned generation = generation_;
    const HWND window = window_;

    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDirectory.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, generation, window](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (generation != generation_) return S_OK; // the window was closed meanwhile
                if (FAILED(result) || !environment) {
                    spdlog::warn("WebView2 is not available (0x{:08x})", static_cast<unsigned>(result));
                    PostMessageW(window, kUnavailableMessage, 0, 0);
                    return S_OK;
                }
                environment->CreateCoreWebView2Controller(
                    window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [this, generation, window](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                                    if (generation != generation_) return S_OK;
                                    if (FAILED(created) || !controller) {
                                        spdlog::warn("The sign-in browser could not be created (0x{:08x})",
                                                     static_cast<unsigned>(created));
                                        PostMessageW(window, kUnavailableMessage, 0, 0);
                                        return S_OK;
                                    }
                                    controller->AddRef();
                                    controller_ = controller;
                                    controller_->get_CoreWebView2(&view_);
                                    resizeBrowser();
                                    if (!view_) {
                                        PostMessageW(window, kUnavailableMessage, 0, 0);
                                        return S_OK;
                                    }

                                    // Every navigation of the page passes here, redirects included.
                                    EventRegistrationToken token{};
                                    view_->add_NavigationStarting(
                                        Callback<ICoreWebView2NavigationStartingEventHandler>(
                                            [this, generation, window](ICoreWebView2*,
                                                                       ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                                if (generation != generation_) return S_OK;
                                                LPWSTR uri = nullptr;
                                                if (FAILED(args->get_Uri(&uri)) || !uri) return S_OK;
                                                const std::wstring target(uri);
                                                CoTaskMemFree(uri);
                                                if (target.compare(0, redirectPrefix_.size(), redirectPrefix_) == 0) {
                                                    // That page has nothing to show. Keep the address and
                                                    // close once this handler has returned.
                                                    args->put_Cancel(TRUE);
                                                    captured_ = target;
                                                    PostMessageW(window, kCapturedMessage, 0, 0);
                                                }
                                                return S_OK;
                                            })
                                            .Get(),
                                        &token);
                                    view_->Navigate(url_.c_str());
                                    return S_OK;
                                })
                                .Get());
                return S_OK;
            })
            .Get());

    if (FAILED(started)) {
        // The runtime is missing: there is no callback to wait for.
        spdlog::warn("WebView2 is not installed (0x{:08x})", static_cast<unsigned>(started));
        PostMessageW(window, kUnavailableMessage, 0, 0);
    }
}

void LoginWindow::resizeBrowser() {
    if (!controller_ || !window_) return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);
}

void LoginWindow::close() {
    ++generation_;
    if (view_) {
        view_->Release();
        view_ = nullptr;
    }
    if (controller_) {
        controller_->Close();
        controller_->Release();
        controller_ = nullptr;
    }
    if (window_) {
        const HWND window = window_;
        window_ = nullptr; // before DestroyWindow, whose messages come back in here
        DestroyWindow(window);
    }
    done_ = nullptr;
    unavailable_ = nullptr;
}

void LoginWindow::finish(std::optional<std::string> redirect) {
    Done done = std::move(done_);
    close();
    if (done) done(std::move(redirect));
}

void LoginWindow::giveUp() {
    Unavailable unavailable = std::move(unavailable_);
    close();
    if (unavailable) unavailable();
}

LRESULT CALLBACK LoginWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<LoginWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    // Messages for a window that was already given up are not ours any more.
    if (!self || self->window_ != window) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
        case WM_SIZE:
            self->resizeBrowser();
            return 0;
        case WM_CLOSE:
            self->finish(std::nullopt);
            return 0;
        case kCapturedMessage:
            self->finish(toUtf8(self->captured_));
            return 0;
        case kUnavailableMessage:
            self->giveUp();
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace veloplay
