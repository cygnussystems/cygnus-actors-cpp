#include "../test_common.h"

// Level 4: Advanced - Stateful actor with selective message processing

namespace stateful_test {
    struct start_download : public cas::message_base {
        int file_id;
    };

    struct cancel_download : public cas::message_base {};

    struct download_complete : public cas::message_base {};

    class downloader : public cas::stateful_actor {
    public:
        enum class state { idle, downloading };

    private:
        state m_state = state::idle;
        int m_downloads_completed = 0;
        int m_downloads_cancelled = 0;
        int m_current_file_id = -1;

    protected:
        void on_start() override {
            set_name("downloader");

            // Register handlers
            handler<start_download>(&downloader::on_start_download);
            handler<cancel_download>(&downloader::on_cancel);
            handler<download_complete>(&downloader::on_complete);

            // Start in idle state
            enter_idle_state();
        }

        void enter_idle_state() {
            m_state = state::idle;
            accept_message_type<start_download>();
            reject_message_type<cancel_download>();
            reject_message_type<download_complete>();
        }

        void enter_downloading_state() {
            m_state = state::downloading;
            reject_message_type<start_download>();
            accept_message_type<cancel_download>();
            accept_message_type<download_complete>();
        }

        void on_start_download(const start_download& msg) {
            m_current_file_id = msg.file_id;
            enter_downloading_state();
        }

        void on_cancel(const cancel_download& /*msg*/) {
            m_downloads_cancelled++;
            m_current_file_id = -1;
            enter_idle_state();
        }

        void on_complete(const download_complete& /*msg*/) {
            m_downloads_completed++;
            m_current_file_id = -1;
            enter_idle_state();
        }

    public:
        int get_downloads_completed() const { return m_downloads_completed; }
        int get_downloads_cancelled() const { return m_downloads_cancelled; }
        state get_state() const { return m_state; }
    };
}

TEST_CASE("Stateful actor rejects messages in wrong state", "[04_advanced][stateful]") {
    using namespace stateful_test;

    auto dl = cas::system::create<downloader>();

    cas::system::start();
    wait_ms(50);

    auto dl_ref = cas::actor_registry::get("downloader");
    REQUIRE(dl_ref.is_valid());

    // Send cancel while idle - should be queued but not processed
    cancel_download cancel_msg;
    dl_ref.tell(cancel_msg);

    wait_ms(100);

    // Verify cancel was not processed (still in idle state)
    auto& dl_actor = dl.get_checked<downloader>();
    REQUIRE(dl_actor.get_downloads_cancelled() == 0);

    TEST_CLEANUP();
}

TEST_CASE("Stateful actor processes deferred messages when state changes", "[04_advanced][stateful]") {
    using namespace stateful_test;

    auto dl = cas::system::create<downloader>();

    cas::system::start();
    wait_ms(50);

    auto dl_ref = cas::actor_registry::get("downloader");
    REQUIRE(dl_ref.is_valid());

    // Send cancel while idle (will be queued)
    cancel_download cancel_msg;
    dl_ref.tell(cancel_msg);

    wait_ms(50);

    // Start download (should process, then process queued cancel)
    start_download start_msg;
    start_msg.file_id = 42;
    dl_ref.tell(start_msg);

    wait_ms(100);

    // The cancel should have been processed after entering downloading state
    auto& dl_actor = dl.get_checked<downloader>();
    REQUIRE(dl_actor.get_downloads_cancelled() == 1);

    TEST_CLEANUP();
}

TEST_CASE("Stateful actor only processes accepted message types", "[04_advanced][stateful]") {
    using namespace stateful_test;

    auto dl = cas::system::create<downloader>();

    cas::system::start();
    wait_ms(50);

    auto dl_ref = cas::actor_registry::get("downloader");
    REQUIRE(dl_ref.is_valid());

    // Start download
    start_download start_msg;
    start_msg.file_id = 123;
    dl_ref.tell(start_msg);

    wait_ms(50);

    // Try to start another download while downloading (should be queued/ignored)
    start_download start_msg2;
    start_msg2.file_id = 456;
    dl_ref.tell(start_msg2);

    // Complete the download
    download_complete complete_msg;
    dl_ref.tell(complete_msg);

    wait_ms(100);

    auto& dl_actor = dl.get_checked<downloader>();
    REQUIRE(dl_actor.get_downloads_completed() == 1);
    // Second start should now be processed
    REQUIRE(dl_actor.get_state() == downloader::state::downloading);

    TEST_CLEANUP();
}
