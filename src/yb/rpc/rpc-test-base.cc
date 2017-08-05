//
// Copyright (c) YugaByte, Inc.
//

#include "yb/rpc/rpc-test-base.h"

#include <thread>

#include "yb/util/random_util.h"

using namespace std::chrono_literals;

namespace yb { namespace rpc {

using yb::rpc_test::CalculatorServiceIf;
using yb::rpc_test::CalculatorError;

using yb::rpc_test::AddRequestPB;
using yb::rpc_test::AddResponsePB;
using yb::rpc_test::EchoRequestPB;
using yb::rpc_test::EchoResponsePB;
using yb::rpc_test::ForwardRequestPB;
using yb::rpc_test::ForwardResponsePB;
using yb::rpc_test::PanicRequestPB;
using yb::rpc_test::PanicResponsePB;
using yb::rpc_test::SendStringsRequestPB;
using yb::rpc_test::SendStringsResponsePB;
using yb::rpc_test::SleepRequestPB;
using yb::rpc_test::SleepResponsePB;
using yb::rpc_test::WhoAmIRequestPB;
using yb::rpc_test::WhoAmIResponsePB;
using yb::rpc_test::PingRequestPB;
using yb::rpc_test::PingResponsePB;
using yb::rpc_test::DisconnectRequestPB;
using yb::rpc_test::DisconnectResponsePB;

using yb::rpc_test_diff_package::ReqDiffPackagePB;
using yb::rpc_test_diff_package::RespDiffPackagePB;

namespace {

constexpr size_t kQueueLength = 50;

Slice GetSidecarPointer(const RpcController& controller, int idx, int expected_size) {
  Slice sidecar;
  CHECK_OK(controller.GetSidecar(idx, &sidecar));
  CHECK_EQ(expected_size, sidecar.size());
  return Slice(sidecar.data(), expected_size);
}

std::shared_ptr<Messenger> CreateMessenger(const std::string& name,
                                           const scoped_refptr<MetricEntity>& metric_entity,
                                           const MessengerOptions& options) {
  MessengerBuilder bld(name);
  bld.set_num_reactors(options.n_reactors);
  static constexpr std::chrono::milliseconds kMinCoarseTimeGranularity(1);
  static constexpr std::chrono::milliseconds kMaxCoarseTimeGranularity(100);
  auto coarse_time_granularity = std::max(std::min(options.keep_alive_timeout,
                                                   kMaxCoarseTimeGranularity),
                                          kMinCoarseTimeGranularity);
  VLOG(1) << "Creating a messenger with connection keep alive time: "
          << options.keep_alive_timeout.count() << " ms, "
          << "coarse time granularity: " << coarse_time_granularity.count() << " ms";
  bld.set_connection_keepalive_time(options.keep_alive_timeout);
  bld.set_coarse_timer_granularity(coarse_time_granularity);
  bld.set_metric_entity(metric_entity);
  std::shared_ptr<Messenger> messenger;
  CHECK_OK(bld.Build(&messenger));
  return messenger;
}

#ifdef THREAD_SANITIZER
constexpr std::chrono::milliseconds kDefaultKeepAlive = 15s;
#else
constexpr std::chrono::milliseconds kDefaultKeepAlive = 1s;
#endif

} // namespace

const MessengerOptions kDefaultClientMessengerOptions = {1, kDefaultKeepAlive};
const MessengerOptions kDefaultServerMessengerOptions = {3, kDefaultKeepAlive};

const char* GenericCalculatorService::kFullServiceName = "yb.rpc.GenericCalculatorService";
const char* GenericCalculatorService::kAddMethodName = "Add";
const char* GenericCalculatorService::kSleepMethodName = "Sleep";
const char* GenericCalculatorService::kSendStringsMethodName = "SendStrings";

const char* GenericCalculatorService::kFirstString =
    "1111111111111111111111111111111111111111111111111111111111";
const char* GenericCalculatorService::kSecondString =
    "2222222222222222222222222222222222222222222222222222222222222222222222";

void GenericCalculatorService::Handle(InboundCallPtr incoming) {
  if (incoming->method_name() == kAddMethodName) {
    DoAdd(incoming.get());
  } else if (incoming->method_name() == kSleepMethodName) {
    DoSleep(incoming.get());
  } else if (incoming->method_name() == kSendStringsMethodName) {
    DoSendStrings(incoming.get());
  } else {
    incoming->RespondFailure(ErrorStatusPB::ERROR_NO_SUCH_METHOD,
        STATUS(InvalidArgument, "bad method"));
  }
}

void GenericCalculatorService::GenericCalculatorService::DoAdd(InboundCall* incoming) {
  Slice param(incoming->serialized_request());
  AddRequestPB req;
  if (!req.ParseFromArray(param.data(), param.size())) {
    LOG(FATAL) << "couldn't parse: " << param.ToDebugString();
  }

  AddResponsePB resp;
  resp.set_result(req.x() + req.y());
  down_cast<YBInboundCall*>(incoming)->RespondSuccess(resp);
}

void GenericCalculatorService::DoSendStrings(InboundCall* incoming) {
  Slice param(incoming->serialized_request());
  SendStringsRequestPB req;
  if (!req.ParseFromArray(param.data(), param.size())) {
    LOG(FATAL) << "couldn't parse: " << param.ToDebugString();
  }

  Random r(req.random_seed());
  SendStringsResponsePB resp;
  for (auto size : req.sizes()) {
    auto sidecar = RefCntBuffer(size);
    RandomString(sidecar.udata(), size, &r);
    int idx = 0;
    auto status = down_cast<YBInboundCall*>(incoming)->AddRpcSidecar(sidecar, &idx);
    if (!status.ok()) {
      incoming->RespondFailure(ErrorStatusPB::ERROR_APPLICATION, status);
      return;
    }
    resp.add_sidecars(idx);
  }

  down_cast<YBInboundCall*>(incoming)->RespondSuccess(resp);
}

void GenericCalculatorService::DoSleep(InboundCall* incoming) {
  Slice param(incoming->serialized_request());
  SleepRequestPB req;
  if (!req.ParseFromArray(param.data(), param.size())) {
    incoming->RespondFailure(ErrorStatusPB::ERROR_INVALID_REQUEST,
        STATUS(InvalidArgument, "Couldn't parse pb",
            req.InitializationErrorString()));
    return;
  }

  LOG(INFO) << "got call: " << req.ShortDebugString();
  SleepFor(MonoDelta::FromMicroseconds(req.sleep_micros()));
  SleepResponsePB resp;
  down_cast<YBInboundCall*>(incoming)->RespondSuccess(resp);
}

namespace {

class CalculatorService: public CalculatorServiceIf {
 public:
  explicit CalculatorService(const scoped_refptr<MetricEntity>& entity,
                             std::string name)
      : CalculatorServiceIf(entity), name_(std::move(name)) {
  }

  void SetMessenger(const std::weak_ptr<Messenger>& messenger) {
    messenger_ = messenger;
  }

  void Add(const AddRequestPB* req, AddResponsePB* resp, RpcContext context) override {
    resp->set_result(req->x() + req->y());
    context.RespondSuccess();
  }

  void Sleep(const SleepRequestPB* req, SleepResponsePB* resp, RpcContext context) override {
    if (req->return_app_error()) {
      CalculatorError my_error;
      my_error.set_extra_error_data("some application-specific error data");
      context.RespondApplicationError(CalculatorError::app_error_ext.number(),
          "Got some error", my_error);
      return;
    }

    // Respond w/ error if the RPC specifies that the client deadline is set,
    // but it isn't.
    if (req->client_timeout_defined()) {
      MonoTime deadline = context.GetClientDeadline();
      if (deadline.Equals(MonoTime::Max())) {
        CalculatorError my_error;
        my_error.set_extra_error_data("Timeout not set");
        context.RespondApplicationError(CalculatorError::app_error_ext.number(),
            "Missing required timeout", my_error);
        return;
      }
    }

    if (req->deferred()) {
      // Spawn a new thread which does the sleep and responds later.
      std::thread thread([this, req, context = std::move(context)]() mutable {
        DoSleep(req, std::move(context));
      });
      thread.detach();
      return;
    }
    DoSleep(req, std::move(context));
  }

  void Echo(const EchoRequestPB* req, EchoResponsePB* resp, RpcContext context) override {
    resp->set_data(req->data());
    context.RespondSuccess();
  }

  void WhoAmI(const WhoAmIRequestPB* req, WhoAmIResponsePB* resp, RpcContext context) override {
    const UserCredentials& creds = context.user_credentials();
    if (creds.has_effective_user()) {
      resp->mutable_credentials()->set_effective_user(creds.effective_user());
    }
    resp->mutable_credentials()->set_real_user(creds.real_user());
    resp->set_address(yb::ToString(context.remote_address()));
    context.RespondSuccess();
  }

  void TestArgumentsInDiffPackage(
      const ReqDiffPackagePB* req, RespDiffPackagePB* resp, RpcContext context) override {
    context.RespondSuccess();
  }

  void Panic(const PanicRequestPB* req, PanicResponsePB* resp, RpcContext context) override {
    TRACE("Got panic request");
    PANIC_RPC(&context, "Test method panicking!");
  }

  void Ping(const PingRequestPB* req, PingResponsePB* resp, RpcContext context) override {
    auto now = MonoTime::Now(MonoTime::FINE);
    resp->set_time(now.ToUint64());
    context.RespondSuccess();
  }

  void Disconnect(
      const DisconnectRequestPB* peq, DisconnectResponsePB* resp, RpcContext context) override {
    context.CloseConnection();
    context.RespondSuccess();
  }

  void Forward(const ForwardRequestPB* req, ForwardResponsePB* resp, RpcContext context) override {
    if (!req->has_host() || !req->has_port()) {
      resp->set_name(name_);
      context.RespondSuccess();
      return;
    }
    auto messenger = messenger_.lock();
    YB_ASSERT_TRUE(messenger);
    boost::system::error_code ec;
    Endpoint endpoint(IpAddress::from_string(req->host(), ec), req->port());
    if (ec) {
      context.RespondFailure(STATUS_SUBSTITUTE(NetworkError, "Invalid host: $0", ec.message()));
      return;
    }
    rpc_test::CalculatorServiceProxy proxy(messenger, endpoint);

    ForwardRequestPB forwarded_req;
    ForwardResponsePB forwarded_resp;
    RpcController controller;
    auto status = proxy.Forward(forwarded_req, &forwarded_resp, &controller);
    if (!status.ok()) {
      context.RespondFailure(status);
    } else {
      resp->set_name(forwarded_resp.name());
      context.RespondSuccess();
    }
  }

 private:
  void DoSleep(const SleepRequestPB* req, RpcContext context) {
    SleepFor(MonoDelta::FromMicroseconds(req->sleep_micros()));
    context.RespondSuccess();
  }

  std::string name_;
  std::weak_ptr<Messenger> messenger_;
};

} // namespace

std::unique_ptr<ServiceIf> CreateCalculatorService(
    const scoped_refptr<MetricEntity>& metric_entity, std::string name) {
  return std::unique_ptr<ServiceIf>(new CalculatorService(metric_entity, std::move(name)));
}

TestServer::TestServer(std::unique_ptr<ServiceIf> service,
                       const scoped_refptr<MetricEntity>& metric_entity,
                       const TestServerOptions& options)
    : service_name_(service->service_name()),
      messenger_(CreateMessenger("TestServer",
                                 metric_entity,
                                 options.messenger_options)),
      thread_pool_("rpc-test", kQueueLength, options.n_worker_threads) {

  // If it is CalculatorService then we should set messenger for it.
  CalculatorService* calculator_service = dynamic_cast<CalculatorService*>(service.get());
  if (calculator_service) {
    calculator_service->SetMessenger(messenger_);
  }

  service_pool_.reset(new ServicePool(kQueueLength,
                                      &thread_pool_,
                                      std::move(service),
                                      messenger_->metric_entity()));

  EXPECT_OK(messenger_->ListenAddress(options.endpoint, &bound_endpoint_));
  EXPECT_OK(messenger_->RegisterService(service_name_, service_pool_));
  EXPECT_OK(messenger_->StartAcceptor());
}

TestServer::~TestServer() {
  thread_pool_.Shutdown();
  if (service_pool_) {
    const Status unregister_service_status = messenger_->UnregisterService(service_name_);
    if (!unregister_service_status.IsServiceUnavailable()) {
      EXPECT_OK(unregister_service_status);
    }
    service_pool_->Shutdown();
  }
  if (messenger_) {
    messenger_->Shutdown();
  }
}

void TestServer::Shutdown() {
  ASSERT_OK(messenger_->UnregisterService(service_name_));
  service_pool_->Shutdown();
  messenger_->Shutdown();
}

RpcTestBase::RpcTestBase()
    : metric_entity_(METRIC_ENTITY_server.Instantiate(&metric_registry_, "test.rpc_test")) {
}

void RpcTestBase::TearDown() {
  server_.reset();
  YBTest::TearDown();
}

CHECKED_STATUS RpcTestBase::DoTestSyncCall(const Proxy& p, const char* method) {
  AddRequestPB req;
  req.set_x(RandomUniformInt<uint32_t>());
  req.set_y(RandomUniformInt<uint32_t>());
  AddResponsePB resp;
  RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(10000));
  RETURN_NOT_OK(p.SyncRequest(method, req, &resp, &controller));

  LOG(INFO) << "Result: " << resp.ShortDebugString();
  CHECK_EQ(req.x() + req.y(), resp.result());
  return Status::OK();
}

void RpcTestBase::DoTestSidecar(const Proxy& p,
                                std::vector<size_t> sizes,
                                Status::Code expected_code) {
  const uint32_t kSeed = 12345;

  SendStringsRequestPB req;
  for (auto size : sizes) {
    req.add_sizes(size);
  }
  req.set_random_seed(kSeed);

  SendStringsResponsePB resp;
  RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(10000));
  auto status = p.SyncRequest(GenericCalculatorService::kSendStringsMethodName,
      req,
      &resp,
      &controller);

  ASSERT_EQ(expected_code, status.code()) << "Invalid status received: " << status.ToString();

  if (!status.ok()) {
    return;
  }

  Random rng(kSeed);
  faststring expected;
  for (size_t i = 0; i != sizes.size(); ++i) {
    size_t size = sizes[i];
    expected.resize(size);
    Slice sidecar = GetSidecarPointer(controller, resp.sidecars(i), size);
    RandomString(expected.data(), size, &rng);
    ASSERT_EQ(0, sidecar.compare(expected)) << "Invalid sidecar at " << i << " position";
  }
}

void RpcTestBase::DoTestExpectTimeout(const Proxy& p, const MonoDelta& timeout) {
  SleepRequestPB req;
  SleepResponsePB resp;
  req.set_sleep_micros(500000); // 0.5sec

  RpcController c;
  c.set_timeout(timeout);
  Stopwatch sw;
  sw.start();
  Status s = p.SyncRequest(GenericCalculatorService::kSleepMethodName, req, &resp, &c);
  ASSERT_FALSE(s.ok());
  sw.stop();

  int expected_millis = timeout.ToMilliseconds();
  int elapsed_millis = sw.elapsed().wall_millis();

  // We shouldn't timeout significantly faster than our configured timeout.
  EXPECT_GE(elapsed_millis, expected_millis - 10);
  // And we also shouldn't take the full 0.5sec that we asked for
  EXPECT_LT(elapsed_millis, 500);
  EXPECT_TRUE(s.IsTimedOut());
  LOG(INFO) << "status: " << s.ToString() << ", seconds elapsed: " << sw.elapsed().wall_seconds();
}

void RpcTestBase::StartTestServer(Endpoint* server_endpoint, const TestServerOptions& options) {
  std::unique_ptr<ServiceIf> service(new GenericCalculatorService(metric_entity_));
  server_.reset(new TestServer(std::move(service), metric_entity_, options));
  *server_endpoint = server_->bound_endpoint();
}

void RpcTestBase::StartTestServerWithGeneratedCode(Endpoint* server_endpoint,
                                                   const TestServerOptions& options) {
  server_.reset(new TestServer(CreateCalculatorService(metric_entity_), metric_entity_, options));
  *server_endpoint = server_->bound_endpoint();
}

CHECKED_STATUS RpcTestBase::StartFakeServer(Socket* listen_sock, Endpoint* listen_endpoint) {
  RETURN_NOT_OK(listen_sock->Init(0));
  RETURN_NOT_OK(listen_sock->BindAndListen(Endpoint(), 1));
  RETURN_NOT_OK(listen_sock->GetSocketAddress(listen_endpoint));
  LOG(INFO) << "Bound to: " << *listen_endpoint;
  return Status::OK();
}

std::shared_ptr<Messenger> RpcTestBase::CreateMessenger(const string &name,
                                                        const MessengerOptions& options) {
  return yb::rpc::CreateMessenger(name, metric_entity_, options);
}

} // namespace rpc
} // namespace yb
