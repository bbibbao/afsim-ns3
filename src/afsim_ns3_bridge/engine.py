from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, TextIO

from .state import StateSnapshot


class RunnerError(RuntimeError):
    pass


@dataclass(frozen=True)
class LinkRecord:
    link_id: str
    source_entity_id: str
    target_entity_id: str
    kind: str
    data_rate_bps: int
    delay_ms: float
    configured_loss_rate: float
    state: str


class Ns3Runner:
    """Persistent ns-3 worker; Simulator::Destroy resets each calculation."""

    def __init__(self, executable: Path) -> None:
        self._executable = executable
        self._lock = threading.Lock()
        self._process: subprocess.Popen[str] | None = None

    def compute(
        self,
        snapshot: StateSnapshot,
        request_id: str,
    ) -> dict[str, Any]:
        started = time.perf_counter()
        with self._lock:
            process = self._ensure_process()
            entities = list(snapshot.entities)
            node_index = {
                entity.entity_id: index
                for index, entity in enumerate(entities)
            }
            alive = {
                entity.entity_id: entity.alive
                for entity in entities
            }
            links = self._links(snapshot, alive)
            flows = list(snapshot.flows)
            scenario_id = f"{snapshot.revision}-{snapshot.timestamp_ms}"

            lines = [
                (
                    f"SCENARIO\t{scenario_id}\t{len(entities)}\t"
                    f"{len(links)}\t{len(flows)}"
                )
            ]
            for index, entity in enumerate(entities):
                lines.append(
                    "\t".join(
                        [
                            "NODE",
                            str(index),
                            str(entity.position.x_m),
                            str(entity.position.y_m),
                            str(entity.position.z_m),
                            "1" if entity.alive else "0",
                        ]
                    )
                )

            for index, link in enumerate(links):
                lines.append(
                    "\t".join(
                        [
                            "LINK",
                            str(index),
                            str(node_index[link.source_entity_id]),
                            str(node_index[link.target_entity_id]),
                            str(link.data_rate_bps),
                            str(link.delay_ms),
                            str(link.configured_loss_rate),
                            "1" if link.state == "UP" else "0",
                        ]
                    )
                )

            for index, flow in enumerate(flows):
                enabled = (
                    flow.active
                    and alive.get(flow.source_entity_id, False)
                    and alive.get(flow.target_entity_id, False)
                )
                lines.append(
                    "\t".join(
                        [
                            "FLOW",
                            str(index),
                            str(node_index[flow.source_entity_id]),
                            str(node_index[flow.target_entity_id]),
                            str(flow.packet_size_bytes),
                            str(flow.interval_ms),
                            str(flow.duration_ms),
                            "1" if enabled else "0",
                        ]
                    )
                )
            lines.append("RUN")

            stdin = _require_stream(process.stdin, "stdin")
            stdout = _require_stream(process.stdout, "stdout")
            stdin.write("\n".join(lines) + "\n")
            stdin.flush()

            measured: dict[int, dict[str, Any]] = {}
            while True:
                line = stdout.readline()
                if not line:
                    stderr = (
                        process.stderr.read()
                        if process.stderr is not None
                        else ""
                    )
                    self._process = None
                    raise RunnerError(
                        f"ns-3 runner stopped unexpectedly: {stderr.strip()}"
                    )
                parts = line.rstrip("\r\n").split("\t")
                if not parts:
                    continue
                if parts[0] == "ERROR":
                    raise RunnerError("\t".join(parts[1:]))
                if parts[0] == "RESULT":
                    if len(parts) != 9:
                        raise RunnerError(f"invalid RESULT line: {line!r}")
                    measured[int(parts[1])] = {
                        "connected": parts[2] == "1",
                        "latency_ms": float(parts[3]),
                        "loss_rate": float(parts[4]),
                        "throughput_bps": float(parts[5]),
                        "tx_packets": int(parts[6]),
                        "rx_packets": int(parts[7]),
                        "link_state": parts[8],
                    }
                if parts[0] == "DONE":
                    if len(parts) < 2 or parts[1] != scenario_id:
                        raise RunnerError(f"invalid DONE line: {line!r}")
                    break

        metrics = []
        for index, flow in enumerate(flows):
            values = measured.get(
                index,
                {
                    "connected": False,
                    "latency_ms": 0.0,
                    "loss_rate": 1.0,
                    "throughput_bps": 0.0,
                    "tx_packets": 0,
                    "rx_packets": 0,
                    "link_state": "DOWN",
                },
            )
            metrics.append(
                {
                    "flow_id": flow.flow_id,
                    "source_entity_id": flow.source_entity_id,
                    "target_entity_id": flow.target_entity_id,
                    **values,
                    "traffic_direction": (
                        f"{flow.source_entity_id}->{flow.target_entity_id}"
                    ),
                }
            )

        connected_entities = {
            entity_id
            for link in links
            if link.state == "UP"
            for entity_id in (
                link.source_entity_id,
                link.target_entity_id,
            )
        }
        return {
            "request_id": request_id,
            "timestamp_ms": snapshot.timestamp_ms,
            "revision": snapshot.revision,
            "node_mappings": [
                {
                    "entity_id": entity.entity_id,
                    "business_node_id": entity.business_node_id,
                    "ns3_node_id": node_index[entity.entity_id],
                }
                for entity in entities
            ],
            "nodes": [
                {
                    "entity_id": entity.entity_id,
                    "business_node_id": entity.business_node_id,
                    "ns3_node_id": node_index[entity.entity_id],
                    "alive": entity.alive,
                    "connected": (
                        entity.alive
                        and entity.entity_id in connected_entities
                    ),
                }
                for entity in entities
            ],
            "links": [
                {
                    "link_id": link.link_id,
                    "source_entity_id": link.source_entity_id,
                    "target_entity_id": link.target_entity_id,
                    "kind": link.kind,
                    "data_rate_bps": link.data_rate_bps,
                    "delay_ms": link.delay_ms,
                    "configured_loss_rate": link.configured_loss_rate,
                    "state": link.state,
                }
                for link in links
            ],
            "metrics": metrics,
            "compute_ms": round(
                (time.perf_counter() - started) * 1000.0,
                3,
            ),
        }

    def close(self) -> None:
        with self._lock:
            if self._process is None:
                return
            process = self._process
            try:
                stdin = _require_stream(process.stdin, "stdin")
                stdin.write("QUIT\n")
                stdin.flush()
                process.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            finally:
                for stream in (process.stdin, process.stdout, process.stderr):
                    if stream is not None:
                        try:
                            stream.close()
                        except OSError:
                            pass
                self._process = None

    def _ensure_process(self) -> subprocess.Popen[str]:
        if self._process is not None and self._process.poll() is None:
            return self._process
        if not self._executable.is_file():
            raise RunnerError(f"ns-3 runner does not exist: {self._executable}")
        self._process = subprocess.Popen(
            [str(self._executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        return self._process

    @staticmethod
    def _links(
        snapshot: StateSnapshot,
        alive: dict[str, bool],
    ) -> list[LinkRecord]:
        links: list[LinkRecord] = []
        for entity in snapshot.entities:
            for device in entity.devices:
                state = (
                    "UP"
                    if (
                        device.link_state == "UP"
                        and entity.alive
                        and alive.get(device.peer_entity_id, False)
                    )
                    else "DOWN"
                )
                links.append(
                    LinkRecord(
                        link_id=f"{entity.entity_id}:{device.device_id}",
                        source_entity_id=entity.entity_id,
                        target_entity_id=device.peer_entity_id,
                        kind=device.kind,
                        data_rate_bps=device.data_rate_bps,
                        delay_ms=device.delay_ms,
                        configured_loss_rate=device.loss_rate,
                        state=state,
                    )
                )
        return links


def _require_stream(
    stream: TextIO | None,
    name: str,
) -> TextIO:
    if stream is None:
        raise RunnerError(f"runner {name} is unavailable")
    return stream
