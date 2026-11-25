// WINDOWS IME SEAM. See window_ime.cppm for why this exists at all.
//
// THE MESSAGES THIS SEAM READS
// - WM_IME_SETCONTEXT: arrives before a composition and asks whether the system should draw its own
//   composition window. Observation mode leaves it alone (`system_composition_window` records it).
// - WM_IME_STARTCOMPOSITION / WM_IME_ENDCOMPOSITION: the brackets around a preedit.
// - WM_IME_COMPOSITION: GCS_COMPSTR carries the preedit and GCS_CURSORPOS its caret; GCS_RESULTSTR
//   carries the committed string, which GLFW's character callback also reports one code unit at a
//   time, so the commit event here is for the UI layer and the callback still types into the game.
// - WM_IME_NOTIFY: IMN_OPENCANDIDATE / IMN_CLOSECANDIDATE count the candidate page.
//
// THE CANDIDATE WINDOW IS MOVED, NOT REPLACED: ImmSetCandidateWindow with CFS_CANDIDATEPOS tells the
// system where the caret is, which is the only part of the composition UI the prototype does not
// want to own. The composition window itself stays system-drawn until M1 draws preedit in the
// element tree.
module;
#include <windows.h>
#include <imm.h>

module deren.app.window_ime;
import deren.vstd;
import deren.utility;

namespace deren::app {
    namespace {
        // The previous procedure lives on the HWND, not in a global, so two windows can never
        // overwrite each other's chain and a stale handle cannot resurrect a dead procedure.
        WNDPROC previous_proc(HWND window) noexcept {
            return reinterpret_cast<WNDPROC>(GetPropW(window, L"deren.ime.previous_proc"));
        }

        std::string utf8_from_utf16(wchar_t const* text, int length) {
            if (text == nullptr || length <= 0) {
                return {};
            }
            int const needed = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
            if (needed <= 0) {
                return {};
            }
            std::string result(static_cast<std::size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), needed, nullptr, nullptr);
            return result;
        }

        std::int32_t scalar_count(std::wstring_view text) noexcept {
            std::int32_t count = 0;
            for (wchar_t const unit : text) {
                // A surrogate pair is one scalar value; the low half does not count twice.
                if (unit >= 0xDC00 && unit <= 0xDFFF) {
                    continue;
                }
                ++count;
            }
            return count;
        }

        /// The probe's input is UTF-8 because that is what the managed side sends; IMM wants UTF-16.
        std::wstring utf16_from_utf8(std::string_view text) {
            if (text.empty()) {
                return {};
            }
            int const needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (needed <= 0) {
                return {};
            }
            std::wstring result(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), needed);
            return result;
        }

        /// Converts a caret offset Windows reports in UTF-16 units into a scalar count, so the UI's
        /// caret index matches its own decoding rather than the platform's.
        std::int32_t scalar_offset(std::wstring_view text, int utf16_offset) noexcept {
            int const limit = std::min<int>(utf16_offset, static_cast<int>(text.size()));
            return scalar_count(text.substr(0, static_cast<std::size_t>(std::max(0, limit))));
        }

        std::wstring composition_string(HIMC context, DWORD kind) {
            if (context == nullptr) {
                return {};
            }
            LONG const bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
            if (bytes <= 0) {
                return {};
            }
            std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            LONG const written = ImmGetCompositionStringW(context, kind, text.data(), bytes);
            if (written <= 0) {
                return {};
            }
            text.resize(static_cast<std::size_t>(written) / sizeof(wchar_t));
            return text;
        }

        window_ime* owner = nullptr;
    } // namespace

    window_ime::~window_ime() {
        uninstall();
    }

    bool window_ime::install(void* native_window) noexcept {
        if (native_window == nullptr || this->window_ != nullptr) {
            return false;
        }
        HWND const window = static_cast<HWND>(native_window);
        if (previous_proc(window) != nullptr) {
            // Another seam already owns this window's chain: chaining two of our own procedures
            // would recurse, and the existing one already reports what this one would.
            return false;
        }
        auto const previous = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_ime::proc)));
        if (previous == nullptr) {
            return false;
        }
        SetPropW(window, L"deren.ime.previous_proc", reinterpret_cast<HANDLE>(previous));
        this->window_ = native_window;
        this->previous_proc_ = reinterpret_cast<void*>(previous);
        owner = this;
        return true;
    }

    void window_ime::uninstall() noexcept {
        if (this->window_ == nullptr) {
            return;
        }
        HWND const window = static_cast<HWND>(this->window_);
        WNDPROC const current = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(this->previous_proc_)));
        // Only remove the marker when the procedure really went back: a failed restore leaves the
        // chain intact and the property is what proves it, so dropping it would strand the window.
        if (reinterpret_cast<std::intptr_t>(current) == reinterpret_cast<std::intptr_t>(&window_ime::proc)) {
            RemovePropW(window, L"deren.ime.previous_proc");
        }
        if (owner == this) {
            owner = nullptr;
        }
        this->window_ = nullptr;
        this->previous_proc_ = nullptr;
    }

    bool window_ime::installed() const noexcept {
        return this->window_ != nullptr;
    }

    void window_ime::set_sink(sink callback, void* opaque) noexcept {
        this->sink_ = callback;
        this->sink_opaque_ = opaque;
    }

    void window_ime::set_caret_rect(float x, float y, float width, float height) noexcept {
        this->caret_x_ = x;
        this->caret_y_ = y;
        this->caret_width_ = width > 0.0F ? width : 1.0F;
        this->caret_height_ = height > 0.0F ? height : 16.0F;
    }

    ime_report window_ime::report() const noexcept {
        return this->report_;
    }

    bool window_ime::probe_composition(std::string_view text, bool commit) noexcept {
        if (this->window_ == nullptr) {
            return false;
        }
        HWND const window = static_cast<HWND>(this->window_);
        HIMC context = ImmGetContext(window);
        bool const created = context == nullptr;
        if (created) {
            // A window without an input method has no context, and ImmSetCompositionString has
            // nothing to write into without one. Creating and associating one is local to this
            // probe: the association is removed again below, so a real session is unaffected.
            context = ImmCreateContext();
            if (context == nullptr || ImmAssociateContext(window, context) == nullptr) {
                if (context != nullptr) {
                    ImmDestroyContext(context);
                }
                return false;
            }
        }
        std::wstring const wide = utf16_from_utf8(text);
        // START first: the composition has to be open before the string can be set, which is the
        // order a real IME uses (it opens the composition, then fills it in).
        SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
        if (!wide.empty()) {
            // imm.h (MinGW) declares these parameters LPVOID even though they are read-only.
            BOOL const set = ImmSetCompositionStringW(context, SCS_SETSTR, const_cast<wchar_t*>(wide.data()),
                                                      static_cast<DWORD>(wide.size() * sizeof(wchar_t)), nullptr, 0);
            deren::utility::log("ime: probe set composition string -> {} (error {})", set != 0, GetLastError());
            LONG const readback = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
            deren::utility::log("ime: probe reads back {} bytes of composition string", readback);
        }
        SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
        if (commit) {
            // GCS_RESULTSTR is what a real IME sends when the user picks a candidate; the seam turns
            // it into a character event through the same path GLFW's character callback uses.
            SendMessageW(window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        }
        SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
        if (created) {
            ImmAssociateContext(window, nullptr);
            ImmDestroyContext(context);
        } else {
            ImmReleaseContext(window, context);
        }
        return true;
    }

    bool window_ime::probe_text_mapping(std::string& utf8, std::int32_t& caret) noexcept {
        // The probe is U+3042 (one UTF-16 unit), U+1F600 (a surrogate pair, two units) and U+597D
        // (one unit), so a caret after all three sits at UTF-16 offset 4 and at scalar index 3.
        wchar_t const text[] = {L'\u3042', L'\xD83D', L'\xDE00', L'\u597D'};
        std::wstring_view const units{text, std::size(text)};
        utf8 = utf8_from_utf16(text, static_cast<int>(units.size()));
        caret = scalar_offset(units, static_cast<int>(units.size()));
        return !utf8.empty();
    }

    void window_ime::emit(ime_event const& event) noexcept {
        if (this->sink_ != nullptr) {
            this->sink_(this->sink_opaque_, event);
        }
    }

    std::int32_t window_ime::candidate_count(void* native_window) noexcept {
        HWND const window = static_cast<HWND>(native_window);
        HIMC const context = ImmGetContext(window);
        if (context == nullptr) {
            return 0;
        }
        std::int32_t count = 0;
        DWORD const needed = ImmGetCandidateListW(context, 0, nullptr, 0);
        if (needed != 0) {
            std::vector<unsigned char> buffer(needed);
            auto* const list = reinterpret_cast<CANDIDATELIST*>(buffer.data());
            if (ImmGetCandidateListW(context, 0, list, needed) != 0) {
                count = static_cast<std::int32_t>(list->dwCount);
            }
        }
        ImmReleaseContext(window, context);
        return count;
    }

    void window_ime::place_candidate_window(void* native_window, void* native_context) noexcept {
        HWND const window = static_cast<HWND>(native_window);
        HIMC const context = static_cast<HIMC>(native_context);
        if (window == nullptr || context == nullptr) {
            return;
        }
        // The caller reports the caret in CLIENT pixels; the candidate window wants SCREEN pixels.
        POINT point{static_cast<LONG>(this->caret_x_), static_cast<LONG>(this->caret_y_)};
        if (ClientToScreen(window, &point) == 0) {
            return;
        }
        CANDIDATEFORM form{};
        form.dwIndex = 0;
        form.dwStyle = CFS_CANDIDATEPOS;
        form.ptCurrentPos = point;
        // The excluded rectangle is the caret's own box, so the list never covers what is being typed.
        form.rcArea = RECT{point.x, point.y, point.x + static_cast<LONG>(this->caret_width_),
                           point.y + static_cast<LONG>(this->caret_height_)};
        if (ImmSetCandidateWindow(context, &form) != 0) {
            this->report_.candidate_followed_caret = true;
        }
    }

    std::intptr_t window_ime::proc(void* native_window, unsigned message, std::uintptr_t wparam,
                                   std::intptr_t lparam) noexcept {
        HWND const window = static_cast<HWND>(native_window);
        WNDPROC const previous = previous_proc(window);
        if (previous == nullptr) {
            // DefWindowProcW is NOT the fallback: the seam was installed by replacing a real
            // procedure, and answering as a bare window would swallow what GLFW tracks.
            return 0;
        }
        if (owner == nullptr) {
            return static_cast<std::intptr_t>(
                CallWindowProcW(previous, window, message, static_cast<WPARAM>(wparam),
                                static_cast<LPARAM>(lparam)));
        }
        window_ime& seam = *owner;
        switch (message) {
        case WM_IME_SETCONTEXT:
            // Observations only: the system composition window stays on screen in this prototype,
            // so the flags are not stripped (M1 strips ISC_SHOWUICOMPOSITIONWINDOW and draws the
            // preedit itself).
            seam.report_.system_composition_window = true;
            break;
        case WM_IME_STARTCOMPOSITION:
            ++seam.report_.compositions_started;
            seam.composing_ = true;
            seam.emit(ime_event{.type = ime_event_type::start, .text = {}});
            break;
        case WM_IME_COMPOSITION: {
            HIMC const context = ImmGetContext(window);
            if ((static_cast<std::size_t>(lparam) & GCS_RESULTSTR) != 0) {
                std::wstring const committed = composition_string(context, GCS_RESULTSTR);
                if (!committed.empty()) {
                    ++seam.report_.commits;
                    // The commit ends the composition: the WM_IME_ENDCOMPOSITION that follows is the
                    // normal end of a finished composition, not a cancellation.
                    seam.composing_ = false;
                    ime_event event{};
                    event.type = ime_event_type::commit;
                    event.text = utf8_from_utf16(committed.data(), static_cast<int>(committed.size()));
                    seam.emit(event);
                }
            }
            if ((static_cast<std::size_t>(lparam) & GCS_COMPSTR) != 0) {
                std::wstring const preedit = composition_string(context, GCS_COMPSTR);
                LONG const caret = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
                ++seam.report_.updates;
                ime_event update{};
                update.type = ime_event_type::update;
                update.text = utf8_from_utf16(preedit.data(), static_cast<int>(preedit.size()));
                update.caret = scalar_offset(preedit, caret < 0 ? 0 : static_cast<int>(caret));
                seam.emit(update);
                seam.place_candidate_window(native_window, context);
            }
            ImmReleaseContext(window, context);
            break;
        }
        case WM_IME_ENDCOMPOSITION:
            // An end that no commit consumed is a cancellation (Escape, focus loss, or a plain blur).
            if (seam.composing_) {
                ++seam.report_.cancelled;
            }
            seam.composing_ = false;
            seam.emit(ime_event{.type = ime_event_type::end, .text = {}});
            break;
        case WM_IME_NOTIFY:
            if (wparam == IMN_OPENCANDIDATE) {
                ++seam.report_.candidate_lists;
                std::int32_t const count = seam.candidate_count(native_window);
                if (count > seam.report_.largest_candidate_list) {
                    seam.report_.largest_candidate_list = count;
                }
                ime_event candidates{};
                candidates.type = ime_event_type::candidates;
                candidates.candidate_count = count;
                seam.emit(candidates);
            } else if (wparam == IMN_CLOSECANDIDATE) {
                ime_event closed{};
                closed.type = ime_event_type::candidates;
                seam.emit(closed);
            }
            break;
        default:
            break;
        }
        return static_cast<std::intptr_t>(CallWindowProcW(previous, window, message, static_cast<WPARAM>(wparam),
                                                          static_cast<LPARAM>(lparam)));
    }
}
