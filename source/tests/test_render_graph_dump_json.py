"""Validate public dump JSON with an independent standards-compliant parser."""
import json
import re
import subprocess
import sys


def read(mode):
    result = subprocess.run([sys.argv[1], mode], check=True, capture_output=True)
    return json.loads(result.stdout.decode("utf-8"))


plan = read("--sample-json")
assert plan["schema_version"] == 1 and plan["kind"] == "compiled_graph"
assert plan["synchronization_planned"] is True
assert plan["execution_order"] == [1, 2]
image, buffer = plan["resources"]
assert image["identity"] == {"device": str(2**64 - 1), "object": str(2**64 - 2)}
assert image["name"] == 'HDR "line"\n\t\\\0测试\ufffd'
assert image["shape"]["mip_levels"] == 2
assert image["use_interval"] == {"first": 0, "last": 1}
assert buffer["use_interval"] == {"first": 1, "last": 1}
assert image["declared_final"]["last_version"] == "1"
assert image["declared_final"]["last_producer"] is None
assert plan["passes"][0]["cull_reason"] == "not_reachable_from_outputs_or_side_effects"
assert plan["passes"][2]["parameters_present"] is True
assert plan["passes"][1]["accesses"][0]["scope"]["usage"] == "storage"
assert any(edge["resource"] is None for edge in plan["dependencies"])
assert any(read["producer"] is None for read in plan["reads"])
assert plan["outputs"][0]["range"]["mip_count"] == 1
assert len(plan["barriers"]) == 2 and plan["final_states"]
assert plan["barriers"][0]["after"]["layout"] == "general"
assert plan["barriers"][0]["after"]["stages"] == ["compute_shader"]
assert plan["barriers"][0]["after"]["access"] == ["write"]
assert plan == read("--sample-json"), "dump must be deterministic"

text = subprocess.run([sys.argv[1], "--sample-text"], check=True, capture_output=True).stdout.decode("utf-8")
lines = text.splitlines()
assert lines[0] == "render_graph schema=1 synchronization=planned"
assert json.loads(lines[1].removeprefix("execution_order: ")) == plan["execution_order"]
groups = {"resource": "resources", "pass": "passes", "dependency": "dependencies", "read": "reads",
          "output": "outputs", "barrier": "barriers", "final_state": "final_states"}
counts = dict.fromkeys(groups, 0)
for line in lines[2:]:
    match = re.fullmatch(r"(\w+)\[(\d+)\]: (.*)", line)
    assert match, line
    kind, index, body = match.groups()
    assert int(index) == counts[kind]
    assert json.loads(body) == plan[groups[kind]][int(index)]
    counts[kind] += 1
assert all(counts[kind] == len(plan[group]) for kind, group in groups.items())

error = read("--error-json")
assert error["kind"] == "graph_error" and error["code"] == "invalid_range"
assert error["pass"] is None and error["resource"] == 3
assert error["pass_name"] == "bad\npass"
assert error["range"]["offset"] == str(2**64 - 1)
assert error["range"]["size"] == str(2**64 - 1)

unicode = read("--unicode-json")
raw = bytes(range(256)) + bytes.fromhex("c2a2e6b58bf0908080c0afeda080f4908080e2")
assert unicode["resource_name"] == raw.decode("utf-8", errors="replace")

empty = read("--empty-json")
assert empty["synchronization_planned"] is False
for key in ("resources", "passes", "dependencies", "reads", "execution_order", "outputs", "barriers", "final_states"):
    assert empty[key] == [], key
print("render graph dump JSON parsed and validated")
