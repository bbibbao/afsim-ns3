from __future__ import annotations

import asyncio
import hashlib
import json
from collections import OrderedDict
from contextlib import suppress
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlsplit

from .effects import evaluate_effects
from .engine import Ns3Runner, RunnerError
from .metrics_store import MetricsStore
from .models import PROTOCOL_VERSION, ProtocolError, validate_envelope
from .state import StateSnapshot, StateStore

MAX_JSONL_BYTES = 5 * 1024 * 1024
MAX_HTTP_HEADER_BYTES = 16 * 1024
IDEMPOTENCY_CACHE_SIZE = 1000


@dataclass(frozen=True)
class ComputationJob:
    request_id: str
    snapshot: StateSnapshot


class BridgeService:
    """Coordinates incremental state, coalesced ns-3 work, and subscribers."""

    def __init__(self, runner: Ns3Runner, metrics_store: MetricsStore) -> None:
        self._runner = runner
        self._metrics_store = metrics_store
        self._state = StateStore()
        self._state_lock = asyncio.Lock()
        self._pending_lock = asyncio.Lock()
        self._pending_event = asyncio.Event()
        self._pending: ComputationJob | None = None
        self._worker: asyncio.Task[None] | None = None
        self._clients: set[asyncio.StreamWriter] = set()
        self._write_locks: dict[asyncio.StreamWriter, asyncio.Lock] = {}
        self._idempotency: OrderedDict[str, tuple[str, dict[str, Any]]] = (
            OrderedDict()
        )
        self._last_error: str | None = None
        self._computed_revision = 0

    async def start(self) -> None:
        if self._worker is None:
            self._worker = asyncio.create_task(
                self._compute_loop(),
                name="ns3-compute-worker",
            )

    async def close(self) -> None:
        if self._worker is not None:
            self._worker.cancel()
            with suppress(asyncio.CancelledError):
                await self._worker
            self._worker = None
        await asyncio.to_thread(self._runner.close)

    def add_client(self, writer: asyncio.StreamWriter) -> None:
        self._clients.add(writer)
        self._write_locks.setdefault(writer, asyncio.Lock())

    def remove_client(self, writer: asyncio.StreamWriter) -> None:
        self._clients.discard(writer)
        self._write_locks.pop(writer, None)

    async def process(
        self,
        payload: Any,
    ) -> dict[str, Any]:
        message_type, request_id, timestamp_ms = validate_envelope(payload)
        assert isinstance(payload, dict)

        if message_type == "metrics_query":
            return self._metrics_query_response(request_id)

        digest = hashlib.sha256(
            json.dumps(
                payload,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
        ).hexdigest()

        async with self._state_lock:
            cached = self._idempotency.get(request_id)
            if cached is not None:
                cached_digest, cached_response = cached
                if cached_digest != digest:
                    raise ProtocolError(
                        1004,
                        "request_id was reused with different content",
                    )
                duplicate = dict(cached_response)
                duplicate["duplicate"] = True
                return duplicate

            if message_type == "afsim_init":
                self._state.initialize(payload, timestamp_ms)
            else:
                self._state.apply_delta(payload, timestamp_ms)
            snapshot = self._state.snapshot()

            replaced_revision = await self._queue(
                ComputationJob(request_id=request_id, snapshot=snapshot)
            )
            response: dict[str, Any] = {
                "protocol_version": PROTOCOL_VERSION,
                "message_type": "ns3_ack",
                "request_id": request_id,
                "timestamp_ms": timestamp_ms,
                "revision": snapshot.revision,
                "status": {"code": 0, "message": "accepted"},
                "queued": True,
                "coalesced_revision": replaced_revision,
                "duplicate": False,
            }
            self._remember_request(request_id, digest, response)
            return response

    def health(self) -> dict[str, Any]:
        latest = self._metrics_store.latest()
        return {
            "status": "ok" if self._last_error is None else "degraded",
            "protocol_version": PROTOCOL_VERSION,
            "initialized": self._state.initialized,
            "state_revision": self._state.revision,
            "computed_revision": self._computed_revision,
            "latest_metrics_revision": latest.get("revision", 0),
            "pending": self._pending is not None,
            "last_error": self._last_error,
        }

    async def send(
        self,
        writer: asyncio.StreamWriter,
        response: dict[str, Any],
    ) -> None:
        lock = self._write_locks.setdefault(writer, asyncio.Lock())
        encoded = (
            json.dumps(
                response,
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
            + b"\n"
        )
        async with lock:
            writer.write(encoded)
            await writer.drain()

    async def _queue(self, job: ComputationJob) -> int | None:
        async with self._pending_lock:
            replaced_revision = (
                self._pending.snapshot.revision
                if self._pending is not None
                else None
            )
            self._pending = job
            self._pending_event.set()
            return replaced_revision

    async def _compute_loop(self) -> None:
        while True:
            await self._pending_event.wait()
            async with self._pending_lock:
                job = self._pending
                self._pending = None
                self._pending_event.clear()
            if job is None:
                continue

            try:
                measured = await asyncio.to_thread(
                    self._runner.compute,
                    job.snapshot,
                    job.request_id,
                )
                self._last_error = None
            except (RunnerError, OSError, ValueError) as error:
                self._last_error = str(error)
                await self._broadcast(
                    error_response(
                        error,
                        request_id=job.request_id,
                        timestamp_ms=job.snapshot.timestamp_ms,
                        revision=job.snapshot.revision,
                        code=2001,
                    )
                )
                continue

            # A result for an old revision must never re-enable a subsystem.
            if job.snapshot.revision < self._state.revision:
                continue

            response: dict[str, Any] = {
                "protocol_version": PROTOCOL_VERSION,
                "message_type": "ns3_metrics",
                "status": {"code": 0, "message": "ok"},
                **measured,
            }
            async with self._pending_lock:
                response["queue_state"] = {
                    "pending": self._pending is not None,
                    "latest_pending_revision": (
                        self._pending.snapshot.revision
                        if self._pending is not None
                        else None
                    ),
                }
            response["effects"] = evaluate_effects(
                job.snapshot,
                response["metrics"],
            )
            self._metrics_store.record(response)
            self._computed_revision = job.snapshot.revision
            await self._broadcast(response)

    async def _broadcast(self, response: dict[str, Any]) -> None:
        failed: list[asyncio.StreamWriter] = []
        for writer in tuple(self._clients):
            try:
                await self.send(writer, response)
            except (ConnectionError, OSError, RuntimeError):
                failed.append(writer)
        for writer in failed:
            self.remove_client(writer)

    def _metrics_query_response(self, request_id: str) -> dict[str, Any]:
        response = self._metrics_store.latest()
        source_request_id = response.get("request_id")
        response["request_id"] = request_id
        if source_request_id:
            response["source_request_id"] = source_request_id
        return response

    def _remember_request(
        self,
        request_id: str,
        digest: str,
        response: dict[str, Any],
    ) -> None:
        self._idempotency[request_id] = (digest, dict(response))
        self._idempotency.move_to_end(request_id)
        while len(self._idempotency) > IDEMPOTENCY_CACHE_SIZE:
            self._idempotency.popitem(last=False)


class JsonlTcpServer:
    def __init__(
        self,
        service: BridgeService,
        host: str,
        port: int,
    ) -> None:
        self._service = service
        self._host = host
        self._port = port
        self._server: asyncio.Server | None = None

    @property
    def port(self) -> int:
        if self._server is None or not self._server.sockets:
            return self._port
        return int(self._server.sockets[0].getsockname()[1])

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._handle_client,
            self._host,
            self._port,
            limit=MAX_JSONL_BYTES + 1,
        )

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

    async def _handle_client(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        self._service.add_client(writer)
        try:
            while True:
                payload: Any = None
                try:
                    line = await reader.readline()
                except ValueError as error:
                    await self._service.send(
                        writer,
                        error_response(error, code=1005),
                    )
                    break
                if not line:
                    break
                if len(line) > MAX_JSONL_BYTES:
                    await self._service.send(
                        writer,
                        error_response(
                            "message exceeds 5 MiB",
                            code=1005,
                        ),
                    )
                    break
                try:
                    payload = json.loads(
                        line.decode("utf-8"),
                        parse_constant=_reject_json_constant,
                    )
                    response = await self._service.process(payload)
                except UnicodeDecodeError as error:
                    response = error_response(error, code=1001)
                except json.JSONDecodeError as error:
                    response = error_response(error, code=1001)
                except ProtocolError as error:
                    request_id = ""
                    timestamp_ms = 0
                    if isinstance(payload, dict):
                        request_id = str(payload.get("request_id", ""))
                        raw_timestamp = payload.get("timestamp_ms", 0)
                        if isinstance(raw_timestamp, int):
                            timestamp_ms = raw_timestamp
                    response = error_response(
                        error,
                        request_id=request_id,
                        timestamp_ms=timestamp_ms,
                        code=error.code,
                    )
                except ValueError as error:
                    response = error_response(error, code=1001)
                await self._service.send(writer, response)
        except (ConnectionError, OSError):
            pass
        finally:
            self._service.remove_client(writer)
            writer.close()
            with suppress(ConnectionError, OSError):
                await writer.wait_closed()


class MetricsHttpServer:
    def __init__(
        self,
        service: BridgeService,
        metrics_store: MetricsStore,
        host: str,
        port: int,
    ) -> None:
        self._service = service
        self._metrics_store = metrics_store
        self._host = host
        self._port = port
        self._server: asyncio.Server | None = None

    @property
    def port(self) -> int:
        if self._server is None or not self._server.sockets:
            return self._port
        return int(self._server.sockets[0].getsockname()[1])

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._handle_client,
            self._host,
            self._port,
            limit=MAX_HTTP_HEADER_BYTES,
        )

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

    async def _handle_client(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        try:
            request_line = await reader.readline()
            if not request_line:
                return
            if len(request_line) >= MAX_HTTP_HEADER_BYTES:
                await _write_http(writer, 431, b"headers too large", "text/plain")
                return
            while True:
                line = await reader.readline()
                if line in {b"\r\n", b"\n", b""}:
                    break

            parts = request_line.decode("ascii", errors="replace").split()
            if len(parts) != 3 or parts[0] != "GET":
                await _write_http(
                    writer,
                    405,
                    b"only GET is supported",
                    "text/plain; charset=utf-8",
                )
                return

            path = urlsplit(parts[1]).path
            if path == "/health":
                body = _json_bytes(self._service.health())
                await _write_http(writer, 200, body, "application/json")
            elif path == "/metrics/latest":
                body = self._metrics_store.latest_json()
                await _write_http(writer, 200, body, "application/json")
            elif path == "/metrics/export.csv":
                body = self._metrics_store.history_csv()
                await _write_http(
                    writer,
                    200,
                    body,
                    "text/csv; charset=utf-8",
                    {
                        "Content-Disposition": (
                            'attachment; filename="afsim-ns3-metrics.csv"'
                        )
                    },
                )
            else:
                await _write_http(writer, 404, b"not found", "text/plain")
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            writer.close()
            with suppress(ConnectionError, OSError):
                await writer.wait_closed()


def error_response(
    error: Exception | str,
    *,
    request_id: str = "",
    timestamp_ms: int = 0,
    revision: int = 0,
    code: int = 1000,
) -> dict[str, Any]:
    message = error.message if isinstance(error, ProtocolError) else str(error)
    return {
        "protocol_version": PROTOCOL_VERSION,
        "message_type": "ns3_error",
        "request_id": request_id,
        "timestamp_ms": timestamp_ms,
        "revision": revision,
        "status": {"code": code, "message": message},
    }


def _json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"invalid JSON numeric constant: {value}")


async def _write_http(
    writer: asyncio.StreamWriter,
    status: int,
    body: bytes,
    content_type: str,
    extra_headers: dict[str, str] | None = None,
) -> None:
    reasons = {
        200: "OK",
        404: "Not Found",
        405: "Method Not Allowed",
        431: "Request Header Fields Too Large",
    }
    headers = {
        "Content-Type": content_type,
        "Content-Length": str(len(body)),
        "Connection": "close",
        "Access-Control-Allow-Origin": "*",
    }
    if extra_headers:
        headers.update(extra_headers)
    head = [f"HTTP/1.1 {status} {reasons[status]}"]
    head.extend(f"{key}: {value}" for key, value in headers.items())
    writer.write(("\r\n".join(head) + "\r\n\r\n").encode("ascii") + body)
    await writer.drain()
