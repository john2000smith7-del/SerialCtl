"""Controlled observer-cost regressions; these are not actual Windows/UI tests."""
import contextlib
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "desktop_observation_actions", Path(__file__).with_name("desktop_user_actions.py"))
desktop = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(desktop)


class Clock:
    def __init__(self):
        self.now = 0.0

    def perf_counter(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class ControlledActions(desktop.DesktopActions):
    """Real sidebar input loop, controlled native observations and 250ms capture."""
    def __init__(self, clock, *, lose_click=False, move_anchor=False):
        self.clock, self.lose_click, self.move_anchor = clock, lose_click, move_anchor
        self.scale, self.expanded, self.anchor_moved = 1, True, False
        self.args = SimpleNamespace(mode="desktop")
        self.operations, self.frames = [], []
        self.user = SimpleNamespace(GetSystemMetrics=lambda _: 4, GetDoubleClickTime=lambda: 500,
                                    WindowFromPoint=lambda _: 125)

    @contextlib.contextmanager
    def operation(self, name):
        entry = {"name": name}
        self.operations.append(entry)
        yield entry

    def control(self, identifier):
        return identifier

    def rect(self, _):
        return (101, 10, 141, 30) if self.anchor_moved else (100, 10, 140, 30)

    def click_control(self, _):
        self.expanded = not self.expanded

    def caption(self, _):
        return "collapse" if self.expanded else "expand"

    def move_mouse(self, _):
        pass

    def button(self, down):
        if not down:
            if not self.lose_click:
                self.expanded = not self.expanded
            self.anchor_moved = self.move_anchor

    def capture(self, _):
        self.clock.sleep(.250)
        self.frames.append({"sidebar_toggle_bounds": self.rect(125)})

    def check_layout(self, **_):
        return {"controlled_geometry": "PASS"}

    def visible(self, _):
        return self.expanded

    def timestamp(self):
        return {"seconds": self.clock.now}


class DesktopObservationTests(unittest.TestCase):
    def exercise(self, *, synchronous, **faults):
        clock = Clock()
        actions = ControlledActions(clock, **faults)
        with mock.patch.object(desktop, "time", clock):
            actions.sidebar_toggle(observation_budget=1., capture_rapid_frames=synchronous)
        return actions

    def test_slow_capture_reproduces_invalid_input_interval_and_independent_mode_preserves_it(self):
        with self.assertRaisesRegex(AssertionError, "Input/capture was too slow"):
            self.exercise(synchronous=True)
        actions = self.exercise(synchronous=False)
        for operation in actions.operations[-2:]:
            self.assertEqual(len(operation["single_click_observations"]), 10)
            self.assertTrue(all(value < 200 for value in operation["click_intervals_ms"]))
            self.assertTrue(all(row["caption_flipped"] for row in operation["single_click_observations"]))

    def test_independent_capture_does_not_hide_lost_click(self):
        with self.assertRaisesRegex(desktop.SidebarBehaviorFailure, "did not change its target state"):
            self.exercise(synchronous=False, lose_click=True)

    def test_independent_capture_does_not_hide_moving_anchor(self):
        with self.assertRaisesRegex(desktop.SidebarBehaviorFailure, "Sidebar toggle moved"):
            self.exercise(synchronous=False, move_anchor=True)


if __name__ == "__main__":
    unittest.main()
