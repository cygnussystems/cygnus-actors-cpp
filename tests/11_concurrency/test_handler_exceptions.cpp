#include "../test_common.h"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Issue 014: an exception escaping a tell() handler of a pooled or fast actor
// unwound out of a framework thread and terminated the process. It is now
// caught, reported (callback, counter, stderr fallback) and the actor either
// resumes (default) or stops, as its on_handler_exception() decides.

namespace handler_exception_test {

    struct boom : public cas::message_base {};
    struct boom_int : public cas::message_base {};
    struct ok : public cas::message_base {};
    struct explode_op {};

    // Records every report; safe to read from the test thread
    struct recorder {
        std::mutex mutex;
        std::vector<cas::handler_exception_info> reports;

        void install() {
            cas::system::set_handler_exception_handler([this](const cas::handler_exception_info& info) {
                std::lock_guard<std::mutex> lock(mutex);
                reports.push_back(info);
            });
        }
        size_t size() {
            std::lock_guard<std::mutex> lock(mutex);
            return reports.size();
        }
        cas::handler_exception_info at(size_t i) {
            std::lock_guard<std::mutex> lock(mutex);
            return reports.at(i);
        }
    };

    template<typename Base>
    class thrower : public Base {
    public:
        std::atomic<int> handled{0};
        cas::exception_action policy = cas::exception_action::resume;
        std::atomic<int> policy_calls{0};

    protected:
        void on_start() override {
            this->template handler<boom>([](const boom&) { throw std::runtime_error("bad order"); });
            this->template handler<boom_int>([](const boom_int&) { throw 42; });
            this->template handler<ok>([this](const ok&) { handled++; });
        }

        cas::exception_action on_handler_exception(const cas::message_base&, std::exception_ptr) override {
            policy_calls++;
            return policy;
        }
    };

    class stateful_thrower : public cas::stateful_actor {
    public:
        std::atomic<int> handled{0};
    protected:
        void on_start() override {
            handler<boom>([](const boom&) { throw std::runtime_error("stateful"); });
            handler<ok>([this](const ok&) { handled++; });
            accept_all_message_types();
        }
    };

    class watcher : public cas::actor {
    public:
        std::atomic<int> terminations{0};
    protected:
        void on_start() override {
            handler<cas::termination_msg>([this](const cas::termination_msg&) { terminations++; });
        }
    };

    class bad_hooks : public cas::actor {
    public:
        bool throw_in_shutdown = false;
    protected:
        void on_start() override {}
        void on_shutdown() override {
            if (throw_in_shutdown) throw std::runtime_error("shutdown failed");
        }
        void on_stop() override { throw std::logic_error("stop failed"); }
    };

    class calc : public cas::actor {
    protected:
        void on_start() override {
            call_handler<int, explode_op>(&calc::explode);
        }
        int explode() { throw std::runtime_error("call failed"); }
    };

    template<typename Pred>
    bool wait_for(Pred pred) {
        for (int i = 0; i < 400 && !pred(); ++i) {
            wait_ms(5);
        }
        return pred();
    }

    template<typename Base>
    void check_resume() {
        recorder rec;
        rec.install();
        const size_t before = cas::system::handler_exception_count();

        auto ref = cas::system::create<thrower<Base>>();
        cas::system::start();
        auto& a = ref.template get_checked<thrower<Base>>();

        ref.tell(boom{});
        ref.tell(ok{});  // the actor keeps working after the exception
        REQUIRE(wait_for([&] { return a.handled.load() == 1 && rec.size() == 1; }));

        auto info = rec.at(0);
        REQUIRE(info.actor_name == a.name());
        REQUIRE(info.message_type == std::type_index(typeid(boom)));
        REQUIRE(info.message_id != 0);
        REQUIRE(info.source == "handler");
        REQUIRE(info.what == "bad order");
        REQUIRE(info.error != nullptr);
        REQUIRE(info.action == cas::exception_action::resume);
        REQUIRE(a.policy_calls.load() == 1);
        REQUIRE(cas::system::handler_exception_count() == before + 1);
        REQUIRE(ref.is_running());
    }
}

using namespace handler_exception_test;

TEST_CASE("A throwing handler on a pooled actor is reported and the actor resumes",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    check_resume<cas::actor>();
}

TEST_CASE("A throwing handler on a fast actor is reported and the actor resumes",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    check_resume<cas::fast_actor>();
}

TEST_CASE("A throwing handler on a stateful actor is reported and the actor resumes",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<stateful_thrower>();
    cas::system::start();
    auto& a = ref.get_checked<stateful_thrower>();

    ref.tell(boom{});
    ref.tell(ok{});
    REQUIRE(wait_for([&] { return a.handled.load() == 1 && rec.size() == 1; }));
    REQUIRE(rec.at(0).what == "stateful");
}

TEST_CASE("A non-std exception is reported as unknown",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<thrower<cas::actor>>();
    cas::system::start();

    ref.tell(boom_int{});
    REQUIRE(wait_for([&] { return rec.size() == 1; }));
    REQUIRE(rec.at(0).what == "unknown exception");
}

TEST_CASE("exception_action::stop stops the actor and notifies watchers",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<thrower<cas::actor>>();
    auto w = cas::system::create<watcher>();
    auto& a = ref.get_checked<thrower<cas::actor>>();
    a.policy = cas::exception_action::stop;
    cas::system::watch(w, ref);
    cas::system::start();

    ref.tell(boom{});
    REQUIRE(wait_for([&] { return w.get_checked<watcher>().terminations.load() == 1; }));
    REQUIRE_FALSE(ref.is_running());
    REQUIRE(rec.at(0).action == cas::exception_action::stop);
}

TEST_CASE("Exceptions from on_stop and on_shutdown are reported and the stop completes",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();

    auto stopped = cas::system::create<bad_hooks>();
    auto shut_down = cas::system::create<bad_hooks>();
    shut_down.get_checked<bad_hooks>().throw_in_shutdown = true;
    cas::system::start();

    REQUIRE(cas::system::stop_actor(stopped));  // waits for on_stop()
    REQUIRE(stopped.get_checked<bad_hooks>().get_state() == cas::actor_state::stopped);
    REQUIRE(rec.size() == 1);
    REQUIRE(rec.at(0).source == "on_stop");
    REQUIRE(rec.at(0).message_type == std::type_index(typeid(void)));

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    REQUIRE(shut_down.get_checked<bad_hooks>().get_state() == cas::actor_state::stopped);
    REQUIRE(rec.size() == 3);  // + on_shutdown and on_stop of the second actor
}

TEST_CASE("Without a callback a handler exception is still survived and counted",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    const size_t before = cas::system::handler_exception_count();
    auto ref = cas::system::create<thrower<cas::actor>>();
    cas::system::start();
    auto& a = ref.get_checked<thrower<cas::actor>>();

    ref.tell(boom{});  // written to stderr
    ref.tell(ok{});
    REQUIRE(wait_for([&] { return a.handled.load() == 1; }));
    REQUIRE(cas::system::handler_exception_count() == before + 1);
}

TEST_CASE("call() handler exceptions still go to the caller, not the report",
          "[11_concurrency][exceptions][issue014]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<calc>();
    cas::system::start();

    REQUIRE_THROWS_AS(ref.call<int>(explode_op{}), std::runtime_error);
    REQUIRE(rec.size() == 0);
}
