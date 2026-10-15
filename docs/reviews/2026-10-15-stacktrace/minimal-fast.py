from pathlib import Path
import re
p=Path('source/utility/shared/stacktrace_native.hpp');s=p.read_text();s=re.sub(r'void fast_panic\(char const\* message, std::size_t size, char const\* file = nullptr,\s*char const\* function = nullptr, unsigned line = 0\)', 'void fast_panic(char const* message, std::size_t size)',s);p.write_text(s)
p=Path('source/utility/shared/stacktrace_native.cpp');s=p.read_text();a=s.index('        void origin(');b=s.index('    } // namespace',a);s=s[:a]+s[b:];s=re.sub(r'void fast_panic\(char const\* message, std::size_t size, char const\* file,\s*char const\* function, unsigned line\)', 'void fast_panic(char const* message, std::size_t size)',s);s=s.replace('        origin(file, function, line);\n        write_trace(capture(1, capacity));\n','');p.write_text(s)
p=Path('source/utility/shared/shared_utility.cppm');s=p.read_text();a=s.index('    // Fatal/OOM entry:');s=s[:a]+'''    // Fatal/OOM entry: bounded raw text then abort. No formatting, location,
    // stack capture, heap objects, log queue/flush, GPU wait or subprocess.
    [[noreturn]] void fast_panic(std::string_view message = {}) noexcept;
} // namespace deren::utility
''';p.write_text(s)
p=Path('source/utility/shared/stacktrace.cpp');s=p.read_text().replace('fast_panic(std::string_view message, std::source_location location)','fast_panic(std::string_view message)').replace('message.data(), message.size(), location.file_name(), location.function_name(), location.line()','message.data(), message.size()');p.write_text(s)
p=Path('docs/superpowers/plans/2026-10-15-stacktrace.md');s=p.read_text();s+='\nUser correction supersedes initial fast_panic design: bounded raw text + abort only; no stacktrace, source_location or format-string overload. OOM uses this minimal native entry. Ordinary panic/explicit diagnostics retain stacks. Regression observed RED: two unexpected origin/stack outputs before removing them.\n';p.write_text(s)
