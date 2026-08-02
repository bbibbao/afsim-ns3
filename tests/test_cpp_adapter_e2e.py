from __future__ import annotations

import asyncio
import tempfile
import unittest
from pathlib import Path

from afsim_ns3_bridge.engine import Ns3Runner
from afsim_ns3_bridge.metrics_store import MetricsStore
from afsim_ns3_bridge.server import BridgeService, JsonlTcpServer

from common import PROJECT_ROOT


class CppAdapterEndToEndTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        runner_path = PROJECT_ROOT / ".deps/bin/afsim-ns3-runner"
        probe_path = (
            PROJECT_ROOT
            / ".deps/windows-adapter-build/windows_adapter_probe"
        )
        if not runner_path.is_file() or not probe_path.is_file():
            self.skipTest("local ns-3 runner or C++ adapter probe is not built")

        self.probe_path = probe_path
        self.temporary = tempfile.TemporaryDirectory()
        self.metrics_store = MetricsStore(
            Path(self.temporary.name) / "metrics.csv"
        )
        self.service = BridgeService(
            Ns3Runner(runner_path),
            self.metrics_store,
        )
        self.tcp = JsonlTcpServer(self.service, "127.0.0.1", 0)
        await self.service.start()
        await self.tcp.start()

    async def asyncTearDown(self) -> None:
        await self.tcp.close()
        await self.service.close()
        self.temporary.cleanup()

    async def test_cpp_init_delta_ns3_and_effect_return_path(self) -> None:
        process = await asyncio.create_subprocess_exec(
            str(self.probe_path),
            "127.0.0.1",
            str(self.tcp.port),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            stdout, stderr = await asyncio.wait_for(
                process.communicate(),
                timeout=30,
            )
        except TimeoutError:
            process.kill()
            await process.wait()
            self.fail("C++ adapter end-to-end probe timed out")

        self.assertEqual(
            process.returncode,
            0,
            stderr.decode("utf-8", errors="replace"),
        )
        self.assertIn(
            "revision=2 weapon=BLOCKED radar=DEGRADED",
            stdout.decode("utf-8"),
        )

        latest = self.metrics_store.latest()
        self.assertEqual(latest["revision"], 2)
        metric = latest["metrics"][0]
        self.assertTrue(metric["connected"])
        self.assertGreaterEqual(metric["latency_ms"], 120.0)
        self.assertGreater(metric["throughput_bps"], 0.0)
        self.assertEqual(metric["traffic_direction"], "AFSIM-1->AFSIM-2")
        self.assertEqual(metric["link_state"], "UP")

        effects = {
            item["subsystem_id"]: item["state"]
            for item in latest["effects"]
        }
        self.assertEqual(effects["WEAPON-1"], "BLOCKED")
        self.assertEqual(effects["RADAR-1"], "DEGRADED")

        csv_text = self.metrics_store.history_csv().decode("utf-8-sig")
        self.assertIn("cpp-init-001,0,1", csv_text)
        self.assertIn("cpp-delta-001,1000,2", csv_text)


if __name__ == "__main__":
    unittest.main()
