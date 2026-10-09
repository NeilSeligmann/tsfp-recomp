"""T1793 operator help contract."""

MUTATIONS = [
    {
        "id": "voice-help-return-limit",
        "file": "src/host/host_options.c",
        "old": "Return values unavailable.",
        "new": "Return values recorded.",
        "targets": ["test_host_options"],
        "why": "Observer must announce its entry-only return limitation.",
    }
]
