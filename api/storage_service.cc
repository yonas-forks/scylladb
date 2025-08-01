/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
 */

#include "storage_service.hh"
#include "api/api.hh"
#include "api/api-doc/column_family.json.hh"
#include "api/api-doc/storage_service.json.hh"
#include "api/api-doc/storage_proxy.json.hh"
#include "api/scrub_status.hh"
#include "db/config.hh"
#include "db/schema_tables.hh"
#include "gms/feature_service.hh"
#include "schema/schema_builder.hh"
#include "sstables/sstables_manager.hh"
#include "utils/hash.hh"
#include <optional>
#include <sstream>
#include <time.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <chrono>
#include <boost/algorithm/string/trim_all.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/functional/hash.hpp>
#include <fmt/ranges.h>
#include "service/raft/raft_group0_client.hh"
#include "service/storage_service.hh"
#include "service/load_meter.hh"
#include "gms/feature_service.hh"
#include "gms/gossiper.hh"
#include "db/system_keyspace.hh"
#include <seastar/http/exception.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/coroutine/exception.hh>
#include "repair/row_level.hh"
#include "locator/snitch_base.hh"
#include "locator/tablets.hh"
#include "column_family.hh"
#include "utils/log.hh"
#include "release.hh"
#include "compaction/compaction_manager.hh"
#include "compaction/task_manager_module.hh"
#include "sstables/sstables.hh"
#include "replica/database.hh"
#include "db/extensions.hh"
#include "db/snapshot-ctl.hh"
#include "transport/controller.hh"
#include "locator/token_metadata.hh"
#include "cdc/generation_service.hh"
#include "locator/abstract_replication_strategy.hh"
#include "sstables_loader.hh"
#include "db/view/view_builder.hh"
#include "utils/rjson.hh"
#include "utils/user_provided_param.hh"
#include "sstable_dict_autotrainer.hh"

using namespace seastar::httpd;
using namespace std::chrono_literals;

extern logging::logger apilog;

namespace api {

namespace ss = httpd::storage_service_json;
namespace sp = httpd::storage_proxy_json;
namespace cf = httpd::column_family_json;
using namespace json;

sstring validate_keyspace(const http_context& ctx, sstring ks_name) {
    if (ctx.db.local().has_keyspace(ks_name)) {
        return ks_name;
    }
    throw bad_param_exception(replica::no_such_keyspace(ks_name).what());
}

sstring validate_keyspace(const http_context& ctx, const std::unique_ptr<http::request>& req) {
    return validate_keyspace(ctx, req->get_path_param("keyspace"));
}

sstring validate_keyspace(const http_context& ctx, const http::request& req) {
    return validate_keyspace(ctx, req.get_path_param("keyspace"));
}

table_id validate_table(const replica::database& db, sstring ks_name, sstring table_name) {
    try {
        return db.find_uuid(ks_name, table_name);
    } catch (replica::no_such_column_family& e) {
        throw bad_param_exception(e.what());
    }
}

static void ensure_tablets_disabled(const http_context& ctx, const sstring& ks_name, const sstring& api_endpoint_path) {
    if (ctx.db.local().find_keyspace(ks_name).uses_tablets()) {
        throw bad_param_exception{fmt::format("{} is per-table in keyspace '{}'. Please provide table name using 'cf' parameter.", api_endpoint_path, ks_name)};
    }
}

static bool any_of_keyspaces_use_tablets(const http_context& ctx) {
    auto& db = ctx.db.local();
    auto uses_tablets = [&db](const auto& ks_name) {
        return db.find_keyspace(ks_name).uses_tablets();
    };

    auto keyspaces = db.get_all_keyspaces();
    return std::any_of(std::begin(keyspaces), std::end(keyspaces), uses_tablets);
}

locator::host_id validate_host_id(const sstring& param) {
    auto hoep = locator::host_id_or_endpoint(param, locator::host_id_or_endpoint::param_type::host_id);
    return hoep.id();
}

bool validate_bool(const sstring& param) {
    if (param == "true") {
        return true;
    } else if (param == "false") {
        return false;
    } else {
        throw std::runtime_error("Parameter must be either 'true' or 'false'");
    }
}

bool validate_bool_x(const sstring& param, bool default_value) {
    if (param.empty()) {
        return default_value;
    }

    if (strcasecmp(param.c_str(), "true") == 0 || strcasecmp(param.c_str(), "yes") == 0 || param == "1") {
        return true;
    }
    if (strcasecmp(param.c_str(), "false") == 0 || strcasecmp(param.c_str(), "no") == 0 || param == "0") {
        return false;
    }

    throw std::runtime_error("Invalid boolean parameter value");
}

static
int64_t validate_int(const sstring& param) {
    return std::atoll(param.c_str());
}

std::vector<table_info> parse_table_infos(const sstring& ks_name, const http_context& ctx, sstring value) {
    std::vector<table_info> res;
    try {
        if (value.empty()) {
            const auto& cf_meta_data = ctx.db.local().find_keyspace(ks_name).metadata().get()->cf_meta_data();
            res.reserve(cf_meta_data.size());
            for (const auto& [name, schema] : cf_meta_data) {
                res.emplace_back(table_info{name, schema->id()});
            }
        } else {
            std::vector<sstring> names = split(value, ",");
            res.reserve(names.size());
            const auto& db = ctx.db.local();
            for (const auto& table_name : names) {
                res.emplace_back(table_info{table_name, db.find_uuid(ks_name, table_name)});
            }
        }
    } catch (const replica::no_such_keyspace& e) {
        throw bad_param_exception(e.what());
    } catch (const replica::no_such_column_family& e) {
        throw bad_param_exception(e.what());
    }
    return res;
}

std::pair<sstring, std::vector<table_info>> parse_table_infos(const http_context& ctx, const http::request& req, sstring cf_param_name) {
    auto keyspace = validate_keyspace(ctx, req);
    const auto& query_params = req.query_parameters;
    auto it = query_params.find(cf_param_name);
    auto tis = parse_table_infos(keyspace, ctx, it != query_params.end() ? it->second : "");
    return std::make_pair(std::move(keyspace), std::move(tis));
}

static ss::token_range token_range_endpoints_to_json(const dht::token_range_endpoints& d) {
    ss::token_range r;
    r.start_token = d._start_token;
    r.end_token = d._end_token;
    r.endpoints = d._endpoints;
    r.rpc_endpoints = d._rpc_endpoints;
    for (auto det : d._endpoint_details) {
        ss::endpoint_detail ed;
        ed.host = fmt::to_string(det._host);
        ed.datacenter = det._datacenter;
        if (det._rack != "") {
            ed.rack = det._rack;
        }
        r.endpoint_details.push(ed);
    }
    return r;
}

seastar::future<json::json_return_type> run_toppartitions_query(db::toppartitions_query& q, http_context &ctx, bool legacy_request) {
    return q.scatter().then([&q, legacy_request] {
        return sleep(q.duration()).then([&q, legacy_request] {
            return q.gather(q.capacity()).then([&q, legacy_request] (auto topk_results) {
                apilog.debug("toppartitions query: processing results");
                cf::toppartitions_query_results results;

                results.read_cardinality = topk_results.read.size();
                results.write_cardinality = topk_results.write.size();

                for (auto& d: topk_results.read.top(q.list_size())) {
                    cf::toppartitions_record r;
                    r.partition = (legacy_request ? "" : "(" + d.item.schema->ks_name() + ":" + d.item.schema->cf_name() + ") ") + sstring(d.item);
                    r.count = d.count;
                    r.error = d.error;
                    results.read.push(r);
                }
                for (auto& d: topk_results.write.top(q.list_size())) {
                    cf::toppartitions_record r;
                    r.partition = (legacy_request ? "" : "(" + d.item.schema->ks_name() + ":" + d.item.schema->cf_name() + ") ") + sstring(d.item);
                    r.count = d.count;
                    r.error = d.error;
                    results.write.push(r);
                }
                return make_ready_future<json::json_return_type>(results);
            });
        });
    });
}

future<scrub_info> parse_scrub_options(const http_context& ctx, sharded<db::snapshot_ctl>& snap_ctl, std::unique_ptr<http::request> req) {
    scrub_info info;
    auto [ keyspace, table_infos ] = parse_table_infos(ctx, *req, "cf");
    info.keyspace = std::move(keyspace);
    info.column_families = table_infos | std::views::transform(&table_info::name) | std::ranges::to<std::vector>();
    auto scrub_mode_str = req->get_query_param("scrub_mode");
    auto scrub_mode = sstables::compaction_type_options::scrub::mode::abort;

    if (scrub_mode_str.empty()) {
        const auto skip_corrupted = validate_bool_x(req->get_query_param("skip_corrupted"), false);

        if (skip_corrupted) {
            scrub_mode = sstables::compaction_type_options::scrub::mode::skip;
        }
    } else {
        if (scrub_mode_str == "ABORT") {
            scrub_mode = sstables::compaction_type_options::scrub::mode::abort;
        } else if (scrub_mode_str == "SKIP") {
            scrub_mode = sstables::compaction_type_options::scrub::mode::skip;
        } else if (scrub_mode_str == "SEGREGATE") {
            scrub_mode = sstables::compaction_type_options::scrub::mode::segregate;
        } else if (scrub_mode_str == "VALIDATE") {
            scrub_mode = sstables::compaction_type_options::scrub::mode::validate;
        } else {
            throw httpd::bad_param_exception(fmt::format("Unknown argument for 'scrub_mode' parameter: {}", scrub_mode_str));
        }
    }

    if (!req_param<bool>(*req, "disable_snapshot", false) && !info.column_families.empty()) {
        auto tag = format("pre-scrub-{:d}", db_clock::now().time_since_epoch().count());
        co_await snap_ctl.local().take_column_family_snapshot(info.keyspace, info.column_families, tag, db::snapshot_ctl::skip_flush::no);
    }

    info.opts = {
        .operation_mode = scrub_mode,
    };
    const sstring quarantine_mode_str = req_param<sstring>(*req, "quarantine_mode", "INCLUDE");
    if (quarantine_mode_str == "INCLUDE") {
        info.opts.quarantine_operation_mode = sstables::compaction_type_options::scrub::quarantine_mode::include;
    } else if (quarantine_mode_str == "EXCLUDE") {
        info.opts.quarantine_operation_mode = sstables::compaction_type_options::scrub::quarantine_mode::exclude;
    } else if (quarantine_mode_str == "ONLY") {
        info.opts.quarantine_operation_mode = sstables::compaction_type_options::scrub::quarantine_mode::only;
    } else {
        throw httpd::bad_param_exception(fmt::format("Unknown argument for 'quarantine_mode' parameter: {}", quarantine_mode_str));
    }

    co_return info;
}

void set_transport_controller(http_context& ctx, routes& r, cql_transport::controller& ctl) {
    ss::start_native_transport.set(r, [&ctl](std::unique_ptr<http::request> req) {
        return smp::submit_to(0, [&] {
            return ctl.start_server();
        }).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::stop_native_transport.set(r, [&ctl](std::unique_ptr<http::request> req) {
        return smp::submit_to(0, [&] {
            return ctl.request_stop_server();
        }).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::is_native_transport_running.set(r, [&ctl] (std::unique_ptr<http::request> req) {
        return smp::submit_to(0, [&] {
            return !ctl.listen_addresses().empty();
        }).then([] (bool running) {
            return make_ready_future<json::json_return_type>(running);
        });
    });
}

void unset_transport_controller(http_context& ctx, routes& r) {
    ss::start_native_transport.unset(r);
    ss::stop_native_transport.unset(r);
    ss::is_native_transport_running.unset(r);
}

// NOTE: preserved only for backward compatibility
void set_thrift_controller(http_context& ctx, routes& r) {
    ss::is_thrift_server_running.set(r, [] (std::unique_ptr<http::request> req) {
        return smp::submit_to(0, [] {
            return make_ready_future<json::json_return_type>(false);
        });
    });
}

void unset_thrift_controller(http_context& ctx, routes& r) {
    ss::is_thrift_server_running.unset(r);
}

void set_repair(http_context& ctx, routes& r, sharded<repair_service>& repair, sharded<gms::gossip_address_map>& am) {
    ss::repair_async.set(r, [&ctx, &repair, &am](std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        static std::unordered_set<sstring> options = {"primaryRange", "parallelism", "incremental",
                "jobThreads", "ranges", "columnFamilies", "dataCenters", "hosts", "ignore_nodes", "trace",
                "startToken", "endToken", "ranges_parallelism", "small_table_optimization"};

        // Nodetool still sends those unsupported options. Ignore them to avoid failing nodetool repair.
        static std::unordered_set<sstring> legacy_options_to_ignore = {"pullRepair", "ignoreUnreplicatedKeyspaces"};

        for (auto& x : req->query_parameters) {
            if (legacy_options_to_ignore.contains(x.first)) {
                continue;
            }
            if (!options.contains(x.first)) {
                throw httpd::bad_param_exception(format("option {} is not supported", x.first));
            }
        }
        std::unordered_map<sstring, sstring> options_map;
        for (auto o : options) {
            auto s = req->get_query_param(o);
            if (s != "") {
                options_map[o] = s;
            }
        }

        // The repair process is asynchronous: repair_start only starts it and
        // returns immediately, not waiting for the repair to finish. The user
        // then has other mechanisms to track the ongoing repair's progress,
        // or stop it.
        try {
            int res = co_await repair_start(repair, am, validate_keyspace(ctx, req), options_map);
            co_return json::json_return_type(res);
        } catch (const std::invalid_argument& e) {
            // if the option is not sane, repair_start() throws immediately, so
            // convert the exception to an HTTP error
            throw httpd::bad_param_exception(e.what());
        } catch (const tablets_unsupported& e) {
            throw base_exception("Cannot repair tablet keyspace. Use /storage_service/tablets/repair to repair tablet keyspaces.",
                    http::reply::status_type::forbidden);
        }
    });

    ss::get_active_repair_async.set(r, [&repair] (std::unique_ptr<http::request> req) {
        return repair.local().get_active_repairs().then([] (std::vector<int> res) {
            return make_ready_future<json::json_return_type>(res);
        });
    });

    ss::repair_async_status.set(r, [&repair] (std::unique_ptr<http::request> req) {
        return repair.local().get_status(boost::lexical_cast<int>( req->get_query_param("id")))
                .then_wrapped([] (future<repair_status>&& fut) {
            ss::ns_repair_async_status::return_type_wrapper res;
            try {
                res = fut.get();
            } catch(std::runtime_error& e) {
                throw httpd::bad_param_exception(e.what());
            }
            return make_ready_future<json::json_return_type>(json::json_return_type(res));
        });
    });

    ss::repair_await_completion.set(r, [&repair] (std::unique_ptr<http::request> req) {
        int id;
        using clock = std::chrono::steady_clock;
        clock::time_point expire;
        try {
            id = boost::lexical_cast<int>(req->get_query_param("id"));
            // If timeout is not provided, it means no timeout.
            sstring s = req->get_query_param("timeout");
            int64_t timeout = s.empty() ? int64_t(-1) : boost::lexical_cast<int64_t>(s);
            if (timeout < 0 && timeout != -1) {
                return make_exception_future<json::json_return_type>(
                        httpd::bad_param_exception("timeout can only be -1 (means no timeout) or non negative integer"));
            }
            if (timeout < 0) {
                expire = clock::time_point::max();
            } else {
                expire = clock::now() + std::chrono::seconds(timeout);
            }
        } catch (std::exception& e) {
            return make_exception_future<json::json_return_type>(httpd::bad_param_exception(e.what()));
        }
        return repair.local().await_completion(id, expire)
                .then_wrapped([] (future<repair_status>&& fut) {
            ss::ns_repair_async_status::return_type_wrapper res;
            try {
                res = fut.get();
            } catch (std::exception& e) {
                return make_exception_future<json::json_return_type>(httpd::bad_param_exception(e.what()));
            }
            return make_ready_future<json::json_return_type>(json::json_return_type(res));
        });
    });

    ss::force_terminate_all_repair_sessions.set(r, [&repair] (std::unique_ptr<http::request> req) {
        return repair.local().abort_all().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::force_terminate_all_repair_sessions_new.set(r, [&repair] (std::unique_ptr<http::request> req) {
        return repair.local().abort_all().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

}

void unset_repair(http_context& ctx, routes& r) {
    ss::repair_async.unset(r);
    ss::get_active_repair_async.unset(r);
    ss::repair_async_status.unset(r);
    ss::repair_await_completion.unset(r);
    ss::force_terminate_all_repair_sessions.unset(r);
    ss::force_terminate_all_repair_sessions_new.unset(r);
}

static sstables_loader::stream_scope parse_stream_scope(const sstring& scope_str) {
    using namespace ss::ns_start_restore;
    auto sc = scope_str.empty() ? scope::all : str2scope(scope_str);

    switch (sc) {
    case scope::all: return sstables_loader::stream_scope::all;
    case scope::dc: return sstables_loader::stream_scope::dc;
    case scope::rack: return sstables_loader::stream_scope::rack;
    case scope::node: return sstables_loader::stream_scope::node;
    case scope::NUM_ITEMS:
        break;
    }

    throw httpd::bad_param_exception("invalid scope parameter value");
}

void set_sstables_loader(http_context& ctx, routes& r, sharded<sstables_loader>& sst_loader) {
    ss::load_new_ss_tables.set(r, [&ctx, &sst_loader](std::unique_ptr<http::request> req) {
        auto ks = validate_keyspace(ctx, req);
        auto cf = req->get_query_param("cf");
        auto stream = req->get_query_param("load_and_stream");
        auto primary_replica = req->get_query_param("primary_replica_only");
        auto skip_cleanup_p = req->get_query_param("skip_cleanup");
        boost::algorithm::to_lower(stream);
        boost::algorithm::to_lower(primary_replica);
        bool load_and_stream = stream == "true" || stream == "1";
        bool primary_replica_only = primary_replica == "true" || primary_replica == "1";
        bool skip_cleanup = skip_cleanup_p == "true" || skip_cleanup_p == "1";
        auto scope = parse_stream_scope(req->get_query_param("scope"));
        auto skip_reshape_p = req->get_query_param("skip_reshape");
        auto skip_reshape = skip_reshape_p == "true" || skip_reshape_p == "1";

        if (scope != sstables_loader::stream_scope::all && !load_and_stream) {
            throw httpd::bad_param_exception("scope takes no effect without load-and-stream");
        }

        // No need to add the keyspace, since all we want is to avoid always sending this to the same
        // CPU. Even then I am being overzealous here. This is not something that happens all the time.
        auto coordinator = std::hash<sstring>()(cf) % smp::count;
        return sst_loader.invoke_on(coordinator,
                [ks = std::move(ks), cf = std::move(cf),
                load_and_stream, primary_replica_only, skip_cleanup, skip_reshape, scope] (sstables_loader& loader) {
            return loader.load_new_sstables(ks, cf, load_and_stream, primary_replica_only, skip_cleanup, skip_reshape, scope);
        }).then_wrapped([] (auto&& f) {
            if (f.failed()) {
                auto msg = fmt::format("Failed to load new sstables: {}", f.get_exception());
                return make_exception_future<json::json_return_type>(httpd::server_error_exception(msg));
            }
            return make_ready_future<json::json_return_type>(json_void());
        });
    });

    ss::start_restore.set(r, [&sst_loader] (std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        auto endpoint = req->get_query_param("endpoint");
        auto keyspace = req->get_query_param("keyspace");
        auto table = req->get_query_param("table");
        auto bucket = req->get_query_param("bucket");
        auto prefix = req->get_query_param("prefix");
        auto scope = parse_stream_scope(req->get_query_param("scope"));

        // TODO: the http_server backing the API does not use content streaming
        // should use it for better performance
        rjson::value parsed = rjson::parse(req->content);
        if (!parsed.IsArray()) {
            throw httpd::bad_param_exception("malformatted sstables in body");
        }
        auto sstables = parsed.GetArray() |
            std::views::transform([] (const auto& s) { return sstring(rjson::to_string_view(s)); }) |
            std::ranges::to<std::vector>();
        auto task_id = co_await sst_loader.local().download_new_sstables(keyspace, table, prefix, std::move(sstables), endpoint, bucket, scope);
        co_return json::json_return_type(fmt::to_string(task_id));
    });

}

void unset_sstables_loader(http_context& ctx, routes& r) {
    ss::load_new_ss_tables.unset(r);
    ss::start_restore.unset(r);
}

void set_view_builder(http_context& ctx, routes& r, sharded<db::view::view_builder>& vb, sharded<gms::gossiper>& g) {
    ss::view_build_statuses.set(r, [&ctx, &vb, &g] (std::unique_ptr<http::request> req) {
        auto keyspace = validate_keyspace(ctx, req);
        auto view = req->get_path_param("view");
        return vb.local().view_build_statuses(std::move(keyspace), std::move(view), g.local()).then([] (std::unordered_map<sstring, sstring> status) {
            std::vector<storage_service_json::mapper> res;
            return make_ready_future<json::json_return_type>(map_to_key_value(std::move(status), res));
        });
    });

}

void unset_view_builder(http_context& ctx, routes& r) {
    ss::view_build_statuses.unset(r);
}

static future<json::json_return_type> describe_ring_as_json(sharded<service::storage_service>& ss, sstring keyspace) {
    co_return json::json_return_type(stream_range_as_array(co_await ss.local().describe_ring(keyspace), token_range_endpoints_to_json));
}

static future<json::json_return_type> describe_ring_as_json_for_table(const sharded<service::storage_service>& ss, sstring keyspace, sstring table) {
    co_return json::json_return_type(stream_range_as_array(co_await ss.local().describe_ring_for_table(keyspace, table), token_range_endpoints_to_json));
}

static
future<json::json_return_type>
rest_get_token_endpoint(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        const auto keyspace_name = req->get_query_param("keyspace");
        const auto table_name = req->get_query_param("cf");

        std::map<dht::token, gms::inet_address> token_endpoints;
        if (keyspace_name.empty() && table_name.empty()) {
            token_endpoints = ss.local().get_token_to_endpoint_map();
        } else if (!keyspace_name.empty() && !table_name.empty()) {
            auto& db = ctx.db.local();
            if (!db.has_schema(keyspace_name, table_name)) {
                throw bad_param_exception(fmt::format("Failed to find table {}.{}", keyspace_name, table_name));
            }
            token_endpoints = co_await ss.local().get_tablet_to_endpoint_map(db.find_schema(keyspace_name, table_name)->id());
        } else {
            throw bad_param_exception("Either provide both keyspace and table (for tablet table) or neither (for vnodes)");
        }

        co_return json::json_return_type(stream_range_as_array(token_endpoints, [](const auto& i) {
            storage_service_json::mapper val;
            val.key = fmt::to_string(i.first);
            val.value = fmt::to_string(i.second);
            return val;
        }));
}

static
future<json::json_return_type>
rest_toppartitions_generic(http_context& ctx, std::unique_ptr<http::request> req) {
        bool filters_provided = false;

        std::unordered_set<std::tuple<sstring, sstring>, utils::tuple_hash> table_filters {};
        if (req->query_parameters.contains("table_filters")) {
            filters_provided = true;
            auto filters = req->get_query_param("table_filters");
            std::stringstream ss { filters };
            std::string filter;
            while (!filters.empty() && ss.good()) {
                std::getline(ss, filter, ',');
                table_filters.emplace(parse_fully_qualified_cf_name(filter));
            }
        }

        std::unordered_set<sstring> keyspace_filters {};
        if (req->query_parameters.contains("keyspace_filters")) {
            filters_provided = true;
            auto filters = req->get_query_param("keyspace_filters");
            std::stringstream ss { filters };
            std::string filter;
            while (!filters.empty() && ss.good()) {
                std::getline(ss, filter, ',');
                keyspace_filters.emplace(std::move(filter));
            }
        }

        // when the query is empty return immediately
        if (filters_provided && table_filters.empty() && keyspace_filters.empty()) {
            apilog.debug("toppartitions query: processing results");
            httpd::column_family_json::toppartitions_query_results results;

            results.read_cardinality = 0;
            results.write_cardinality = 0;

            return make_ready_future<json::json_return_type>(results);
        }

        api::req_param<std::chrono::milliseconds, unsigned> duration{*req, "duration", 1000ms};
        api::req_param<unsigned> capacity(*req, "capacity", 256);
        api::req_param<unsigned> list_size(*req, "list_size", 10);

        apilog.info("toppartitions query: #table_filters={} #keyspace_filters={} duration={} list_size={} capacity={}",
            !table_filters.empty() ? std::to_string(table_filters.size()) : "all", !keyspace_filters.empty() ? std::to_string(keyspace_filters.size()) : "all", duration.value, list_size.value, capacity.value);

        return seastar::do_with(db::toppartitions_query(ctx.db, std::move(table_filters), std::move(keyspace_filters), duration.value, list_size, capacity), [&ctx] (db::toppartitions_query& q) {
            return run_toppartitions_query(q, ctx);
        });
}

static
json::json_return_type
rest_get_release_version(sharded<service::storage_service>& ss, const_req& req) {
        return ss.local().get_release_version();
}

static
json::json_return_type
rest_get_scylla_release_version(sharded<service::storage_service>& ss, const_req& req) {
        return scylla_version();
}

static
json::json_return_type
rest_get_schema_version(sharded<service::storage_service>& ss, const_req& req) {
        return ss.local().get_schema_version();
}

static
future<json::json_return_type>
rest_get_range_to_endpoint_map(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto keyspace = validate_keyspace(ctx, req);
        auto table = req->get_query_param("cf");

        auto erm = std::invoke([&]() -> locator::effective_replication_map_ptr {
            auto& ks = ctx.db.local().find_keyspace(keyspace);
            if (table.empty()) {
                ensure_tablets_disabled(ctx, keyspace, "storage_service/range_to_endpoint_map");
                return ks.get_vnode_effective_replication_map();
            } else {
                auto table_id = validate_table(ctx.db.local(), keyspace, table);
                auto& cf = ctx.db.local().find_column_family(table_id);
                return cf.get_effective_replication_map();
            }
        });

        std::vector<ss::maplist_mapper> res;
        co_return stream_range_as_array(co_await ss.local().get_range_to_address_map(erm),
                [](const std::pair<dht::token_range, inet_address_vector_replica_set>& entry){
            ss::maplist_mapper m;
            if (entry.first.start()) {
                m.key.push(entry.first.start().value().value().to_sstring());
            } else {
                m.key.push("");
            }
            if (entry.first.end()) {
                m.key.push(entry.first.end().value().value().to_sstring());
            } else {
                m.key.push("");
            }
            for (const gms::inet_address& address : entry.second) {
                m.value.push(fmt::to_string(address));
            }
            return m;
        });
}

static
future<json::json_return_type>
rest_get_pending_range_to_endpoint_map(http_context& ctx, std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto keyspace = validate_keyspace(ctx, req);
        std::vector<ss::maplist_mapper> res;
        return make_ready_future<json::json_return_type>(res);
}

static
future<json::json_return_type>
rest_describe_ring(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        if (!req->param.exists("keyspace")) {
            throw bad_param_exception("The keyspace param is not provided");
        }
        auto keyspace = req->get_path_param("keyspace");
        auto table = req->get_query_param("table");
        if (!table.empty()) {
            validate_table(ctx.db.local(), keyspace, table);
            return describe_ring_as_json_for_table(ss, keyspace, table);
        }
        return describe_ring_as_json(ss, validate_keyspace(ctx, req));
}

static
future<json::json_return_type>
rest_get_load(http_context& ctx, std::unique_ptr<http::request> req) {
        return get_cf_stats(ctx, &replica::column_family_stats::live_disk_space_used);
}

static
future<json::json_return_type>
rest_get_current_generation_number(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto ep = ss.local().get_token_metadata().get_topology().my_host_id();
        return ss.local().gossiper().get_current_generation_number(ep).then([](gms::generation_type res) {
            return make_ready_future<json::json_return_type>(res.value());
        });
}

static
json::json_return_type
rest_get_natural_endpoints(http_context& ctx, sharded<service::storage_service>& ss, const_req req) {
        auto keyspace = validate_keyspace(ctx, req);
        auto res = ss.local().get_natural_endpoints(keyspace, req.get_query_param("cf"), req.get_query_param("key"));
        return res | std::views::transform([] (auto& ep) { return fmt::to_string(ep); }) | std::ranges::to<std::vector>();
}

static
future<json::json_return_type>
rest_cdc_streams_check_and_repair(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.invoke_on(0, [] (service::storage_service& ss) {
            return ss.check_and_repair_cdc_streams();
        }).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_force_compaction(http_context& ctx, std::unique_ptr<http::request> req) {
        auto& db = ctx.db;
        auto flush = validate_bool_x(req->get_query_param("flush_memtables"), true);
        auto consider_only_existing_data = validate_bool_x(req->get_query_param("consider_only_existing_data"), false);
        apilog.info("force_compaction: flush={} consider_only_existing_data={}", flush, consider_only_existing_data);

        auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
        std::optional<flush_mode> fmopt;
        if (!flush && !consider_only_existing_data) {
            fmopt = flush_mode::skip;
        }
        auto task = co_await compaction_module.make_and_start_task<global_major_compaction_task_impl>({}, db, fmopt, consider_only_existing_data);
        co_await task->done();
        co_return json_void();
}

static
future<json::json_return_type>
rest_force_keyspace_compaction(http_context& ctx, std::unique_ptr<http::request> req) {
        auto& db = ctx.db;
        auto [ keyspace, table_infos ] = parse_table_infos(ctx, *req, "cf");
        auto flush = validate_bool_x(req->get_query_param("flush_memtables"), true);
        auto consider_only_existing_data = validate_bool_x(req->get_query_param("consider_only_existing_data"), false);
        apilog.info("force_keyspace_compaction: keyspace={} tables={}, flush={} consider_only_existing_data={}", keyspace, table_infos, flush, consider_only_existing_data);

        auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
        std::optional<flush_mode> fmopt;
        if (!flush && !consider_only_existing_data) {
            fmopt = flush_mode::skip;
        }
        auto task = co_await compaction_module.make_and_start_task<major_keyspace_compaction_task_impl>({}, std::move(keyspace), tasks::task_id::create_null_id(), db, table_infos, fmopt, consider_only_existing_data);
        co_await task->done();
        co_return json_void();
}

static
future<json::json_return_type>
rest_force_keyspace_cleanup(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto& db = ctx.db;
        auto [keyspace, table_infos] = parse_table_infos(ctx, *req);
        const auto& rs = db.local().find_keyspace(keyspace).get_replication_strategy();
        if (rs.get_type() == locator::replication_strategy_type::local || !rs.is_vnode_based()) {
            auto reason = rs.get_type() == locator::replication_strategy_type::local ? "require" : "support";
            apilog.info("Keyspace {} does not {} cleanup", keyspace, reason);
            co_return json::json_return_type(0);
        }
        apilog.info("force_keyspace_cleanup: keyspace={} tables={}", keyspace, table_infos);
        if (!co_await ss.local().is_cleanup_allowed(keyspace)) {
            auto msg = "Can not perform cleanup operation when topology changes";
            apilog.warn("force_keyspace_cleanup: keyspace={} tables={}: {}", keyspace, table_infos, msg);
            co_await coroutine::return_exception(std::runtime_error(msg));
        }

        auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<cleanup_keyspace_compaction_task_impl>(
            {}, std::move(keyspace), db, table_infos, flush_mode::all_tables, tasks::is_user_task::yes);
        co_await task->done();
        co_return json::json_return_type(0);
}

static
future<json::json_return_type>
rest_cleanup_all(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        apilog.info("cleanup_all");
        auto done = co_await ss.invoke_on(0, [] (service::storage_service& ss) -> future<bool> {
            if (!ss.is_topology_coordinator_enabled()) {
                co_return false;
            }
            co_await ss.do_cluster_cleanup();
            co_return true;
        });
        if (done) {
            co_return json::json_return_type(0);
        }
        // fall back to the local global cleanup if topology coordinator is not enabled
        auto& db = ctx.db;
        auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<global_cleanup_compaction_task_impl>({}, db);
        co_await task->done();
        co_return json::json_return_type(0);
}

static
future<json::json_return_type>
rest_perform_keyspace_offstrategy_compaction(http_context& ctx, std::unique_ptr<http::request> req) {
        auto [keyspace, table_infos] = parse_table_infos(ctx, *req);
        apilog.info("perform_keyspace_offstrategy_compaction: keyspace={} tables={}", keyspace, table_infos);
        bool res = false;
        auto& compaction_module = ctx.db.local().get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<offstrategy_keyspace_compaction_task_impl>({}, std::move(keyspace), ctx.db, table_infos, &res);
        co_await task->done();
        co_return json::json_return_type(res);
}

static
future<json::json_return_type>
rest_upgrade_sstables(http_context& ctx, std::unique_ptr<http::request> req) {
        auto& db = ctx.db;
        auto [keyspace, table_infos] = parse_table_infos(ctx, *req);
        bool exclude_current_version = req_param<bool>(*req, "exclude_current_version", false);

        apilog.info("upgrade_sstables: keyspace={} tables={} exclude_current_version={}", keyspace, table_infos, exclude_current_version);

        auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<upgrade_sstables_compaction_task_impl>({}, std::move(keyspace), db, table_infos, exclude_current_version);
        co_await task->done();
        co_return json::json_return_type(0);
}

static
future<json::json_return_type>
rest_force_flush(http_context& ctx, std::unique_ptr<http::request> req) {
        apilog.info("flush all tables");
        co_await ctx.db.invoke_on_all([] (replica::database& db) {
            return db.flush_all_tables();
        });
        co_return json_void();
}

static
future<json::json_return_type>
rest_force_keyspace_flush(http_context& ctx, std::unique_ptr<http::request> req) {
        auto [keyspace, table_infos] = parse_table_infos(ctx, *req);
        apilog.info("perform_keyspace_flush: keyspace={} tables={}", keyspace, table_infos);
        auto& db = ctx.db;
        co_await replica::database::flush_tables_on_all_shards(db, std::move(table_infos));
        co_return json_void();
}

static
future<json::json_return_type>
rest_decommission(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        apilog.info("decommission");
        return ss.local().decommission().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_move(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto new_token = req->get_query_param("new_token");
        return ss.local().move(new_token).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_remove_node(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto host_id = validate_host_id(req->get_query_param("host_id"));
        std::vector<sstring> ignore_nodes_strs = utils::split_comma_separated_list(req->get_query_param("ignore_nodes"));
        apilog.info("remove_node: host_id={} ignore_nodes={}", host_id, ignore_nodes_strs);
        locator::host_id_or_endpoint_list ignore_nodes;
        ignore_nodes.reserve(ignore_nodes_strs.size());
        for (const sstring& n : ignore_nodes_strs) {
            try {
                auto hoep = locator::host_id_or_endpoint(n);
                if (!ignore_nodes.empty() && hoep.has_host_id() != ignore_nodes.front().has_host_id()) {
                    throw std::runtime_error("All nodes should be identified using the same method: either Host IDs or ip addresses.");
                }
                ignore_nodes.push_back(std::move(hoep));
            } catch (...) {
                throw std::runtime_error(fmt::format("Failed to parse ignore_nodes parameter: ignore_nodes={}, node={}: {}", ignore_nodes_strs, n, std::current_exception()));
            }
        }
        return ss.local().removenode(host_id, std::move(ignore_nodes)).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_get_removal_status(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().get_removal_status().then([] (auto status) {
            return make_ready_future<json::json_return_type>(status);
        });
}

static
future<json::json_return_type>
rest_force_remove_completion(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().force_remove_completion().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_set_logging_level(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto class_qualifier = req->get_query_param("class_qualifier");
        auto level = req->get_query_param("level");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_get_logging_levels(std::unique_ptr<http::request> req) {
        std::vector<ss::mapper> res;
        for (auto i : logging::logger_registry().get_all_logger_names()) {
            ss::mapper log;
            log.key = i;
            log.value = logging::level_name(logging::logger_registry().get_logger_level(i));
            res.push_back(log);
        }
        return make_ready_future<json::json_return_type>(res);
}

static
future<json::json_return_type>
rest_get_operation_mode(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().get_operation_mode().then([] (auto mode) {
            return make_ready_future<json::json_return_type>(format("{}", mode));
        });
}

static
future<json::json_return_type>
rest_is_starting(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().get_operation_mode().then([] (auto mode) {
            return make_ready_future<json::json_return_type>(mode <= service::storage_service::mode::STARTING);
        });
}

static
future<json::json_return_type>
rest_get_drain_progress(http_context& ctx, std::unique_ptr<http::request> req) {
        return ctx.db.map_reduce(adder<replica::database::drain_progress>(), [] (auto& db) {
            return db.get_drain_progress();
        }).then([] (auto&& progress) {
            auto progress_str = format("Drained {}/{} ColumnFamilies", progress.remaining_cfs, progress.total_cfs);
            return make_ready_future<json::json_return_type>(std::move(progress_str));
        });
}

static
future<json::json_return_type>
rest_drain(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        apilog.info("drain");
        return ss.local().drain().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
json::json_return_type
rest_get_keyspaces(http_context& ctx, const_req req) {
        auto type = req.get_query_param("type");
        auto replication = req.get_query_param("replication");
        std::vector<sstring> keyspaces;
        if (type == "user") {
            keyspaces = ctx.db.local().get_user_keyspaces();
        } else if (type == "non_local_strategy") {
            keyspaces = ctx.db.local().get_non_local_strategy_keyspaces();
        } else {
            keyspaces = ctx.db.local().get_all_keyspaces();
        }
        if (replication.empty() || replication == "all") {
            return keyspaces;
        }
        const auto want_tablets = replication == "tablets";
        return keyspaces | std::views::filter([&ctx, want_tablets] (const sstring& ks) {
            return ctx.db.local().find_keyspace(ks).get_replication_strategy().uses_tablets() == want_tablets;
        }) | std::ranges::to<std::vector>();
}

static
future<json::json_return_type>
rest_stop_gossiping(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        apilog.info("stop_gossiping");
        return ss.local().stop_gossiping().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_start_gossiping(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        apilog.info("start_gossiping");
        return ss.local().start_gossiping().then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_is_gossip_running(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().is_gossip_running().then([] (bool running){
            return make_ready_future<json::json_return_type>(running);
        });
}


static
future<json::json_return_type>
rest_stop_daemon(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_is_initialized(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().get_operation_mode().then([&ss] (auto mode) {
            bool is_initialized = mode >= service::storage_service::mode::STARTING && mode != service::storage_service::mode::MAINTENANCE;
            if (mode == service::storage_service::mode::NORMAL) {
                is_initialized = ss.local().gossiper().is_enabled();
            }
            return make_ready_future<json::json_return_type>(is_initialized);
        });
}

static
future<json::json_return_type>
rest_join_ring(std::unique_ptr<http::request> req) {
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_is_joined(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().get_operation_mode().then([] (auto mode) {
            return make_ready_future<json::json_return_type>(mode >= service::storage_service::mode::JOINING && mode != service::storage_service::mode::MAINTENANCE);
        });
}

static
future<json::json_return_type>
rest_is_incremental_backups_enabled(http_context& ctx, std::unique_ptr<http::request> req) {
        // If this is issued in parallel with an ongoing change, we may see values not agreeing.
        // Reissuing is asking for trouble, so we will just return true upon seeing any true value.
        return ctx.db.map_reduce(adder<bool>(), [] (replica::database& db) {
            for (auto& pair: db.get_keyspaces()) {
                auto& ks = pair.second;
                if (ks.incremental_backups_enabled()) {
                    return true;
                }
            }
            return false;
        }).then([] (bool val) {
            return make_ready_future<json::json_return_type>(val);
        });
}

static
future<json::json_return_type>
rest_set_incremental_backups_enabled(http_context& ctx, std::unique_ptr<http::request> req) {
        auto val_str = req->get_query_param("value");
        bool value = (val_str == "True") || (val_str == "true") || (val_str == "1");
        return ctx.db.invoke_on_all([value] (replica::database& db) {
            db.set_enable_incremental_backups(value);

            // Change both KS and CF, so they are in sync
            for (auto& pair: db.get_keyspaces()) {
                auto& ks = pair.second;
                ks.set_incremental_backups(value);
            }

            db.get_tables_metadata().for_each_table([&] (table_id, lw_shared_ptr<replica::table> table) {
                table->set_incremental_backups(value);
            });
        }).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_rebuild(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        utils::optional_param source_dc;
        if (auto source_dc_str = req->get_query_param("source_dc"); !source_dc_str.empty()) {
            source_dc.emplace(std::move(source_dc_str)).set_user_provided();
        }
        if (auto force_str = req->get_query_param("force"); !force_str.empty() && service::loosen_constraints(validate_bool(force_str))) {
            if (!source_dc) {
                throw bad_param_exception("The `source_dc` option must be provided for using the `force` option");
            }
            source_dc.set_force();
        }
        apilog.info("rebuild: source_dc={}", source_dc);
        return ss.local().rebuild(std::move(source_dc)).then([] {
            return make_ready_future<json::json_return_type>(json_void());
        });
}

static
future<json::json_return_type>
rest_bulk_load(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto path = req->get_path_param("path");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_bulk_load_async(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto path = req->get_path_param("path");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_reschedule_failed_deletions(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_sample_key_range(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        std::vector<sstring> res;
        return make_ready_future<json::json_return_type>(res);
}

static
future<json::json_return_type>
rest_reset_local_schema(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        // FIXME: We should truncate schema tables if more than one node in the cluster.
        apilog.info("reset_local_schema");
        co_await ss.local().reload_schema();
        co_return json_void();
}

static
future<json::json_return_type>
rest_set_trace_probability(std::unique_ptr<http::request> req) {
        auto probability = req->get_query_param("probability");
        apilog.info("set_trace_probability: probability={}", probability);
        return futurize_invoke([probability] {
            double real_prob = std::stod(probability.c_str());
            return tracing::tracing::tracing_instance().invoke_on_all([real_prob] (auto& local_tracing) {
                local_tracing.set_trace_probability(real_prob);
            }).then([] {
                return make_ready_future<json::json_return_type>(json_void());
            });
        }).then_wrapped([probability] (auto&& f) {
            try {
                f.get();
                return make_ready_future<json::json_return_type>(json_void());
            } catch (std::out_of_range& e) {
                throw httpd::bad_param_exception(e.what());
            } catch (std::invalid_argument&){
                throw httpd::bad_param_exception(format("Bad format in a probability value: \"{}\"", probability.c_str()));
            }
        });
}

static
future<json::json_return_type>
rest_get_trace_probability(std::unique_ptr<http::request> req) {
        return make_ready_future<json::json_return_type>(tracing::tracing::get_local_tracing_instance().get_trace_probability());
}

static
json::json_return_type
rest_get_slow_query_info(const_req req) {
        ss::slow_query_info res;
        res.enable = tracing::tracing::get_local_tracing_instance().slow_query_tracing_enabled();
        res.ttl = tracing::tracing::get_local_tracing_instance().slow_query_record_ttl().count() ;
        res.threshold = tracing::tracing::get_local_tracing_instance().slow_query_threshold().count();
        res.fast = tracing::tracing::get_local_tracing_instance().ignore_trace_events_enabled();
        return res;
}

static
future<json::json_return_type>
rest_set_slow_query(std::unique_ptr<http::request> req) {
        auto enable = req->get_query_param("enable");
        auto ttl = req->get_query_param("ttl");
        auto threshold = req->get_query_param("threshold");
        auto fast = req->get_query_param("fast");
        apilog.info("set_slow_query: enable={} ttl={} threshold={} fast={}", enable, ttl, threshold, fast);
        try {
            return tracing::tracing::tracing_instance().invoke_on_all([enable, ttl, threshold, fast] (auto& local_tracing) {
                if (threshold != "") {
                    local_tracing.set_slow_query_threshold(std::chrono::microseconds(std::stol(threshold.c_str())));
                }
                if (ttl != "") {
                    local_tracing.set_slow_query_record_ttl(std::chrono::seconds(std::stol(ttl.c_str())));
                }
                if (enable != "") {
                    local_tracing.set_slow_query_enabled(strcasecmp(enable.c_str(), "true") == 0);
                }
                if (fast != "") {
                    local_tracing.set_ignore_trace_events(strcasecmp(fast.c_str(), "true") == 0);
                }
            }).then([] {
                return make_ready_future<json::json_return_type>(json_void());
            });
        } catch (...) {
            throw httpd::bad_param_exception(format("Bad format value: "));
        }
}

static
future<json::json_return_type>
rest_deliver_hints(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto host = req->get_query_param("host");
        return make_ready_future<json::json_return_type>(json_void());
}

static
json::json_return_type
rest_get_cluster_name(sharded<service::storage_service>& ss, const_req req) {
        return ss.local().gossiper().get_cluster_name();
}

static
json::json_return_type
rest_get_partitioner_name(sharded<service::storage_service>& ss, const_req req) {
        return ss.local().gossiper().get_partitioner_name();
}

static
future<json::json_return_type>
rest_get_tombstone_warn_threshold(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
}

static
future<json::json_return_type>
rest_set_tombstone_warn_threshold(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto debug_threshold = req->get_query_param("debug_threshold");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_get_tombstone_failure_threshold(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
}

static
future<json::json_return_type>
rest_set_tombstone_failure_threshold(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto debug_threshold = req->get_query_param("debug_threshold");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_get_batch_size_failure_threshold(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
}

static
future<json::json_return_type>
rest_set_batch_size_failure_threshold(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto threshold = req->get_query_param("threshold");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_set_hinted_handoff_throttle_in_kb(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        auto debug_threshold = req->get_query_param("throttle");
        return make_ready_future<json::json_return_type>(json_void());
}

static
future<json::json_return_type>
rest_get_metrics_load(http_context& ctx, std::unique_ptr<http::request> req) {
        return get_cf_stats(ctx, &replica::column_family_stats::live_disk_space_used);
}

static
json::json_return_type
rest_get_exceptions(sharded<service::storage_service>& ss, const_req req) {
        return ss.local().get_exception_count();
}

static
future<json::json_return_type>
rest_get_total_hints_in_progress(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
}

static
future<json::json_return_type>
rest_get_total_hints(std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
}

static
future<json::json_return_type>
rest_get_ownership(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        if (any_of_keyspaces_use_tablets(ctx)) {
            throw httpd::bad_param_exception("storage_service/ownership cannot be used when a keyspace uses tablets");
        }

        return ss.local().get_ownership().then([] (auto&& ownership) {
            std::vector<storage_service_json::mapper> res;
            return make_ready_future<json::json_return_type>(map_to_key_value(ownership, res));
        });
}

static
future<json::json_return_type>
rest_get_effective_ownership(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto keyspace_name = req->get_path_param("keyspace") == "null" ? "" : validate_keyspace(ctx, req);
        auto table_name = req->get_query_param("cf");

        if (!keyspace_name.empty()) {
            if (table_name.empty()) {
                ensure_tablets_disabled(ctx, keyspace_name, "storage_service/ownership");
            } else {
                validate_table(ctx.db.local(), keyspace_name, table_name);
            }
        }

        return ss.local().effective_ownership(keyspace_name, table_name).then([] (auto&& ownership) {
            std::vector<storage_service_json::mapper> res;
            return make_ready_future<json::json_return_type>(map_to_key_value(ownership, res));
        });
}

static
future<json::json_return_type>
rest_estimate_compression_ratios(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
    if (!ss.local().get_feature_service().sstable_compression_dicts) {
        apilog.warn("estimate_compression_ratios: called before the cluster feature was enabled");
        throw std::runtime_error("estimate_compression_ratios requires all nodes to support the SSTABLE_COMPRESSION_DICTS cluster feature");
    }
    auto ticket = get_units(ss.local().get_do_sample_sstables_concurrency_limiter(), 1);
    auto ks = api::req_param<sstring>(*req, "keyspace", {}).value;
    auto cf = api::req_param<sstring>(*req, "cf", {}).value;
    apilog.debug("estimate_compression_ratios: called with ks={} cf={}", ks, cf);

    auto s = ctx.db.local().find_column_family(ks, cf).schema();

    auto training_sample = co_await ss.local().do_sample_sstables(s->id(), 4096, 4096);
    auto validation_sample = co_await ss.local().do_sample_sstables(s->id(), 16*1024, 1024);
    apilog.debug("estimate_compression_ratios: got training sample with {} blocks and validation sample with {}", training_sample.size(), validation_sample.size());

    auto dict = co_await ss.local().train_dict(std::move(training_sample));
    apilog.debug("estimate_compression_ratios: got dict of size {}", dict.size());

    std::vector<ss::compression_config_result> res;
    auto make_result = [](std::string_view name, int chunk_length_kb, std::string_view dict, int level, float ratio) -> ss::compression_config_result {
        ss::compression_config_result x;
        x.sstable_compression = sstring(name);
        x.chunk_length_in_kb = chunk_length_kb;
        x.dict = sstring(dict);
        x.level = level;
        x.ratio = ratio;
        return x;
    };

    using algorithm = compression_parameters::algorithm;
    for (const auto& algo : {algorithm::lz4_with_dicts, algorithm::zstd_with_dicts}) {
        for (const auto& chunk_size_kb : {1, 4, 16}) {
            std::vector<int> levels;
            if (algo == compressor::algorithm::zstd_with_dicts) {
                for (int i = 1; i <= 5; ++i) {
                    levels.push_back(i);
                }
            } else {
                levels.push_back(1);
            }
            for (auto level : levels) {
                auto algo_name = compression_parameters::algorithm_to_name(algo);
                auto m = std::map<sstring, sstring>{
                    {compression_parameters::CHUNK_LENGTH_KB, std::to_string(chunk_size_kb)},
                    {compression_parameters::SSTABLE_COMPRESSION, sstring(algo_name)},
                };
                if (algo == compressor::algorithm::zstd_with_dicts) {
                    m.insert(decltype(m)::value_type{sstring("compression_level"), sstring(std::to_string(level))});
                }
                auto params = compression_parameters(std::move(m));
                auto ratio_with_no_dict = co_await try_one_compression_config({}, s, params, validation_sample);
                auto ratio_with_past_dict = co_await try_one_compression_config(ctx.db.local().get_user_sstables_manager().get_compressor_factory(), s, params, validation_sample);
                auto ratio_with_future_dict = co_await try_one_compression_config(dict, s, params, validation_sample);
                res.push_back(make_result(algo_name, chunk_size_kb, "none", level, ratio_with_no_dict));
                res.push_back(make_result(algo_name, chunk_size_kb, "past", level, ratio_with_past_dict));
                res.push_back(make_result(algo_name, chunk_size_kb, "future", level, ratio_with_future_dict));
            }
        }
    }

    co_return res;
}

static
future<json::json_return_type>
rest_retrain_dict(http_context& ctx, sharded<service::storage_service>& ss, service::raft_group0_client& group0_client, std::unique_ptr<http::request> req) {
    if (!ss.local().get_feature_service().sstable_compression_dicts) {
        apilog.warn("retrain_dict: called before the cluster feature was enabled");
        throw std::runtime_error("retrain_dict requires all nodes to support the SSTABLE_COMPRESSION_DICTS cluster feature");
    }
    auto ticket = get_units(ss.local().get_do_sample_sstables_concurrency_limiter(), 1);
    auto ks = api::req_param<sstring>(*req, "keyspace", {}).value;
    auto cf = api::req_param<sstring>(*req, "cf", {}).value;
    apilog.debug("retrain_dict: called with ks={} cf={}", ks, cf);
    const auto t_id = ctx.db.local().find_column_family(ks, cf).schema()->id();
    constexpr uint64_t chunk_size = 4096;
    constexpr uint64_t n_chunks = 4096;
    auto sample = co_await ss.local().do_sample_sstables(t_id, chunk_size, n_chunks);
    apilog.debug("retrain_dict: got sample with {} blocks", sample.size());
    auto dict = co_await ss.local().train_dict(std::move(sample));
    apilog.debug("retrain_dict: got dict of size {}", dict.size());
    co_await ss.local().publish_new_sstable_dict(t_id, dict, group0_client);
    apilog.debug("retrain_dict: published new dict");
    co_return json_void();
}

static
future<json::json_return_type>
rest_sstable_info(http_context& ctx, std::unique_ptr<http::request> req) {
        auto ks = api::req_param<sstring>(*req, "keyspace", {}).value;
        auto cf = api::req_param<sstring>(*req, "cf", {}).value;

        // The size of this vector is bound by ks::cf. I.e. it is as most Nks + Ncf long
        // which is not small, but not huge either. 
        using table_sstables_list = std::vector<ss::table_sstables>;

        return do_with(table_sstables_list{}, [ks, cf, &ctx](table_sstables_list& dst) {
            return ctx.db.map_reduce([&dst](table_sstables_list&& res) {
                for (auto&& t : res) {
                    auto i = std::find_if(dst.begin(), dst.end(), [&t](const ss::table_sstables& t2) {
                        return t.keyspace() == t2.keyspace() && t.table() == t2.table();
                    });
                    if (i == dst.end()) {
                        dst.emplace_back(std::move(t));
                        continue;
                    }
                    auto& ssd = i->sstables; 
                    for (auto&& sd : t.sstables._elements) {
                        auto j = std::find_if(ssd._elements.begin(), ssd._elements.end(), [&sd](const ss::sstable& s) {
                            return s.generation() == sd.generation();
                        });
                        if (j == ssd._elements.end()) {
                            i->sstables.push(std::move(sd));
                        }
                    }
                }
            }, [ks, cf](const replica::database& db) {
                // see above
                table_sstables_list res;

                auto& ext = db.get_config().extensions();

                db.get_tables_metadata().for_each_table([&] (table_id, lw_shared_ptr<replica::table> t) {
                    auto& schema = t->schema();
                    if ((ks.empty() || ks == schema->ks_name()) && (cf.empty() || cf == schema->cf_name())) {
                        // at most Nsstables long
                        ss::table_sstables tst;
                        tst.keyspace = schema->ks_name();
                        tst.table = schema->cf_name();

                        for (auto sstables = t->get_sstables_including_compacted_undeleted(); auto sstable : *sstables) {
                            auto ts = db_clock::to_time_t(sstable->data_file_write_time());
                            ::tm t;
                            ::gmtime_r(&ts, &t);

                            ss::sstable info;

                            info.timestamp = t;
                            info.generation = fmt::to_string(sstable->generation());
                            info.level = sstable->get_sstable_level();
                            info.size = sstable->bytes_on_disk();
                            info.data_size = sstable->ondisk_data_size();
                            info.index_size = sstable->index_size();
                            info.filter_size = sstable->filter_size();
                            info.version = sstable->get_version();

                            if (sstable->has_component(sstables::component_type::CompressionInfo)) {
                                const auto& cp = sstable->get_compression().get_compressor();

                                ss::named_maps nm;
                                nm.group = "compression_parameters";
                                for (auto& p : cp.options()) {
                                    if (compressor::is_hidden_option_name(p.first)) {
                                        continue;
                                    }
                                    ss::mapper e;
                                    e.key = p.first;
                                    e.value = p.second;
                                    nm.attributes.push(std::move(e));
                                }
                                if (!cp.options().contains(compression_parameters::SSTABLE_COMPRESSION)) {
                                    ss::mapper e;
                                    e.key = compression_parameters::SSTABLE_COMPRESSION;
                                    e.value = sstring(cp.name());
                                    nm.attributes.push(std::move(e));
                                }
                                info.extended_properties.push(std::move(nm));
                            }

                            sstables::file_io_extension::attr_value_map map;

                            for (auto* ep : ext.sstable_file_io_extensions()) {
                                map.merge(ep->get_attributes(*sstable));
                            }

                            for (auto& p : map) {
                                struct {
                                    const sstring& key; 
                                    ss::sstable& info;
                                    void operator()(const std::map<sstring, sstring>& map) const {
                                        ss::named_maps nm;
                                        nm.group = key;
                                        for (auto& p : map) {
                                            ss::mapper e;
                                            e.key = p.first;
                                            e.value = p.second;
                                            nm.attributes.push(std::move(e));
                                        }
                                        info.extended_properties.push(std::move(nm));
                                    }
                                    void operator()(const sstring& value) const {
                                        ss::mapper e;
                                        e.key = key;
                                        e.value = value;
                                        info.properties.push(std::move(e));                                        
                                    }
                                } v{p.first, info};

                                std::visit(v, p.second);
                            }

                            tst.sstables.push(std::move(info));
                        }
                        res.emplace_back(std::move(tst));
                    }
                });
                std::sort(res.begin(), res.end(), [](const ss::table_sstables& t1, const ss::table_sstables& t2) {
                    return t1.keyspace() < t2.keyspace() || (t1.keyspace() == t2.keyspace() && t1.table() < t2.table());
                });
                return res;
            }).then([&dst] {
                return make_ready_future<json::json_return_type>(stream_object(dst));
            });
        });
}

static
future<json::json_return_type>
rest_reload_raft_topology_state(sharded<service::storage_service>& ss, service::raft_group0_client& group0_client, std::unique_ptr<http::request> req) {
        co_await ss.invoke_on(0, [&group0_client] (service::storage_service& ss) -> future<> {
            return ss.reload_raft_topology_state(group0_client);
        });
        co_return json_void();
}

static
future<json::json_return_type>
rest_upgrade_to_raft_topology(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        apilog.info("Requested to schedule upgrade to raft topology");
        try {
            co_await ss.invoke_on(0, [] (auto& ss) {
                return ss.start_upgrade_to_raft_topology();
            });
        } catch (...) {
            auto ex = std::current_exception();
            apilog.error("Failed to schedule upgrade to raft topology: {}", ex);
            std::rethrow_exception(std::move(ex));
        }
        co_return json_void();
}

static
future<json::json_return_type>
rest_raft_topology_upgrade_status(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        const auto ustate = co_await ss.invoke_on(0, [] (auto& ss) {
            return ss.get_topology_upgrade_state();
        });
        co_return sstring(format("{}", ustate));
}

static
future<json::json_return_type>
rest_raft_topology_get_cmd_status(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        const auto status = co_await ss.invoke_on(0, [] (auto& ss) {
            return ss.get_topology_cmd_status();
        });
        if (status.active_dst.empty()) {
            co_return sstring("none");
        }
        co_return sstring(fmt::format("{}[{}]: {}", status.current, status.index, fmt::join(status.active_dst, ",")));
}

static
future<json::json_return_type>
rest_move_tablet(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto src_host_id = validate_host_id(req->get_query_param("src_host"));
        shard_id src_shard_id = validate_int(req->get_query_param("src_shard"));
        auto dst_host_id = validate_host_id(req->get_query_param("dst_host"));
        shard_id dst_shard_id = validate_int(req->get_query_param("dst_shard"));
        auto token = dht::token::from_int64(validate_int(req->get_query_param("token")));
        auto ks = req->get_query_param("ks");
        auto table = req->get_query_param("table");
        auto table_id = validate_table(ctx.db.local(), ks, table);
        auto force_str = req->get_query_param("force");
        auto force = service::loosen_constraints(force_str == "" ? false : validate_bool(force_str));

        co_await ss.local().move_tablet(table_id, token,
            locator::tablet_replica{src_host_id, src_shard_id},
            locator::tablet_replica{dst_host_id, dst_shard_id},
            force);

        co_return json_void();
}

static
future<json::json_return_type>
rest_add_tablet_replica(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto dst_host_id = validate_host_id(req->get_query_param("dst_host"));
        shard_id dst_shard_id = validate_int(req->get_query_param("dst_shard"));
        auto token = dht::token::from_int64(validate_int(req->get_query_param("token")));
        auto ks = req->get_query_param("ks");
        auto table = req->get_query_param("table");
        auto table_id = validate_table(ctx.db.local(), ks, table);
        auto force_str = req->get_query_param("force");
        auto force = service::loosen_constraints(force_str == "" ? false : validate_bool(force_str));

        co_await ss.local().add_tablet_replica(table_id, token,
            locator::tablet_replica{dst_host_id, dst_shard_id},
            force);

        co_return json_void();
}

static
future<json::json_return_type>
rest_del_tablet_replica(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto dst_host_id = validate_host_id(req->get_query_param("host"));
        shard_id dst_shard_id = validate_int(req->get_query_param("shard"));
        auto token = dht::token::from_int64(validate_int(req->get_query_param("token")));
        auto ks = req->get_query_param("ks");
        auto table = req->get_query_param("table");
        auto table_id = validate_table(ctx.db.local(), ks, table);
        auto force_str = req->get_query_param("force");
        auto force = service::loosen_constraints(force_str == "" ? false : validate_bool(force_str));

        co_await ss.local().del_tablet_replica(table_id, token,
            locator::tablet_replica{dst_host_id, dst_shard_id},
            force);

        co_return json_void();
}

static
future<json::json_return_type>
rest_repair_tablet(http_context& ctx, sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto tokens_param = split(req->get_query_param("tokens"), ",");
        utils::chunked_vector<dht::token> tokens;
        bool all_tokens = tokens_param.size() == 1 && tokens_param.front() == "all";
        if (!all_tokens) {
            tokens.reserve(tokens_param.size());
            for (auto& t : tokens_param) {
                auto token = dht::token::from_int64(validate_int(t));
                tokens.push_back(token);
            }
        }
        auto ks = req->get_query_param("ks");
        auto table = req->get_query_param("table");
        bool await_completion = false;
        auto await = req->get_query_param("await_completion");
        if (!await.empty()) {
            await_completion = validate_bool(await);
        }
        auto table_id = validate_table(ctx.db.local(), ks, table);
        std::variant<utils::chunked_vector<dht::token>, service::storage_service::all_tokens_tag> tokens_variant;
        if (all_tokens) {
            tokens_variant = service::storage_service::all_tokens_tag();
        } else {
            tokens_variant = tokens;
        }
        auto hosts = req->get_query_param("hosts_filter");
        auto dcs = req->get_query_param("dcs_filter");

        std::unordered_set<locator::host_id> hosts_filter;
        if (!hosts.empty()) {
            std::string delim = ",";
            hosts_filter = std::ranges::views::split(hosts, delim) | std::views::transform([](auto&& h) {
                try {
                    return locator::host_id(utils::UUID(std::string_view{h}));
                } catch (...) {
                    throw httpd::bad_param_exception(fmt::format("Wrong host_id format {}", h));
                }
            }) | std::ranges::to<std::unordered_set>();
        }
        auto dcs_filter = locator::tablet_task_info::deserialize_repair_dcs_filter(dcs);
        auto res = co_await ss.local().add_repair_tablet_request(table_id, tokens_variant, hosts_filter, dcs_filter, await_completion);
        co_return json::json_return_type(res);
}

static
future<json::json_return_type>
rest_tablet_balancing_enable(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        auto enabled = validate_bool(req->get_query_param("enabled"));
        co_await ss.local().set_tablet_balancing_enabled(enabled);
        co_return json_void();
}

static
future<json::json_return_type>
rest_quiesce_topology(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        co_await ss.local().await_topology_quiesced();
        co_return json_void();
}

static
future<json::json_return_type>
rest_get_schema_versions(sharded<service::storage_service>& ss, std::unique_ptr<http::request> req) {
        return ss.local().describe_schema_versions().then([] (auto result) {
            std::vector<sp::mapper_list> res;
            res.reserve(result.size());
            for (auto e : result) {
                sp::mapper_list entry;
                entry.key = std::move(e.first);
                entry.value = std::move(e.second);
                res.emplace_back(std::move(entry));
            }
            return make_ready_future<json::json_return_type>(std::move(res));
        });
}


// Disambiguate between a function that returns a future and a function that returns a plain value, also
// add std::ref() as a courtesy. Also handles ks_cf_func signatures.
template <typename FuncType, typename... BindArgs>
requires std::invocable<FuncType, BindArgs&..., const_req>
    && std::same_as<seastar::json::json_return_type, std::invoke_result_t<FuncType, BindArgs&..., const_req&>>
static
seastar::httpd::json_request_function
rest_bind(FuncType func, BindArgs&... args) {
    return std::bind_front(func, std::ref(args)...);
}

template <typename FuncType, typename... BindArgs>
requires std::invocable<FuncType, BindArgs&..., std::unique_ptr<seastar::http::request>>
    && std::same_as<future<seastar::json::json_return_type>, std::invoke_result_t<FuncType, BindArgs&..., std::unique_ptr<seastar::http::request>>>
static
seastar::httpd::future_json_function
rest_bind(FuncType func, BindArgs&... args) {
    return std::bind_front(func, std::ref(args)...);
}

void set_storage_service(http_context& ctx, routes& r, sharded<service::storage_service>& ss, service::raft_group0_client& group0_client) {
    ss::get_token_endpoint.set(r, rest_bind(rest_get_token_endpoint, ctx, ss));
    ss::toppartitions_generic.set(r, rest_bind(rest_toppartitions_generic, ctx));
    ss::get_release_version.set(r, rest_bind(rest_get_release_version, ss));
    ss::get_scylla_release_version.set(r, rest_bind(rest_get_scylla_release_version, ss));
    ss::get_schema_version.set(r, rest_bind(rest_get_schema_version, ss));
    ss::get_range_to_endpoint_map.set(r, rest_bind(rest_get_range_to_endpoint_map, ctx, ss));
    ss::get_pending_range_to_endpoint_map.set(r, rest_bind(rest_get_pending_range_to_endpoint_map, ctx));
    ss::describe_ring.set(r, rest_bind(rest_describe_ring, ctx, ss));
    ss::get_load.set(r, rest_bind(rest_get_load, ctx));
    ss::get_current_generation_number.set(r, rest_bind(rest_get_current_generation_number, ss));
    ss::get_natural_endpoints.set(r, rest_bind(rest_get_natural_endpoints, ctx, ss));
    ss::cdc_streams_check_and_repair.set(r, rest_bind(rest_cdc_streams_check_and_repair, ss));
    ss::force_compaction.set(r, rest_bind(rest_force_compaction, ctx));
    ss::force_keyspace_compaction.set(r, rest_bind(rest_force_keyspace_compaction, ctx));
    ss::force_keyspace_cleanup.set(r, rest_bind(rest_force_keyspace_cleanup, ctx, ss));
    ss::cleanup_all.set(r, rest_bind(rest_cleanup_all, ctx, ss));
    ss::perform_keyspace_offstrategy_compaction.set(r, rest_bind(rest_perform_keyspace_offstrategy_compaction, ctx));
    ss::upgrade_sstables.set(r, rest_bind(rest_upgrade_sstables, ctx));
    ss::force_flush.set(r, rest_bind(rest_force_flush, ctx));
    ss::force_keyspace_flush.set(r, rest_bind(rest_force_keyspace_flush, ctx));
    ss::decommission.set(r, rest_bind(rest_decommission, ss));
    ss::move.set(r, rest_bind(rest_move, ss));
    ss::remove_node.set(r, rest_bind(rest_remove_node, ss));
    ss::get_removal_status.set(r, rest_bind(rest_get_removal_status, ss));
    ss::force_remove_completion.set(r, rest_bind(rest_force_remove_completion, ss));
    ss::set_logging_level.set(r, rest_bind(rest_set_logging_level));
    ss::get_logging_levels.set(r, rest_bind(rest_get_logging_levels));
    ss::get_operation_mode.set(r, rest_bind(rest_get_operation_mode, ss));
    ss::is_starting.set(r, rest_bind(rest_is_starting, ss));
    ss::get_drain_progress.set(r, rest_bind(rest_get_drain_progress, ctx));
    ss::drain.set(r, rest_bind(rest_drain, ss));
    ss::get_keyspaces.set(r, rest_bind(rest_get_keyspaces, ctx));
    ss::stop_gossiping.set(r, rest_bind(rest_stop_gossiping, ss));
    ss::start_gossiping.set(r, rest_bind(rest_start_gossiping, ss));
    ss::is_gossip_running.set(r, rest_bind(rest_is_gossip_running, ss));
    ss::stop_daemon.set(r, rest_bind(rest_stop_daemon));
    ss::is_initialized.set(r, rest_bind(rest_is_initialized, ss));
    ss::join_ring.set(r, rest_bind(rest_join_ring));
    ss::is_joined.set(r, rest_bind(rest_is_joined, ss));
    ss::is_incremental_backups_enabled.set(r, rest_bind(rest_is_incremental_backups_enabled, ctx));
    ss::set_incremental_backups_enabled.set(r, rest_bind(rest_set_incremental_backups_enabled, ctx));
    ss::rebuild.set(r, rest_bind(rest_rebuild, ss));
    ss::bulk_load.set(r, rest_bind(rest_bulk_load));
    ss::bulk_load_async.set(r, rest_bind(rest_bulk_load_async));
    ss::reschedule_failed_deletions.set(r, rest_bind(rest_reschedule_failed_deletions));
    ss::sample_key_range.set(r, rest_bind(rest_sample_key_range));
    ss::reset_local_schema.set(r, rest_bind(rest_reset_local_schema, ss));
    ss::set_trace_probability.set(r, rest_bind(rest_set_trace_probability));
    ss::get_trace_probability.set(r, rest_bind(rest_get_trace_probability));
    ss::get_slow_query_info.set(r, rest_bind(rest_get_slow_query_info));
    ss::set_slow_query.set(r, rest_bind(rest_set_slow_query));
    ss::deliver_hints.set(r, rest_bind(rest_deliver_hints));
    ss::get_cluster_name.set(r, rest_bind(rest_get_cluster_name, ss));
    ss::get_partitioner_name.set(r, rest_bind(rest_get_partitioner_name, ss));
    ss::get_tombstone_warn_threshold.set(r, rest_bind(rest_get_tombstone_warn_threshold));
    ss::set_tombstone_warn_threshold.set(r, rest_bind(rest_set_tombstone_warn_threshold));
    ss::get_tombstone_failure_threshold.set(r, rest_bind(rest_get_tombstone_failure_threshold));
    ss::set_tombstone_failure_threshold.set(r, rest_bind(rest_set_tombstone_failure_threshold));
    ss::get_batch_size_failure_threshold.set(r, rest_bind(rest_get_batch_size_failure_threshold));
    ss::set_batch_size_failure_threshold.set(r, rest_bind(rest_set_batch_size_failure_threshold));
    ss::set_hinted_handoff_throttle_in_kb.set(r, rest_bind(rest_set_hinted_handoff_throttle_in_kb));
    ss::get_metrics_load.set(r, rest_bind(rest_get_metrics_load, ctx));
    ss::get_exceptions.set(r, rest_bind(rest_get_exceptions, ss));
    ss::get_total_hints_in_progress.set(r, rest_bind(rest_get_total_hints_in_progress));
    ss::get_total_hints.set(r, rest_bind(rest_get_total_hints));
    ss::get_ownership.set(r, rest_bind(rest_get_ownership, ctx, ss));
    ss::get_effective_ownership.set(r, rest_bind(rest_get_effective_ownership, ctx, ss));
    ss::retrain_dict.set(r, rest_bind(rest_retrain_dict, ctx, ss, group0_client));
    ss::estimate_compression_ratios.set(r, rest_bind(rest_estimate_compression_ratios, ctx, ss));
    ss::sstable_info.set(r, rest_bind(rest_sstable_info, ctx));
    ss::reload_raft_topology_state.set(r, rest_bind(rest_reload_raft_topology_state, ss, group0_client));
    ss::upgrade_to_raft_topology.set(r, rest_bind(rest_upgrade_to_raft_topology, ss));
    ss::raft_topology_upgrade_status.set(r, rest_bind(rest_raft_topology_upgrade_status, ss));
    ss::raft_topology_get_cmd_status.set(r, rest_bind(rest_raft_topology_get_cmd_status, ss));
    ss::move_tablet.set(r, rest_bind(rest_move_tablet, ctx, ss));
    ss::add_tablet_replica.set(r, rest_bind(rest_add_tablet_replica, ctx, ss));
    ss::del_tablet_replica.set(r, rest_bind(rest_del_tablet_replica, ctx, ss));
    ss::repair_tablet.set(r, rest_bind(rest_repair_tablet, ctx, ss));
    ss::tablet_balancing_enable.set(r, rest_bind(rest_tablet_balancing_enable, ss));
    ss::quiesce_topology.set(r, rest_bind(rest_quiesce_topology, ss));
    sp::get_schema_versions.set(r, rest_bind(rest_get_schema_versions, ss));
}

void unset_storage_service(http_context& ctx, routes& r) {
    ss::get_token_endpoint.unset(r);
    ss::toppartitions_generic.unset(r);
    ss::get_release_version.unset(r);
    ss::get_scylla_release_version.unset(r);
    ss::get_schema_version.unset(r);
    ss::get_range_to_endpoint_map.unset(r);
    ss::get_pending_range_to_endpoint_map.unset(r);
    ss::describe_ring.unset(r);
    ss::get_load.unset(r);
    ss::get_current_generation_number.unset(r);
    ss::get_natural_endpoints.unset(r);
    ss::cdc_streams_check_and_repair.unset(r);
    ss::force_compaction.unset(r);
    ss::force_keyspace_compaction.unset(r);
    ss::force_keyspace_cleanup.unset(r);
    ss::cleanup_all.unset(r);
    ss::perform_keyspace_offstrategy_compaction.unset(r);
    ss::upgrade_sstables.unset(r);
    ss::force_flush.unset(r);
    ss::force_keyspace_flush.unset(r);
    ss::decommission.unset(r);
    ss::move.unset(r);
    ss::remove_node.unset(r);
    ss::get_removal_status.unset(r);
    ss::force_remove_completion.unset(r);
    ss::set_logging_level.unset(r);
    ss::get_logging_levels.unset(r);
    ss::get_operation_mode.unset(r);
    ss::is_starting.unset(r);
    ss::get_drain_progress.unset(r);
    ss::drain.unset(r);
    ss::get_keyspaces.unset(r);
    ss::stop_gossiping.unset(r);
    ss::start_gossiping.unset(r);
    ss::is_gossip_running.unset(r);
    ss::stop_daemon.unset(r);
    ss::is_initialized.unset(r);
    ss::join_ring.unset(r);
    ss::is_joined.unset(r);
    ss::is_incremental_backups_enabled.unset(r);
    ss::set_incremental_backups_enabled.unset(r);
    ss::rebuild.unset(r);
    ss::bulk_load.unset(r);
    ss::bulk_load_async.unset(r);
    ss::reschedule_failed_deletions.unset(r);
    ss::sample_key_range.unset(r);
    ss::reset_local_schema.unset(r);
    ss::set_trace_probability.unset(r);
    ss::get_trace_probability.unset(r);
    ss::get_slow_query_info.unset(r);
    ss::set_slow_query.unset(r);
    ss::deliver_hints.unset(r);
    ss::get_cluster_name.unset(r);
    ss::get_partitioner_name.unset(r);
    ss::get_tombstone_warn_threshold.unset(r);
    ss::set_tombstone_warn_threshold.unset(r);
    ss::get_tombstone_failure_threshold.unset(r);
    ss::set_tombstone_failure_threshold.unset(r);
    ss::get_batch_size_failure_threshold.unset(r);
    ss::set_batch_size_failure_threshold.unset(r);
    ss::set_hinted_handoff_throttle_in_kb.unset(r);
    ss::get_metrics_load.unset(r);
    ss::get_exceptions.unset(r);
    ss::get_total_hints_in_progress.unset(r);
    ss::get_total_hints.unset(r);
    ss::get_ownership.unset(r);
    ss::get_effective_ownership.unset(r);
    ss::sstable_info.unset(r);
    ss::reload_raft_topology_state.unset(r);
    ss::upgrade_to_raft_topology.unset(r);
    ss::raft_topology_upgrade_status.unset(r);
    ss::raft_topology_get_cmd_status.unset(r);
    ss::move_tablet.unset(r);
    ss::add_tablet_replica.unset(r);
    ss::del_tablet_replica.unset(r);
    ss::repair_tablet.unset(r);
    ss::tablet_balancing_enable.unset(r);
    ss::quiesce_topology.unset(r);
    sp::get_schema_versions.unset(r);
}

void set_load_meter(http_context& ctx, routes& r, service::load_meter& lm) {
    ss::get_load_map.set(r, [&lm] (std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        auto load_map = co_await lm.get_load_map();
        std::vector<ss::map_string_double> res;
        for (auto i : load_map) {
            ss::map_string_double val;
            val.key = i.first;
            val.value = i.second;
            res.push_back(val);
        }
        co_return res;
    });
}

void unset_load_meter(http_context& ctx, routes& r) {
    ss::get_load_map.unset(r);
}

void set_snapshot(http_context& ctx, routes& r, sharded<db::snapshot_ctl>& snap_ctl) {
    ss::get_snapshot_details.set(r, [&snap_ctl](std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        auto result = co_await snap_ctl.local().get_snapshot_details();
        co_return std::function([res = std::move(result)] (output_stream<char>&& o) -> future<> {
            std::exception_ptr ex;
            output_stream<char> out = std::move(o);
            try {
                auto result = std::move(res);
                bool first = true;

                co_await out.write("[");
                for (auto& [name, details] : result) {
                    if (!first) {
                        co_await out.write(", ");
                    }
                    std::vector<ss::snapshot> snapshot;
                    for (auto& cf : details) {
                        ss::snapshot snp;
                        snp.ks = cf.ks;
                        snp.cf = cf.cf;
                        snp.live = cf.details.live;
                        snp.total = cf.details.total;
                        snapshot.push_back(std::move(snp));
                    }
                    ss::snapshots all_snapshots;
                    all_snapshots.key = name;
                    all_snapshots.value = std::move(snapshot);
                    co_await all_snapshots.write(out);
                    first = false;
                }
                co_await out.write("]");
                co_await out.flush();
            } catch (...) {
              ex = std::current_exception();
            }
            co_await out.close();
            if (ex) {
                co_await coroutine::return_exception_ptr(std::move(ex));
            }
        });
    });

    ss::take_snapshot.set(r, [&snap_ctl](std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        apilog.info("take_snapshot: {}", req->query_parameters);
        auto tag = req->get_query_param("tag");
        auto column_families = split(req->get_query_param("cf"), ",");
        auto sfopt = req->get_query_param("sf");
        auto sf = db::snapshot_ctl::skip_flush(strcasecmp(sfopt.c_str(), "true") == 0);

        std::vector<sstring> keynames = split(req->get_query_param("kn"), ",");
        try {
            if (column_families.empty()) {
                co_await snap_ctl.local().take_snapshot(tag, keynames, sf);
            } else {
                if (keynames.empty()) {
                    throw httpd::bad_param_exception("The keyspace of column families must be specified");
                }
                if (keynames.size() > 1) {
                    throw httpd::bad_param_exception("Only one keyspace allowed when specifying a column family");
                }
                co_await snap_ctl.local().take_column_family_snapshot(keynames[0], column_families, tag, sf);
            }
            co_return json_void();
        } catch (...) {
            apilog.error("take_snapshot failed: {}", std::current_exception());
            throw;
        }
    });

    ss::del_snapshot.set(r, [&snap_ctl](std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        apilog.info("del_snapshot: {}", req->query_parameters);
        auto tag = req->get_query_param("tag");
        auto column_family = req->get_query_param("cf");

        std::vector<sstring> keynames = split(req->get_query_param("kn"), ",");
        try {
            co_await snap_ctl.local().clear_snapshot(tag, keynames, column_family);
            co_return json_void();
        } catch (...) {
            apilog.error("del_snapshot failed: {}", std::current_exception());
            throw;
        }
    });

    ss::true_snapshots_size.set(r, [&snap_ctl](std::unique_ptr<http::request> req) {
        return snap_ctl.local().true_snapshots_size().then([] (int64_t size) {
            return make_ready_future<json::json_return_type>(size);
        });
    });

    ss::scrub.set(r, [&ctx, &snap_ctl] (std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        auto& db = ctx.db;
        auto info = co_await parse_scrub_options(ctx, snap_ctl, std::move(req));

        sstables::compaction_stats stats;
        auto& compaction_module = db.local().get_compaction_manager().get_task_manager_module();
        auto task = co_await compaction_module.make_and_start_task<scrub_sstables_compaction_task_impl>({}, info.keyspace, db, info.column_families, info.opts, &stats);
        try {
            co_await task->done();
            if (stats.validation_errors) {
                co_return json::json_return_type(static_cast<int>(scrub_status::validation_errors));
            }
        } catch (const sstables::compaction_aborted_exception&) {
            co_return json::json_return_type(static_cast<int>(scrub_status::aborted));
        } catch (...) {
            apilog.error("scrub keyspace={} tables={} failed: {}", info.keyspace, info.column_families, std::current_exception());
            throw;
        }

        co_return json::json_return_type(static_cast<int>(scrub_status::successful));
    });

    ss::start_backup.set(r, [&snap_ctl] (std::unique_ptr<http::request> req) -> future<json::json_return_type> {
        auto endpoint = req->get_query_param("endpoint");
        auto keyspace = req->get_query_param("keyspace");
        auto table = req->get_query_param("table");
        auto bucket = req->get_query_param("bucket");
        auto prefix = req->get_query_param("prefix");
        auto snapshot_name = req->get_query_param("snapshot");
        auto move_files = req_param<bool>(*req, "move_files", false);
        if (snapshot_name.empty()) {
            // TODO: If missing, snapshot should be taken by scylla, then removed
            throw httpd::bad_param_exception("The snapshot name must be specified");
        }

        auto& ctl = snap_ctl.local();
        auto task_id = co_await ctl.start_backup(std::move(endpoint), std::move(bucket), std::move(prefix), std::move(keyspace), std::move(table), std::move(snapshot_name), move_files);
        co_return json::json_return_type(fmt::to_string(task_id));
    });

    cf::get_true_snapshots_size.set(r, [&snap_ctl] (std::unique_ptr<http::request> req) {
        auto [ks, cf] = parse_fully_qualified_cf_name(req->get_path_param("name"));
        return snap_ctl.local().true_snapshots_size(std::move(ks), std::move(cf)).then([] (int64_t res) {
            return make_ready_future<json::json_return_type>(res);
        });
    });

    cf::get_all_true_snapshots_size.set(r, [] (std::unique_ptr<http::request> req) {
        //TBD
        unimplemented();
        return make_ready_future<json::json_return_type>(0);
    });

}

void unset_snapshot(http_context& ctx, routes& r) {
    ss::get_snapshot_details.unset(r);
    ss::take_snapshot.unset(r);
    ss::del_snapshot.unset(r);
    ss::true_snapshots_size.unset(r);
    ss::scrub.unset(r);
    ss::start_backup.unset(r);
    cf::get_true_snapshots_size.unset(r);
    cf::get_all_true_snapshots_size.unset(r);
}

}
