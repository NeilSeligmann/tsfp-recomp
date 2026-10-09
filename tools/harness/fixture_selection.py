"""Explicit provider membership and durable per-root source-run provenance."""

from collections.abc import Mapping, Sequence

SELECTION_FIELD = "fixture_provider_selection"


def selection(value: object) -> tuple[str, ...] | None:
    """None preserves legacy all-provider meaning; an empty selection disables all."""
    if value is None:
        return None
    if not isinstance(value, (list, tuple)) or any(
        not isinstance(label, str) or not label or label != label.strip() for label in value
    ):
        raise ValueError("fixture provider selection must contain nonempty labels")
    if len(set(value)) != len(value):
        raise ValueError("duplicate fixture provider selection")
    return tuple(value)


def select_labels(
    value: object, available: Sequence[str], *, disabled: bool = False
) -> tuple[str, ...] | None:
    labels = selection(value)
    if disabled:
        if labels is not None:
            raise ValueError("fixture provider selection conflicts with --no-fixture-providers")
        return ()
    if labels is None:
        return None
    unknown = set(labels) - set(available)
    if unknown:
        raise ValueError(f"unknown fixture provider selection: {sorted(unknown)}")
    return tuple(label for label in available if label in labels)


def validate_selection(document: Mapping[str, object]) -> None:
    """A row must retain its selected original run, including synthetic snapshots."""
    harness = document.get("harness")
    context = harness if isinstance(harness, dict) else document
    raw_runs = context.get("merged_runs", [])
    rows = document.get("functions")
    has_selection = (
        context.get(SELECTION_FIELD) is not None
        or (
            isinstance(rows, list)
            and any(isinstance(row, dict) and row.get(SELECTION_FIELD) is not None for row in rows)
        )
        or (
            isinstance(raw_runs, list)
            and any(
                isinstance(run, dict) and run.get(SELECTION_FIELD) is not None for run in raw_runs
            )
        )
    )
    if not has_selection:
        # No new channel: leave legacy scalar parsing and compatibility unchanged.
        return
    top = selection(context.get(SELECTION_FIELD))
    if not isinstance(raw_runs, list):
        raise ValueError("fixture selection source runs must be a list")
    runs = {}
    for run in raw_runs:
        if not isinstance(run, dict):
            raise ValueError("fixture selection source run must be an object")
        label = run.get("label")
        if not isinstance(label, str) or not label or label in runs:
            raise ValueError("duplicate or missing fixture selection source run label")
        runs[label] = selection(run.get(SELECTION_FIELD))
    if not isinstance(rows, list):
        raise ValueError("fixture selection functions must be a list")
    explicit = (
        top is not None
        or any(value is not None for value in runs.values())
        or any(isinstance(row, dict) and row.get(SELECTION_FIELD) is not None for row in rows)
    )
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("fixture selection function must be an object")
        actual = selection(row.get(SELECTION_FIELD))
        expected = top
        if runs and explicit:
            source = row.get("source_run")
            if not isinstance(source, str) or source not in runs:
                raise ValueError("missing fixture selection source run")
            expected = runs[source]
        if actual != expected:
            raise ValueError("inconsistent or missing per-root fixture provider selection")
