from pathlib import Path
import re,json
roots=['engine','utility','app','application_configuration','gltf_loader','gaussian_loader','backends','promise','tests']
pattern=re.compile(r'\bstd::(?:vector|deque|list|forward_list|map|multimap|set|multiset|unordered_map|unordered_multimap|unordered_set|unordered_multiset|queue|stack|priority_queue|basic_string|string|wstring|u8string|u16string|u32string|array|ostringstream|istringstream|stringstream)\b')
files=0;violations=[]
for root in roots:
 for p in (Path('source')/root).rglob('*'):
  if p.suffix not in ('.cpp','.cppm','.h','.hpp'):continue
  if p.name=='shared_utility.cppm':continue
  files+=1
  for line,text in enumerate(p.read_text(encoding='utf-8-sig').splitlines(),1):
   if pattern.search(text): violations.append(dict(path=str(p),line=line,text=text))
report=dict(scanned_project_files=files,excluded=['third_party','vstd','reference','shared_utility.cppm alias implementation'],old_std_container_mentions=violations)
Path('docs/reviews/2026-10-14-vessel-complete/container-census.json').write_text(json.dumps(report,indent=2))
assert not violations, violations
print('PASS:',files,'project source/header files, zero old std container declarations or mentions outside alias implementation')
