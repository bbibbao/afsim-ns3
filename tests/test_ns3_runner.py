from __future__ import annotations

import unittest

from afsim_ns3_bridge.effects import evaluate_effects
from afsim_ns3_bridge.engine import Ns3Runner
from afsim_ns3_bridge.state import StateStore

from common import PROJECT_ROOT, sample


class Ns3RunnerIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runner_path = PROJECT_ROOT / ".deps/bin/afsim-ns3-runner"
        if not cls.runner_path.is_file():
            raise unittest.SkipTest("project-local ns-3 runner is not built")

    def setUp(self) -> None:
        self.runner = Ns3Runner(self.runner_path)

    def tearDown(self) -> None:
        self.runner.close()

    def test_real_ns3_metrics_follow_link_delay(self) -> None:
        state = StateStore()
        initial = sample("sample_init.json")
        state.initialize(initial, 0)
        first = self.runner.compute(state.snapshot(), "init-001")

        self.assertEqual(len(first["node_mappings"]), 3)
        self.assertEqual(len(first["nodes"]), 3)
        self.assertTrue(all(node["connected"] for node in first["nodes"]))
        self.assertEqual(len(first["metrics"]), 2)
        flow_one = next(
            metric
            for metric in first["metrics"]
            if metric["flow_id"] == "FLOW-1"
        )
        self.assertTrue(flow_one["connected"])
        self.assertGreaterEqual(flow_one["latency_ms"], 10.0)
        self.assertEqual(flow_one["loss_rate"], 0.0)
        self.assertGreater(flow_one["throughput_bps"], 0.0)
        self.assertEqual(flow_one["traffic_direction"], "AFSIM-1->AFSIM-2")
        self.assertEqual(flow_one["link_state"], "UP")

        delta = sample("sample_delta.json")
        state.apply_delta(delta, 1000)
        second = self.runner.compute(state.snapshot(), "delta-001")
        flow_one = next(
            metric
            for metric in second["metrics"]
            if metric["flow_id"] == "FLOW-1"
        )
        self.assertTrue(flow_one["connected"])
        self.assertGreaterEqual(flow_one["latency_ms"], 120.0)

        link_down = sample("sample_delta.json")
        link_down["request_id"] = "link-down-001"
        link_down["timestamp_ms"] = 2000
        link_down["entity_upserts"][0]["devices"][0]["link_state"] = "DOWN"
        state.apply_delta(link_down, 2000)
        third = self.runner.compute(state.snapshot(), "link-down-001")
        flow_one = next(
            metric
            for metric in third["metrics"]
            if metric["flow_id"] == "FLOW-1"
        )
        self.assertFalse(flow_one["connected"])
        self.assertEqual(flow_one["loss_rate"], 1.0)
        self.assertEqual(flow_one["throughput_bps"], 0.0)
        self.assertEqual(flow_one["link_state"], "DOWN")
        link_one = next(
            link
            for link in third["links"]
            if link["link_id"] == "AFSIM-1:LINK-1"
        )
        self.assertEqual(link_one["state"], "DOWN")
        effects = evaluate_effects(state.snapshot(), third["metrics"])
        self.assertTrue(all(item["state"] == "BLOCKED" for item in effects))
        self.assertTrue(
            all("DISCONNECTED" in item["reasons"] for item in effects)
        )

    def test_real_ns3_packet_loss_and_data_rate_affect_metrics(self) -> None:
        state = StateStore()
        initial = sample("sample_init.json")
        device = initial["entities"][0]["devices"][0]
        device["loss_rate"] = 0.5
        device["data_rate_bps"] = 128000
        state.initialize(initial, 0)

        measured = self.runner.compute(state.snapshot(), "loss-rate-001")
        flow_one = next(
            metric
            for metric in measured["metrics"]
            if metric["flow_id"] == "FLOW-1"
        )
        self.assertTrue(flow_one["connected"])
        self.assertGreater(flow_one["loss_rate"], 0.0)
        self.assertLess(flow_one["loss_rate"], 1.0)
        self.assertGreater(flow_one["throughput_bps"], 0.0)
        self.assertLess(flow_one["throughput_bps"], 128000.0)
        effects = evaluate_effects(state.snapshot(), measured["metrics"])
        by_id = {item["subsystem_id"]: item for item in effects}
        self.assertEqual(by_id["WEAPON-1"]["state"], "BLOCKED")
        self.assertEqual(by_id["RADAR-1"]["state"], "DEGRADED")
        self.assertIn("LOSS_EXCEEDED", by_id["WEAPON-1"]["reasons"])

        limited = sample("sample_init.json")
        limited["entities"][0]["devices"][0]["data_rate_bps"] = 64000
        limited_state = StateStore()
        limited_state.initialize(limited, 0)
        low_throughput = self.runner.compute(
            limited_state.snapshot(),
            "low-throughput-001",
        )
        limited_flow = next(
            metric
            for metric in low_throughput["metrics"]
            if metric["flow_id"] == "FLOW-1"
        )
        self.assertTrue(limited_flow["connected"])
        self.assertLess(limited_flow["throughput_bps"], 100000.0)
        effects = evaluate_effects(
            limited_state.snapshot(),
            low_throughput["metrics"],
        )
        self.assertTrue(
            all(
                "THROUGHPUT_BELOW_MINIMUM" in item["reasons"]
                for item in effects
            )
        )

        total_loss = sample("sample_init.json")
        total_loss["entities"][0]["devices"][0]["loss_rate"] = 1.0
        total_loss_state = StateStore()
        total_loss_state.initialize(total_loss, 0)
        total_loss_metrics = self.runner.compute(
            total_loss_state.snapshot(),
            "total-loss-001",
        )
        total_loss_flow = next(
            metric
            for metric in total_loss_metrics["metrics"]
            if metric["flow_id"] == "FLOW-1"
        )
        self.assertTrue(total_loss_flow["connected"])
        self.assertEqual(total_loss_flow["loss_rate"], 1.0)
        self.assertGreater(total_loss_flow["tx_packets"], 0)
        self.assertEqual(total_loss_flow["rx_packets"], 0)
        self.assertEqual(total_loss_flow["link_state"], "UP")


if __name__ == "__main__":
    unittest.main()
