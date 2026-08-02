from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

PROTOCOL_VERSION = "0.2"
ALLOWED_MESSAGE_TYPES = {"afsim_init", "afsim_delta", "metrics_query"}


class ProtocolError(ValueError):
    """Raised when an external JSONL message violates the protocol contract."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def _required_text(data: dict[str, Any], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ProtocolError(1002, f"{key} must be a non-empty string")
    return value.strip()


def _optional_text(data: dict[str, Any], key: str) -> str | None:
    value = data.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value.strip():
        raise ProtocolError(1002, f"{key} must be null or a non-empty string")
    return value.strip()


def _number(
    data: dict[str, Any],
    key: str,
    *,
    minimum: float | None = None,
    maximum: float | None = None,
) -> float:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProtocolError(1002, f"{key} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise ProtocolError(1002, f"{key} must be finite")
    if minimum is not None and result < minimum:
        raise ProtocolError(1002, f"{key} must be >= {minimum}")
    if maximum is not None and result > maximum:
        raise ProtocolError(1002, f"{key} must be <= {maximum}")
    return result


def _integer(
    data: dict[str, Any],
    key: str,
    *,
    minimum: int | None = None,
) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError(1002, f"{key} must be an integer")
    if minimum is not None and value < minimum:
        raise ProtocolError(1002, f"{key} must be >= {minimum}")
    return value


@dataclass(frozen=True)
class Position:
    x_m: float
    y_m: float
    z_m: float

    @classmethod
    def from_dict(cls, data: Any) -> "Position":
        if not isinstance(data, dict):
            raise ProtocolError(1002, "position must be an object")
        return cls(
            x_m=_number(data, "x_m"),
            y_m=_number(data, "y_m"),
            z_m=_number(data, "z_m"),
        )


@dataclass(frozen=True)
class Velocity:
    vx_mps: float
    vy_mps: float
    vz_mps: float

    @classmethod
    def from_dict(cls, data: Any) -> "Velocity":
        if not isinstance(data, dict):
            raise ProtocolError(1002, "velocity must be an object")
        return cls(
            vx_mps=_number(data, "vx_mps"),
            vy_mps=_number(data, "vy_mps"),
            vz_mps=_number(data, "vz_mps"),
        )


@dataclass(frozen=True)
class DeviceConfig:
    device_id: str
    peer_entity_id: str
    kind: str
    data_rate_bps: int
    delay_ms: float
    loss_rate: float
    link_state: str

    @classmethod
    def from_dict(cls, data: Any) -> "DeviceConfig":
        if not isinstance(data, dict):
            raise ProtocolError(1002, "devices entries must be objects")
        kind = _required_text(data, "kind").lower()
        if kind not in {"wired", "wireless"}:
            raise ProtocolError(1002, "device kind must be wired or wireless")
        link_state = _required_text(data, "link_state").upper()
        if link_state not in {"UP", "DOWN"}:
            raise ProtocolError(1002, "link_state must be UP or DOWN")
        return cls(
            device_id=_required_text(data, "device_id"),
            peer_entity_id=_required_text(data, "peer_entity_id"),
            kind=kind,
            data_rate_bps=_integer(data, "data_rate_bps", minimum=1),
            delay_ms=_number(data, "delay_ms", minimum=0.0),
            loss_rate=_number(data, "loss_rate", minimum=0.0, maximum=1.0),
            link_state=link_state,
        )


@dataclass(frozen=True)
class EffectPolicy:
    subsystem_id: str
    subsystem_type: str
    peer_entity_id: str
    max_delay_ms: float
    max_loss_rate: float
    min_throughput_bps: float
    violation_state: str

    @classmethod
    def from_dict(cls, data: Any) -> "EffectPolicy":
        if not isinstance(data, dict):
            raise ProtocolError(1002, "effect_policies entries must be objects")
        subsystem_type = _required_text(data, "subsystem_type").lower()
        if subsystem_type not in {"weapon", "radar"}:
            raise ProtocolError(1002, "subsystem_type must be weapon or radar")
        violation_state = _required_text(data, "violation_state").upper()
        if violation_state not in {"DEGRADED", "BLOCKED"}:
            raise ProtocolError(
                1002,
                "violation_state must be DEGRADED or BLOCKED",
            )
        return cls(
            subsystem_id=_required_text(data, "subsystem_id"),
            subsystem_type=subsystem_type,
            peer_entity_id=_required_text(data, "peer_entity_id"),
            max_delay_ms=_number(data, "max_delay_ms", minimum=0.0),
            max_loss_rate=_number(
                data,
                "max_loss_rate",
                minimum=0.0,
                maximum=1.0,
            ),
            min_throughput_bps=_number(
                data,
                "min_throughput_bps",
                minimum=0.0,
            ),
            violation_state=violation_state,
        )


@dataclass(frozen=True)
class EntityState:
    entity_id: str
    business_node_id: str
    parent_entity_id: str | None
    position: Position
    velocity: Velocity
    heading_deg: float
    alive: bool
    devices: tuple[DeviceConfig, ...] = field(default_factory=tuple)
    effect_policies: tuple[EffectPolicy, ...] = field(default_factory=tuple)

    @classmethod
    def from_dict(cls, data: Any) -> "EntityState":
        if not isinstance(data, dict):
            raise ProtocolError(1002, "entities entries must be objects")
        alive = data.get("alive")
        if not isinstance(alive, bool):
            raise ProtocolError(1002, "alive must be boolean")
        devices = data.get("devices", [])
        policies = data.get("effect_policies", [])
        if not isinstance(devices, list) or not isinstance(policies, list):
            raise ProtocolError(
                1002,
                "devices and effect_policies must be arrays",
            )
        return cls(
            entity_id=_required_text(data, "entity_id"),
            business_node_id=_required_text(data, "business_node_id"),
            parent_entity_id=_optional_text(data, "parent_entity_id"),
            position=Position.from_dict(data.get("position")),
            velocity=Velocity.from_dict(data.get("velocity")),
            heading_deg=_number(
                data,
                "heading_deg",
                minimum=0.0,
                maximum=360.0,
            ),
            alive=alive,
            devices=tuple(DeviceConfig.from_dict(item) for item in devices),
            effect_policies=tuple(
                EffectPolicy.from_dict(item) for item in policies
            ),
        )


@dataclass(frozen=True)
class FlowConfig:
    flow_id: str
    source_entity_id: str
    target_entity_id: str
    packet_size_bytes: int
    interval_ms: int
    duration_ms: int
    active: bool

    @classmethod
    def from_dict(cls, data: Any) -> "FlowConfig":
        if not isinstance(data, dict):
            raise ProtocolError(1002, "flows entries must be objects")
        active = data.get("active", True)
        if not isinstance(active, bool):
            raise ProtocolError(1002, "active must be boolean")
        return cls(
            flow_id=_required_text(data, "flow_id"),
            source_entity_id=_required_text(data, "source_entity_id"),
            target_entity_id=_required_text(data, "target_entity_id"),
            packet_size_bytes=_integer(
                data,
                "packet_size_bytes",
                minimum=64,
            ),
            interval_ms=_integer(data, "interval_ms", minimum=1),
            duration_ms=_integer(data, "duration_ms", minimum=100),
            active=active,
        )


def validate_envelope(payload: Any) -> tuple[str, str, int]:
    if not isinstance(payload, dict):
        raise ProtocolError(1001, "message must be a JSON object")
    version = payload.get("protocol_version")
    if version != PROTOCOL_VERSION:
        raise ProtocolError(
            1003,
            f"protocol_version must be {PROTOCOL_VERSION}",
        )
    message_type = _required_text(payload, "message_type")
    if message_type not in ALLOWED_MESSAGE_TYPES:
        raise ProtocolError(1003, f"unsupported message_type: {message_type}")
    request_id = _required_text(payload, "request_id")
    timestamp_ms = _integer(payload, "timestamp_ms", minimum=0)
    return message_type, request_id, timestamp_ms
