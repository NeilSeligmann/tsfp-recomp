"""Mutation for the measured XGBuffer pointer field offset (T1067)."""

MUTATIONS: list[dict] = [
    {
        "id": "t1067-xgrph-buffer-pointer-reads-wrong-field",
        "file": "src/gpu/xgrph_object_lifetime.c",
        "old": "kernel_guest_read_u32((kernel_guest_ptr)(object + 4u), &buffer)",
        "new": "kernel_guest_read_u32((kernel_guest_ptr)(object + 8u), &buffer)",
        "targets": ["test_t1067_xgrph_vtable_reset"],
        "why": (
            "the retail XGBuffer_GetBufferPointer body reads [object+4]; reading the adjacent "
            "word returns a different pointer or zero and violates the original field contract."
        ),
    },
]
