import json
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from codexbar_companion.projection import project_usage, token_summary


UTC = timezone.utc
NOW = datetime(2026, 8, 21, 12, 0, tzinfo=UTC)


def window(percent, minutes=None, reset="2026-08-21T15:00:00Z"):
    result = {"usedPercent": percent, "resetsAt": reset}
    if minutes is not None:
        result["windowMinutes"] = minutes
    return result


def entry(provider, updated_at="2026-08-21T12:00:00Z", **fields):
    result = {"provider": provider, "updatedAt": updated_at}
    result.update(fields)
    return result


class ProjectionTests(unittest.TestCase):
    def test_projects_firmware_usage_shape_and_supported_periods(self):
        snapshot = {
            "generatedAt": "2026-08-21T12:00:00Z",
            "enabledProviders": ["codex", "claude", "grok"],
            "entries": [
                entry(
                    "codex",
                    primary=window(12.5, 300),
                    secondary=window(47.25, 10080),
                    accountEmail="must not cross the seam",
                ),
                entry(
                    "claude",
                    primary=window(31, 300),
                    secondary=window(62.5, 10080),
                    costUSD=99.0,
                ),
                entry("grok", primary=window(18.75, 10080)),
                entry("cursor", primary=window(99, 300)),
            ],
        }

        projected = project_usage(snapshot, NOW)

        self.assertEqual(projected["observedAt"], "2026-08-21T12:00:00Z")
        self.assertEqual([p["provider"] for p in projected["providers"]], ["codex", "claude", "xai"])
        self.assertEqual(projected["providers"][0]["windows"], [
            {"period": "five_hour", "usedPercent": 12.5, "resetAt": "2026-08-21T15:00:00Z"},
            {"period": "weekly", "usedPercent": 47.25, "resetAt": "2026-08-21T15:00:00Z"},
        ])
        self.assertEqual(projected["providers"][1]["status"], "available")
        self.assertEqual(projected["providers"][2]["windows"], [
            {"period": "weekly", "usedPercent": 18.75, "resetAt": "2026-08-21T15:00:00Z"},
        ])
        self.assertNotIn("accountEmail", json.dumps(projected))
        self.assertNotIn("costUSD", json.dumps(projected))

    def test_grok_weekly_primary_label_is_authoritative(self):
        snapshot = {
            "enabledProviders": ["grok"],
            "entries": [
                entry(
                    "grok",
                    primary={"usedPercent": 23.5, "resetsAt": "2026-08-22T12:00:00Z"},
                    usageRows=[{"id": "primary", "title": "Weekly", "percentLeft": 76.5}],
                )
            ],
        }

        xai = project_usage(snapshot, NOW)["providers"][2]

        self.assertEqual(xai["provider"], "xai")
        self.assertEqual(xai["status"], "available")
        self.assertEqual(xai["windows"], [
            {"period": "weekly", "usedPercent": 23.5, "resetAt": "2026-08-22T12:00:00Z"},
        ])

    def test_grok_without_authoritative_duration_is_unavailable(self):
        snapshot = {
            "enabledProviders": ["grok"],
            "entries": [
                entry(
                    "grok",
                    primary={"usedPercent": 23.5, "resetsAt": "2026-08-22T12:00:00Z"},
                    usageRows=[{"id": "primary", "title": "Monthly", "percentLeft": 76.5}],
                )
            ],
        }

        xai = project_usage(snapshot, NOW)["providers"][2]

        self.assertEqual(xai["status"], "unavailable")
        self.assertEqual(xai["windows"], [])

    def test_stale_is_strictly_after_threshold_and_disabled_rows_are_valid(self):
        snapshot = {
            "enabledProviders": ["claude"],
            "entries": [
                entry(
                    "claude",
                    updated_at="2026-08-21T11:44:59Z",
                    primary=window(25, 300),
                )
            ],
        }

        projected = project_usage(snapshot, NOW)
        codex, claude, xai = projected["providers"]

        self.assertEqual(codex["status"], "unavailable")
        self.assertEqual(codex["observedAt"], "2026-08-21T12:00:00Z")
        self.assertEqual(claude["status"], "stale")
        self.assertEqual(claude["observedAt"], "2026-08-21T11:44:59Z")
        self.assertEqual(xai["status"], "unavailable")

        exact = project_usage(
            {"enabledProviders": ["claude"], "entries": [
                entry("claude", updated_at="2026-08-21T11:45:00Z", primary=window(25, 300))
            ]},
            NOW,
            stale_seconds=900,
        )
        self.assertEqual(exact["providers"][1]["status"], "available")

        at_boundary = project_usage(
            {"enabledProviders": ["claude"], "entries": [
                entry("claude", updated_at="2026-08-21T11:45:01Z", primary=window(25, 300))
            ]},
            NOW,
            stale_seconds=899,
        )
        self.assertEqual(at_boundary["providers"][1]["status"], "available")

    def test_missing_or_unsupported_windows_are_not_zero_filled(self):
        snapshot = {
            "enabledProviders": ["codex", "claude"],
            "entries": [
                entry("codex", primary={"usedPercent": 0, "windowMinutes": 1440}),
                entry("claude", primary={"usedPercent": 0, "windowMinutes": 300}),
            ],
        }

        projected = project_usage(snapshot, NOW)

        self.assertEqual(projected["providers"][0]["windows"], [])
        self.assertEqual(projected["providers"][0]["status"], "unavailable")
        self.assertEqual(projected["providers"][1]["windows"], [])
        self.assertEqual(projected["providers"][1]["status"], "unavailable")

    def test_invalid_percent_and_reset_are_rejected_without_nan(self):
        snapshot = {
            "enabledProviders": ["codex", "claude"],
            "entries": [
                entry("codex", primary=window(float("nan"), 300)),
                entry("claude", primary=window(12, 300, reset="not-a-timestamp")),
            ],
        }

        projected = project_usage(snapshot, NOW)
        encoded = json.dumps(projected, allow_nan=False)

        self.assertEqual(projected["providers"][0]["status"], "error")
        self.assertEqual(projected["providers"][0]["windows"], [])
        self.assertEqual(projected["providers"][1]["status"], "error")
        self.assertEqual(projected["providers"][1]["windows"], [])
        self.assertNotIn("NaN", encoded)

    def test_token_summary_uses_fresh_selected_provider_totals(self):
        snapshot = {
            "enabledProviders": ["codex", "claude", "grok", "cursor"],
            "entries": [
                entry(
                    "codex",
                    tokenUsage={
                        "updatedAt": "2026-08-21T11:59:00Z",
                        "sessionTokens": 1_500_000,
                        "last30DaysTokens": 2_000_000,
                    },
                ),
                entry(
                    "claude",
                    tokenUsage={
                        "updatedAt": "2026-08-21T12:00:00Z",
                        "sessionTokens": 250_000,
                        "last30DaysTokens": 3_000_000,
                    },
                ),
                entry(
                    "grok",
                    tokenUsage={
                        "updatedAt": "2026-08-21T11:58:00Z",
                        "last30DaysTokens": 4_000_000,
                    },
                ),
                entry(
                    "cursor",
                    tokenUsage={
                        "updatedAt": "2026-08-21T12:00:00Z",
                        "sessionTokens": 900_000_000,
                        "last30DaysTokens": 900_000_000,
                    },
                ),
            ],
        }

        self.assertEqual(token_summary(snapshot, NOW), "Today 1.8M | 30d 9.0M")

    def test_token_summary_drops_stale_and_invalid_token_usage(self):
        snapshot = {
            "enabledProviders": ["codex", "claude"],
            "entries": [
                entry(
                    "codex",
                    tokenUsage={
                        "updatedAt": "2026-08-21T11:00:00Z",
                        "sessionTokens": 500_000,
                        "last30DaysTokens": 500_000,
                    },
                ),
                entry(
                    "claude",
                    tokenUsage={
                        "updatedAt": "2026-08-21T12:00:00Z",
                        "sessionTokens": -1,
                        "last30DaysTokens": "unknown",
                    },
                ),
            ],
        }

        self.assertEqual(token_summary(snapshot, NOW), "")

    def test_oversized_numeric_values_degrade_without_publisher_crash(self):
        huge = 10**1000
        snapshot = {
            "enabledProviders": ["codex", "claude"],
            "entries": [
                entry("codex", primary=window(huge, 300)),
                entry(
                    "claude",
                    tokenUsage={
                        "updatedAt": "2026-08-21T12:00:00Z",
                        "sessionTokens": huge,
                        "last30DaysTokens": 42,
                    },
                ),
            ],
        }

        projected = project_usage(snapshot, NOW)

        self.assertEqual(projected["providers"][0]["status"], "error")
        self.assertEqual(projected["providers"][0]["windows"], [])
        self.assertEqual(token_summary(snapshot, NOW), "30d 42")
        json.dumps(projected, allow_nan=False)

    def test_future_observation_is_an_error_and_fallback_time_is_parseable(self):
        snapshot = {
            "enabledProviders": ["codex", "claude"],
            "entries": [
                entry("codex", updated_at="2026-08-21T12:00:01Z", primary=window(4, 300)),
                entry("claude", updated_at="bad", primary=window(4, 300)),
            ],
        }

        projected = project_usage(snapshot, NOW)

        self.assertEqual(projected["providers"][0]["status"], "error")
        self.assertEqual(projected["providers"][1]["status"], "error")
        self.assertEqual(projected["providers"][1]["observedAt"], "2026-08-21T12:00:00Z")


if __name__ == "__main__":
    unittest.main()
