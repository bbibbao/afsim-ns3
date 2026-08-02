from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from afsim_ns3_bridge.effects import evaluate_effects
from afsim_ns3_bridge.metrics_store import MetricsStore
from afsim_ns3_bridge.models import ProtocolError, validate_envelope
from afsim_ns3_bridge.state import StateStore

from common import sample


class ModelsStateEffectsTest(unittest.TestCase):
    def test_full_init_then_partial_delta(self) -> None:
        state = StateStore()
        initial = sample("sample_init.json")
        message_type, request_id, timestamp_ms = validate_envelope(initial)
        self.assertEqual(message_type, "afsim_init")
        self.assertEqual(request_id, "init-001")
        state.initialize(initial, timestamp_ms)

        delta = sample("sample_delta.json")
        state.apply_delta(delta, delta["timestamp_ms"])
        snapshot = state.snapshot()

        self.assertEqual(snapshot.revision, 2)
        self.assertEqual(snapshot.timestamp_ms, 1000)
        self.assertEqual(snapshot.entities[0].position.x_m, 100.0)
        self.assertEqual(snapshot.entities[0].position.y_m, 0.0)
        self.assertEqual(snapshot.entities[0].devices[0].delay_ms, 120.0)

    def test_delta_before_init_and_time_regression_are_rejected(self) -> None:
        state = StateStore()
        with self.assertRaises(ProtocolError):
            state.apply_delta(sample("sample_delta.json"), 1000)

        initial = sample("sample_init.json")
        state.initialize(initial, 1000)
        delta = sample("sample_delta.json")
        with self.assertRaises(ProtocolError):
            state.apply_delta(delta, 999)

    def test_metrics_drive_weapon_and_radar_states(self) -> None:
        state = StateStore()
        initial = sample("sample_init.json")
        state.initialize(initial, 0)
        snapshot = state.snapshot()
        metric = {
            "source_entity_id": "AFSIM-1",
            "target_entity_id": "AFSIM-2",
            "connected": True,
            "latency_ms": 120.0,
            "loss_rate": 0.0,
            "throughput_bps": 900000.0,
        }

        effects = evaluate_effects(snapshot, [metric])
        by_id = {item["subsystem_id"]: item for item in effects}
        self.assertEqual(by_id["WEAPON-1"]["state"], "BLOCKED")
        self.assertEqual(by_id["RADAR-1"]["state"], "DEGRADED")
        self.assertIn("DELAY_EXCEEDED", by_id["WEAPON-1"]["reasons"])

        metric["connected"] = False
        effects = evaluate_effects(snapshot, [metric])
        self.assertTrue(all(item["state"] == "BLOCKED" for item in effects))

    def test_csv_keeps_response_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = MetricsStore(Path(directory) / "metrics.csv")
            store.record(
                {
                    "request_id": "req-7",
                    "timestamp_ms": 700,
                    "revision": 7,
                    "metrics": [
                        {
                            "flow_id": "flow",
                            "source_entity_id": "a",
                            "target_entity_id": "b",
                            "connected": True,
                        }
                    ],
                }
            )
            csv_text = store.history_csv().decode("utf-8-sig")
            self.assertIn("req-7,700,7,flow,a,b,True", csv_text)


if __name__ == "__main__":
    unittest.main()
