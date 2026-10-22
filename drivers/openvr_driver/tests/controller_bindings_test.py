"""Check the binding resource contract, not SteamVR's closed-source rebinder.

Optionally check exported application bindings with --bindings-zip PATH.
Game names, app IDs and action names are deliberately absent from the driver.
"""
import argparse
import json
from pathlib import Path
import unittest
import zipfile

RESOURCE_DIR = Path(__file__).resolve().parents[1] / "resources" / "input"
PROFILE = json.loads((RESOURCE_DIR / "xr_hand_controller_profile.json").read_text())
REMAPPING = json.loads((RESOURCE_DIR / "xr_hand_controller_remapping.json").read_text())
LAYOUTS = {x["from_controller_type"]: x for x in REMAPPING["layouts"]}
EXPORTED_BINDINGS = []


def mirror(path):
    if "/left/" in path:
        return path.replace("/left/", "/right/")
    return path.replace("/right/", "/left/")


def rules(layout, key):
    for rule in layout.get(key, []):
        yield rule
        if rule.get("mirror", True):
            # Only paths need swapping in these resources.
            copy = json.loads(json.dumps(rule))
            for end in ("from", "to"):
                if isinstance(copy[end], str):
                    copy[end] = mirror(copy[end])
                else:
                    copy[end]["path"] = mirror(copy[end]["path"])
            yield copy


def destination(layout, path, mode=None):
    """Resolve declared paths/mode overrides; do not simulate auto templates."""
    explicit = sorted(rules(layout, "remappings"),
                      key=lambda r: "mode" in r["from"], reverse=True)
    for rule in explicit:
        source = rule["from"]
        if source["path"] == path and ("mode" not in source or source["mode"] == mode):
            return rule["to"]["path"], rule["to"].get("mode", mode)
    for rule in rules(layout, "autoremappings"):
        if rule["from"] == path:
            return rule["to"], mode
    return path, mode


def source(path):
    for hand in ("left", "right"):
        prefix = f"/user/hand/{hand}"
        if path.startswith(prefix + "/"):
            return PROFILE["input_source"][path[len(prefix):]]
    raise AssertionError(f"not a hand component: {path}")


class BindingContract(unittest.TestCase):
    def test_layout_selection_and_identity(self):
        self.assertEqual(REMAPPING["to_controller_type"], PROFILE["controller_type"])
        self.assertEqual(PROFILE["remapping"],
                         "{xr_tracking}/input/xr_hand_controller_remapping.json")
        self.assertNotIn("compatibility_mode_controller_type", PROFILE)
        available = set(LAYOUTS)
        for expected in ("oculus_touch", "knuckles", "vive_controller", "khr_simple_controller"):
            selected = max((LAYOUTS[x] for x in available), key=lambda x: x["priority"])
            self.assertEqual(selected["from_controller_type"], expected)
            available.remove(expected)
        for layout in LAYOUTS.values():
            self.assertTrue(layout["simulate_controller_type"])
            self.assertFalse(layout["simulate_HMD"])

    def test_all_declared_destinations_exist(self):
        for layout in LAYOUTS.values():
            for key in ("autoremappings", "remappings"):
                for rule in rules(layout, key):
                    target = rule["to"]
                    source(target if isinstance(target, str) else target["path"])

    def test_touch_and_index_face_buttons(self):
        for hand, names in (("left", ("x", "y")), ("right", ("a", "b"))):
            for name in names:
                path = f"/user/hand/{hand}/input/{name}"
                self.assertEqual(destination(LAYOUTS["oculus_touch"], path)[0], path)
                self.assertTrue(source(path)["click"])
                self.assertTrue(source(path)["touch"])
        for original, target in (("a", "x"), ("b", "y")):
            self.assertEqual(destination(LAYOUTS["knuckles"], f"/user/hand/left/input/{original}")[0],
                             f"/user/hand/left/input/{target}")
            self.assertEqual(destination(LAYOUTS["knuckles"], f"/user/hand/right/input/{original}")[0],
                             f"/user/hand/right/input/{original}")

    def test_direction_and_centre_are_distinct(self):
        layout = LAYOUTS["vive_controller"]
        for hand in ("left", "right"):
            path = f"/user/hand/{hand}/input/trackpad"
            target, mode = destination(layout, path, "dpad_click")
            self.assertEqual(mode, "dpad_touch")
            self.assertTrue(source(target)["touch"])
            self.assertEqual(destination(layout, path, "button"), (path, "button"))
            self.assertTrue(source(path)["click"])
        rule = next(r for r in layout["remappings"] if r["from"].get("mode") == "dpad_click")
        self.assertEqual(rule["to"]["parameters_mode"], "copy")

    def test_generic_openxr_and_steamvr_components(self):
        for name, layout in LAYOUTS.items():
            for hand in ("left", "right"):
                prefix = f"/user/hand/{hand}"
                for alias in ("raw", "grip", "tip", "openxr_grip", "openxr_aim", "openxr_handmodel"):
                    self.assertEqual(destination(layout, prefix + "/pose/" + alias)[0], prefix + "/pose/raw")
                self.assertEqual(source(prefix + "/output/haptic")["type"], "haptic")
                trigger = "select" if name == "khr_simple_controller" else "trigger"
                self.assertTrue(source(destination(layout, prefix + "/input/" + trigger)[0])["value"])

    def test_exported_application_components(self):
        if not EXPORTED_BINDINGS:
            self.skipTest("no optional application binding exports supplied")
        checked = 0
        for binding in EXPORTED_BINDINGS:
            layout = LAYOUTS[binding["controller_type"]]
            for actionset in binding["bindings"].values():
                for entry in actionset.get("sources", []):
                    path, mode = destination(layout, entry["path"], entry["mode"])
                    component = source(path)
                    inputs = entry.get("inputs", {})
                    for channel in inputs:
                        if channel in ("click", "touch"):
                            self.assertTrue(component.get(channel), (path, channel))
                        elif channel in ("position_x", "position_y"):
                            self.assertIn(component["type"], ("joystick", "trackpad"))
                        elif channel == "pull":
                            self.assertTrue(component.get("value"), path)
                        elif channel == "force":
                            # Valve's force-sensor -> trigger auto template uses value.
                            self.assertTrue(component.get("force") or component.get("value"), path)
                    if mode in ("dpad_click", "dpad_touch"):
                        self.assertTrue(component.get("touch" if mode == "dpad_touch" else "click"), path)
                    checked += len(inputs)
                for kind in ("poses", "haptics"):
                    for entry in actionset.get(kind, []):
                        path, _ = destination(layout, entry["path"])
                        self.assertEqual(source(path)["type"], "pose" if kind == "poses" else "haptic")
                        checked += 1
        self.assertGreater(checked, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bindings-zip", type=Path)
    args, remaining = parser.parse_known_args()
    if args.bindings_zip:
        with zipfile.ZipFile(args.bindings_zip) as archive:
            EXPORTED_BINDINGS.extend(json.loads(archive.read(name)) for name in archive.namelist()
                                     if name.endswith(".json"))
        print(f"Checking {len(EXPORTED_BINDINGS)} exported binding layouts")
    unittest.main(argv=[__file__, *remaining])
