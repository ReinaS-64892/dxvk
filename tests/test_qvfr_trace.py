import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('qvfr_trace', Path(__file__).parents[1] / 'tools/qvfr_trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def record(**changes):
    value = dict(version=1, width=4000, height=2000, layers=1,
                 colors=[dict(image=12, mip=0, layer=0, layers=1, format=37, samples=1)],
                 depth=None, count=1, instances=2, storage=False, xfb=False,
                 layer_export=False, context=10, **{'pass': 1},
                 shaders=['VS_hash', None, None, None, 'FS_hash'], viewports=[])
    value.update(changes)
    return 'info:  QVFR ' + json.dumps(value) + '\n'


class TraceTest(unittest.TestCase):
    def test_merged_and_indirect(self):
        result = trace.summarize(['info:  QVFR trace enabled\n', record(),
                                  record(count=8, instances=None, storage=True, **{'pass': 2})])
        self.assertEqual(result['records'], 2)
        target, = result['targets']
        self.assertEqual(target['records'], 2)
        self.assertEqual(target['passes'], 2)
        self.assertEqual(target['draw_count_or_upper_bound'], 9)
        self.assertEqual(target['indirect_records'], 1)
        self.assertEqual(target['storage_records'], 1)
        self.assertEqual(target['instance_counts'], {2: 1})
        self.assertEqual(len(target['shaders']), 1)

    def test_subresources_and_contexts(self):
        result = trace.summarize([record(), record(context=11),
                                  record(colors=[dict(image=12, mip=1, layers=1)])])
        self.assertEqual(len(result['targets']), 2)
        self.assertEqual(result['targets'][0]['passes'], 2)

    def test_empty_and_other_logs(self):
        self.assertEqual(trace.summarize(['info: device ready\n']), dict(records=0, targets=[]))

    def test_bad_record_reports_line(self):
        for bad in ['info: QVFR {truncated', record(version=2), 'QVFR {}']:
            with self.assertRaisesRegex(ValueError, 'line 2: malformed'):
                trace.summarize(['ignored', bad])


if __name__ == '__main__':
    unittest.main()
