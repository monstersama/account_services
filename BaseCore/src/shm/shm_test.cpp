/**
 * shm_test: ShmWriter / ShmReader 功能测试
 * 用法: shm_test [base_name]
 *   base_name: 共享内存名或文件路径前缀，默认 /tmp/shm_test_<pid>
 * 支持 SHM_USE_FILE=1 时使用文件后端
 */

#include "shm/shm_interface.hpp"
#include "shm/shm_config_reader.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << (msg) << "\n"; \
            return false; \
        } \
    } while (0)

static std::string g_test_base;

static std::string make_name() {
    std::ostringstream oss;
    oss << g_test_base << "_" << getpid() << "_"
        << std::chrono::steady_clock::now().time_since_epoch().count();
    return oss.str();
}

static bool test_create_open() {
    const std::string name = make_name();
    const std::size_t size = 4096;

    ASSERT(shm::create_if_not_exists(name, size), "Create: create_if_not_exists failed");
    shm::ShmWriter w1;
    void* p1 = w1.open(name, size);
    ASSERT(p1 != nullptr, "Create: open failed");
    ASSERT(w1.is_open(), "Create: is_open");
    ASSERT(w1.size() == size, "Create: size");
    ASSERT(w1.name() == name, "Create: name");
    ASSERT(w1.data() == p1, "Create: data");

    w1.close();
    ASSERT(!w1.is_open(), "Create: after close");

    shm::ShmWriter w2;
    void* p2 = w2.open(name, size);
    ASSERT(p2 != nullptr, "Open: open failed");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_open_or_create() {
    const std::string name = make_name();
    const std::size_t size = 8192;

    ASSERT(shm::create_if_not_exists(name, size), "OpenOrCreate first: create failed");
    shm::ShmWriter w1;
    void* p1 = w1.open(name, size);
    ASSERT(p1 != nullptr, "OpenOrCreate first: failed");

    w1.close();

    ASSERT(shm::create_if_not_exists(name, size), "OpenOrCreate second: create (idempotent)");
    shm::ShmWriter w2;
    void* p2 = w2.open(name, size);
    ASSERT(p2 != nullptr, "OpenOrCreate second: failed");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_create_idempotent() {
    const std::string name = make_name();
    const std::size_t size = 4096;

    ASSERT(shm::create_if_not_exists(name, size), "Create first: failed");
    shm::ShmWriter w1;
    void* p1 = w1.open(name, size);
    ASSERT(p1 != nullptr, "Create first: open failed");

    w1.close();

    ASSERT(shm::create_if_not_exists(name, size), "Create second (exists): should succeed as no-op");
    shm::ShmWriter w2;
    void* p2 = w2.open(name, size);
    ASSERT(p2 != nullptr, "Create second: open failed");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_open_fail_if_not_exists() {
    const std::string name = g_test_base + "_nonexist_" + std::to_string(getpid());
    const std::size_t size = 4096;

    shm::ShmWriter w;
    void* p = w.open(name, size);
    ASSERT(p == nullptr, "Open nonexistent: should fail");
    return true;
}

static bool test_write_read() {
    const std::string name = make_name();
    const std::size_t size = 4096;
    const char* msg = "hello shm";

    ASSERT(shm::create_if_not_exists(name, size), "write_read: create failed");
    shm::ShmWriter w;
    void* p = w.open(name, size);
    ASSERT(p != nullptr, "write_read: writer open failed");

    ASSERT(w.write_at(0, msg, std::strlen(msg) + 1), "write_at offset 0");
    ASSERT(w.write_at(100, "world", 6), "write_at offset 100");

    w.close();

    shm::ShmReader r;
    const void* pr = r.open(name, size);
    ASSERT(pr != nullptr, "write_read: reader open failed");

    char buf[64]{};
    ASSERT(r.read_at(0, buf, 10), "read_at offset 0");
    ASSERT(std::strcmp(buf, "hello shm") == 0, "read_at content");

    std::memset(buf, 0, sizeof(buf));
    ASSERT(r.read_at(100, buf, 6), "read_at offset 100");
    ASSERT(std::strcmp(buf, "world") == 0, "read_at content 2");

    r.close();
    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_write_read_boundary() {
    const std::string name = make_name();
    const std::size_t size = 256;

    ASSERT(shm::create_if_not_exists(name, size), "boundary: create failed");
    shm::ShmWriter w;
    void* p = w.open(name, size);
    ASSERT(p != nullptr, "boundary: writer open");

    char fill = 'x';
    ASSERT(w.write_at(size - 1, &fill, 1), "write_at last byte");

    w.close();

    shm::ShmReader r;
    const void* pr = r.open(name, size);
    ASSERT(pr != nullptr, "boundary: reader open");

    char buf;
    ASSERT(r.read_at(size - 1, &buf, 1), "read_at last byte");
    ASSERT(buf == 'x', "boundary: content");

    r.close();
    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_write_read_errors() {
    const std::string name = make_name();
    const std::size_t size = 256;

    ASSERT(shm::create_if_not_exists(name, size), "errors: create failed");
    shm::ShmWriter w;
    void* p = w.open(name, size);
    ASSERT(p != nullptr, "errors: writer open");

    char data = 'a';
    ASSERT(!w.write_at(size, &data, 1), "write_at out of bounds should fail");
    ASSERT(!w.write_at(0, nullptr, 1), "write_at null data should fail");

    w.close();

    shm::ShmReader r;
    const void* pr = r.open(name, size);
    ASSERT(pr != nullptr, "errors: reader open");

    char buf;
    ASSERT(!r.read_at(size, &buf, 1), "read_at out of bounds should fail");
    ASSERT(!r.read_at(0, nullptr, 1), "read_at null buf should fail");

    r.close();

    shm::ShmWriter w2;
    ASSERT(!w2.write_at(0, &data, 1), "write_at when closed should fail");

    shm::ShmReader r2;
    ASSERT(!r2.read_at(0, &buf, 1), "read_at when closed should fail");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_move_semantics() {
    const std::string name = make_name();
    const std::size_t size = 256;

    ASSERT(shm::create_if_not_exists(name, size), "move: create failed");
    shm::ShmWriter w1;
    void* p = w1.open(name, size);
    ASSERT(p != nullptr, "move: open");

    shm::ShmWriter w2 = std::move(w1);
    ASSERT(w2.is_open() && !w1.is_open(), "move writer");
    ASSERT(w2.data() == p, "move writer data");

    shm::ShmWriter w3;
    w3 = std::move(w2);
    ASSERT(w3.is_open() && !w2.is_open(), "move assign writer");

    w3.close();

    shm::ShmReader r1;
    const void* pr = r1.open(name, size);
    ASSERT(pr != nullptr, "move: reader open");

    shm::ShmReader r2 = std::move(r1);
    ASSERT(r2.is_open() && !r1.is_open(), "move reader");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_multi_reader() {
    const std::string name = make_name();
    const std::size_t size = 4096;
    const char* msg = "multi reader test";

    ASSERT(shm::create_if_not_exists(name, size), "multi: create failed");
    shm::ShmWriter w;
    void* p = w.open(name, size);
    ASSERT(p != nullptr, "multi: writer open");
    ASSERT(w.write_at(0, msg, std::strlen(msg) + 1), "multi: write");
    w.close();

    auto reader_fn = [&]() {
        shm::ShmReader r;
        const void* pr = r.open(name, size);
        if (!pr) return;
        char buf[64]{};
        r.read_at(0, buf, std::strlen(msg) + 1);
    };

    std::thread t1(reader_fn);
    std::thread t2(reader_fn);
    t1.join();
    t2.join();

    shm::ShmReader r;
    const void* pr = r.open(name, size);
    ASSERT(pr != nullptr, "multi: main reader");
    char buf[64]{};
    ASSERT(r.read_at(0, buf, std::strlen(msg) + 1), "multi: read");
    ASSERT(std::strcmp(buf, msg) == 0, "multi: content");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_size_mismatch() {
    const std::string name = make_name();
    const std::size_t size = 1024;

    ASSERT(shm::create_if_not_exists(name, size), "size_mismatch: create failed");
    shm::ShmWriter w;
    void* p = w.open(name, size);
    ASSERT(p != nullptr, "size_mismatch: create");
    w.close();

    shm::ShmReader r;
    const void* pr = r.open(name, size + 100);
    ASSERT(pr == nullptr, "size_mismatch: should fail when size differs");

    shm::ShmWriter::unlink(name);
    return true;
}

static bool test_account_name_overload() {
    const std::string base = make_name();
    const std::string account = "acc1";
    const std::string full_name = base + "_" + account;
    const std::size_t size = 256;

    ASSERT(shm::create_if_not_exists(full_name, size), "account: create failed");
    shm::ShmWriter w;
    void* p = w.open(full_name, size);
    ASSERT(p != nullptr, "account: create");
    w.close();

    shm::ShmReader r;
    const void* pr = r.open(account, base, size);
    ASSERT(pr != nullptr, "account: open with account");
    ASSERT(r.name() == full_name, "account: name");

    shm::ShmWriter::unlink(full_name);
    return true;
}

static bool test_config_reader() {
    const std::string path = "/tmp/shm_test_config_" + std::to_string(getpid()) + ".conf";
    {
        std::ofstream f(path);
        ASSERT(f, "config_reader: create file");
        f << "name = /foo_shm, size = 4096\n";
        f << "name = /bar_shm, size = 8M\n";
        f << "# comment\n";
        f << "name = \"/baz_shm\", size = 0.015625M\n";
        f << "name = /gig_shm, size = 1G\n";
    }

    shm::ShmConfigReader reader;
    auto cfg = reader.load(path);
    ASSERT(cfg.size() == 4, "config_reader: size");
    ASSERT(cfg["/foo_shm"] == 4096, "config_reader: foo");
    ASSERT(cfg["/bar_shm"] == 8388608, "config_reader: bar (8M)");
    ASSERT(cfg["/baz_shm"] == 16384, "config_reader: baz (0.015625M)");
    ASSERT(cfg["/gig_shm"] == 1073741824ULL, "config_reader: gig (1G)");

    auto empty = reader.load("/nonexistent/path.conf");
    ASSERT(empty.empty(), "config_reader: nonexistent");

    std::remove(path.c_str());
    return true;
}

int main(int argc, char* argv[]) {
    const char* env = std::getenv("SHM_USE_FILE");
    if (env && env[0] == '1') {
        g_test_base = (argc >= 2) ? argv[1] : "/tmp/shm_test";
        std::cout << "shm_test: using file backend (SHM_USE_FILE=1), base=" << g_test_base << "\n";
    } else {
        g_test_base = (argc >= 2) ? argv[1] : "/shm_test";
        std::cout << "shm_test: using POSIX shm, base=" << g_test_base << "\n";
    }

    int passed = 0;
    int failed = 0;

#define RUN(t) \
    do { \
        std::cout << "  " #t " ... "; \
        if (test_##t()) { std::cout << "OK\n"; ++passed; } \
        else { std::cout << "FAIL\n"; ++failed; } \
    } while (0)

    std::cout << "shm_test: running functional tests\n";
    RUN(create_open);
    RUN(open_or_create);
    RUN(create_idempotent);
    RUN(open_fail_if_not_exists);
    RUN(write_read);
    RUN(write_read_boundary);
    RUN(write_read_errors);
    RUN(move_semantics);
    RUN(multi_reader);
    RUN(size_mismatch);
    RUN(account_name_overload);
    RUN(config_reader);

    std::cout << "shm_test: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
