#!/usr/bin/env python3
"""Corrections applied to open3e's datapoint definitions on the way in.

open3e's database is the authority on layout, and nothing here changes that.
What it does change is metadata open3e leaves blank because it does not need
it: units, and the enumeration behind a sensor's error byte. Both matter here
because Home Assistant builds its entity out of them -- a value with no unit
gets no device class, no statistics and no place on a dashboard.

Four rules only, all narrow, all verified against the whole database rather
than guessed:

  * A leaf with no unit whose datapoint name says what it measures gets that
    unit. 131 fields gain one; the 530 that already have one are never
    touched.
  * A subfield named `Error` becomes an enumeration over `SensorStates`. All
    twenty datapoints in the database with such a field are named `*Sensor`
    and carry a single byte there, so the rule cannot catch anything else.
  * A `*PressureSensor` field labelled `hPa` is in bar. This is the one place
    an existing unit is replaced, because it is plainly wrong: the numbers are
    1.7 for a heating circuit and 4.6 / 9.6 for a refrigerant circuit, and
    WaterPressureConfiguration (1721) carries the same scale as `bar`. Home
    Assistant took the label at its word and showed "1,70 hPa" (#25). Three
    datapoints: 318, 322, 325.
  * The heating curve of a mixer circuit (880..887) is writable. open3e lists
    it read-only; a Vitocal 250-A acknowledged a forced write of 880 over the
    bus, and it is the setting people most want next to the room setpoints.
    Tried on circuit one of one installation -- the other seven share its
    layout and are included on that strength. The device still decides: one
    that does not allow it answers the write with a negative response.

Applied in two places that must not drift apart: gen_dpdb.py patches the JSON
that becomes the firmware's database, and the fixture generators patch the
Python codec objects they compare against. Skipping the second would make the
test suite fail on a difference that was intended.
"""
import re

# Deliberately short. A wrong unit is worse than none: Home Assistant will
# attach a device class to it and present the number as authoritative. Rules
# for `Runtime` and `PowerConsumption` were considered and dropped -- the
# database gives no way to tell hours from minutes for QuickModeRuntime.
UNIT_RULES = [
    (re.compile(r"TemperatureSensor$"), "°C"),
    (re.compile(r"HumiditySensor$"),    "%"),
    (re.compile(r"VolumeFlowSensor$"),  "m³/h"),
    (re.compile(r"OperationHours$"),    "h"),
]

# (datapoint name, unit as open3e has it) -> unit the value is really in.
UNIT_FIXES = [
    (re.compile(r"PressureSensor$"), "hPa", "bar"),
]

# Datapoints open3e records as read-only that a device accepted a write for.
ACCESS_FIXES = [
    re.compile(r"^Mixer\w+CircuitCentralHeatingCurve$"),
]

NUMERIC = {"O3EInt8", "O3EInt16", "O3EInt32", "O3EInt64",
           "O3EByteVal", "O3EFloat32"}

SENSOR_ERROR_FIELD = "Error"
SENSOR_ERROR_ENUM = "SensorStates"


def unit_for(dp_name: str) -> str | None:
    for pattern, unit in UNIT_RULES:
        if pattern.search(dp_name):
            return unit
    return None


def _fix_unit(dp_name: str, args: dict) -> bool:
    for pattern, wrong, right in UNIT_FIXES:
        if args.get("unit") == wrong and pattern.search(dp_name):
            args["unit"] = right
            return True
    return False


def _is_sensor_error(dp_name: str, sub_id: str, codec: str) -> bool:
    return (sub_id == SENSOR_ERROR_FIELD
            and codec == "O3EByteVal"
            and dp_name.endswith("Sensor"))


def apply_to_json(dps: dict) -> dict:
    """Patch Open3Edatapoints.json in place. Returns a count per change."""
    stats = {"units": 0, "unit_fixes": 0, "sensor_error": 0, "access": 0}
    for key, dp in dps.items():
        if not (key.isdigit() and isinstance(dp, dict)):
            continue
        name = dp.get("id", "")
        unit = unit_for(name)
        if dp["args"].get("acc") != "rw" and any(p.search(name) for p in ACCESS_FIXES):
            dp["args"]["acc"] = "rw"
            stats["access"] += 1
        subs = dp["args"].get("subTypes") if dp["codec"] == "O3EComplexType" else None

        if subs is None:
            if _fix_unit(name, dp["args"]):
                stats["unit_fixes"] += 1
            elif unit and dp["codec"] in NUMERIC and not dp["args"].get("unit"):
                dp["args"]["unit"] = unit
                stats["units"] += 1
            continue

        for sub in subs:
            if _is_sensor_error(name, sub.get("id", ""), sub["codec"]):
                sub["codec"] = "O3EEnum"
                sub["args"] = {"listStr": SENSOR_ERROR_ENUM,
                               **{k: v for k, v in sub["args"].items()
                                  if k in ("desc", "info", "acc")}}
                stats["sensor_error"] += 1
            elif _fix_unit(name, sub["args"]):
                stats["unit_fixes"] += 1
            elif unit and sub["codec"] in NUMERIC and not sub["args"].get("unit"):
                sub["args"]["unit"] = unit
                stats["units"] += 1
    return stats


def apply_to_codecs(dids: dict, C) -> int:
    """Patch the constructed open3e codec objects the fixtures are built from.

    Only the enumeration matters here: a unit never reaches a decoded value,
    so patching it would change nothing the fixtures can see.
    """
    n = 0
    for codec in dids.values():
        name = getattr(codec, "id", "") or ""
        subs = getattr(codec, "subTypes", None)
        if not subs or not name.endswith("Sensor"):
            continue
        for i, sub in enumerate(subs):
            if (getattr(sub, "id", "") == SENSOR_ERROR_FIELD
                    and type(sub).__name__ == "O3EByteVal"):
                subs[i] = C.O3EEnum(sub.string_len, sub.id, SENSOR_ERROR_ENUM)
                n += 1
    return n
