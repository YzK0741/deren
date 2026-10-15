#pragma once
struct vessel_child_result { bool started = false; bool aborted = false; unsigned code = 0; };
vessel_child_result vessel_run_child(char const* executable, char const* mode, char const* log) noexcept;
void vessel_prepare_child() noexcept;
