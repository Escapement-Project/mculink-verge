"""Host resource helpers shared by the local verification tools."""

import os


def _cgroup_v2_cpu_quota(
    root: str = "/sys/fs/cgroup", membership_file: str = "/proc/self/cgroup"
) -> int | None:
    """Return the rounded CPU quota visible in the current cgroup hierarchy."""
    try:
        with open(membership_file) as f:
            membership = next(
                (line.rstrip().partition("::")[2] for line in f if line.startswith("0::")),
                None,
            )
    except OSError:
        return None
    if membership is None or not membership.startswith("/"):
        return None

    root = os.path.abspath(root)
    current = os.path.abspath(os.path.join(root, membership.lstrip("/")))
    if os.path.commonpath((root, current)) != root:
        return None

    quotas = []
    while True:
        try:
            with open(os.path.join(current, "cpu.max")) as f:
                quota, period = f.read().split()
            if quota != "max":
                quota, period = int(quota), int(period)
                if quota > 0 and period > 0:
                    quotas.append(max(1, (2 * quota + period) // (2 * period)))
        except (OSError, ValueError):
            pass

        if current == root:
            break
        current = os.path.dirname(current)

    return min(quotas) if quotas else None


def available_cpus() -> int:
    """Return usable CPUs, bounded by process affinity and any cgroup v2 quota."""
    process_cpu_count = getattr(os, "process_cpu_count", None)
    count = process_cpu_count() if process_cpu_count is not None else None
    if count is None:
        affinity = getattr(os, "sched_getaffinity", None)
        count = len(affinity(0)) if affinity is not None else os.cpu_count()
    if os.name == "posix" and os.path.exists("/proc/self/cgroup"):
        quota = _cgroup_v2_cpu_quota()
        if quota is not None:
            count = min(count or quota, quota)
    return max(1, count or 1)


if __name__ == "__main__":
    print(available_cpus())
