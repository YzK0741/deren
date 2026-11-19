#pragma once

// Keep the audio DLL in the executable import table without importing its C++ modules.
// This entry point does not initialize the mixer or open an audio device.
extern "C" void deren_audio_ensure_linked() noexcept;
