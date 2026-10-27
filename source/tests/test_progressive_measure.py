"""Measurement failures must not be reported as successful memory measurements."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2]/'scripts/measure/measure_progressive_textures.py'
spec = importlib.util.spec_from_file_location('progressive_measure', SCRIPT)
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class MemoryEvidenceTests(unittest.TestCase):
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
