from __future__ import annotations

import argparse
import asyncio
import signal
from contextlib import suppress
from pathlib import Path

from afsim_ns3_bridge.engine import Ns3Runner
from afsim_ns3_bridge.metrics_store import MetricsStore
from afsim_ns3_bridge.server import (
    BridgeService,
    JsonlTcpServer,
    MetricsHttpServer,
)

PROJECT_ROOT = Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="AFSIM/ns-3 incremental JSONL bridge",
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--tcp-port", type=int, default=18080)
    parser.add_argument("--http-port", type=int, default=18081)
    parser.add_argument(
        "--runner",
        type=Path,
        default=PROJECT_ROOT / ".deps/bin/afsim-ns3-runner",
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=PROJECT_ROOT / "output/metrics/metrics.csv",
    )
    return parser.parse_args()


async def run(args: argparse.Namespace) -> None:
    runner = Ns3Runner(args.runner.resolve())
    metrics_store = MetricsStore(args.csv.resolve())
    service = BridgeService(runner, metrics_store)
    tcp_server = JsonlTcpServer(service, args.host, args.tcp_port)
    http_server = MetricsHttpServer(
        service,
        metrics_store,
        args.host,
        args.http_port,
    )

    await service.start()
    try:
        await tcp_server.start()
        await http_server.start()
    except BaseException:
        await tcp_server.close()
        await http_server.close()
        await service.close()
        raise

    print(
        (
            f"AFSIM/ns-3 JSONL: {args.host}:{args.tcp_port}; "
            f"metrics HTTP: {args.host}:{args.http_port}"
        ),
        flush=True,
    )

    stopped = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signal_name in (signal.SIGINT, signal.SIGTERM):
        with suppress(NotImplementedError):
            loop.add_signal_handler(signal_name, stopped.set)

    try:
        await stopped.wait()
    finally:
        await tcp_server.close()
        await http_server.close()
        await service.close()


def main() -> None:
    asyncio.run(run(parse_args()))


if __name__ == "__main__":
    main()
