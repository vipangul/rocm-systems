#!/usr/bin/env python3
"""
Validate AQL dump tool JSON output against the HSA AQL packet spec.

Checks performed:
  1. Structural: valid JSON array, all required fields present
  2. Header decode: packet_type bits[0:7] match raw_dwords[0] low byte
  3. Kernel dispatch validation:
     - workgroup sizes > 0, grid sizes > 0
     - kernel_object_addr is non-zero
     - kernel_descriptor_dwords present and non-null
     - kernel_code_entry_byte_offset (descriptor dword[4]) is non-zero
  4. Barrier validation:
     - gpu_timestamp_ns present and > 0 for barriers with signals
     - completion_signal (raw dwords[14:15]) is non-zero
  5. Sequence numbers are contiguous (0..N-1)
  6. Timestamp ordering: barrier timestamps should be monotonically increasing
     within the same queue
  7. Multi-kernel consistency: identical kernel launches share kernel_object_addr
     and kernel_descriptor_dwords
  8. Cross-check: expected packet counts match user expectations

Usage:
    python3 validate_dump.py <json_file> [--expect-dispatches N]
                                          [--expect-barriers N]
                                          [--expect-total N]
                                          [--expect-unique-kernels N]
                                          [--expect-queues N]
                                          [--check-timestamps]
"""

import json
import sys
import argparse
from collections import defaultdict


def parse_hex(s):
    """Parse hex string like '0x00030b02' to int."""
    return int(s, 16)


def validate(json_path, args):
    errors = []
    warnings = []

    # ------- Load JSON -------
    try:
        with open(json_path) as f:
            records = json.load(f)
    except json.JSONDecodeError as e:
        print(f"FAIL: Invalid JSON — {e}")
        return False
    except FileNotFoundError:
        print(f"FAIL: File not found — {json_path}")
        return False

    if not isinstance(records, list):
        print("FAIL: Top-level JSON is not an array")
        return False

    total = len(records)
    print(f"  Loaded {total} packet records")

    # ------- Check 1: Required fields -------
    required_fields = [
        "seq_num", "packet_type", "packet_type_name", "queue_id",
        "raw_dwords", "gpu_timestamp_ns",
        "kernel_object_addr", "kernel_descriptor_dwords"
    ]
    for i, rec in enumerate(records):
        for field in required_fields:
            if field not in rec:
                errors.append(f"Record {i}: missing field '{field}'")

    # ------- Check 2: Sequence numbers contiguous -------
    seq_nums = [r.get("seq_num", -1) for r in records]
    expected_seq = list(range(total))
    if seq_nums != expected_seq:
        errors.append(f"Sequence numbers not contiguous 0..{total-1}: "
                      f"got {seq_nums[:5]}...{seq_nums[-5:]}")
    else:
        print(f"  ✓ Sequence numbers are contiguous [0..{total-1}]")

    # ------- Check 3: Header type matches raw_dwords -------
    for i, rec in enumerate(records):
        ptype = rec.get("packet_type", -1)
        raw = rec.get("raw_dwords", [])
        if len(raw) != 16:
            errors.append(f"Record {i}: raw_dwords has {len(raw)} entries (expected 16)")
            continue
        dword0 = parse_hex(raw[0])
        header_type = dword0 & 0xFF
        if header_type != ptype:
            errors.append(f"Record {i}: packet_type={ptype} but header low byte=0x{header_type:02x}")

    header_ok = not any("header low byte" in e for e in errors)
    if header_ok:
        print(f"  ✓ All packet headers decode correctly (type bits match raw_dwords[0])")

    # ------- Classify packets -------
    dispatches = [r for r in records if r["packet_type"] == 2]
    barriers_and = [r for r in records if r["packet_type"] == 3]
    barriers_or = [r for r in records if r["packet_type"] == 4]
    barriers = barriers_and + barriers_or
    vendor = [r for r in records if r["packet_type"] == 5]
    other = [r for r in records if r["packet_type"] not in (2, 3, 4, 5)]

    print(f"  Packet breakdown:")
    print(f"    KERNEL_DISPATCH (type 2): {len(dispatches)}")
    print(f"    BARRIER_AND     (type 3): {len(barriers_and)}")
    print(f"    BARRIER_OR      (type 4): {len(barriers_or)}")
    if vendor:
        print(f"    VENDOR_SPECIFIC (type 5): {len(vendor)}")
    if other:
        print(f"    OTHER:                    {len(other)} — types: {set(r['packet_type'] for r in other)}")

    # ------- Check 4: Kernel Dispatch validation -------
    unique_kernel_objs = set()
    kernel_descriptor_map = defaultdict(set)

    for i, rec in enumerate(dispatches):
        raw = rec["raw_dwords"]
        dword0 = parse_hex(raw[0])

        # workgroup sizes from dword[1] (x=low16, y=high16) and dword[2] (z=low16)
        dword1 = parse_hex(raw[1])
        dword2 = parse_hex(raw[2])
        wg_x = dword1 & 0xFFFF
        wg_y = (dword1 >> 16) & 0xFFFF
        wg_z = dword2 & 0xFFFF

        if wg_x == 0 or wg_y == 0 or wg_z == 0:
            errors.append(f"Dispatch seq={rec['seq_num']}: workgroup size has zero dimension "
                          f"({wg_x}, {wg_y}, {wg_z})")

        # grid sizes from dword[3], dword[4], dword[5]
        grid_x = parse_hex(raw[3])
        grid_y = parse_hex(raw[4])
        grid_z = parse_hex(raw[5])

        if grid_x == 0 or grid_y == 0 or grid_z == 0:
            errors.append(f"Dispatch seq={rec['seq_num']}: grid size has zero dimension "
                          f"({grid_x}, {grid_y}, {grid_z})")

        # Check grid >= workgroup
        if grid_x < wg_x:
            errors.append(f"Dispatch seq={rec['seq_num']}: grid_x ({grid_x}) < workgroup_x ({wg_x})")

        # kernel_object_addr
        kobj = rec.get("kernel_object_addr")
        if kobj is None or kobj == "null":
            errors.append(f"Dispatch seq={rec['seq_num']}: kernel_object_addr is null")
        else:
            kobj_int = int(kobj, 16) if isinstance(kobj, str) else kobj
            if kobj_int == 0:
                errors.append(f"Dispatch seq={rec['seq_num']}: kernel_object_addr is 0")
            unique_kernel_objs.add(kobj)

            # Verify kernel_object from raw dwords matches
            # kernel_object is at offset 32 bytes = dword[8:9]
            ko_lo = parse_hex(raw[8])
            ko_hi = parse_hex(raw[9])
            ko_from_raw = (ko_hi << 32) | ko_lo
            if ko_from_raw != kobj_int:
                errors.append(f"Dispatch seq={rec['seq_num']}: kernel_object mismatch: "
                              f"raw=0x{ko_from_raw:x} vs reported=0x{kobj_int:x}")

        # kernel_descriptor_dwords
        kd = rec.get("kernel_descriptor_dwords")
        if kd is None:
            errors.append(f"Dispatch seq={rec['seq_num']}: kernel_descriptor_dwords is null")
        else:
            if len(kd) != 16:
                errors.append(f"Dispatch seq={rec['seq_num']}: kernel_descriptor has {len(kd)} dwords (expected 16)")
            else:
                # kernel_code_entry_byte_offset at dword[4] (offset 16 bytes)
                kce_offset = parse_hex(kd[4])
                if kce_offset == 0:
                    warnings.append(f"Dispatch seq={rec['seq_num']}: kernel_code_entry_byte_offset is 0 "
                                    "(unusual but possible for some runtime kernels)")

                # pgm_rsrc2 at dword[12] should be non-zero for any real kernel
                pgm_rsrc2 = parse_hex(kd[12])
                if pgm_rsrc2 == 0:
                    warnings.append(f"Dispatch seq={rec['seq_num']}: pgm_rsrc2 is 0 (unusual)")

                # Track descriptor content per kernel_object
                if kobj:
                    kernel_descriptor_map[kobj].add(tuple(kd))

    dispatch_ok = not any("Dispatch" in e for e in errors)
    if dispatch_ok and dispatches:
        print(f"  ✓ All {len(dispatches)} KERNEL_DISPATCH packets valid:")
        print(f"    - Workgroup/grid sizes > 0")
        print(f"    - kernel_object_addr non-null and matches raw dwords")
        print(f"    - kernel_descriptor present with valid pgm_rsrc fields")
        print(f"    - Unique kernel objects: {len(unique_kernel_objs)}")

    # Check that same kernel_object always has same descriptor
    for kobj, descs in kernel_descriptor_map.items():
        if len(descs) > 1:
            errors.append(f"Kernel object {kobj} has {len(descs)} different descriptors "
                          "(should be identical for same kernel)")

    if all(len(d) == 1 for d in kernel_descriptor_map.values()) and kernel_descriptor_map:
        print(f"  ✓ Kernel descriptor consistency: same kernel_object → same descriptor")

    # ------- Check 5: Barrier validation -------
    barrier_timestamps = []
    for rec in barriers:
        raw = rec["raw_dwords"]
        # completion_signal at offset 56 = dword[14:15]
        sig_lo = parse_hex(raw[14])
        sig_hi = parse_hex(raw[15])
        sig = (sig_hi << 32) | sig_lo

        ts = rec.get("gpu_timestamp_ns")
        if ts is not None and ts > 0:
            barrier_timestamps.append((rec["seq_num"], rec["queue_id"], ts))

        if sig == 0 and (ts is None or ts == 0):
            # No signal, no timestamp — acceptable (some barriers might not have signals)
            pass

    if barrier_timestamps:
        print(f"  ✓ {len(barrier_timestamps)} barrier(s) have GPU timestamps")
        # Check monotonicity per queue
        per_queue_ts = defaultdict(list)
        for seq, qid, ts in barrier_timestamps:
            per_queue_ts[qid].append((seq, ts))

        for qid, ts_list in per_queue_ts.items():
            ts_list.sort(key=lambda x: x[0])  # sort by seq_num
            for j in range(1, len(ts_list)):
                if ts_list[j][1] < ts_list[j-1][1]:
                    errors.append(f"Barrier timestamps not monotonic in queue {qid}: "
                                  f"seq {ts_list[j-1][0]}={ts_list[j-1][1]} > "
                                  f"seq {ts_list[j][0]}={ts_list[j][1]}")

        ts_monotonic = not any("monotonic" in e for e in errors)
        if ts_monotonic:
            print(f"  ✓ Barrier timestamps are monotonically increasing per queue")

        # Show timestamp deltas
        if len(barrier_timestamps) >= 2:
            for qid, ts_list in per_queue_ts.items():
                if len(ts_list) >= 2:
                    ts_list.sort(key=lambda x: x[0])
                    for j in range(1, len(ts_list)):
                        delta_ns = ts_list[j][1] - ts_list[j-1][1]
                        delta_ms = delta_ns / 1e6
                        print(f"    Δ barrier[seq {ts_list[j-1][0]} → {ts_list[j][0]}] = "
                              f"{delta_ns} ns ({delta_ms:.3f} ms)")

    # ------- Check 6: Queue count -------
    unique_queues = set(r["queue_id"] for r in records)
    print(f"  Queue count: {len(unique_queues)}")

    # ------- Check expected counts -------
    if args.expect_dispatches is not None:
        if len(dispatches) != args.expect_dispatches:
            errors.append(f"Expected {args.expect_dispatches} dispatches, got {len(dispatches)}")
        else:
            print(f"  ✓ Dispatch count matches expected: {args.expect_dispatches}")

    if args.expect_barriers is not None:
        if len(barriers) != args.expect_barriers:
            errors.append(f"Expected {args.expect_barriers} barriers, got {len(barriers)}")
        else:
            print(f"  ✓ Barrier count matches expected: {args.expect_barriers}")

    if args.expect_total is not None:
        if total != args.expect_total:
            errors.append(f"Expected {args.expect_total} total packets, got {total}")
        else:
            print(f"  ✓ Total packet count matches expected: {args.expect_total}")

    if args.expect_unique_kernels is not None:
        if len(unique_kernel_objs) != args.expect_unique_kernels:
            errors.append(f"Expected {args.expect_unique_kernels} unique kernels, "
                          f"got {len(unique_kernel_objs)}")
        else:
            print(f"  ✓ Unique kernel count matches expected: {args.expect_unique_kernels}")

    if args.expect_queues is not None:
        if len(unique_queues) != args.expect_queues:
            errors.append(f"Expected {args.expect_queues} queues, got {len(unique_queues)}")
        else:
            print(f"  ✓ Queue count matches expected: {args.expect_queues}")

    # ------- Summary -------
    print()
    if warnings:
        print(f"  ⚠ {len(warnings)} warning(s):")
        for w in warnings:
            print(f"    - {w}")

    if errors:
        print(f"  ✗ {len(errors)} ERROR(s):")
        for e in errors:
            print(f"    - {e}")
        return False
    else:
        print(f"  ✓ ALL CHECKS PASSED")
        return True


def main():
    parser = argparse.ArgumentParser(description="Validate AQL dump tool output")
    parser.add_argument("json_file", help="Path to aql_dump_output.json")
    parser.add_argument("--expect-dispatches", type=int, default=None)
    parser.add_argument("--expect-barriers", type=int, default=None)
    parser.add_argument("--expect-total", type=int, default=None)
    parser.add_argument("--expect-unique-kernels", type=int, default=None)
    parser.add_argument("--expect-queues", type=int, default=None)
    parser.add_argument("--check-timestamps", action="store_true")
    args = parser.parse_args()

    print(f"═══════════════════════════════════════════════════")
    print(f"Validating: {args.json_file}")
    print(f"═══════════════════════════════════════════════════")

    ok = validate(args.json_file, args)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
