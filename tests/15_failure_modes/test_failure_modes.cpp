#include "../test_common.h"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// INVARIANT 7: user code never throws out of a framework thread.
//
// Failure-mode matrix: every place the framework runs user code, for every
// actor type, with that code throwing. Each cell asserts a defined outcome -
// reported (handler_exception_handler), delivered to the caller, or
// propagated to the sender - and that the process survives. The one outcome
// that is deliberately fatal (on_start() of a fast actor created before
// start()) is checked by the separate fatal_probe executable.
//
// Keep this matrix complete: a new place that runs user code gets a row here.

namespace failure_matrix {

    struct boom : public cas::message_base {};
    struct ok : public cas::message_base {};
    struct nobody_handles : public cas::message_base {};
    struct explode_op {};

    // Collects reports; safe to read from the test thread
    struct recorder {
        std::mutex mutex;
        std::vector<std::string> sources;
        std::vector<std::string> whats;

        void install() {
            cas::system::set_handler_exception_handler([this](const cas::handler_exception_info& info) {
                std::lock_guard<std::mutex> lock(mutex);
                sources.push_back(info.source);
                whats.push_back(info.what);
            });
        }
        size_t count(const std::string& source) {
            std::lock_guard<std::mutex> lock(mutex);
            size_t n = 0;
            for (const auto& s : sources) n += (s == source) ? 1 : 0;
            return n;
        }
        size_t size() {
            std::lock_guard<std::mutex> lock(mutex);
            return sources.size();
        }
    };

    template<typename Pred>
    bool wait_for(Pred pred) {
        for (int i = 0; i < 400 && !pred(); ++i) {
            wait_ms(5);
        }
        return pred();
    }

    // Which piece of user code throws
    enum class fault { none, handler, unhandled, policy, on_shutdown, on_stop };

    // One actor class per base, configurable fault
    template<typename Base>
    class faulty : public Base {
    public:
        fault what = fault::none;
        std::atomic<int> handled{0};

    protected:
        void on_start() override {
            if constexpr (std::is_base_of_v<cas::stateful_actor, Base> ||
                          std::is_base_of_v<cas::stateful_fast_actor, Base>) {
                this->accept_all_message_types();
            }
            this->template handler<boom>([this](const boom&) {
                if (what == fault::handler || what == fault::policy) {
                    throw std::runtime_error("handler");
                }
            });
            this->template handler<ok>([this](const ok&) { handled++; });
            this->template call_handler<int, explode_op>(&faulty::explode);
        }

        void on_unhandled_message(cas::message_base*) override {
            if (what == fault::unhandled) throw std::runtime_error("unhandled");
        }

        cas::exception_action on_handler_exception(const cas::message_base&, std::exception_ptr) override {
            if (what == fault::policy) throw std::runtime_error("policy");
            return cas::exception_action::resume;
        }

        void on_shutdown() override {
            if (what == fault::on_shutdown) throw std::runtime_error("on_shutdown");
        }

        void on_stop() override {
            if (what == fault::on_stop) throw std::runtime_error("on_stop");
        }

        int explode() { throw std::runtime_error("call"); }
    };

    class watcher : public cas::actor {
    public:
        std::atomic<int> terminations{0};
    protected:
        void on_start() override {
            handler<cas::termination_msg>([this](const cas::termination_msg&) { terminations++; });
        }
    };

    template<bool ThreadSafe>
    class faulty_inline : public cas::inline_actor<ThreadSafe> {
    public:
        bool throw_on_stop = false;
    protected:
        void on_start() override {
            this->template handler<boom>([](const boom&) { throw std::runtime_error("inline"); });
        }
        void on_stop() override {
            if (throw_on_stop) throw std::runtime_error("inline on_stop");
        }
    };

    class throws_in_ctor : public cas::actor {
    public:
        throws_in_ctor() { throw std::runtime_error("ctor"); }
    protected:
        void on_start() override {}
    };

    class throws_in_on_start : public cas::actor {
    protected:
        void on_start() override { throw std::runtime_error("on_start"); }
    };

    class fast_throws_in_on_start : public cas::fast_actor {
    protected:
        void on_start() override { throw std::runtime_error("fast on_start"); }
    };

    class sink : public cas::actor {
    public:
        std::atomic<bool> release{false};
        std::atomic<int> handled{0};
    protected:
        void on_start() override {
            handler<ok>([this](const ok&) {
                while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                handled++;
            });
        }
    };
}

using namespace failure_matrix;

#define FAILURE_MATRIX_TYPES \
    faulty<cas::actor>, faulty<cas::fast_actor>, faulty<cas::stateful_actor>, faulty<cas::stateful_fast_actor>

// ---------------------------------------------------------------------------
// Rows: user code of queued actors (pooled, fast, stateful, stateful fast)
// ---------------------------------------------------------------------------

TEMPLATE_TEST_CASE("Matrix: a throwing message handler is reported and the actor resumes",
                   "[15_failure_modes][invariant7]", FAILURE_MATRIX_TYPES) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    auto& a = ref.template get_checked<TestType>();
    a.what = fault::handler;
    cas::system::start();

    ref.tell(boom{});
    ref.tell(ok{});
    REQUIRE(wait_for([&] { return a.handled.load() == 1; }));
    REQUIRE(rec.count("handler") == 1);
    REQUIRE(ref.is_running());
}

TEMPLATE_TEST_CASE("Matrix: a throwing on_unhandled_message is reported and the actor resumes",
                   "[15_failure_modes][invariant7]", FAILURE_MATRIX_TYPES) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    auto& a = ref.template get_checked<TestType>();
    a.what = fault::unhandled;
    cas::system::start();

    ref.tell(nobody_handles{});
    ref.tell(ok{});
    REQUIRE(wait_for([&] { return a.handled.load() == 1; }));
    REQUIRE(rec.count("handler") == 1);
}

TEMPLATE_TEST_CASE("Matrix: a throwing on_handler_exception stops the actor",
                   "[15_failure_modes][invariant7]", FAILURE_MATRIX_TYPES) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    auto w = cas::system::create<watcher>();
    ref.template get_checked<TestType>().what = fault::policy;
    cas::system::watch(w, ref);
    cas::system::start();

    ref.tell(boom{});
    REQUIRE(wait_for([&] { return w.get_checked<watcher>().terminations.load() == 1; }));
    REQUIRE_FALSE(ref.is_running());
    REQUIRE(rec.count("handler") == 1);
}

TEMPLATE_TEST_CASE("Matrix: a throwing on_shutdown during stop_actor is reported; the stop completes",
                   "[15_failure_modes][invariant7]", FAILURE_MATRIX_TYPES) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    ref.template get_checked<TestType>().what = fault::on_shutdown;
    cas::system::start();

    REQUIRE(cas::system::stop_actor(ref));
    REQUIRE(ref.template get_checked<TestType>().get_state() == cas::actor_state::stopped);
    REQUIRE(rec.count("on_shutdown") == 1);
}

TEMPLATE_TEST_CASE("Matrix: a throwing on_stop during system shutdown is reported; shutdown completes",
                   "[15_failure_modes][invariant7]", FAILURE_MATRIX_TYPES) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    ref.template get_checked<TestType>().what = fault::on_stop;
    cas::system::start();

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    REQUIRE(ref.template get_checked<TestType>().get_state() == cas::actor_state::stopped);
    REQUIRE(rec.count("on_stop") == 1);
}

TEMPLATE_TEST_CASE("Matrix: a throwing call handler reaches the caller, not the report",
                   "[15_failure_modes][invariant7]", FAILURE_MATRIX_TYPES) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    cas::system::start();

    REQUIRE_THROWS_AS(ref.template call<int>(explode_op{}), std::runtime_error);
    REQUIRE(rec.size() == 0);
    REQUIRE(ref.is_running());
}

// ---------------------------------------------------------------------------
// Inline actors: handler exceptions belong to the sender
// ---------------------------------------------------------------------------

TEMPLATE_TEST_CASE("Matrix: an inline actor's throwing handler propagates to the sender",
                   "[15_failure_modes][invariant7]", faulty_inline<true>, faulty_inline<false>) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    cas::system::start();

    REQUIRE_THROWS_AS(ref.tell(boom{}), std::runtime_error);
    REQUIRE(rec.size() == 0);
    REQUIRE(ref.is_running());
}

TEMPLATE_TEST_CASE("Matrix: an inline actor's throwing on_stop is reported; the stop completes",
                   "[15_failure_modes][invariant7]", faulty_inline<true>, faulty_inline<false>) {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    auto ref = cas::system::create<TestType>();
    ref.template get_checked<TestType>().throw_on_stop = true;
    cas::system::start();

    REQUIRE(cas::system::stop_actor(ref));
    REQUIRE(ref.template get_checked<TestType>().get_state() == cas::actor_state::stopped);
    REQUIRE(rec.count("on_stop") == 1);
}

// ---------------------------------------------------------------------------
// System callbacks: run on whatever thread triggered them
// ---------------------------------------------------------------------------

TEST_CASE("Matrix: a throwing dead-letter handler is reported, not thrown at the sender",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    cas::system::set_dead_letter_handler([](const cas::dead_letter_info&) {
        throw std::runtime_error("dead letter handler");
    });
    auto ref = cas::system::create<faulty<cas::actor>>();
    cas::system::start();
    REQUIRE(cas::system::stop_actor(ref));

    REQUIRE_NOTHROW(ref.tell(ok{}));             // stopped target, from main()
    REQUIRE_NOTHROW(cas::actor_ref{}.tell(ok{})); // invalid ref
    REQUIRE(rec.count("dead_letter_handler") == 2);
}

TEST_CASE("Matrix: a throwing dead-letter handler on a worker (discard stop) is survived",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    recorder rec;
    rec.install();
    cas::system::set_dead_letter_handler([](const cas::dead_letter_info&) {
        throw std::runtime_error("dead letter handler");
    });
    auto ref = cas::system::create<sink>();
    auto& s = ref.get_checked<sink>();
    cas::system::start();

    for (int i = 0; i < 5; ++i) ref.tell(ok{});  // first one blocks, 4 stay queued
    cas::stop_config config;
    config.mode = cas::stop_mode::discard;
    config.wait_for_stop = false;
    REQUIRE(cas::system::stop_actor(ref, config));
    s.release = true;  // the worker now discards the queue: dead letters on the worker

    REQUIRE(wait_for([&] { return s.get_state() == cas::actor_state::stopped; }));
    REQUIRE(wait_for([&] { return rec.count("dead_letter_handler") >= 1; }));
}

TEST_CASE("Matrix: a throwing queue-threshold handler is reported, not thrown at the sender",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    cas::system_config config;
    config.queue_threshold = 3;
    cas::system::configure(config);
    recorder rec;
    rec.install();
    cas::system::set_queue_threshold_handler([](const cas::queue_threshold_info&) {
        throw std::runtime_error("threshold handler");
    });
    auto ref = cas::system::create<sink>();
    cas::system::start();

    for (int i = 0; i < 10; ++i) {
        REQUIRE_NOTHROW(ref.tell(ok{}));
    }
    REQUIRE(rec.count("queue_threshold_handler") == 1);
    ref.get_checked<sink>().release = true;
}

TEST_CASE("Matrix: a throwing handler-exception callback falls back to stderr",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    const size_t before = cas::system::handler_exception_count();
    cas::system::set_handler_exception_handler([](const cas::handler_exception_info&) {
        throw std::runtime_error("reporting callback");
    });
    auto ref = cas::system::create<faulty<cas::actor>>();
    auto& a = ref.get_checked<faulty<cas::actor>>();
    a.what = fault::handler;
    cas::system::start();

    ref.tell(boom{});
    ref.tell(ok{});
    REQUIRE(wait_for([&] { return a.handled.load() == 1; }));
    REQUIRE(cas::system::handler_exception_count() == before + 1);
}

// ---------------------------------------------------------------------------
// Construction and on_start(): propagate to the caller of create()/start()
// ---------------------------------------------------------------------------

TEST_CASE("Matrix: a throwing constructor propagates out of create()",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    cas::system::start();
    const size_t before = cas::system::actor_count();
    REQUIRE_THROWS_AS(cas::system::create<throws_in_ctor>(), std::runtime_error);
    REQUIRE(cas::system::actor_count() == before);
}

TEST_CASE("Matrix: a throwing on_start propagates out of create() at runtime",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    cas::system::start();
    const size_t before = cas::system::actor_count();
    REQUIRE_THROWS_AS(cas::system::create<throws_in_on_start>(), std::runtime_error);
    REQUIRE_THROWS_AS(cas::system::create<fast_throws_in_on_start>(), std::runtime_error);
    REQUIRE(cas::system::actor_count() == before);  // not left behind half-created

    // ...so shutdown does not wait out a drain timeout for them
    const auto t0 = std::chrono::steady_clock::now();
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

TEST_CASE("Matrix: a throwing on_start propagates out of start()",
          "[15_failure_modes][invariant7]") {
    CAS_CONFIG_GUARD();
    cas::system::create<throws_in_on_start>();
    REQUIRE_THROWS_AS(cas::system::start(), std::runtime_error);
    // The guard shuts the half-started system down; reaching the end without
    // a hang or crash is part of the check.
}
