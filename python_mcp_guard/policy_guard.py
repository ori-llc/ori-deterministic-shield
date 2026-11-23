"""
ORI Deterministic Policy Guard (Track B — Software Companion)
License: Apache-2.0 (see LICENSE-APACHE)

Applies the exact same "AI Proposes, Deterministic Code Disposes" architecture used
on bare-metal ARM Cortex-M4F silicon to autonomous AI / MCP tool calls (e.g.,
quantitative risk limits, treasury/FX exposure caps, and automated compliance guards).
"""
from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass, field


@dataclass(frozen=True)
class GuardConfig:
    """Hard deterministic safety envelope for AI/MCP proposed numeric tool calls."""

    min_notional: float = 0.0
    max_notional: float = 10_000.0
    max_step_delta: float = 2_500.0
    max_drawdown_pct: float = 5.0
    allowed_tools: tuple[str, ...] = ("set_target_weight", "rebalance_position", "set_hedge_ratio")


@dataclass(frozen=True)
class GuardDecision:
    """Auditable deterministic verdict returned before any downstream execution."""

    allowed: bool
    clamped: bool
    proposed_value: float
    approved_value: float
    fault_flags: tuple[str, ...]
    audit_sha256: str
    reason: str


@dataclass
class PolicyGuard:
    """Stateful deterministic policy clamp with latching kill-switch & audit digest."""

    config: GuardConfig = field(default_factory=GuardConfig)
    last_approved: float = 0.0
    faults: set[str] = field(default_factory=set)

    def evaluate(
        self,
        tool_name: str,
        proposed_value: float,
        current_drawdown_pct: float = 0.0,
        kill_switch_active: bool = False,
    ) -> GuardDecision:
        if kill_switch_active:
            self.faults.add("KILL_SWITCH_LATCHED")

        if not math.isfinite(current_drawdown_pct) or current_drawdown_pct < 0.0:
            self.faults.add("INVALID_RISK_TELEMETRY")
        elif current_drawdown_pct >= self.config.max_drawdown_pct:
            self.faults.add("MAX_DRAWDOWN_BREACHED")

        if tool_name not in self.config.allowed_tools:
            return self._decision(
                allowed=False,
                clamped=True,
                proposed=proposed_value,
                approved=0.0,
                reason=f"Tool '{tool_name}' is outside the deterministic allowlist.",
            )

        if not isinstance(proposed_value, (int, float)) or isinstance(proposed_value, bool) or not math.isfinite(proposed_value):
            self.faults.add("NON_FINITE_PROPOSAL")
            self.last_approved = 0.0
            return self._decision(
                allowed=False,
                clamped=True,
                proposed=0.0,
                approved=0.0,
                reason="Rejected NaN/Inf/non-numeric AI proposal and latched fault.",
            )

        if self.faults:
            self.last_approved = 0.0
            return self._decision(
                allowed=False,
                clamped=True,
                proposed=float(proposed_value),
                approved=0.0,
                reason=f"Blocked by latched deterministic fault(s): {', '.join(sorted(self.faults))}.",
            )

        clamped = False
        val = float(proposed_value)
        if val < self.config.min_notional:
            val = self.config.min_notional
            clamped = True
        elif val > self.config.max_notional:
            val = self.config.max_notional
            clamped = True

        delta = val - self.last_approved
        if abs(delta) > self.config.max_step_delta:
            val = self.last_approved + math.copysign(self.config.max_step_delta, delta)
            clamped = True

        self.last_approved = val
        return self._decision(
            allowed=True,
            clamped=clamped,
            proposed=float(proposed_value),
            approved=val,
            reason="Clamped to deterministic envelope." if clamped else "Within deterministic envelope.",
        )

    def clear_faults(self, current_drawdown_pct: float, kill_switch_active: bool = False) -> bool:
        """Clears latched faults ONLY when physical/risk invariants are back inside safe margin."""
        if kill_switch_active or not math.isfinite(current_drawdown_pct) or current_drawdown_pct < 0.0:
            return False
        if current_drawdown_pct >= (self.config.max_drawdown_pct * 0.8):
            return False
        self.faults.clear()
        return True

    def _decision(
        self,
        allowed: bool,
        clamped: bool,
        proposed: float,
        approved: float,
        reason: str,
    ) -> GuardDecision:
        ordered_faults = tuple(sorted(self.faults))
        payload = json.dumps(
            {
                "allowed": allowed,
                "clamped": clamped,
                "proposed": proposed,
                "approved": approved,
                "faults": ordered_faults,
            },
            sort_keys=True,
        ).encode("utf-8")
        return GuardDecision(
            allowed=allowed,
            clamped=clamped,
            proposed_value=proposed,
            approved_value=approved,
            fault_flags=ordered_faults,
            audit_sha256=hashlib.sha256(payload).hexdigest()[:16],
            reason=reason,
        )
