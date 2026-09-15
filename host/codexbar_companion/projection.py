"""Project CodexBar's widget snapshot into the firmware usage contract.

The widget snapshot is a provider-facing document.  The companion receives a
small, deliberately lossy view of it: only the three ring providers, explicit
rate-limit periods, percentages, reset timestamps, and freshness state cross
the boundary.  Generic UI labels are not period evidence.  Grok's provider
contract supplies a weekly period only for its exact primary usage row label;
other unlabeled or monthly rows remain unavailable to the xAI ring.
"""

from __future__ import annotations

from collections.abc import Mapping
from datetime import datetime, timezone
import math
from typing import Any


UTC = timezone.utc
DEFAULT_STALE_SECONDS = 900

_PROVIDER_ORDER = ("codex", "claude", "xai")
_PROVIDER_ALIASES = {"codex": "codex", "claude": "claude", "grok": "xai", "xai": "xai"}
_PERIOD_BY_MINUTES = {300: "five_hour", 10_080: "weekly", 43_200: "monthly"}
_ALLOWED_PERIODS = {
    "codex": frozenset(("five_hour", "weekly")),
    "claude": frozenset(("five_hour", "weekly")),
    "xai": frozenset(("weekly",)),
}
_PERIOD_ORDER = {"five_hour": 0, "weekly": 1, "monthly": 2}
_MAX_TOKEN_COUNT = 2**53 - 1
_MISSING = object()


def project_usage(
    snapshot: dict,
    now: datetime,
    stale_seconds: int = DEFAULT_STALE_SECONDS,
) -> dict:
    """Return the firmware ``usage`` object for one CodexBar snapshot.

    The returned object uses the exact field names required by the firmware
    decoder: ``provider``, ``observedAt``, ``usedPercent``, and ``resetAt``.
    Rows are always emitted in the canonical Codex, Claude, xAI order so a
    disabled or missing provider can be represented as an unavailable row.
    """

    current = _normalise_now(now)
    stale_after = _validate_stale_seconds(stale_seconds)
    source = snapshot if isinstance(snapshot, Mapping) else {}
    entries = _entries_by_provider(source.get("entries"))
    enabled = _enabled_providers(source, entries)

    generated_at = _parse_timestamp(source.get("generatedAt"))
    if generated_at is None or generated_at > current:
        generated_at = current

    providers = [
        _project_provider(
            provider,
            entries.get(provider),
            provider in enabled,
            current,
            stale_after,
        )
        for provider in _PROVIDER_ORDER
    ]
    return {"observedAt": _format_timestamp(generated_at), "providers": providers}


def token_summary(
    snapshot: dict,
    now: datetime,
    stale_seconds: int = DEFAULT_STALE_SECONDS,
) -> str:
    """Format fresh selected-provider token totals for a compact display.

    CodexBar's ``tokenUsage`` counters are preserved as reported.  Only
    ``sessionTokens`` (Today) and ``last30DaysTokens`` (30d) are used; no
    remaining quota is inferred, and Cursor is intentionally excluded from
    the aggregate to avoid overlapping provider accounting.
    """

    current = _normalise_now(now)
    stale_after = _validate_stale_seconds(stale_seconds)
    source = snapshot if isinstance(snapshot, Mapping) else {}
    entries = _entries_by_provider(source.get("entries"))
    enabled = _enabled_providers(source, entries)

    totals = {"sessionTokens": 0, "last30DaysTokens": 0}
    present = {"sessionTokens": False, "last30DaysTokens": False}
    for provider in _PROVIDER_ORDER:
        if provider not in enabled:
            continue
        entry = entries.get(provider)
        token_usage = entry.get("tokenUsage") if isinstance(entry, Mapping) else None
        if not isinstance(token_usage, Mapping):
            continue
        updated_at = _parse_timestamp(token_usage.get("updatedAt"))
        if updated_at is None or updated_at > current:
            continue
        if (current - updated_at).total_seconds() > stale_after:
            continue
        for field in totals:
            count = _token_count(token_usage.get(field))
            if count is None:
                continue
            totals[field] += count
            present[field] = True

    parts = []
    if present["sessionTokens"]:
        parts.append(f"Today {_format_token_count(totals['sessionTokens'])}")
    if present["last30DaysTokens"]:
        parts.append(f"30d {_format_token_count(totals['last30DaysTokens'])}")
    return " | ".join(parts)


def _project_provider(
    provider: str,
    entry: Mapping[str, Any] | None,
    enabled: bool,
    now: datetime,
    stale_after: float,
) -> dict[str, Any]:
    fallback = _format_timestamp(now)
    if not enabled or entry is None:
        return _provider_row(provider, "unavailable", [], fallback)

    observed_at = _parse_timestamp(entry.get("updatedAt"))
    if observed_at is None:
        return _provider_row(provider, "error", [], fallback)
    if observed_at > now:
        return _provider_row(provider, "error", [], fallback)

    windows, invalid = _project_windows(provider, entry)
    observed_text = _format_timestamp(observed_at)
    if invalid:
        return _provider_row(provider, "error", [], observed_text)
    if not windows:
        return _provider_row(provider, "unavailable", [], observed_text)

    age = (now - observed_at).total_seconds()
    status = "stale" if age > stale_after else "available"
    return _provider_row(provider, status, windows, observed_text)


def _provider_row(provider: str, status: str, windows: list[dict[str, Any]], observed_at: str) -> dict[str, Any]:
    return {
        "provider": provider,
        "status": status,
        "windows": windows,
        "observedAt": observed_at,
    }


def _project_windows(provider: str, entry: Mapping[str, Any]) -> tuple[list[dict[str, Any]], bool]:
    allowed = _ALLOWED_PERIODS[provider]
    projected: dict[str, dict[str, Any]] = {}
    invalid = False
    for source_window in _source_windows(provider, entry):
        period = _period_for(source_window)
        if period not in allowed:
            continue

        percent = _percent(source_window.get("usedPercent"))
        if percent is None:
            invalid = True
            continue

        raw_reset = source_window.get("resetsAt", _MISSING)
        if raw_reset is _MISSING:
            raw_reset = source_window.get("resetAt", _MISSING)
        if raw_reset is _MISSING or raw_reset is None:
            # A window without a reset cannot satisfy the firmware's required
            # RateWindow fields.  Let another duplicate source fill it in.
            continue
        reset_at = _parse_timestamp(raw_reset)
        if reset_at is None:
            invalid = True
            continue

        projected.setdefault(
            period,
            {
                "period": period,
                "usedPercent": percent,
                "resetAt": _format_timestamp(reset_at),
            },
        )

    return [projected[key] for key in sorted(projected, key=_PERIOD_ORDER.get)], invalid


def _source_windows(provider: str, entry: Mapping[str, Any]):
    # Direct slots are authoritative and are visited before usageRows, which
    # commonly repeat the same weekly window in a display-oriented wrapper.
    for field in ("primary", "secondary", "tertiary", "modelSpecific"):
        value = entry.get(field)
        if isinstance(value, Mapping):
            yield value

    # Grok's widget contract reports the primary rate as a bare object.  The
    # companion can use it only when CodexBar's provider-owned row identifies
    # that same object as exactly "Weekly"; arbitrary display labels do not
    # establish a rate-window period.
    if provider == "xai":
        primary = entry.get("primary")
        rows = entry.get("usageRows")
        if isinstance(primary, Mapping) and isinstance(rows, (list, tuple)):
            if any(
                isinstance(row, Mapping)
                and row.get("id") == "primary"
                and row.get("title") == "Weekly"
                for row in rows
            ):
                yield {**primary, "period": "weekly"}

    for field in ("extraWindows", "extraRateWindows"):
        values = entry.get(field)
        if not isinstance(values, (list, tuple)):
            continue
        for value in values:
            if not isinstance(value, Mapping):
                continue
            nested = value.get("window")
            if isinstance(nested, Mapping):
                yield nested
            elif "usedPercent" in value:
                yield value

    usage_rows = entry.get("usageRows")
    if not isinstance(usage_rows, (list, tuple)):
        return
    for row in usage_rows:
        if not isinstance(row, Mapping):
            continue
        nested = row.get("window")
        if isinstance(nested, Mapping):
            yield nested
        elif "usedPercent" in row:
            yield row


def _period_for(window: Mapping[str, Any]) -> str | None:
    explicit = window.get("period", _MISSING)
    if explicit is not _MISSING and explicit is not None:
        if not isinstance(explicit, str):
            return None
        value = explicit.strip().lower()
        return value if value in _PERIOD_ORDER else None

    minutes = window.get("windowMinutes", _MISSING)
    if isinstance(minutes, bool):
        return None
    if isinstance(minutes, int):
        return _PERIOD_BY_MINUTES.get(minutes)
    if isinstance(minutes, float) and math.isfinite(minutes) and minutes.is_integer():
        return _PERIOD_BY_MINUTES.get(int(minutes))
    return None


def _entries_by_provider(raw_entries: Any) -> dict[str, Mapping[str, Any]]:
    if not isinstance(raw_entries, (list, tuple)):
        return {}
    entries: dict[str, Mapping[str, Any]] = {}
    for entry in raw_entries:
        if not isinstance(entry, Mapping):
            continue
        provider = _canonical_provider(entry.get("provider"))
        if provider in _PROVIDER_ORDER:
            # Widget snapshots are intended to contain one selected source per
            # provider.  Keep the first occurrence rather than aggregating.
            entries.setdefault(provider, entry)
    return entries


def _enabled_providers(source: Mapping[str, Any], entries: Mapping[str, Mapping[str, Any]]) -> set[str]:
    if "enabledProviders" not in source:
        # Keep the pure projector useful for a minimal snapshot fixture while
        # still treating an explicit [] as all providers disabled.
        return set(entries)
    raw_enabled = source.get("enabledProviders")
    if not isinstance(raw_enabled, (list, tuple)):
        return set()
    return {
        provider
        for value in raw_enabled
        if (provider := _canonical_provider(value)) in _PROVIDER_ORDER
    }


def _canonical_provider(value: Any) -> str | None:
    return _PROVIDER_ALIASES.get(value) if isinstance(value, str) else None


def _percent(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        # Check the JSON integer in its native representation before converting
        # it; float(10**1000) raises OverflowError instead of returning inf.
        return float(value) if 0 <= value <= 100 else None
    if not isinstance(value, float):
        return None
    number = value
    if not math.isfinite(number) or number < 0 or number > 100:
        return None
    return number


def _token_count(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value if 0 <= value <= _MAX_TOKEN_COUNT else None
    if (isinstance(value, float) and math.isfinite(value) and 0 <= value <= _MAX_TOKEN_COUNT
            and value.is_integer()):
        return int(value)
    return None


def _format_token_count(value: int) -> str:
    if value < 1_000:
        return str(value)
    if value < 1_000_000:
        return f"{value / 1_000:.1f}K"
    if value < 1_000_000_000:
        return f"{value / 1_000_000:.1f}M"
    return f"{value / 1_000_000_000:.2f}B"


def _normalise_now(value: datetime) -> datetime:
    if not isinstance(value, datetime):
        raise TypeError("now must be a datetime")
    if value.tzinfo is None:
        return value.replace(tzinfo=UTC)
    if value.utcoffset() is None:
        return value.replace(tzinfo=UTC)
    return value.astimezone(UTC)


def _validate_stale_seconds(value: int) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("stale_seconds must be a non-negative finite number")
    threshold = float(value)
    if not math.isfinite(threshold) or threshold < 0:
        raise ValueError("stale_seconds must be a non-negative finite number")
    return threshold


def _parse_timestamp(value: Any) -> datetime | None:
    if isinstance(value, datetime):
        if value.tzinfo is None or value.utcoffset() is None:
            return None
        return value.astimezone(UTC)
    if not isinstance(value, str):
        return None
    text = value.strip()
    if len(text) < 20 or text[10:11] not in ("T", "t"):
        return None
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(UTC)


def _format_timestamp(value: datetime) -> str:
    value = value.astimezone(UTC)
    base = value.strftime("%Y-%m-%dT%H:%M:%S")
    if value.microsecond:
        fraction = f"{value.microsecond:06d}".rstrip("0")
        return f"{base}.{fraction}Z"
    return f"{base}Z"


__all__ = ["DEFAULT_STALE_SECONDS", "project_usage", "token_summary"]
