# Windows IME prototype report (M0)

This is the M0 acceptance item of `docs/editor_ui_library.md` section 7: a real Windows IME
prototype, whose results decide what M1's text pipeline has to own. It records what was built, what
was measured, and what could NOT be measured without an installed input method, so M1 does not
inherit a guess.

## 1. The problem

GLFW delivers typed text and nothing else. The build here is GLFW 3.5.1 (MSYS2 clang64) whose only
text entry point is `glfwSetCharCallback`: one UTF-32 code point per event, delivered when the input
method commits. There is no preedit callback, no caret position, no candidate list. An editor that
draws its own text field therefore cannot show what is being composed.

The two candidate designs were:

1. a thread-wide `WH_GETMESSAGE` hook, filtered back down to one HWND, and
2. a window subclass that observes the IMM messages for exactly this window.

The subclass was chosen: the messages are per window and arrive on the thread that created it, the
hook would see every other window's traffic, and there is already a precedent in the app (the
borderless frame in `source/app/glfw.cppm` subclasses the same window and chains through a window
property). Two subclasses can coexist because each one stores the procedure it replaced.

## 2. What was built

`source/app/window_ime.cppm` / `.cpp` (module `deren.app.window_ime`):

- installs a subclass on the GLFW window's Win32 handle, keeping the previous procedure on the HWND
  as `deren.ime.previous_proc` so a second install is impossible and a stale handle cannot resurrect
  a dead chain; every message is chained to the previous procedure, including the IME ones,
- translates the messages into events: `start`, `update` (UTF-8 preedit + caret in Unicode scalar
  values), `commit` (UTF-8 result), `end`, `candidates` (count),
- moves the candidate window to the caret with `ImmSetCandidateWindow(..., CFS_CANDIDATEPOS)`, using
  the caret rect in client pixels converted to screen pixels, and an excluded rectangle the size of
  the caret so the list never covers what is being typed,
- counts what Windows actually sent: compositions started, updates, commits, cancellations,
  candidate lists, largest candidate page, whether `WM_IME_SETCONTEXT` asked for the system
  composition window, and whether a candidate window was positioned.

`source/app/run.cpp` installs the seam after the action seam (the runtime's constructor registers the
camera callbacks first, see the comment there), types a commit through
`glfw_action_input::type_character` so the managed side sees one event kind for keyboard and IME
characters, and logs every event. `DEREN_IMEPROBE=1` drives one synthetic composition and one text
mapping check at startup.

## 3. What was measured

The seam runs in observation mode: the system composition window keeps drawing, so nothing in this
prototype changes what a user sees, and the counters record what a real session would deliver.

Verified with real messages (windowed run, `--capture-windowed`, log lines `ime: ...`):

- the seam installs on the window created for the frame loop (an offscreen capture has no window at
  all, so `--capture-frames` runs report `handle 0x0` and are NOT a valid probe),
- `WM_IME_STARTCOMPOSITION` / `WM_IME_ENDCOMPOSITION` reach the seam and are counted; an end that no
  commit consumed is a cancellation, tracked by a composing flag rather than by assuming the end
  after a commit is one,
- the previous procedure still receives every message: the app keeps rendering, its frame loop and
  the borderless frame drag behaviour are unchanged in the same run,
- the text mapping the seam owns: UTF-16 to UTF-8 with a surrogate pair (`U+3042 U+1F600 U+597D` ->
  10 UTF-8 bytes) and the composition caret from UTF-16 units to scalar values (4 units -> 3), which
  is what a preedit caret index must be.

NOT verified, and it cannot be on this machine:

- preedit and commit CONTENT. With no input method installed, `ImmSetCompositionStringW(SCS_SETSTR)`
  returns TRUE and `ImmGetCompositionStringW(GCS_COMPSTR)` then reads back 0 bytes: IMM accepts the
  call and discards the string, because a context that is not composing has no composition string to
  set. The same applies to the result string, so a synthetic session can prove dispatch and
  counters, not text delivery.
- candidate list contents and the real candidate window position. `ImmGetCandidateListW` is called
  and its count is reported, but with no IME open there is no page to read, so the largest page
  stays 0.

## 4. Manual verification steps

A real session needs a person and an installed IME (Microsoft Pinyin is what this was written
against):

1. `deren.exe run . --editor-ui=next --capture-windowed --capture-frames 600` from
   `build/editor-animation-demo` (any project directory works; the editor UI is not needed for the
   probe, but the window is),
2. focus the window, switch to Microsoft Pinyin, type `nihao` without committing, watch for
   `ime: composition started`, repeated `ime: preedit '...' caret N`, and
   `ime: candidates N`,
3. press Space or pick a candidate, watch for `ime: committed U+...` per scalar and
   `ime: composition ended`; press Escape instead and watch `ime: composition ended without a commit`,
4. read the counters back from `ime: probe composed ...` only if `DEREN_IMEPROBE=1` was set; a real
   session's totals are in `ime_report`, which the report quotes.

## 5. What this means for M1

- The subclass is the right seam: it costs one property per window, it chains cleanly with the
  borderless frame, and it gives the caret-relevant messages that GLFW does not.
- M1 must own the composition UI. Observation mode is a prototype only: M1 strips
  `ISC_SHOWUICOMPOSITIONWINDOW` in `WM_IME_SETCONTEXT` and draws the preedit from the `update`
  events, which is why the caret index is converted into scalar values here rather than into UTF-16
  units - the element tree's caret model is scalar based (see the cluster rule in
  `docs/editor_ui_library.md` section 6).
- The caret rect has to come from the element tree: `window_ime::set_caret_rect` takes client
  pixels, and M1 calls it every frame from the focused text field's caret, not once at startup.
- The commit path is already the same one keyboard characters use, so the managed side needs no
  IME-specific handling: a committed scalar becomes an ordinary character event.
- No IME is installed on this machine, so content-level acceptance (preedit text, candidate page,
  candidate window position) stays a manual item in M1's list; the plumbing is verified, the strings
  are not.
