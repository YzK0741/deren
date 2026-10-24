# Startup Splash Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan in the current workspace.

**Goal:** Display the approved dark white wordmark during interactive Windows startup.

**Architecture:** An application-owned native window runs its own message loop. An embedded bitmap removes runtime file dependencies. Close after the first successful present; destruction covers early exits.

**Tech Stack:** C++, Win32 / GDI, existing windres resource build.

**Spec:** `docs/superpowers/specs/2026-10-24-startup-splash-design.md`

## Constraints and review focus

- Preserve current dirty work; no commit requested.
- No Vulkan dependency for the splash; non-Windows implementation is a no-op.
- Captures/offscreen/version must not display a splash.
- Window/thread/resource failures skip the splash without blocking startup.
- Close must be idempotent, owner attachment must not race window destruction.
- Loading must not freeze repainting; main-window activation must not cover the splash.

## Task 1: Native splash and application integration

- [x] Add `source/app/startup_splash.hpp` and `.cpp`: constructor(bool enabled), attach_owner(GLFWwindow*), close(), destructor; noncopyable RAII.
- [x] Convert approved dark wordmark into embedded BMP; add resource ID 102 and CMake dependencies/linkage.
- [x] Start after configuration, attach the GLFW native window, close after successful present. Skip captures and offscreen runs.
- [x] Build with 14 jobs. Exercise disabled/enabled/owner/repaint/repeated-close paths in a native smoke harness.
- [x] Run application capture to verify normal rendering remains intact; inspect a native splash capture.

## Execution record

Design choice approved by user. Details above are routine implementation decisions; continue without another approval round. The Windows implementation intentionally leaves other platforms' startup unchanged.

Completed: build target `deren` succeeded with 14 jobs. Native smoke checks passed disabled, visible, owner attachment, repaint during loading, eight repeated create/close cycles, repeated close, scope cleanup and missing-resource fallback. The non-Windows no-op branch compiled with `_WIN32` undefined (not a full Linux build). Actual interactive Gaussian startup showed the splash, dismissed it after first presentation, and kept the renderer alive; graceful window close exited 0. Eight-frame windowed capture exited 0, produced a PNG, never displayed a splash, and retained GUI content (1826 bright text pixels in the panel check). Preview: `tmp/startup-splash-preview.png`. Independent final review found no actionable issues.

Ruling: create GLFW owner immediately before splash, earlier than previously, so reverse destruction order always joins the splash before destroying the owner, including early returns. No changes to runtime/Vulkan initialization logic.
