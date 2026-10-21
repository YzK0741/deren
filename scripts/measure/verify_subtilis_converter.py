#!/usr/bin/env python3
"""Acceptance gate, independent validators and LOD0/cut geometry checks for the Subtilis converter.

Subcommands
  gate        Structural gate over `measurements.json`: every asset must have every configured
              (threads, round) run; every run must be successful and structurally complete (all
              eleven stage rows, finite non-negative times, accumulated == sum of exclusive rows,
              rounds present, real cross-group DAG for the --dag-required assets, identical payload
              bytes across threads/rounds); medians must be recomputed from ALL recorded rounds and
              match, never best-only. `--self-test` runs hand-broken variants of a valid measurement
              and FAILS unless every one of them is rejected (the negative control).
  check       Runs the pinned official Khronos core validator (SHA-256-checked manifest) on a
              normal converter output and on export-cut previews, plus `deren-subtilis validate
              --full` on the normal output, and checks LOD0/cut geometry identity (oriented triangle
              multiset, attributes, nodes/scenes/materials, monotone cut sizes). Writes acceptance.json.

No timing threshold is asserted anywhere: the gate is about completeness, structure and identity.
Exit code 0 only when every required check passes; findings are printed.
"""

import argparse
import hashlib
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import subtilis_converter_gltf as gltf  # noqa: E402

STAGE_NAMES = [
    "sourceFingerprint",
    "leafCluster",
    "topology",
    "partition",
    "boundary",
    "simplify",
    "recluster",
    "candidate",
    "commit",
    "validate",
    "encode",
]
INCLUSIVE_STAGES = {"candidate"}
OFFICIAL_VALIDATOR_VERSION = "2.0.0-dev.3.10"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def close_enough(left, right, relative=1e-9, absolute=1e-9):
    if not is_number(left) or not is_number(right):
        return False
    return abs(float(left) - float(right)) <= max(absolute, relative * max(1.0, abs(float(left))))


def check_measurements(measurements):
    """Return the list of findings (empty list = accepted)."""
    findings = []
    if not isinstance(measurements, dict) or measurements.get("schema") != "subtilis-converter-measurements/1":
        return ["measurements: unexpected or missing schema"]
    converter = measurements.get("converter", {})
    if not converter.get("sha256") or not converter.get("path"):
        findings.append("measurements: converter identity (path/sha256) missing")
    parameters = measurements.get("parameters", {})
    threads = parameters.get("threads")
    rounds = parameters.get("rounds")
    if not isinstance(threads, list) or not threads or not all(isinstance(item, int) and item >= 1 for item in threads):
        findings.append("parameters: threads must be a non-empty list of positive integers")
        return findings
    if not isinstance(rounds, int) or rounds < 1:
        findings.append("parameters: rounds must be a positive integer")
        return findings
    assets = measurements.get("assets")
    if not isinstance(assets, list) or not assets:
        return findings + ["measurements: no assets recorded"]
    if not any(asset.get("dag_required") for asset in assets):
        findings.append("measurements: no asset marked dag_required, so no real cross-group DAG is required")
    for asset in assets:
        name = asset.get("name", "?")
        if not asset.get("path") or not asset.get("source_sha256"):
            findings.append(f"{name}: source path/sha256 missing")
        runs = asset.get("runs")
        if not isinstance(runs, list) or not runs:
            findings.append(f"{name}: no runs recorded")
            continue
        expected = {(thread, round_index) for thread in threads for round_index in range(1, rounds + 1)}
        observed = [(run.get("threads"), run.get("round")) for run in runs]
        for pair in sorted(expected, key=lambda item: (item[0], item[1])):
            if observed.count(pair) == 0:
                findings.append(f"{name}: missing run threads={pair[0]} round={pair[1]} (round coverage incomplete)")
            elif observed.count(pair) > 1:
                findings.append(f"{name}: duplicate run threads={pair[0]} round={pair[1]}")
        for pair in observed:
            if pair not in expected:
                findings.append(f"{name}: unexpected run threads={pair[0]} round={pair[1]}")
        stage_reports = []
        payload_hashes = set()
        for run in runs:
            thread_count, round_index = run.get("threads"), run.get("round")
            label = f"{name} threads={thread_count} round={round_index}"
            if run.get("exit") != 0:
                findings.append(f"{label}: build exited {run.get('exit')}")
            if not is_number(run.get("external_wall_ms")) or float(run["external_wall_ms"]) <= 0:
                findings.append(f"{label}: external wall time missing or not positive")
            if run.get("peak_working_set_bytes") is not None and (not is_number(run["peak_working_set_bytes"]) or float(run["peak_working_set_bytes"]) <= 0):
                findings.append(f"{label}: sampled peak working set is not positive")
            if not run.get("output_sha256"):
                findings.append(f"{label}: output payload hash missing")
            else:
                payload_hashes.add(run["output_sha256"])
            if not run.get("output_bytes") or int(run["output_bytes"]) <= 0:
                findings.append(f"{label}: output file size missing")
            report = run.get("report")
            if not isinstance(report, dict) or report.get("_missing") or report.get("_parse_error"):
                findings.append(f"{label}: build report missing or unparsable")
                continue
            stage_reports.append((label, report))
            if report.get("threads") != thread_count:
                findings.append(f"{label}: report threads={report.get('threads')} does not match the requested budget")
            if int(report.get("primitives", 0)) < 1:
                findings.append(f"{label}: report has no converted primitives")
            if int(report.get("rounds_max", 0)) < 1:
                findings.append(f"{label}: report records no reduction round")
            if not is_number(report.get("accumulated_ms")) or float(report.get("accumulated_ms")) <= 0:
                findings.append(f"{label}: accumulated stage time missing or not positive")
            if not is_number(report.get("wall_ms")) or float(report.get("wall_ms")) <= 0:
                findings.append(f"{label}: reported wall time missing or not positive")
            if not is_number(report.get("known_work_peak_bytes_max")) or float(report.get("known_work_peak_bytes_max", 0)) <= 0:
                findings.append(f"{label}: known-work peak bytes missing")
            if not is_number(report.get("known_work_limit_bytes_min")) or float(report.get("known_work_limit_bytes_min", 0)) <= 0:
                findings.append(f"{label}: known-work budget limit missing")
            elif is_number(report.get("known_work_peak_bytes_max")) and float(report["known_work_peak_bytes_max"]) > float(report["known_work_limit_bytes_min"]):
                findings.append(f"{label}: known-work peak exceeds the recorded budget limit")
            stages = report.get("stages")
            if not isinstance(stages, dict):
                findings.append(f"{label}: stage profile missing")
            else:
                for stage_name in STAGE_NAMES:
                    stage = stages.get(stage_name)
                    if not isinstance(stage, dict):
                        findings.append(f"{label}: stage {stage_name} missing")
                        continue
                    if not is_number(stage.get("cpu_ms")) or float(stage["cpu_ms"]) < 0:
                        findings.append(f"{label}: stage {stage_name} time is not finite/non-negative")
                    if not isinstance(stage.get("count"), int) or stage["count"] < 0:
                        findings.append(f"{label}: stage {stage_name} task count is invalid")
                    if bool(stage.get("inclusive")) != (stage_name in INCLUSIVE_STAGES):
                        findings.append(f"{label}: stage {stage_name} inclusive flag is wrong")
                    if int(stage.get("primitives_with_stage", 0)) != int(report.get("primitives", 0)):
                        findings.append(f"{label}: stage {stage_name} not reported for every primitive")
                if len(stages) != len(STAGE_NAMES):
                    findings.append(f"{label}: unexpected extra stage rows")
            levels = report.get("levels")
            if not isinstance(levels, list) or not levels:
                findings.append(f"{label}: frontier level table missing")
            else:
                for level in levels:
                    if not isinstance(level, dict) or any(not isinstance(level.get(key), int) or level.get(key) < 0 for key in ("triangles", "clusters", "groups", "roots")):
                        findings.append(f"{label}: frontier level table has an invalid entry")
                        break
                if int(levels[-1].get("groups", 0)) > int(report.get("groups_final", 0)):
                    findings.append(f"{label}: level-table group count exceeds the final group count")
        for label, report in stage_reports:
            stages = report.get("stages", {})
            if not isinstance(stages, dict) or any(name not in stages for name in STAGE_NAMES):
                continue  # already reported as a missing stage row
            exclusive = sum(float(stages[name]["cpu_ms"]) for name in STAGE_NAMES if name not in INCLUSIVE_STAGES)
            if not close_enough(report.get("accumulated_ms"), exclusive, relative=1e-6, absolute=1e-3):
                findings.append(f"{label}: accumulated time {report.get('accumulated_ms')} != exclusive stage sum {exclusive} (double counting)")
            primitives = int(report.get("primitives", 0))
            counts = {name: int(stages[name].get("count", 0)) for name in STAGE_NAMES}
            for required_stage in ("sourceFingerprint", "leafCluster", "topology", "validate", "encode"):
                if counts[required_stage] != primitives:
                    findings.append(f"{label}: stage {required_stage} ran {counts[required_stage]} times for {primitives} primitives")
            if counts["partition"] < primitives:
                findings.append(f"{label}: partition stage count {counts['partition']} < primitives {primitives}")
            if counts["candidate"] != counts["commit"]:
                findings.append(f"{label}: candidate/commit task counts differ")
            if counts["candidate"] < primitives:
                findings.append(f"{label}: no reduction candidates recorded")
        if asset.get("dag_required"):
            cross = 0
            groups = 0
            converted = 0
            for run in runs:
                report = run.get("report") or {}
                cross = max(cross, int(report.get("cross_group_repartitions", 0)))
                groups = max(groups, int(report.get("groups_final", 0)))
                converted = max(converted, int(report.get("converted", 0)))
            if converted < 1:
                findings.append(f"{name}: dag_required asset converted no primitives")
            if groups < 1:
                findings.append(f"{name}: dag_required asset produced no reduction group")
            if cross < 1:
                findings.append(f"{name}: dag_required asset shows no real cross-group DAG repartition")
        identity = asset.get("identity") or {}
        distinct = [item for item in identity.get("distinct_output_sha256", []) if item]
        if not identity.get("byte_identical_across_threads_and_rounds"):
            findings.append(f"{name}: payload is not byte-identical across threads/rounds")
        if len(distinct) != 1:
            findings.append(f"{name}: expected exactly one distinct output payload hash, got {len(distinct)}")
        elif payload_hashes and set(payload_hashes) != {distinct[0]}:
            findings.append(f"{name}: recorded output hashes disagree with the identity claim")
        medians = asset.get("medians")
        if not isinstance(medians, dict):
            findings.append(f"{name}: medians missing")
            continue
        for thread in threads:
            thread_runs = [run for run in runs if run.get("threads") == thread and run.get("exit") == 0 and isinstance(run.get("report"), dict)]
            median = medians.get(str(thread))
            if not isinstance(median, dict):
                findings.append(f"{name} threads={thread}: median block missing")
                continue
            if len(thread_runs) != rounds:
                findings.append(f"{name} threads={thread}: medians cover {len(thread_runs)} of {rounds} rounds (best-only)")
                continue
            try:
                expected_wall = statistics.median([float(run["external_wall_ms"]) for run in thread_runs])
                expected_report_wall = statistics.median([float(run["report"]["wall_ms"]) for run in thread_runs])
                expected_accumulated = statistics.median([float(run["report"]["accumulated_ms"]) for run in thread_runs])
                expected_bytes = statistics.median([int(run["output_bytes"]) for run in thread_runs])
                expected_cross = statistics.median([int(run["report"]["cross_group_repartitions"]) for run in thread_runs])
            except (KeyError, TypeError, ValueError) as error:
                findings.append(f"{name} threads={thread}: medians cannot be verified ({error})")
                continue
            if not close_enough(median.get("external_wall_ms"), expected_wall):
                findings.append(f"{name} threads={thread}: external wall median is not the median of all rounds")
            if not close_enough(median.get("report_wall_ms"), expected_report_wall):
                findings.append(f"{name} threads={thread}: reported wall median is not the median of all rounds")
            if not close_enough(median.get("accumulated_ms"), expected_accumulated):
                findings.append(f"{name} threads={thread}: accumulated median is not the median of all rounds")
            if median.get("output_bytes") != expected_bytes:
                findings.append(f"{name} threads={thread}: output-size median is not the median of all rounds")
            if median.get("cross_group_repartitions") != expected_cross:
                findings.append(f"{name} threads={thread}: cross-group DAG median is not the median of all rounds")
            stage_medians = median.get("stages") or {}
            for stage_name in STAGE_NAMES:
                try:
                    expected_cpu = statistics.median([float(run["report"]["stages"][stage_name]["cpu_ms"]) for run in thread_runs])
                except (KeyError, TypeError, ValueError) as error:
                    findings.append(f"{name} threads={thread}: stage {stage_name} median cannot be verified ({error})")
                    continue
                stage = stage_medians.get(stage_name) or {}
                if not close_enough(stage.get("cpu_ms"), expected_cpu, relative=1e-6, absolute=1e-6):
                    findings.append(f"{name} threads={thread}: stage {stage_name} median is not the median of all rounds")
            peaks = [run["peak_working_set_bytes"] for run in thread_runs if run.get("peak_working_set_bytes") is not None]
            if peaks and not close_enough(median.get("peak_working_set_bytes"), statistics.median(peaks), relative=1e-6):
                findings.append(f"{name} threads={thread}: peak working-set median is not the median of all rounds")
    return findings


# --------------------------------------------------------------------------- negative control
def _self_test_base():
    threads = [1, 12]
    rounds = 2
    payload_hash = "a" * 64
    levels = [
        {"triangles": 100, "clusters": 10, "groups": 0, "roots": 10},
        {"triangles": 60, "clusters": 6, "groups": 2, "roots": 6},
    ]
    counts = {
        "sourceFingerprint": 1, "leafCluster": 1, "topology": 1, "partition": 2,
        "boundary": 2, "simplify": 2, "recluster": 2, "candidate": 2, "commit": 2,
        "validate": 1, "encode": 1,
    }
    stages = {
        name: {
            "primitives_with_stage": 1,
            "cpu_ms": 2.0 if name in INCLUSIVE_STAGES else 1.0,
            "count": counts[name],
            "inclusive": name in INCLUSIVE_STAGES,
        }
        for name in STAGE_NAMES
    }
    runs = []
    for thread in threads:
        for round_index in range(1, rounds + 1):
            runs.append({
                "threads": thread,
                "round": round_index,
                "exit": 0,
                "external_wall_ms": 100.0 + thread + round_index,
                "peak_working_set_bytes": 1000000 + thread * 1000 + round_index,
                "output_bytes": 2000 + round_index,
                "output_sha256": payload_hash,
                "output_path": "unused.glb",
                "report_path": "unused.report.json",
                "report": {
                    "converted": 1, "reused": 0, "skipped": 0, "lod0Only": 0,
                    "threads": thread, "primitives": 1, "rounds_max": 2,
                    "cross_group_repartitions": 2, "groups_final": 2,
                    "rejected": {"noProgress": 1},
                    "known_work_peak_bytes_max": 1000, "known_work_limit_bytes_min": 100000,
                    "accumulated_ms": 10.0, "wall_ms": 12.0,
                    "stages": json.loads(json.dumps(stages)),
                    "levels": json.loads(json.dumps(levels)),
                },
            })
    medians = {}
    for thread in threads:
        thread_runs = [run for run in runs if run["threads"] == thread]
        medians[str(thread)] = {
            "runs": len(thread_runs),
            "external_wall_ms": statistics.median([run["external_wall_ms"] for run in thread_runs]),
            "peak_working_set_bytes": statistics.median([run["peak_working_set_bytes"] for run in thread_runs]),
            "output_bytes": statistics.median([run["output_bytes"] for run in thread_runs]),
            "converted": 1, "skipped": 0, "lod0Only": 0, "rounds_max": 2,
            "cross_group_repartitions": 2, "groups_final": 2,
            "report_wall_ms": statistics.median([run["report"]["wall_ms"] for run in thread_runs]),
            "accumulated_ms": statistics.median([run["report"]["accumulated_ms"] for run in thread_runs]),
            "stages": {
                name: {
                    "cpu_ms": statistics.median([run["report"]["stages"][name]["cpu_ms"] for run in thread_runs]),
                    "count": statistics.median([run["report"]["stages"][name]["count"] for run in thread_runs]),
                }
                for name in STAGE_NAMES
            },
        }
    return {
        "schema": "subtilis-converter-measurements/1",
        "converter": {"path": "converter", "sha256": "0" * 64, "bytes": 1},
        "environment": {"platform": "self-test"},
        "parameters": {"threads": threads, "rounds": rounds},
        "assets": [{
            "name": "large",
            "path": "large.glb",
            "dag_required": True,
            "source_bytes": 1,
            "source_sha256": "1" * 64,
            "runs": runs,
            "medians": medians,
            "identity": {"distinct_output_sha256": [payload_hash], "byte_identical_across_threads_and_rounds": True},
        }],
    }


def self_test():
    def run_gate(mutate):
        data = _self_test_base()
        mutate(data)
        return check_measurements(data)

    base_findings = run_gate(lambda data: None)
    failures = []
    if base_findings:
        failures.append(f"valid base measurement was rejected: {base_findings[0]}")
    cases = []

    def remove_stage(data):
        data["assets"][0]["runs"][1]["report"]["stages"].pop("simplify")

    cases.append(("missing-stage", remove_stage, "simplify"))
    cases.append(("missing-round", lambda data: data["assets"][0]["runs"].pop(), "round"))
    cases.append(("best-only-medians", lambda data: data["parameters"].__setitem__("rounds", 3), "missing run"))
    cases.append(("no-real-dag", lambda data: [run["report"].__setitem__("cross_group_repartitions", 0) for run in data["assets"][0]["runs"]], "cross-group"))
    cases.append(("accumulated-double-counted", lambda data: data["assets"][0]["runs"][0]["report"].__setitem__("accumulated_ms", 99.0), "accumulated"))
    cases.append(("threads-not-reported", lambda data: data["assets"][0]["runs"][0]["report"].__setitem__("threads", 7), "threads"))
    def payload_not_identical(data):
        data["assets"][0]["runs"][0]["output_sha256"] = "b" * 64
        data["assets"][0]["identity"]["byte_identical_across_threads_and_rounds"] = False
    cases.append(("payload-not-identical", payload_not_identical, "byte-identical"))
    cases.append(("wrong-inclusive-flag", lambda data: data["assets"][0]["runs"][0]["report"]["stages"]["candidate"].__setitem__("inclusive", False), "inclusive"))
    cases.append(("wall-time-missing", lambda data: data["assets"][0]["runs"][0]["report"].pop("wall_ms"), "wall"))
    cases.append(("median-not-from-all-rounds", lambda data: data["assets"][0]["medians"]["12"].__setitem__("external_wall_ms", 1.0), "median"))
    for name, mutate, expected in cases:
        findings = run_gate(mutate)
        text = " | ".join(findings)
        if not findings or expected not in text:
            failures.append(f"negative control {name!r} was accepted (expected a finding about {expected!r})")
        else:
            print(f"negative control {name}: rejected -> {findings[0]}")
    print(f"self-test: {len(cases) + 1} cases, {len(failures)} failed")
    for failure in failures:
        print(f"  FAIL {failure}")
    return failures


# --------------------------------------------------------------------------- independent checks
def run_command(command, timeout=3600):
    completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    return completed.returncode, completed.stdout, completed.stderr


def khronos_issues(validator, path):
    """Run `gltf_validator --stdout` and parse its JSON report."""
    code, stdout, stderr = run_command([str(validator), "--stdout", str(path)])
    text = stdout.strip()
    start = text.find("{")
    if code != 0 or start < 0:
        return {"_error": f"validator exit {code}: {stderr.strip()[:400]}"}
    try:
        report = json.loads(text[start:])
    except json.JSONDecodeError as error:
        return {"_error": f"validator JSON parse failed: {error}"}
    issues = report.get("issues", {})
    messages = issues.get("messages", [])
    unsupported = [message for message in messages if message.get("code") == "UNSUPPORTED_EXTENSION" and "DEREN_subtilis" in str(message.get("message", ""))]
    return {
        "validator_version": report.get("validatorVersion"),
        "numErrors": int(issues.get("numErrors", -1)),
        "numWarnings": int(issues.get("numWarnings", -1)),
        "numInfos": int(issues.get("numInfos", -1)),
        "numHints": int(issues.get("numHints", -1)),
        "messages_total": len(messages),
        "deren_unsupported_info": len(unsupported),
        "deren_unsupported_message": unsupported[0]["message"] if unsupported else None,
    }


def triangle_count(document):
    return sum(len(gltf.indices(document, primitive)) // 3 for _, _, primitive in gltf.primitives(document))


def check_asset(source_path, normal_path, converter, validator, work_dir, thresholds):
    asset_report = {"source": str(source_path), "normal_output": str(normal_path), "findings": [], "previews": []}
    findings = asset_report["findings"]
    source = gltf.load_gltf(source_path)
    normal = gltf.load_gltf(normal_path)
    # 1. normal output: independent Khronos core validator + deren full validator + LOD0 identity
    asset_report["khronos_normal"] = khronos_issues(validator, normal_path)
    if asset_report["khronos_normal"].get("numErrors") != 0:
        findings.append(f"Khronos validator reports {asset_report['khronos_normal'].get('numErrors', 'an error')} core error(s) on the normal output")
    code, stdout, stderr = run_command([str(converter), "validate", str(normal_path), "--full"])
    asset_report["deren_validate_full"] = {"exit": code, "stdout": stdout.strip()[:2000], "stderr": stderr.strip()[:2000]}
    if code != 0:
        findings.append(f"deren-subtilis validate --full failed (exit {code})")
    asset_report["lod0_and_core_problems"] = gltf.compare_lod0_and_core(source, normal)
    findings.extend(asset_report["lod0_and_core_problems"][:5])
    converted = sum(1 for _, _, primitive in gltf.primitives(normal) if gltf.has_deren_extension(primitive))
    asset_report["converted_primitives"] = converted
    if converted < 1:
        findings.append("normal output has no converted primitive")
    source_triangles = triangle_count(source)
    # 2. export-cut previews at several thresholds: independent validation + geometry identity
    normal_triangles = triangle_count(normal)
    cut_counts = {}
    for threshold in thresholds:
        preview = work_dir / f"{source_path.stem}-cut-{threshold}.glb"
        if preview.exists():
            preview.unlink()
        code, stdout, stderr = run_command([str(converter), "export-cut", str(normal_path), "--error", str(threshold), "-o", str(preview)])
        entry = {"threshold": threshold, "exit": code, "path": str(preview) if preview.exists() else None}
        if code != 0 or not preview.exists():
            findings.append(f"export-cut --error {threshold} failed (exit {code}): {stderr.strip()[:300]}")
            asset_report["previews"].append(entry)
            continue
        entry["bytes"] = preview.stat().st_size
        entry["sha256"] = sha256_file(preview)
        entry["khronos"] = khronos_issues(validator, preview)
        if entry["khronos"].get("numErrors") != 0:
            findings.append(f"Khronos validator reports {entry['khronos'].get('numErrors', 'an error')} core error(s) on the cut preview at error {threshold}")
        cut = gltf.load_gltf(preview)
        cut_counts[str(threshold)] = triangle_count(cut)
        entry["triangles"] = cut_counts[str(threshold)]
        entry["core_problems"] = gltf.compare_cut(source, cut, threshold)
        findings.extend(entry["core_problems"][:5])
        # attribute arrays and index ranges must be untouched; every cut triangle references source vertices
        source_primitives = list(gltf.primitives(source))
        cut_primitives = list(gltf.primitives(cut))
        if len(source_primitives) != len(cut_primitives):
            findings.append(f"cut preview at error {threshold} changed the primitive count")
        else:
            for (mesh, primitive_index, source_primitive), (_, _, cut_primitive) in zip(source_primitives, cut_primitives):
                for name in source_primitive.get("attributes", {}):
                    if not np.array_equal(gltf.attribute(source, source_primitive, name), gltf.attribute(cut, cut_primitive, name)):
                        findings.append(f"cut preview at error {threshold}: mesh {mesh} primitive {primitive_index} attribute {name} changed")
                        break
                cut_indices = gltf.indices(cut, cut_primitive)
                if cut_indices.size and (cut_indices.min() < 0 or cut_indices.max() >= int(gltf.attribute(cut, cut_primitive, "POSITION").shape[0])):
                    findings.append(f"cut preview at error {threshold}: mesh {mesh} primitive {primitive_index} index out of range")
                if threshold == 0:
                    if not np.array_equal(
                        gltf.canonical_triangles(gltf.indices(source, source_primitive)),
                        gltf.canonical_triangles(cut_indices)):
                        findings.append(f"LOD0 cut preview differs from the source triangle multiset (mesh {mesh} primitive {primitive_index})")
        # the cut preview intentionally has no DEREN payload: deren validate must say so, not pretend success
        code, stdout, stderr = run_command([str(converter), "validate", str(preview), "--full"])
        entry["deren_validate_full"] = {"exit": code, "stdout": stdout.strip()[:500], "no_payload_by_design": code == 3 and "no Subtilis payload" in (stdout + stderr)}
        asset_report["previews"].append(entry)
    ordered = [cut_counts[key] for key in sorted(cut_counts, key=float)]
    if ordered and any(later > earlier for earlier, later in zip(ordered, ordered[1:])):
        findings.append("cut triangle counts are not monotone non-increasing with a larger model-space error")
    asset_report["source_triangles"] = source_triangles
    asset_report["normal_triangles"] = normal_triangles
    asset_report["cut_triangle_counts"] = cut_counts
    return asset_report


def command_check(args):
    measurements = json.loads(Path(args.measurements).read_text(encoding="utf-8"))
    findings = []
    manifest = json.loads(Path(args.validator_manifest).read_text(encoding="utf-8"))
    validator = Path(args.validator).resolve()
    if not validator.is_file():
        findings.append(f"validator binary not found: {validator}")
    else:
        actual = sha256_file(validator)
        expected = (manifest.get("executable") or {}).get("sha256")
        expected_version = manifest.get("version", OFFICIAL_VALIDATOR_VERSION)
        if expected_version != OFFICIAL_VALIDATOR_VERSION:
            findings.append(f"validator manifest pins version {expected_version}, the acceptance requires {OFFICIAL_VALIDATOR_VERSION}")
        if actual != expected:
            findings.append(f"validator SHA-256 mismatch: expected {expected} got {actual}")
        code, stdout, stderr = run_command([str(validator), "--help"])
        if expected_version not in (stdout + stderr):
            findings.append(f"validator does not report version {expected_version}")
    converter = Path(args.converter).resolve()
    work_dir = Path(args.work_dir).resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    thresholds = [float(item) for item in args.cut_thresholds.split(",")]
    acceptance = {"schema": "subtilis-converter-acceptance/1", "validator_manifest": manifest, "assets": [], "findings": findings}
    for asset in measurements.get("assets", []):
        if args.asset and asset.get("name") != args.asset:
            continue
        runs = [run for run in asset.get("runs", []) if run.get("exit") == 0 and run.get("report_path")]
        if not runs:
            findings.append(f"{asset.get('name')}: no successful run to validate")
            continue
        source_path = Path(asset["path"])
        output_path = Path(next(run["output_path"] for run in asset["runs"] if run.get("output_path")))
        print(f"checking {asset['name']}: {output_path}")
        report = check_asset(source_path, output_path, converter, validator, work_dir, thresholds)
        report["name"] = asset["name"]
        acceptance["assets"].append(report)
        findings.extend(f"{asset['name']}: {finding}" for finding in report["findings"])
    acceptance["findings"] = findings
    acceptance_path = Path(args.out)
    acceptance_path.write_text(json.dumps(acceptance, indent=1))
    print(f"acceptance: {acceptance_path}")
    if findings:
        for finding in findings:
            print(f"FAIL {finding}")
        print(f"ACCEPTANCE FAIL: {len(findings)} finding(s)")
        return 1
    print("ACCEPTANCE PASS")
    return 0


def command_gate(args):
    if args.self_test:
        failures = self_test()
        return 0 if not failures else 1
    measurements = json.loads(Path(args.measurements).read_text(encoding="utf-8"))
    findings = check_measurements(measurements)
    output = {
        "schema": "subtilis-converter-gate/1",
        "measurements": str(Path(args.measurements).resolve()),
        "accepted": not findings,
        "findings": findings,
    }
    if args.out:
        Path(args.out).write_text(json.dumps(output, indent=1))
    for finding in findings:
        print(f"FAIL {finding}")
    print(f"GATE {'PASS' if not findings else 'FAIL'}: {len(findings)} finding(s)")
    return 0 if not findings else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)
    gate = subparsers.add_parser("gate", help="structural completeness/identity gate over measurements.json")
    gate.add_argument("--measurements", type=Path, required=False)
    gate.add_argument("--out", type=Path)
    gate.add_argument("--self-test", action="store_true", help="run the negative controls instead of a real file")
    gate.set_defaults(func=command_gate)
    check = subparsers.add_parser("check", help="independent validators + LOD0/cut geometry checks")
    check.add_argument("--measurements", type=Path, required=True)
    check.add_argument("--converter", type=Path, required=True)
    check.add_argument("--validator", type=Path, required=True)
    check.add_argument("--validator-manifest", type=Path, required=True)
    check.add_argument("--work-dir", type=Path, required=True)
    check.add_argument("--out", type=Path, required=True)
    check.add_argument("--cut-thresholds", default="0,0.01,0.1,1.0")
    check.add_argument("--asset", help="only check this asset (default: all)")
    check.set_defaults(func=command_check)
    args = parser.parse_args()
    if args.command == "gate" and not args.self_test and not args.measurements:
        parser.error("gate needs --measurements (or --self-test)")
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
