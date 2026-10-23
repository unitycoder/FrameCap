#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include "encoder.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <cwchar>

namespace {
constexpr UINT Done = WM_APP + 1;
constexpr int RecordId = 101, DiscardId = 102, FpsId = 103, CursorId = 104;
enum class State { Ready, Recording, Stopping, Pending };
HWND window, recordButton, discardButton, fpsEdit, cursorBox;
HFONT font; HBRUSH background;
State state = State::Ready;
std::thread worker;
std::atomic<bool> stop{false};
std::atomic<unsigned long long> frameCount{0};
std::mutex waitMutex;
std::condition_variable wake;
std::wstring workerError, notice;
std::filesystem::path temporary;
std::chrono::steady_clock::time_point started;
bool closeAfter = false, hotkey = false, modal = false;
struct ModalGuard { bool previous = modal; ModalGuard() { modal = true; } ~ModalGuard() { modal = previous; } };
int dpi = 96;
int px(int n) { return MulDiv(n, dpi, 96); }
RECT captureRect() {
    RECT r; GetClientRect(window, &r);
    r.left = px(7); r.top = px(35); r.right -= px(7); r.bottom -= px(62);
    // I420 requires even pixel dimensions. The visible hole matches these exactly.
    r.right = r.left + ((r.right - r.left) & ~1); r.bottom = r.top + ((r.bottom - r.top) & ~1);
    return r;
}
RECT screenCaptureRect() {
    RECT r = captureRect(); POINT p{r.left, r.top}; ClientToScreen(window, &p);
    OffsetRect(&r, p.x - r.left, p.y - r.top); return r;
}
void message(const std::wstring& s) { ModalGuard guard; MessageBoxW(window, s.c_str(), L"FrameCap", MB_OK | MB_ICONERROR); }
void layout() {
    static bool layingOut = false;
    if (layingOut) return;
    layingOut = true;
    RECT r; GetClientRect(window, &r); RECT c = captureRect();
    HRGN outside = CreateRectRgn(0, 0, r.right, r.bottom), hole = CreateRectRgn(c.left, c.top, c.right, c.bottom);
    CombineRgn(outside, outside, hole, RGN_DIFF); DeleteObject(hole);
    if (!SetWindowRgn(window, outside, TRUE)) DeleteObject(outside);
    int y = c.bottom + px(9);
    MoveWindow(recordButton, px(14), y, px(104), px(30), TRUE);
    MoveWindow(fpsEdit, px(165), y + px(2), px(42), px(25), TRUE);
    MoveWindow(cursorBox, px(220), y, px(90), px(28), TRUE);
    MoveWindow(discardButton, r.right - px(100), y, px(86), px(30), TRUE);
    InvalidateRect(window, nullptr, FALSE);
    layingOut = false;
}
void updateControls() {
    bool ready = state == State::Ready;
    EnableWindow(fpsEdit, ready); EnableWindow(cursorBox, ready);
    EnableWindow(recordButton, state != State::Stopping);
    SetWindowTextW(recordButton, state == State::Recording ? L"Stop" : state == State::Stopping ? L"Finishing..." : state == State::Pending ? L"Save..." : L"Record");
    ShowWindow(discardButton, state == State::Pending ? SW_SHOW : SW_HIDE);
    InvalidateRect(window, nullptr, FALSE);
}
bool saveRecording() {
    ModalGuard guard;
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t name[32768];
    swprintf_s(name, L"capture_%02u%02u%04u_%02u%02u.webm", now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute);
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
    dialog.lpstrFilter = L"WebM video (*.webm)\0*.webm\0\0";
    dialog.lpstrFile = name; dialog.nMaxFile = 32768; dialog.lpstrDefExt = L"webm";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) {
        if (CommDlgExtendedError()) message(L"The save dialog could not open. Your recording is still available; try Save again.");
        return false;
    }
    // Copy first: a failed save never destroys the only recording.
    if (!CopyFileW(temporary.c_str(), name, FALSE)) {
        message(L"Could not save the file (Windows error " + std::to_wstring(GetLastError()) + L"). Your recording is still available.");
        return false;
    }
    if (!DeleteFileW(temporary.c_str())) {
        message(L"The recording was saved, but its temporary copy could not be removed (Windows error " + std::to_wstring(GetLastError()) + L"). Use Discard to try again.");
        return false;
    }
    temporary.clear(); state = State::Ready;
    notice = L"Saved"; updateControls();
    std::wstring arguments = L"/select,\"" + std::wstring(name) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    return true;
}
struct Capture {
    HDC screen = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr; HGDIOBJ old = nullptr; uint8_t* pixels = nullptr;
    int width, height;
    Capture(int w, int h) : width(w), height(h) {
        screen = GetDC(nullptr); if (screen) memory = CreateCompatibleDC(screen);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        if (memory) bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (bitmap) old = SelectObject(memory, bitmap);
        if (!screen || !memory || !bitmap || !old || old == HGDI_ERROR) { cleanup(); throw std::runtime_error("Cannot create screen capture buffers"); }
    }
    void cleanup() {
        if (old && old != HGDI_ERROR) SelectObject(memory, old);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        screen = memory = nullptr; bitmap = nullptr; old = nullptr;
    }
    ~Capture() { cleanup(); }
    void grab(RECT r, bool cursor) {
        if (!BitBlt(memory, 0, 0, width, height, screen, r.left, r.top, SRCCOPY | CAPTUREBLT))
            throw std::runtime_error("Screen capture failed (possibly a locked desktop)");
        if (cursor) {
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO icon{};
                if (GetIconInfo(ci.hCursor, &icon)) {
                    DrawIconEx(memory, ci.ptScreenPos.x - r.left - int(icon.xHotspot),
                               ci.ptScreenPos.y - r.top - int(icon.yHotspot), ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                    if (icon.hbmMask) DeleteObject(icon.hbmMask);
                    if (icon.hbmColor) DeleteObject(icon.hbmColor);
                }
            }
        }
        GdiFlush();
    }
};
void record(RECT r, int fps, bool cursor, std::filesystem::path path) {
    try {
        int width = r.right - r.left, height = r.bottom - r.top;
        Capture capture(width, height); encoding::Encoder encoder(width, height, fps);
        webm::Writer out(path, width, height, fps);
        using Clock = std::chrono::steady_clock;
        auto origin = Clock::now(); uint64_t slot = 0, lastPts = 0;
        while (true) {
            auto deadline = origin + std::chrono::nanoseconds(slot * 1000000000ULL / fps);
            { std::unique_lock<std::mutex> lock(waitMutex); wake.wait_until(lock, deadline, [] { return stop.load(); }); }
            if (stop.load() && frameCount.load() > 0) break;
            auto now = Clock::now();
            uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - origin).count());
            int64_t pts = frameCount.load() == 0 ? 0 : int64_t(std::max(ms, lastPts + 1));
            capture.grab(r, cursor); encoding::convert(capture.pixels, width, height, encoder.image);
            encoder.encode(out, &encoder.image, pts, std::max(1, 1000 / fps));
            lastPts = uint64_t(pts); ++frameCount;
            // Skip overdue capture slots without speeding up playback or queuing frames.
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin).count();
            slot = std::max(slot + 1, uint64_t(elapsed) * fps / 1000000000ULL + 1);
        }
        auto duration = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
        encoder.encode(out, nullptr, 0, 0); out.finish(std::max(duration, lastPts + uint64_t(std::max(1, 1000/fps))));
    } catch (const std::exception& e) {
        std::string s = e.what(); workerError.assign(s.begin(), s.end());
    } catch (...) { workerError = L"Unexpected recording failure"; }
    PostMessageW(window, Done, 0, 0);
}
void begin() {
    BOOL valid; UINT fps = GetDlgItemInt(window, FpsId, &valid, FALSE);
    if (!valid || fps < 1 || fps > 60) { message(L"Enter a frame rate from 1 to 60."); return; }
    RECT r = screenCaptureRect();
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (r.left < desktop.left || r.top < desktop.top || r.right > desktop.right || r.bottom > desktop.bottom) {
        message(L"Move the entire capture area onto your desktop before recording."); return;
    }
    if (r.right-r.left > 8192 || r.bottom-r.top > 8192) { message(L"The maximum supported capture size is 8192 x 8192."); return; }
    wchar_t dir[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"FCP", 0, name)) { message(L"Could not create a temporary recording file."); return; }
    temporary = name; stop = false; frameCount = 0; workerError.clear(); notice.clear();
    state = State::Recording; started = std::chrono::steady_clock::now(); updateControls();
    try { worker = std::thread(record, r, int(fps), SendMessageW(cursorBox, BM_GETCHECK, 0, 0) == BST_CHECKED, temporary); }
    catch (...) {
        std::wstring failure = L"Could not start the recording thread.";
        if (!DeleteFileW(temporary.c_str()))
            failure += L" The temporary file could not be removed (Windows error " + std::to_wstring(GetLastError()) + L").";
        temporary.clear(); state = State::Ready; updateControls(); message(failure);
    }
}
void toggle() {
    if (state == State::Ready) begin();
    else if (state == State::Recording) { state = State::Stopping; stop = true; wake.notify_all(); updateControls(); }
    else if (state == State::Pending) saveRecording();
}
bool confirmDiscard() {
    ModalGuard guard;
    return MessageBoxW(window, L"Discard the unsaved recording?", L"FrameCap", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}
void discard() {
    if (!confirmDiscard()) return;
    if (!DeleteFileW(temporary.c_str())) {
        message(L"Could not remove the temporary recording (Windows error " + std::to_wstring(GetLastError()) + L").");
        return;
    }
    temporary.clear(); state = State::Ready; notice.clear(); updateControls();
}
void closeWindow() {
    if (state == State::Stopping) return;
    if (state == State::Recording) { closeAfter = true; toggle(); return; }
    if (state == State::Pending) {
        ModalGuard guard;
        int answer = MessageBoxW(window, L"Save your recording before closing?", L"FrameCap", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL || (answer == IDYES && !saveRecording())) return;
        if (answer == IDNO) {
            if (!DeleteFileW(temporary.c_str())) {
                message(L"Could not remove the temporary recording (Windows error " + std::to_wstring(GetLastError()) + L").");
                return;
            }
            temporary.clear();
        }
    }
    DestroyWindow(window);
}
HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE); return h;
}
LRESULT CALLBACK procedure(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        window = h; dpi = int(GetDpiForWindow(h));
        font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        recordButton = control(L"BUTTON", L"Record", BS_PUSHBUTTON | WS_TABSTOP, RecordId);
        discardButton = control(L"BUTTON", L"Discard", BS_PUSHBUTTON | WS_TABSTOP, DiscardId);
        fpsEdit = control(L"EDIT", L"20", ES_NUMBER | ES_CENTER | WS_BORDER | WS_TABSTOP, FpsId);
        SendMessageW(fpsEdit, EM_SETLIMITTEXT, 2, 0);
        cursorBox = control(L"BUTTON", L"Cursor", BS_AUTOCHECKBOX | WS_TABSTOP, CursorId);
        SendMessageW(cursorBox, BM_SETCHECK, BST_CHECKED, 0);
        hotkey = RegisterHotKey(h, 1, MOD_NOREPEAT, VK_F8) != FALSE;
        SetTimer(h, 1, 250, nullptr); updateControls(); return 0;
    }
    case WM_NCCALCSIZE: return 0;
    case WM_SIZE: if (recordButton) layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize = {px(600), px(260)}; return 0;
    }
    case WM_NCHITTEST: {
        POINT p{int(short(LOWORD(l))), int(short(HIWORD(l)))}; ScreenToClient(h, &p);
        RECT r; GetClientRect(h, &r);
        bool left = p.x < px(7), right = p.x >= r.right - px(7), top = p.y < px(7), bottom = p.y >= r.bottom - px(7);
        if (state == State::Ready) {
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (p.y < px(35) && p.x < r.right-px(70)) return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_LBUTTONUP: {
        RECT r; GetClientRect(h, &r);
        int x = int(short(LOWORD(l))), y = int(short(HIWORD(l)));
        if (y < px(35) && x >= r.right-px(70) && x < r.right-px(35) && (state == State::Ready || state == State::Pending))
            SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        else if (x >= r.right-px(35) && y < px(35)) closeWindow();
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_MAXIMIZE) return 0;
        if ((w & 0xfff0) == SC_MINIMIZE && state != State::Ready && state != State::Pending) return 0;
        if ((state != State::Ready) && (((w & 0xfff0) == SC_MOVE) || ((w & 0xfff0) == SC_SIZE))) return 0;
        break;
    case WM_DPICHANGED: {
        // Keep the frozen rectangle while recording; use the new DPI for the next idle layout.
        if (state == State::Ready) {
            dpi = HIWORD(w); auto* r = reinterpret_cast<RECT*>(l);
            DeleteObject(font); font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            for (HWND child : {recordButton, discardButton, fpsEdit, cursorBox}) SendMessageW(child, WM_SETFONT, WPARAM(font), TRUE);
            SetWindowPos(h, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (state == State::Recording) { notice = L"Display changed; recording stopped"; toggle(); } return 0;
    case WM_HOTKEY: if (w == 1 && !modal) toggle(); return 0;
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) == RecordId) toggle();
        else if (HIWORD(w) == BN_CLICKED && LOWORD(w) == DiscardId) discard();
        return 0;
    case Done:
        if (worker.joinable()) worker.join();
        if (!workerError.empty()) {
            std::wstring failure = L"Recording failed: " + workerError;
            if (!DeleteFileW(temporary.c_str()))
                failure += L" The temporary file could not be removed (Windows error " + std::to_wstring(GetLastError()) + L").";
            state = State::Ready; temporary.clear();
            message(failure); closeAfter = false;
        } else {
            state = State::Pending; notice = L"Unsaved recording"; updateControls();
            bool saved = saveRecording();
            if (closeAfter && saved) { DestroyWindow(h); return 0; }
            closeAfter = false;
        }
        updateControls(); return 0;
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: {
        HDC dc = HDC(w); SetTextColor(dc, RGB(226, 232, 240)); SetBkColor(dc, RGB(9, 9, 9)); return LRESULT(background);
    }
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc = BeginPaint(h, &paint); RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, background); SelectObject(dc, font); SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(232, 238, 248));
         RECT title{px(14), px(7), r.right-px(70), px(31)};
        DrawTextW(dc, L"FrameCap", -1, &title, DT_SINGLELINE | DT_VCENTER);
         SetTextColor(dc, state == State::Recording || state == State::Stopping ? RGB(100, 105, 115) : RGB(232, 238, 248));
         RECT minimize{r.right-px(70), px(7), r.right-px(35), px(31)}; DrawTextW(dc, L"_", -1, &minimize, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
         RECT close{r.right-px(35), px(7), r.right-px(9), px(31)}; DrawTextW(dc, L"X", -1, &close, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        RECT c = captureRect(); HBRUSH border = CreateSolidBrush(state == State::Recording ? RGB(239, 83, 96) : RGB(67, 169, 229));
        RECT edge = c; InflateRect(&edge, 2, 2); FrameRect(dc, &edge, border); DeleteObject(border);
        RECT label{px(130), c.bottom+px(15), px(164), c.bottom+px(38)}; DrawTextW(dc, L"FPS", -1, &label, DT_SINGLELINE | DT_VCENTER);
        std::wstring status;
        if (state == State::Recording || state == State::Stopping) {
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started).count();
            status = std::to_wstring(seconds) + L"s  /  " + std::to_wstring(frameCount.load()) + L" frames";
        } else if (state == State::Pending) status = L"Unsaved";
        else status = std::to_wstring(c.right-c.left) + L" x " + std::to_wstring(c.bottom-c.top) + (hotkey ? L"  /  F8" : L"  /  F8 unavailable");
        RECT statusRect{px(320), c.bottom+px(12), r.right-px(state == State::Pending ? 110 : 14), c.bottom+px(36)};
        SetTextColor(dc, RGB(167, 184, 205)); DrawTextW(dc, status.c_str(), -1, &statusRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        EndPaint(h, &paint); return 0;
    }
    case WM_CLOSE: closeWindow(); return 0;
    case WM_DESTROY:
        stop = true; wake.notify_all(); if (worker.joinable()) worker.join();
        if (hotkey) UnregisterHotKey(h, 1);
        KillTimer(h, 1);
        DeleteObject(font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    background = CreateSolidBrush(RGB(9, 9, 9));
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = L"FrameCap.Main"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = cls.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(101)); cls.hbrBackground = background;
    if (!RegisterClassExW(&cls)) return 1;
    HWND h = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"FrameCap", WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(820, int(GetDpiForSystem()), 96), MulDiv(560, int(GetDpiForSystem()), 96), nullptr, nullptr, instance, nullptr);
    if (!h) return 1;
    ShowWindow(h, show); UpdateWindow(h);
    MSG msg; int result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DeleteObject(background); return result < 0 ? 1 : int(msg.wParam);
}
