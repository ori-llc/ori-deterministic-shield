import math
import unittest

from policy_guard import GuardConfig, PolicyGuard


class PolicyGuardTests(unittest.TestCase):
    def test_nominal_slew_and_ceiling_clamping(self):
        guard = PolicyGuard(GuardConfig(min_notional=0.0, max_notional=10000.0, max_step_delta=2500.0))

        d1 = guard.evaluate("rebalance_position", 6000.0, current_drawdown_pct=1.0)
        self.assertTrue(d1.allowed)
        self.assertTrue(d1.clamped)
        self.assertEqual(d1.approved_value, 2500.0)

        d2 = guard.evaluate("rebalance_position", 4000.0, current_drawdown_pct=1.0)
        self.assertTrue(d2.allowed)
        self.assertFalse(d2.clamped)
        self.assertEqual(d2.approved_value, 4000.0)

    def test_disallowed_tool_and_non_finite_proposal_rejected(self):
        guard = PolicyGuard()
        d_tool = guard.evaluate("wire_external_funds", 500.0)
        self.assertFalse(d_tool.allowed)
        self.assertEqual(d_tool.approved_value, 0.0)

        d_nan = guard.evaluate("rebalance_position", math.nan)
        self.assertFalse(d_nan.allowed)
        self.assertIn("NON_FINITE_PROPOSAL", d_nan.fault_flags)

    def test_drawdown_and_kill_switch_latch_and_safe_recovery(self):
        guard = PolicyGuard(GuardConfig(max_drawdown_pct=5.0))
        d_trip = guard.evaluate("set_target_weight", 1000.0, current_drawdown_pct=5.2)
        self.assertFalse(d_trip.allowed)
        self.assertEqual(d_trip.approved_value, 0.0)
        self.assertIn("MAX_DRAWDOWN_BREACHED", d_trip.fault_flags)

        # Still blocked inside recovery margin (>= 4.0%)
        self.assertFalse(guard.clear_faults(current_drawdown_pct=4.5))
        self.assertTrue(guard.clear_faults(current_drawdown_pct=2.0))

        # Kill switch immediately blocks and latches
        d_ks = guard.evaluate("set_target_weight", 500.0, current_drawdown_pct=1.0, kill_switch_active=True)
        self.assertFalse(d_ks.allowed)
        self.assertIn("KILL_SWITCH_LATCHED", d_ks.fault_flags)
        self.assertFalse(guard.clear_faults(current_drawdown_pct=1.0, kill_switch_active=True))
        self.assertTrue(guard.clear_faults(current_drawdown_pct=1.0, kill_switch_active=False))


if __name__ == "__main__":
    unittest.main()
