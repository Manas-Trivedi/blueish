#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <netdb.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

enum : uint8_t {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 1234;
    size_t connections = 16;
    size_t requests = 20000;
    size_t warmup = 2000;
    size_t key_count = 4096;
    size_t value_size = 64;
    std::string server_cmd;
    bool compare_redis = false;
    std::string redis_host = "127.0.0.1";
    uint16_t redis_port = 6379;
    std::string redis_benchmark_bin = "redis-benchmark";
};

struct PhaseResult {
    std::string name;
    size_t completed = 0;
    size_t errors = 0;
    double seconds = 0.0;
    double ops_per_sec = 0.0;
    double avg_us = 0.0;
    double min_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
};

struct RedisComparison {
    bool available = false;
    double set_ops = 0.0;
    double get_ops = 0.0;
    std::string error;
};

enum class PhaseKind {
    SetInsert,
    SetOverwrite,
    GetHit,
    DelHit,
};

struct WorkerResult {
    std::vector<double> latencies_us;
    size_t completed = 0;
    size_t errors = 0;
};

struct SharedFailure {
    std::mutex mutex;
    std::exception_ptr error;

    void capture() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!error) {
            error = std::current_exception();
        }
    }
};

struct ProcessGuard {
    pid_t pid = -1;

    ~ProcessGuard() {
        stop();
    }

    void stop() {
        if (pid <= 0) {
            return;
        }
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, nullptr, 0);
        pid = -1;
    }
};

[[noreturn]] void die(const std::string &message) {
    throw std::runtime_error(message);
}

int32_t write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rv == 0) {
            return -1;
        }
        buf += rv;
        n -= (size_t)rv;
    }
    return 0;
}

int32_t read_full(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rv == 0) {
            return -1;
        }
        buf += rv;
        n -= (size_t)rv;
    }
    return 0;
}

std::vector<uint8_t> encode_request(const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &arg : cmd) {
        len += 4 + (uint32_t)arg.size();
    }

    std::vector<uint8_t> out(4 + len);
    memcpy(out.data(), &len, 4);

    uint32_t nargs = (uint32_t)cmd.size();
    memcpy(out.data() + 4, &nargs, 4);

    size_t cur = 8;
    for (const std::string &arg : cmd) {
        uint32_t arg_len = (uint32_t)arg.size();
        memcpy(out.data() + cur, &arg_len, 4);
        cur += 4;
        memcpy(out.data() + cur, arg.data(), arg.size());
        cur += arg.size();
    }

    return out;
}

uint8_t round_trip(int fd, const std::vector<std::string> &cmd) {
    std::vector<uint8_t> req = encode_request(cmd);
    if (write_all(fd, req.data(), req.size()) != 0) {
        die("write() failed while sending benchmark request");
    }

    uint32_t len = 0;
    if (read_full(fd, reinterpret_cast<uint8_t *>(&len), 4) != 0) {
        die("read() failed while reading response header");
    }

    std::vector<uint8_t> body(len);
    if (len > 0 && read_full(fd, body.data(), len) != 0) {
        die("read() failed while reading response body");
    }
    if (body.empty()) {
        die("malformed response from server");
    }
    return body[0];
}

int connect_socket(const std::string &host, uint16_t port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    std::string port_str = std::to_string(port);
    int rv = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rv != 0) {
        die(std::string("getaddrinfo() failed: ") + gai_strerror(rv));
    }

    int fd = -1;
    for (struct addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        die("connect() failed; start the server or pass --server-cmd");
    }
    return fd;
}

bool can_connect(const std::string &host, uint16_t port) {
    try {
        int fd = connect_socket(host, port);
        close(fd);
        return true;
    } catch (...) {
        return false;
    }
}

std::string make_key(const std::string &prefix, size_t index) {
    return prefix + ":" + std::to_string(index);
}

std::string make_value(size_t value_size, size_t index) {
    std::string value;
    value.reserve(value_size);
    for (size_t i = 0; i < value_size; ++i) {
        value.push_back((char)('a' + ((index + i) % 26)));
    }
    return value;
}

void wait_for_server(const Options &options) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        if (can_connect(options.host, options.port)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    die("server did not become ready in time");
}

ProcessGuard launch_server_if_requested(const Options &options) {
    ProcessGuard guard;
    if (options.server_cmd.empty()) {
        return guard;
    }

    if (can_connect(options.host, options.port)) {
        return guard;
    }

    pid_t pid = fork();
    if (pid < 0) {
        die("fork() failed while launching server");
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execl("/bin/sh", "sh", "-c", options.server_cmd.c_str(), (char *)nullptr);
        _exit(127);
    }

    guard.pid = pid;
    wait_for_server(options);
    return guard;
}

void run_unmeasured_set_load(
    const Options &options,
    const std::string &prefix,
    size_t total_keys,
    size_t connections
) {
    std::atomic<size_t> next_index{0};
    std::atomic<bool> stop{false};
    SharedFailure failure;
    std::vector<std::thread> threads;
    threads.reserve(connections);

    for (size_t i = 0; i < connections; ++i) {
        threads.emplace_back([&, i]() {
            int fd = -1;
            try {
                fd = connect_socket(options.host, options.port);
                while (!stop.load()) {
                    size_t index = next_index.fetch_add(1);
                    if (index >= total_keys) {
                        break;
                    }
                    std::vector<std::string> cmd = {
                        "set",
                        make_key(prefix, index),
                        make_value(options.value_size, index),
                    };
                    uint8_t tag = round_trip(fd, cmd);
                    if (tag == TAG_ERR) {
                        die("server returned error during preload");
                    }
                }
            } catch (...) {
                stop.store(true);
                failure.capture();
            }
            if (fd >= 0) {
                close(fd);
            }
        });
    }

    for (std::thread &thread : threads) {
        thread.join();
    }

    if (failure.error) {
        std::rethrow_exception(failure.error);
    }
}

std::vector<std::string> build_command(
    PhaseKind kind,
    const std::string &prefix,
    size_t op_index,
    size_t key_count,
    size_t value_size
) {
    if (kind == PhaseKind::SetInsert) {
        return {"set", make_key(prefix, op_index), make_value(value_size, op_index)};
    }

    size_t key_index = key_count == 0 ? 0 : (op_index % key_count);
    if (kind == PhaseKind::SetOverwrite) {
        return {"set", make_key(prefix, key_index), make_value(value_size, op_index)};
    }
    if (kind == PhaseKind::GetHit) {
        return {"get", make_key(prefix, key_index)};
    }
    return {"del", make_key(prefix, op_index)};
}

PhaseResult run_phase(
    const Options &options,
    PhaseKind kind,
    const std::string &name,
    const std::string &prefix,
    size_t total_ops,
    size_t key_count
) {
    std::atomic<size_t> next_index{0};
    std::atomic<bool> stop{false};
    SharedFailure failure;
    std::vector<WorkerResult> worker_results(options.connections);
    std::vector<std::thread> threads;
    threads.reserve(options.connections);

    const auto started = Clock::now();
    for (size_t worker = 0; worker < options.connections; ++worker) {
        threads.emplace_back([&, worker]() {
            WorkerResult &result = worker_results[worker];
            result.latencies_us.reserve((total_ops / options.connections) + 1);

            int fd = -1;
            try {
                fd = connect_socket(options.host, options.port);
                while (!stop.load()) {
                    size_t op_index = next_index.fetch_add(1);
                    if (op_index >= total_ops) {
                        break;
                    }

                    std::vector<std::string> cmd = build_command(
                        kind,
                        prefix,
                        op_index,
                        key_count,
                        options.value_size
                    );

                    const auto t0 = Clock::now();
                    uint8_t tag = round_trip(fd, cmd);
                    const auto t1 = Clock::now();

                    double latency_us =
                        std::chrono::duration<double, std::micro>(t1 - t0).count();
                    result.latencies_us.push_back(latency_us);
                    result.completed++;

                    bool is_error = false;
                    if (tag == TAG_ERR) {
                        is_error = true;
                    } else if (kind == PhaseKind::GetHit && tag == TAG_NIL) {
                        is_error = true;
                    } else if (kind == PhaseKind::DelHit && tag != TAG_INT) {
                        is_error = true;
                    }

                    if (is_error) {
                        result.errors++;
                    }
                }
            } catch (...) {
                stop.store(true);
                failure.capture();
            }
            if (fd >= 0) {
                close(fd);
            }
        });
    }

    for (std::thread &thread : threads) {
        thread.join();
    }
    if (failure.error) {
        std::rethrow_exception(failure.error);
    }
    const auto finished = Clock::now();

    std::vector<double> latencies;
    latencies.reserve(total_ops);

    size_t completed = 0;
    size_t errors = 0;
    for (const WorkerResult &worker : worker_results) {
        completed += worker.completed;
        errors += worker.errors;
        latencies.insert(latencies.end(), worker.latencies_us.begin(), worker.latencies_us.end());
    }

    if (latencies.empty()) {
        die("benchmark produced no latency samples");
    }

    std::sort(latencies.begin(), latencies.end());
    const double seconds = std::chrono::duration<double>(finished - started).count();
    double sum = 0.0;
    for (double value : latencies) {
        sum += value;
    }

    auto percentile = [&](double p) -> double {
        if (latencies.empty()) {
            return 0.0;
        }
        double rank = p * (latencies.size() - 1);
        size_t idx = (size_t)std::llround(rank);
        if (idx >= latencies.size()) {
            idx = latencies.size() - 1;
        }
        return latencies[idx];
    };

    PhaseResult result;
    result.name = name;
    result.completed = completed;
    result.errors = errors;
    result.seconds = seconds;
    result.ops_per_sec = seconds > 0.0 ? completed / seconds : 0.0;
    result.avg_us = sum / latencies.size();
    result.min_us = latencies.front();
    result.p50_us = percentile(0.50);
    result.p95_us = percentile(0.95);
    result.p99_us = percentile(0.99);
    result.max_us = latencies.back();
    return result;
}

double parse_requests_per_second(const std::string &text) {
    std::regex pattern(R"(([\d.]+)\s+requests per second)");
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        return 0.0;
    }
    return std::stod(match[1].str());
}

std::string run_command_capture(const std::string &command) {
    std::string wrapped = command + " 2>/dev/null";
    FILE *pipe = popen(wrapped.c_str(), "r");
    if (!pipe) {
        return "";
    }

    char buffer[512];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    int status = pclose(pipe);
    if (status != 0) {
        return "";
    }
    return output;
}

RedisComparison maybe_run_redis_comparison(const Options &options) {
    RedisComparison comparison;
    if (!options.compare_redis) {
        return comparison;
    }

    std::ostringstream set_cmd;
    set_cmd
        << options.redis_benchmark_bin
        << " -q"
        << " -h " << options.redis_host
        << " -p " << options.redis_port
        << " -c " << options.connections
        << " -n " << options.requests
        << " SET __blueish_bench__:__key__ value";

    std::ostringstream get_cmd;
    get_cmd
        << options.redis_benchmark_bin
        << " -q"
        << " -h " << options.redis_host
        << " -p " << options.redis_port
        << " -c " << options.connections
        << " -n " << options.requests
        << " GET __blueish_bench__:__key__";

    std::string set_output = run_command_capture(set_cmd.str());
    std::string get_output = run_command_capture(get_cmd.str());
    if (set_output.empty() || get_output.empty()) {
        comparison.error =
            "redis-benchmark was requested but could not be executed successfully.";
        return comparison;
    }

    comparison.set_ops = parse_requests_per_second(set_output);
    comparison.get_ops = parse_requests_per_second(get_output);
    if (comparison.set_ops <= 0.0 || comparison.get_ops <= 0.0) {
        comparison.error = "redis-benchmark output could not be parsed.";
        return comparison;
    }

    comparison.available = true;
    return comparison;
}

void print_phase(const PhaseResult &result) {
    std::cout << result.name << "\n";
    std::cout << "  ops=" << result.completed
              << " errors=" << result.errors
              << " throughput=" << result.ops_per_sec << " ops/sec"
              << " avg=" << result.avg_us << " us"
              << " p50=" << result.p50_us << " us"
              << " p95=" << result.p95_us << " us"
              << " p99=" << result.p99_us << " us"
              << " min=" << result.min_us << " us"
              << " max=" << result.max_us << " us\n";
}

void print_usage(const char *argv0) {
    std::cout
        << "usage: " << argv0 << " [options]\n"
        << "  --host <host>                default: 127.0.0.1\n"
        << "  --port <port>                default: 1234\n"
        << "  --connections <n>            default: 16\n"
        << "  --requests <n>               total measured ops per phase, default: 20000\n"
        << "  --warmup <n>                 unmeasured preload keys, default: 2000\n"
        << "  --key-count <n>              keyspace for get/overwrite, default: 4096\n"
        << "  --value-size <bytes>         default: 64\n"
        << "  --server-cmd <command>       launch blueish automatically if not running\n"
        << "  --compare-redis              run redis-benchmark comparison for SET/GET\n"
        << "  --redis-host <host>          default: 127.0.0.1\n"
        << "  --redis-port <port>          default: 6379\n"
        << "  --redis-benchmark-bin <bin>  default: redis-benchmark\n"
        << "  --help\n";
}

size_t parse_size_arg(const std::string &value, const char *flag) {
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = strtoull(value.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        die(std::string("invalid numeric value for ") + flag);
    }
    return (size_t)parsed;
}

Options parse_args(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const char *flag) -> std::string {
            if (i + 1 >= argc) {
                die(std::string("missing value for ") + flag);
            }
            return argv[++i];
        };

        if (arg == "--host") {
            options.host = require_value("--host");
        } else if (arg == "--port") {
            options.port = (uint16_t)parse_size_arg(require_value("--port"), "--port");
        } else if (arg == "--connections") {
            options.connections = parse_size_arg(require_value("--connections"), "--connections");
        } else if (arg == "--requests") {
            options.requests = parse_size_arg(require_value("--requests"), "--requests");
        } else if (arg == "--warmup") {
            options.warmup = parse_size_arg(require_value("--warmup"), "--warmup");
        } else if (arg == "--key-count") {
            options.key_count = parse_size_arg(require_value("--key-count"), "--key-count");
        } else if (arg == "--value-size") {
            options.value_size = parse_size_arg(require_value("--value-size"), "--value-size");
        } else if (arg == "--server-cmd") {
            options.server_cmd = require_value("--server-cmd");
        } else if (arg == "--compare-redis") {
            options.compare_redis = true;
        } else if (arg == "--redis-host") {
            options.redis_host = require_value("--redis-host");
        } else if (arg == "--redis-port") {
            options.redis_port =
                (uint16_t)parse_size_arg(require_value("--redis-port"), "--redis-port");
        } else if (arg == "--redis-benchmark-bin") {
            options.redis_benchmark_bin = require_value("--redis-benchmark-bin");
        } else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            die("unknown argument: " + arg);
        }
    }

    if (options.connections == 0 || options.requests == 0 || options.key_count == 0) {
        die("connections, requests, and key-count must be greater than zero");
    }
    return options;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        Options options = parse_args(argc, argv);
        ProcessGuard server = launch_server_if_requested(options);
        wait_for_server(options);

        if (options.warmup > 0) {
            run_unmeasured_set_load(
                options,
                "bench:warmup",
                options.warmup,
                std::min(options.connections, options.warmup)
            );
        }

        PhaseResult set_insert = run_phase(
            options,
            PhaseKind::SetInsert,
            "SET insert",
            "bench:set:insert",
            options.requests,
            options.requests
        );

        run_unmeasured_set_load(
            options,
            "bench:set:overwrite",
            options.key_count,
            std::min(options.connections, options.key_count)
        );
        PhaseResult set_overwrite = run_phase(
            options,
            PhaseKind::SetOverwrite,
            "SET overwrite",
            "bench:set:overwrite",
            options.requests,
            options.key_count
        );

        run_unmeasured_set_load(
            options,
            "bench:get:hit",
            options.key_count,
            std::min(options.connections, options.key_count)
        );
        PhaseResult get_hit = run_phase(
            options,
            PhaseKind::GetHit,
            "GET hit",
            "bench:get:hit",
            options.requests,
            options.key_count
        );

        run_unmeasured_set_load(
            options,
            "bench:del:hit",
            options.requests,
            std::min(options.connections, options.requests)
        );
        PhaseResult del_hit = run_phase(
            options,
            PhaseKind::DelHit,
            "DEL hit",
            "bench:del:hit",
            options.requests,
            options.requests
        );

        RedisComparison redis = maybe_run_redis_comparison(options);

        std::cout << "blueish benchmark\n";
        std::cout << "  target=" << options.host << ":" << options.port
                  << " connections=" << options.connections
                  << " requests/phase=" << options.requests
                  << " key-count=" << options.key_count
                  << " value-size=" << options.value_size << "B\n";
        std::cout << "\n";

        print_phase(set_insert);
        print_phase(set_overwrite);
        print_phase(get_hit);
        print_phase(del_hit);

        std::cout << "\nsummary\n";
        std::cout << "  Achieved ~" << get_hit.avg_us
                  << " us average GET latency under "
                  << options.connections
                  << " concurrent connections; peak measured throughput reached ~"
                  << std::max(
                         std::max(set_insert.ops_per_sec, set_overwrite.ops_per_sec),
                         std::max(get_hit.ops_per_sec, del_hit.ops_per_sec)
                     )
                  << " ops/sec.\n";

        if (options.compare_redis) {
            if (redis.available) {
                std::cout << "  Redis comparison: SET "
                          << set_overwrite.ops_per_sec
                          << " vs " << redis.set_ops
                          << " ops/sec, GET "
                          << get_hit.ops_per_sec
                          << " vs " << redis.get_ops
                          << " ops/sec.\n";
            } else {
                std::cout << "  Redis comparison unavailable: " << redis.error << "\n";
            }
        }

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "benchmark error: " << ex.what() << "\n";
        return 1;
    }
}
