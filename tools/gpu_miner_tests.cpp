// Standalone offline suite. Includes the CLI to exercise the production session with injected I/O.
#define main gpu_miner_cli_main
#include "gpu_miner.cpp"
#undef main

#include <cmath>

namespace {
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
gm::DeviceInfo device(int id, gm::Backend backend = gm::Backend::Cuda)
{
    gm::DeviceInfo d;
    d.id = id;
    d.backend = backend;
    d.name = "offline identical GPU";
    d.available = true;
    d.pci_bus_id = gm::pci_identity(0, id + 1, 0);
    return d;
}
struct Capture {
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf(output.rdbuf());
    ~Capture() { std::cout.rdbuf(previous); }
    std::vector<json> events() const {
        std::vector<json> result;
        std::istringstream lines(output.str());
        std::string line;
        while (std::getline(lines, line)) if (!line.empty()) result.push_back(json::parse(line));
        return result;
    }
};
template<class Operation>
void expect_failure(gm::Failure category, Operation operation)
{
    try { operation(); } catch (const gm::FailureError& e) { check(e.category == category, "wrong failure category"); return; }
    throw std::runtime_error("expected failure");
}

void test_stats()
{
    const auto start = gm::Clock::time_point{};
    gm::SessionStats stats(start);
    auto a = device(0), b = device(1);
    stats.begin_job();
    stats.observe({a, b}, {100, 200});
    stats.observe({a, b}, {150, 300});
    stats.observe({a, b}, {150, 300});
    stats.begin_job();
    stats.observe({b, a}, {50, 25});
    // Renumbering must retain the stable identity, not create a new counter.
    a.id = 7;
    stats.observe({a, b}, {50, 100});
    const auto snapshot = stats.snapshot(start + std::chrono::seconds(2));
    check(snapshot["hashes"] == 600, "cumulative session total");
    check(snapshot["devices"][0]["hashes"] == 200, "cumulative device A");
    check(snapshot["devices"][1]["hashes"] == 400, "cumulative device B");
    check(snapshot["hashrate"] == 300.0, "shared total elapsed window");
    check(snapshot["devices"][0]["hashrate"] == 100.0, "shared device elapsed window");
    check(snapshot["devices"][1]["hashrate"] == 200.0, "shared device B elapsed window");
    stats.begin_job();
    stats.observe({device(0, gm::Backend::OpenCl)}, {10});
    check(stats.snapshot(start)["devices"].size() == 3, "backend namespaces must not merge");
    check(stats.snapshot(start)["hashrate"] == 0.0, "zero elapsed rate");
    stats.reset(start + std::chrono::seconds(5));
    check(stats.hashes() == 0 && stats.snapshot(start)["devices"].empty(), "session reset");
    check(stats.snapshot(start)["elapsed_ms"] == 0, "negative elapsed clamp");
    stats.observe({a}, {5});
    try { stats.observe({a}, {4}); throw std::logic_error("decreasing counters accepted"); }
    catch (const std::runtime_error& e) { check(std::string(e.what()) == "GPU job counters decreased", "counter validation"); }
}

void test_retry()
{
    gm::RetryPolicy policy;
    for (int delay : {1000, 2000, 4000, 8000, 16000, 30000, 30000})
        check(policy.next_delay_ms() == delay, "retry exponential cap");
    policy.reset();
    check(policy.next_delay_ms() == 1000 && policy.attempt() == 1, "retry reset");
    int calls = 0;
    std::vector<int> delays;
    auto operation = [&] {
        if (++calls < 4) throw gm::FailureError(gm::Failure::Transient, "offline unavailable");
        return 42;
    };
    check(gm::retry_rpc(operation, true, [] { return false; },
        [](const auto&, unsigned, int) {}, [&](int ms) { delays.push_back(ms); return true; }) == 42, "retry recovery");
    check(delays == std::vector<int>({1000, 2000, 4000}), "retry scheduling");
    for (auto category : {gm::Failure::Auth, gm::Failure::Corrupt, gm::Failure::Device, gm::Failure::Rpc}) {
        calls = 0;
        expect_failure(category, [&] {
            gm::retry_rpc([&]() -> int { ++calls; throw gm::FailureError(category, "fatal"); }, true,
                [] { return false; }, [](const auto&, unsigned, int) {}, [](int) -> bool { throw std::runtime_error("fatal retried"); });
        });
        check(calls == 1, "fatal error must not retry");
    }
    expect_failure(gm::Failure::Transient, [&] {
        gm::retry_rpc([]() -> int { throw gm::FailureError(gm::Failure::Transient, "finite"); }, false,
            [] { return false; }, [](const auto&, unsigned, int) {}, [](int) { return true; });
    });
    bool stopped = false;
    try {
        gm::retry_rpc([]() -> int { throw gm::FailureError(gm::Failure::Transient, "retry cancel"); }, true,
            [&] { return stopped; }, [](const auto&, unsigned, int) {}, [&](int) { stopped = true; return false; });
        throw std::logic_error("retry cancellation missing");
    } catch (const gm::Cancelled&) {}
    auto now = gm::Clock::time_point{};
    int sleeps = 0;
    check(!gm::wait_cancelable(30000, [&] { return sleeps == 3; }, [&] { return now; },
        [&](auto duration) { now += duration; ++sleeps; }), "cancelable backoff");
    check(now.time_since_epoch() == std::chrono::milliseconds(150), "backoff cancellation latency");
}

void test_rpc_errors()
{
    check(gm::decode_rpc_result(200, R"({"result":null,"error":null})", "submitblock").is_null(), "null RPC error compatible");
    expect_failure(gm::Failure::Auth, [] { gm::decode_rpc_result(401, "not JSON", "getblocktemplate"); });
    expect_failure(gm::Failure::Auth, [] { gm::decode_rpc_result(403, "", "getblocktemplate"); });
    expect_failure(gm::Failure::Transient, [] { gm::decode_rpc_result(503, "", "getblocktemplate"); });
    expect_failure(gm::Failure::Transient, [] { gm::decode_rpc_result(500, R"({"error":{"code":-28}})", "getblocktemplate"); });
    expect_failure(gm::Failure::Auth, [] { gm::decode_rpc_result(500, R"({"error":{"code":-2}})", "getblocktemplate"); });
    expect_failure(gm::Failure::Rpc, [] { gm::decode_rpc_result(500, R"({"error":{"code":-32602}})", "getblocktemplate"); });
    for (const auto* body : {"{", "[]", "{}", R"({"error":"broken"})"})
        expect_failure(gm::Failure::Corrupt, [&] { gm::decode_rpc_result(200, body, "getblocktemplate"); });
    try {
        gm::cancellable_http_post("http://127.0.0.1:1/", "fixture", "fixture", "{}", [] { return true; });
        throw std::logic_error("transport cancellation missing");
    } catch (const gm::Cancelled&) {}
}

void test_control()
{
    gm::StdinControl control(false);
    control.consume("unknown\nsto");
    check(!control.cancelled(), "partial stop command");
    control.consume("p\r\n");
    check(control.cancelled(), "split CRLF stop command");
    gm::StdinControl other(false);
    other.consume("cancel\n");
    check(other.cancelled(), "cancel alias");
    gm::MiningJob job;
    check(gm::launch_attempt_limit(job) == job.intensity, "finite CLI launch unchanged");
    job.cancel_requested = [] { return true; };
    check(gm::job_cancelled(job), "backend cancellation callback");
    check(gm::launch_attempt_limit(job) == 67108864, "cooperative launch bounded");
    gm::LaunchSizer sizing(job);
    check(sizing.attempts() == 262144, "unknown-device calibration launch");
    for (int i = 0; i < 10; ++i) sizing.completed(sizing.attempts(), std::chrono::microseconds(100));
    check(sizing.attempts() == 67108864, "fast-device adaptive cap");
    sizing.completed(67108864, std::chrono::seconds(1));
    check(sizing.attempts() == 6710886, "slow-device adaptive reduction");
    job.max_launch_attempts = 1024;
    check(gm::launch_attempt_limit(job) == 1024, "explicit launch cap");
    gm::NonceCursor first(10, 0, 2), second(10, 1, 2);
    check(first.take(4) == 10 && first.take(16) == 18 && first.take(2) == 50, "adaptive nonce continuity");
    check(second.take(8) == 11 && second.take(2) == 27, "independent multi-device nonce lanes");
    for (auto state : {gm::State::Starting, gm::State::Running, gm::State::Retrying,
        gm::State::Stopping, gm::State::Stopped, gm::State::Error}) {
        const auto event = gm::state_event(state, "cuda");
        check(event["event"] == "state" && event["name"] == gm::state_name(state), "Qt state schema");
    }
}

void test_sessions()
{
    for (bool continuous : {false, true}) {
        Options opts;
        opts.stats_json = true;
        opts.continuous = continuous;
        opts.blocks = 2;
        gm::StdinControl control(false);
        int jobs = 0, templates = 0;
        Capture capture;
        try {
            run_session(opts, gm::Backend::Cuda, control,
                [&](const std::string& method, const json&) -> json {
                    if (method == "submitblock") return nullptr;
                    if (++templates == 5) { control.request_stop(); throw gm::Cancelled(); }
                    return {{"bits", uint32_t{0x207fffff}}, {"blockhex", std::string(248, '0')}, {"height", templates}};
                }, [&](const gm::MiningJob& job) {
                    ++jobs;
                    gm::MiningResult result;
                    result.backend = "cuda";
                    result.devices = {device(0), device(1)};
                    result.device_hashes = {100, 200};
                    result.hashes = 300;
                    result.found = jobs != 1;
                    result.timed_out = !result.found;
                    if (result.timed_out) result.error = "cuda mining timeout";
                    job.progress_callback("cuda", 150, result.devices, {50, 100});
                    job.progress_callback("cuda", 300, result.devices, result.device_hashes);
                    return result;
                });
            check(!continuous, "continuous session stopped at block count");
        } catch (const gm::Cancelled&) { check(continuous, "finite session cancelled"); }
        check(jobs == (continuous ? 4 : 3), "template refresh / finite accepted limit");
        uint64_t last_hashes = 0;
        for (const auto& event : capture.events()) {
            if (event["event"] != "status") continue;
            const auto hashes = event["hashes"].get<uint64_t>();
            check(hashes >= last_hashes, "session counters reset across blocks");
            check(hashes == event["devices"][0]["hashes"].get<uint64_t>() + event["devices"][1]["hashes"].get<uint64_t>(), "total matches per-device sum");
            check(event["devices"][0].contains("pci_bus_id") && event["devices"][0].contains("uuid"), "device identity schema");
            last_hashes = hashes;
        }
        check(last_hashes == static_cast<uint64_t>(jobs * 300), "final callback not double counted");
    }
    Options opts;
    gm::StdinControl control(false);
    expect_failure(gm::Failure::Corrupt, [&] {
        run_session(opts, gm::Backend::Cuda, control, [](const auto&, const auto&) -> json {
            return {{"bits", uint32_t{1}}, {"blockhex", "00"}};
        }, [](const auto&) -> gm::MiningResult { throw std::runtime_error("corrupt template mined"); });
    });
    expect_failure(gm::Failure::Device, [&] {
        run_session(opts, gm::Backend::Cuda, control, [](const auto&, const auto&) -> json {
            return {{"bits", uint32_t{1}}, {"blockhex", std::string(248, '0')}};
        }, [](const auto&) { gm::MiningResult r; r.error = "driver timeout fatal"; return r; });
    });
    for (bool benchmark : {false, true}) {
        opts.benchmark = benchmark;
        opts.control_stdin = true;
        gm::StdinControl finite_control(false);
        try {
            run_session(opts, gm::Backend::Cuda, finite_control, [](const auto&, const auto&) -> json {
                return {{"bits", uint32_t{1}}, {"blockhex", std::string(248, '0')}};
            }, [&](const gm::MiningJob& job) {
                check(static_cast<bool>(job.cancel_requested), "finite/benchmark cancellation attached");
                finite_control.request_stop();
                check(job.cancel_requested(), "finite/benchmark cancellation observed");
                gm::MiningResult result;
                result.cancelled = true;
                return result;
            });
            throw std::logic_error("finite cancellation missing");
        } catch (const gm::Cancelled&) {}
    }
}
} // namespace

// No real backend is linked into this test executable.
namespace geocoin::tools::gpu_miner {
std::vector<DeviceInfo> list_cuda_devices() { return {device(0)}; }
std::vector<DeviceInfo> list_hip_devices() { return {}; }
std::vector<DeviceInfo> list_opencl_devices() { return {}; }
MiningResult mine_cuda(const MiningJob& job, const std::vector<int>&) {
    while (!job_cancelled(job)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    MiningResult result;
    result.cancelled = true;
    result.backend = "cuda";
    return result;
}
MiningResult mine_hip(const MiningJob&, const std::vector<int>&) { throw std::runtime_error("unexpected HIP"); }
MiningResult mine_opencl(const MiningJob&, const std::vector<int>&) { throw std::runtime_error("unexpected OpenCL"); }
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--hang-fixture") {
        std::cout << gm::state_event(gm::State::Running, "cuda").dump() << std::endl;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (argc == 2 && std::string(argv[1]) == "--crash-fixture") {
        auto state = gm::state_event(gm::State::Error, "cuda", "offline fixture crash");
        state["category"] = "device";
        std::cout << state.dump() << '\n'
            << json{{"event", "error"}, {"category", "device"}, {"message", "offline fixture crash"}}.dump() << std::endl;
        return 7;
    }
    if (argc == 3 && std::string(argv[1]) == "--rpc-fixture") {
        char name[] = "offline-miner", continuous[] = "--continuous", control[] = "--control-stdin", stats[] = "--stats-json";
        char url[] = "--url", user[] = "--rpc-user", username[] = "offline-fixture", pass[] = "--rpc-pass", password[] = "offline-fixture";
        char* args[] = {name, continuous, control, stats, url, argv[2], user, username, pass, password};
        return gpu_miner_cli_main(10, args);
    }
    if (argc > 1 && std::string(argv[1]) == "--stdin-fixture") {
        char name[] = "offline-miner", benchmark[] = "--benchmark", control[] = "--control-stdin", stats[] = "--stats-json";
        char* args[] = {name, benchmark, control, stats};
        return gpu_miner_cli_main(4, args);
    }
    try {
        test_stats();
        test_retry();
        test_rpc_errors();
        test_control();
        test_sessions();
        std::cout << "GPU offline stats/retry/reset/cancel/session/schema tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU offline test failed: " << error.what() << "\n";
        return 1;
    }
}
