// MIT License
// Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
// AQL Dump Tool — captures raw AQL packet streams with barrier timestamps
// and kernel descriptor dumps.

#pragma once

// HSA headers — included without hsa/ prefix for source-tree compatibility.
// CMake sets up the include path to resolve these correctly whether building
// against installed ROCm (/opt/rocm/include/hsa/) or source tree (rocr-runtime/inc/).
#include <hsa.h>
#include <hsa_ext_amd.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aql_dump
{
/// Human-readable names for HSA AQL packet types
inline const char*
packet_type_name(uint8_t type)
{
    switch(type)
    {
        case 0: return "VENDOR_SPECIFIC";
        case 1: return "INVALID";
        case 2: return "KERNEL_DISPATCH";
        case 3: return "BARRIER_AND";
        case 4: return "BARRIER_OR";
        case 5: return "AGENT_DISPATCH";
        default: return "UNKNOWN";
    }
}

/// Record captured for each AQL packet observed in the queue
struct PacketRecord
{
    uint64_t    seq_num;                     ///< Sequential packet number (global)
    uint8_t     packet_type;                 ///< HSA packet type (bits 0-7 of header)
    uint64_t    queue_id;                    ///< Queue identifier (from hsa_queue_t*)
    uint32_t    raw_dwords[16];              ///< Raw 64 bytes of the AQL packet
    // Kernel descriptor (only for KERNEL_DISPATCH packets)
    bool        has_kernel_descriptor;
    uint32_t    kernel_descriptor_dwords[16]; ///< 64 bytes from kernel_object pointer
    uint64_t    kernel_object_addr;          ///< The kernel_object pointer value
    // Barrier timestamp (only for BARRIER_AND / BARRIER_OR packets)
    bool        has_barrier_timestamp;
    uint64_t    gpu_timestamp_ns;            ///< GPU-side completion timestamp
};

/// Context for barrier timestamp collection.
/// Allocated with new; cleaned up in tool_fini.
///
/// Strategy: We ALWAYS create our own signal for every barrier packet.
/// If the barrier already had an original signal (from HIP events), we
/// install ours instead and chain: when our signal completes, an async
/// handler decrements the original signal. This way:
///   - Our signal is always valid until we destroy it (safe for fini reads)
///   - The application's signal still gets decremented (preserves semantics)
struct BarrierTimestampCtx
{
    hsa_agent_t              agent;
    hsa_signal_t             our_signal;     ///< Signal WE created (installed in packet)
    hsa_signal_t             original_signal;///< App's original signal (may be {0})
    uint64_t                 seq_num;        ///< Matches PacketRecord::seq_num
    bool                     chain_needed;   ///< If true, we must decrement original_signal
    std::atomic<bool>        chain_done{false}; ///< Set by async handler
};

/// Per-queue context for the write interceptor
struct QueueContext
{
    hsa_queue_t* queue;      ///< The intercept queue handle
    uint64_t     queue_id;   ///< Numeric queue ID for output
    hsa_agent_t  agent;      ///< Agent this queue belongs to (needed for timestamp API)
};

/// Global state — thread-safe, accumulates all packet records
struct ToolState
{
    std::mutex                 mutex;
    std::vector<PacketRecord>  records;
    std::vector<QueueContext>  queues;
    std::atomic<uint64_t>      seq_counter{0};

    // Barrier timestamp contexts (allocated with new, cleaned up in tool_fini)
    std::vector<BarrierTimestampCtx*> barrier_ts_ctxs;

    // Saved original hsa_queue_create function pointer (for fallback)
    decltype(hsa_queue_create)* original_hsa_queue_create = nullptr;

    // Output file path
    std::string output_path = "aql_dump_output.json";
};

/// Get the singleton tool state
ToolState&
get_tool_state();

/// Write all captured records to JSON file
void
write_json_output();

}  // namespace aql_dump
