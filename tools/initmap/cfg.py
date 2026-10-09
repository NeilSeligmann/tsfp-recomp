# SPDX-License-Identifier: GPL-3.0-or-later
"""Graph questions about one parsed function: order, loops, and what always runs.

Three answers are needed to turn a call graph into an approximate execution order.

- `reverse_postorder`: a block order in which a join point follows both arms that
  lead to it, with the fall-through arm of a branch first. Back edges are ignored.
- `cyclic_nodes`: blocks that sit inside a loop, so a call there may run many times
  or, if the loop is not entered, never.
- `unconditional_nodes`: blocks every execution of the function passes through. A
  block is unconditional exactly when it post-dominates the entry, so the answer is
  the chain of immediate post-dominators starting at the entry.

A function with no reachable exit (an infinite loop) has no post-dominator tree. It is
analysed against its back-edge sources instead, which asks "what runs before the
first trip round the loop", and that substitution is reported through
`has_real_exit` rather than hidden.
"""

from __future__ import annotations

from dataclasses import dataclass

from tools.initmap.liftparse import Function


@dataclass(frozen=True)
class Shape:
    """Everything the walker needs to know about a function's block structure."""

    order: tuple[int, ...]
    cyclic: frozenset[int]
    unconditional: frozenset[int]
    #: False when no exit is reachable and back-edge sources stood in for exits.
    has_real_exit: bool


def _reachable_and_back_edges(function: Function) -> tuple[list[int], set[int]]:
    """Iterative DFS from the entry. Returns reachable ids and the sources of back edges."""
    seen: set[int] = set()
    on_stack: set[int] = set()
    back_sources: set[int] = set()
    order: list[int] = []
    stack: list[tuple[int, int]] = [(0, 0)]
    seen.add(0)
    on_stack.add(0)
    order.append(0)
    while stack:
        node_id, index = stack[-1]
        successors = function.nodes[node_id].successors
        if index < len(successors):
            stack[-1] = (node_id, index + 1)
            child = successors[index]
            if child in on_stack:
                back_sources.add(node_id)
            elif child not in seen:
                seen.add(child)
                on_stack.add(child)
                order.append(child)
                stack.append((child, 0))
        else:
            on_stack.discard(node_id)
            stack.pop()
    return order, back_sources


def reverse_postorder(function: Function) -> list[int]:
    """Block ids from the entry, fall-through arm first, joins after both arms."""
    seen: set[int] = {0}
    post: list[int] = []
    # `pop()` takes from the end, so the LAST successor is visited first and the FIRST one
    # finishes last, which puts it first once the postorder is reversed.
    stack: list[tuple[int, list[int]]] = [(0, list(function.nodes[0].successors))]
    while stack:
        node_id, pending = stack[-1]
        if pending:
            child = pending.pop()
            if child not in seen:
                seen.add(child)
                stack.append((child, list(function.nodes[child].successors)))
        else:
            post.append(node_id)
            stack.pop()
    post.reverse()
    return post


def cyclic_nodes(function: Function) -> frozenset[int]:
    """Blocks in a strongly connected component of more than one block, or a self loop."""
    reachable, _ = _reachable_and_back_edges(function)
    reachable_set = set(reachable)
    index_of: dict[int, int] = {}
    low: dict[int, int] = {}
    on_stack: set[int] = set()
    scc_stack: list[int] = []
    result: set[int] = set()
    counter = 0
    for root in reachable:
        if root in index_of:
            continue
        work: list[tuple[int, int]] = [(root, 0)]
        index_of[root] = low[root] = counter
        counter += 1
        scc_stack.append(root)
        on_stack.add(root)
        while work:
            node_id, position = work[-1]
            successors = [
                child for child in function.nodes[node_id].successors if child in reachable_set
            ]
            if position < len(successors):
                work[-1] = (node_id, position + 1)
                child = successors[position]
                if child not in index_of:
                    index_of[child] = low[child] = counter
                    counter += 1
                    scc_stack.append(child)
                    on_stack.add(child)
                    work.append((child, 0))
                elif child in on_stack:
                    low[node_id] = min(low[node_id], index_of[child])
            else:
                work.pop()
                if work:
                    parent = work[-1][0]
                    low[parent] = min(low[parent], low[node_id])
                if low[node_id] == index_of[node_id]:
                    component: list[int] = []
                    while True:
                        member = scc_stack.pop()
                        on_stack.discard(member)
                        component.append(member)
                        if member == node_id:
                            break
                    if len(component) > 1:
                        result.update(component)
                    elif node_id in function.nodes[node_id].successors:
                        result.add(node_id)
    return frozenset(result)


def unconditional_nodes(function: Function) -> tuple[frozenset[int], bool]:
    """Blocks on every path from the entry to an exit, and whether a real exit existed."""
    reachable, back_sources = _reachable_and_back_edges(function)
    reachable_set = set(reachable)
    exits = {node_id for node_id in reachable if function.nodes[node_id].is_exit}
    has_real_exit = bool(exits)
    if not exits:
        exits = set(back_sources)
    if not exits:
        return frozenset(reachable), has_real_exit

    virtual_exit = len(function.nodes)
    # Reverse graph: the "predecessors" of a block, for the dominance fixpoint, are its
    # forward successors. The virtual exit's are the exits themselves.
    reverse_successors: dict[int, list[int]] = {node_id: [] for node_id in reachable}
    reverse_successors[virtual_exit] = sorted(exits)
    for node_id in reachable:
        for child in function.nodes[node_id].successors:
            if child in reachable_set:
                reverse_successors[child].append(node_id)

    # Reverse postorder of the reverse graph, from the virtual exit.
    seen = {virtual_exit}
    post: list[int] = []
    stack: list[tuple[int, list[int]]] = [
        (virtual_exit, list(reversed(reverse_successors[virtual_exit])))
    ]
    while stack:
        node_id, pending = stack[-1]
        if pending:
            child = pending.pop()
            if child not in seen:
                seen.add(child)
                stack.append((child, list(reversed(reverse_successors[child]))))
        else:
            post.append(node_id)
            stack.pop()
    post.reverse()
    position = {node_id: index for index, node_id in enumerate(post)}

    # Predecessors in the reverse graph are forward successors, plus the virtual exit
    # for each exit block.
    reverse_predecessors: dict[int, list[int]] = {node_id: [] for node_id in post}
    for node_id in post:
        if node_id == virtual_exit:
            continue
        for child in function.nodes[node_id].successors:
            if child in position:
                reverse_predecessors[node_id].append(child)
        if node_id in exits:
            reverse_predecessors[node_id].append(virtual_exit)

    idom: dict[int, int] = {virtual_exit: virtual_exit}

    def intersect(first: int, second: int) -> int:
        while first != second:
            while position[first] > position[second]:
                first = idom[first]
            while position[second] > position[first]:
                second = idom[second]
        return first

    changed = True
    while changed:
        changed = False
        for node_id in post:
            if node_id == virtual_exit:
                continue
            processed = [pred for pred in reverse_predecessors[node_id] if pred in idom]
            if not processed:
                continue
            new_idom = processed[0]
            for other in processed[1:]:
                new_idom = intersect(other, new_idom)
            if idom.get(node_id) != new_idom:
                idom[node_id] = new_idom
                changed = True

    if 0 not in idom:
        return frozenset({0}), has_real_exit
    chain = {0}
    current = 0
    while idom[current] != virtual_exit:
        current = idom[current]
        chain.add(current)
    return frozenset(chain), has_real_exit


def analyse(function: Function) -> Shape:
    """All three answers for one function."""
    unconditional, has_real_exit = unconditional_nodes(function)
    return Shape(
        order=tuple(reverse_postorder(function)),
        cyclic=cyclic_nodes(function),
        unconditional=unconditional,
        has_real_exit=has_real_exit,
    )
