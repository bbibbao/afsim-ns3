from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from typing import Any

from .models import EntityState, FlowConfig, ProtocolError


@dataclass(frozen=True)
class StateSnapshot:
    timestamp_ms: int
    entities: tuple[EntityState, ...]
    flows: tuple[FlowConfig, ...]
    revision: int


def _merge_entity(
    current: dict[str, Any],
    update: dict[str, Any],
) -> dict[str, Any]:
    merged = deepcopy(current)
    for key, value in update.items():
        if (
            key in {"position", "velocity"}
            and isinstance(value, dict)
            and isinstance(merged.get(key), dict)
        ):
            merged[key] = {**merged[key], **value}
        else:
            merged[key] = deepcopy(value)
    return merged


class StateStore:
    """Owns the full in-memory state while the wire protocol stays incremental."""

    def __init__(self) -> None:
        self._entities: dict[str, dict[str, Any]] = {}
        self._flows: dict[str, dict[str, Any]] = {}
        self._timestamp_ms = 0
        self._revision = 0
        self._initialized = False

    @property
    def initialized(self) -> bool:
        return self._initialized

    @property
    def revision(self) -> int:
        return self._revision

    def initialize(self, payload: dict[str, Any], timestamp_ms: int) -> None:
        entities = payload.get("entities")
        flows = payload.get("flows")
        if not isinstance(entities, list) or not isinstance(flows, list):
            raise ProtocolError(1002, "afsim_init requires entities and flows")

        entity_map: dict[str, dict[str, Any]] = {}
        for raw in entities:
            parsed = EntityState.from_dict(raw)
            if parsed.entity_id in entity_map:
                raise ProtocolError(
                    1002,
                    f"duplicate entity_id: {parsed.entity_id}",
                )
            entity_map[parsed.entity_id] = deepcopy(raw)

        flow_map: dict[str, dict[str, Any]] = {}
        for raw in flows:
            parsed = FlowConfig.from_dict(raw)
            if parsed.flow_id in flow_map:
                raise ProtocolError(1002, f"duplicate flow_id: {parsed.flow_id}")
            flow_map[parsed.flow_id] = deepcopy(raw)

        self._validate_references(entity_map, flow_map)
        self._entities = entity_map
        self._flows = flow_map
        self._timestamp_ms = timestamp_ms
        self._revision += 1
        self._initialized = True

    def apply_delta(self, payload: dict[str, Any], timestamp_ms: int) -> None:
        if not self._initialized:
            raise ProtocolError(1002, "afsim_init must be sent before afsim_delta")
        if timestamp_ms < self._timestamp_ms:
            raise ProtocolError(
                1002,
                "timestamp_ms must not move backwards",
            )

        entities = deepcopy(self._entities)
        flows = deepcopy(self._flows)

        entity_upserts = payload.get("entity_upserts", [])
        entity_removals = payload.get("entity_removals", [])
        flow_upserts = payload.get("flow_upserts", [])
        flow_removals = payload.get("flow_removals", [])
        if not all(
            isinstance(value, list)
            for value in (
                entity_upserts,
                entity_removals,
                flow_upserts,
                flow_removals,
            )
        ):
            raise ProtocolError(1002, "delta fields must be arrays")

        for entity_id in entity_removals:
            if not isinstance(entity_id, str) or not entity_id:
                raise ProtocolError(1002, "entity_removals must contain IDs")
            entities.pop(entity_id, None)

        for update in entity_upserts:
            if not isinstance(update, dict):
                raise ProtocolError(1002, "entity_upserts entries must be objects")
            entity_id = update.get("entity_id")
            if not isinstance(entity_id, str) or not entity_id:
                raise ProtocolError(1002, "entity_upserts requires entity_id")
            merged = (
                _merge_entity(entities[entity_id], update)
                if entity_id in entities
                else deepcopy(update)
            )
            EntityState.from_dict(merged)
            entities[entity_id] = merged

        for flow_id in flow_removals:
            if not isinstance(flow_id, str) or not flow_id:
                raise ProtocolError(1002, "flow_removals must contain IDs")
            flows.pop(flow_id, None)

        for update in flow_upserts:
            if not isinstance(update, dict):
                raise ProtocolError(1002, "flow_upserts entries must be objects")
            flow_id = update.get("flow_id")
            if not isinstance(flow_id, str) or not flow_id:
                raise ProtocolError(1002, "flow_upserts requires flow_id")
            merged = {**flows.get(flow_id, {}), **deepcopy(update)}
            FlowConfig.from_dict(merged)
            flows[flow_id] = merged

        self._validate_references(entities, flows)
        self._entities = entities
        self._flows = flows
        self._timestamp_ms = timestamp_ms
        self._revision += 1

    def snapshot(self) -> StateSnapshot:
        if not self._initialized:
            raise ProtocolError(1002, "state is not initialized")
        entities = tuple(
            EntityState.from_dict(self._entities[key])
            for key in sorted(self._entities)
        )
        flows = tuple(
            FlowConfig.from_dict(self._flows[key])
            for key in sorted(self._flows)
        )
        return StateSnapshot(
            timestamp_ms=self._timestamp_ms,
            entities=entities,
            flows=flows,
            revision=self._revision,
        )

    @staticmethod
    def _validate_references(
        entities: dict[str, dict[str, Any]],
        flows: dict[str, dict[str, Any]],
    ) -> None:
        parsed_entities = {
            entity_id: EntityState.from_dict(raw)
            for entity_id, raw in entities.items()
        }
        for entity in parsed_entities.values():
            if (
                entity.parent_entity_id is not None
                and entity.parent_entity_id not in parsed_entities
            ):
                raise ProtocolError(
                    1002,
                    f"unknown parent_entity_id: {entity.parent_entity_id}",
                )
            for device in entity.devices:
                if device.peer_entity_id not in parsed_entities:
                    raise ProtocolError(
                        1002,
                        f"unknown peer_entity_id: {device.peer_entity_id}",
                    )
            for policy in entity.effect_policies:
                if policy.peer_entity_id not in parsed_entities:
                    raise ProtocolError(
                        1002,
                        f"unknown policy peer_entity_id: {policy.peer_entity_id}",
                    )

        for raw in flows.values():
            flow = FlowConfig.from_dict(raw)
            if flow.source_entity_id not in parsed_entities:
                raise ProtocolError(
                    1002,
                    f"unknown flow source: {flow.source_entity_id}",
                )
            if flow.target_entity_id not in parsed_entities:
                raise ProtocolError(
                    1002,
                    f"unknown flow target: {flow.target_entity_id}",
                )
