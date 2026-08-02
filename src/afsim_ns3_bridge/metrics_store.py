from __future__ import annotations

import csv
import io
import json
import threading
from copy import deepcopy
from pathlib import Path
from typing import Any

CSV_FIELDS = [
    "request_id",
    "timestamp_ms",
    "revision",
    "flow_id",
    "source_entity_id",
    "target_entity_id",
    "connected",
    "latency_ms",
    "loss_rate",
    "throughput_bps",
    "tx_packets",
    "rx_packets",
    "traffic_direction",
    "link_state",
]
METRIC_FIELDS = CSV_FIELDS[3:]


class MetricsStore:
    """Thread-safe latest/history store shared by TCP and HTTP surfaces."""

    def __init__(self, export_path: Path) -> None:
        self._export_path = export_path
        self._lock = threading.Lock()
        self._latest: dict[str, Any] | None = None

    def record(self, response: dict[str, Any]) -> None:
        with self._lock:
            self._latest = deepcopy(response)
            self._append_csv(response)

    def latest(self) -> dict[str, Any]:
        with self._lock:
            if self._latest is None:
                return {
                    "protocol_version": "0.2",
                    "message_type": "ns3_metrics",
                    "request_id": "",
                    "timestamp_ms": 0,
                    "revision": 0,
                    "status": {"code": 0, "message": "no metrics yet"},
                    "metrics": [],
                    "effects": [],
                    "links": [],
                    "nodes": [],
                    "node_mappings": [],
                    "queue_state": {
                        "pending": False,
                        "latest_pending_revision": None,
                    },
                    "compute_ms": 0.0,
                }
            return deepcopy(self._latest)

    def latest_json(self) -> bytes:
        return json.dumps(
            self.latest(),
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")

    def history_csv(self) -> bytes:
        with self._lock:
            if self._export_path.exists():
                return self._export_path.read_bytes()
            stream = io.StringIO(newline="")
            csv.DictWriter(stream, fieldnames=CSV_FIELDS).writeheader()
            return ("\ufeff" + stream.getvalue()).encode("utf-8")

    def _append_csv(self, response: dict[str, Any]) -> None:
        self._export_path.parent.mkdir(parents=True, exist_ok=True)
        write_header = not self._export_path.exists()
        with self._export_path.open("a", encoding="utf-8-sig", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=CSV_FIELDS)
            if write_header:
                writer.writeheader()
            for metric in response.get("metrics", []):
                writer.writerow(
                    {
                        "request_id": response["request_id"],
                        "timestamp_ms": response["timestamp_ms"],
                        "revision": response["revision"],
                        **{
                            field: metric.get(field, "")
                            for field in METRIC_FIELDS
                        },
                    }
                )
