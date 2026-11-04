import importlib.util
from pathlib import Path
import tempfile
import unittest
import subprocess


spec = importlib.util.spec_from_file_location("qvfr_control", Path(__file__).resolve().parents[2] / "qvfr-control.py")
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class ControlTest(unittest.TestCase):
    def test_layout_has_independent_density_and_screen_coverage(self):
        settings = control.parse_settings("scale=.25\nfocus_scale=.75\nfocus=.125\nprofile_passes=full")
        self.assertEqual(settings["profile_passes"], "full")
        description = control.layout_description(settings)
        self.assertIn("peripheral=648x648", description)
        self.assertIn("focus_texture=243x243", description)
        self.assertIn("focus_screen=324x324", description)
        settings["focus_scale"] = .5
        description = control.layout_description(settings)
        self.assertIn("focus_texture=162x162", description)
        self.assertIn("focus_screen=324x324", description)

    def test_unsafe_options_are_independent_and_survive_toggle(self):
        keys = ("unsafe_skip_image_seed", "unsafe_skip_stencil_seed", "unsafe_skip_stencil_compose",
                "unsafe_skip_depth_compose")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            path.write_text("enabled=on\nprofile=on\n")
            for key in keys:
                self.assertFalse(control.read_settings(path)[key])
            for mask in range(1 << len(keys)):
                control.update_settings(path, {key: bool(mask & (1 << i)) for i, key in enumerate(keys)})
                control.write_state(path, False)
                control.write_state(path, True)
                result = control.read_settings(path)
                self.assertTrue(result["profile"])
                for i, key in enumerate(keys):
                    self.assertEqual(result[key], bool(mask & (1 << i)))

    def test_absent_file_defaults_on_and_writes_replace_atomically(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            self.assertTrue(control.read_state(path))
            for state in (False, True, False):
                control.write_state(path, state)
                self.assertEqual(control.read_state(path), state)
                self.assertEqual(path.read_bytes(), b"on\n" if state else b"off\n")
                self.assertEqual(list(Path(temporary).iterdir()), [path])

    def test_invalid_and_partial_requests_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            for value in ("", "ON", "on off", "of", "2"):
                path.write_text(value)
                with self.assertRaises(ValueError):
                    control.read_state(path)

    def test_toggle_and_setting_preserve_other_settings_and_comments(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            path.write_text("# keep this\nenabled=on\nprofile=off # measurement\ngaze=shared\nsamples=2\n")
            control.write_state(path, False)
            control.update_settings(path, {"profile": "on"})
            result = control.read_settings(path)
            self.assertFalse(result["enabled"])
            self.assertTrue(result["profile"])
            self.assertEqual(result["gaze"], "shared")
            self.assertIn("# keep this", path.read_text())
            self.assertIn("# measurement", path.read_text())
            control.write_state(path, True)
            self.assertTrue(control.read_settings(path)["profile"])
            self.assertEqual(list(Path(temporary).iterdir()), [path])

    def test_invalid_update_does_not_replace_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            path.write_text("on\n")
            for key, value in (("enabled", "broken"), ("profile", "yes"), ("samples", "3"),
                               ("scale", "nan"), ("gaze", "wrong"), ("unknown", "on"),
                               ("profile", "off\nenabled=off")):
                with self.assertRaises(ValueError):
                    control.update_settings(path, {key: value})
                self.assertEqual(path.read_text(), "on\n")

    def test_legacy_file_can_be_upgraded(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            path.write_text("off\n")
            control.update_settings(path, {"profile": "on", "focus": ".3"})
            result = control.read_settings(path)
            self.assertFalse(result["enabled"])
            self.assertTrue(result["profile"])
            self.assertEqual(result["focus"], .3)

    def test_cli_set_toggle_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "control.txt"
            path.write_text("enabled=on\nprofile=off\ngaze=shared\n")
            script = Path(control.__file__)
            import sys
            def run(*args):
                return subprocess.run([sys.executable, "-B", str(script), *args, "--file", str(path)],
                                      check=True, text=True, capture_output=True).stdout
            run("set", "profile", "on")
            run("toggle")
            output = run("status")
            self.assertIn("requested=off", output)
            self.assertIn("profile=on", output)
            self.assertIn("gaze=shared", output)

    def test_cpp_and_python_validate_the_same_requests(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "control-parse"
            subprocess.run(["g++", "-std=c++17", "-O2", str(Path(__file__).with_name("qvfr_control.cpp")),
                            "-o", str(binary)], check=True, capture_output=True)
            requests = ["on\n", "off", "profile=on\ngaze=fixed\n", "enabled=false\n",
                        "unsafe_skip_image_seed=on\nunsafe_skip_stencil_seed=off\nunsafe_skip_stencil_compose=true\n",
                        "unsafe_skip_depth_compose=true\ncolor_single_pass=false\ncolor_fade_pixels=24",
                        "color_fade_pixels=0", "color_fade_pixels=256", "color_fade_pixels=257",
                        "color_fade_pixels=-1", "color_fade_pixels=1.5",
                        "unsafe_skip_image_seed=yes", "unsafe_skip_stencil_seed=on\nunsafe_skip_stencil_seed=off",
                        "# comment\noutline=off # inline\n", "# 日本語コメント\nprofile=off\n", "samples=2\nsize=5184x2592\n",
                        "profile=o", "profile=off\nprofile=on", "samples=3", "size=3x2",
                        "focus_scale=.001", "focus_scale=1", "focus_scale=0", "focus_scale=1.1",
                        "profile_passes=full", "profile_passes=sampled", "profile_passes=all",
                        "scale=nan", "scale=+0.5", "scale=1.1", "focus=0", "focus=.25",
                        "scale=1e-2", "format=1000000001", "profile=off\nwrong=on",
                        "gaze_file=Z:/dev/shm/vrchat_dfr_gaze\n", "gaze_file=", "\0", " "*4097,
                        "", "# comment only\n", "profile=on\nsize=broken\n",
                        control.DEFAULT_PATH.read_text(encoding="ascii")]
            for request in requests:
                with self.subTest(request=request[:80]):
                    try:
                        control.parse_settings(request)
                        python_valid = True
                    except ValueError:
                        python_valid = False
                    result = subprocess.run([str(binary), "--parse"], input=request,
                                            text=True, capture_output=True)
                    self.assertEqual(result.returncode == 0, python_valid)


if __name__ == "__main__":
    unittest.main()
