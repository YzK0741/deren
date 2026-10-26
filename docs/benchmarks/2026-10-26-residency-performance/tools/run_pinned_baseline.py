"""Pin authored scene animation; retain the original interleaved measurement harness."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT/'scripts/measure'))
import measure_texture_residency_detailed as harness

original_make_config = harness.make_config


def make_config(*args):
    original_make_config(*args)
    path = args[0]
    path.write_text(path.read_text().replace('[render]\n', '[render]\nanimation_time = 0.0\n'))


harness.make_config = make_config
harness.main()
