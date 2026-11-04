import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class AttachmentJsonTest(unittest.TestCase):
    def test_real_serializer_has_numeric_fields_and_no_control_bytes(self):
        source = Path(__file__).with_name('qvfr_json.cpp')
        with tempfile.TemporaryDirectory(prefix='qvfr-json-') as directory:
            binary = Path(directory) / 'qvfr-json'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(binary)], check=True)
            output = subprocess.check_output([binary])
        self.assertNotIn(b'\0', output)
        self.assertTrue(all(byte >= 32 or byte == 10 for byte in output))
        objects = [json.loads(line) for line in output.splitlines()]
        self.assertEqual([a['mip'] for a in objects], [0, 1, 255])
        self.assertEqual(len(objects), 3)
        for a in objects:
            self.assertEqual(a['format'], 43)
            self.assertEqual(a['samples'], 4)
            self.assertEqual(a['width'], 5184)
            self.assertTrue(all(type(value) is int for value in a.values()))


if __name__ == '__main__':
    unittest.main()
