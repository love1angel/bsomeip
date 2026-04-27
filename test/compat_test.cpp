// SPDX-License-Identifier: MIT
// Tests for vsomeip compatibility shim.
#include <bsomeip/compat/vsomeip.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ---- Test helpers ----

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do {                                        \
    tests_run++;                                                  \
    fn();                                                         \
    tests_passed++;                                               \
    std::printf("[PASS] %s\n", #fn);                              \
} while (0)

// ---- Tests ----

void test_runtime_singleton() {
    auto rt1 = vsomeip::runtime::get();
    auto rt2 = vsomeip::runtime::get();
    assert(rt1.get() == rt2.get());
}

void test_create_application() {
    auto rt = vsomeip::runtime::get();
    auto app = rt->create_application("test_app");
    assert(app != nullptr);
    assert(app->get_name() == "test_app");
    assert(app->init() == true);
}

void test_state_handler() {
    auto app = std::make_shared<vsomeip::application>("state_test");
    app->init();

    vsomeip::state_type_e observed = vsomeip::state_type_e::ST_DEREGISTERED;
    app->register_state_handler([&](vsomeip::state_type_e s) {
        observed = s;
    });

    app->start();
    assert(observed == vsomeip::state_type_e::ST_REGISTERED);

    app->stop();
    assert(observed == vsomeip::state_type_e::ST_DEREGISTERED);
}

void test_create_message() {
    auto msg = vsomeip::create_request();
    assert(msg != nullptr);
    assert(msg->get_message_type() == vsomeip::message_type_e::MT_REQUEST);

    msg->set_service(0x1234);
    msg->set_instance(0x0001);
    msg->set_method(0x0421);
    assert(msg->get_service() == 0x1234);
    assert(msg->get_instance() == 0x0001);
    assert(msg->get_method() == 0x0421);
}

void test_create_response() {
    auto req = vsomeip::create_request();
    req->set_service(0x1111);
    req->set_method(0x2222);
    req->set_client(0x0042);
    req->set_session(0x0007);
    req->set_interface_version(1);

    auto resp = vsomeip::create_response(req);
    assert(resp->get_service() == 0x1111);
    assert(resp->get_method() == 0x2222);
    assert(resp->get_client() == 0x0042);
    assert(resp->get_session() == 0x0007);
    assert(resp->get_interface_version() == 1);
    assert(resp->get_message_type() == vsomeip::message_type_e::MT_RESPONSE);
    assert(resp->get_return_code() == vsomeip::return_code_e::E_OK);
}

void test_payload() {
    auto pl = vsomeip::create_payload();
    assert(pl != nullptr);
    assert(pl->get_length() == 0);

    std::uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    pl->set_data(data, 4);
    assert(pl->get_length() == 4);
    assert(pl->get_data()[0] == 0x01);
    assert(pl->get_data()[3] == 0x04);
}

void test_payload_in_message() {
    auto msg = vsomeip::create_request();
    msg->set_service(0xABCD);
    msg->set_method(0x0001);

    auto pl = vsomeip::create_payload();
    std::uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    pl->set_data(data, 4);
    msg->set_payload(pl);

    auto retrieved = msg->get_payload();
    assert(retrieved->get_length() == 4);
    assert(retrieved->get_data()[0] == 0xDE);
    assert(retrieved->get_data()[3] == 0xEF);
}

void test_offer_and_request_service() {
    auto app = std::make_shared<vsomeip::application>("svc_test");
    app->init();

    // Should not throw
    app->offer_service(0x1234, 0x0001, 1, 0);
    app->request_service(0x1234, 0x0001);
    app->release_service(0x1234, 0x0001);
    app->stop_offer_service(0x1234, 0x0001);
}

void test_message_handler_dispatch() {
    auto app = std::make_shared<vsomeip::application>("handler_test");
    app->init();

    bool handler_called = false;
    vsomeip::service_t recv_svc = 0;
    vsomeip::method_t recv_mth = 0;

    app->register_message_handler(0x1234, 0x0001, 0x0421,
        [&](const std::shared_ptr<vsomeip::message>& msg) {
            handler_called = true;
            recv_svc = msg->get_service();
            recv_mth = msg->get_method();
        });

    // Build a raw SOME/IP message and route it
    auto req = vsomeip::create_request();
    req->set_service(0x1234);
    req->set_method(0x0421);
    req->set_client(0x0042);
    req->set_session(0x0001);

    app->send(req);

    assert(handler_called);
    assert(recv_svc == 0x1234);
    assert(recv_mth == 0x0421);
}

void test_request_response_flow() {
    // Server app
    auto server_app = std::make_shared<vsomeip::application>("server");
    server_app->init();
    server_app->offer_service(0x1234, 0x0001);

    bool response_built = false;

    // Server registers handler that builds a response
    server_app->register_message_handler(0x1234, 0x0001, 0x0421,
        [&](const std::shared_ptr<vsomeip::message>& req) {
            // Only handle requests (avoid infinite recursion from response dispatch)
            if (req->get_message_type() != vsomeip::message_type_e::MT_REQUEST)
                return;

            auto resp = vsomeip::create_response(req);
            auto pl = vsomeip::create_payload();
            std::uint8_t data[] = {0x42};
            pl->set_data(data, 1);
            resp->set_payload(pl);

            assert(resp->get_service() == 0x1234);
            assert(resp->get_method() == 0x0421);
            assert(resp->get_session() == 0x0005);
            assert(resp->get_message_type() == vsomeip::message_type_e::MT_RESPONSE);
            auto rpl = resp->get_payload();
            assert(rpl->get_length() == 1);
            assert(rpl->get_data()[0] == 0x42);
            response_built = true;
        });

    // Send a request
    auto req = vsomeip::create_request();
    req->set_service(0x1234);
    req->set_method(0x0421);
    req->set_client(0x0042);
    req->set_session(0x0005);
    server_app->send(req);

    assert(response_built);
}

void test_notification() {
    auto app = std::make_shared<vsomeip::application>("notify_test");
    app->init();

    auto pl = vsomeip::create_payload();
    std::uint8_t data[] = {0x01, 0x02};
    pl->set_data(data, 2);

    // Should not throw
    app->notify(0x1234, 0x0001, 0x8001, pl);
}

void test_native_access() {
    auto app = std::make_shared<vsomeip::application>("native_test");
    app->init();

    // Can access the underlying bsomeip application
    bsomeip::api::application& native = app->native();
    assert(native.config().name == "native_test");

    auto msg = vsomeip::create_request();
    bsomeip::api::message& native_msg = msg->native();
    assert(native_msg.data.size() >= bsomeip::wire::header_size);
}

void test_constants() {
    assert(vsomeip::ANY_SERVICE == 0xFFFF);
    assert(vsomeip::ANY_INSTANCE == 0xFFFF);
    assert(vsomeip::ANY_METHOD == 0xFFFF);
    assert(vsomeip::ANY_MAJOR == 0xFF);
    assert(vsomeip::ANY_MINOR == 0xFFFFFFFF);
}

// ---- Main ----

int main() {
    std::printf("=== vsomeip Compatibility Layer Tests ===\n\n");

    RUN_TEST(test_runtime_singleton);
    RUN_TEST(test_create_application);
    RUN_TEST(test_state_handler);
    RUN_TEST(test_create_message);
    RUN_TEST(test_create_response);
    RUN_TEST(test_payload);
    RUN_TEST(test_payload_in_message);
    RUN_TEST(test_offer_and_request_service);
    RUN_TEST(test_message_handler_dispatch);
    RUN_TEST(test_request_response_flow);
    RUN_TEST(test_notification);
    RUN_TEST(test_native_access);
    RUN_TEST(test_constants);

    std::printf("\n=== %d/%d compat tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
