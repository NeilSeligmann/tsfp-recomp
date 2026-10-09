# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit additive fixture domains, ordered exactly as the historical CLI loops.

New families add a standalone module and one plain row. No module discovery.
"""

from collections.abc import Callable, Iterator
from dataclasses import dataclass
from functools import partial

from . import animation_copy_cases as animation_copy
from . import animation_stream_cases as animation_stream
from . import bit_row_cases as bit_row
from . import call_leaf_cases as call_leaf
from . import call_record_cases as call_record
from . import callback_registration_cases as callback_registration
from . import color_index_cases as color_index
from . import cross_triplet_cases as cross_triplet
from . import fixed_record_cases as fixed_record
from . import flag_packer_cases as flag_packer
from . import free_slot_cases as free_slot
from . import generic_global_cases as generic_global
from . import global_object_flag_cases as global_object_flag
from . import indexed_field_cases as indexed_field
from . import int_array_cases as int_array
from . import nth_active_cases as nth_active
from . import object_mode_flag_cases as object_mode_flag
from . import pool_initializer_cases as pool_initializer
from . import request_slot_cases as request_slot
from . import signed_word_cases as signed_word
from . import slot_byte_cases as slot_byte
from . import slot_predicate_cases as slot_predicate
from . import slot_word_cases as slot_word
from . import string_cases as string
from . import t1475_batch2_cases as t1475_batch2
from . import t1475_batch3_cases as t1475_batch3
from . import t1475_continuation_cases as t1475_continuation
from . import t1476_chain_cases as t1476_chain
from . import t1476_deferred_cases as t1476_deferred
from . import t1476_table_lookup_cases as t1476_table_lookup
from . import t1477_batch4_cases as t1477_batch4
from . import t1477_crc_cases as t1477_crc
from . import t1477_followup_cases as t1477_followup
from . import t1477_row_cases as t1477_row
from . import t1478_batch4_cases as t1478_batch4
from . import t1478_batch5_cases as t1478_batch5
from . import t1478_batch6_cases as t1478_batch6
from . import t1478_batch7_cases as t1478_batch7
from . import t1478_call_cases as t1478_call
from . import t1478_followup_cases as t1478_followup
from . import t1478_nested_link_cases as t1478_nested_link
from . import t1478_predecessor_cases as t1478_predecessor
from . import t1478_scalar_cases as t1478_scalar
from . import t1479_actor_list_cases as t1479_actor_list
from . import t1479_batch3_cases as t1479_batch3
from . import t1479_batch4_cases as t1479_batch4
from . import t1479_batch5_cases as t1479_batch5
from . import t1479_cache_table_cases as t1479_cache_table
from . import t1479_net_cases as t1479_net
from . import t1479_scalar_cases as t1479_scalar
from . import t1479_signed_table_cases as t1479_signed_table
from . import t1480_case_conversion_cases as t1480_case_conversion
from . import t1480_code_flag_cases as t1480_code_flag
from . import t1480_ctype_cases as t1480_ctype
from . import t1480_eighth_cases as t1480_eighth
from . import t1480_fifth_cases as t1480_fifth
from . import t1480_hextodec_cases as t1480_hextodec
from . import t1480_seventh_cases as t1480_seventh
from . import t1480_sixth_cases as t1480_sixth
from . import t1510_locale_fold_cases as t1510_locale_fold
from . import t1537_editor_profile_family as t1537_editor_profile
from . import t1559_this_object_cases as t1559_this_object
from . import t1576_guarded_cases_a as t1576_guarded_a
from . import table_cases as table
from . import text_bank_cases as text_bank
from . import tls_cases as tls
from . import vector_matrix_cases as vector_matrix
from . import vector_scalar_cases as vector_scalar
from . import vertex_format_cases as vertex_format
from .fixture_selection import select_labels
from .model import Case
from .named_global_object_contracts import CONTRACTS
from .named_global_objects import LABEL as NAMED_GLOBAL_LABEL
from .named_global_objects import NAMESPACE as NAMED_GLOBAL_NAMESPACE
from .named_global_objects import Factory as NamedGlobalFactory
from .seeding import SeedPolicy


def fixed_count(count: int, va: int) -> int:
    """Count for single-size domains; Provider bounds the supported VA first."""
    return count


@dataclass(frozen=True, slots=True)
class Provider:
    namespace: int
    supported_vas: tuple[int, ...]
    count: Callable[[int], int]
    make: Callable[..., Case]
    label: str
    requires_vector: bool = False
    after_feedback: bool = False
    authenticate: Callable[..., dict[str, object]] | None = None
    bind: Callable[..., object] | None = None
    custom_contracts: tuple[object, ...] = ()

    def case_count(self, va: int) -> int:
        if va not in self.supported_vas:
            return 0
        count = self.count(va)
        if type(count) is not int or count <= 0 or count >= 1 << 40:
            raise ValueError(f"{self.label}: invalid fixture count")
        return count

    def case(
        self, seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
    ) -> Case:
        if type(ordinal) is not int or not 0 <= ordinal < self.case_count(va):
            raise ValueError(f"{self.label}: unsupported root or ordinal")
        case = self.make(seed, ordinal, va, size, policy=policy)
        identity = (self.label, self.make.__module__, self.make.__name__)
        shift = HISTORICAL_VA_INDEX_BITS.get(identity)
        expected_index = (
            (self.namespace << 40) + (va << shift if shift is not None else 0) + ordinal
        )
        if (case.seed, case.va, case.size) != (
            seed,
            va,
            size,
        ) or case.index != expected_index:
            raise ValueError(f"{self.label}: factory identity/namespace/ordinal mismatch")
        return case


# Exact legacy index recipes; every new family uses namespace-base + ordinal.
HISTORICAL_VA_INDEX_BITS = {
    ("string", "tools.harness.string_cases", "make_string_case"): 11,
    ("tls", "tools.harness.tls_cases", "make_tls_case"): 11,
    ("table", "tools.harness.table_cases", "make_table_case"): 11,
}


# Exact historical collisions, frozen by identity and disjoint VA domains.
# These preserve existing bytes; no third/new shared namespace is admitted.
HISTORICAL_SHARED_NAMESPACES = {
    24: frozenset(
        (
            ("color-index", "tools.harness.color_index_cases", "make_color_index_case"),
            ("vector-matrix", "tools.harness.vector_matrix_cases", "make_matrix_call_case"),
        )
    ),
    25: frozenset(
        (
            ("flag-packer", "tools.harness.flag_packer_cases", "make_flag_packer_case"),
            ("bit-row", "tools.harness.bit_row_cases", "make_bit_row_case"),
        )
    ),
    26: frozenset(
        (
            ("fixed-record", "tools.harness.fixed_record_cases", "make_fixed_record_case"),
            ("call-leaf", "tools.harness.call_leaf_cases", "make_call_leaf_case"),
        )
    ),
    40: frozenset(
        (
            ("t1480-code-flag", "tools.harness.t1480_code_flag_cases", "make_code_flag_case"),
            ("text-bank", "tools.harness.text_bank_cases", "make_text_bank_case"),
        )
    ),
}


def validate_registry(rows: tuple[Provider, ...]) -> None:
    labels: set[str] = set()
    namespaces: dict[int, list[Provider]] = {}
    for row in rows:
        if type(row.namespace) is not int or row.namespace < 3 or row.namespace == 4:
            raise ValueError("invalid/reserved fixture namespace")
        if not isinstance(row.label, str) or not row.label or row.label in labels:
            raise ValueError("invalid/duplicate fixture label")
        labels.add(row.label)
        if (
            type(row.supported_vas) is not tuple
            or not row.supported_vas
            or any(type(va) is not int or not 0 < va < 1 << 32 for va in row.supported_vas)
            or len(set(row.supported_vas)) != len(row.supported_vas)
        ):
            raise ValueError(f"{row.label}: invalid/duplicate supported guest VA")
        if (
            type(row.requires_vector) is not bool
            or type(row.after_feedback) is not bool
            or not callable(row.count)
            or not callable(row.make)
            or (row.authenticate is not None and not callable(row.authenticate))
        ):
            raise ValueError(f"{row.label}: invalid provider contract")
        if type(row.custom_contracts) is not tuple:
            raise ValueError("immutable custom fixture declarations required")
        if row.custom_contracts:
            from .scoped_fixture_domain import Domain

            roots = set()
            for domain in row.custom_contracts:
                if type(domain) is not Domain:
                    raise ValueError("exact custom fixture Domain required")
                if (
                    domain.entry.va not in row.supported_vas
                    or domain.entry.va in roots
                    or domain.label != row.label
                    or domain.namespace != row.namespace
                    or row.authenticate is not None
                    or row.bind is not None
                    or row.requires_vector
                ):
                    raise ValueError("mixed/duplicate custom fixture authority")
                roots.add(domain.entry.va)
                domain.validate()
        if row.namespace == NAMED_GLOBAL_NAMESPACE:
            factory = getattr(row.make, "__self__", None)
            if (
                type(factory) is not NamedGlobalFactory
                or row.label != NAMED_GLOBAL_LABEL
                or row.supported_vas != factory.supported_vas
                or not row.after_feedback
                or row.requires_vector
                or getattr(row.count, "__self__", None) is not factory
                or getattr(row.authenticate, "__self__", None) is not factory
                or getattr(row.bind, "__self__", None) is not factory
            ):
                raise ValueError(
                    "named graph provider requires its immutable authenticated factory"
                )
        for va in row.supported_vas:
            row.case_count(va)
        peers = namespaces.setdefault(row.namespace, [])
        if peers:
            identities = frozenset(
                (peer.label, peer.make.__module__, peer.make.__name__) for peer in (*peers, row)
            )
            if identities != HISTORICAL_SHARED_NAMESPACES.get(row.namespace):
                raise ValueError("duplicate fixture namespace outside exact historical pair")
            if any(set(peer.supported_vas) & set(row.supported_vas) for peer in peers):
                raise ValueError("ambiguous duplicate namespace/guest VA")
        peers.append(row)


def named_global_provider(factory: NamedGlobalFactory) -> Provider:
    """One ordinary additive provider for a fully declared graph set."""
    if not factory.domains:
        raise ValueError("cannot register an empty named graph provider")
    return Provider(
        NAMED_GLOBAL_NAMESPACE,
        factory.supported_vas,
        factory.count,
        factory.make,
        NAMED_GLOBAL_LABEL,
        after_feedback=True,
        authenticate=factory.authenticate,
        bind=factory.bind,
    )


REGISTRY = (
    Provider(
        3,
        (0x003C8690, 0x003C8960, 0x003C94F0, 0x003C9546, 0x003C9C4E, 0x003C9F80, 0x003CA6C0),
        partial(fixed_count, 512),
        string.make_string_case,
        "string",
    ),
    Provider(5, (0x0037E9A7,), partial(fixed_count, 256), tls.make_tls_case, "tls"),
    Provider(
        6,
        (0x001CF640, 0x002FDF50, 0x00311C00),
        partial(fixed_count, 512),
        table.make_table_case,
        "table",
    ),
    Provider(
        8,
        (0x000591D0,),
        partial(fixed_count, 512),
        animation_copy.make_animation_copy_case,
        "animation-copy",
    ),
    Provider(
        7,
        (0x00060390,),
        partial(fixed_count, 512),
        animation_stream.make_animation_stream_case,
        "animation-stream",
    ),
    Provider(
        26,
        (0x00356290,),
        partial(fixed_count, 512),
        fixed_record.make_fixed_record_case,
        "fixed-record",
    ),
    Provider(
        25,
        (0x0031DCD0,),
        partial(fixed_count, 512),
        flag_packer.make_flag_packer_case,
        "flag-packer",
    ),
    Provider(
        24,
        (0x003194E0,),
        partial(fixed_count, 512),
        color_index.make_color_index_case,
        "color-index",
    ),
    Provider(
        23, (0x00070C90,), partial(fixed_count, 640), int_array.make_int_array_case, "int-array"
    ),
    Provider(
        22,
        (0x00064D60,),
        partial(fixed_count, 512),
        signed_word.make_signed_word_case,
        "signed-word",
    ),
    Provider(
        21, (0x00025B80,), partial(fixed_count, 512), slot_byte.make_slot_byte_case, "slot-byte"
    ),
    Provider(
        20,
        (0x0001E8D0,),
        partial(fixed_count, 640),
        vertex_format.make_vertex_format_case,
        "vertex-format",
    ),
    Provider(
        35,
        (0x001EC8E0, 0x0024D260, 0x00259800),
        t1478_followup.t1478_followup_case_count,
        t1478_followup.make_t1478_followup_case,
        "t1478-followup",
    ),
    Provider(42, (0x00160B80,), t1477_row.row_case_count, t1477_row.make_row_case, "t1477-row"),
    Provider(
        37,
        (0x00190690, 0x001CD560, 0x0022DE90),
        t1478_scalar.t1478_scalar_case_count,
        t1478_scalar.make_t1478_scalar_case,
        "t1478-scalar",
    ),
    Provider(
        44,
        (
            0x001829D0,
            0x00190A70,
            0x00190A80,
            0x001DE560,
            0x001EC980,
            0x001EC9C0,
            0x001ECA00,
            0x00240560,
            0x00249880,
        ),
        t1478_batch4.t1478_batch4_case_count,
        t1478_batch4.make_t1478_batch4_case,
        "t1478-batch4",
    ),
    Provider(
        46,
        (0x001CD2B0, 0x001CD380, 0x001CD590, 0x00208AC0, 0x00235600, 0x0025EF30),
        t1478_batch5.t1478_batch5_case_count,
        t1478_batch5.make_t1478_batch5_case,
        "t1478-batch5",
    ),
    Provider(
        48,
        (
            0x001B7E80,
            0x002291E0,
            0x002346A0,
            0x00247A10,
            0x00248910,
            0x0025F320,
            0x00275640,
            0x00275EF0,
        ),
        t1478_batch6.t1478_batch6_case_count,
        t1478_batch6.make_t1478_batch6_case,
        "t1478-batch6",
    ),
    Provider(
        100,
        (0x001DE540, 0x0022DE00),
        t1478_batch7.t1478_batch7_case_count,
        t1478_batch7.make_t1478_batch7_case,
        "t1478-batch7",
    ),
    Provider(
        32,
        (0x0018AC60, 0x00190AC0),
        t1478_call.t1478_call_case_count,
        t1478_call.make_t1478_call_case,
        "t1478-call",
    ),
    Provider(
        29,
        (0x000657A0, 0x00071A60),
        call_record.call_record_case_count,
        call_record.make_call_record_case,
        "call-record",
    ),
    Provider(
        26,
        (0x00018CE0, 0x000425A0),
        call_leaf.call_leaf_case_count,
        call_leaf.make_call_leaf_case,
        "call-leaf",
    ),
    Provider(
        28,
        (0x00150530,),
        t1477_crc.crc_string_case_count,
        t1477_crc.make_crc_string_case,
        "crc-string",
    ),
    Provider(
        31,
        (0x00115F10, 0x00150560),
        t1477_followup.followup_case_count,
        t1477_followup.make_followup_case,
        "t1477-followup",
    ),
    Provider(
        36,
        (0x003CAD32, 0x003CD157, 0x003CF214),
        t1480_case_conversion.conversion_case_count,
        t1480_case_conversion.make_conversion_case,
        "t1480-conversion",
    ),
    Provider(
        39,
        (0x003CDA3C,),
        t1480_hextodec.hextodec_case_count,
        t1480_hextodec.make_hextodec_case,
        "t1480-hextodec",
    ),
    Provider(
        34,
        (0x003C887E, 0x003C88AC, 0x003C88D5, 0x003C88FE, 0x003C892C),
        t1480_ctype.ctype_case_count,
        t1480_ctype.make_ctype_case,
        "t1480-ctype",
    ),
    Provider(
        40,
        (0x003E6486,),
        t1480_code_flag.code_flag_case_count,
        t1480_code_flag.make_code_flag_case,
        "t1480-code-flag",
    ),
    Provider(
        45,
        (0x003CAB7B, 0x0044B4AE),
        t1480_fifth.fifth_case_count,
        t1480_fifth.make_fifth_case,
        "t1480-fifth",
    ),
    Provider(
        47,
        (0x0008E920, 0x00094390, 0x00094410, 0x004098CE),
        t1480_sixth.sixth_case_count,
        t1480_sixth.make_sixth_case,
        "t1480-sixth",
    ),
    Provider(
        81,
        (0x0008E920,),
        t1476_chain.chain_case_count,
        t1476_chain.make_chain_case,
        "t1476-chain",
    ),
    Provider(
        49,
        (0x000DB480, 0x003C3C50, 0x003C3D20),
        t1480_seventh.seventh_case_count,
        t1480_seventh.make_seventh_case,
        "t1480-seventh",
    ),
    Provider(
        61,
        (0x00045E60, 0x000606E0, 0x00060740, 0x0006B4E0, 0x0006BB90, 0x0017A190),
        t1475_batch2.batch2_case_count,
        t1475_batch2.make_batch2_case,
        "t1475-batch2",
    ),
    Provider(
        62,
        (0x00042640,),
        t1475_batch3.batch3_case_count,
        t1475_batch3.make_batch3_case,
        "t1475-batch3",
    ),
    Provider(
        63,
        (0x0015C4E0,),
        t1477_batch4.batch4_case_count,
        t1477_batch4.make_batch4_case,
        "t1477-batch4",
    ),
    Provider(
        60,
        (0x000406A0, 0x00061EE0),
        t1475_continuation.continuation_case_count,
        t1475_continuation.make_continuation_case,
        "t1475-continuation",
    ),
    Provider(
        41,
        (0x000AF330,),
        t1476_table_lookup.table_lookup_case_count,
        t1476_table_lookup.make_table_lookup_case,
        "t1476-table-lookup",
    ),
    Provider(
        25,
        (0x00066280, 0x000662D0, 0x000662F0),
        bit_row.bit_row_case_count,
        bit_row.make_bit_row_case,
        "bit-row",
    ),
    Provider(
        27,
        (0x000AE8B0, 0x000AEA20, 0x000B2FA0, 0x000C3510, 0x000D9210),
        callback_registration.callback_case_count,
        callback_registration.make_callback_case,
        "callback-registration",
    ),
    Provider(
        30,
        (0x000AE910,),
        pool_initializer.pool_initializer_case_count,
        pool_initializer.make_pool_initializer_case,
        "pool-initializer",
    ),
    Provider(
        40,
        (0x00082860,),
        text_bank.text_bank_case_count,
        text_bank.make_text_bank_case,
        "text-bank",
    ),
    Provider(
        56,
        (0x002B2390, 0x002C3390, 0x002E6F80, 0x002FE0F0, 0x00321560, 0x0033ACB0),
        t1479_batch5.batch5_case_count,
        t1479_batch5.make_batch5_case,
        "t1479-batch5",
    ),
    Provider(
        55,
        (
            0x002875F0,
            0x002BE5E0,
            0x002CB1E0,
            0x002D74B0,
            0x002E4470,
            0x002E4C10,
            0x002E4CB0,
            0x003381D0,
            0x00361950,
            0x0036EBE0,
        ),
        t1479_batch4.batch4_case_count,
        t1479_batch4.make_batch4_case,
        "t1479-batch4",
    ),
    Provider(
        54,
        (
            0x002BE9B0,
            0x002D28F0,
            0x002E7270,
            0x00356080,
            0x003560E0,
            0x00356870,
            0x00358130,
            0x00358160,
            0x00358560,
            0x00359910,
            0x0035AFB0,
            0x0035B140,
        ),
        t1479_batch3.batch3_case_count,
        t1479_batch3.make_batch3_case,
        "t1479-batch3",
    ),
    Provider(
        43,
        (0x002BFEC0, 0x002EF160, 0x003568C0, 0x00356990, 0x0035E6A0, 0x0035EAF0),
        t1479_net.t1479_net_case_count,
        t1479_net.make_t1479_net_case,
        "t1479-net",
    ),
    Provider(
        33,
        (0x00356320, 0x0035E630),
        t1479_scalar.t1479_scalar_case_count,
        t1479_scalar.make_t1479_scalar_case,
        "t1479-scalar",
    ),
    Provider(
        19, (0x00038FD0,), partial(fixed_count, 640), nth_active.make_nth_active_case, "nth-active"
    ),
    Provider(
        18, (0x00031040,), partial(fixed_count, 640), free_slot.make_free_slot_case, "free-slot"
    ),
    Provider(
        16,
        (0x00024E60,),
        partial(fixed_count, 512),
        slot_predicate.make_slot_predicate_case,
        "slot-predicate",
    ),
    Provider(
        15, (0x0001E750,), partial(fixed_count, 512), slot_word.make_slot_word_case, "slot-word"
    ),
    Provider(
        14,
        (0x00018000,),
        partial(fixed_count, 512),
        cross_triplet.make_cross_triplet_case,
        "cross-triplet",
    ),
    Provider(
        13,
        (0x00012EC0,),
        partial(fixed_count, 512),
        global_object_flag.make_global_object_flag_case,
        "global-object-flag",
    ),
    Provider(
        24,
        (0x00167AB0,),
        partial(fixed_count, 256),
        vector_matrix.make_matrix_call_case,
        "vector-matrix",
        requires_vector=True,
    ),
    Provider(
        38,
        (0x0009E260,),
        partial(fixed_count, 576),
        vector_scalar.make_scalar_call_case,
        "vector-scalar",
        requires_vector=True,
    ),
    Provider(
        12,
        (0x00012BA0,),
        partial(fixed_count, 512),
        indexed_field.make_indexed_field_case,
        "indexed-field",
    ),
    Provider(
        11,
        (0x000174E0,),
        partial(fixed_count, 512),
        object_mode_flag.make_object_mode_flag_case,
        "object-mode-flag",
    ),
    Provider(
        9,
        (0x000294B0,),
        partial(fixed_count, 512),
        request_slot.make_request_slot_case,
        "request-slot",
    ),
    Provider(
        70,
        (0x003BC2A0, 0x0038F565, 0x00417ADA),
        t1480_eighth.eighth_case_count,
        t1480_eighth.make_eighth_case,
        "t1480-eighth",
    ),
    Provider(
        71,
        (0x0009D740, 0x0009D780),
        t1476_deferred.deferred_case_count,
        t1476_deferred.make_deferred_case,
        "t1476-deferred",
    ),
    Provider(
        73,
        (0x001BA1B0, 0x001BFA60),
        t1478_predecessor.predecessor_case_count,
        t1478_predecessor.make_predecessor_case,
        "t1478-predecessor",
    ),
    Provider(
        74,
        (0x00272430,),
        t1478_nested_link.nested_link_case_count,
        t1478_nested_link.make_nested_link_case,
        "t1478-nested-link",
    ),
    Provider(
        75,
        (0x002FBF70, 0x003158C0),
        t1479_actor_list.actor_list_case_count,
        t1479_actor_list.make_actor_list_case,
        "t1479-actor-list",
    ),
    Provider(
        77,
        t1537_editor_profile.SUPPORTED_VAS,
        t1537_editor_profile.editor_profile_family_count,
        t1537_editor_profile.make_editor_profile_family_case,
        "t1479-profile-list",
    ),
    Provider(
        78,
        (0x00350DC0,),
        t1479_cache_table.cache_table_case_count,
        t1479_cache_table.make_cache_table_case,
        "t1479-cache-table",
    ),
    Provider(
        79,
        (0x002D2C10, 0x002D2C40, 0x00338150, 0x002D2BE0, 0x00139160),
        t1479_signed_table.signed_table_case_count,
        t1479_signed_table.make_signed_table_case,
        "t1479-signed-table",
    ),
    Provider(
        80,
        generic_global.SUPPORTED_VAS,
        generic_global.global_case_count,
        generic_global.make_global_case,
        "generic-global-data",
        after_feedback=True,
    ),
    Provider(
        t1559_this_object.NAMESPACE,
        tuple(t1559_this_object.ROOT_SIZES),
        t1559_this_object.this_object_case_count,
        t1559_this_object.make_this_object_case,
        t1559_this_object.LABEL,
        after_feedback=True,
    ),
    Provider(
        t1510_locale_fold.NAMESPACE,
        (t1510_locale_fold.ROOT,),
        t1510_locale_fold.locale_fold_case_count,
        t1510_locale_fold.make_locale_fold_case,
        t1510_locale_fold.LABEL,
    ),
    Provider(
        t1576_guarded_a.NAMESPACE,
        t1576_guarded_a.SUPPORTED_VAS,
        t1576_guarded_a.guarded_case_count,
        t1576_guarded_a.make_guarded_case,
        t1576_guarded_a.LABEL,
    ),
)
if CONTRACTS:
    REGISTRY = (*REGISTRY, named_global_provider(NamedGlobalFactory(CONTRACTS)))
validate_registry(REGISTRY)


def iter_provider_cases(
    seed: int,
    va: int,
    size: int,
    *,
    policy: SeedPolicy | None = None,
    vector: bool = False,
    disabled: bool = False,
    selected: tuple[str, ...] | None = None,
    after_feedback: bool | None = None,
    registry: tuple[Provider, ...] = REGISTRY,
) -> Iterator[tuple[str, Case]]:
    """Only additive fixtures; random/argument-edge/feedback streams stay in the CLI."""
    if disabled and selected not in (None, ()):
        raise ValueError("fixture provider selection conflicts with disabled providers")
    if disabled:
        return
    labels = select_labels(selected, tuple(provider.label for provider in registry))
    if after_feedback is not None and type(after_feedback) is not bool:
        raise ValueError("unsupported fixture provider phase")
    for provider in registry:
        if provider.requires_vector and not vector:
            continue
        if labels is not None and provider.label not in labels:
            continue
        if after_feedback is not None and provider.after_feedback != after_feedback:
            continue
        for ordinal in range(provider.case_count(va)):
            yield provider.label, provider.case(seed, ordinal, va, size, policy=policy)
