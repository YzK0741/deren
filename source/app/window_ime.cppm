// module version: 0.1.0
// THE WINDOWS IME SEAM, PROTOTYPED FOR M0 OF THE EDITOR UI LIBRARY.
//
// GLFW 3.5 has no preedit API: it delivers a commit string through the character callback and
// nothing at all about the composition in progress, so an editor that wants to draw its own
// composition text has to read the IMM messages on the window procedure. This module installs a
// second subclass beside the borderless frame (each one chains to the procedure it replaced), keeps
// the system composition window working for now, and reports what Windows actually sends, which is
// what docs/editor_ui_ime_report.md records.
//
// WHY A SUBCLASS AND NOT A MESSAGE HOOK: the messages are per window and arrive on the thread that
// created it; a thread-wide WH_GETMESSAGE hook would also see every other window's messages and
// would have to filter them back down to this HWND. The subclass sees exactly this window, and the
// previous procedure property makes a double install impossible (see glfw.cppm frame_proc).
module;
export module deren.app.window_ime;

export import deren.vstd;

export namespace deren::app {
    enum class ime_event_type {
        /// A composition began: `text` is empty and `caret` is 0.
        start,
        /// The composition text changed: `text` is the UTF-8 preedit and `caret` the caret in
        /// Unicode scalar values from its start.
        update,
        /// The user committed: `text` is the UTF-8 result. The system already removed the preedit.
        commit,
        /// The composition ended without a commit (Escape, or focus loss).
        end,
        /// The candidate list opened, closed or moved: `candidate_count` is the count reported, 0
        /// after it closed.
        candidates,
    };

    struct ime_event {
        ime_event_type type = ime_event_type::start;
        /// UTF-8. Empty for `start` and `end`.
        std::string text;
        /// Caret offset of the composition in Unicode scalar values.
        std::int32_t caret = 0;
        /// Candidates reported by the last WM_IME_NOTIFY, for `candidates`.
        std::int32_t candidate_count = 0;
        /// The candidate list's first page selection is not tracked yet: the prototype only counts.
        std::int32_t first_candidate = 0;
    };

    /// What the seam has seen so far. The acceptance report quotes these numbers, so they are
    /// counters of real messages rather than guesses.
    struct ime_report {
        std::uint32_t compositions_started = 0;
        std::uint32_t updates = 0;
        std::uint32_t commits = 0;
        std::uint32_t cancelled = 0;
        std::uint32_t candidate_lists = 0;
        /// The largest candidate list Windows opened in this session.
        std::int32_t largest_candidate_list = 0;
        /// True once a WM_IME_SETCONTEXT arrived asking for the system composition window.
        bool system_composition_window = false;
        /// True once a candidate window was positioned at the caret this seam asked for.
        bool candidate_followed_caret = false;
    };

    /// One IME seam per window. Install it once the window exists; it does nothing on other platforms.
    class window_ime {
    public:
        using sink = void (*)(void* opaque, ime_event const& event);

        window_ime() = default;
        window_ime(window_ime const&) = delete;
        window_ime& operator=(window_ime const&) = delete;
        ~window_ime();

        /// Installs the subclass on a native Win32 window handle. Returns false when the window is
        /// not Win32, when it already carries this seam, or when the subclass did not take.
        bool install(void* native_window) noexcept;
        void uninstall() noexcept;
        bool installed() const noexcept;

        void set_sink(sink callback, void* opaque) noexcept;

        /// The caret in CLIENT pixels, which is where the candidate window should point. The
        /// element tree knows it from the text field; the prototype takes it from the caller.
        void set_caret_rect(float x, float y, float width, float height) noexcept;

        /// Feeds a message to the seam and returns what the window procedure should return, or
        /// `handled = false` when this message is not an IME one. The prototype runs in observation
        /// mode: the system composition window keeps drawing, and the message is chained through.
        ime_report report() const noexcept;

        /// Sends one synthetic composition through the window: START, an update carrying `text` as
        /// the preedit, optionally a commit carrying the same text, then END. This is how the
        /// prototype is exercised without a real input method (DEREN_IMEPROBE=1 in the app), and it
        /// goes through the same window procedure a real session does, so the report's counters are
        /// counters of messages the seam really received. Returns false when the window has no
        /// context and one could not be created.
        bool probe_composition(std::string_view text, bool commit) noexcept;

        /// Exercises the seam's own text mapping with a surrogate pair: a synthetic composition
        /// cannot deliver one without an installed IME (IMM discards strings set on a context that
        /// is not composing), so this checks the two conversions the seam owns instead - UTF-16 to
        /// UTF-8, and the composition caret from UTF-16 units into Unicode scalar values. Returns
        /// the converted UTF-8 and caret; the caller compares them against the known answer.
        static bool probe_text_mapping(std::string& utf8, std::int32_t& caret) noexcept;

    private:
        // THE WINDOW PROCEDURE IS A STATIC MEMBER so it can reach the seam state without a friend
        // declaration or another global. Its parameter widths match Win32 exactly on Windows
        // (LRESULT/LPARAM are pointer-sized, WPARAM unsigned pointer-sized), so installing it is a
        // reinterpret_cast of the address; the module interface stays free of <windows.h> so every
        // importer does not inherit it.
        static std::intptr_t proc(void* window, unsigned message, std::uintptr_t wparam,
                                  std::intptr_t lparam) noexcept;
        void emit(ime_event const& event) noexcept;
        void place_candidate_window(void* window, void* context) noexcept;
        std::int32_t candidate_count(void* window) noexcept;

        void* window_ = nullptr;
        void* previous_proc_ = nullptr;
        sink sink_ = nullptr;
        void* sink_opaque_ = nullptr;
        float caret_x_ = 0.0F;
        float caret_y_ = 0.0F;
        float caret_width_ = 1.0F;
        float caret_height_ = 16.0F;
        /// True between STARTCOMPOSITION and the end of the composition, so END can tell a
        /// cancellation (no commit arrived) from the end that follows a commit.
        bool composing_ = false;
        ime_report report_{};
    };
}
