/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "service/mapreduce_service.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/smp.hh>
#include <stdexcept>

#include "db/consistency_level.hh"
#include "dht/sharder.hh"
#include "exceptions/exceptions.hh"
#include "gms/gossiper.hh"
#include "idl/mapreduce_request.dist.hh"
#include "locator/abstract_replication_strategy.hh"
#include "utils/error_injection.hh"
#include "utils/log.hh"
#include "message/messaging_service.hh"
#include "query-request.hh"
#include "query_ranges_to_vnodes.hh"
#include "replica/database.hh"
#include "schema/schema.hh"
#include "schema/schema_registry.hh"
#include <seastar/core/future.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/when_all.hh>
#include "service/pager/query_pagers.hh"
#include "tracing/trace_state.hh"
#include "tracing/tracing.hh"
#include "types/types.hh"
#include "service/storage_proxy.hh"

#include "cql3/column_identifier.hh"
#include "cql3/cql_config.hh"
#include "cql3/query_options.hh"
#include "cql3/result_set.hh"
#include "cql3/selection/raw_selector.hh"
#include "cql3/selection/selection.hh"
#include "cql3/functions/functions.hh"
#include "cql3/functions/aggregate_fcts.hh"
#include "cql3/expr/expr-utils.hh"

namespace service {

static constexpr int DEFAULT_INTERNAL_PAGING_SIZE = 10000;
static logging::logger flogger("forward_service"); // not "mapreduce", for compatibility with dtest

static std::vector<::shared_ptr<db::functions::aggregate_function>> get_functions(const query::mapreduce_request& request);

class mapreduce_aggregates {
private:
    std::vector<::shared_ptr<db::functions::aggregate_function>> _funcs;
    std::vector<db::functions::stateless_aggregate_function> _aggrs;
public:
    mapreduce_aggregates(const query::mapreduce_request& request);
    void merge(query::mapreduce_result& result, query::mapreduce_result&& other);
    void finalize(query::mapreduce_result& result);

    template<typename Func>
    auto with_thread_if_needed(Func&& func) const {
        if (requires_thread()) {
            return async(std::move(func));
        } else {
            return futurize_invoke(std::move(func));
        }
    }

    bool requires_thread() const {
        return std::any_of(_funcs.cbegin(), _funcs.cend(), [](const ::shared_ptr<db::functions::aggregate_function>& f) {
            return f->requires_thread();
        });
    }
};

mapreduce_aggregates::mapreduce_aggregates(const query::mapreduce_request& request) {
    _funcs = get_functions(request);
    std::vector<db::functions::stateless_aggregate_function> aggrs;

    for (auto& func: _funcs) {
        aggrs.push_back(func->get_aggregate());
    }
    _aggrs = std::move(aggrs);
}

void mapreduce_aggregates::merge(query::mapreduce_result &result, query::mapreduce_result&& other) {
    if (result.query_results.empty()) {
        result.query_results = std::move(other.query_results);
        return;
    } else if (other.query_results.empty()) {
        return;
    }

    if (result.query_results.size() != other.query_results.size() || result.query_results.size() != _aggrs.size()) {
        on_internal_error(
            flogger,
            format("mapreduce_aggregates::merge(): operation cannot be completed due to invalid argument sizes. "
                    "this.aggrs.size(): {} "
                    "result.query_result.size(): {} "
                    "other.query_results.size(): {} ",
                    _aggrs.size(), result.query_results.size(), other.query_results.size())
        );
    }

    for (size_t i = 0; i < _aggrs.size(); i++) {
        result.query_results[i] = _aggrs[i].state_reduction_function->execute(std::vector({std::move(result.query_results[i]), std::move(other.query_results[i])}));
    }
}

void mapreduce_aggregates::finalize(query::mapreduce_result &result) {
    if (result.query_results.empty()) {
        // An empty result means that we didn't send the aggregation request
        // to any node. I.e., it was a query that matched no partition, such
        // as "WHERE p IN ()". We need to build a fake result with the result
        // of empty aggregation.
        for (size_t i = 0; i < _aggrs.size(); i++) {
            result.query_results.push_back(_aggrs[i].state_to_result_function
                    ? _aggrs[i].state_to_result_function->execute(std::vector({_aggrs[i].initial_state}))
                    : _aggrs[i].initial_state);
        }
        return;
    }
    if (result.query_results.size() != _aggrs.size()) {
        on_internal_error(
            flogger,
            format("mapreduce_aggregates::finalize(): operation cannot be completed due to invalid argument sizes. "
                    "this.aggrs.size(): {} "
                    "result.query_result.size(): {} ",
                    _aggrs.size(), result.query_results.size())
        );
    }

    for (size_t i = 0; i < _aggrs.size(); i++) {
        result.query_results[i] = _aggrs[i].state_to_result_function
            ? _aggrs[i].state_to_result_function->execute(std::vector({std::move(result.query_results[i])}))
            : result.query_results[i];
    }
}

static std::vector<::shared_ptr<db::functions::aggregate_function>> get_functions(const query::mapreduce_request& request) {
    
    schema_ptr schema = local_schema_registry().get(request.cmd.schema_version);
    std::vector<::shared_ptr<db::functions::aggregate_function>> aggrs;

    auto name_as_type = [&] (const sstring& name) -> data_type {
        auto t = schema->get_column_definition(to_bytes(name))->type->underlying_type();

        if (t->is_counter()) {
            return long_type;
        }
        return t;
    };

    for (size_t i = 0; i < request.reduction_types.size(); i++) {
        ::shared_ptr<db::functions::aggregate_function> aggr;

        if (!request.aggregation_infos) {
            if (request.reduction_types[i] == query::mapreduce_request::reduction_type::aggregate) {
                throw std::runtime_error("No aggregation info for reduction type aggregation.");
            }

            auto name = db::functions::function_name::native_function("countRows");
            auto func = cql3::functions::instance().find(name, {});
            aggr = dynamic_pointer_cast<db::functions::aggregate_function>(func);
            if (!aggr) {
                throw std::runtime_error("Count function not found.");
            }
        } else {
            auto& info = request.aggregation_infos.value()[i];
            auto types = info.column_names | std::views::transform(name_as_type) | std::ranges::to<std::vector<data_type>>();
            
            auto func = cql3::functions::instance().mock_get(info.name, types);
            if (!func) {
                throw std::runtime_error(format("Cannot mock aggregate function {}", info.name));    
            }

            aggr = dynamic_pointer_cast<db::functions::aggregate_function>(func);
            if (!aggr) {
                throw std::runtime_error(format("Aggregate function {} not found.", info.name));
            }
        }
        aggrs.emplace_back(aggr);
    }
    
    return aggrs;
}

static const dht::token& end_token(const dht::partition_range& r) {
    static const dht::token max_token = dht::maximum_token();
    return r.end() ? r.end()->value().token() : max_token;
}

static void retain_local_endpoints(const locator::topology& topo, host_id_vector_replica_set& eps) {
    auto [b, e] = std::ranges::remove_if(eps, std::not_fn(topo.get_local_dc_filter()));
    eps.erase(b, e);
}

// Given an initial partition range vector, iterate through ranges owned by
// current shard.
class partition_ranges_owned_by_this_shard {
    schema_ptr _s;
    // _partition_ranges will contain a list of partition ranges that are known
    // to be owned by this node. We'll further need to split each such range to
    // the pieces owned by the current shard, using _intersecter.
    const dht::partition_range_vector _partition_ranges;
    size_t _range_idx;
    std::optional<dht::ring_position_range_sharder> _intersecter;
    locator::effective_replication_map_ptr _erm;
    std::optional<shard_id> forced_shard;
public:
    partition_ranges_owned_by_this_shard(schema_ptr s, dht::partition_range_vector v, std::optional<shard_id> forced_shard)
        :  _s(s)
        , _partition_ranges(v)
        , _range_idx(0)
        , _erm(_s->table().get_effective_replication_map())
        , forced_shard(forced_shard)
    {}

    // Return the next partition_range owned by this shard, or nullopt when the
    // iteration ends.
    std::optional<dht::partition_range> next(const schema& s) {

        // If forced shard is set and supported, return all ranges for that shard
        if (forced_shard.has_value() && forced_shard.value() < seastar::smp::count) {
            if (forced_shard.value() != this_shard_id() || _range_idx == _partition_ranges.size()) {
                return std::nullopt;
            } else {
                return _partition_ranges[_range_idx++];
            }
        }

        // We may need three or more iterations in the following loop if a
        // vnode doesn't intersect with the given shard at all (such a small
        // vnode is unlikely, but possible). The loop cannot be infinite
        // because each iteration of the loop advances _range_idx.
        for (;;) {
            if (_intersecter) {
                // Filter out ranges that are not owned by this shard.
                while (auto ret = _intersecter->next(s)) {
                    if (ret->shard == this_shard_id()) {
                        return {ret->ring_range};
                    }
                }

                // Done with this range, go to next one.
                ++_range_idx;
                _intersecter = std::nullopt;
            }

            if (_range_idx == _partition_ranges.size()) {
                return std::nullopt;
            }

            _intersecter.emplace(_erm->get_sharder(*_s), std::move(_partition_ranges[_range_idx]));
        }
    }
};

// `retrying_dispatcher` is a class that dispatches mapreduce_requests to other
// nodes. In case of a failure, local retries are available - request being
// retried is executed on the super-coordinator.
class retrying_dispatcher {
    mapreduce_service& _mapreducer;
    tracing::trace_state_ptr _tr_state;
    std::optional<tracing::trace_info> _tr_info;

    future<query::mapreduce_result> dispatch_to_shards_locally(query::mapreduce_request req, std::optional<tracing::trace_info> tr_info) {
        try {
            co_return co_await _mapreducer.dispatch_to_shards(req, _tr_info);
        } catch (const std::exception& e) {
            // For remote rpc_calls, the remote exceptions are converted to rpc::remote_verb_error.
            // This catch behaves similarly for local dispatch_to_shards, to prevent from having two different
            // behaviours for local and remote calls.
            std::throw_with_nested(std::runtime_error(e.what()));
        }
    }
public:
    retrying_dispatcher(mapreduce_service& mapreducer, tracing::trace_state_ptr tr_state)
        : _mapreducer(mapreducer),
        _tr_state(tr_state),
        _tr_info(tracing::make_trace_info(tr_state))
    {}

    future<query::mapreduce_result> dispatch_to_node(const locator::effective_replication_map& erm, locator::host_id id, query::mapreduce_request req) {
        if (_mapreducer._proxy.is_me(erm, id)) {
            co_return co_await dispatch_to_shards_locally(req, _tr_info);
        }

        _mapreducer._stats.requests_dispatched_to_other_nodes += 1;

        // Check for a shutdown request before sending a mapreduce_request to
        // another node. During the drain process, the messaging service is shut
        // down early (but not earlier than the mapreduce_service::shutdown
        // invocation), so by performing this check, we can prevent hanging on
        // the RPC call.
        if (_mapreducer._shutdown) {
            throw std::runtime_error("mapreduce_service is shutting down");
        }

        // Try to send this mapreduce_request to another node.
        try {
            co_return co_await ser::mapreduce_request_rpc_verbs::send_mapreduce_request(
                &_mapreducer._messaging, id, _mapreducer._abort_outgoing_tasks, req, _tr_info
            );
        } catch (rpc::closed_error& e) {
            if (_mapreducer._shutdown) {
                // Do not retry if shutting down.
                throw;
            }
            // In case of mapreduce failure, retry using super-coordinator as a coordinator
            flogger.warn("retrying mapreduce_request={} on a super-coordinator after failing to send it to {} ({})", req, id, e.what());
            tracing::trace(_tr_state, "retrying mapreduce_request={} on a super-coordinator after failing to send it to {} ({})", req, id, e.what());
            // Fall through since we cannot co_await in a catch block.
        }
        co_return co_await dispatch_to_shards_locally(req, _tr_info);
    }
};

future<> mapreduce_service::stop() {
    return uninit_messaging_service();
}

// Due to `cql3::selection::selection` not being serializable, it cannot be
// stored in `mapreduce_request`. It has to mocked on the receiving node,
// based on requested reduction types.
static shared_ptr<cql3::selection::selection> mock_selection(
    query::mapreduce_request& request,
    schema_ptr schema,
    replica::database& db
) {
    std::vector<cql3::selection::prepared_selector> prepared_selectors;

    auto functions = get_functions(request);

    auto mock_singular_selection = [&] (
        const ::shared_ptr<db::functions::aggregate_function>& aggr_function,
        const query::mapreduce_request::reduction_type& reduction,
        const std::optional<query::mapreduce_request::aggregation_info>& info
    ) {
        auto name_as_expression = [] (const sstring& name) -> cql3::expr::expression {
            constexpr bool keep_case = true;
            return cql3::expr::unresolved_identifier {
                make_shared<cql3::column_identifier_raw>(name, keep_case)
            };
        };

        if (reduction == query::mapreduce_request::reduction_type::count) {
            auto count_expr = cql3::expr::function_call{
                .func = cql3::functions::aggregate_fcts::make_count_rows_function(),
                .args = {},
            };
            auto column_identifier = make_shared<cql3::column_identifier>("count", false);
            return cql3::selection::prepared_selector{std::move(count_expr), column_identifier};
        }

        if (!info) {
            on_internal_error(flogger, "No aggregation info for reduction type aggregation.");
        }

        auto reducible_aggr = aggr_function->reducible_aggregate_function();
        auto arg_exprs = info->column_names | std::views::transform(name_as_expression) | std::ranges::to<std::vector<cql3::expr::expression>>();
        auto fc_expr = cql3::expr::function_call{reducible_aggr, arg_exprs};
        auto column_identifier = make_shared<cql3::column_identifier>(info->name.name, false);
        auto prepared_expr = cql3::expr::prepare_expression(fc_expr, db.as_data_dictionary(), "", schema.get(), nullptr);
        return cql3::selection::prepared_selector{std::move(prepared_expr), column_identifier};
    };

    for (size_t i = 0; i < request.reduction_types.size(); i++) {
        auto info = (request.aggregation_infos) ? std::optional(request.aggregation_infos->at(i)) : std::nullopt;
        prepared_selectors.emplace_back(mock_singular_selection(functions[i], request.reduction_types[i], info));
    }

    return cql3::selection::selection::from_selectors(db.as_data_dictionary(), schema, schema->ks_name(), std::move(prepared_selectors));
}

future<query::mapreduce_result> mapreduce_service::dispatch_to_shards(
    query::mapreduce_request req,
    std::optional<tracing::trace_info> tr_info
) {
    co_await utils::get_local_injector().inject("mapreduce_pause_dispatch_to_shards", utils::wait_for_message(5min));

    _stats.requests_dispatched_to_own_shards += 1;
    std::optional<query::mapreduce_result> result;
    std::vector<future<query::mapreduce_result>> futures;

    for (const auto& s : smp::all_cpus()) {
        futures.push_back(container().invoke_on(s, [req, tr_info] (auto& fs) {
            return fs.execute_on_this_shard(req, tr_info);
        }));
    }
    auto results = co_await when_all_succeed(futures.begin(), futures.end());

    mapreduce_aggregates aggrs(req);
    co_return co_await aggrs.with_thread_if_needed([&aggrs, req, results = std::move(results), result = std::move(result)] () mutable {
        for (auto&& r : results) {
            if (result) {
                aggrs.merge(*result, std::move(r));
            }
            else {
                result = r;
            }
        }

        flogger.debug("on node execution result is {}", seastar::value_of([&req, &result] {
            return query::mapreduce_result::printer {
                .functions = get_functions(req),
                .res = *result
            };})
        );

        return *result;
    });
}

static lowres_clock::time_point compute_timeout(const query::mapreduce_request& req) {
    lowres_system_clock::duration time_left = req.timeout - lowres_system_clock::now();
    lowres_clock::time_point timeout_point = lowres_clock::now() + time_left;

    return timeout_point;
}

// This function executes mapreduce_request on a shard.
// It retains partition ranges owned by this shard from requested partition
// ranges vector, so that only owned ones are queried.
future<query::mapreduce_result> mapreduce_service::execute_on_this_shard(
    query::mapreduce_request req,
    std::optional<tracing::trace_info> tr_info
) {
    tracing::trace_state_ptr tr_state;
    if (tr_info) {
        tr_state = tracing::tracing::get_local_tracing_instance().create_session(*tr_info);
        tracing::begin(tr_state);
    }

    tracing::trace(tr_state, "Executing mapreduce_request");
    _stats.requests_executed += 1;

    schema_ptr schema = local_schema_registry().get(req.cmd.schema_version);

    auto timeout = compute_timeout(req);
    auto now = gc_clock::now();

    auto selection = mock_selection(req, schema, _db.local());
    auto query_state = make_lw_shared<service::query_state>(
        client_state::for_internal_calls(),
        tr_state,
        empty_service_permit() // FIXME: it probably shouldn't be empty.
    );
    auto query_options = make_lw_shared<cql3::query_options>(
        cql3::default_cql_config,
        req.cl,
        std::optional<std::vector<std::string_view>>(), // Represents empty names.
        std::vector<cql3::raw_value>(), // Represents empty values.
        true, // Skip metadata.
        cql3::query_options::specific_options::DEFAULT
    );

    auto rs_builder = cql3::selection::result_set_builder(
        *selection,
        now,
        nullptr,
        std::vector<size_t>() // Represents empty GROUP BY indices.
    );

    // We serve up to 256 ranges at a time to avoid allocating a huge vector for ranges
    static constexpr size_t max_ranges = 256;
    dht::partition_range_vector ranges_owned_by_this_shard;
    ranges_owned_by_this_shard.reserve(std::min(max_ranges, req.pr.size()));
    partition_ranges_owned_by_this_shard owned_iter(schema, std::move(req.pr), req.shard_id_hint);

    std::optional<dht::partition_range> current_range;
    do {
        while ((current_range = owned_iter.next(*schema))) {
            ranges_owned_by_this_shard.push_back(std::move(*current_range));
            if (ranges_owned_by_this_shard.size() >= max_ranges) {
                break;
            }
        }
        if (ranges_owned_by_this_shard.empty()) {
            break;
        }
        flogger.trace("Forwarding to {} ranges owned by this shard", ranges_owned_by_this_shard.size());

        auto pager = service::pager::query_pagers::pager(
            _proxy,
            schema,
            selection,
            *query_state,
            *query_options,
            make_lw_shared<query::read_command>(req.cmd),
            std::move(ranges_owned_by_this_shard),
            nullptr // No filtering restrictions
        );

        // Execute query.
        while (!pager->is_exhausted()) {
            // It is necessary to check for a shutdown request before each
            // fetch_page operation. During the drain process, the messaging
            // service is shut down early (but not earlier than the
            // mapreduce_service::shutdown invocation), so by performing this
            // check, we can prevent hanging on the RPC call (which can be made
            // during fetching a page).
            if (_shutdown) {
                throw std::runtime_error("mapreduce_service is shutting down");
            }

            co_await pager->fetch_page(rs_builder, DEFAULT_INTERNAL_PAGING_SIZE, now, timeout);
        }

        ranges_owned_by_this_shard.clear();
    } while (current_range);

    co_return co_await rs_builder.with_thread_if_needed([&req, &rs_builder, reductions = req.reduction_types, tr_state = std::move(tr_state)] {
        auto rs = rs_builder.build();
        auto& rows = rs->rows();
        if (rows.size() != 1) {
            flogger.error("aggregation result row count != 1");
            throw std::runtime_error("aggregation result row count != 1");
        }
        if (rows[0].size() != reductions.size()) {
            flogger.error("aggregation result column count does not match requested column count");
            throw std::runtime_error("aggregation result column count does not match requested column count");
        }
        query::mapreduce_result res = { .query_results = rows[0] | std::views::transform([] (const managed_bytes_opt& x) { return to_bytes_opt(x); }) | std::ranges::to<std::vector<bytes_opt>>() };

        auto printer = seastar::value_of([&req, &res] {
            return query::mapreduce_result::printer {
                .functions = get_functions(req),
                .res = res
            };
        });
        tracing::trace(tr_state, "On shard execution result is {}", printer);
        flogger.debug("on shard execution result is {}", printer);

        return res;
    });
}

void mapreduce_service::init_messaging_service() {
    ser::mapreduce_request_rpc_verbs::register_mapreduce_request(
        &_messaging,
        [this](query::mapreduce_request req, std::optional<tracing::trace_info> tr_info) -> future<query::mapreduce_result> {
            return dispatch_to_shards(req, tr_info);
        }
    );
}

future<> mapreduce_service::uninit_messaging_service() {
    return ser::mapreduce_request_rpc_verbs::unregister(&_messaging);
}

future<> mapreduce_service::dispatch_range_and_reduce(const locator::effective_replication_map_ptr& erm, retrying_dispatcher& dispatcher, const query::mapreduce_request& req, query::mapreduce_request&& req_with_modified_pr, locator::host_id addr, query::mapreduce_result& shared_accumulator, tracing::trace_state_ptr tr_state) {
    tracing::trace(tr_state, "Sending mapreduce_request to {}", addr);
    flogger.debug("dispatching mapreduce_request={} to address={}", req_with_modified_pr, addr);

    query::mapreduce_result partial_result = co_await dispatcher.dispatch_to_node(*erm, addr, std::move(req_with_modified_pr));
    auto partial_printer = seastar::value_of([&req, &partial_result] {
        return query::mapreduce_result::printer {
            .functions = get_functions(req),
            .res = partial_result
        };
    });
    tracing::trace(tr_state, "Received mapreduce_result={} from {}", partial_printer, addr);
    flogger.debug("received mapreduce_result={} from {}", partial_printer, addr);

    auto aggrs = mapreduce_aggregates(req);
    // Anytime this coroutine yields, other coroutines may want to write to `shared_accumulator`.
    // As merging can yield internally, merging directly to `shared_accumulator` would result in race condition.
    // We can safely write to `shared_accumulator` only when it is empty.
    while (!shared_accumulator.query_results.empty()) {
        // Move `shared_accumulator` content to local variable. Leave `shared_accumulator` empty - now other coroutines can safely write to it.
        query::mapreduce_result previous_results = std::exchange(shared_accumulator, {});
        // Merge two local variables - it can yield.
        co_await aggrs.with_thread_if_needed([&previous_results, &aggrs, &partial_result] () mutable {
            aggrs.merge(partial_result, std::move(previous_results));
        });
     // `partial_result` now contains results merged by this coroutine, but `shared_accumulator` might have been updated by others.
    }
    // `shared_accumulator` is empty, we can atomically write results merged by this coroutine.
    shared_accumulator = std::move(partial_result);
}

std::optional<dht::partition_range> get_next_partition_range(query_ranges_to_vnodes_generator& generator) {
    if (auto vnode = generator(1); !vnode.empty()) {
        return vnode[0];
    }
    return {};
} 

future<> mapreduce_service::dispatch_to_vnodes(schema_ptr schema, replica::column_family& cf, query::mapreduce_request& req, query::mapreduce_result& result, tracing::trace_state_ptr tr_state) {
    auto erm = cf.get_effective_replication_map();
    // Group vnodes by assigned endpoint.
    std::map<locator::host_id, dht::partition_range_vector> vnodes_per_addr;
    const auto& topo = erm->get_topology();
    auto generator = query_ranges_to_vnodes_generator(erm->make_splitter(), schema, req.pr);
    while (std::optional<dht::partition_range> vnode = get_next_partition_range(generator)) {
        host_id_vector_replica_set live_endpoints = _proxy.get_live_endpoints(*erm, end_token(*vnode));
        // Do not choose an endpoint outside the current datacenter if a request has a local consistency
        if (db::is_datacenter_local(req.cl)) {
            retain_local_endpoints(topo, live_endpoints);
        }

        if (live_endpoints.empty()) {
            throw std::runtime_error("No live endpoint available");
        }

        vnodes_per_addr[*live_endpoints.begin()].push_back(std::move(*vnode));
        // can potentially stall e.g. with a large vnodes count.
        co_await coroutine::maybe_yield();
    }

    tracing::trace(tr_state, "Dispatching mapreduce_request to {} endpoints", vnodes_per_addr.size());
    flogger.debug("dispatching mapreduce_request to {} endpoints", vnodes_per_addr.size());

    retrying_dispatcher dispatcher(*this, tr_state);

    co_await coroutine::parallel_for_each(vnodes_per_addr,
            [&] (std::pair<const locator::host_id, dht::partition_range_vector>& vnodes_with_addr) -> future<> {
        co_await utils::get_local_injector().inject("mapreduce_pause_parallel_dispatch", utils::wait_for_message(5min));
        locator::host_id addr = vnodes_with_addr.first;
        query::mapreduce_request req_with_modified_pr = req;
        req_with_modified_pr.pr = std::move(vnodes_with_addr.second);
        co_await dispatch_range_and_reduce(erm, dispatcher, req, std::move(req_with_modified_pr), addr, result, tr_state);
    });
}

class mapreduce_tablet_algorithm {
private:
    class ranges_per_tablet_replica_t;
public:
    mapreduce_tablet_algorithm(mapreduce_service& mapreducer, schema_ptr schema, replica::column_family& cf,  query::mapreduce_request& req, query::mapreduce_result& result, tracing::trace_state_ptr tr_state)
        : _mapreducer(mapreducer),
        _schema(schema),
        _cf(cf),
        _req(req),
        _result(result),
        _tr_state(tr_state),
        _dispatcher(_mapreducer, tr_state),
        _limit_per_replica(2)
    {}

    future<> initialize_ranges_left() {
        auto erm = _cf.get_effective_replication_map();
        auto generator = query_ranges_to_vnodes_generator(erm->make_splitter(), _schema, _req.pr);
        while (std::optional<dht::partition_range> range = get_next_partition_range(generator)) {
            _ranges_left.insert(std::move(*range));
            // can potentially stall e.g. with a large tablet count.
            co_await coroutine::maybe_yield();
        }

        tracing::trace(_tr_state, "Dispatching {} ranges", _ranges_left.size());
        flogger.debug("Dispatching {} ranges", _ranges_left.size());
    }

    future<> prepare_ranges_per_replica() {
        auto erm = _cf.get_effective_replication_map();
        const auto& topo = erm->get_topology();
        auto& tablets = erm->get_token_metadata_ptr()->tablets().get_tablet_map(_schema->id());

        std::map<locator::tablet_replica, dht::partition_range_vector> ranges_per_tablet_replica_map;
        for (auto& range : _ranges_left) {
            auto tablet_id = tablets.get_tablet_id(end_token(range));
            const auto& tablet_info = tablets.get_tablet_info(tablet_id);

            size_t skipped_replicas = 0;
            for (auto& replica : tablet_info.replicas) {
                bool is_alive = _mapreducer._proxy.is_alive(*erm, replica.host);
                bool has_correct_locality = !db::is_datacenter_local(_req.cl) || topo.get_datacenter(replica.host) == topo.get_datacenter();
                if (is_alive && has_correct_locality) {
                    ranges_per_tablet_replica_map[replica].push_back(range);
                } else {
                    ++skipped_replicas;
                    if (skipped_replicas == tablet_info.replicas.size()) {
                        throw std::runtime_error("No live endpoint available");
                    }
                }
            }

            // can potentially stall e.g. with a large tablet count.
            co_await coroutine::maybe_yield();
        }

        _ranges_per_replica = ranges_per_tablet_replica_t(erm->get_token_metadata_ptr()->get_version(), std::move(ranges_per_tablet_replica_map));
    }

    std::vector<locator::tablet_replica> get_processing_slots() const {
        std::vector<locator::tablet_replica> slots;
        for (const auto& [replica, _] : _ranges_per_replica.get_map()) {
            for (size_t i = 0; i < _limit_per_replica; ++i) {
                slots.push_back(replica);
            }
        }
        return slots;
    }

    future<> dispatch_work_and_wait_to_finish() {
        while (_ranges_left.size() > 0) {
            co_await prepare_ranges_per_replica();

            co_await utils::get_local_injector().inject("mapreduce_pause_parallel_dispatch", utils::wait_for_message(5min));

            co_await coroutine::parallel_for_each(get_processing_slots(),
                    [&] (locator::tablet_replica replica) -> future<> {
                auto& ranges = _ranges_per_replica.get_map().find(replica)->second;
                for (const auto& range : ranges) {
                    auto erm = _cf.get_effective_replication_map();
                    if (!_ranges_per_replica.is_up_to_date(erm->get_token_metadata_ptr())) {
                        co_return;
                    }

                    auto it = _ranges_left.find(range);
                    if (it != _ranges_left.end()) {
                        _ranges_left.erase(it);
                        query::mapreduce_request req_with_modified_pr = _req;
                        req_with_modified_pr.pr = dht::partition_range_vector{range};
                        req_with_modified_pr.shard_id_hint = replica.shard;
                        co_await _mapreducer.dispatch_range_and_reduce(erm, _dispatcher, _req, std::move(req_with_modified_pr), replica.host, _result, _tr_state);
                    }

                    // can potentially stall e.g. with a large tablet count.
                    co_await coroutine::maybe_yield();
                }
            });
        }
    }

private:
    // The motivation for ranges_per_tablet_replica_t is to store
    // a `tablet_replica -> range` mapping that is guaranteed to be
    // consistent with the given topology version
    class ranges_per_tablet_replica_t {
    public:
        ranges_per_tablet_replica_t() = default;
        ranges_per_tablet_replica_t(topology::version_t topology_version, std::map<locator::tablet_replica, dht::partition_range_vector>&& map) 
            : _topology_version(topology_version)
            , _map(std::move(map))
        {}

        ranges_per_tablet_replica_t& operator=(ranges_per_tablet_replica_t&& other) noexcept = default;

        bool is_up_to_date(locator::token_metadata_ptr token_metadata_ptr) const {
            return _topology_version == token_metadata_ptr->get_version();
        }
        const std::map<locator::tablet_replica, dht::partition_range_vector>& get_map() const {
            return _map;
        }

    private:
        topology::version_t _topology_version;
        std::map<locator::tablet_replica, dht::partition_range_vector> _map;
    };

    mapreduce_service& _mapreducer;
    schema_ptr _schema;
    replica::column_family& _cf;
    query::mapreduce_request& _req;
    query::mapreduce_result& _result;
    tracing::trace_state_ptr _tr_state;
    retrying_dispatcher _dispatcher;
    size_t _limit_per_replica;

    struct partition_range_cmp {
        bool operator() (const dht::partition_range& a, const dht::partition_range& b) const {
            return end_token(a) < end_token(b);
        };
    };

    std::set<dht::partition_range, partition_range_cmp> _ranges_left;
    ranges_per_tablet_replica_t _ranges_per_replica;
};

future<> mapreduce_service::dispatch_to_tablets(schema_ptr schema, replica::column_family& cf, query::mapreduce_request& req, query::mapreduce_result& result, tracing::trace_state_ptr tr_state) {
    mapreduce_tablet_algorithm algorithm(*this, schema, cf, req, result, tr_state);
    co_await algorithm.initialize_ranges_left();
    co_await algorithm.dispatch_work_and_wait_to_finish();
}

future<query::mapreduce_result> mapreduce_service::dispatch(query::mapreduce_request req, tracing::trace_state_ptr tr_state) {
    schema_ptr schema = local_schema_registry().get(req.cmd.schema_version);
    replica::table& cf = _db.local().find_column_family(schema);
    
    query::mapreduce_result result;
    if (cf.uses_tablets()) {
        co_await dispatch_to_tablets(schema, cf, req, result, tr_state);
    } else {
        co_await dispatch_to_vnodes(schema, cf, req, result, tr_state);
    }

    mapreduce_aggregates aggrs(req);
    const bool requires_thread = aggrs.requires_thread();

    auto merge_result = [&result, &req, &tr_state, aggrs = std::move(aggrs)] () mutable {
        auto printer = seastar::value_of([&req, &result] {
            return query::mapreduce_result::printer {
                .functions = get_functions(req),
                .res = result
            };
        });
        tracing::trace(tr_state, "Merged result is {}", printer);
        flogger.debug("merged result is {}", printer);

        aggrs.finalize(result);
        return result;
    };
    if (requires_thread) {
        co_return co_await seastar::async(std::move(merge_result));
    } else {
        co_return merge_result();
    }
}

void mapreduce_service::register_metrics() {
    namespace sm = seastar::metrics;
    _metrics.add_group("mapreduce_service", {
        sm::make_total_operations("requests_dispatched_to_other_nodes", _stats.requests_dispatched_to_other_nodes,
             sm::description("how many mapreduce requests were dispatched to other nodes"), {}),
        sm::make_total_operations("requests_dispatched_to_own_shards", _stats.requests_dispatched_to_own_shards,
             sm::description("how many mapreduce requests were dispatched to local shards"), {}),
        sm::make_total_operations("requests_executed", _stats.requests_executed,
             sm::description("how many mapreduce requests were executed"), {}),
    });
}

} // namespace service
