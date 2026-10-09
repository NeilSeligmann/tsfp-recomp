"""
Tiny JSON-schema subset validator (type, properties, required, items, enum,
additionalProperties).
"""

from __future__ import annotations

_TYPES = {
    "object": dict,
    "array": list,
    "string": str,
    "boolean": bool,
    "null": type(None),
}


class SchemaError(ValueError):
    pass


def validate(value: object, schema: dict, path: str = "$") -> None:
    kind = schema.get("type")
    if kind == "integer":
        if not isinstance(value, int) or isinstance(value, bool):
            raise SchemaError(f"{path}: expected integer")
    elif kind == "number":
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise SchemaError(f"{path}: expected number")
    elif kind in _TYPES:
        if not isinstance(value, _TYPES[kind]) or (
            kind != "boolean" and isinstance(value, bool) and kind != "null"
        ):
            raise SchemaError(f"{path}: expected {kind}")
    if "enum" in schema and value not in schema["enum"]:
        raise SchemaError(f"{path}: not in enum")
    if kind == "object":
        props = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value:
                raise SchemaError(f"{path}: missing required {name}")
        if schema.get("additionalProperties") is False:
            for name in value:
                if name not in props:
                    raise SchemaError(f"{path}: unexpected property {name}")
        for name, sub in props.items():
            if name in value:
                validate(value[name], sub, f"{path}.{name}")
    elif kind == "array" and "items" in schema:
        for index, item in enumerate(value):
            validate(item, schema["items"], f"{path}[{index}]")
