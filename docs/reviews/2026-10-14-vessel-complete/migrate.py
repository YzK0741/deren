from pathlib import Path
import re
root=Path.cwd()
roots=['engine','utility','app','application_configuration','gltf_loader','gaussian_loader','backends','promise','tests']
pattern=re.compile(r'\bstd::(vector|deque|list|forward_list|map|multimap|set|multiset|unordered_map|unordered_multimap|unordered_set|unordered_multiset|queue|stack|priority_queue|basic_string|string|wstring|u8string|u16string|u32string|array)\b')
changed=[]
for name in roots:
    for p in (root/'source'/name).rglob('*'):
        if p.suffix not in ('.cpp','.cppm','.h','.hpp'): continue
        if p.name in ('shared_utility.cppm','vk_test.h'): continue
        s=p.read_text(encoding='utf-8-sig')
        t=pattern.sub(r'deren::utility::vessel::\1',s)
        if s==t: continue
        if p.suffix=='.cppm':
            if 'import deren.utility.shared;' not in t:
                t=re.sub(r'(^export module [^;]+;)',r'\1\nexport import deren.utility.shared;',t,count=1,flags=re.M)
        elif p.suffix=='.cpp' and 'module deren.' in t:
            if 'import deren.utility.shared;' not in t:
                t=re.sub(r'(^module deren\.[^;]+;)',r'\1\nimport deren.utility.shared;',t,count=1,flags=re.M)
        elif p.suffix=='.cpp':
            if 'import deren.utility.shared;' not in t:
                imports=list(re.finditer(r'^import [^;]+;',t,re.M))
                if imports:
                    i=imports[0].start(); t=t[:i]+'import deren.utility.shared;\n'+t[i:]
                else:
                    # All textual platform/STL headers precede the module import.
                    includes=list(re.finditer(r'^#include [^\n]+',t,re.M))
                    i=includes[-1].end() if includes else 0
                    t=t[:i]+'\n\nimport deren.utility.shared;\n'+t[i:]
        p.write_text(t,encoding='utf-8',newline='\n')
        changed.append(str(p.relative_to(root)))
(root/'docs/reviews/2026-10-14-vessel-complete/migration-files.txt').write_text('\n'.join(changed))
# Generated fixtures must use the production signatures and expose vessel before their bodies.
p=root/'source/tests/generate_vulkan_query_fixture.py'
s=p.read_text(); s=pattern.sub(r'deren::utility::vessel::\1',s); p.write_text(s)
# Contract layouts have changed: old DLLs must be refused before returning typed storage.
p=root/'source/promise/rhi/rhi.contract.cppm'
s=p.read_text().replace('abi_version = 32u;','abi_version = 33u;')
s=s.replace('    inline constexpr std::uint32_t abi_version = 33u;','    // ABI33: owned readback storage uses vessel allocator across the DLL boundary.\n    inline constexpr std::uint32_t abi_version = 33u;'); p.write_text(s)
p=root/'source/promise/gui/gui.cppm'; s=p.read_text().replace('gui_abi_version = 2u;','gui_abi_version = 3u;'); p.write_text(s)
for name in ['test_acceleration_structures.cpp','test_dynamic_link.cpp','test_runtime_dyn.cpp']:
 p=root/'source/tests'/name; s=p.read_text().replace('rhi::abi_version == 32u','rhi::abi_version == 33u'); p.write_text(s)
p=root/'source/tests/test_gui_plugin.cpp';s=p.read_text().replace('gui_abi_version == 2u','gui_abi_version == 3u').replace('GUI recording must use the RHI command buffer boundary','GUI ABI3 uses vessel-owned strings and containers');p.write_text(s)
print('migrated files:',len(changed))
