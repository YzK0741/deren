from pathlib import Path
for name in ('engine','utility','app','application_configuration','gltf_loader','gaussian_loader','backends','promise','tests'):
 for p in (Path('source')/name).rglob('*'):
  if p.suffix not in ('.cpp','.cppm'):continue
  if p.name=='shared_utility.cppm':continue
  s=p.read_text();t=s.replace('std::ostringstream','deren::utility::vessel::ostringstream').replace('std::istringstream','deren::utility::vessel::istringstream').replace('std::stringstream','deren::utility::vessel::stringstream').replace('std::format(', 'deren::utility::vessel::format(').replace('std::to_string(', 'deren::utility::vessel::to_string(')
  if t!=s:p.write_text(t)
p=Path('source/engine/runtime/runtime.constructor.cppm');s=p.read_text().replace('deren::utility::vessel::array const handles{&this->material_buffer','deren::utility::vessel::array<decltype(&this->material_buffer), 9> const handles{&this->material_buffer').replace('deren::utility::vessel::array const mappings{&this->material_mapped','deren::utility::vessel::array<decltype(&this->material_mapped), 9> const mappings{&this->material_mapped').replace('deren::utility::vessel::array const handles{this->material_buffer','deren::utility::vessel::array<decltype(this->material_buffer), 9> const handles{this->material_buffer');p.write_text(s)
p=Path('source/utility/shared/shared_utility.cppm');s=p.read_text().replace('module version: 1.0.0','module version: 1.1.0');p.write_text(s)
for name in ('source/backends/vulkan/core/core.declarations.cppm','source/backends/vulkan/graphical_user_interface/graphical_user_interface.cppm'):
 p=Path(name);s=p.read_text();import re;s=re.sub('module version: [0-9.a-z]+','module version: 1.0.0',s,count=1);p.write_text(s)
