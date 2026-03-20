// MIT License
// Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
// AQL Dump Tool — rocprofiler-sdk based tool for capturing raw AQL packet
// streams, barrier timestamps, and kernel descriptor dumps.
//
// Architecture:
//   1. rocprofiler_configure() registers us as a tool
//   2. We intercept the HSA API table to wrap hsa_queue_create
//   3. For each created queue, we create an intercept (proxy) queue via
//      hsa_amd_queue_intercept_create and register our WriteInterceptor
//   4. WriteInterceptor processes ALL packet types (not just dispatches)
//   5. At exit, we write JSON output with raw packet dumps, barrier timestamps,
//      and kernel descriptor dumps.

#include "aql_dump_tool.h"

// Only include the specific rocprofiler-sdk headers we need.
// DO NOT include <rocprofiler-sdk/rocprofiler.h> — it pulls in hip/hip_runtime.h
// and other heavy dependencies we don't need.
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/intercept_table.h>
#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/defines.h>
#include <rocprofiler-sdk/version.h>

// HSA headers — flat includes, CMake sets up the correct paths
#include <hsa.h>
#include <hsa_ext_amd.h>
#include <hsa_api_trace.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#if defined(AQL_DUMP_TOOL_HAS_SQLITE) && AQL_DUMP_TOOL_HAS_SQLITE
#include <sqlite3.h>
#endif

// ---------------------------------------------------------------------------
// Macro helpers (two-level indirection needed for __LINE__ expansion with ##)
// ---------------------------------------------------------------------------
#define ROCPROFILER_VAR_NAME_COMBINE(X, Y) X##Y
#define ROCPROFILER_VARIABLE(X, Y)         ROCPROFILER_VAR_NAME_COMBINE(X, Y)

#define ROCPROFILER_CALL(result, msg)                                               \
    {                                                                               \
        rocprofiler_status_t ROCPROFILER_VARIABLE(_status, __LINE__) = (result);    \
        if(ROCPROFILER_VARIABLE(_status, __LINE__) != ROCPROFILER_STATUS_SUCCESS)   \
        {                                                                           \
            fprintf(stderr, "[aql_dump_tool] %s failed with status %d\n",           \
                    (msg), static_cast<int>(ROCPROFILER_VARIABLE(_status, __LINE__)));\
            abort();                                                                \
        }                                                                           \
    }

namespace aql_dump
{
// ---------------------------------------------------------------------------
// Singleton tool state
// ---------------------------------------------------------------------------
ToolState&
get_tool_state()
{
    static auto* state = new ToolState{};
    return *state;
}

// ---------------------------------------------------------------------------
// HSA function pointer storage (from the intercepted tables)
// We store both core_ and amd_ext_ table pointers so we can call original
// HSA functions from within our interceptors.
// ---------------------------------------------------------------------------
static CoreApiTable*  g_core_table   = nullptr;
static AmdExtTable*   g_amd_ext_table = nullptr;

// ---------------------------------------------------------------------------
// Async signal handler — chains our completion signal to the app's original.
// When OUR signal completes (GPU done), this handler fires and decrements
// the application's original signal to preserve HIP's synchronization.
// ---------------------------------------------------------------------------
static bool
barrier_chain_handler(hsa_signal_value_t /*value*/, void* arg)
{
    auto* bctx = static_cast<BarrierTimestampCtx*>(arg);

    // Decrement the application's original signal (if it had one).
    // This preserves hipEvent synchronization semantics.
    if(bctx->chain_needed && bctx->original_signal.handle != 0 && g_core_table)
    {
        g_core_table->hsa_signal_store_screlease_fn(bctx->original_signal, 0);
    }

    bctx->chain_done.store(true, std::memory_order_release);
    return false;  // Don't re-register
}

// ---------------------------------------------------------------------------
// Write Interceptor — called for EVERY packet written to an intercepted queue
// ---------------------------------------------------------------------------
static void
WriteInterceptor(const void* packets,
                 uint64_t    pkt_count,
                 uint64_t    /*user_pkt_index*/,
                 void*       data,
                 hsa_amd_queue_intercept_packet_writer writer)
{
    auto& state = get_tool_state();
    auto* ctx   = static_cast<QueueContext*>(data);

    const auto* raw = static_cast<const uint8_t*>(packets);

    for(uint64_t i = 0; i < pkt_count; ++i)
    {
        const uint8_t* pkt_bytes = raw + (i * 64);

        // Extract packet type from header bits 0-7
        uint16_t header      = 0;
        std::memcpy(&header, pkt_bytes, sizeof(header));
        uint8_t  packet_type = header & 0xFF;

        PacketRecord rec{};
        rec.seq_num     = state.seq_counter.fetch_add(1);
        rec.packet_type = packet_type;
        rec.queue_id    = ctx->queue_id;
        rec.has_kernel_descriptor  = false;
        rec.has_barrier_timestamp  = false;
        rec.kernel_object_addr     = 0;
        rec.gpu_timestamp_ns       = 0;

        // Copy raw 64 bytes (16 dwords)
        std::memcpy(rec.raw_dwords, pkt_bytes, 64);

        // --- KERNEL_DISPATCH (type 2): dereference kernel_object ---
        if(packet_type == HSA_PACKET_TYPE_KERNEL_DISPATCH)
        {
            const auto* dispatch =
                reinterpret_cast<const hsa_kernel_dispatch_packet_t*>(pkt_bytes);
            uint64_t kernel_obj_addr = dispatch->kernel_object;
            rec.kernel_object_addr = kernel_obj_addr;

            if(kernel_obj_addr != 0)
            {
                // kernel_object points to the kernel descriptor in GPU-accessible
                // memory. On APU / unified memory (gfx1150), this should be
                // directly readable from the host.
                const void* kd_ptr = reinterpret_cast<const void*>(kernel_obj_addr);
                std::memcpy(rec.kernel_descriptor_dwords, kd_ptr, 64);
                rec.has_kernel_descriptor = true;
            }
        }

        // --- BARRIER_AND (type 3) / BARRIER_OR (type 4): install timestamp signal ---
        //
        // Strategy: We ALWAYS create our own completion signal for barrier packets.
        // If the packet already had a signal (e.g. from hipEventRecord), we replace
        // it with ours and register an async handler that chains to the original
        // signal when our signal completes. This guarantees:
        //   1. Our signal is always valid until we destroy it (safe for fini reads)
        //   2. The application's signal still gets decremented (preserves semantics)
        //
        // Some HIP-created signals don't support hsa_amd_signal_async_handler
        // (return HSA_STATUS_ERROR_OUT_OF_RESOURCES). By using our own signal as
        // the packet's completion signal, the async handler always works.
        if(packet_type == HSA_PACKET_TYPE_BARRIER_AND ||
           packet_type == HSA_PACKET_TYPE_BARRIER_OR)
        {
            auto* mutable_pkt = const_cast<uint8_t*>(pkt_bytes);

            // Read original signal from packet (offset 56)
            hsa_signal_t original_signal = {};
            std::memcpy(&original_signal, mutable_pkt + 56, sizeof(hsa_signal_t));

            // Create our own signal
            hsa_signal_t our_signal = {};
            hsa_status_t sig_status = g_core_table->hsa_signal_create_fn(
                1, 0, nullptr, &our_signal);

            if(sig_status == HSA_STATUS_SUCCESS)
            {
                // Replace the packet's signal with ours
                std::memcpy(mutable_pkt + 56, &our_signal, sizeof(hsa_signal_t));

                rec.has_barrier_timestamp = true;

                // Create context for chain handler + timestamp collection
                auto* bctx = new BarrierTimestampCtx{};
                bctx->agent           = ctx->agent;
                bctx->our_signal      = our_signal;
                bctx->original_signal = original_signal;
                bctx->seq_num         = rec.seq_num;
                bctx->chain_needed    = (original_signal.handle != 0);

                // Register async handler on OUR signal (always works for user signals)
                // When the barrier completes (signal → 0), the handler fires and
                // chains to the original signal.
                if(bctx->chain_needed)
                {
                    hsa_status_t ah_status = g_amd_ext_table->hsa_amd_signal_async_handler_fn(
                        our_signal,
                        HSA_SIGNAL_CONDITION_LT,
                        1,
                        barrier_chain_handler,
                        bctx);

                    if(ah_status != HSA_STATUS_SUCCESS)
                    {
                        // Async handler failed — chain immediately at fini as fallback
                        fprintf(stderr, "[aql_dump_tool] WARNING: async chain handler failed "
                                "for barrier seq=%lu (status=%d), will chain at fini\n",
                                rec.seq_num, static_cast<int>(ah_status));
                    }
                }

                std::lock_guard<std::mutex> lock(state.mutex);
                state.barrier_ts_ctxs.push_back(bctx);
            }
        }

        // Store the record
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.records.push_back(rec);
        }
    }

    // Forward ALL packets (possibly modified) to the real hardware queue
    writer(packets, pkt_count);
}

// ---------------------------------------------------------------------------
// Wrapped hsa_queue_create — installs our interceptor on new queues
// ---------------------------------------------------------------------------
static hsa_status_t
wrapped_hsa_queue_create(hsa_agent_t       agent,
                         uint32_t          size,
                         hsa_queue_type32_t type,
                         void (*callback)(hsa_status_t status, hsa_queue_t* source, void* data),
                         void*             data,
                         uint32_t          private_segment_size,
                         uint32_t          group_segment_size,
                         hsa_queue_t**     queue)
{
    auto& state = get_tool_state();

    // Create an intercept (proxy) queue instead of a regular queue.
    // This is the same pattern the SDK uses internally.
    hsa_queue_t* intercept_queue = nullptr;
    hsa_status_t status = g_amd_ext_table->hsa_amd_queue_intercept_create_fn(
        agent, size, type, callback, data,
        private_segment_size, group_segment_size, &intercept_queue);

    if(status != HSA_STATUS_SUCCESS || intercept_queue == nullptr)
    {
        // Fallback: create a normal queue if intercept fails
        fprintf(stderr, "[aql_dump_tool] WARNING: intercept_create failed, falling back to normal queue\n");
        if(state.original_hsa_queue_create)
        {
            return state.original_hsa_queue_create(
                agent, size, type, callback, data,
                private_segment_size, group_segment_size, queue);
        }
        return status;
    }

    // Enable profiling on the queue (needed for accurate timestamps)
    g_amd_ext_table->hsa_amd_profiling_set_profiler_enabled_fn(intercept_queue, true);

    // Create a context for this queue
    auto* qctx = new QueueContext{};
    qctx->queue    = intercept_queue;
    qctx->queue_id = reinterpret_cast<uint64_t>(intercept_queue);
    qctx->agent    = agent;

    // Register our write interceptor
    status = g_amd_ext_table->hsa_amd_queue_intercept_register_fn(
        intercept_queue, WriteInterceptor, qctx);

    if(status != HSA_STATUS_SUCCESS)
    {
        fprintf(stderr, "[aql_dump_tool] WARNING: Failed to register write interceptor\n");
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.queues.push_back(*qctx);
    }

    *queue = intercept_queue;
    return HSA_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Collect barrier timestamps — called at tool_fini after workload completes.
// Reads GPU timestamps from our owned signals and cleans up.
// ---------------------------------------------------------------------------
static void
collect_barrier_timestamps()
{
    if(!g_core_table || !g_amd_ext_table) return;

    auto& state = get_tool_state();

    // Build a lookup from seq_num -> record index for fast matching
    std::unordered_map<uint64_t, size_t> seq_to_idx;
    for(size_t i = 0; i < state.records.size(); ++i)
        seq_to_idx[state.records[i].seq_num] = i;

    for(auto* bctx : state.barrier_ts_ctxs)
    {
        if(!bctx) continue;

        // Wait for our signal to complete (should already be done at fini time)
        hsa_signal_value_t val =
            g_core_table->hsa_signal_load_relaxed_fn(bctx->our_signal);

        if(val <= 0)
        {
            // Signal completed — read the profiling timestamp
            hsa_amd_profiling_dispatch_time_t time = {};
            hsa_status_t status = g_amd_ext_table->hsa_amd_profiling_get_dispatch_time_fn(
                bctx->agent, bctx->our_signal, &time);

            if(status == HSA_STATUS_SUCCESS)
            {
                auto it = seq_to_idx.find(bctx->seq_num);
                if(it != seq_to_idx.end())
                {
                    state.records[it->second].gpu_timestamp_ns = time.end;
                }
            }
        }

        // If the chain handler didn't fire yet, decrement original signal now
        if(bctx->chain_needed &&
           !bctx->chain_done.load(std::memory_order_acquire) &&
           bctx->original_signal.handle != 0)
        {
            g_core_table->hsa_signal_store_screlease_fn(bctx->original_signal, 0);
        }

        // Destroy our signal (we always own it)
        g_core_table->hsa_signal_destroy_fn(bctx->our_signal);

        delete bctx;
    }
    state.barrier_ts_ctxs.clear();
}

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------
void
write_json_output()
{
    auto& state = get_tool_state();

    std::ofstream ofs(state.output_path);
    if(!ofs.is_open())
    {
        fprintf(stderr, "[aql_dump_tool] ERROR: Cannot open output file: %s\n",
                state.output_path.c_str());
        return;
    }

    ofs << "[\n";
    for(size_t i = 0; i < state.records.size(); ++i)
    {
        const auto& rec = state.records[i];
        if(i > 0) ofs << ",\n";

        ofs << "  {\n";
        ofs << "    \"seq_num\": " << rec.seq_num << ",\n";
        ofs << "    \"packet_type\": " << static_cast<int>(rec.packet_type) << ",\n";
        ofs << "    \"packet_type_name\": \"" << packet_type_name(rec.packet_type) << "\",\n";
        ofs << "    \"queue_id\": " << rec.queue_id << ",\n";

        // Raw dwords
        ofs << "    \"raw_dwords\": [";
        for(int d = 0; d < 16; ++d)
        {
            if(d > 0) ofs << ", ";
            ofs << "\"0x" << std::hex << std::setw(8) << std::setfill('0')
                << rec.raw_dwords[d] << "\"";
        }
        ofs << std::dec << "],\n";

        // GPU timestamp
        if(rec.has_barrier_timestamp && rec.gpu_timestamp_ns > 0)
            ofs << "    \"gpu_timestamp_ns\": " << rec.gpu_timestamp_ns << ",\n";
        else
            ofs << "    \"gpu_timestamp_ns\": null,\n";

        // Kernel descriptor
        if(rec.has_kernel_descriptor)
        {
            ofs << "    \"kernel_object_addr\": \"0x" << std::hex << rec.kernel_object_addr
                << std::dec << "\",\n";
            ofs << "    \"kernel_descriptor_dwords\": [";
            for(int d = 0; d < 16; ++d)
            {
                if(d > 0) ofs << ", ";
                ofs << "\"0x" << std::hex << std::setw(8) << std::setfill('0')
                    << rec.kernel_descriptor_dwords[d] << "\"";
            }
            ofs << std::dec << "]\n";
        }
        else
        {
            ofs << "    \"kernel_object_addr\": null,\n";
            ofs << "    \"kernel_descriptor_dwords\": null\n";
        }

        ofs << "  }";
    }
    ofs << "\n]\n";

    ofs.close();
    fprintf(stderr, "[aql_dump_tool] Wrote %zu packet records to %s\n",
            state.records.size(), state.output_path.c_str());
}

// ---------------------------------------------------------------------------
// SQLite .db output (optional, when built with SQLite3)
// ---------------------------------------------------------------------------
#if defined(AQL_DUMP_TOOL_HAS_SQLITE) && AQL_DUMP_TOOL_HAS_SQLITE
void
write_db_output()
{
    auto& state = get_tool_state();
    // output_path is set from AQL_DUMP_OUTPUT in tool_fini before we are called

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(state.output_path.c_str(),
                            &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr);
    if(rc != SQLITE_OK || db == nullptr)
    {
        fprintf(stderr, "[aql_dump_tool] ERROR: Cannot open SQLite DB: %s (%s)\n",
                state.output_path.c_str(),
                db ? sqlite3_errmsg(db) : "open failed");
        if(db) sqlite3_close(db);
        return;
    }

    const char* create_sql =
        "CREATE TABLE IF NOT EXISTS aql_packets (\n"
        "  seq_num INTEGER NOT NULL,\n"
        "  packet_type INTEGER NOT NULL,\n"
        "  packet_type_name TEXT NOT NULL,\n"
        "  queue_id INTEGER NOT NULL,\n"
        "  raw_dwords TEXT NOT NULL,\n"
        "  gpu_timestamp_ns INTEGER,\n"
        "  kernel_object_addr TEXT,\n"
        "  kernel_descriptor_dwords TEXT\n"
        ");";
    char* errmsg = nullptr;
    rc = sqlite3_exec(db, create_sql, nullptr, nullptr, &errmsg);
    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "[aql_dump_tool] ERROR: CREATE TABLE failed: %s\n", errmsg ? errmsg : "");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* insert_sql =
        "INSERT INTO aql_packets(seq_num, packet_type, packet_type_name, queue_id, "
        "raw_dwords, gpu_timestamp_ns, kernel_object_addr, kernel_descriptor_dwords) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "[aql_dump_tool] ERROR: INSERT prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    for(const auto& rec : state.records)
    {
        // raw_dwords as comma-separated hex
        std::ostringstream raw_ss;
        for(int d = 0; d < 16; ++d)
        {
            if(d > 0) raw_ss << ",";
            raw_ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << rec.raw_dwords[d];
        }
        std::string raw_str = raw_ss.str();

        std::string kernel_addr_str;
        std::string kernel_desc_str;
        if(rec.has_kernel_descriptor)
        {
            std::ostringstream addr_ss;
            addr_ss << "0x" << std::hex << rec.kernel_object_addr;
            kernel_addr_str = addr_ss.str();
            std::ostringstream desc_ss;
            for(int d = 0; d < 16; ++d)
            {
                if(d > 0) desc_ss << ",";
                desc_ss << "0x" << std::hex << std::setw(8) << std::setfill('0')
                        << rec.kernel_descriptor_dwords[d];
            }
            kernel_desc_str = desc_ss.str();
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rec.seq_num));
        sqlite3_bind_int(stmt, 2, static_cast<int>(rec.packet_type));
        sqlite3_bind_text(stmt, 3, packet_type_name(rec.packet_type), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(rec.queue_id));
        sqlite3_bind_text(stmt, 5, raw_str.c_str(), -1, SQLITE_TRANSIENT);
        if(rec.has_barrier_timestamp && rec.gpu_timestamp_ns > 0)
            sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(rec.gpu_timestamp_ns));
        else
            sqlite3_bind_null(stmt, 6);
        if(rec.has_kernel_descriptor)
        {
            sqlite3_bind_text(stmt, 7, kernel_addr_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, kernel_desc_str.c_str(), -1, SQLITE_TRANSIENT);
        }
        else
        {
            sqlite3_bind_null(stmt, 7);
            sqlite3_bind_null(stmt, 8);
        }

        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE)
        {
            fprintf(stderr, "[aql_dump_tool] ERROR: INSERT failed: %s\n", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return;
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    fprintf(stderr, "[aql_dump_tool] Wrote %zu packet records to %s (SQLite)\n",
            state.records.size(), state.output_path.c_str());
}
#endif  // AQL_DUMP_TOOL_HAS_SQLITE

// ---------------------------------------------------------------------------
// HSA API table interception callback
// ---------------------------------------------------------------------------
static void
hsa_table_registration_callback(rocprofiler_intercept_table_t type,
                                uint64_t                      lib_version,
                                uint64_t                      lib_instance,
                                void**                        tables,
                                uint64_t                      num_tables,
                                void*                         user_data)
{
    (void) lib_version;
    (void) lib_instance;
    (void) user_data;

    if(type != ROCPROFILER_HSA_TABLE) return;
    if(num_tables < 1 || tables == nullptr) return;

    auto* hsa_api_table = static_cast<HsaApiTable*>(tables[0]);

    // Save the original table pointers so we can call original HSA functions
    g_core_table    = hsa_api_table->core_;
    g_amd_ext_table = hsa_api_table->amd_ext_;

    // Save original hsa_queue_create so we can call it as fallback
    get_tool_state().original_hsa_queue_create = hsa_api_table->core_->hsa_queue_create_fn;

    // Replace hsa_queue_create with our wrapper.
    // When HIP (or any HSA client) calls hsa_queue_create, our wrapper runs
    // instead. We create an intercept queue and register our write interceptor.
    hsa_api_table->core_->hsa_queue_create_fn = wrapped_hsa_queue_create;
}

// ---------------------------------------------------------------------------
// rocprofiler tool_init / tool_fini callbacks
// ---------------------------------------------------------------------------
static int
tool_init(rocprofiler_client_finalize_t fini_func, void* tool_data)
{
    (void) fini_func;
    (void) tool_data;
    // Initialization is handled by the HSA table interception callback
    // which is registered in rocprofiler_configure().
    fprintf(stderr, "[aql_dump_tool] Tool initialized — intercepting all AQL packet types\n");
    return 0;  // success
}

static bool
path_ends_with(const std::string& path, const char* suffix)
{
    size_t plen = path.length();
    size_t slen = std::strlen(suffix);
    return plen >= slen && path.compare(plen - slen, slen, suffix) == 0;
}

static void
tool_fini(void* tool_data)
{
    (void) tool_data;

    fprintf(stderr, "[aql_dump_tool] Tool finalizing — collecting timestamps and writing output\n");

    // Collect GPU timestamps from barrier completion signals
    collect_barrier_timestamps();

    auto& state = get_tool_state();
    if(const char* env = std::getenv("AQL_DUMP_OUTPUT"))
    {
        state.output_path = env;
    }

    if(path_ends_with(state.output_path, ".db"))
    {
#if defined(AQL_DUMP_TOOL_HAS_SQLITE) && AQL_DUMP_TOOL_HAS_SQLITE
        write_db_output();
#else
        fprintf(stderr, "[aql_dump_tool] WARNING: .db output requested but SQLite3 not available; "
                        "writing JSON instead. Set AQL_DUMP_OUTPUT to a .json path or build with SQLite3.\n");
        if(state.output_path.size() >= 3)
        {
            state.output_path.replace(state.output_path.size() - 3, 3, "json");
        }
        write_json_output();
#endif
    }
    else
    {
        write_json_output();
    }
}

}  // namespace aql_dump

// ===========================================================================
// rocprofiler_configure — entry point called by rocprofiler-sdk runtime
// ===========================================================================
extern "C" rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t                 version,
                      const char*              runtime_version,
                      uint32_t                 priority,
                      rocprofiler_client_id_t* id)
{
    // Set the client name
    id->name = "AqlDumpTool";

    // Log version info
    uint32_t major = version / 10000;
    uint32_t minor = (version % 10000) / 100;
    uint32_t patch = version % 100;
    fprintf(stderr,
            "[aql_dump_tool] %s (priority=%u) using rocprofiler-sdk v%u.%u.%u (%s)\n",
            id->name, priority, major, minor, patch, runtime_version);

    // Register for HSA API table interception.
    // When the HSA runtime initializes and provides its function pointer table,
    // our callback will be invoked, allowing us to wrap hsa_queue_create.
    ROCPROFILER_CALL(
        rocprofiler_at_intercept_table_registration(
            aql_dump::hsa_table_registration_callback,
            ROCPROFILER_HSA_TABLE,
            nullptr),
        "HSA table registration");

    // Create and return the configuration result
    static auto cfg = rocprofiler_tool_configure_result_t{
        sizeof(rocprofiler_tool_configure_result_t),
        &aql_dump::tool_init,
        &aql_dump::tool_fini,
        nullptr  // tool_data (we use the singleton ToolState instead)
    };

    return &cfg;
}
