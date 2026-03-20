# AQL Dump Tool

A `rocprofiler-sdk`-based tool that captures the full HSA AQL (Architected Queuing Language) packet stream from any HIP application, providing three capabilities that are not available through `rocprofv3` alone:

1. **Full AQL packet dump** — Raw 64-byte hex dump of *every* AQL packet type (not just kernel dispatches). This includes `BARRIER_AND`, `BARRIER_OR`, `VENDOR_SPECIFIC`, and `AGENT_DISPATCH` packets.
2. **GPU-side barrier timestamps** — Installs completion signals on barrier packets (`BARRIER_AND` / `BARRIER_OR`) to capture GPU-side timestamps. HIP translates `hipEventRecord()` calls into `BARRIER_AND` packets, so this gives you precise GPU-side event timing.
3. **Kernel descriptor memory dump** — For each `KERNEL_DISPATCH` packet, dereferences the `kernel_object` pointer and dumps 16 dwords (64 bytes) of the kernel descriptor, which contains shader resource configuration (`pgm_rsrc1`, `pgm_rsrc2`, `pgm_rsrc3`, register counts, etc.).

## Target Hardware

- **gfx1150 / gfx1151** (AMD APU with unified memory — kernel descriptors are directly host-readable)
- Should work on other RDNA/CDNA GPUs, though kernel descriptor reads may require system-unified memory or XNACK support.

## Prerequisites

- **ROCm** (6.x or later) with:
  - HSA runtime (`libhsa-runtime64.so`)
  - HIP compiler (`amdclang++`) — required only for building test applications
  - `rocprofiler-sdk` runtime library (`librocprofiler-sdk64.so`)
- **CMake** ≥ 3.21
- **C++17** compiler (GCC or Clang)

The build system can resolve headers from either:
- An **installed ROCm** (e.g., `/opt/rocm`)
- The **rocm-systems monorepo source tree** (sibling `projects/` directory)

## Quick Start

### 1. Build

```bash
cd aql_dump_tool
mkdir build && cd build

# Point to your ROCm installation (or source build)
cmake .. -DROCM_PATH=/opt/rocm
make -j$(nproc)
```

This produces:
- `libaql_dump_tool.so` — the tool shared library
- `aql_test_app` — minimal HIP test application (if HIP compiler is found)
- `compute_comm_overlap`, `multistream_app`, `vector_ops` — additional test apps from the rocprofiler-sdk test suite

### 2. Run

Load the tool via the `ROCP_TOOL_LIBRARIES` environment variable:

```bash
# Set library paths (adjust ROCM_PATH as needed)
export ROCM_PATH=/opt/rocm

export LD_LIBRARY_PATH=${ROCM_PATH}/lib:$LD_LIBRARY_PATH

# Run with the tool loaded
ROCP_TOOL_LIBRARIES=./libaql_dump_tool.so ./aql_test_app
```

By default, the output is written to `aql_dump_output.json` in the current directory. Override with:

```bash
AQL_DUMP_OUTPUT=/path/to/output.json ROCP_TOOL_LIBRARIES=./libaql_dump_tool.so ./aql_test_app
```

To write **SQLite .db** output instead of JSON, set the output path to a file ending in `.db` (requires building with SQLite3):

```bash
AQL_DUMP_OUTPUT=/path/to/output.db ROCP_TOOL_LIBRARIES=./libaql_dump_tool.so ./aql_test_app
```

Format is determined by the file extension: `.db` → SQLite database, anything else → JSON.

### 3. Validate

A Python validation script checks structural correctness, packet decoding, kernel descriptor consistency, and timestamp monotonicity:

```bash
python3 ../test/validate_dump.py aql_dump_output.json
```

Expected output for `aql_test_app`:

```
═══════════════════════════════════════════════════
Validating: aql_dump_output.json
═══════════════════════════════════════════════════
  Loaded 15 packet records
  ✓ Sequence numbers are contiguous [0..14]
  ✓ All packet headers decode correctly (type bits match raw_dwords[0])
  Packet breakdown:
    KERNEL_DISPATCH (type 2): 12
    BARRIER_AND     (type 3): 3
  ✓ All 12 KERNEL_DISPATCH packets valid
  ✓ Kernel descriptor consistency: same kernel_object → same descriptor
  ✓ 3 barrier(s) have GPU timestamps
  ✓ Barrier timestamps are monotonically increasing per queue
  ✓ ALL CHECKS PASSED
```

The validator also supports optional expected-count arguments:

```bash
python3 ../test/validate_dump.py output.json \
    --expect-dispatches 10 \
    --expect-barriers 2 \
    --expect-queues 1
```

## Output Format

The tool writes a JSON array where each element represents one AQL packet:

```json
[
  {
    "seq_num": 0,
    "packet_type": 3,
    "packet_type_name": "BARRIER_AND",
    "queue_id": 129537024,
    "raw_dwords": ["0x00030b02", "0x00000000", ...],
    "gpu_timestamp_ns": 1234567890,
    "kernel_object_addr": null,
    "kernel_descriptor_dwords": null
  },
  {
    "seq_num": 1,
    "packet_type": 2,
    "packet_type_name": "KERNEL_DISPATCH",
    "queue_id": 129537024,
    "raw_dwords": ["0x00030b02", "0x01000100", ...],
    "gpu_timestamp_ns": null,
    "kernel_object_addr": "0x7f8a1c000040",
    "kernel_descriptor_dwords": ["0x00000000", "0x00000000", ...]
  }
]
```

### Field Reference

| Field | Type | Description |
|---|---|---|
| `seq_num` | integer | Global sequential packet number (0-based) |
| `packet_type` | integer | HSA packet type from header bits [0:7] |
| `packet_type_name` | string | Human-readable type: `KERNEL_DISPATCH`, `BARRIER_AND`, `BARRIER_OR`, `VENDOR_SPECIFIC`, `AGENT_DISPATCH`, `INVALID` |
| `queue_id` | integer | Queue handle (unique per queue) |
| `raw_dwords` | string[16] | Raw 64 bytes as 16 hex-encoded 32-bit dwords |
| `gpu_timestamp_ns` | integer or null | GPU-side completion timestamp (barrier packets only) |
| `kernel_object_addr` | string or null | Hex address of kernel descriptor (dispatch packets only) |
| `kernel_descriptor_dwords` | string[16] or null | 64 bytes of kernel descriptor memory (dispatch packets only) |

### Interpreting raw_dwords

All AQL packets are 64 bytes (16 dwords). The layout depends on packet type:

**KERNEL_DISPATCH (type 2):**
| Dword | Offset | Field |
|-------|--------|-------|
| 0 | 0 | header (bits[0:7]=type, bits[8:15]=barrier+acquire+release fence) |
| 0 | 2 | setup (dimensions) |
| 1 | 4 | workgroup_size_x (low 16), workgroup_size_y (high 16) |
| 2 | 8 | workgroup_size_z (low 16), reserved |
| 3–5 | 12–20 | grid_size_x, grid_size_y, grid_size_z |
| 6–7 | 24 | private_segment_size, group_segment_size |
| 8–9 | 32 | kernel_object (64-bit pointer to kernel descriptor) |
| 10–11 | 40 | kernarg_address (64-bit) |
| 14–15 | 56 | completion_signal (64-bit) |

**BARRIER_AND / BARRIER_OR (type 3/4):**
| Dword | Offset | Field |
|-------|--------|-------|
| 0 | 0 | header |
| 2–11 | 8–44 | dep_signal[0..4] (5 dependency signals, 64-bit each) |
| 14–15 | 56 | completion_signal (64-bit) |

### Interpreting kernel_descriptor_dwords

The kernel descriptor is a 64-byte structure at the address pointed to by `kernel_object`:

| Dword | Offset | Field |
|-------|--------|-------|
| 0–3 | 0 | group_segment_fixed_size, private_segment_fixed_size, kernel_code_prefetch_byte_size |
| 4–5 | 16 | kernel_code_entry_byte_offset (64-bit, relative to descriptor start) |
| 8 | 32 | compute_pgm_rsrc3 |
| 10 | 40 | compute_pgm_rsrc1 (SGPRs, VGPRs, float modes, etc.) |
| 11 | 44 | compute_pgm_rsrc2 (scratch, LDS, trap handler flags, etc.) |
| 12–15 | 48 | kernel_code_properties, kernarg_preload, reserved |

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  HIP Application (hipEventRecord, kernel<<<>>>, etc.)    │
└────────────────────────┬─────────────────────────────────┘
                         │ hsa_queue_create()
                         ▼
┌──────────────────────────────────────────────────────────┐
│  aql_dump_tool (loaded via ROCP_TOOL_LIBRARIES)          │
│                                                          │
│  ┌─ rocprofiler_configure() ─────────────────────────┐   │
│  │  Registers HSA table interception callback         │   │
│  └────────────────────────────────────────────────────┘   │
│                         │                                 │
│  ┌─ hsa_table_registration_callback() ───────────────┐   │
│  │  Saves CoreApiTable + AmdExtTable pointers         │   │
│  │  Wraps hsa_queue_create → wrapped_hsa_queue_create │   │
│  └────────────────────────────────────────────────────┘   │
│                         │                                 │
│  ┌─ wrapped_hsa_queue_create() ──────────────────────┐   │
│  │  Creates intercept (proxy) queue via               │   │
│  │    hsa_amd_queue_intercept_create                  │   │
│  │  Enables profiling on the queue                    │   │
│  │  Registers WriteInterceptor callback               │   │
│  └────────────────────────────────────────────────────┘   │
│                         │                                 │
│  ┌─ WriteInterceptor() ─────────────────────────────┐    │
│  │  For EVERY packet written to the queue:            │    │
│  │                                                    │    │
│  │  • Copy raw 64 bytes into PacketRecord             │    │
│  │                                                    │    │
│  │  • KERNEL_DISPATCH:                                │    │
│  │    → dereference kernel_object pointer             │    │
│  │    → copy 64 bytes of kernel descriptor            │    │
│  │                                                    │    │
│  │  • BARRIER_AND / BARRIER_OR:                       │    │
│  │    → create our own completion signal              │    │
│  │    → replace packet's signal with ours             │    │
│  │    → register async handler to chain original      │    │
│  │                                                    │    │
│  │  • Forward all packets to real hardware queue      │    │
│  └────────────────────────────────────────────────────┘   │
│                         │                                 │
│  ┌─ tool_fini() ────────────────────────────────────┐    │
│  │  collect_barrier_timestamps():                     │    │
│  │    → read GPU timestamps from our signals          │    │
│  │    → chain to original signals (preserve app sync) │    │
│  │    → destroy our signals                           │    │
│  │  write_json_output() → aql_dump_output.json        │    │
│  └────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────┘
```

### Key Design Decisions

**Proxy queue interception (not LD_PRELOAD wrapping)**

The tool uses `hsa_amd_queue_intercept_create` to create proxy queues, which is the same mechanism `rocprofiler-sdk` uses internally. This is more reliable than wrapping individual HSA API calls because it captures packets at the AQL level regardless of which API the application uses (HIP, OpenCL, raw HSA).

**Signal chaining for barrier timestamps**

HIP events (`hipEventRecord`) translate to `BARRIER_AND` packets with application-owned completion signals. We cannot simply read timestamps from these signals at `tool_fini` time because HIP may destroy them before our tool finalizes. Instead:

1. We **always create our own signal** for every barrier packet.
2. We **replace** the packet's completion signal with ours.
3. We register an **async handler** (`hsa_amd_signal_async_handler`) on our signal that decrements the application's original signal when GPU work completes. This preserves HIP's synchronization semantics.
4. At `tool_fini`, we read timestamps from our signals (which we fully own) and then clean up.

This avoids a race condition where signals are destroyed before timestamps can be read, and works even with HIP's GPU-only doorbell signals (which don't support async handlers directly).

**Kernel descriptor host-side read**

On gfx1150 (APU with unified memory), the `kernel_object` pointer in dispatch packets points to GPU-accessible memory that is also host-readable. We simply `memcpy` 64 bytes from this address. On discrete GPUs, this may require XNACK or explicit memory mapping.

## File Structure

```
aql_dump_tool/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── src/
│   ├── aql_dump_tool.cpp       # Core implementation (interception, timestamps, JSON output)
│   └── aql_dump_tool.h         # Data structures (PacketRecord, BarrierTimestampCtx, etc.)
└── test/
    ├── aql_test_app.cpp        # Minimal HIP test: hipEventRecord + 10 kernel launches
    └── validate_dump.py        # Automated JSON output validation
```

## Build Options

| CMake Variable | Default | Description |
|---|---|---|
| `ROCM_PATH` | `/opt/rocm` | Path to ROCm installation (libraries + optional headers) |
| `ROCPROFILER_SDK_SOURCE_DIR` | `../projects/rocprofiler-sdk` | Fallback path to rocprofiler-sdk source tree for headers |
| `ROCR_RUNTIME_SOURCE_DIR` | `../projects/rocr-runtime/runtime/hsa-runtime` | Fallback path to rocr-runtime source tree for HSA headers |
| `ROCR_LIBHSAKMT_INCLUDE_DIR` | `../projects/rocr-runtime/libhsakmt/include` | Path to libhsakmt headers (needed by SDK headers) |
| `CMAKE_HIP_COMPILER` | auto-detected | Path to `amdclang++` (needed for test apps only) |

SQLite3 is detected via `find_package(SQLite3)`. If found, the tool is built with support for `.db` output; otherwise only JSON output is available.

### Building Against Installed ROCm

```bash
cmake .. -DROCM_PATH=/opt/rocm
```

### Building Against Source Tree (rocm-systems monorepo)

```bash
# Headers come from sibling projects/ directory, libraries from a ROCm build
cmake .. -DROCM_PATH=/path/to/rocm/build
```

The build system automatically generates a `version.h` from the SDK's template when building from source (since the SDK's own build hasn't been run).

## Environment Variables

| Variable | Description |
|---|---|
| `ROCP_TOOL_LIBRARIES` | Set to path of `libaql_dump_tool.so` to load the tool |
| `AQL_DUMP_OUTPUT` | Override output file path (default: `aql_dump_output.json`). Use a path ending in `.db` for SQLite output. |
| `LD_LIBRARY_PATH` | Must include `${ROCM_PATH}/lib` for HSA and HIP runtime libraries |

### Output formats

- **JSON** — Default. Set `AQL_DUMP_OUTPUT` to any path not ending in `.db` (e.g. `output.json`).
- **SQLite (.db)** — Set `AQL_DUMP_OUTPUT` to a path ending in `.db` (e.g. `output.db`). The tool must be built with SQLite3 (`find_package(SQLite3)`). The database contains a single table `aql_packets` with columns: `seq_num`, `packet_type`, `packet_type_name`, `queue_id`, `raw_dwords` (text), `gpu_timestamp_ns`, `kernel_object_addr`, `kernel_descriptor_dwords` (text).

## Troubleshooting

### "Signal time stamps may be invalid"

```
Signal 0x... time stamps may be invalid.
```

This is an HSA runtime warning (not from this tool) indicating that profiling timestamps for certain signals may be unreliable. It typically appears for signals that were very short-lived. The tool's own timestamps (collected from signals it creates) are generally reliable.

### "rocprofiler-sdk shared library not found"

The tool compiles with headers from the source tree but needs `librocprofiler-sdk64.so` at link time. Ensure `ROCM_PATH` points to a ROCm installation that includes the profiler SDK.

### "libamdhip64.so: cannot open shared object file"

Set `LD_LIBRARY_PATH` to include your ROCm `lib` directory:

```bash
export LD_LIBRARY_PATH=/opt/rocm/lib:$LD_LIBRARY_PATH
```

### Kernel descriptor reads show all zeros

This may occur on discrete GPUs where `kernel_object` points to device-only memory. The tool is primarily designed for APU targets (gfx1150/gfx1151) with unified memory.
