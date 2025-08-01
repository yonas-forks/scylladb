/*
 * Copyright (C) 2016 ScyllaDB
 */



#include <boost/test/unit_test.hpp>

#include <stdint.h>
#include <random>
#include <regex>

#include <seastar/core/future-util.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/defer.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/tls.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>

#include <seastar/testing/test_case.hh>

#include <fmt/ranges.h>

#include "ent/encryption/azure_host.hh"
#include "ent/encryption/encryption.hh"
#include "ent/encryption/symmetric_key.hh"
#include "ent/encryption/local_file_provider.hh"
#include "ent/encryption/encryption_exceptions.hh"
#include "test/lib/tmpdir.hh"
#include "test/lib/test_utils.hh"
#include "test/lib/random_utils.hh"
#include "test/lib/cql_test_env.hh"
#include "test/lib/cql_assertions.hh"
#include "test/lib/log.hh"
#include "test/lib/proc_utils.hh"
#include "db/config.hh"
#include "db/extensions.hh"
#include "db/commitlog/commitlog.hh"
#include "db/commitlog/commitlog_replayer.hh"
#include "init.hh"
#include "sstables/sstables.hh"
#include "cql3/untyped_result_set.hh"
#include "utils/rjson.hh"
#include "utils/azure/identity/exceptions.hh"
#include "utils/azure/identity/managed_identity_credentials.hh"
#include "utils/azure/identity/service_principal_credentials.hh"
#include "replica/database.hh"
#include "service/client_state.hh"

using namespace encryption;
namespace fs = std::filesystem;

using test_hook = std::function<void(cql_test_env&)>;

struct test_provider_args {
    const tmpdir& tmp;
    std::string options;
    std::string extra_yaml = {};
    unsigned n_tables = 1;
    unsigned n_restarts = 1; 
    std::string explicit_provider = {};

    test_hook before_create_table;
    test_hook after_create_table;
    test_hook after_insert;
    test_hook on_insert_exception;

    test_hook before_verify;

    std::optional<timeout_config> timeout;
};

static void do_create_and_insert(cql_test_env& env, const test_provider_args& args, const std::string& pk, const std::string& v) {
    for (auto i = 0u; i < args.n_tables; ++i) {
        if (args.before_create_table) {
            testlog.debug("Calling before create table");
            args.before_create_table(env);
        }
        if (args.options.empty()) {
            env.execute_cql(fmt::format("create table t{} (pk text primary key, v text)", i)).get();
        } else {
            env.execute_cql(fmt::format("create table t{} (pk text primary key, v text) WITH scylla_encryption_options={{{}}}", i, args.options)).get();
        }

        if (args.after_create_table) {
            testlog.debug("Calling after create table");
            args.after_create_table(env);
        }
        try {
            env.execute_cql(fmt::format("insert into ks.t{} (pk, v) values ('{}', '{}')", i, pk, v)).get();
        } catch (...) {
            testlog.info("Insert error {}. Notifying.", std::current_exception());
            args.on_insert_exception(env);
            throw;
        }
        if (args.after_insert) {
            testlog.debug("Calling after insert");
            args.after_insert(env);
        }
    }
}

static future<> test_provider(const test_provider_args& args) {
    auto make_config = [&] {
        auto ext = std::make_shared<db::extensions>();
        auto cfg = seastar::make_shared<db::config>(ext);
        cfg->data_file_directories({args.tmp.path().string()});

        // Currently the test fails with consistent_cluster_management = true. See #2995.
        cfg->consistent_cluster_management(false);

        {
            boost::program_options::options_description desc;
            boost::program_options::options_description_easy_init init(&desc);
            configurable::append_all(*cfg, init);
        }
        if (!args.extra_yaml.empty()) {
            cfg->read_from_yaml(args.extra_yaml);
        }

        return std::make_tuple(cfg, ext);
    };

    std::string pk = "apa";
    std::string v = "ko";

    {
        auto [cfg, ext] = make_config();

        co_await do_with_cql_env_thread([&] (cql_test_env& env) {
            do_create_and_insert(env, args, pk, v);
        }, cfg, {}, cql_test_init_configurables{ *ext });
    }


    for (auto rs = 0u; rs < args.n_restarts; ++rs) {
        auto [cfg, ext] = make_config();

        co_await do_with_cql_env_thread([&] (cql_test_env& env) {
            if (args.before_verify) {
                testlog.debug("Calling after second start");
                args.before_verify(env);
            }
            for (auto i = 0u; i < args.n_tables; ++i) {
                require_rows(env, fmt::format("select * from ks.t{}", i), {{utf8_type->decompose(pk), utf8_type->decompose(v)}});

                auto provider = args.explicit_provider;

                // check that all sstables have the defined provider class (i.e. are encrypted using correct optons)
                if (provider.empty() && args.options.find("'key_provider'") != std::string::npos) {
                    static std::regex ex(R"foo('key_provider'\s*:\s*'(\w+)')foo");

                    std::smatch m;
                    BOOST_REQUIRE(std::regex_search(args.options.begin(), args.options.end(), m, ex));
                    provider = m[1].str();
                    BOOST_REQUIRE(!provider.empty());
                }
                if (!provider.empty()) {
                    env.db().invoke_on_all([&](replica::database& db) {
                        auto& cf = db.find_column_family("ks", "t" + std::to_string(i));
                        auto sstables = cf.get_sstables_including_compacted_undeleted();

                        if (sstables) {
                            for (auto& t : *sstables) {
                                auto sst_provider = encryption::encryption_provider(*t);
                                BOOST_REQUIRE_EQUAL(provider, sst_provider);
                            }
                        }
                    }).get();
                }
            }
        }, cfg, {}, cql_test_init_configurables{ *ext });
    }
}

static future<> test_provider(const std::string& options, const tmpdir& tmp, const std::string& extra_yaml = {}, unsigned n_tables = 1, unsigned n_restarts = 1, const std::string& explicit_provider = {}) {
    test_provider_args args{
        .tmp = tmp,
        .options = options,
        .extra_yaml = extra_yaml,
        .n_tables = n_tables, 
        .n_restarts = n_restarts, 
        .explicit_provider = explicit_provider
    };
    co_await test_provider(args);
}

SEASTAR_TEST_CASE(test_local_file_provider) {
    tmpdir tmp;
    auto keyfile = tmp.path() / "secret_key";
    co_await test_provider(fmt::format("'key_provider': 'LocalFileSystemKeyProviderFactory', 'secret_key_file': '{}', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", keyfile.string()), tmp);
}

static future<> create_key_file(const fs::path& path, const std::vector<key_info>& key_types) {
    std::ostringstream ss;

    for (auto& info : key_types) {
        symmetric_key k(info);
        ss << info.alg << ":" << info.len << ":" << base64_encode(k.key()) << std::endl;
    }

    auto s = ss.str();
    co_await seastar::recursive_touch_directory(fs::path(path).remove_filename().string());
    co_await write_text_file_fully(path.string(), s);
}

static future<> do_test_replicated_provider(unsigned n_tables, unsigned n_restarts, const std::string& extra = {}, test_hook hook = {}) {
    tmpdir tmp;
    auto keyfile = tmp.path() / "secret_key";
    auto sysdir = tmp.path() / "system_keys";
    auto syskey = sysdir / "system_key";
    auto yaml = fmt::format("system_key_directory: {}", sysdir.string());

    co_await create_key_file(syskey, { { "AES/CBC/PKCSPadding", 256 }});

    BOOST_REQUIRE(fs::exists(syskey));;

    test_provider_args args{
        .tmp = tmp,
        .options = fmt::format("'key_provider': 'ReplicatedKeyProviderFactory', 'system_key_file': 'system_key', 'secret_key_file': '{}','cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128{}", keyfile.string(), extra),
        .extra_yaml = yaml,
        .n_tables = n_tables, 
        .n_restarts = n_restarts, 
        .explicit_provider = {},
        .after_create_table = hook
    };

    co_await test_provider(args);

    BOOST_REQUIRE(fs::exists(tmp.path()));
}

SEASTAR_TEST_CASE(test_replicated_provider) {
    co_await do_test_replicated_provider(1, 1);
}

SEASTAR_TEST_CASE(test_replicated_provider_many_tables) {
    co_await do_test_replicated_provider(100, 5);
}

using namespace std::chrono_literals;

static const timeout_config rkp_db_timeout_config {
    5s, 5s, 5s, 5s, 5s, 5s, 5s,
};

static service::query_state& rkp_db_query_state() {
    static thread_local service::client_state cs(service::client_state::internal_tag{}, rkp_db_timeout_config);
    static thread_local service::query_state qs(cs, empty_service_permit());
    return qs;
}

SEASTAR_TEST_CASE(test_replicated_provider_shutdown_failure) {
    co_await do_test_replicated_provider(1, 1, ", 'DEBUG': 'nocache,novalidate'", [](cql_test_env& env) {
        /**
         * Try to remove all keys in replicated table. Note: we can't use truncate because we 
         * are not running any proper remotes.
        */
        auto res = env.local_qp().execute_internal("select * from system_replicated_keys.encrypted_keys", 
            db::consistency_level::ONE, rkp_db_query_state(), {}, cql3::query_processor::cache_internal::no
            ).get();
        for (auto& row : (*res)) {
            auto key_file = row.get_as<sstring>("key_file");
            auto cipher = row.get_as<sstring>("cipher");
            auto strength = row.get_as<int32_t>("strength");
            auto uuid = row.get_as<utils::UUID>("key_id");

            env.local_qp().execute_internal("delete from system_replicated_keys.encrypted_keys where key_file=? AND cipher=? AND strength=? AND key_id=?", 
                db::consistency_level::ONE, rkp_db_query_state(), 
                { key_file, cipher, strength, uuid }, 
                cql3::query_processor::cache_internal::no
            ).get();
        }
    });
}

static std::string get_var_or_default(const char* var, std::string_view def, bool* set) {
    const char* val = std::getenv(var);
    if (val == nullptr) {
        *set = false;
        return std::string(def);
    }
    *set = true;
    return val;
}

static std::string get_var_or_default(const char* var, std::string_view def) {
    bool dummy;
    return get_var_or_default(var, def, &dummy);
}

static bool check_run_test(const char* var, bool defval = false) {
    auto do_test = get_var_or_default(var, std::to_string(defval));

    if (!strcasecmp(do_test.data(), "0") || !strcasecmp(do_test.data(), "false")) {
        BOOST_TEST_MESSAGE(fmt::format("Skipping test. Set {}=1 to run", var));
        return false;
    }
    return true;
}

static auto check_run_test_decorator(const char* var, bool def = false) {
    return boost::unit_test::precondition(std::bind(&check_run_test, var, def));
}

#ifdef HAVE_KMIP

struct kmip_test_info {
    std::string host;
    std::string cert;
    std::string key;
    std::string ca;
    std::string prio;
};

static future<> kmip_test_helper(const std::function<future<>(const kmip_test_info&, const tmpdir&)>& f) {
    tmpdir tmp;
    bool host_set = false;

    static const char* def_resourcedir = "./test/resource/certs";
    const char* resourcedir = std::getenv("KMIP_RESOURCE_DIR");
    if (resourcedir == nullptr) {
        resourcedir = def_resourcedir;
    } 

    kmip_test_info info {
        .host = get_var_or_default("KMIP_HOST", "127.0.0.1", &host_set),
        .cert = get_var_or_default("KMIP_CERT", fmt::format("{}/scylla.pem", resourcedir)),
        .key = get_var_or_default("KMIP_KEY", fmt::format("{}/scylla.pem", resourcedir)),
        .ca = get_var_or_default("KMIP_CA", fmt::format("{}/cacert.pem", resourcedir)),
        .prio = get_var_or_default("KMIP_PRIO", "SECURE128:+RSA:-VERS-TLS1.0:-ECDHE-ECDSA")
    };

    // note: default kmip port = 5696;

    if (!host_set) {
        // Note: we set `enable_tls_client_auth=False` - client cert is still validated, 
        // but we have note generated certs with "extended usage client OID", which 
        // pykmip will check for if this is true.
        auto cfg = fmt::format(R"foo(
[server]
hostname=127.0.0.1
port=1
certificate_path={}
key_path={}
ca_path={}
auth_suite=TLS1.2
policy_path={}
enable_tls_client_auth=False
logging_level=DEBUG
database_path=:memory:
        )foo", info.cert, info.key, info.ca, tmp.path().string());

        auto cfgfile = fmt::format("{}/pykmip.conf", tmp.path().string());
        auto log = fmt::format("{}/pykmip.log", tmp.path().string());

        {
            std::ofstream of(cfgfile);
            of << cfg;
        }

        auto pyexec = tests::proc::find_file_in_path("python");

        promise<int> port_promise;
        auto port_future = port_promise.get_future();

        auto python = co_await tests::proc::process_fixture::create(pyexec, 
            { // args
                pyexec.string(),
                "test/boost/kmip_wrapper.py",
                "-l", log,
                "-f", cfgfile,
                "-v", "DEBUG",
            },
            { // env
                fmt::format("TMPDIR={}", tmp.path().string())
            },
            // stdout handler
            [port_promise = std::move(port_promise), b = false](std::string_view line) mutable -> future<consumption_result<char>> {
                static std::regex port_ex("Listening on (\\d+)");

                std::cout << line << std::endl;
                std::match_results<typename std::string_view::const_iterator> m;
                if (!b && std::regex_match(line.begin(), line.end(), m, port_ex)) {
                    port_promise.set_value(std::stoi(m[1].str()));
                    BOOST_TEST_MESSAGE("Matched PyKMIP port: " + m[1].str());
                    b = true;
                }
                co_return continue_consuming{};
            },
            // stderr handler
            tests::proc::process_fixture::create_copy_handler(std::cerr)
        );

        std::exception_ptr ep;

        try {
            // arbitrary timeout of 20s for the server to make some output. Very generous.
            auto port = co_await with_timeout(std::chrono::steady_clock::now() + 20s, std::move(port_future));

            if (port <= 0) {
                throw std::runtime_error("Invalid port");
            }

            tls::credentials_builder b;
            co_await b.set_x509_trust_file(info.ca, seastar::tls::x509_crt_format::PEM);
            co_await b.set_x509_key_file(info.cert, info.key, seastar::tls::x509_crt_format::PEM);
            auto certs = b.build_certificate_credentials();

            // wait for port.
            for (;;) {
                try {
                    // TODO: seastar does not have a connect with timeout. That would be helpful here. But alas...
                    auto c = co_await seastar::tls::connect(certs, socket_address(net::inet_address("127.0.0.1"), port));
                    BOOST_TEST_MESSAGE("PyKMIP server up and available"); // debug print. Why not.
                    co_await tls::check_session_is_resumed(c); // forces handshake. Make python ssl happy.
                    c.shutdown_output();
                    break;
                } catch (...) {
                }
                co_await sleep(100ms);
            }

            info.host = fmt::format("127.0.0.1:{}", port);

            co_await f(info, tmp);

        } catch (timed_out_error&) {
            ep = std::make_exception_ptr(std::runtime_error("Could not start pykmip"));
        } catch (...) {
            ep = std::current_exception();
        }

        BOOST_TEST_MESSAGE("Stopping PyKMIP server"); // debug print. Why not.

        python.terminate();
        co_await python.wait();

        if (ep) {
            std::rethrow_exception(ep);
        }
    } else {
        co_await f(info, tmp);
    }
}

SEASTAR_TEST_CASE(test_kmip_provider, *check_run_test_decorator("ENABLE_KMIP_TEST", true)) {
    co_await kmip_test_helper([](const kmip_test_info& info, const tmpdir& tmp) -> future<> {
        auto yaml = fmt::format(R"foo(
            kmip_hosts:
                kmip_test:
                    hosts: {0}
                    certificate: {1}
                    keyfile: {2}
                    truststore: {3}
                    priority_string: {4}
                    )foo"
            , info.host, info.cert, info.key, info.ca, info.prio
        );
        co_await test_provider("'key_provider': 'KmipKeyProviderFactory', 'kmip_host': 'kmip_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    });
}

#endif // HAVE_KMIP

class fake_proxy {
    seastar::server_socket _socket;
    socket_address _address;
    bool _go_on = true;
    bool _do_proxy = true;
    future<> _f;

    future<> run(std::string dst_addr) {
        uint16_t port = 443u;
        auto i = dst_addr.find_last_of(':');
        if (i != std::string::npos && i > 0 && dst_addr[i - 1] != ':') { // just check against ipv6...
            port = std::stoul(dst_addr.substr(i + 1));
            dst_addr = dst_addr.substr(0, i);
        }

        auto addr = co_await seastar::net::dns::resolve_name(dst_addr);
        std::vector<future<>> work;

        while (_go_on) {
            try {
                auto client = co_await _socket.accept();
                auto dst = co_await seastar::connect(socket_address(addr, port));

                testlog.debug("Got proxy connection: {}->{}:{} ({})", client.remote_address, dst_addr, port, _do_proxy);

                auto f = [&]() -> future<> {
                    auto& s = client.connection;
                    auto& ldst = dst;
                    auto addr = client.remote_address;

                    auto do_io = [this, &addr, &dst_addr, port](connected_socket& src, connected_socket& dst) noexcept -> future<> {
                        try {
                            auto sin = src.input();
                            auto dout = output_stream<char>(dst.output().detach(), 1024);
                            // note: have to have differing conditions for proxying
                            // and shutdown, and need to check inside look, because
                            // kmip connector caches connection -> not new socket.
                            std::exception_ptr p;
                            try {
                                while (_go_on && _do_proxy && !sin.eof()) {
                                    auto buf = co_await sin.read();
                                    auto n = buf.size();
                                    testlog.trace("Read {} bytes: {}->{}:{}", n, addr, dst_addr, port);
                                    if (_do_proxy) {
                                        co_await dout.write(std::move(buf));
                                        co_await dout.flush();
                                        testlog.trace("Wrote {} bytes: {}->{}:{}", n, addr, dst_addr, port);
                                    }
                                }
                            } catch (...) {
                                p = std::current_exception();
                            }
                            co_await dout.flush();
                            co_await dout.close();
                            co_await sin.close();
                            if (p) {
                                std::rethrow_exception(p);
                            }
                        } catch (...) {
                            testlog.warn("Exception running proxy {}:{}->{}: {}", dst_addr, port, _address, std::current_exception());
                        }
                    };
                    co_await when_all(do_io(s, ldst), do_io(ldst, s));
                }();

                work.emplace_back(std::move(f));
            } catch (...) {
                testlog.warn("Exception running proxy {}: {}", _address, std::current_exception());
            }
        }

        for (auto&& f : work) {
            try {
                co_await std::move(f);
            } catch (...) {
            }
        }
    }
public:
    fake_proxy(std::string dst)
        : _socket(seastar::listen(socket_address(0x7f000001, 0)))
        , _address(_socket.local_address())
        , _f(run(std::move(dst)))
    {}

    const socket_address& address() const {
        return _address;
    }
    void enable(bool b) {
        _do_proxy = b;
        testlog.info("Set proxy {} enabled = {}", _address, b);
    }
    future<> stop() {
        if (std::exchange(_go_on, false)) {
            testlog.info("Stopping proxy {}", _address);
            _socket.abort_accept();
            co_await std::move(_f);
        }
    }
};

#ifdef HAVE_KMIP

SEASTAR_TEST_CASE(test_kmip_provider_multiple_hosts, *check_run_test_decorator("ENABLE_KMIP_TEST", true)) {
    /**
     * Tests for #3251. KMIP connector ends up in endless loop if using more than one
     * fallover host. This is only in initial connection (in real life only in initial connection verification).
     * 
     * We don't have access to more than one KMIP server for testing (at a time).
     * Pretend to have failover by using a local proxy.
    */
    co_await kmip_test_helper([](const kmip_test_info& info, const tmpdir& tmp) -> future<> {
        fake_proxy proxy(info.host);

        auto host2 = boost::lexical_cast<std::string>(proxy.address());

        auto yaml = fmt::format(R"foo(
            kmip_hosts:
                kmip_test:
                    hosts: {0}, {5}
                    certificate: {1}
                    keyfile: {2}
                    truststore: {3}
                    priority_string: {4}
                    )foo"
            , info.host, info.cert, info.key, info.ca, info.prio, host2
        );

        std::exception_ptr ex;

        try {
            co_await test_provider("'key_provider': 'KmipKeyProviderFactory', 'kmip_host': 'kmip_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
        } catch (...) {
            ex = std::current_exception();    
        }

        co_await proxy.stop();

        if (ex) {
            std::rethrow_exception(ex);
        }
    });
}

#endif // HAVE_KMIP

/*
Simple test of KMS provider. Still has some caveats:

    1.) Uses aws CLI credentials for auth. I.e. you need to have a valid
        ~/.aws/credentials for the user running the test.
    2.) I can't figure out a good way to set up a key "everyone" can access. So user needs
        to have read/encrypt access to the key alias (default "alias/kms_encryption_test")
        in the scylla AWS account.

    A "better" solution might be to create dummmy user only for KMS testing with only access
    to a single key, and no other priviledges. But that seems dangerous as well.

    For this reason, this test is parameterized with env vars:
    * ENABLE_KMS_TEST - set to non-zero (1/true) to run
    * KMS_KEY_ALIAS - default "alias/kms_encryption_test" - set to key alias you have access to.
    * KMS_AWS_REGION - default us-east-1 - set to whatever region your key is in.

    NOTE: When run via test.py, the minio server used there will, unless already set,
    put AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY into the inherited process env, with
    values purely fictional, and only usable by itself. This _will_ screw up credentials
    resolution in the KMS connector, and will lead to errors not intended.

    In CI, we provide the vars from jenkins, with working values, and the minio
    respects this.

    As a workaround, try setting the vars yourself to something that actually works (i.e. 
    values from your .awscredentials). Or complain until we find a way to make the minio
    server optional for tests.

*/
static future<> kms_test_helper(std::function<future<>(const tmpdir&, std::string_view, std::string_view, std::string_view)> f) {
    auto kms_key_alias = get_var_or_default("KMS_KEY_ALIAS", "alias/kms_encryption_test");
    auto kms_aws_region = get_var_or_default("KMS_AWS_REGION", "us-east-1");
    auto kms_aws_profile = get_var_or_default("KMS_AWS_PROFILE", "default");

    tmpdir tmp;

    co_await f(tmp, kms_key_alias, kms_aws_region, kms_aws_profile);
}

SEASTAR_TEST_CASE(test_kms_provider, *check_run_test_decorator("ENABLE_KMS_TEST")) {
    co_await kms_test_helper([](const tmpdir& tmp, std::string_view kms_key_alias, std::string_view kms_aws_region, std::string_view kms_aws_profile) -> future<> {
        /**
         * Note: NOT including any auth stuff here. The provider will pick up AWS credentials
         * from ~/.aws/credentials
         */
        auto yaml = fmt::format(R"foo(
            kms_hosts:
                kms_test:
                    master_key: {0}
                    aws_region: {1}
                    aws_profile: {2}
                    )foo"
            , kms_key_alias, kms_aws_region, kms_aws_profile
        );

        co_await test_provider("'key_provider': 'KmsKeyProviderFactory', 'kms_host': 'kms_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    });
}

SEASTAR_TEST_CASE(test_kms_provider_with_master_key_in_cf, *check_run_test_decorator("ENABLE_KMS_TEST")) {
    co_await kms_test_helper([](const tmpdir& tmp, std::string_view kms_key_alias, std::string_view kms_aws_region, std::string_view kms_aws_profile) -> future<> {
        /**
         * Note: NOT including any auth stuff here. The provider will pick up AWS credentials
         * from ~/.aws/credentials
         */
        auto yaml = fmt::format(R"foo(
            kms_hosts:
                kms_test:
                    aws_region: {1}
                    aws_profile: {2}
                    )foo"
            , kms_key_alias, kms_aws_region, kms_aws_profile
        );

        // should fail
        try {
            try {
                co_await test_provider("'key_provider': 'KmsKeyProviderFactory', 'kms_host': 'kms_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', "
                                       "'secret_key_strength': 128",
                        tmp, yaml);
            } catch (std::nested_exception& ex) {
                std::rethrow_if_nested(ex);
            }
            BOOST_FAIL("Required an exception to be re-thrown");
        } catch (encryption::configuration_error&) {
            // EXPECTED
        } catch (...) {
            BOOST_FAIL(format("Unexpected exception: {}", std::current_exception()));
        }

        // should be ok
        co_await test_provider(fmt::format("'key_provider': 'KmsKeyProviderFactory', 'kms_host': 'kms_test', 'master_key': '{}', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", kms_key_alias)
            , tmp, yaml
            );
    });
}


SEASTAR_TEST_CASE(test_user_info_encryption) {
    tmpdir tmp;
    auto keyfile = tmp.path() / "secret_key";

    auto yaml = fmt::format(R"foo(
        user_info_encryption:
            enabled: True
            key_provider: LocalFileSystemKeyProviderFactory
            secret_key_file: {}
            cipher_algorithm: AES/CBC/PKCS5Padding
            secret_key_strength: 128
        )foo"
    , keyfile.string());

    co_await test_provider({}, tmp, yaml, 4, 1, "LocalFileSystemKeyProviderFactory" /* verify encrypted even though no kp in options*/);
}

SEASTAR_TEST_CASE(test_kms_provider_with_broken_algo, *check_run_test_decorator("ENABLE_KMS_TEST")) {
    co_await kms_test_helper([](const tmpdir& tmp, std::string_view kms_key_alias, std::string_view kms_aws_region, std::string_view kms_aws_profile) -> future<> {
        /**
         * Note: NOT including any auth stuff here. The provider will pick up AWS credentials
         * from ~/.aws/credentials
         */
        auto yaml = fmt::format(R"foo(
            kms_hosts:
                kms_test:
                    master_key: {0}
                    aws_region: {1}
                    aws_profile: {2}
                    )foo"
            , kms_key_alias, kms_aws_region, kms_aws_profile
        );

        try {
            co_await test_provider("'key_provider': 'KmsKeyProviderFactory', 'kms_host': 'kms_test', 'cipher_algorithm':'', 'secret_key_strength': 128", tmp, yaml);
            BOOST_FAIL("should not reach");
        } catch (exceptions::configuration_exception&) {
            // ok
        }
    });
}

static auto make_commitlog_config(const test_provider_args& args, const std::unordered_map<std::string, std::string>& scopts) {
    auto ext = std::make_shared<db::extensions>();
    auto cfg = seastar::make_shared<db::config>(ext);
    cfg->data_file_directories({args.tmp.path().string()});
    cfg->commitlog_sync("batch"); // just to make sure files are written

    // Currently the test fails with consistent_cluster_management = true. See #2995.
    cfg->consistent_cluster_management(false);

    boost::program_options::options_description desc;
    boost::program_options::options_description_easy_init init(&desc);
    configurable::append_all(*cfg, init);

    std::ostringstream ss;
    ss  << "system_info_encryption:" << std::endl
        << "    enabled: true" << std::endl
        << "    cipher_algorithm: AES/CBC/PKCS5Padding" << std::endl
        << "    secret_key_strength: 128" << std::endl
        ;

    for (auto& [k, v] : scopts) {
        ss << "    " << k << ": " << v << std::endl;
    }
    auto str = ss.str();
    cfg->read_from_yaml(str);

    if (!args.extra_yaml.empty()) {
        cfg->read_from_yaml(args.extra_yaml);
    }

    return std::make_tuple(cfg, ext);
}

static future<> test_encrypted_commitlog(const test_provider_args& args, std::unordered_map<std::string, std::string> scopts = {}) {
    fs::path clback = args.tmp.path() / "commitlog_back";

    std::string pk = "apa";
    std::string v = "ko";


    {
        auto [cfg, ext] = make_commitlog_config(args, scopts);

        cql_test_config cqlcfg(cfg);

        if (args.timeout) {
            cqlcfg.query_timeout = args.timeout;
        }

        co_await do_with_cql_env_thread([&] (cql_test_env& env) {
            do_create_and_insert(env, args, pk, v);
            fs::copy(fs::path(cfg->commitlog_directory()), clback);
        }, cqlcfg, {}, cql_test_init_configurables{ *ext });

    }

    {
        auto [cfg, ext] = make_commitlog_config(args, scopts);

        cql_test_config cqlcfg(cfg);

        if (args.timeout) {
            cqlcfg.query_timeout = args.timeout;
        }

        co_await do_with_cql_env_thread([&] (cql_test_env& env) {
            // Fake commitlog replay using the files copied.
            std::vector<sstring> paths;
            for (auto const& dir_entry : fs::directory_iterator{clback})  {
                auto p = dir_entry.path();
                try {
                    db::commitlog::descriptor d(p);
                    paths.emplace_back(std::move(p));
                } catch (...) {
                }
            }

            BOOST_REQUIRE(!paths.empty()); 

            auto rp = db::commitlog_replayer::create_replayer(env.db(), env.get_system_keyspace()).get();
            rp.recover(paths, db::commitlog::descriptor::FILENAME_PREFIX).get();

            // not really checking anything, but make sure we did not break anything.
            for (auto i = 0u; i < args.n_tables; ++i) {
                require_rows(env, fmt::format("select * from ks.t{}", i), {{utf8_type->decompose(pk), utf8_type->decompose(v)}});
            }
        }, cqlcfg, {}, cql_test_init_configurables{ *ext });
    }
}

static future<> test_encrypted_commitlog(const tmpdir& tmp, std::unordered_map<std::string, std::string> scopts = {}, const std::string& extra_yaml = {}, unsigned n_tables = 1) {
    test_provider_args args{
        .tmp = tmp,
        .extra_yaml = extra_yaml,
        .n_tables = n_tables, 
    };

    co_await test_encrypted_commitlog(args, std::move(scopts));
}

SEASTAR_TEST_CASE(test_commitlog_kms_encryption_with_slow_key_resolve, *check_run_test_decorator("ENABLE_KMS_TEST")) {
    co_await kms_test_helper([](const tmpdir& tmp, std::string_view kms_key_alias, std::string_view kms_aws_region, std::string_view kms_aws_profile) -> future<> {
        /**
         * Note: NOT including any auth stuff here. The provider will pick up AWS credentials
         * from ~/.aws/credentials
         */
        auto yaml = fmt::format(R"foo(
            kms_hosts:
                kms_test:
                    master_key: {0}
                    aws_region: {1}
                    aws_profile: {2}
                    )foo"
            , kms_key_alias, kms_aws_region, kms_aws_profile
        );

        co_await test_encrypted_commitlog(tmp, { { "key_provider", "KmsKeyProviderFactory" }, { "kms_host", "kms_test" } }, yaml);
    });
}

#ifdef HAVE_KMIP

SEASTAR_TEST_CASE(test_commitlog_kmip_encryption_with_slow_key_resolve, *check_run_test_decorator("ENABLE_KMIP_TEST")) {
    co_await kmip_test_helper([](const kmip_test_info& info, const tmpdir& tmp) -> future<> {
        auto yaml = fmt::format(R"foo(
            kmip_hosts:
                kmip_test:
                    hosts: {0}
                    certificate: {1}
                    keyfile: {2}
                    truststore: {3}
                    priority_string: {4}
                    )foo"
            , info.host, info.cert, info.key, info.ca, info.prio
        );
        co_await test_encrypted_commitlog(tmp, { { "key_provider", "KmipKeyProviderFactory" }, { "kmip_host", "kmip_test" } }, yaml);
    });
}

#endif // HAVE_KMIP

SEASTAR_TEST_CASE(test_user_info_encryption_dont_allow_per_table_encryption) {
    tmpdir tmp;
    auto keyfile = tmp.path() / "secret_key";

    auto yaml = fmt::format(R"foo(
        allow_per_table_encryption: false
        user_info_encryption:
            enabled: True
            key_provider: LocalFileSystemKeyProviderFactory
            secret_key_file: {}
            cipher_algorithm: AES/CBC/PKCS5Padding
            secret_key_strength: 128
        )foo"
    , keyfile.string());

    try {
        co_await test_provider(
            fmt::format("'key_provider': 'LocalFileSystemKeyProviderFactory', 'secret_key_file': '{}', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", keyfile.string())
            , tmp, yaml, 4, 1
            );
        BOOST_FAIL("Should not reach");
    } catch (std::invalid_argument&) {
        // Ok. 
    }
}

/*
    Simple test of GCP cloudkms provider. Uses scylladb GCP project "scylla-kms-test" and keys therein.

    Note: the above text blobs are service account credentials, including private keys. 
    _Never_ give any real priviledges to these accounts, as we are obviously exposing them here.

    User1 is assumed to have permissions to encrypt/decrypt using the given key
    User2 is assumed to _not_ have permissions to encrypt/decrypt using the given key, but permission to 
    impersonate User1.

    This test is parameterized with env vars:
    * ENABLE_GCP_TEST - set to non-zero (1/true) to run
    * GCP_USER_1_CREDENTIALS - set to credentials file for user1
    * GCP_USER_2_CREDENTIALS - set to credentials file for user2
    * GCP_KEY_NAME - set to <keychain>/<keyname> to override.
    * GCP_PROJECT_ID - set to test project
    * GCP_LOCATION - set to test location
*/

struct gcp_test_env {
    std::string key_name;
    std::string location;
    std::string project_id; 
    std::string user_1_creds;
    std::string user_2_creds;
};

static future<> gcp_test_helper(std::function<future<>(const tmpdir&, const gcp_test_env&)> f) {
    gcp_test_env env {
        .key_name = get_var_or_default("GCP_KEY_NAME", "test_ring/test_key"),
        .location = get_var_or_default("GCP_LOCATION", "global"),
        .project_id = get_var_or_default("GCP_PROJECT_ID", "scylla-kms-test"),
        .user_1_creds = get_var_or_default("GCP_USER_1_CREDENTIALS", ""),
        .user_2_creds = get_var_or_default("GCP_USER_2_CREDENTIALS", ""),
    };

    tmpdir tmp;

    if (env.user_1_creds.empty()) {
        BOOST_ERROR("No 'GCP_USER_1_CREDENTIALS' provided");
    }
    if (env.user_2_creds.empty()) {
        BOOST_ERROR("No 'GCP_USER_2_CREDENTIALS' provided");
    }

    co_await f(tmp, env);
}

SEASTAR_TEST_CASE(test_gcp_provider, *check_run_test_decorator("ENABLE_GCP_TEST")) {
    co_await gcp_test_helper([](const tmpdir& tmp, const gcp_test_env& gcp) -> future<> {
        auto yaml = fmt::format(R"foo(
            gcp_hosts:
                gcp_test:
                    master_key: {0}
                    gcp_project_id: {1}
                    gcp_location: {2}
                    gcp_credentials_file: {3}
                    )foo"
            , gcp.key_name, gcp.project_id, gcp.location, gcp.user_1_creds
        );

        co_await test_provider("'key_provider': 'GcpKeyProviderFactory', 'gcp_host': 'gcp_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    });
}

SEASTAR_TEST_CASE(test_gcp_provider_with_master_key_in_cf, *check_run_test_decorator("ENABLE_GCP_TEST")) {
    co_await gcp_test_helper([](const tmpdir& tmp, const gcp_test_env& gcp) -> future<> {
        auto yaml = fmt::format(R"foo(
            gcp_hosts:
                gcp_test:
                    gcp_project_id: {1}
                    gcp_location: {2}
                    gcp_credentials_file: {3}
                    )foo"
            , gcp.key_name, gcp.project_id, gcp.location, gcp.user_1_creds
        );

        // should fail
        try {
            try {
                co_await test_provider(
                    "'key_provider': 'GcpKeyProviderFactory', 'gcp_host': 'gcp_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128",
                    tmp,
                    yaml);
            } catch (std::nested_exception& ex) {
                std::rethrow_if_nested(ex);
            }
            BOOST_FAIL("Required an exception to be re-thrown");
        } catch (encryption::configuration_error&) {
            // EXPECTED
        } catch (...) {
            BOOST_FAIL(format("Unexpected exception: {}", std::current_exception()));
        }

        // should be ok
        co_await test_provider(fmt::format("'key_provider': 'GcpKeyProviderFactory', 'gcp_host': 'gcp_test', 'master_key': '{}', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", gcp.key_name)
            , tmp, yaml
            );
    });
}

/**
 * Verify that trying to access key materials with a user w/o permissions to encrypt/decrypt using cloudkms
 * fails.
*/
SEASTAR_TEST_CASE(test_gcp_provider_with_invalid_user, *check_run_test_decorator("ENABLE_GCP_TEST")) {
    co_await gcp_test_helper([](const tmpdir& tmp, const gcp_test_env& gcp) -> future<> {
        auto yaml = fmt::format(R"foo(
            gcp_hosts:
                gcp_test:
                    master_key: {0}
                    gcp_project_id: {1}
                    gcp_location: {2}
                    gcp_credentials_file: {3}
                    )foo"
            , gcp.key_name, gcp.project_id, gcp.location, gcp.user_2_creds
        );

        // should fail
        BOOST_REQUIRE_THROW(
            co_await test_provider("'key_provider': 'GcpKeyProviderFactory', 'gcp_host': 'gcp_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml)
            , std::exception
        );
    });
}

/**
 * Verify that impersonation of an allowed service account works. User1 can encrypt, but we run 
 * as User2. However, impersonating user1 will allow us do it ourselves.
*/
SEASTAR_TEST_CASE(test_gcp_provider_with_impersonated_user, *check_run_test_decorator("ENABLE_GCP_TEST")) {
    co_await gcp_test_helper([](const tmpdir& tmp, const gcp_test_env& gcp) -> future<> {
        auto buf = co_await read_text_file_fully(sstring(gcp.user_1_creds));
        auto json = rjson::parse(std::string_view(buf.begin(), buf.end()));
        auto user1 = rjson::get<std::string>(json, "client_email");

        auto yaml = fmt::format(R"foo(
            gcp_hosts:
                gcp_test:
                    master_key: {0}
                    gcp_project_id: {1}
                    gcp_location: {2}
                    gcp_credentials_file: {3}
                    gcp_impersonate_service_account: {4}
                    )foo"
            , gcp.key_name, gcp.project_id, gcp.location, gcp.user_2_creds, user1
        );

        co_await test_provider("'key_provider': 'GcpKeyProviderFactory', 'gcp_host': 'gcp_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    });
}

std::string make_aws_host(std::string_view aws_region, std::string_view service);

using scopts_map = std::unordered_map<std::string, std::string>;

static future<> test_broken_encrypted_commitlog(const test_provider_args& args, scopts_map scopts = {}) {
    std::string pk = "apa";
    std::string v = "ko";

    {
        auto [cfg, ext] = make_commitlog_config(args, scopts);

        cql_test_config cqlcfg(cfg);

        if (args.timeout) {
            cqlcfg.query_timeout = args.timeout;
        }

        co_await do_with_cql_env_thread([&] (cql_test_env& env) {
            do_create_and_insert(env, args, pk, v);
        }, cqlcfg, {}, cql_test_init_configurables{ *ext });
    }
}

/**
 * Tests that a network error in key resolution (in commitlog in this case) results in a non-fatal, non-isolating
 * exception, i.e. an eventual write error.
 */
static future<> network_error_test_helper(const tmpdir& tmp, const std::string& host, std::function<std::tuple<scopts_map, std::string>(const fake_proxy&)> make_opts) {
    fake_proxy proxy(host);
    std::exception_ptr p;
    try {
        auto [scopts, yaml] = make_opts(proxy);

        test_provider_args args{
            .tmp = tmp,
            .extra_yaml = yaml,
            .n_tables = 10, 
            .before_create_table = [&](auto& env) {
                // turn off proxy. all key resolution after this should fail
                proxy.enable(false);
                // wait for key cache expiry.
                seastar::sleep(10ms).get();
                // ensure commitlog will create a new segment on write -> eventual write failure
                env.db().invoke_on_all([](replica::database& db) {
                    return db.commitlog()->force_new_active_segment();
                }).get();
            },
            .on_insert_exception = [&](auto&&) {
                // once we get the exception we have to enable key resolution again, 
                // otherwise we can't shut down cql test env.
                proxy.enable(true);
            },
            .timeout = timeout_config{
                // set really low write timeouts so we get a failure (timeout)
                // when we fail to write to commitlog
                100ms, 100ms, 100ms, 100ms, 100ms, 100ms, 100ms
            },
        };

        BOOST_REQUIRE_THROW(
            co_await test_broken_encrypted_commitlog(args, scopts);
            , exceptions::mutation_write_timeout_exception
        );
    } catch (...) {
        p = std::current_exception();
    }

    co_await proxy.stop();

    if (p) {
        std::rethrow_exception(p);
    }
}

SEASTAR_TEST_CASE(test_kms_network_error, *check_run_test_decorator("ENABLE_KMS_TEST")) {
    co_await kms_test_helper([](const tmpdir& tmp, std::string_view kms_key_alias, std::string_view kms_aws_region, std::string_view kms_aws_profile) -> future<> {
        auto host = make_aws_host(kms_aws_region, "kms");

        co_await network_error_test_helper(tmp, host, [&](const auto& proxy) {
            auto yaml = fmt::format(R"foo(
                kms_hosts:
                    kms_test:
                        master_key: {0}
                        aws_region: {1}
                        aws_profile: {2}
                        endpoint: https://{3}
                        key_cache_expiry: 1ms
                        )foo"
                , kms_key_alias, kms_aws_region, kms_aws_profile, proxy.address()
            );
            return std::make_tuple(scopts_map({ { "key_provider", "KmsKeyProviderFactory" }, { "kms_host", "kms_test" } }), yaml);
        });
    });
}

#ifdef HAVE_KMIP

SEASTAR_TEST_CASE(test_kmip_network_error, *check_run_test_decorator("ENABLE_KMIP_TEST")) {
    co_await kmip_test_helper([](const kmip_test_info& info, const tmpdir& tmp) -> future<> {
        co_await network_error_test_helper(tmp, info.host, [&](const auto& proxy) {
            auto yaml = fmt::format(R"foo(
                kmip_hosts:
                    kmip_test:
                        hosts: {0}
                        certificate: {1}
                        keyfile: {2}
                        truststore: {3}
                        priority_string: {4}
                        key_cache_expiry: 1ms
                        )foo"
                , proxy.address(), info.cert, info.key, info.ca, info.prio
            );
            return std::make_tuple(scopts_map({ { "key_provider", "KmipKeyProviderFactory" }, { "kmip_host", "kmip_test" } }), yaml);
        });
    });
}

SEASTAR_TEST_CASE(test_kmip_provider_broken_config_on_restart, *check_run_test_decorator("ENABLE_KMIP_TEST", true)) {
    co_await kmip_test_helper([](const kmip_test_info& info, const tmpdir& tmp) -> future<> {
        auto yaml = fmt::format(R"foo(
            kmip_hosts:
                kmip_test:
                    hosts: {0}
                    certificate: {1}
                    keyfile: {2}
                    truststore: {3}
                    priority_string: {4}
                    )foo"
            , info.host, info.cert, info.key, info.ca, info.prio
        );

        bool past_create = false;

        test_provider_args args{
            .tmp = tmp,
            .options = "'key_provider': 'KmipKeyProviderFactory', 'kmip_host': 'kmip_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128",
            .extra_yaml = yaml,
            .n_tables = 1, 
            .n_restarts = 1, 
            .explicit_provider = {},
        };

        // After tables are created, and data inserted, remove EAR config
        // for the restart. This should cause us to fail creating the 
        // tables from schema tables, since the extension will throw.
        args.after_insert = [&](cql_test_env&) {
            past_create = true;
            args.extra_yaml = {};
        };

        BOOST_REQUIRE_THROW(
            co_await test_provider(args);
            , std::exception
        );

        BOOST_REQUIRE(past_create);
    });
}


SEASTAR_TEST_CASE(test_kmip_provider_broken_sstables_on_restart, *check_run_test_decorator("ENABLE_KMIP_TEST", true)) {
    co_await kmip_test_helper([](const kmip_test_info& info, const tmpdir& tmp) -> future<> {
        auto yaml = fmt::format(R"foo(
            kmip_hosts:
                kmip_test:
                    hosts: {0}
                    certificate: {1}
                    keyfile: {2}
                    truststore: {3}
                    priority_string: {4}
                    )foo"
            , info.host, info.cert, info.key, info.ca, info.prio
        );

        bool past_create = false;
        bool past_second_start = false;

        test_provider_args args{
            .tmp = tmp,
            .options = "'key_provider': 'KmipKeyProviderFactory', 'kmip_host': 'kmip_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128",
            .extra_yaml = yaml,
            .n_tables = 1, 
            .n_restarts = 1, 
            .explicit_provider = {},
        };

        // After data is inserted, flush all shards and alter the table
        // to no longer use EAR, then remove EAR config. This will result
        // in a schema that loads fine, but accessing the sstables will
        // throw.
        args.after_insert = [&](cql_test_env& env) {
            try {
                env.db().invoke_on_all([](replica::database& db) {
                    auto& cf = db.find_column_family("ks", "t0");
                    return cf.flush();
                }).get();
                env.execute_cql(fmt::format("alter table ks.t0 WITH scylla_encryption_options={{'key_provider': 'none'}}")).get();
            } catch (...) {
                testlog.error("Unexpected exception {}", std::current_exception());
                throw;
            }
            past_create = true;
            args.extra_yaml = {};
            args.options = {};
        };
        // If we get here, startup of second run was successful.
        args.before_verify = [&](cql_test_env& env) {
            past_second_start = true;
        };

        BOOST_REQUIRE_THROW(
            co_await test_provider(args);
            , std::exception
        );

        BOOST_REQUIRE(past_create);
        // We'd really want to be past this here, since "only" the sstables
        // on disk should mention unresolvable EAR stuff here. But scylla will
        // scan sstables on startup, and thus fail already there.
        // TODO: move and upload sstables?
        BOOST_REQUIRE(!past_second_start);
    });
}
#endif // HAVE_KMIP

// Note: cannot do the above test for gcp, because we can't use false endpoints there. Could mess with address resolution,
// but there is no infrastructure for that atm.

/*
    Simple test of Azure Key Provider.

    User1 is assumed to have permissions to wrap/unwrap using the given key.
    User2 is assumed to _not_ have permissions to wrap/unwrap using the given key.

    This test is parameterized with env vars:
    * ENABLE_AZURE_TEST - set to non-zero (1/true) to run Azure tests (enabled by default)
    * ENABLE_AZURE_TEST_REAL - set to non-zero (1/true) to run tests against real Azure services (disabled by default, same tests run against a local mock server regardless)
    * AZURE_TENANT_ID - the tenant where the principals live
    * AZURE_USER_1_CLIENT_ID - the client ID of user1
    * AZURE_USER_1_CLIENT_SECRET - the secret of user1
    * AZURE_USER_1_CLIENT_CERTIFICATE - the PEM-encoded certificate and private key of user1
    * AZURE_USER_2_CLIENT_ID - the client ID of user2
    * AZURE_USER_2_CLIENT_SECRET - the secret of user2
    * AZURE_USER_2_CLIENT_CERTIFICATE - the PEM-encoded certificate and private key of user2
    * AZURE_KEY_NAME - set to <vault_name>/<keyname>
*/

struct azure_test_env {
    std::string key_name;
    std::string tenant_id;
    std::string user_1_client_id;
    std::string user_1_client_secret;
    std::string user_1_client_certificate;
    std::string user_2_client_id;
    std::string user_2_client_secret;
    std::string user_2_client_certificate;
    std::string authority_host;
    std::string imds_endpoint;
};

static std::string get_mock_azure_addr() {
    return tests::getenv_safe("MOCK_AZURE_VAULT_SERVER_HOST");
}

static unsigned long get_mock_azure_port() {
    return std::stoul(tests::getenv_safe("MOCK_AZURE_VAULT_SERVER_PORT"));
}

static future<azure_test_env> get_mock_azure_env(const tmpdir& tmp) {
    co_return azure_test_env {
        .key_name = fmt::format("http://{}:{}/mock-key", get_mock_azure_addr(), get_mock_azure_port()),
        .tenant_id = "00000000-1111-2222-3333-444444444444",
        .user_1_client_id = "mock-client-id",
        .user_1_client_secret = "mock-client-secret",
        .user_1_client_certificate = "test/resource/certs/scylla.pem", // a cert file with valid format - the contents won't be checked by the mock server
        .user_2_client_id = "mock-client-id-invalid",
        .user_2_client_secret = "mock-client-secret-invalid",
        .user_2_client_certificate = "/dev/null", // a cert file with invalid format
        .authority_host = fmt::format("http://{}:{}", get_mock_azure_addr(), get_mock_azure_port()),
        .imds_endpoint = fmt::format("http://{}:{}", get_mock_azure_addr(), get_mock_azure_port()),
    };
}

static azure_test_env get_real_azure_env() {
    return azure_test_env {
        .key_name = get_var_or_default("AZURE_KEY_NAME", ""),
        .tenant_id = get_var_or_default("AZURE_TENANT_ID", ""),
        .user_1_client_id = get_var_or_default("AZURE_USER_1_CLIENT_ID", ""),
        .user_1_client_secret = get_var_or_default("AZURE_USER_1_CLIENT_SECRET", ""),
        .user_1_client_certificate = get_var_or_default("AZURE_USER_1_CLIENT_CERTIFICATE", ""),
        .user_2_client_id = get_var_or_default("AZURE_USER_2_CLIENT_ID", ""),
        .user_2_client_secret = get_var_or_default("AZURE_USER_2_CLIENT_SECRET", ""),
        .user_2_client_certificate = get_var_or_default("AZURE_USER_2_CLIENT_CERTIFICATE", ""),
        .authority_host = "''",
        .imds_endpoint = "''",
    };
}

static future<> azure_test_helper(std::function<future<>(const tmpdir&, const azure_test_env&)> f, bool real_server = false) {
    tmpdir tmp;

    auto env = real_server ? get_real_azure_env() : co_await get_mock_azure_env(tmp);

    if (real_server) {
        if (env.key_name.empty()) {
            BOOST_ERROR("No 'AZURE_KEY_NAME' provided");
        }
        if (env.tenant_id.empty()) {
            BOOST_ERROR("No 'AZURE_TENANT_ID' provided");
        }
        if (env.user_1_client_id.empty() || env.user_1_client_secret.empty() || env.user_1_client_certificate.empty()) {
            BOOST_ERROR("Missing or incompete credentials for user 1: All three of 'AZURE_USER_1_CLIENT_ID', 'AZURE_USER_1_CLIENT_SECRET' and 'AZURE_USER_1_CLIENT_CERTIFICATE' must be provided");
        }
        if (env.user_2_client_id.empty() || env.user_2_client_secret.empty() || env.user_2_client_certificate.empty()) {
            BOOST_ERROR("Missing or incompete credentials for user 2: All three of 'AZURE_USER_2_CLIENT_ID', 'AZURE_USER_2_CLIENT_SECRET' and 'AZURE_USER_2_CLIENT_CERTIFICATE' must be provided");
        }
    }

    co_await f(tmp, env);
}

static auto check_azure_mock_test_decorator() {
    return check_run_test_decorator("ENABLE_AZURE_TEST", true);
}

static auto check_azure_real_test_decorator() {
    return boost::unit_test::precondition([](boost::unit_test::test_unit_id){
        return check_run_test("ENABLE_AZURE_TEST", true)
            && check_run_test("ENABLE_AZURE_TEST_REAL", false);
    });
}

SEASTAR_TEST_CASE(test_azure_provider_with_imds, *check_azure_mock_test_decorator()) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    master_key: {0}
                    imds_endpoint: {1}
                    )foo"
            , azure.key_name, azure.imds_endpoint
        );

        co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    }, false);
}

static future<> do_test_azure_provider_with_secret(bool real_server) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    master_key: {0}
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    azure_client_secret: {3}
                    azure_authority_host: {5}
                    )foo"
            , azure.key_name, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.user_1_client_certificate, azure.authority_host
        );

        co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    }, real_server);
}

SEASTAR_TEST_CASE(test_azure_provider_with_secret, *check_azure_mock_test_decorator()) {
    co_await do_test_azure_provider_with_secret(false);
}

SEASTAR_TEST_CASE(test_azure_provider_with_secret_real, *check_azure_real_test_decorator()) {
    co_await do_test_azure_provider_with_secret(true);
}

static future<> do_test_azure_provider_with_certificate(bool real_server) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    master_key: {0}
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    azure_client_certificate_path: {4}
                    azure_authority_host: {5}
                    )foo"
            , azure.key_name, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.user_1_client_certificate, azure.authority_host
        );

        co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml);
    }, real_server);
}

SEASTAR_TEST_CASE(test_azure_provider_with_certificate, *check_azure_mock_test_decorator()) {
    co_await do_test_azure_provider_with_certificate(false);
}

SEASTAR_TEST_CASE(test_azure_provider_with_certificate_real, *check_azure_real_test_decorator()) {
    co_await do_test_azure_provider_with_certificate(true);
}

SEASTAR_TEST_CASE(test_azure_provider_with_master_key_in_cf, *check_azure_mock_test_decorator()) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    azure_client_secret: {3}
                    azure_authority_host: {5}
                    )foo"
            , azure.key_name, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.user_1_client_certificate, azure.authority_host
        );

        // should fail
        BOOST_REQUIRE_EXCEPTION(
            co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml)
            , exceptions::configuration_exception, [](const exceptions::configuration_exception& e) {
                try {
                    std::rethrow_if_nested(e);
                } catch (const encryption::configuration_error& inner) {
                    return sstring(inner.what()).find("No master key set") != sstring::npos;
                } catch (...) {
                    return false; // Unexpected nested exception type
                }
                return false; // No nested exception
            }
        );

        // should be ok
        co_await test_provider(fmt::format("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'master_key': '{}', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", azure.key_name)
            , tmp, yaml
            );
    }, false);
}

SEASTAR_TEST_CASE(test_azure_provider_with_no_host, *check_azure_mock_test_decorator()) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = R"foo(
            azure_hosts:
            )foo";

        // should fail
        BOOST_REQUIRE_EXCEPTION(
            co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml)
            , std::invalid_argument
            , [](const std::invalid_argument& e) {
                return sstring(e.what()).find("No such host") != sstring::npos;
            }
        );
    }, false);
}

/**
 * Verify that the Azure key provider throws if the provided Service Principal
 * credentials are incomplete. The provider will first fall back to the default
 * credentials source to detect credentials from the system (env vars, Azure CLI,
 * IMDS), and only after all these attempts fail will it throw.
 *
 * Note: Just in case we ever run these tests on Azure VMs, use a non-routable
 * IP address for the IMDS endpoint to ensure the connection will fail.
 */
SEASTAR_TEST_CASE(test_azure_provider_with_incomplete_creds, *check_azure_mock_test_decorator()) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    master_key: {0}
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    imds_endpoint: http://192.0.2.1:80
                    )foo"
            , azure.key_name, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.user_1_client_certificate
        );

        // should fail
        BOOST_REQUIRE_EXCEPTION(
            co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml)
            , permission_error, [](const encryption::permission_error& e) {
                try {
                    std::rethrow_if_nested(e);
                } catch (const azure::auth_error& inner) {
                    return sstring(inner.what()).find("No credentials found in any source.") != sstring::npos;
                } catch (...) {
                    return false; // Unexpected nested exception type
                }
                return false; // No nested exception
            }
        );
    }, false);
}

static future<> do_test_azure_provider_with_invalid_key(bool real_server) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto vault = azure.key_name.substr(0, azure.key_name.find_last_of('/'));
        auto master_key = fmt::format("{}/nonexistentkey", vault);
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    master_key: {0}
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    azure_client_secret: {3}
                    azure_authority_host: {5}
                    )foo"
            , master_key, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.user_1_client_certificate, azure.authority_host
        );

        // should fail
        BOOST_REQUIRE_EXCEPTION(
            co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml)
            , service_error, [](const service_error& e) {
                try {
                    std::rethrow_if_nested(e);
                } catch (const encryption::vault_error& inner) {
                    // Both error codes are valid depending on the scope of the role assignment:
                    // - "Forbidden": key-scoped permissions
                    // - "KeyNotFound": vault-scoped permissions
                    return inner.code() == "Forbidden" || inner.code() == "KeyNotFound";
                } catch (...) {
                    return false; // Unexpected nested exception type
                }
                return false; // No nested exception
            }
        );
    }, real_server);
}

SEASTAR_TEST_CASE(test_azure_provider_with_invalid_key, *check_azure_mock_test_decorator()) {
    co_await do_test_azure_provider_with_invalid_key(false);
}

SEASTAR_TEST_CASE(test_azure_provider_with_invalid_key_real, *check_azure_real_test_decorator()) {
    co_await do_test_azure_provider_with_invalid_key(true);
}

/**
 * Verify that trying to access key materials with a user w/o permissions to wrap/unwrap using vault
 * fails.
 */
static future<> do_test_azure_provider_with_invalid_user(bool real_server) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    master_key: {0}
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    azure_client_secret: {3}
                    azure_authority_host: {5}
                    )foo"
            , azure.key_name, azure.tenant_id, azure.user_2_client_id, azure.user_2_client_secret, azure.user_2_client_certificate, azure.authority_host
        );

        // should fail
        BOOST_REQUIRE_EXCEPTION(
            co_await test_provider("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", tmp, yaml)
            , service_error, [](const service_error& e) {
                try {
                    std::rethrow_if_nested(e);
                } catch (const encryption::vault_error& inner) {
                    return inner.code() == "Forbidden";
                } catch (...) {
                    return false; // Unexpected nested exception type
                }
                return false; // No nested exception
            }
        );
    }, real_server);
}

SEASTAR_TEST_CASE(test_azure_provider_with_invalid_user, *check_azure_mock_test_decorator()) {
    co_await do_test_azure_provider_with_invalid_user(false);
}

SEASTAR_TEST_CASE(test_azure_provider_with_invalid_user_real, *check_azure_real_test_decorator()) {
    co_await do_test_azure_provider_with_invalid_user(true);
}

/**
 * Verify that the secret has higher precedence that the certificate.
 * Use the wrong user's certificate to make sure it causes the test to fail.
 */
static future<> do_test_azure_provider_with_both_secret_and_cert(bool real_server) {
    co_await azure_test_helper([](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto yaml = fmt::format(R"foo(
            azure_hosts:
                azure_test:
                    azure_tenant_id: {1}
                    azure_client_id: {2}
                    azure_client_secret: {3}
                    azure_client_certificate_path: {4}
                    azure_authority_host: {5}
                    )foo"
            , azure.key_name, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.user_2_client_certificate, azure.authority_host
        );

        // should be ok
        co_await test_provider(fmt::format("'key_provider': 'AzureKeyProviderFactory', 'azure_host': 'azure_test', 'master_key': '{}', 'cipher_algorithm':'AES/CBC/PKCS5Padding', 'secret_key_strength': 128", azure.key_name)
            , tmp, yaml
            );
    }, real_server);
}

SEASTAR_TEST_CASE(test_azure_provider_with_both_secret_and_cert, *check_azure_mock_test_decorator()) {
    co_await do_test_azure_provider_with_both_secret_and_cert(false);
}

SEASTAR_TEST_CASE(test_azure_provider_with_both_secret_and_cert_real, *check_azure_real_test_decorator()) {
    co_await do_test_azure_provider_with_both_secret_and_cert(true);
}

SEASTAR_TEST_CASE(test_azure_network_error, *check_azure_mock_test_decorator()) {
    co_await azure_test_helper([&](const tmpdir& tmp, const azure_test_env& azure) -> future<> {
        auto host_endpoint = fmt::format("{}:{}", get_mock_azure_addr(), get_mock_azure_port());
        auto key = azure.key_name.substr(azure.key_name.find_last_of('/') + 1);
        co_await network_error_test_helper(tmp, host_endpoint, [&](const auto& proxy) {
            auto yaml = fmt::format(R"foo(
                azure_hosts:
                    azure_test:
                        master_key: http://{0}/{1}
                        azure_tenant_id: {2}
                        azure_client_id: {3}
                        azure_client_secret: {4}
                        azure_authority_host: {5}
                        key_cache_expiry: 1ms
                        )foo"
                , proxy.address(), key, azure.tenant_id, azure.user_1_client_id, azure.user_1_client_secret, azure.authority_host
            );
            return std::make_tuple(scopts_map({ { "key_provider", "AzureKeyProviderFactory" }, { "azure_host", "azure_test" } }), yaml);
        });
    }, false);
}

/*
 * Utility function to spawn a dedicated mock server instance for a particular test case.
 * Useful for tests that require error injection, where the server's global state needs to be configured accordingly.
 * Since test.py may run tests in parallel, using the global server instance is not safe for such tests.
 *
 * The code was based on `kmip_test_helper()`.
 */
static future<> with_dedicated_azure_mock_server(const std::function<future<>(std::string host, unsigned int port)>& f) {
    tmpdir tmp;

    auto pyexec = tests::proc::find_file_in_path("python");

    promise<std::pair<std::string, int>> authority_promise;
    auto fut = authority_promise.get_future();

    BOOST_TEST_MESSAGE("Starting dedicated Azure Vault mock server");

    auto python = co_await tests::proc::process_fixture::create(pyexec,
        { // args
            pyexec.string(),
            "test/pylib/start_azure_vault_mock.py",
            "--log-level", "INFO",
            "--host", get_var_or_default("MOCK_AZURE_VAULT_SERVER_HOST", "127.0.0.1"),
            "--port", "0", // random port
        },
        // env
        {},
        // stdout handler
        tests::proc::process_fixture::create_copy_handler(std::cout),
        // stderr handler
        [authority_promise = std::move(authority_promise), b = false](std::string_view line) mutable -> future<consumption_result<char>> {
            static std::regex authority_ex(R"foo(Starting Azure Vault mock server on \('([\d\.]+)', (\d+)\))foo");

            std::cerr << line << std::endl;
            std::match_results<typename std::string_view::const_iterator> m;
            if (!b && std::regex_search(line.begin(), line.end(), m, authority_ex)) {
                authority_promise.set_value(std::make_pair(m[1].str(), std::stoi(m[2].str())));
                BOOST_TEST_MESSAGE("Matched Azure Vault host and port: " + m[1].str() + ":" + m[2].str());
                b = true;
            }
            co_return continue_consuming{};
        }
    );

    std::exception_ptr ep;

    try {
        // arbitrary timeout of 20s for the server to make some output. Very generous.
        auto [host, port] = co_await with_timeout(std::chrono::steady_clock::now() + 20s, std::move(fut));

        // wait for port.
        auto sleep_interval = 100ms;
        auto timeout = 5s;
        auto end_time = seastar::lowres_clock::now() + timeout;
        bool connected = false;
        while (seastar::lowres_clock::now() < end_time) {
            BOOST_TEST_MESSAGE(fmt::format("Connecting to {}:{}", host, port));
            try {
                // TODO: seastar does not have a connect with timeout. That would be helpful here. But alas...
                co_await seastar::connect(socket_address(net::inet_address(host), uint16_t(port)));
                BOOST_TEST_MESSAGE("Dedicated Azure Vault mock server up and available");
                connected = true;
                break;
            } catch (...) {
            }
            co_await sleep(sleep_interval);
        }

        if (!connected) {
            throw std::runtime_error(fmt::format("Timed out connecting to Azure Vault mock server at {}:{}", host, port));
        }

        co_await f(host, port);

    } catch (timed_out_error&) {
        ep = std::make_exception_ptr(std::runtime_error("Could not start dedicated Azure Vault mock server"));
    } catch (...) {
        ep = std::current_exception();
    }

    BOOST_TEST_MESSAGE("Stopping dedicated Azure Vault mock server");

    python.terminate();
    co_await python.wait();

    if (ep) {
        std::rethrow_exception(ep);
    }
}

static future<> configure_azure_mock_server(const std::string& host, const unsigned int port, const std::string& service, const std::string& error_type, int repeat) {
    auto cln = http::experimental::client(socket_address(net::inet_address(host), uint16_t(port)));
    auto close_client = deferred_close(cln);
    auto req = http::request::make("POST", host, "/config/error");
    req._headers["Content-Length"] = "0";
    req.query_parameters["service"] = service;
    req.query_parameters["error_type"] = error_type;
    req.query_parameters["repeat"] = std::to_string(repeat);
    co_await cln.make_request(std::move(req), [](const http::reply&, input_stream<char>&&) -> future<> { return seastar::make_ready_future(); });
}

SEASTAR_TEST_CASE(test_imds, *check_azure_mock_test_decorator()) {
    co_await with_dedicated_azure_mock_server([](std::string host, unsigned int port) -> future<> {
        // Create new credential object for each test case because it caches the token.
        {
            testlog.info("Testing IMDS success path");
            azure::managed_identity_credentials creds { fmt::format("{}:{}", host, port) };
            co_await creds.get_access_token("https://vault.azure.net/.default");
        }

        {
            testlog.info("Testing IMDS transient errors");
            azure::managed_identity_credentials creds { fmt::format("{}:{}", host, port) };
            co_await configure_azure_mock_server(host, port, "imds", "InternalError", 1);
            // expected to not throw
            co_await creds.get_access_token("https://vault.azure.net/.default");
        }

        {
            testlog.info("Testing IMDS non-transient errors");
            azure::managed_identity_credentials creds { fmt::format("{}:{}", host, port) };
            co_await configure_azure_mock_server(host, port, "imds", "NoIdentity", 1);
            BOOST_REQUIRE_THROW(
                co_await creds.get_access_token("https://vault.azure.net/.default"),
                azure::creds_auth_error
            );
        }
    });
}

SEASTAR_TEST_CASE(test_entra_sts, *check_azure_mock_test_decorator()) {
    co_await with_dedicated_azure_mock_server([](std::string host, unsigned int port) -> future<> {
        auto make_entra_creds = [&] {
            return azure::service_principal_credentials {
                "00000000-1111-2222-3333-444444444444",
                "mock-client-id",
                "mock-client-secret",
                "",
                fmt::format("http://{}:{}", host, port),
             };
        };

        // Create new credential object for each test case because it caches the token.
        {
            testlog.info("Testing Entra STS success path");
            auto creds = make_entra_creds();
            co_await creds.get_access_token("https://vault.azure.net/.default");
        }

        {
            testlog.info("Testing Entra STS transient errors");
            auto creds = make_entra_creds();
            co_await configure_azure_mock_server(host, port, "entra", "TemporarilyUnavailable", 1);
            // expected to not throw
            co_await creds.get_access_token("https://vault.azure.net/.default");
        }

        {
            testlog.info("Testing Entra STS non-transient errors");
            auto creds = make_entra_creds();
            co_await configure_azure_mock_server(host, port, "entra", "InvalidSecret", 1);
            BOOST_REQUIRE_THROW(
                co_await creds.get_access_token("https://vault.azure.net/.default"),
                azure::creds_auth_error
            );
        }
    });
}

SEASTAR_TEST_CASE(test_azure_host, *check_azure_mock_test_decorator()) {
    co_await with_dedicated_azure_mock_server([](std::string host, unsigned int port) -> future<> {
        key_info kinfo { .alg = "AES/CBC/PKCS5Padding", .len = 128};
        azure_host::host_options options {
            .imds_endpoint = fmt::format("http://{}:{}", host, port),
            .master_key = fmt::format("http://{}:{}/test-key", host, port),
        };

        {
            testlog.info("Testing Key Vault success path");
            azure_host azhost {"azure_test", options};
            co_await azhost.get_or_create_key(kinfo, nullptr);
        }

        {
            testlog.info("Testing Key Vault transient errors");
            azure_host azhost {"azure_test", options};
            co_await configure_azure_mock_server(host, port, "vault", "Throttled", 1);
            co_await azhost.get_or_create_key(kinfo, nullptr);
        }

        {
            testlog.info("Testing Key Vault non-transient errors");
            azure_host azhost {"azure_test", options};
            co_await configure_azure_mock_server(host, port, "vault", "Forbidden", 1);
            BOOST_REQUIRE_THROW(
                co_await azhost.get_or_create_key(kinfo, nullptr),
                encryption::service_error
            );
        }
    });
}
