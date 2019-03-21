/*
 * Copyright (C) 2014 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "log.hh"
#include "lister.hh"
#include "database.hh"
#include "unimplemented.hh"
#include <seastar/core/future-util.hh>
#include "db/commitlog/commitlog_entry.hh"
#include "db/system_keyspace.hh"
#include "db/commitlog/commitlog.hh"
#include "db/config.hh"
#include "to_string.hh"
#include "query-result-writer.hh"
#include "cql3/column_identifier.hh"
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/rwlock.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/util/defer.hh>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include "sstables/sstables.hh"
#include "sstables/sstables_manager.hh"
#include "sstables/compaction.hh"
#include "sstables/remove.hh"
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include "locator/simple_snitch.hh"
#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/function_output_iterator.hpp>
#include <boost/range/algorithm/heap_algorithm.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/adaptor/map.hpp>
#include "frozen_mutation.hh"
#include "mutation_partition_applier.hh"
#include <seastar/core/do_with.hh>
#include "service/migration_manager.hh"
#include "service/storage_service.hh"
#include "message/messaging_service.hh"
#include "mutation_query.hh"
#include <seastar/core/fstream.hh>
#include <seastar/core/enum.hh>
#include "utils/latency.hh"
#include "schema_registry.hh"
#include "service/priority_manager.hh"
#include "cell_locking.hh"
#include "db/view/row_locking.hh"
#include "view_info.hh"
#include "memtable-sstable.hh"
#include "db/schema_tables.hh"
#include "db/query_context.hh"
#include "sstables/compaction_manager.hh"
#include "sstables/compaction_backlog_manager.hh"
#include "sstables/progress_monitor.hh"
#include "auth/common.hh"
#include "tracing/trace_keyspace_helper.hh"

#include "checked-file-impl.hh"
#include "disk-error-handler.hh"

#include "db/timeout_clock.hh"

#include "db/data_listeners.hh"
#include "distributed_loader.hh"

#include "user_types_metadata.hh"
#include <seastar/core/shared_ptr_incomplete.hh>

using namespace std::chrono_literals;

logging::logger dblog("database");

namespace seastar {

void
lw_shared_ptr_deleter<user_types_metadata>::dispose(user_types_metadata* o) {
    delete o;
}

}

template
user_types_metadata*
seastar::internal::lw_shared_ptr_accessors<user_types_metadata, void>::to_value(seastar::lw_shared_ptr_counter_base*);

sstables::sstable::version_types get_highest_supported_format() {
    if (service::get_local_storage_service().cluster_supports_mc_sstable()) {
        return sstables::sstable::version_types::mc;
    } else if (service::get_local_storage_service().cluster_supports_la_sstable()) {
        return sstables::sstable::version_types::la;
    } else {
        return sstables::sstable::version_types::ka;
    }
}

// Used for tests where the CF exists without a database object. We need to pass a valid
// dirty_memory manager in that case.
thread_local dirty_memory_manager default_dirty_memory_manager;

inline
flush_controller
make_flush_controller(db::config& cfg, seastar::scheduling_group sg, const ::io_priority_class& iop, std::function<double()> fn) {
    if (cfg.memtable_flush_static_shares() > 0) {
        return flush_controller(sg, iop, cfg.memtable_flush_static_shares());
    }
    return flush_controller(sg, iop, 50ms, cfg.virtual_dirty_soft_limit(), std::move(fn));
}

inline
std::unique_ptr<compaction_manager>
make_compaction_manager(db::config& cfg, database_config& dbcfg) {
    if (cfg.compaction_static_shares() > 0) {
        return std::make_unique<compaction_manager>(dbcfg.compaction_scheduling_group, service::get_local_compaction_priority(), dbcfg.available_memory, cfg.compaction_static_shares());
    }
    return std::make_unique<compaction_manager>(dbcfg.compaction_scheduling_group, service::get_local_compaction_priority(), dbcfg.available_memory);
}

const lw_shared_ptr<user_types_metadata>& keyspace_metadata::user_types() const {
    return _user_types;
}

lw_shared_ptr<keyspace_metadata>
keyspace_metadata::new_keyspace(sstring name,
                                sstring strategy_name,
                                std::map<sstring, sstring> options,
                                bool durables_writes,
                                std::vector<schema_ptr> cf_defs)
{
    return ::make_lw_shared<keyspace_metadata>(name, strategy_name, options, durables_writes, cf_defs);
}

void keyspace_metadata::add_user_type(const user_type ut) {
    _user_types->add_type(ut);
}

void keyspace_metadata::remove_user_type(const user_type ut) {
    _user_types->remove_type(ut);
}

keyspace::keyspace(lw_shared_ptr<keyspace_metadata> metadata, config cfg)
    : _metadata(std::move(metadata))
    , _config(std::move(cfg))
{}

lw_shared_ptr<keyspace_metadata> keyspace::metadata() const {
    return _metadata;
}

void keyspace::add_or_update_column_family(const schema_ptr& s) {
    _metadata->add_or_update_column_family(s);
}

void keyspace::add_user_type(const user_type ut) {
    _metadata->add_user_type(ut);
}

void keyspace::remove_user_type(const user_type ut) {
    _metadata->remove_user_type(ut);
}

utils::UUID database::empty_version = utils::UUID_gen::get_name_UUID(bytes{});

database::database() : database(db::config(), database_config())
{}

database::database(const db::config& cfg, database_config dbcfg)
    : _stats(make_lw_shared<db_stats>())
    , _cl_stats(std::make_unique<cell_locker_stats>())
    , _cfg(std::make_unique<db::config>(cfg))
    // Allow system tables a pool of 10 MB memory to write, but never block on other regions.
    , _system_dirty_memory_manager(*this, 10 << 20, cfg.virtual_dirty_soft_limit(), default_scheduling_group())
    , _dirty_memory_manager(*this, dbcfg.available_memory * 0.45, cfg.virtual_dirty_soft_limit(), dbcfg.statement_scheduling_group)
    , _streaming_dirty_memory_manager(*this, dbcfg.available_memory * 0.10, cfg.virtual_dirty_soft_limit(), dbcfg.streaming_scheduling_group)
    , _dbcfg(dbcfg)
    , _memtable_controller(make_flush_controller(*_cfg, dbcfg.memtable_scheduling_group, service::get_local_memtable_flush_priority(), [this, limit = float(_dirty_memory_manager.throttle_threshold())] {
        auto backlog = (_dirty_memory_manager.virtual_dirty_memory()) / limit;
        if (_dirty_memory_manager.has_extraneous_flushes_requested()) {
            backlog = std::max(backlog, _memtable_controller.backlog_of_shares(200));
        }
        return backlog;
    }))
    , _read_concurrency_sem(max_count_concurrent_reads,
        max_memory_concurrent_reads(),
        max_inactive_queue_length(),
        [this] {
            ++_stats->sstable_read_queue_overloaded;
            return std::make_exception_ptr(std::runtime_error("sstable inactive read queue overloaded"));
        })
    // No timeouts or queue length limits - a failure here can kill an entire repair.
    // Trust the caller to limit concurrency.
    , _streaming_concurrency_sem(max_count_streaming_concurrent_reads, max_memory_streaming_concurrent_reads())
    , _system_read_concurrency_sem(max_count_system_concurrent_reads, max_memory_system_concurrent_reads())
    , _data_query_stage("data_query", &column_family::query)
    , _mutation_query_stage()
    , _apply_stage("db_apply", &database::do_apply)
    , _version(empty_version)
    , _compaction_manager(make_compaction_manager(*_cfg, dbcfg))
    , _enable_incremental_backups(cfg.incremental_backups())
    , _querier_cache(_read_concurrency_sem, dbcfg.available_memory * 0.04)
    , _large_data_handler(std::make_unique<db::cql_table_large_data_handler>(_cfg->compaction_large_partition_warning_threshold_mb()*1024*1024,
              _cfg->compaction_large_row_warning_threshold_mb()*1024*1024,
              _cfg->compaction_large_cell_warning_threshold_mb()*1024*1024))
    , _nop_large_data_handler(std::make_unique<db::nop_large_data_handler>())
    , _sstables_manager(std::make_unique<sstables::sstables_manager>())
    , _result_memory_limiter(dbcfg.available_memory / 10)
    , _data_listeners(std::make_unique<db::data_listeners>(*this))
{
    local_schema_registry().init(*this); // TODO: we're never unbound.
    setup_metrics();

    _row_cache_tracker.set_compaction_scheduling_group(dbcfg.memory_compaction_scheduling_group);

    dblog.debug("Row: max_vector_size: {}, internal_count: {}", size_t(row::max_vector_size), size_t(row::internal_count));
}

const db::extensions& database::extensions() const {
    return get_config().extensions();
}

void backlog_controller::adjust() {
    auto backlog = _current_backlog();

    if (backlog >= _control_points.back().input) {
        update_controller(_control_points.back().output);
        return;
    }

    // interpolate to find out which region we are. This run infrequently and there are a fixed
    // number of points so a simple loop will do.
    size_t idx = 1;
    while ((idx < _control_points.size() - 1) && (_control_points[idx].input < backlog)) {
        idx++;
    }

    control_point& cp = _control_points[idx];
    control_point& last = _control_points[idx - 1];
    float result = last.output + (backlog - last.input) * (cp.output - last.output)/(cp.input - last.input);
    update_controller(result);
}

float backlog_controller::backlog_of_shares(float shares) const {
    size_t idx = 1;
    while ((idx < _control_points.size() - 1) && (_control_points[idx].output < shares)) {
        idx++;
    }
    const control_point& cp = _control_points[idx];
    const control_point& last = _control_points[idx - 1];
    // Compute the inverse function of the backlog in the interpolation interval that we fall
    // into.
    //
    // The formula for the backlog inside an interpolation point is y = a + bx, so the inverse
    // function is x = (y - a) / b

    return last.input + (shares - last.output) * (cp.input - last.input) / (cp.output - last.output);
}

void backlog_controller::update_controller(float shares) {
    _scheduling_group.set_shares(shares);
    if (!_inflight_update.available()) {
        return; // next timer will fix it
    }
    _inflight_update = engine().update_shares_for_class(_io_priority, uint32_t(shares));
}

void
dirty_memory_manager::setup_collectd(sstring namestr) {
    namespace sm = seastar::metrics;

    _metrics.add_group("memory", {
        sm::make_gauge(namestr + "_dirty_bytes", [this] { return real_dirty_memory(); },
                       sm::description("Holds the current size of a all non-free memory in bytes: used memory + released memory that hasn't been returned to a free memory pool yet. "
                                       "Total memory size minus this value represents the amount of available memory. "
                                       "If this value minus virtual_dirty_bytes is too high then this means that the dirty memory eviction lags behind.")),

        sm::make_gauge(namestr +"_virtual_dirty_bytes", [this] { return virtual_dirty_memory(); },
                       sm::description("Holds the size of used memory in bytes. Compare it to \"dirty_bytes\" to see how many memory is wasted (neither used nor available).")),
    });
}

static const metrics::label class_label("class");

void
database::setup_metrics() {
    _dirty_memory_manager.setup_collectd("regular");
    _system_dirty_memory_manager.setup_collectd("system");
    _streaming_dirty_memory_manager.setup_collectd("streaming");

    namespace sm = seastar::metrics;

    auto user_label_instance = class_label("user");
    auto streaming_label_instance = class_label("streaming");
    auto system_label_instance = class_label("system");

    _metrics.add_group("memory", {
        sm::make_gauge("dirty_bytes", [this] { return _dirty_memory_manager.real_dirty_memory() + _system_dirty_memory_manager.real_dirty_memory() + _streaming_dirty_memory_manager.real_dirty_memory(); },
                       sm::description("Holds the current size of all (\"regular\", \"system\" and \"streaming\") non-free memory in bytes: used memory + released memory that hasn't been returned to a free memory pool yet. "
                                       "Total memory size minus this value represents the amount of available memory. "
                                       "If this value minus virtual_dirty_bytes is too high then this means that the dirty memory eviction lags behind.")),

        sm::make_gauge("virtual_dirty_bytes", [this] { return _dirty_memory_manager.virtual_dirty_memory() + _system_dirty_memory_manager.virtual_dirty_memory() + _streaming_dirty_memory_manager.virtual_dirty_memory(); },
                       sm::description("Holds the size of all (\"regular\", \"system\" and \"streaming\") used memory in bytes. Compare it to \"dirty_bytes\" to see how many memory is wasted (neither used nor available).")),
    });

    _metrics.add_group("memtables", {
        sm::make_gauge("pending_flushes", _cf_stats.pending_memtables_flushes_count,
                       sm::description("Holds the current number of memtables that are currently being flushed to sstables. "
                                       "High value in this metric may be an indication of storage being a bottleneck.")),

        sm::make_gauge("pending_flushes_bytes", _cf_stats.pending_memtables_flushes_bytes,
                       sm::description("Holds the current number of bytes in memtables that are currently being flushed to sstables. "
                                       "High value in this metric may be an indication of storage being a bottleneck.")),
    });

    _metrics.add_group("database", {
        sm::make_gauge("requests_blocked_memory_current", [this] { return _dirty_memory_manager.region_group().blocked_requests(); },
                       sm::description(
                           seastar::format("Holds the current number of requests blocked due to reaching the memory quota ({}B). "
                                           "Non-zero value indicates that our bottleneck is memory and more specifically - the memory quota allocated for the \"database\" component.", _dirty_memory_manager.throttle_threshold()))),

        sm::make_derive("requests_blocked_memory", [this] { return _dirty_memory_manager.region_group().blocked_requests_counter(); },
                       sm::description(seastar::format("Holds the current number of requests blocked due to reaching the memory quota ({}B). "
                                       "Non-zero value indicates that our bottleneck is memory and more specifically - the memory quota allocated for the \"database\" component.", _dirty_memory_manager.throttle_threshold()))),

        sm::make_derive("clustering_filter_count", _cf_stats.clustering_filter_count,
                       sm::description("Counts bloom filter invocations.")),

        sm::make_derive("clustering_filter_sstables_checked", _cf_stats.sstables_checked_by_clustering_filter,
                       sm::description("Counts sstables checked after applying the bloom filter. "
                                       "High value indicates that bloom filter is not very efficient.")),

        sm::make_derive("clustering_filter_fast_path_count", _cf_stats.clustering_filter_fast_path_count,
                       sm::description("Counts number of times bloom filtering short cut to include all sstables when only one full range was specified.")),

        sm::make_derive("clustering_filter_surviving_sstables", _cf_stats.surviving_sstables_after_clustering_filter,
                       sm::description("Counts sstables that survived the clustering key filtering. "
                                       "High value indicates that bloom filter is not very efficient and still have to access a lot of sstables to get data.")),

        sm::make_derive("dropped_view_updates", _cf_stats.dropped_view_updates,
                       sm::description("Counts the number of view updates that have been dropped due to cluster overload. ")),

       sm::make_derive("view_building_paused", _cf_stats.view_building_paused,
                      sm::description("Counts the number of times view building process was paused (e.g. due to node unavailability). ")),

        sm::make_derive("total_writes", _stats->total_writes,
                       sm::description("Counts the total number of successful write operations performed by this shard.")),

        sm::make_derive("total_writes_failed", _stats->total_writes_failed,
                       sm::description("Counts the total number of failed write operations. "
                                       "A sum of this value plus total_writes represents a total amount of writes attempted on this shard.")),

        sm::make_derive("total_writes_timedout", _stats->total_writes_timedout,
                       sm::description("Counts write operations failed due to a timeout. A positive value is a sign of storage being overloaded.")),

        sm::make_derive("total_reads", _stats->total_reads,
                       sm::description("Counts the total number of successful reads on this shard.")),

        sm::make_derive("total_reads_failed", _stats->total_reads_failed,
                       sm::description("Counts the total number of failed read operations. "
                                       "Add the total_reads to this value to get the total amount of reads issued on this shard.")),

        sm::make_current_bytes("view_update_backlog", [this] { return get_view_update_backlog().current; },
                       sm::description("Holds the current size in bytes of the pending view updates for all tables")),

        sm::make_derive("querier_cache_lookups", _querier_cache.get_stats().lookups,
                       sm::description("Counts querier cache lookups (paging queries)")),

        sm::make_derive("querier_cache_misses", _querier_cache.get_stats().misses,
                       sm::description("Counts querier cache lookups that failed to find a cached querier")),

        sm::make_derive("querier_cache_drops", _querier_cache.get_stats().drops,
                       sm::description("Counts querier cache lookups that found a cached querier but had to drop it due to position mismatch")),

        sm::make_derive("querier_cache_time_based_evictions", _querier_cache.get_stats().time_based_evictions,
                       sm::description("Counts querier cache entries that timed out and were evicted.")),

        sm::make_derive("querier_cache_resource_based_evictions", _querier_cache.get_stats().resource_based_evictions,
                       sm::description("Counts querier cache entries that were evicted to free up resources "
                                       "(limited by reader concurency limits) necessary to create new readers.")),

        sm::make_derive("querier_cache_memory_based_evictions", _querier_cache.get_stats().memory_based_evictions,
                       sm::description("Counts querier cache entries that were evicted because the memory usage "
                                       "of the cached queriers were above the limit.")),

        sm::make_gauge("querier_cache_population", _querier_cache.get_stats().population,
                       sm::description("The number of entries currently in the querier cache.")),

        sm::make_derive("sstable_read_queue_overloads", _stats->sstable_read_queue_overloaded,
                       sm::description("Counts the number of times the sstable read queue was overloaded. "
                                       "A non-zero value indicates that we have to drop read requests because they arrive faster than we can serve them.")),

        sm::make_gauge("active_reads", [this] { return max_count_concurrent_reads - _read_concurrency_sem.available_resources().count; },
                       sm::description("Holds the number of currently active read operations. "),
                       {user_label_instance}),

        sm::make_gauge("active_reads_memory_consumption", [this] { return max_memory_concurrent_reads() - _read_concurrency_sem.available_resources().memory; },
                       sm::description(seastar::format("Holds the amount of memory consumed by currently active read operations. "
                                                       "If this value gets close to {} we are likely to start dropping new read requests. "
                                                       "In that case sstable_read_queue_overloads is going to get a non-zero value.", max_memory_concurrent_reads())),
                       {user_label_instance}),

        sm::make_gauge("queued_reads", [this] { return _read_concurrency_sem.waiters(); },
                       sm::description("Holds the number of currently queued read operations."),
                       {user_label_instance}),

        sm::make_gauge("paused_reads", _read_concurrency_sem.get_inactive_read_stats().population,
                       sm::description("The number of currently active reads that are temporarily paused."),
                       {user_label_instance}),

        sm::make_derive("paused_reads_permit_based_evictions", _read_concurrency_sem.get_inactive_read_stats().permit_based_evictions,
                       sm::description("The number of paused reads evicted to free up permits."
                                       " Permits are required for new reads to start, and the database will evict paused reads (if any)"
                                       " to be able to admit new ones, if there is a shortage of permits."),
                       {user_label_instance}),

        sm::make_gauge("active_reads", [this] { return max_count_streaming_concurrent_reads - _streaming_concurrency_sem.available_resources().count; },
                       sm::description("Holds the number of currently active read operations issued on behalf of streaming "),
                       {streaming_label_instance}),


        sm::make_gauge("active_reads_memory_consumption", [this] { return max_memory_streaming_concurrent_reads() - _streaming_concurrency_sem.available_resources().memory; },
                       sm::description(seastar::format("Holds the amount of memory consumed by currently active read operations issued on behalf of streaming "
                                                       "If this value gets close to {} we are likely to start dropping new read requests. "
                                                       "In that case sstable_read_queue_overloads is going to get a non-zero value.", max_memory_streaming_concurrent_reads())),
                       {streaming_label_instance}),

        sm::make_gauge("queued_reads", [this] { return _streaming_concurrency_sem.waiters(); },
                       sm::description("Holds the number of currently queued read operations on behalf of streaming."),
                       {streaming_label_instance}),

        sm::make_gauge("paused_reads", _streaming_concurrency_sem.get_inactive_read_stats().population,
                       sm::description("The number of currently ongoing streaming reads that are temporarily paused."),
                       {streaming_label_instance}),

        sm::make_derive("paused_reads_permit_based_evictions", _streaming_concurrency_sem.get_inactive_read_stats().permit_based_evictions,
                       sm::description("The number of inactive streaming reads evicted to free up permits"
                                       " Permits are required for new reads to start, and the database will evict paused reads (if any)"
                                       " to be able to admit new ones, if there is a shortage of permits."),
                       {streaming_label_instance}),

        sm::make_gauge("active_reads", [this] { return max_count_system_concurrent_reads - _system_read_concurrency_sem.available_resources().count; },
                       sm::description("Holds the number of currently active read operations from \"system\" keyspace tables. "),
                       {system_label_instance}),

        sm::make_gauge("active_reads_memory_consumption", [this] { return max_memory_system_concurrent_reads() - _system_read_concurrency_sem.available_resources().memory; },
                       sm::description(seastar::format("Holds the amount of memory consumed by currently active read operations from \"system\" keyspace tables. "
                                                       "If this value gets close to {} we are likely to start dropping new read requests. "
                                                       "In that case sstable_read_queue_overloads is going to get a non-zero value.", max_memory_system_concurrent_reads())),
                       {system_label_instance}),

        sm::make_gauge("queued_reads", [this] { return _system_read_concurrency_sem.waiters(); },
                       sm::description("Holds the number of currently queued read operations from \"system\" keyspace tables."),
                       {system_label_instance}),

        sm::make_gauge("paused_reads", _system_read_concurrency_sem.get_inactive_read_stats().population,
                       sm::description("The number of currently ongoing system reads that are temporarily paused."),
                       {system_label_instance}),

        sm::make_derive("paused_reads_permit_based_evictions", _system_read_concurrency_sem.get_inactive_read_stats().permit_based_evictions,
                       sm::description("The number of paused system reads evicted to free up permits"
                                       " Permits are required for new reads to start, and the database will evict inactive reads (if any)"
                                       " to be able to admit new ones, if there is a shortage of permits."),
                       {system_label_instance}),

        sm::make_gauge("total_result_bytes", [this] { return get_result_memory_limiter().total_used_memory(); },
                       sm::description("Holds the current amount of memory used for results.")),

        sm::make_derive("short_data_queries", _stats->short_data_queries,
                       sm::description("The rate of data queries (data or digest reads) that returned less rows than requested due to result size limiting.")),

        sm::make_derive("short_mutation_queries", _stats->short_mutation_queries,
                       sm::description("The rate of mutation queries that returned less rows than requested due to result size limiting.")),

        sm::make_derive("multishard_query_unpopped_fragments", _stats->multishard_query_unpopped_fragments,
                       sm::description("The total number of fragments that were extracted from the shard reader but were unconsumed by the query and moved back into the reader.")),

        sm::make_derive("multishard_query_unpopped_bytes", _stats->multishard_query_unpopped_bytes,
                       sm::description("The total number of bytes that were extracted from the shard reader but were unconsumed by the query and moved back into the reader.")),

        sm::make_derive("multishard_query_failed_reader_stops", _stats->multishard_query_failed_reader_stops,
                       sm::description("The number of times the stopping of a shard reader failed.")),

        sm::make_derive("multishard_query_failed_reader_saves", _stats->multishard_query_failed_reader_saves,
                       sm::description("The number of times the saving of a shard reader failed.")),

        sm::make_total_operations("counter_cell_lock_acquisition", _cl_stats->lock_acquisitions,
                                 sm::description("The number of acquired counter cell locks.")),

        sm::make_queue_length("counter_cell_lock_pending", _cl_stats->operations_waiting_for_lock,
                             sm::description("The number of counter updates waiting for a lock.")),

        sm::make_counter("large_partition_exceeding_threshold", [this] { return _large_data_handler->stats().partitions_bigger_than_threshold; },
            sm::description("Number of large partitions exceeding compaction_large_partition_warning_threshold_mb. "
                "Large partitions have performance impact and should be avoided, check the documentation for details.")),

        sm::make_total_operations("total_view_updates_pushed_local", _cf_stats.total_view_updates_pushed_local,
                sm::description("Total number of view updates generated for tables and applied locally.")),

        sm::make_total_operations("total_view_updates_pushed_remote", _cf_stats.total_view_updates_pushed_remote,
                sm::description("Total number of view updates generated for tables and sent to remote replicas.")),

        sm::make_total_operations("total_view_updates_failed_local", _cf_stats.total_view_updates_failed_local,
                sm::description("Total number of view updates generated for tables and failed to be applied locally.")),

        sm::make_total_operations("total_view_updates_failed_remote", _cf_stats.total_view_updates_failed_remote,
                sm::description("Total number of view updates generated for tables and failed to be sent to remote replicas.")),
    });
}

database::~database() {
    _read_concurrency_sem.clear_inactive_reads();
    _streaming_concurrency_sem.clear_inactive_reads();
    _system_read_concurrency_sem.clear_inactive_reads();
}

void database::update_version(const utils::UUID& version) {
    _version = version;
}

const utils::UUID& database::get_version() const {
    return _version;
}

static future<>
do_parse_schema_tables(distributed<service::storage_proxy>& proxy, const sstring& _cf_name, std::function<future<> (db::schema_tables::schema_result_value_type&)> func) {
    using namespace db::schema_tables;

    auto cf_name = make_lw_shared<sstring>(_cf_name);
    return db::system_keyspace::query(proxy, db::schema_tables::NAME, *cf_name).then([] (auto rs) {
        auto names = std::set<sstring>();
        for (auto& r : rs->rows()) {
            auto keyspace_name = r.template get_nonnull<sstring>("keyspace_name");
            names.emplace(keyspace_name);
        }
        return std::move(names);
    }).then([&proxy, cf_name, func = std::move(func)] (std::set<sstring>&& names) mutable {
        return parallel_for_each(names.begin(), names.end(), [&proxy, cf_name, func = std::move(func)] (sstring name) mutable {
            if (is_system_keyspace(name)) {
                return make_ready_future<>();
            }

            return read_schema_partition_for_keyspace(proxy, *cf_name, name).then([func, cf_name] (auto&& v) mutable {
                return do_with(std::move(v), [func = std::move(func), cf_name] (auto& v) {
                    return func(v).then_wrapped([cf_name, &v] (future<> f) {
                        try {
                            f.get();
                        } catch (std::exception& e) {
                            dblog.error("Skipping: {}. Exception occurred when loading system table {}: {}", v.first, *cf_name, e.what());
                        }
                    });
                });
            });
        });
    });
}

future<> database::parse_system_tables(distributed<service::storage_proxy>& proxy) {
    using namespace db::schema_tables;
    return do_parse_schema_tables(proxy, db::schema_tables::KEYSPACES, [this] (schema_result_value_type &v) {
        auto ksm = create_keyspace_from_schema_partition(v);
        return create_keyspace(ksm);
    }).then([&proxy, this] {
        return do_parse_schema_tables(proxy, db::schema_tables::TYPES, [this, &proxy] (schema_result_value_type &v) {
            auto&& user_types = create_types_from_schema_partition(v);
            auto& ks = this->find_keyspace(v.first);
            for (auto&& type : user_types) {
                ks.add_user_type(type);
            }
            return make_ready_future<>();
        });
    }).then([&proxy, this] {
        return do_parse_schema_tables(proxy, db::schema_tables::TABLES, [this, &proxy] (schema_result_value_type &v) {
            return create_tables_from_tables_partition(proxy, v.second).then([this] (std::map<sstring, schema_ptr> tables) {
                return parallel_for_each(tables.begin(), tables.end(), [this] (auto& t) {
                    return this->add_column_family_and_make_directory(t.second);
                });
            });
            });
    }).then([&proxy, this] {
        return do_parse_schema_tables(proxy, db::schema_tables::VIEWS, [this, &proxy] (schema_result_value_type &v) {
            return create_views_from_schema_partition(proxy, v.second).then([this] (std::vector<view_ptr> views) {
                return parallel_for_each(views.begin(), views.end(), [this] (auto&& v) {
                    return this->add_column_family_and_make_directory(v);
                });
            });
        });
    });
}

future<>
database::init_commitlog() {
    return db::commitlog::create_commitlog(db::commitlog::config::from_db_config(*_cfg, _dbcfg.available_memory)).then([this](db::commitlog&& log) {
        _commitlog = std::make_unique<db::commitlog>(std::move(log));
        _commitlog->add_flush_handler([this](db::cf_id_type id, db::replay_position pos) {
            if (_column_families.count(id) == 0) {
                // the CF has been removed.
                _commitlog->discard_completed_segments(id);
                return;
            }
            _column_families[id]->flush();
        }).release(); // we have longer life time than CL. Ignore reg anchor
    });
}

unsigned
database::shard_of(const dht::token& t) {
    return dht::shard_of(t);
}

unsigned
database::shard_of(const mutation& m) {
    return shard_of(m.token());
}

unsigned
database::shard_of(const frozen_mutation& m) {
    // FIXME: This lookup wouldn't be necessary if we
    // sent the partition key in legacy form or together
    // with token.
    schema_ptr schema = find_schema(m.column_family_id());
    return shard_of(dht::global_partitioner().get_token(*schema, m.key(*schema)));
}

void database::add_keyspace(sstring name, keyspace k) {
    if (_keyspaces.count(name) != 0) {
        throw std::invalid_argument("Keyspace " + name + " already exists");
    }
    _keyspaces.emplace(std::move(name), std::move(k));
}

future<> database::update_keyspace(const sstring& name) {
    auto& proxy = service::get_storage_proxy();
    return db::schema_tables::read_schema_partition_for_keyspace(proxy, db::schema_tables::KEYSPACES, name).then([this, name](db::schema_tables::schema_result_value_type&& v) {
        auto& ks = find_keyspace(name);

        auto tmp_ksm = db::schema_tables::create_keyspace_from_schema_partition(v);
        auto new_ksm = ::make_lw_shared<keyspace_metadata>(tmp_ksm->name(), tmp_ksm->strategy_name(), tmp_ksm->strategy_options(), tmp_ksm->durable_writes(),
                        boost::copy_range<std::vector<schema_ptr>>(ks.metadata()->cf_meta_data() | boost::adaptors::map_values), ks.metadata()->user_types());
        ks.update_from(std::move(new_ksm));
        return service::get_local_migration_manager().notify_update_keyspace(ks.metadata());
    });
}

void database::drop_keyspace(const sstring& name) {
    _keyspaces.erase(name);
}

void database::add_column_family(keyspace& ks, schema_ptr schema, column_family::config cfg) {
    schema = local_schema_registry().learn(schema);
    schema->registry_entry()->mark_synced();

    lw_shared_ptr<column_family> cf;
    if (cfg.enable_commitlog && _commitlog) {
       cf = make_lw_shared<column_family>(schema, std::move(cfg), *_commitlog, *_compaction_manager, *_cl_stats, _row_cache_tracker);
    } else {
       cf = make_lw_shared<column_family>(schema, std::move(cfg), column_family::no_commitlog(), *_compaction_manager, *_cl_stats, _row_cache_tracker);
    }

    auto uuid = schema->id();
    if (_column_families.count(uuid) != 0) {
        throw std::invalid_argument("UUID " + uuid.to_sstring() + " already mapped");
    }
    auto kscf = std::make_pair(schema->ks_name(), schema->cf_name());
    if (_ks_cf_to_uuid.count(kscf) != 0) {
        throw std::invalid_argument("Column family " + schema->cf_name() + " exists");
    }
    ks.add_or_update_column_family(schema);
    cf->start();
    _column_families.emplace(uuid, std::move(cf));
    _ks_cf_to_uuid.emplace(std::move(kscf), uuid);
    if (schema->is_view()) {
        find_column_family(schema->view_info()->base_id()).add_or_update_view(view_ptr(schema));
    }
}

future<> database::add_column_family_and_make_directory(schema_ptr schema) {
    auto& ks = find_keyspace(schema->ks_name());
    add_column_family(ks, schema, ks.make_column_family_config(*schema, *this));
    find_column_family(schema).get_index_manager().reload();
    return ks.make_directory_for_column_family(schema->cf_name(), schema->id());
}

bool database::update_column_family(schema_ptr new_schema) {
    column_family& cfm = find_column_family(new_schema->id());
    bool columns_changed = !cfm.schema()->equal_columns(*new_schema);
    auto s = local_schema_registry().learn(new_schema);
    s->registry_entry()->mark_synced();
    cfm.set_schema(s);
    find_keyspace(s->ks_name()).metadata()->add_or_update_column_family(s);
    if (s->is_view()) {
        try {
            find_column_family(s->view_info()->base_id()).add_or_update_view(view_ptr(s));
        } catch (no_such_column_family&) {
            // Update view mutations received after base table drop.
        }
    }
    cfm.get_index_manager().reload();
    return columns_changed;
}

void database::remove(const column_family& cf) {
    auto s = cf.schema();
    auto& ks = find_keyspace(s->ks_name());
    _querier_cache.evict_all_for_table(s->id());
    _column_families.erase(s->id());
    ks.metadata()->remove_column_family(s);
    _ks_cf_to_uuid.erase(std::make_pair(s->ks_name(), s->cf_name()));
    if (s->is_view()) {
        try {
            find_column_family(s->view_info()->base_id()).remove_view(view_ptr(s));
        } catch (no_such_column_family&) {
            // Drop view mutations received after base table drop.
        }
    }
}

future<> database::drop_column_family(const sstring& ks_name, const sstring& cf_name, timestamp_func tsf, bool snapshot) {
    auto uuid = find_uuid(ks_name, cf_name);
    auto cf = _column_families.at(uuid);
    remove(*cf);
    cf->clear_views();
    auto& ks = find_keyspace(ks_name);
    return when_all_succeed(cf->await_pending_writes(), cf->await_pending_reads()).then([this, &ks, cf, tsf = std::move(tsf), snapshot] {
        return truncate(ks, *cf, std::move(tsf), snapshot).finally([this, cf] {
            return cf->stop();
        });
    }).finally([cf] {});
}

const utils::UUID& database::find_uuid(const sstring& ks, const sstring& cf) const {
    try {
        return _ks_cf_to_uuid.at(std::make_pair(ks, cf));
    } catch (...) {
        throw std::out_of_range("");
    }
}

const utils::UUID& database::find_uuid(const schema_ptr& schema) const {
    return find_uuid(schema->ks_name(), schema->cf_name());
}

keyspace& database::find_keyspace(const sstring& name) {
    try {
        return _keyspaces.at(name);
    } catch (...) {
        std::throw_with_nested(no_such_keyspace(name));
    }
}

const keyspace& database::find_keyspace(const sstring& name) const {
    try {
        return _keyspaces.at(name);
    } catch (...) {
        std::throw_with_nested(no_such_keyspace(name));
    }
}

bool database::has_keyspace(const sstring& name) const {
    return _keyspaces.count(name) != 0;
}

std::vector<sstring>  database::get_non_system_keyspaces() const {
    std::vector<sstring> res;
    for (auto const &i : _keyspaces) {
        if (!is_system_keyspace(i.first)) {
            res.push_back(i.first);
        }
    }
    return res;
}

std::vector<lw_shared_ptr<column_family>> database::get_non_system_column_families() const {
    return boost::copy_range<std::vector<lw_shared_ptr<column_family>>>(
        get_column_families()
            | boost::adaptors::map_values
            | boost::adaptors::filtered([](const lw_shared_ptr<column_family>& cf) {
                return !is_system_keyspace(cf->schema()->ks_name());
            }));
}

column_family& database::find_column_family(const sstring& ks_name, const sstring& cf_name) {
    try {
        return find_column_family(find_uuid(ks_name, cf_name));
    } catch (...) {
        std::throw_with_nested(no_such_column_family(ks_name, cf_name));
    }
}

const column_family& database::find_column_family(const sstring& ks_name, const sstring& cf_name) const {
    try {
        return find_column_family(find_uuid(ks_name, cf_name));
    } catch (...) {
        std::throw_with_nested(no_such_column_family(ks_name, cf_name));
    }
}

column_family& database::find_column_family(const utils::UUID& uuid) {
    try {
        return *_column_families.at(uuid);
    } catch (...) {
        std::throw_with_nested(no_such_column_family(uuid));
    }
}

const column_family& database::find_column_family(const utils::UUID& uuid) const {
    try {
        return *_column_families.at(uuid);
    } catch (...) {
        std::throw_with_nested(no_such_column_family(uuid));
    }
}

bool database::column_family_exists(const utils::UUID& uuid) const {
    return _column_families.count(uuid);
}

void
keyspace::create_replication_strategy(const std::map<sstring, sstring>& options) {
    using namespace locator;

    auto& ss = service::get_local_storage_service();
    _replication_strategy =
            abstract_replication_strategy::create_replication_strategy(
                _metadata->name(), _metadata->strategy_name(),
                ss.get_token_metadata(), options);
}

locator::abstract_replication_strategy&
keyspace::get_replication_strategy() {
    return *_replication_strategy;
}


const locator::abstract_replication_strategy&
keyspace::get_replication_strategy() const {
    return *_replication_strategy;
}

void
keyspace::set_replication_strategy(std::unique_ptr<locator::abstract_replication_strategy> replication_strategy) {
    _replication_strategy = std::move(replication_strategy);
}

void keyspace::update_from(::lw_shared_ptr<keyspace_metadata> ksm) {
    _metadata = std::move(ksm);
   create_replication_strategy(_metadata->strategy_options());
}

static bool is_system_table(const schema& s) {
    return s.ks_name() == db::system_keyspace::NAME || s.ks_name() == db::system_distributed_keyspace::NAME;
}

column_family::config
keyspace::make_column_family_config(const schema& s, const database& db) const {
    column_family::config cfg;
    const db::config& db_config = db.get_config();

    for (auto& extra : _config.all_datadirs) {
        cfg.all_datadirs.push_back(column_family_directory(extra, s.cf_name(), s.id()));
    }
    cfg.datadir = cfg.all_datadirs[0];
    cfg.enable_disk_reads = _config.enable_disk_reads;
    cfg.enable_disk_writes = _config.enable_disk_writes;
    cfg.enable_commitlog = _config.enable_commitlog;
    cfg.enable_cache = _config.enable_cache;
    cfg.enable_dangerous_direct_import_of_cassandra_counters = _config.enable_dangerous_direct_import_of_cassandra_counters;
    cfg.compaction_enforce_min_threshold = _config.compaction_enforce_min_threshold;
    cfg.dirty_memory_manager = _config.dirty_memory_manager;
    cfg.streaming_dirty_memory_manager = _config.streaming_dirty_memory_manager;
    cfg.read_concurrency_semaphore = _config.read_concurrency_semaphore;
    cfg.streaming_read_concurrency_semaphore = _config.streaming_read_concurrency_semaphore;
    cfg.cf_stats = _config.cf_stats;
    cfg.enable_incremental_backups = _config.enable_incremental_backups;
    cfg.compaction_scheduling_group = _config.compaction_scheduling_group;
    cfg.memory_compaction_scheduling_group = _config.memory_compaction_scheduling_group;
    cfg.memtable_scheduling_group = _config.memtable_scheduling_group;
    cfg.memtable_to_cache_scheduling_group = _config.memtable_to_cache_scheduling_group;
    cfg.streaming_scheduling_group = _config.streaming_scheduling_group;
    cfg.statement_scheduling_group = _config.statement_scheduling_group;
    cfg.enable_metrics_reporting = db_config.enable_keyspace_column_family_metrics();

    // avoid self-reporting
    if (is_system_table(s)) {
        cfg.large_data_handler = db.get_nop_large_data_handler();
    } else {
        cfg.large_data_handler = db.get_large_data_handler();
    }

    cfg.sstables_manager = &db.get_sstables_manager();
    cfg.view_update_concurrency_semaphore = _config.view_update_concurrency_semaphore;
    cfg.view_update_concurrency_semaphore_limit = _config.view_update_concurrency_semaphore_limit;
    cfg.data_listeners = &db.data_listeners();

    return cfg;
}

sstring
keyspace::column_family_directory(const sstring& name, utils::UUID uuid) const {
    return column_family_directory(_config.datadir, name, uuid);
}

sstring
keyspace::column_family_directory(const sstring& base_path, const sstring& name, utils::UUID uuid) const {
    auto uuid_sstring = uuid.to_sstring();
    boost::erase_all(uuid_sstring, "-");
    return format("{}/{}-{}", base_path, name, uuid_sstring);
}

future<>
keyspace::make_directory_for_column_family(const sstring& name, utils::UUID uuid) {
    std::vector<sstring> cfdirs;
    for (auto& extra : _config.all_datadirs) {
        cfdirs.push_back(column_family_directory(extra, name, uuid));
    }
    return seastar::async([cfdirs = std::move(cfdirs)] {
        for (auto& cfdir : cfdirs) {
            io_check(recursive_touch_directory, cfdir).get();
        }
        io_check(touch_directory, cfdirs[0] + "/upload").get();
        io_check(touch_directory, cfdirs[0] + "/staging").get();
    });
}

no_such_keyspace::no_such_keyspace(const sstring& ks_name)
    : runtime_error{format("Can't find a keyspace {}", ks_name)}
{
}

no_such_column_family::no_such_column_family(const utils::UUID& uuid)
    : runtime_error{format("Can't find a column family with UUID {}", uuid)}
{
}

no_such_column_family::no_such_column_family(const sstring& ks_name, const sstring& cf_name)
    : runtime_error{format("Can't find a column family {} in keyspace {}", cf_name, ks_name)}
{
}

column_family& database::find_column_family(const schema_ptr& schema) {
    return find_column_family(schema->id());
}

const column_family& database::find_column_family(const schema_ptr& schema) const {
    return find_column_family(schema->id());
}

using strategy_class_registry = class_registry<
    locator::abstract_replication_strategy,
    const sstring&,
    locator::token_metadata&,
    locator::snitch_ptr&,
    const std::map<sstring, sstring>&>;

keyspace_metadata::keyspace_metadata(sstring name,
             sstring strategy_name,
             std::map<sstring, sstring> strategy_options,
             bool durable_writes,
             std::vector<schema_ptr> cf_defs)
    : keyspace_metadata(std::move(name),
                        std::move(strategy_name),
                        std::move(strategy_options),
                        durable_writes,
                        std::move(cf_defs),
                        make_lw_shared<user_types_metadata>()) { }

keyspace_metadata::keyspace_metadata(sstring name,
             sstring strategy_name,
             std::map<sstring, sstring> strategy_options,
             bool durable_writes,
             std::vector<schema_ptr> cf_defs,
             lw_shared_ptr<user_types_metadata> user_types)
    : _name{std::move(name)}
    , _strategy_name{strategy_class_registry::to_qualified_class_name(strategy_name.empty() ? "NetworkTopologyStrategy" : strategy_name)}
    , _strategy_options{std::move(strategy_options)}
    , _durable_writes{durable_writes}
    , _user_types{std::move(user_types)}
{
    for (auto&& s : cf_defs) {
        _cf_meta_data.emplace(s->cf_name(), s);
    }
}

void keyspace_metadata::validate() const {
    using namespace locator;

    auto& ss = service::get_local_storage_service();
    abstract_replication_strategy::validate_replication_strategy(name(), strategy_name(), ss.get_token_metadata(), strategy_options());
}

std::vector<schema_ptr> keyspace_metadata::tables() const {
    return boost::copy_range<std::vector<schema_ptr>>(_cf_meta_data
            | boost::adaptors::map_values
            | boost::adaptors::filtered([] (auto&& s) { return !s->is_view(); }));
}

std::vector<view_ptr> keyspace_metadata::views() const {
    return boost::copy_range<std::vector<view_ptr>>(_cf_meta_data
            | boost::adaptors::map_values
            | boost::adaptors::filtered(std::mem_fn(&schema::is_view))
            | boost::adaptors::transformed([] (auto&& s) { return view_ptr(s); }));
}

schema_ptr database::find_schema(const sstring& ks_name, const sstring& cf_name) const {
    try {
        return find_schema(find_uuid(ks_name, cf_name));
    } catch (std::out_of_range&) {
        std::throw_with_nested(no_such_column_family(ks_name, cf_name));
    }
}

schema_ptr database::find_schema(const utils::UUID& uuid) const {
    return find_column_family(uuid).schema();
}

bool database::has_schema(const sstring& ks_name, const sstring& cf_name) const {
    return _ks_cf_to_uuid.count(std::make_pair(ks_name, cf_name)) > 0;
}

std::vector<view_ptr> database::get_views() const {
    return boost::copy_range<std::vector<view_ptr>>(get_non_system_column_families()
            | boost::adaptors::filtered([] (auto& cf) { return cf->schema()->is_view(); })
            | boost::adaptors::transformed([] (auto& cf) { return view_ptr(cf->schema()); }));
}

void database::create_in_memory_keyspace(const lw_shared_ptr<keyspace_metadata>& ksm) {
    keyspace ks(ksm, std::move(make_keyspace_config(*ksm)));
    ks.create_replication_strategy(ksm->strategy_options());
    _keyspaces.emplace(ksm->name(), std::move(ks));
}

future<>
database::create_keyspace(const lw_shared_ptr<keyspace_metadata>& ksm) {
    auto i = _keyspaces.find(ksm->name());
    if (i != _keyspaces.end()) {
        return make_ready_future<>();
    }

    create_in_memory_keyspace(ksm);
    auto& datadir = _keyspaces.at(ksm->name()).datadir();
    if (datadir != "") {
        return io_check(touch_directory, datadir);
    } else {
        return make_ready_future<>();
    }
}

std::set<sstring>
database::existing_index_names(const sstring& ks_name, const sstring& cf_to_exclude) const {
    std::set<sstring> names;
    for (auto& schema : find_keyspace(ks_name).metadata()->tables()) {
        if (!cf_to_exclude.empty() && schema->cf_name() == cf_to_exclude) {
            continue;
        }
        for (const auto& index_name : schema->index_names()) {
            names.emplace(index_name);
        }
    }
    return names;
}

// Based on:
//  - org.apache.cassandra.db.AbstractCell#reconcile()
//  - org.apache.cassandra.db.BufferExpiringCell#reconcile()
//  - org.apache.cassandra.db.BufferDeletedCell#reconcile()
int
compare_atomic_cell_for_merge(atomic_cell_view left, atomic_cell_view right) {
    if (left.timestamp() != right.timestamp()) {
        return left.timestamp() > right.timestamp() ? 1 : -1;
    }
    if (left.is_live() != right.is_live()) {
        return left.is_live() ? -1 : 1;
    }
    if (left.is_live()) {
        auto c = compare_unsigned(left.value(), right.value());
        if (c != 0) {
            return c;
        }
        if (left.is_live_and_has_ttl() != right.is_live_and_has_ttl()) {
            // prefer expiring cells.
            return left.is_live_and_has_ttl() ? 1 : -1;
        }
        if (left.is_live_and_has_ttl() && left.expiry() != right.expiry()) {
            return left.expiry() < right.expiry() ? -1 : 1;
        }
    } else {
        // Both are deleted
        if (left.deletion_time() != right.deletion_time()) {
            // Origin compares big-endian serialized deletion time. That's because it
            // delegates to AbstractCell.reconcile() which compares values after
            // comparing timestamps, which in case of deleted cells will hold
            // serialized expiry.
            return (uint64_t) left.deletion_time().time_since_epoch().count()
                   < (uint64_t) right.deletion_time().time_since_epoch().count() ? -1 : 1;
        }
    }
    return 0;
}

future<lw_shared_ptr<query::result>, cache_temperature>
database::query(schema_ptr s, const query::read_command& cmd, query::result_options opts, const dht::partition_range_vector& ranges,
                tracing::trace_state_ptr trace_state, uint64_t max_result_size, db::timeout_clock::time_point timeout) {
    column_family& cf = find_column_family(cmd.cf_id);
    query::querier_cache_context cache_ctx(_querier_cache, cmd.query_uuid, cmd.is_first_page);
    return _data_query_stage(&cf,
            std::move(s),
            seastar::cref(cmd),
            opts,
            seastar::cref(ranges),
            std::move(trace_state),
            seastar::ref(get_result_memory_limiter()),
            max_result_size,
            timeout,
            std::move(cache_ctx)).then_wrapped([this, s = _stats, hit_rate = cf.get_global_cache_hit_rate(), op = cf.read_in_progress()] (auto f) {
        if (f.failed()) {
            ++s->total_reads_failed;
            return make_exception_future<lw_shared_ptr<query::result>, cache_temperature>(f.get_exception());
        } else {
            ++s->total_reads;
            auto result = f.get0();
            s->short_data_queries += bool(result->is_short_read());
            return make_ready_future<lw_shared_ptr<query::result>, cache_temperature>(std::move(result), hit_rate);
        }
    });
}

future<reconcilable_result, cache_temperature>
database::query_mutations(schema_ptr s, const query::read_command& cmd, const dht::partition_range& range,
                          query::result_memory_accounter&& accounter, tracing::trace_state_ptr trace_state, db::timeout_clock::time_point timeout) {
    column_family& cf = find_column_family(cmd.cf_id);
    query::querier_cache_context cache_ctx(_querier_cache, cmd.query_uuid, cmd.is_first_page);
    return _mutation_query_stage(std::move(s),
            cf.as_mutation_source(),
            seastar::cref(range),
            seastar::cref(cmd.slice),
            cmd.row_limit,
            cmd.partition_limit,
            cmd.timestamp,
            std::move(accounter),
            std::move(trace_state),
            timeout,
            std::move(cache_ctx)).then_wrapped([this, s = _stats, hit_rate = cf.get_global_cache_hit_rate(), op = cf.read_in_progress()] (auto f) {
        if (f.failed()) {
            ++s->total_reads_failed;
            return make_exception_future<reconcilable_result, cache_temperature>(f.get_exception());
        } else {
            ++s->total_reads;
            auto result = f.get0();
            s->short_mutation_queries += bool(result.is_short_read());
            return make_ready_future<reconcilable_result, cache_temperature>(std::move(result), hit_rate);
        }
    });
}

std::unordered_set<sstring> database::get_initial_tokens() {
    std::unordered_set<sstring> tokens;
    sstring tokens_string = get_config().initial_token();
    try {
        boost::split(tokens, tokens_string, boost::is_any_of(sstring(", ")));
    } catch (...) {
        throw std::runtime_error(format("Unable to parse initial_token={}", tokens_string));
    }
    tokens.erase("");
    return tokens;
}

std::optional<gms::inet_address> database::get_replace_address() {
    auto& cfg = get_config();
    sstring replace_address = cfg.replace_address();
    sstring replace_address_first_boot = cfg.replace_address_first_boot();
    try {
        if (!replace_address.empty()) {
            return gms::inet_address(replace_address);
        } else if (!replace_address_first_boot.empty()) {
            return gms::inet_address(replace_address_first_boot);
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool database::is_replacing() {
    sstring replace_address_first_boot = get_config().replace_address_first_boot();
    if (!replace_address_first_boot.empty() && db::system_keyspace::bootstrap_complete()) {
        dblog.info("Replace address on first boot requested; this node is already bootstrapped");
        return false;
    }
    return bool(get_replace_address());
}

void database::register_connection_drop_notifier(netw::messaging_service& ms) {
    ms.register_connection_drop_notifier([this] (gms::inet_address ep) {
        dblog.debug("Drop hit rate info for {} because of disconnect", ep);
        for (auto&& cf : get_non_system_column_families()) {
            cf->drop_hit_rate(ep);
        }
    });
}

std::ostream& operator<<(std::ostream& out, const column_family& cf) {
    return fmt_print(out, "{{column_family: {}/{}}}", cf._schema->ks_name(), cf._schema->cf_name());
}

std::ostream& operator<<(std::ostream& out, const database& db) {
    out << "{\n";
    for (auto&& e : db._column_families) {
        auto&& cf = *e.second;
        out << "(" << e.first.to_sstring() << ", " << cf.schema()->cf_name() << ", " << cf.schema()->ks_name() << "): " << cf << "\n";
    }
    out << "}";
    return out;
}

future<mutation> database::do_apply_counter_update(column_family& cf, const frozen_mutation& fm, schema_ptr m_schema,
                                                   db::timeout_clock::time_point timeout,tracing::trace_state_ptr trace_state) {
    auto m = fm.unfreeze(m_schema);
    m.upgrade(cf.schema());

    // prepare partition slice
    query::column_id_vector static_columns;
    static_columns.reserve(m.partition().static_row().size());
    m.partition().static_row().for_each_cell([&] (auto id, auto&&) {
        static_columns.emplace_back(id);
    });

    query::clustering_row_ranges cr_ranges;
    cr_ranges.reserve(8);
    query::column_id_vector regular_columns;
    regular_columns.reserve(32);

    for (auto&& cr : m.partition().clustered_rows()) {
        cr_ranges.emplace_back(query::clustering_range::make_singular(cr.key()));
        cr.row().cells().for_each_cell([&] (auto id, auto&&) {
            regular_columns.emplace_back(id);
        });
    }

    boost::sort(regular_columns);
    regular_columns.erase(std::unique(regular_columns.begin(), regular_columns.end()),
                          regular_columns.end());

    auto slice = query::partition_slice(std::move(cr_ranges), std::move(static_columns),
        std::move(regular_columns), { }, { }, cql_serialization_format::internal(), query::max_rows);

    return do_with(std::move(slice), std::move(m), std::vector<locked_cell>(),
                   [this, &cf, timeout, trace_state = std::move(trace_state), op = cf.write_in_progress()] (const query::partition_slice& slice, mutation& m, std::vector<locked_cell>& locks) mutable {
        tracing::trace(trace_state, "Acquiring counter locks");
        return cf.lock_counter_cells(m, timeout).then([&, m_schema = cf.schema(), trace_state = std::move(trace_state), timeout, this] (std::vector<locked_cell> lcs) mutable {
            locks = std::move(lcs);

            // Before counter update is applied it needs to be transformed from
            // deltas to counter shards. To do that, we need to read the current
            // counter state for each modified cell...

            tracing::trace(trace_state, "Reading counter values from the CF");
            return counter_write_query(m_schema, cf.as_mutation_source(), m.decorated_key(), slice, trace_state)
                    .then([this, &cf, &m, m_schema, timeout, trace_state] (auto mopt) {
                // ...now, that we got existing state of all affected counter
                // cells we can look for our shard in each of them, increment
                // its clock and apply the delta.
                transform_counter_updates_to_shards(m, mopt ? &*mopt : nullptr, cf.failed_counter_applies_to_memtable());
                tracing::trace(trace_state, "Applying counter update");
                return this->apply_with_commitlog(cf, m, timeout);
            }).then([&m] {
                return std::move(m);
            });
        });
    });
}

future<> dirty_memory_manager::shutdown() {
    _db_shutdown_requested = true;
    _should_flush.signal();
    return std::move(_waiting_flush).then([this] {
        return _virtual_region_group.shutdown().then([this] {
            return _real_region_group.shutdown();
        });
    });
}

future<> memtable_list::request_flush() {
    if (empty() || !may_flush()) {
        return make_ready_future<>();
    } else if (!_flush_coalescing) {
        _flush_coalescing = shared_promise<>();
        _dirty_memory_manager->start_extraneous_flush();
        auto ef = defer([this] { _dirty_memory_manager->finish_extraneous_flush(); });
        return _dirty_memory_manager->get_flush_permit().then([this, ef = std::move(ef)] (auto permit) mutable {
            auto current_flush = std::move(*_flush_coalescing);
            _flush_coalescing = {};
            return _dirty_memory_manager->flush_one(*this, std::move(permit)).then_wrapped([this, ef = std::move(ef),
                                                                                            current_flush = std::move(current_flush)] (auto f) mutable {
                if (f.failed()) {
                    current_flush.set_exception(f.get_exception());
                } else {
                    current_flush.set_value();
                }
            });
        });
    } else {
        return _flush_coalescing->get_shared_future();
    }
}

lw_shared_ptr<memtable> memtable_list::new_memtable() {
    return make_lw_shared<memtable>(_current_schema(), *_dirty_memory_manager, this, _compaction_scheduling_group);
}

future<flush_permit> flush_permit::reacquire_sstable_write_permit() && {
    return _manager->get_flush_permit(std::move(_background_permit));
}

future<> dirty_memory_manager::flush_one(memtable_list& mtlist, flush_permit&& permit) {
    return mtlist.seal_active_memtable_immediate(std::move(permit)).handle_exception([this, schema = mtlist.back()->schema()] (std::exception_ptr ep) {
        dblog.error("Failed to flush memtable, {}:{} - {}", schema->ks_name(), schema->cf_name(), ep);
        return make_exception_future<>(ep);
    });
}

future<> dirty_memory_manager::flush_when_needed() {
    if (!_db) {
        return make_ready_future<>();
    }
    // If there are explicit flushes requested, we must wait for them to finish before we stop.
    return do_until([this] { return _db_shutdown_requested; }, [this] {
        auto has_work = [this] { return has_pressure() || _db_shutdown_requested; };
        return _should_flush.wait(std::move(has_work)).then([this] {
            return get_flush_permit().then([this] (auto permit) {
                // We give priority to explicit flushes. They are mainly user-initiated flushes,
                // flushes coming from a DROP statement, or commitlog flushes.
                if (_flush_serializer.waiters()) {
                    return make_ready_future<>();
                }
                // condition abated while we waited for the semaphore
                if (!this->has_pressure() || _db_shutdown_requested) {
                    return make_ready_future<>();
                }
                // There are many criteria that can be used to select what is the best memtable to
                // flush. Most of the time we want some coordination with the commitlog to allow us to
                // release commitlog segments as early as we can.
                //
                // But during pressure condition, we'll just pick the CF that holds the largest
                // memtable. The advantage of doing this is that this is objectively the one that will
                // release the biggest amount of memory and is less likely to be generating tiny
                // SSTables.
                memtable& candidate_memtable = memtable::from_region(*(this->_virtual_region_group.get_largest_region()));

                if (candidate_memtable.empty()) {
                    // Soft pressure, but nothing to flush. It could be due to fsync or memtable_to_cache lagging.
                    // Back off to avoid OOMing with flush continuations.
                    return sleep(1ms);
                }

                // Do not wait. The semaphore will protect us against a concurrent flush. But we
                // want to start a new one as soon as the permits are destroyed and the semaphore is
                // made ready again, not when we are done with the current one.
                this->flush_one(*(candidate_memtable.get_memtable_list()), std::move(permit));
                return make_ready_future<>();
            });
        });
    }).finally([this] {
        // We'll try to acquire the permit here to make sure we only really stop when there are no
        // in-flight flushes. Our stop condition checks for the presence of waiters, but it could be
        // that we have no waiters, but a flush still in flight. We wait for all background work to
        // stop. When that stops, we know that the foreground work in the _flush_serializer has
        // stopped as well.
        return get_units(_background_work_flush_serializer, _max_background_work).discard_result();
    });
}

void dirty_memory_manager::start_reclaiming() noexcept {
    _should_flush.signal();
}

future<> database::apply_in_memory(const frozen_mutation& m, schema_ptr m_schema, db::rp_handle&& h, db::timeout_clock::time_point timeout) {
    auto& cf = find_column_family(m.column_family_id());

    data_listeners().on_write(m_schema, m);

    return cf.dirty_memory_region_group().run_when_memory_available([this, &m, m_schema = std::move(m_schema), h = std::move(h)]() mutable {
        try {
            auto& cf = find_column_family(m.column_family_id());
            cf.apply(m, m_schema, std::move(h));
        } catch (no_such_column_family&) {
            dblog.error("Attempting to mutate non-existent table {}", m.column_family_id());
        }
    }, timeout);
}

future<> database::apply_in_memory(const mutation& m, column_family& cf, db::rp_handle&& h, db::timeout_clock::time_point timeout) {
    return cf.dirty_memory_region_group().run_when_memory_available([this, &m, &cf, h = std::move(h)]() mutable {
        cf.apply(m, std::move(h));
    }, timeout);
}

future<mutation> database::apply_counter_update(schema_ptr s, const frozen_mutation& m, db::timeout_clock::time_point timeout, tracing::trace_state_ptr trace_state) {
  return update_write_metrics(seastar::futurize_apply([&] {
    if (!s->is_synced()) {
        throw std::runtime_error(format("attempted to mutate using not synced schema of {}.{}, version={}",
                                        s->ks_name(), s->cf_name(), s->version()));
    }
    try {
        auto& cf = find_column_family(m.column_family_id());
        return do_apply_counter_update(cf, m, s, timeout, std::move(trace_state));
    } catch (no_such_column_family&) {
        dblog.error("Attempting to mutate non-existent table {}", m.column_family_id());
        throw;
    }
  }));
}

static future<> maybe_handle_reorder(std::exception_ptr exp) {
    try {
        std::rethrow_exception(exp);
        return make_exception_future(exp);
    } catch (mutation_reordered_with_truncate_exception&) {
        // This mutation raced with a truncate, so we can just drop it.
        dblog.debug("replay_position reordering detected");
        return make_ready_future<>();
    }
}

future<> database::apply_with_commitlog(column_family& cf, const mutation& m, db::timeout_clock::time_point timeout) {
    if (cf.commitlog() != nullptr) {
        return do_with(freeze(m), [this, &m, &cf, timeout] (frozen_mutation& fm) {
            commitlog_entry_writer cew(m.schema(), fm);
            return cf.commitlog()->add_entry(m.schema()->id(), cew, timeout);
        }).then([this, &m, &cf, timeout] (db::rp_handle h) {
            return apply_in_memory(m, cf, std::move(h), timeout).handle_exception(maybe_handle_reorder);
        });
    }
    return apply_in_memory(m, cf, {}, timeout);
}

future<> database::apply_with_commitlog(schema_ptr s, column_family& cf, utils::UUID uuid, const frozen_mutation& m, db::timeout_clock::time_point timeout) {
    auto cl = cf.commitlog();
    if (cl != nullptr) {
        commitlog_entry_writer cew(s, m);
        return cf.commitlog()->add_entry(uuid, cew, timeout).then([&m, this, s, timeout, cl](db::rp_handle h) {
            return this->apply_in_memory(m, s, std::move(h), timeout).handle_exception(maybe_handle_reorder);
        });
    }
    return apply_in_memory(m, std::move(s), {}, timeout);
}

future<> database::do_apply(schema_ptr s, const frozen_mutation& m, db::timeout_clock::time_point timeout) {
    // I'm doing a nullcheck here since the init code path for db etc
    // is a little in flux and commitlog is created only when db is
    // initied from datadir.
    auto uuid = m.column_family_id();
    auto& cf = find_column_family(uuid);
    if (!s->is_synced()) {
        throw std::runtime_error(format("attempted to mutate using not synced schema of {}.{}, version={}",
                                 s->ks_name(), s->cf_name(), s->version()));
    }

    // Signal to view building code that a write is in progress,
    // so it knows when new writes start being sent to a new view.
    auto op = cf.write_in_progress();
    if (cf.views().empty()) {
        return apply_with_commitlog(std::move(s), cf, std::move(uuid), m, timeout).finally([op = std::move(op)] { });
    }
    future<row_locker::lock_holder> f = cf.push_view_replica_updates(s, m, timeout);
    return f.then([this, s = std::move(s), uuid = std::move(uuid), &m, timeout, &cf, op = std::move(op)] (row_locker::lock_holder lock) mutable {
        return apply_with_commitlog(std::move(s), cf, std::move(uuid), m, timeout).finally(
                // Hold the local lock on the base-table partition or row
                // taken before the read, until the update is done.
                [lock = std::move(lock), op = std::move(op)] { });
    });
}

template<typename Future>
Future database::update_write_metrics(Future&& f) {
    return f.then_wrapped([this, s = _stats] (auto f) {
        if (f.failed()) {
            ++s->total_writes_failed;
            try {
                f.get();
            } catch (const timed_out_error&) {
                ++s->total_writes_timedout;
                throw;
            }
            assert(0 && "should not reach");
        }
        ++s->total_writes;
        return f;
    });
}

future<> database::apply(schema_ptr s, const frozen_mutation& m, db::timeout_clock::time_point timeout) {
    if (dblog.is_enabled(logging::log_level::trace)) {
        dblog.trace("apply {}", m.pretty_printer(s));
    }
    return update_write_metrics(_apply_stage(this, std::move(s), seastar::cref(m), timeout));
}

future<> database::apply_streaming_mutation(schema_ptr s, utils::UUID plan_id, const frozen_mutation& m, bool fragmented) {
    if (!s->is_synced()) {
        throw std::runtime_error(format("attempted to mutate using not synced schema of {}.{}, version={}",
                                 s->ks_name(), s->cf_name(), s->version()));
    }
    return with_scheduling_group(_dbcfg.streaming_scheduling_group, [this, s = std::move(s), &m, fragmented, plan_id] () mutable {
        return _streaming_dirty_memory_manager.region_group().run_when_memory_available([this, &m, plan_id, fragmented, s = std::move(s)] {
            auto uuid = m.column_family_id();
            auto& cf = find_column_family(uuid);
            cf.apply_streaming_mutation(s, plan_id, std::move(m), fragmented);
        });
    });
}

keyspace::config
database::make_keyspace_config(const keyspace_metadata& ksm) {
    keyspace::config cfg;
    if (_cfg->data_file_directories().size() > 0) {
        cfg.datadir = format("{}/{}", _cfg->data_file_directories()[0], ksm.name());
        for (auto& extra : _cfg->data_file_directories()) {
            cfg.all_datadirs.push_back(format("{}/{}", extra, ksm.name()));
        }
        cfg.enable_disk_writes = !_cfg->enable_in_memory_data_store();
        cfg.enable_disk_reads = true; // we allways read from disk
        cfg.enable_commitlog = ksm.durable_writes() && _cfg->enable_commitlog() && !_cfg->enable_in_memory_data_store();
        cfg.enable_cache = _cfg->enable_cache();

    } else {
        cfg.datadir = "";
        cfg.enable_disk_writes = false;
        cfg.enable_disk_reads = false;
        cfg.enable_commitlog = false;
        cfg.enable_cache = false;
    }
    cfg.enable_dangerous_direct_import_of_cassandra_counters = _cfg->enable_dangerous_direct_import_of_cassandra_counters();
    cfg.compaction_enforce_min_threshold = _cfg->compaction_enforce_min_threshold();
    cfg.dirty_memory_manager = &_dirty_memory_manager;
    cfg.streaming_dirty_memory_manager = &_streaming_dirty_memory_manager;
    cfg.read_concurrency_semaphore = &_read_concurrency_sem;
    cfg.streaming_read_concurrency_semaphore = &_streaming_concurrency_sem;
    cfg.cf_stats = &_cf_stats;
    cfg.enable_incremental_backups = _enable_incremental_backups;

    cfg.compaction_scheduling_group = _dbcfg.compaction_scheduling_group;
    cfg.memory_compaction_scheduling_group = _dbcfg.memory_compaction_scheduling_group;
    cfg.memtable_scheduling_group = _dbcfg.memtable_scheduling_group;
    cfg.memtable_to_cache_scheduling_group = _dbcfg.memtable_to_cache_scheduling_group;
    cfg.streaming_scheduling_group = _dbcfg.streaming_scheduling_group;
    cfg.statement_scheduling_group = _dbcfg.statement_scheduling_group;
    cfg.enable_metrics_reporting = _cfg->enable_keyspace_column_family_metrics();

    cfg.view_update_concurrency_semaphore = &_view_update_concurrency_sem;
    cfg.view_update_concurrency_semaphore_limit = max_memory_pending_view_updates();
    return cfg;
}

namespace db {

std::ostream& operator<<(std::ostream& os, const write_type& t) {
    switch(t) {
        case write_type::SIMPLE: return os << "SIMPLE";
        case write_type::BATCH: return os << "BATCH";
        case write_type::UNLOGGED_BATCH: return os << "UNLOGGED_BATCH";
        case write_type::COUNTER: return os << "COUNTER";
        case write_type::BATCH_LOG: return os << "BATCH_LOG";
        case write_type::CAS: return os << "CAS";
        case write_type::VIEW: return os << "VIEW";
    }
    abort();
}

std::ostream& operator<<(std::ostream& os, db::consistency_level cl) {
    switch (cl) {
    case db::consistency_level::ANY: return os << "ANY";
    case db::consistency_level::ONE: return os << "ONE";
    case db::consistency_level::TWO: return os << "TWO";
    case db::consistency_level::THREE: return os << "THREE";
    case db::consistency_level::QUORUM: return os << "QUORUM";
    case db::consistency_level::ALL: return os << "ALL";
    case db::consistency_level::LOCAL_QUORUM: return os << "LOCAL_QUORUM";
    case db::consistency_level::EACH_QUORUM: return os << "EACH_QUORUM";
    case db::consistency_level::SERIAL: return os << "SERIAL";
    case db::consistency_level::LOCAL_SERIAL: return os << "LOCAL_SERIAL";
    case db::consistency_level::LOCAL_ONE: return os << "LOCAL_ONE";
    default: abort();
    }
}

}

std::ostream&
operator<<(std::ostream& os, const exploded_clustering_prefix& ecp) {
    // Can't pass to_hex() to transformed(), since it is overloaded, so wrap:
    auto enhex = [] (auto&& x) { return to_hex(x); };
    return fmt_print(os, "prefix{{{}}}", ::join(":", ecp._v | boost::adaptors::transformed(enhex)));
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell_view& acv) {
    if (acv.is_live()) {
        return fmt_print(os, "atomic_cell{{{};ts={:d};expiry={:d},ttl={:d}}}",
            to_hex(acv.value().linearize()),
            acv.timestamp(),
            acv.is_live_and_has_ttl() ? acv.expiry().time_since_epoch().count() : -1,
            acv.is_live_and_has_ttl() ? acv.ttl().count() : 0);
    } else {
        return fmt_print(os, "atomic_cell{{DEAD;ts={:d};deletion_time={:d}}}",
            acv.timestamp(), acv.deletion_time().time_since_epoch().count());
    }
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell& ac) {
    return os << atomic_cell_view(ac);
}

sstring database::get_available_index_name(const sstring &ks_name, const sstring &cf_name,
                                           std::optional<sstring> index_name_root) const
{
    auto existing_names = existing_index_names(ks_name);
    auto base_name = index_metadata::get_default_index_name(cf_name, index_name_root);
    sstring accepted_name = base_name;
    int i = 0;
    while (existing_names.count(accepted_name) > 0) {
        accepted_name = base_name + "_" + std::to_string(++i);
    }
    return accepted_name;
}

schema_ptr database::find_indexed_table(const sstring& ks_name, const sstring& index_name) const {
    for (auto& schema : find_keyspace(ks_name).metadata()->tables()) {
        if (schema->has_index(index_name)) {
            return schema;
        }
    }
    return nullptr;
}

future<> database::close_tables(table_kind kind_to_close) {
    return parallel_for_each(_column_families, [this, kind_to_close](auto& val_pair) {
        table_kind k = is_system_table(*val_pair.second->schema()) ? table_kind::system : table_kind::user;
        if (k == kind_to_close) {
            return val_pair.second->stop();
        } else {
            return make_ready_future<>();
        }
    });
}

future<> stop_database(sharded<database>& sdb) {
    return sdb.invoke_on_all([](database& db) {
        return db.get_compaction_manager().stop();
    }).then([&sdb] {
        // Closing a table can cause us to find a large partition. Since we want to record that, we have to close
        // system.large_partitions after the regular tables.
        return sdb.invoke_on_all([](database& db) {
            return db.close_tables(database::table_kind::user);
        });
    }).then([&sdb] {
        return sdb.invoke_on_all([](database& db) {
            return db.close_tables(database::table_kind::system);
        });
    }).then([&sdb] {
        return sdb.invoke_on_all([](database& db) {
            return db.stop_large_data_handler();
        });
    });
}

future<> database::stop_large_data_handler() {
    return _large_data_handler->stop();
}

future<>
database::stop() {
    assert(_large_data_handler->stopped());
    assert(_compaction_manager->stopped());

    // try to ensure that CL has done disk flushing
    future<> maybe_shutdown_commitlog = _commitlog != nullptr ? _commitlog->shutdown() : make_ready_future<>();
    return maybe_shutdown_commitlog.then([this] {
        return _view_update_concurrency_sem.wait(max_memory_pending_view_updates());
    }).then([this] {
        if (_commitlog != nullptr) {
            return _commitlog->release();
        }
        return make_ready_future<>();
    }).then([this] {
        return _system_dirty_memory_manager.shutdown();
    }).then([this] {
        return _dirty_memory_manager.shutdown();
    }).then([this] {
        return _streaming_dirty_memory_manager.shutdown();
    }).then([this] {
        return _memtable_controller.shutdown();
    });
}

future<> database::flush_all_memtables() {
    return parallel_for_each(_column_families, [this] (auto& cfp) {
        return cfp.second->flush();
    });
}

future<> database::truncate(sstring ksname, sstring cfname, timestamp_func tsf) {
    auto& ks = find_keyspace(ksname);
    auto& cf = find_column_family(ksname, cfname);
    return truncate(ks, cf, std::move(tsf));
}

future<> database::truncate(const keyspace& ks, column_family& cf, timestamp_func tsf, bool with_snapshot) {
    return cf.run_async([this, &ks, &cf, tsf = std::move(tsf), with_snapshot] {
        const auto auto_snapshot = with_snapshot && get_config().auto_snapshot();
        const auto should_flush = auto_snapshot;

        // Force mutations coming in to re-acquire higher rp:s
        // This creates a "soft" ordering, in that we will guarantee that
        // any sstable written _after_ we issue the flush below will
        // only have higher rp:s than we will get from the discard_sstable
        // call.
        auto low_mark = cf.set_low_replay_position_mark();


        return cf.run_with_compaction_disabled([this, &cf, should_flush, auto_snapshot, tsf = std::move(tsf), low_mark]() mutable {
            future<> f = make_ready_future<>();
            if (should_flush) {
                // TODO:
                // this is not really a guarantee at all that we've actually
                // gotten all things to disk. Again, need queue-ish or something.
                f = cf.flush();
            } else {
                f = cf.clear();
            }
            return f.then([this, &cf, auto_snapshot, tsf = std::move(tsf), low_mark, should_flush] {
                dblog.debug("Discarding sstable data for truncated CF + indexes");
                // TODO: notify truncation

                return tsf().then([this, &cf, auto_snapshot, low_mark, should_flush](db_clock::time_point truncated_at) {
                    future<> f = make_ready_future<>();
                    if (auto_snapshot) {
                        auto name = format("{:d}-{}", truncated_at.time_since_epoch().count(), cf.schema()->cf_name());
                        f = cf.snapshot(name);
                    }
                    return f.then([this, &cf, truncated_at, low_mark, should_flush] {
                        return cf.discard_sstables(truncated_at).then([this, &cf, truncated_at, low_mark, should_flush](db::replay_position rp) {
                            // TODO: indexes.
                            // Note: since discard_sstables was changed to only count tables owned by this shard,
                            // we can get zero rp back. Changed assert, and ensure we save at least low_mark.
                            assert(low_mark <= rp || rp == db::replay_position());
                            rp = std::max(low_mark, rp);
                            return truncate_views(cf, truncated_at, should_flush).then([&cf, truncated_at, rp] {
                                return db::system_keyspace::save_truncation_record(cf, truncated_at, rp);
                            });
                        });
                    });
                });
            });
        });
    });
}

future<> database::truncate_views(const column_family& base, db_clock::time_point truncated_at, bool should_flush) {
    return parallel_for_each(base.views(), [this, truncated_at, should_flush] (view_ptr v) {
        auto& vcf = find_column_family(v);
        return vcf.run_with_compaction_disabled([&vcf, truncated_at, should_flush] {
            return (should_flush ? vcf.flush() : vcf.clear()).then([&vcf, truncated_at, should_flush] {
                return vcf.discard_sstables(truncated_at).then([&vcf, truncated_at, should_flush](db::replay_position rp) {
                    return db::system_keyspace::save_truncation_record(vcf, truncated_at, rp);
                });
            });
        });
    });
}

const sstring& database::get_snitch_name() const {
    return _cfg->endpoint_snitch();
}

// For the filesystem operations, this code will assume that all keyspaces are visible in all shards
// (as we have been doing for a lot of the other operations, like the snapshot itself).
future<> database::clear_snapshot(sstring tag, std::vector<sstring> keyspace_names) {
    std::vector<sstring> data_dirs = _cfg->data_file_directories();
    lw_shared_ptr<lister::dir_entry_types> dirs_only_entries_ptr = make_lw_shared<lister::dir_entry_types>({ directory_entry_type::directory });
    lw_shared_ptr<sstring> tag_ptr = make_lw_shared<sstring>(std::move(tag));
    std::unordered_set<sstring> ks_names_set(keyspace_names.begin(), keyspace_names.end());

    return parallel_for_each(data_dirs, [this, tag_ptr, ks_names_set = std::move(ks_names_set), dirs_only_entries_ptr] (const sstring& parent_dir) {
        std::unique_ptr<lister::filter_type> filter = std::make_unique<lister::filter_type>([] (const fs::path& parent_dir, const directory_entry& dir_entry) { return true; });

        // if specific keyspaces names were given - filter only these keyspaces directories
        if (!ks_names_set.empty()) {
            filter = std::make_unique<lister::filter_type>([ks_names_set = std::move(ks_names_set)] (const fs::path& parent_dir, const directory_entry& dir_entry) {
                return ks_names_set.find(dir_entry.name) != ks_names_set.end();
            });
        }

        //
        // The keyspace data directories and their snapshots are arranged as follows:
        //
        //  <data dir>
        //  |- <keyspace name1>
        //  |  |- <column family name1>
        //  |     |- snapshots
        //  |        |- <snapshot name1>
        //  |          |- <snapshot file1>
        //  |          |- <snapshot file2>
        //  |          |- ...
        //  |        |- <snapshot name2>
        //  |        |- ...
        //  |  |- <column family name2>
        //  |  |- ...
        //  |- <keyspace name2>
        //  |- ...
        //
        return lister::scan_dir(parent_dir, *dirs_only_entries_ptr, [this, tag_ptr, dirs_only_entries_ptr] (fs::path parent_dir, directory_entry de) {
            // KS directory
            return lister::scan_dir(parent_dir / de.name, *dirs_only_entries_ptr, [this, tag_ptr, dirs_only_entries_ptr] (fs::path parent_dir, directory_entry de) mutable {
                // CF directory
                return lister::scan_dir(parent_dir / de.name, *dirs_only_entries_ptr, [this, tag_ptr, dirs_only_entries_ptr] (fs::path parent_dir, directory_entry de) mutable {
                    // "snapshots" directory
                    fs::path snapshots_dir(parent_dir / de.name);
                    if (tag_ptr->empty()) {
                        dblog.info("Removing {}", snapshots_dir.native());
                        // kill the whole "snapshots" subdirectory
                        return lister::rmdir(std::move(snapshots_dir));
                    } else {
                        return lister::scan_dir(std::move(snapshots_dir), *dirs_only_entries_ptr, [this, tag_ptr] (fs::path parent_dir, directory_entry de) {
                            fs::path snapshot_dir(parent_dir / de.name);
                            dblog.info("Removing {}", snapshot_dir.native());
                            return lister::rmdir(std::move(snapshot_dir));
                        }, [tag_ptr] (const fs::path& parent_dir, const directory_entry& dir_entry) { return dir_entry.name == *tag_ptr; });
                    }
                 }, [] (const fs::path& parent_dir, const directory_entry& dir_entry) { return dir_entry.name == "snapshots"; });
            });
        }, *filter);
    });
}

future<utils::UUID> update_schema_version(distributed<service::storage_proxy>& proxy)
{
    return db::schema_tables::calculate_schema_digest(proxy).then([&proxy] (utils::UUID uuid) {
        return proxy.local().get_db().invoke_on_all([uuid] (database& db) {
            db.update_version(uuid);
        }).then([uuid] {
            return db::system_keyspace::update_schema_version(uuid);
        }).then([uuid] {
            dblog.info("Schema version changed to {}", uuid);
            return uuid;
        });
    });
}

future<> announce_schema_version(utils::UUID schema_version) {
    return service::get_local_migration_manager().passive_announce(schema_version);
}

future<> update_schema_version_and_announce(distributed<service::storage_proxy>& proxy)
{
    return update_schema_version(proxy).then([] (utils::UUID uuid) {
        return announce_schema_version(uuid);
    });
}

std::ostream& operator<<(std::ostream& os, const user_types_metadata& m) {
    os << "org.apache.cassandra.config.UTMetaData@" << &m;
    return os;
}

std::ostream& operator<<(std::ostream& os, const keyspace_metadata& m) {
    os << "KSMetaData{";
    os << "name=" << m._name;
    os << ", strategyClass=" << m._strategy_name;
    os << ", strategyOptions={";
    int n = 0;
    for (auto& p : m._strategy_options) {
        if (n++ != 0) {
            os << ", ";
        }
        os << p.first << "=" << p.second;
    }
    os << "}";
    os << ", cfMetaData={";
    n = 0;
    for (auto& p : m._cf_meta_data) {
        if (n++ != 0) {
            os << ", ";
        }
        os << p.first << "=" << p.second;
    }
    os << "}";
    os << ", durable_writes=" << m._durable_writes;
    os << ", userTypes=" << m._user_types;
    os << "}";
    return os;
}

template <typename T>
using foreign_unique_ptr = foreign_ptr<std::unique_ptr<T>>;

flat_mutation_reader make_multishard_streaming_reader(distributed<database>& db, dht::i_partitioner& partitioner, schema_ptr schema,
        std::function<std::optional<dht::partition_range>()> range_generator) {
    class streaming_reader_lifecycle_policy
            : public reader_lifecycle_policy
            , public enable_shared_from_this<streaming_reader_lifecycle_policy> {
        struct reader_context {
            foreign_unique_ptr<const dht::partition_range> range;
            foreign_unique_ptr<utils::phased_barrier::operation> read_operation;
            reader_concurrency_semaphore* semaphore;
        };
        distributed<database>& _db;
        std::vector<reader_context> _contexts;
    public:
        explicit streaming_reader_lifecycle_policy(distributed<database>& db) : _db(db), _contexts(smp::count) {
        }
        virtual flat_mutation_reader create_reader(
                schema_ptr schema,
                const dht::partition_range& range,
                const query::partition_slice&,
                const io_priority_class& pc,
                tracing::trace_state_ptr,
                mutation_reader::forwarding fwd_mr) override {
            const auto shard = engine().cpu_id();
            auto& cf = _db.local().find_column_family(schema);

            _contexts[shard].range = make_foreign(std::make_unique<const dht::partition_range>(range));
            _contexts[shard].read_operation = make_foreign(std::make_unique<utils::phased_barrier::operation>(cf.read_in_progress()));
            _contexts[shard].semaphore = &cf.streaming_read_concurrency_semaphore();

            return cf.make_streaming_reader(std::move(schema), *_contexts[shard].range, fwd_mr);
        }
        virtual void destroy_reader(shard_id shard, future<stopped_reader> reader_fut) noexcept override {
            reader_fut.then([this, zis = shared_from_this(), shard] (stopped_reader&& reader) mutable {
                return smp::submit_to(shard, [ctx = std::move(_contexts[shard]), handle = std::move(reader.handle)] () mutable {
                    ctx.semaphore->unregister_inactive_read(std::move(*handle));
                });
            });
        }
        virtual reader_concurrency_semaphore& semaphore() override {
            return *_contexts[engine().cpu_id()].semaphore;
        }
    };
    auto ms = mutation_source([&db, &partitioner] (schema_ptr s,
            const dht::partition_range& pr,
            const query::partition_slice& ps,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            streamed_mutation::forwarding,
            mutation_reader::forwarding fwd_mr) {
        return make_multishard_combining_reader(make_shared<streaming_reader_lifecycle_policy>(db), partitioner, std::move(s), pr, ps, pc,
                std::move(trace_state), fwd_mr);
    });
    return make_flat_multi_range_reader(std::move(schema), std::move(ms), std::move(range_generator), schema->full_slice(),
            service::get_local_streaming_read_priority(), {}, mutation_reader::forwarding::no);
}

std::ostream& operator<<(std::ostream& os, gc_clock::time_point tp) {
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    std::ostream tmp(os.rdbuf());
    tmp << std::setw(12) << sec;
    return os;
}

const timeout_config infinite_timeout_config = {
        // not really infinite, but long enough
        1h, 1h, 1h, 1h, 1h, 1h, 1h,
};
