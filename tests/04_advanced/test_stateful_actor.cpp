#include "../test_common.h"
#include <atomic>
#include <mutex>
#include <vector>

// Level 4: selective receive - stateful_actor (pooled) and stateful_fast_actor
// (dedicated thread). Every case runs against both: the filtering logic is
// shared (detail::selective_stash), and so are the guarantees.
//
// This file was excluded from the build before 1.0 ("ABI sensitivity",
// never explained), so stateful_actor shipped untested. It is rewritten for
// the current design and back in the build.

namespace stateful_test {

struct start_download : cas::message_base { int file_id = 0; };
struct cancel_download : cas::message_base {};
struct download_complete : cas::message_base {};
struct item : cas::message_base { int seq = 0; };
struct status_op {};

// The original scenario: a downloader that accepts different messages in its
// idle and downloading states. Templated on the class under test.
template<typename Base>
class downloader : public Base {
public:
    enum class phase { idle, downloading };

    std::atomic<int> completed{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> started{0};
    std::atomic<bool> shutdown_hook_ran{false};
    std::atomic<phase> current{phase::idle};

protected:
    void on_start() override {
        this->template handler<start_download>(&downloader::on_start_download);
        this->template handler<cancel_download>(&downloader::on_cancel);
        this->template handler<download_complete>(&downloader::on_complete);
        this->template call_handler<int, status_op>(&downloader::status);
        enter_idle();
    }

    void on_shutdown() override { shutdown_hook_ran = true; }

private:
    void enter_idle() {
        current = phase::idle;
        this->reject_all_message_types();
        this->template accept_message_type<start_download>();
    }

    void enter_downloading() {
        current = phase::downloading;
        this->reject_all_message_types();
        this->template accept_message_type<cancel_download>();
        this->template accept_message_type<download_complete>();
    }

    void on_start_download(const start_download&) { ++started; enter_downloading(); }
    void on_cancel(const cancel_download&) { ++cancelled; enter_idle(); }
    void on_complete(const download_complete&) { ++completed; enter_idle(); }
    int status() { return current == phase::idle ? 0 : 1; }
};

struct open_gate : cas::message_base {};

// Holds every `item` until told to open, then must handle them in send order.
template<typename Base>
class gate : public Base {
public:
    std::mutex mutex;
    std::vector<int> seen;

    std::vector<int> received() {
        std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }

protected:
    void on_start() override {
        this->template handler<item>([this](const item& i) {
            std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(i.seq);
        });
        this->template handler<open_gate>([this](const open_gate&) {
            this->accept_all_message_types();
        });
        this->reject_all_message_types();
        this->template accept_message_type<open_gate>();
    }
};

template<typename Pred>
bool wait_for(Pred pred, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

} // namespace stateful_test

using namespace stateful_test;

TEMPLATE_TEST_CASE("Selective receive holds messages the current state does not accept",
                   "[04_advanced][stateful]", cas::stateful_actor, cas::stateful_fast_actor) {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<downloader<TestType>>();
    cas::system::start();
    auto& d = ref.template get_checked<downloader<TestType>>();

    // Idle: only start_download is accepted.
    ref.tell(cancel_download{});
    ref.tell(download_complete{});
    settle();
    REQUIRE(d.cancelled == 0);
    REQUIRE(d.completed == 0);
    REQUIRE(ref.template get<cas::actor>()->queue_size() == 2);  // held, and counted
}

TEMPLATE_TEST_CASE("Held messages are handled when a state change accepts them",
                   "[04_advanced][stateful]", cas::stateful_actor, cas::stateful_fast_actor) {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<downloader<TestType>>();
    cas::system::start();
    auto& d = ref.template get_checked<downloader<TestType>>();

    ref.tell(cancel_download{});   // held: not accepted while idle
    ref.tell(start_download{});    // accepted: -> downloading, which accepts cancel

    REQUIRE(wait_for([&] { return d.cancelled.load() == 1; }));
    REQUIRE(d.started == 1);
    REQUIRE(d.current == downloader<TestType>::phase::idle);  // cancel went back to idle
    REQUIRE(ref.template get<cas::actor>()->queue_size() == 0);
}

TEMPLATE_TEST_CASE("Held messages are handled in the order they were sent",
                   "[04_advanced][stateful]", cas::stateful_actor, cas::stateful_fast_actor) {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<gate<TestType>>();
    cas::system::start();
    auto& g = ref.template get_checked<gate<TestType>>();

    constexpr int n = 500;
    for (int i = 0; i < n; ++i) {
        item it;
        it.seq = i;
        ref.tell(it);
    }
    settle();
    REQUIRE(g.received().empty());

    ref.tell(open_gate{});
    REQUIRE(wait_for([&] { return g.received().size() == static_cast<size_t>(n); }));
    auto got = g.received();
    for (int i = 0; i < n; ++i) {
        REQUIRE(got[i] == i);
    }
}

TEMPLATE_TEST_CASE("A call is answered whatever the filter accepts",
                   "[04_advanced][stateful]", cas::stateful_actor, cas::stateful_fast_actor) {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<downloader<TestType>>();
    cas::system::start();

    // Idle does not accept the call's internal request type - it must not
    // matter: calls bypass the filter. This used to block forever.
    auto answer = ref.template call<int>(status_op{}, std::chrono::milliseconds(2000));
    REQUIRE(answer.has_value());
    REQUIRE(*answer == 0);

    ref.tell(start_download{});
    auto& d = ref.template get_checked<downloader<TestType>>();
    REQUIRE(wait_for([&] { return d.started.load() == 1; }));
    answer = ref.template call<int>(status_op{}, std::chrono::milliseconds(2000));
    REQUIRE(answer.has_value());
    REQUIRE(*answer == 1);
}

TEMPLATE_TEST_CASE("System shutdown is not held back by the filter",
                   "[04_advanced][stateful]", cas::stateful_actor, cas::stateful_fast_actor) {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<downloader<TestType>>();
    cas::system::start();
    auto& d = ref.template get_checked<downloader<TestType>>();

    ref.tell(cancel_download{});  // held
    cas::shutdown_config quick;
    quick.drain_timeout = std::chrono::milliseconds(200);  // the held message never drains
    cas::system::shutdown(quick);
    cas::system::wait_for_shutdown();

    REQUIRE(d.shutdown_hook_ran.load());
    // The held message is reported as unprocessed, not silently forgotten.
    bool logged = false;
    for (const auto& line : cas::system::get_shutdown_log()) {
        if (line.find("1 unprocessed") != std::string::npos) logged = true;
    }
    REQUIRE(logged);
}

TEMPLATE_TEST_CASE("Stopping drops held messages as dead letters",
                   "[04_advanced][stateful]", cas::stateful_actor, cas::stateful_fast_actor) {
    CAS_TEST_GUARD();
    cas::system::reset_dead_letter_stats();
    auto ref = cas::system::create<downloader<TestType>>();
    cas::system::start();

    ref.tell(cancel_download{});
    ref.tell(download_complete{});
    settle();

    SECTION("drain: held messages cannot drain; counted when the stop completes") {
        REQUIRE(cas::system::stop_actor(ref));
    }
    SECTION("discard: dropped at once") {
        cas::stop_config cfg;
        cfg.mode = cas::stop_mode::discard;
        REQUIRE(cas::system::stop_actor(ref, cfg));
    }
    REQUIRE(cas::system::get_dead_letter_stats().dropped_tell == 2);
}
