from pathlib import Path
import re
root=Path.cwd()
for name in ('backends','tests','app'):
 for p in (root/'source'/name).rglob('*.cpp'):
  s=p.read_text();t=s.replace('std::format(', 'deren::utility::vessel::format(').replace('std::to_string(', 'deren::utility::vessel::to_string(')
  if t!=s:p.write_text(t)
for name,anchor in [('test_vulkan_capabilities.cpp','namespace deren::utility {'),('vessel_child.cpp','void vessel_prepare_child()')]:
 p=root/'source/tests'/name;s=p.read_text();s=s.replace('import deren.utility.shared;\n','');i=s.index(anchor);s=s[:i]+'import deren.utility.shared;\n\n'+s[i:];p.write_text(s)
p=root/'source/tests/test_texture_upload_layout.cpp';s=p.read_text().replace('#include "texture_regions_fixture.h"\n','').replace('import deren.utility.shared;','import deren.utility.shared;\n#include "texture_regions_fixture.h"');p.write_text(s)
p=root/'source/tests/test_shader_sources.cpp';s=p.read_text().replace('deren::utility::vessel::string const& path','std::filesystem::path const& path').replace('CHECK_MSG(file.is_open(), path.c_str());','CHECK_MSG(file.is_open(), path.string().c_str());');s=re.sub(r'deren::utility::vessel::string const (\w+) = ([^;]*\.string\(\));',r'auto const \1 = \2;',s);p.write_text(s)
