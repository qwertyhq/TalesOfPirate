"""Проверка API per-instance custom data на ISM компоненте."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal                                        # noqa: E402
from report import Reporter                          # noqa: E402


report = Reporter("probe_ism_custom_data")
try:
    methods = sorted(
        name for name in dir(unreal.InstancedStaticMeshComponent)
        if "custom" in name.lower() or "instance" in name.lower())
    for name in methods:
        report.line(f"ISM API: {name}")
    methods = sorted(
        name for name in dir(unreal.HierarchicalInstancedStaticMeshComponent)
        if "custom" in name.lower())
    for name in methods:
        report.line(f"HISM API: {name}")
finally:
    report.close()
