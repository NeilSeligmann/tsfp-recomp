# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the in game DirectSoundDoWork site (T844): the third admitted caller, the return 0x1CE573 of
sub_001CE400's default case, in src/audio/dsound_listener.c `work_handler`. Reached through
test_dsound_listener_work (which also pins the neighbours and the other returns of sub_001CE400 refused).
The older two sites keep their anchors in `dsound.py` (`work-caller-*`).
"""

LIS = ["test_dsound_listener_work"]

MUTATIONS: list[dict] = [
    {
        "id": "t844-ingame-caller-moved",
        "file": "src/audio/dsound_listener.c",
        "old": "if(have && caller==0x1CE573u)return handler(c,WORK,0x1CE573u,0u);",
        "new": "if(have && caller==0x1CE574u)return handler(c,WORK,0x1CE573u,0u);",
        "targets": LIS,
        "why": "the in game frame would be refused again (the boot stops at the DoWork the owner asked to admit) or a neighbouring return admitted.",
    },
    {
        "id": "t844-ingame-expected-caller-lost",
        "file": "src/audio/dsound_listener.c",
        "old": "if(have && caller==0x1CE573u)return handler(c,WORK,0x1CE573u,0u);",
        "new": "if(have && caller==0x1CE573u)return handler(c,WORK,0x1CE492u,0u);",
        "targets": LIS,
        "why": "the frame check would compare the stack word with the older site and refuse the in game one.",
    },
    {
        "id": "t844-ingame-any-caller",
        "file": "src/audio/dsound_listener.c",
        "old": "if(have && caller==0x1CE573u)return handler(c,WORK,0x1CE573u,0u);",
        "new": "if(have && caller>=0x1CE4CAu && caller<=0x1CE5C8u)return handler(c,WORK,caller,0u);",
        "targets": LIS,
        "why": "a whole address range would be admitted, the neighbours of the six exact returns must stay refused.",
    },
    {
        "id": "t844-ingame-skips-the-gates",
        "file": "src/audio/dsound_listener.c",
        "old": "if(have && caller==0x1CE573u)return handler(c,WORK,0x1CE573u,0u);",
        "new": "if(have && caller==0x1CE573u)return 0u;",
        "targets": LIS,
        "why": "the in game site would skip the policy, IRQL, global audio state and device identity gates every older site passes (the observation `work_seen` too).",
    },
    {
        "id": "t1156-return-1ce4ca-moved",
        "file": "src/audio/dsound_listener.c",
        "old": "caller==0x1CE4CAu || caller==0x1CE50Au || caller==0x1CE5C8u",
        "new": "caller==0x1CE4CBu || caller==0x1CE50Au || caller==0x1CE5C8u",
        "targets": LIS,
        "why": "the T1156 return 0x1CE4CA (a game state call of 0x458B0) would be refused again and the neighbour 0x1CE4CB admitted.",
    },
    {
        "id": "t1156-return-1ce50a-moved",
        "file": "src/audio/dsound_listener.c",
        "old": "caller==0x1CE4CAu || caller==0x1CE50Au || caller==0x1CE5C8u",
        "new": "caller==0x1CE4CAu || caller==0x1CE50Bu || caller==0x1CE5C8u",
        "targets": LIS,
        "why": "the T1156 return 0x1CE50A (a game state call of 0x458B0) would be refused again and the neighbour 0x1CE50B admitted.",
    },
    {
        "id": "t1156-return-1ce5c8-moved",
        "file": "src/audio/dsound_listener.c",
        "old": "caller==0x1CE4CAu || caller==0x1CE50Au || caller==0x1CE5C8u",
        "new": "caller==0x1CE4CAu || caller==0x1CE50Au || caller==0x1CE5C9u",
        "targets": LIS,
        "why": "the T1156 return 0x1CE5C8 (a game state call of 0x458B0) would be refused again and the neighbour 0x1CE5C9 admitted.",
    },
    {
        "id": "t1156-returns-skip-the-gates",
        "file": "src/audio/dsound_listener.c",
        "old": "caller==0x1CE5C8u))return handler(c,WORK,caller,0u);",
        "new": "caller==0x1CE5C8u))return 0u;",
        "targets": LIS,
        "why": "the T1156 returns would skip the policy, IRQL, global audio state and device identity gates.",
    },
]
