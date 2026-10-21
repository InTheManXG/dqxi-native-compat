import unittest
from probe_geometry import layered_vertex


class GeometryTest(unittest.TestCase):
    def test_preserve_float_bits_and_other_outputs(self):
        src = "struct main0_out\n{\n float4 layer_value [[user(locn1)]];\n    float gl_PointSize [[point_size]];\n};\nvertex main0_out main0(){ main0_out out = {};\n    out.gl_PointSize = 1.0;\n return out; }"
        got = layered_vertex(src)
        self.assertIn("uint dm_layer [[render_target_array_index]];", got)
        self.assertIn("as_type<uint>(out.layer_value.x)", got)
        self.assertIn("float4 layer_value [[user(locn1)]];", got)
        self.assertNotIn("gl_PointSize", got)
        with self.assertRaises(ValueError):
            layered_vertex(src.replace("locn1", "locn0"))
        with self.assertRaises(ValueError):
            layered_vertex(src + " return out;")


if __name__ == "__main__":
    unittest.main()
