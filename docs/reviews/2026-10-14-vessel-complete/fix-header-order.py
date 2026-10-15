from pathlib import Path
for filename,inc in [('source/backends/vulkan/graphical_user_interface/gui_dll.cpp','#include "panel_adapter_storage.hpp"'),('source/tests/test_gui_plugin.cpp','#include "../backends/vulkan/graphical_user_interface/panel_adapter_storage.hpp"')]:
 p=Path(filename);s=p.read_text().replace(inc+'\n','');marker='#include "../../../promise/gui/gui_entry.hpp"' if 'gui_dll' in filename else '#include "../promise/gui/gui_entry.hpp"';s=s.replace(marker,inc+'\n'+marker);p.write_text(s)
p=Path('source/tests/test_docs.cpp');s=p.read_text().replace('deren::utility::vessel::string const& path','std::filesystem::path const& path').replace('CHECK_MSG(file.is_open(), path.c_str());','CHECK_MSG(file.is_open(), path.string().c_str());');p.write_text(s)
