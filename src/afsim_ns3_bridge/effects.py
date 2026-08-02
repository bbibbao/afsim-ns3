from __future__ import annotations

from typing import Any

from .state import StateSnapshot


def evaluate_effects(
    snapshot: StateSnapshot,
    metrics: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Convert measured network quality into explicit AFSIM subsystem states."""

    by_direction = {
        (
            metric["source_entity_id"],
            metric["target_entity_id"],
        ): metric
        for metric in metrics
    }
    effects: list[dict[str, Any]] = []

    for entity in snapshot.entities:
        for policy in entity.effect_policies:
            metric = by_direction.get(
                (entity.entity_id, policy.peer_entity_id)
            )
            reasons: list[str] = []
            state = "AVAILABLE"

            if metric is None:
                state = "BLOCKED"
                reasons.append("NO_NETWORK_METRIC")
            elif not metric["connected"]:
                state = "BLOCKED"
                reasons.append("DISCONNECTED")
            else:
                if metric["latency_ms"] > policy.max_delay_ms:
                    reasons.append("DELAY_EXCEEDED")
                if metric["loss_rate"] > policy.max_loss_rate:
                    reasons.append("LOSS_EXCEEDED")
                if metric["throughput_bps"] < policy.min_throughput_bps:
                    reasons.append("THROUGHPUT_BELOW_MINIMUM")
                if reasons:
                    state = policy.violation_state

            effects.append(
                {
                    "entity_id": entity.entity_id,
                    "business_node_id": entity.business_node_id,
                    "subsystem_id": policy.subsystem_id,
                    "subsystem_type": policy.subsystem_type,
                    "peer_entity_id": policy.peer_entity_id,
                    "state": state,
                    "reasons": reasons,
                    "thresholds": {
                        "max_delay_ms": policy.max_delay_ms,
                        "max_loss_rate": policy.max_loss_rate,
                        "min_throughput_bps": policy.min_throughput_bps,
                        "violation_state": policy.violation_state,
                    },
                }
            )

    return effects
