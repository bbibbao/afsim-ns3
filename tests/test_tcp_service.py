from __future__ import annotations

import asyncio
import json
import tempfile
import unittest
from pathlib import Path
from typing import Any

from afsim_ns3_bridge.engine import Ns3Runner
from afsim_ns3_bridge.metrics_store import MetricsStore
from afsim_ns3_bridge.server import (
    BridgeService,
    JsonlTcpServer,
    MetricsHttpServer,
)

from common import PROJECT_ROOT, sample


class TcpServiceIntegrationTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        runner_path = PROJECT_ROOT / ".deps/bin/afsim-ns3-runner"
        if not runner_path.is_file():
            self.skipTest("project-local ns-3 runner is not built")
        self.temporary = tempfile.TemporaryDirectory()
        self.metrics_store = MetricsStore(
            Path(self.temporary.name) / "metrics.csv"
        )
        self.service = BridgeService(
            Ns3Runner(runner_path),
            self.metrics_store,
        )
        self.tcp = JsonlTcpServer(self.service, "127.0.0.1", 0)
        self.http = MetricsHttpServer(
            self.service,
            self.metrics_store,
            "127.0.0.1",
            0,
        )
        await self.service.start()
        await self.tcp.start()
        await self.http.start()
        self.reader, self.writer = await asyncio.open_connection(
            "127.0.0.1",
            self.tcp.port,
        )

    async def asyncTearDown(self) -> None:
        self.writer.close()
        await self.writer.wait_closed()
        await self.tcp.close()
        await self.http.close()
        await self.service.close()
        self.temporary.cleanup()

    async def test_init_delta_metrics_effects_and_exports(self) -> None:
        initial = sample("sample_init.json")
        await self._send(initial)
        init_messages = await self._until_metrics(revision=1)
        self.assertTrue(
            any(item["message_type"] == "ns3_ack" for item in init_messages)
        )
        init_metrics = next(
            item
            for item in init_messages
            if item["message_type"] == "ns3_metrics"
        )
        self.assertTrue(init_metrics["metrics"][0]["connected"])
        self.assertTrue(
            all(
                item["state"] == "AVAILABLE"
                for item in init_metrics["effects"]
            )
        )

        delta = sample("sample_delta.json")
        await self._send(delta)
        delta_messages = await self._until_metrics(revision=2)
        delta_metrics = next(
            item
            for item in delta_messages
            if item["message_type"] == "ns3_metrics"
        )
        effects = {
            item["subsystem_id"]: item["state"]
            for item in delta_metrics["effects"]
        }
        self.assertEqual(effects["WEAPON-1"], "BLOCKED")
        self.assertEqual(effects["RADAR-1"], "DEGRADED")

        status, latest = await self._http_get("/metrics/latest")
        self.assertEqual(status, 200)
        latest_payload = json.loads(latest)
        self.assertEqual(latest_payload["revision"], 2)

        status, csv_body = await self._http_get("/metrics/export.csv")
        self.assertEqual(status, 200)
        csv_text = csv_body.decode("utf-8-sig")
        self.assertIn("request_id,timestamp_ms,revision", csv_text)
        self.assertIn("delta-001,1000,2", csv_text)

        query = {
            "protocol_version": "0.2",
            "message_type": "metrics_query",
            "request_id": "query-001",
            "timestamp_ms": 1001,
        }
        await self._send(query)
        queried = await self._read_json()
        self.assertEqual(queried["request_id"], "query-001")
        self.assertEqual(queried["source_request_id"], "delta-001")

    async def test_idempotency_does_not_reapply_state(self) -> None:
        initial = sample("sample_init.json")
        await self._send(initial)
        await self._until_metrics(revision=1)
        await self._send(initial)
        duplicate = await self._read_json()
        self.assertEqual(duplicate["message_type"], "ns3_ack")
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(duplicate["revision"], 1)

        changed = dict(initial)
        changed["timestamp_ms"] = 1
        await self._send(changed)
        error = await self._read_json()
        self.assertEqual(error["message_type"], "ns3_error")
        self.assertEqual(error["status"]["code"], 1004)

    async def _send(self, payload: dict[str, Any]) -> None:
        self.writer.write(
            json.dumps(payload, separators=(",", ":")).encode("utf-8")
            + b"\n"
        )
        await self.writer.drain()

    async def _read_json(self) -> dict[str, Any]:
        line = await asyncio.wait_for(self.reader.readline(), timeout=10)
        self.assertTrue(line)
        return json.loads(line)

    async def _until_metrics(
        self,
        revision: int,
    ) -> list[dict[str, Any]]:
        messages: list[dict[str, Any]] = []
        while True:
            message = await self._read_json()
            messages.append(message)
            if (
                message["message_type"] == "ns3_metrics"
                and message["revision"] == revision
            ):
                return messages

    async def _http_get(self, path: str) -> tuple[int, bytes]:
        reader, writer = await asyncio.open_connection(
            "127.0.0.1",
            self.http.port,
        )
        writer.write(
            (
                f"GET {path} HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: close\r\n\r\n"
            ).encode("ascii")
        )
        await writer.drain()
        raw = await reader.read()
        writer.close()
        await writer.wait_closed()
        head, body = raw.split(b"\r\n\r\n", 1)
        status = int(head.split(b" ", 2)[1])
        return status, body


if __name__ == "__main__":
    unittest.main()
