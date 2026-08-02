from __future__ import annotations

import asyncio
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any

from afsim_ns3_bridge.metrics_store import MetricsStore
from afsim_ns3_bridge.server import BridgeService
from afsim_ns3_bridge.state import StateSnapshot

from common import sample


class SlowRunner:
    def __init__(self) -> None:
        self.compute_count = 0

    def compute(
        self,
        snapshot: StateSnapshot,
        request_id: str,
    ) -> dict[str, Any]:
        self.compute_count += 1
        time.sleep(0.03)
        return {
            "request_id": request_id,
            "timestamp_ms": snapshot.timestamp_ms,
            "revision": snapshot.revision,
            "node_mappings": [],
            "links": [],
            "metrics": [],
            "compute_ms": 30.0,
        }

    def close(self) -> None:
        return None


class CoalescingTest(unittest.IsolatedAsyncioTestCase):
    async def test_rapid_deltas_are_accepted_without_waiting_for_compute(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = SlowRunner()
            metrics = MetricsStore(Path(directory) / "metrics.csv")
            service = BridgeService(runner, metrics)  # type: ignore[arg-type]
            await service.start()
            try:
                initial = sample("sample_init.json")
                await service.process(initial)

                started = time.perf_counter()
                for index in range(1, 101):
                    delta = {
                        "protocol_version": "0.2",
                        "message_type": "afsim_delta",
                        "request_id": f"rapid-{index}",
                        "timestamp_ms": index,
                        "entity_upserts": [
                            {
                                "entity_id": "AFSIM-1",
                                "position": {"x_m": float(index)},
                            }
                        ],
                        "entity_removals": [],
                        "flow_upserts": [],
                        "flow_removals": [],
                    }
                    acknowledgement = await service.process(delta)
                    self.assertEqual(
                        acknowledgement["status"]["message"],
                        "accepted",
                    )
                submit_ms = (time.perf_counter() - started) * 1000.0

                deadline = time.monotonic() + 3.0
                while metrics.latest().get("revision") != 101:
                    if time.monotonic() >= deadline:
                        self.fail("latest coalesced revision was not computed")
                    await asyncio.sleep(0.01)

                self.assertEqual(metrics.latest()["revision"], 101)
                self.assertLess(runner.compute_count, 20)
                print(
                    (
                        f"rapid_delta_submit_ms={submit_ms:.3f} "
                        f"ns3_computations={runner.compute_count}"
                    )
                )
            finally:
                await service.close()


if __name__ == "__main__":
    unittest.main()
