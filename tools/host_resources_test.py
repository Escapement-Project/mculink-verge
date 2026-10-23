# /// script
# requires-python = ">=3.11"
# ///
"""Check CPU quota detection against cgroup v2 hierarchy fixtures."""

import os
import tempfile
from unittest.mock import patch

import host_resources
from host_resources import _cgroup_v2_cpu_quota


def write(path: str, contents: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(contents)


with tempfile.TemporaryDirectory() as temp:
    root = os.path.join(temp, "cgroup")
    membership = os.path.join(temp, "membership")
    write(membership, "0::/group/child\n")
    write(os.path.join(root, "cpu.max"), "max 100000\n")
    write(os.path.join(root, "group", "cpu.max"), "125000 100000\n")
    write(os.path.join(root, "group", "child", "cpu.max"), "max 100000\n")
    assert _cgroup_v2_cpu_quota(root, membership) == 1

    write(os.path.join(root, "group", "cpu.max"), "150000 100000\n")
    assert _cgroup_v2_cpu_quota(root, membership) == 2

    write(os.path.join(root, "cpu.max"), "100000 100000\n")
    write(os.path.join(root, "group", "child", "cpu.max"), "250000 100000\n")
    assert _cgroup_v2_cpu_quota(root, membership) == 1

    write(os.path.join(root, "cpu.max"), "max 100000\n")
    write(os.path.join(root, "group", "cpu.max"), "max 100000\n")
    write(os.path.join(root, "group", "child", "cpu.max"), "broken\n")
    assert _cgroup_v2_cpu_quota(root, membership) is None

    write(membership, "2:cpu,cpuacct:/group/child\n")
    assert _cgroup_v2_cpu_quota(root, membership) is None

with (
    patch.object(os, "process_cpu_count", lambda: 32, create=True),
    patch.object(host_resources, "_cgroup_v2_cpu_quota", lambda: 2),
    patch("host_resources.os.path.exists", return_value=True),
):
    assert host_resources.available_cpus() == 2

with (
    patch.object(os, "process_cpu_count", lambda: 4, create=True),
    patch.object(host_resources, "_cgroup_v2_cpu_quota", lambda: 8),
    patch("host_resources.os.path.exists", return_value=True),
):
    assert host_resources.available_cpus() == 4

print("cgroup v2 CPU quota detection: all clear")
