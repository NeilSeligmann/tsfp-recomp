# SPDX-License-Identifier: GPL-3.0-or-later
"""Inactive source-bound HUD Domain builder. Pure authentication only, no registry changes."""

from pathlib import Path

from tools.replace.manifest import ManifestEntry

from . import scoped_fixture_authority as authority
from . import t1479_signed_table_cases as family
from . import t1538_hud_slot_cases as hud
from . import t1538_scoped_recipe as recipe
from .effective_index import SelectedSources
from .image import build_guest_image
from .providers import REGISTRY, Provider
from .scoped_fixture_code import build_certificate
from .scoped_fixture_domain import Domain
from .seeding import SeedPolicy

BODY_SHA = "950f77e8a27dcb28162402e53f08d8191cb96aa5a0fa051698f7c98ff5bd0e4a"
IMAGE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
EXPORT_SHA = "05f426225c494ffd94a6b3150e02eb2a97f199f49c66e86bbd275a60f7dbd9ee"
SYMBOL = "game_hud_local_player_slot_state_not_0xc_or_0x11_and_flag_0x40_clear"
REPO = Path(__file__).resolve().parents[2]
PREFLIGHT_ORDINALS = (0, 419, 420, 421, 422, 423, 424, 425)


def build_domain(private: Path) -> tuple[Domain, Provider, SelectedSources]:
    """Capture actual loaded factories and independent streams under stable published API."""
    if authority.CUSTOM_RUNTIME_VALIDATED or authority.CONTRACTS:
        raise ValueError("predeclared pure stage requires custom capability FALSE/contracts EMPTY")
    image_path = private / "build/default.xbe"
    export = private / "generated/retail/functions.csv"
    if authority.digest(image_path.read_bytes()) != IMAGE_SHA:
        raise ValueError("original image changed from authenticated HUD declaration")
    if authority.digest(export.read_bytes()) != EXPORT_SHA:
        raise ValueError("original export changed from authenticated HUD declaration")
    selected = SelectedSources(
        export,
        REPO / "tools/data/function_overrides.csv",
        REPO / "tools/data/function_additions.csv",
    )
    policy = SeedPolicy()
    image = build_guest_image(image_path, policy=policy)
    if authority.digest(image.code_at(recipe.ROOT, recipe.SIZE)) != BODY_SHA:
        raise ValueError("actual HUD original body mismatch")
    dependencies = authority.Dependencies.capture(("tools", "docs/evidence/t1538"))
    source = authority.PhysicalSource.original(image_path, image, dependencies, policy)
    physical = authority.PhysicalPlan(
        source,
        selected.effective.table,
        tuple(authority.Region(*region) for region in recipe.REGIONS),
        policy,
    )
    certificate = build_certificate(
        image, selected.effective.table, recipe.ROOT, effective_source=selected.effective
    )
    if len(certificate.nodes) != 1:
        raise ValueError("authenticated HUD declaration requires exactly one leaf")
    factories = tuple(
        authority.FactoryIdentity.capture(
            recipe.ROOT, recipe.LABEL, recipe.NAMESPACE, factory, dependencies
        )
        for factory in (
            family.signed_table_case_count,
            family.make_signed_table_case,
            hud.hud_slot_case_count,
            hud.make_hud_slot_case,
        )
    )
    # Independent expected equations never call the factories being authenticated.
    streams = tuple(
        authority.FrozenStream(
            seed,
            recipe.ROOT,
            recipe.NAMESPACE,
            tuple(
                authority.ExpectedCase.capture(case, recipe.expected_roles(case))
                for ordinal in range(recipe.COUNT)
                for case in (recipe.expected_case(seed, ordinal),)
            ),
        )
        for seed in recipe.SEEDS
    )
    domain = Domain(
        ManifestEntry(
            recipe.ROOT,
            SYMBOL,
            "cdecl",
            0,
            "eax",
            (),
            "tools/harness/candidates/t1538_hud_slot.c",
        ),
        recipe.SIZE,
        recipe.LABEL,
        recipe.NAMESPACE,
        factories[0],
        factories[1],
        factories[2:],
        streams,
        physical,
        certificate,
    )
    matches = tuple(
        provider
        for provider in REGISTRY
        if provider.namespace == recipe.NAMESPACE and recipe.ROOT in provider.supported_vas
    )
    if len(matches) != 1:
        raise ValueError("actual loaded HUD provider selection ambiguous/missing")
    domain.validate()
    selected.check()
    return domain, matches[0], selected
