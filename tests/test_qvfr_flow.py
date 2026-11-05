import importlib.util
import json
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("qvfr_flow", pathlib.Path(__file__).parents[1] / "tools/qvfr_flow.py")
flow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flow)


def image(cookie, storage=1):
    return dict(cookie=cookie, storage=storage, width=5184, height=2592, format=97, samples=1)


def draw(seq, dest, source, frame=120):
    return dict(frame=frame, seq=seq, kind="draw", output=dest, inputs=[dict(stage=4, backend_slot=0, view=source)], shaders=["fs.test"], **{"pass": 7})


class FlowTests(unittest.TestCase):
    def test_changed_inputs_and_pass_not_double_counted(self):
        events = [draw(1, image(2), image(1)), draw(2, image(3), image(2))]
        lines = flow.summarize(events, {(120, 7): .75}, 120)
        self.assertEqual(sum(line.startswith("pass ") for line in lines), 1)
        self.assertIn("producer=1", "\n".join(lines))

    def test_request_does_not_claim_execution(self):
        request = dict(frame=120, seq=1, kind="transfer-request", operation="resolve", source=image(1), output=image(2))
        lines = flow.summarize([request, draw(2, image(3), image(2))], {}, 120)
        self.assertIn("producer=unobserved", "\n".join(lines))

    def test_storage_and_frame_isolation(self):
        events = [draw(1, image(2), image(1)), draw(2, image(3), image(2, 9)), draw(1, image(4), image(2), frame=240)]
        self.assertIn("producer=unobserved", "\n".join(flow.summarize(events, {}, 240)))
        lines = flow.summarize(events, {}, 120)
        renamed = next(line for line in lines if "2@9 " in line)
        self.assertIn("producer=unobserved", renamed)

    def test_parse_log_and_join_pass(self):
        event = draw(1, image(2), image(1))
        lines = ["info: QVFR flow " + json.dumps(event),
                 'info: QVFR pass-detail {"frame":120,"pass":7,"gpu_ms":0.75}',
                 "info: QVFR stats frame=120 flow_events=1"]
        events, passes = flow.records(lines)
        self.assertEqual(events, [event])
        self.assertIn("0.75 ms", flow.summarize(events, passes, 120)[0])

    def test_cancelled_forward_not_reported_as_producer(self):
        events = [draw(1, image(2), image(1)),
                  dict(frame=120, seq=2, kind="hdr-forward-defer", output=image(2)),
                  dict(frame=120, seq=3, kind="hdr-forward-cancel", output=image(2)),
                  draw(4, image(3), image(2))]
        lines = flow.summarize(events, {}, 120)
        self.assertIn("producer=unobserved", lines[-1])


if __name__ == "__main__":
    unittest.main()
