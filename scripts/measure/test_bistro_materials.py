import unittest
import numpy as np
from convert_bistro_metalrough import convert

class ConversionTests(unittest.TestCase):
    def test_dielectric(self):
        base, metal, rough = convert(np.array([.2,.4,.6]), np.array([.04,.04,.04]), .75)
        np.testing.assert_allclose(base, [.2,.4,.6], atol=1e-6)
        self.assertAlmostEqual(float(metal), 0)
        self.assertAlmostEqual(float(rough), .25)
    def test_metal(self):
        base, metal, _ = convert(np.zeros(3), np.array([.8,.6,.2]), .5)
        np.testing.assert_allclose(base, [.8,.6,.2], atol=1e-6)
        self.assertAlmostEqual(float(metal), 1)
    def test_black_finite(self):
        base, metal, rough = convert(np.zeros(3), np.zeros(3), 0)
        self.assertTrue(np.isfinite(base).all())
        self.assertEqual(float(metal), 0)
        self.assertEqual(float(rough), 1)

if __name__ == '__main__': unittest.main()
