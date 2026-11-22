import builtins
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import gen_dpdb


class E380DatabaseTest(unittest.TestCase):
    def test_missing_sources_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(FileNotFoundError, "fetch_open3e"):
                gen_dpdb.load_e3oncan_e380(Path(directory) / "missing")

    def test_missing_udsoncan_fails(self):
        real_import = builtins.__import__

        def import_without_udsoncan(name, *args, **kwargs):
            if name == "E3onCANdatapointsE380":
                raise ModuleNotFoundError("No module named 'udsoncan'", name="udsoncan")
            return real_import(name, *args, **kwargs)

        with tempfile.TemporaryDirectory() as directory:
            with patch("builtins.__import__", side_effect=import_without_udsoncan):
                with self.assertRaisesRegex(RuntimeError, "udsoncan"):
                    gen_dpdb.load_e3oncan_e380(Path(directory))

    def test_empty_table_fails(self):
        module = types.ModuleType("E3onCANdatapointsE380")
        module.dataIdentifiersE380 = {}
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(sys.modules, {"E3onCANdatapointsE380": module}):
                with self.assertRaisesRegex(ValueError, "table is empty"):
                    gen_dpdb.load_e3oncan_e380(Path(directory))


if __name__ == "__main__":
    unittest.main()
