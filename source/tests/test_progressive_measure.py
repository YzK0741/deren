"""Measurement failures must not be reported as successful memory measurements."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2]/'scripts/measure/measure_progressive_textures.py'
sys.path.insert(0, str(SCRIPT.parent))
from measure_render_optimizations import parse_run_log
spec = importlib.util.spec_from_file_location('progressive_measure', SCRIPT)
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class MemoryEvidenceTests(unittest.TestCase):
    def test_process_memory_requires_frame_file(self):
        with self.assertRaises(ValueError):
            measure.load_process_memory_summary(Path('missing-memory.jsonl'), Path('missing-frames.csv'))

    def test_sampler_initialization_failure_cleans_up_child(self):
        class Child:
            args = ['fake-renderer']
            killed = False
            def poll(self): return 1 if self.killed else None
            def kill(self): self.killed = True
            def wait(self, timeout=None): return 1
        child = Child()
        with mock.patch.object(measure, 'make_process_memory_reader', side_effect=OSError('initialization failed'), create=True) as factory:
            with self.assertRaises(OSError):
                measure.wait_with_process_memory(child)
            factory.assert_called_once_with(child)
        self.assertTrue(child.killed)

    def test_process_memory_uses_rendering_window_and_rejects_missing_samples(self):
        frames = [{'unix_ms': str(i)} for i in range(361)]
        samples = [dict(unix_ms=t, working_bytes=w, private_bytes=w*2, peak_working_bytes=2000)
                   for t, w in [(100, 1000), (180, 100), (190, 200), (200, 300), (400, 1900)]]
        result = measure.summarize_process_memory(samples, frames)
        self.assertEqual(result['working_steady_median_bytes'], 200)
        self.assertEqual(result['private_steady_median_bytes'], 400)
        self.assertEqual(result['steady_samples'], 3)
        self.assertEqual(result['working_peak_bytes'], 2000)
        for invalid in [[], samples[:2], [dict(row, working_bytes=0) for row in samples]]:
            with self.assertRaises(ValueError):
                measure.summarize_process_memory(invalid, frames)

    def test_composed_feature_activity_requires_actual_execution(self):
        self.assertTrue(measure.feature_activity_errors({}, subtilis=True, hzb=True, software=True, gpu_software=True))
        active = dict(cluster_dag_active=True, adopted=464, hzb_cull_active=True, hzb_recovery_active=True,
                      software_raster_active=True, software_triangle_active=True, software_gpu_routing_active=True)
        self.assertEqual(measure.feature_activity_errors(active, subtilis=True, hzb=True, software=True, gpu_software=True), [])
        for field in active:
            missing = dict(active)
            missing[field] = 0
            with self.subTest(field=field):
                self.assertTrue(measure.feature_activity_errors(missing, subtilis=True, hzb=True, software=True, gpu_software=True))

    def test_cluster_dag_activity_requires_published_cut(self):
        imported = 'subtilis import: 1873 adopted (1540 shared), 531 LOD0 fallback\n'
        published = imported + 'subtilis: cut run publication: 43930 -> 36356 records\n'
        self.assertTrue(parse_run_log(published)['cluster_dag_active'])
        # A configured/imported asset without a published cut is not activation evidence.
        self.assertFalse(parse_run_log(imported)['cluster_dag_active'])

    def test_resource_evidence_rejects_missing_or_unsupported_samples(self):
        with self.assertRaises(ValueError):
            measure.summarize_resources([])
        with self.assertRaises(ValueError):
            measure.summarize_resources([{'frame': '180', 'supported': '0'}] * 3)

    def test_resource_evidence_summarizes_steady_and_lifetime_peak(self):
        rows = [dict(frame=str(frame), supported='1', gpu_buffer_local='10', gpu_image_local=str(value),
                     gpu_total_local=str(10 + value), gpu_peak_local='1000', gpu_total='1000',
                     gpu_reserved='2000', gpu_reserved_local='1500', cpu_current=str(value * 2), cpu_peak='1900')
                for frame, value in [(0, 900), (180, 30), (190, 40), (200, 50)]]
        result = measure.summarize_resources(rows)
        self.assertEqual(result['gpu_total_local_steady_median_bytes'], 50)
        self.assertEqual(result['gpu_peak_local_bytes'], 1000)
        self.assertEqual(result['cpu_current_steady_median_bytes'], 80)
        with self.assertRaises(ValueError):
            measure.summarize_resources(rows[:2])
        for field, value in [('frame', '190'), ('gpu_total_local', '999'), ('cpu_current', '-1'),
                             ('cpu_peak', '1'), ('gpu_peak_local', '1'), ('gpu_peak_local', '500'),
                             ('gpu_total', '1')]:
            invalid = [dict(row) for row in rows]
            invalid[-1][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                measure.summarize_resources(invalid)

    def test_failed_sampler_rejected(self):
        self.assertIsNotNone(measure.memory_evidence_error({'memory_sampler_exit': 1, 'memory_steady_samples': 0}))

    def test_missing_or_insufficient_counters_rejected(self):
        self.assertIsNotNone(measure.memory_evidence_error({'memory_sampler_exit': 0, 'memory_steady_samples': 2}))
        self.assertIsNotNone(measure.memory_evidence_error({'memory_sampler_exit': 0, 'memory_steady_samples': 5}))

    def test_valid_counter_evidence_accepted(self):
        self.assertIsNone(measure.memory_evidence_error({'memory_sampler_exit': 0, 'memory_steady_samples': 5,
                                                       'dedicated_steady_median_bytes': 1024}))

    def test_short_memory_capture_refused_before_launch(self):
        workspace_tmp = SCRIPT.parents[2]/'tmp'
        workspace_tmp.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workspace_tmp) as directory:
            result = subprocess.run([sys.executable, str(SCRIPT), '--exe', 'missing.exe', '--out', str(Path(directory)/'capture'),
                                     '--memory', '--frames', '64'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('memory captures need', result.stderr)


if __name__ == '__main__':
    unittest.main()
