# ruff: noqa: E501
"""T1502: the Poisson rate comparison shared by the action analyzers (tools/action_profile_diff.py, tools/action_census_diff.py).

Counts are events of a Poisson process observed for an exposure (seconds, presents or total samples). Two phases are
compared with the exact conditional test: given n = k_a + k_b events in total, k_a is Binomial(n, t_a / (t_a + t_b)) when
both rates are equal. The one sided p is the chance of k_a or more events under equal rates, z the normal score of the
same difference (for ranking, finite also when the other phase saw nothing).
"""

from __future__ import annotations

import math

EXACT_LIMIT = 5000


def binomial_tail(n: int, k: int, p: float) -> float:
    """P(X >= k) for X ~ Binomial(n, p), exact below EXACT_LIMIT events, normal approximation with continuity above."""
    if k <= 0:
        return 1.0
    if k > n:
        return 0.0
    if p <= 0.0:
        return 0.0
    if p >= 1.0:
        return 1.0
    if n <= EXACT_LIMIT:
        log_p, log_q = math.log(p), math.log1p(-p)
        terms = [
            math.lgamma(n + 1)
            - math.lgamma(i + 1)
            - math.lgamma(n - i + 1)
            + i * log_p
            + (n - i) * log_q
            for i in range(k, n + 1)
        ]
        top = max(terms)
        return min(1.0, math.exp(top) * sum(math.exp(term - top) for term in terms))
    mean, sd = n * p, math.sqrt(n * p * (1.0 - p))
    return 0.5 * math.erfc((k - 0.5 - mean) / (sd * math.sqrt(2.0)))


def rate_test(
    count: int, exposure: float, reference_count: int, reference_exposure: float
) -> tuple[float, float]:
    """(p, z) that the rate count/exposure exceeds reference_count/reference_exposure (p = 1, z = 0 when there is no event or no exposure)."""
    total = count + reference_count
    if count <= 0 or exposure <= 0.0 or reference_exposure < 0.0 or total <= 0:
        return 1.0, 0.0
    if reference_exposure == 0.0:
        return 0.0, math.inf
    share = exposure / (exposure + reference_exposure)
    p_value = binomial_tail(total, count, share)
    deviation = math.sqrt(total * share * (1.0 - share))
    z_score = (count - total * share) / deviation if deviation > 0.0 else 0.0
    return p_value, z_score
