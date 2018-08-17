#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/isolator.hpp>
#include <mesos/module/module.hpp>

#include <mesos/slave/containerizer.hpp>
#include <mesos/slave/isolator.hpp>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/read.hpp>

#include "common/http.hpp"

#include "metrics/messages.pb.h"
#include "metrics/isolator.hpp"

#include "module/manager.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::tests;

using mesos::modules::ModuleManager;

using mesos::modules::metrics::ContainerStartRequest;
using mesos::modules::metrics::ContainerStartResponse;
using mesos::modules::metrics::LegacyState;

using mesos::slave::Isolator;

using mesosphere::dcos::metrics::MetricsIsolator;
using mesosphere::dcos::metrics::parse;

using process::Clock;
using process::Future;
using process::Owned;

using std::string;
using std::vector;

using testing::DoAll;
using testing::Return;

namespace mesos {
namespace metrics {
namespace tests {

const string METRICS_PROCESS = "metrics-service";
const string API_PATH = "/containers";
const string LEGACY_HOST = "metricshost";
const Duration REQUEST_TIMEOUT = Seconds(5);

// A map of container IDs and their associated ports for testing.


class MetricsTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // The metrics module will only set its legacy state path data member if the
    // directory exists at the time the module is loaded, so we create it now.
    statePath = path::join(os::getcwd(), "state");
    os::mkdir(path::join(statePath, "containers"));

    // Construct the metrics module configuration.
    const string ip = stringify(process::address().ip);
    const string port = stringify(process::address().port);
    const string config =
"{\n"
"  \"libraries\": [ {\n"
"    \"file\": \"" + path::join(MODULES_BUILD_DIR,
                                ".libs",
                                "libmetrics-module.so") + "\",\n"
"    \"modules\": [ {\n"
"      \"name\": \"com_mesosphere_dcos_MetricsIsolatorModule\",\n"
"      \"parameters\": [\n"
"        {\"key\": \"dcos_metrics_service_scheme\", \"value\": \"http\"},\n"
"        {\"key\": \"dcos_metrics_service_network\", \"value\": \"inet\"},\n"
"        {\"key\": \"dcos_metrics_service_address\",\n"
"         \"value\": \"" + ip + ":" + port + "\"},\n"
"        {\"key\": \"dcos_metrics_service_endpoint\",\n"
"         \"value\": \"/" + path::join(METRICS_PROCESS, API_PATH) + "\"},\n"
"        {\"key\": \"legacy_state_path_dir\",\n"
"         \"value\": \"" + statePath + "\"},\n"
"        {\"key\": \"request_timeout\",\n"
"         \"value\": \"" + stringify(REQUEST_TIMEOUT) + "\"}\n"
"      ]\n"
"    } ]\n"
"  } ]\n"
"}";

    Try<JSON::Object> json = JSON::parse<JSON::Object>(config);
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Initialize the metrics module.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);

    // Create an instance of the module and store it for testing.
    Try<Isolator*> isolator_ =
      ModuleManager::create<Isolator>(
          "com_mesosphere_dcos_MetricsIsolatorModule");

    ASSERT_SOME(isolator_);
    isolator = isolator_.get();
  }

  virtual void TearDown()
  {
    // Unload the metrics module.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(ModuleManager::unload(module.name()));
        }
      }
    }

    MesosTest::TearDown();
  }

  Isolator* isolator;
  string statePath;

private:
  Modules modules;
};


// Simulates the DC/OS metrics service.
class MockMetricsServiceProcess
  : public process::Process<MockMetricsServiceProcess>
{
public:
  MockMetricsServiceProcess() : ProcessBase(METRICS_PROCESS) {}

  MOCK_METHOD1(containers, Future<process::http::Response>(
      const process::http::Request&));

protected:
  void initialize() override
  {
    route(API_PATH, None(), &MockMetricsServiceProcess::containers);
  }
};


class MockMetricsService
{
public:
  MockMetricsService() : mock(new MockMetricsServiceProcess())
  {
    spawn(mock.get());
  }

  ~MockMetricsService()
  {
    terminate(mock.get());
    wait(mock.get());
  }

  Owned<MockMetricsServiceProcess> mock;
};


// This class encapsulates the stored state associated with
// an instance of the legacy DC/OS metrics Mesos module.
class LegacyStateStore
{
public:
  LegacyStateStore(
      const hashmap<string, int>& containers_,
      const string& statePath_)
    : containers(containers_),
      statePath(statePath_)
  {
    foreachpair (const string& containerId, const int port, containers) {
      LegacyState state;
      state.set_container_id(containerId);
      state.set_statsd_host(LEGACY_HOST);
      state.set_statsd_port(port);

      os::write(
          path::join(statePath, "containers", containerId),
          mesos::internal::serialize(ContentType::JSON, state));
    }
  }

  ~LegacyStateStore()
  {
    foreachkey (const string& containerId, containers) {
      os::rm(path::join(statePath, "containers", containerId));
    }
  }

private:
  const hashmap<string, int> containers;
  const string statePath;
};


// When the isolator's `prepare()` method is called with a container ID, it
// should make an API call to the metrics service to retrieve the metrics port
// for that container, and then inject the correct host and port into the
// environment contained in the returned `ContainerLaunchInfo`.
TEST_F(MetricsTest, PrepareSuccess)
{
  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_port(1111);
  responseBody.set_statsd_host("127.0.0.1");

  process::http::Response response(
      string(jsonify(JSON::Protobuf(responseBody))),
      process::http::Status::CREATED,
      "application/json");

  Future<process::http::Request> request;
  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(DoAll(FutureArg<0>(&request),
                    Return(response)));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_READY(request);

  // Verify the contents of the module's request.
  Try<ContainerStartRequest> requestBody =
    parse<ContainerStartRequest>(request->body);

  ASSERT_SOME(requestBody);
  ASSERT_EQ(request->method, "POST");
  ASSERT_FALSE(requestBody->has_statsd_host());
  ASSERT_FALSE(requestBody->has_statsd_port());
  ASSERT_EQ(requestBody->container_id(), CONTAINER_ID);

  AWAIT_READY(prepared);

  // Check that the `ContainerLaunchInfo` contains environment variables.
  ASSERT_SOME(prepared.get());
  ASSERT_TRUE(prepared.get()->has_environment());
  ASSERT_GT(prepared.get()->environment().variables().size(), 0);

  // Verify the contents of the returned environment.
  hashmap<string, string> expectedEnvironment =
    {{"STATSD_UDP_HOST", "127.0.0.1"},
     {"STATSD_UDP_PORT", "1111"}};

  foreach (const Environment::Variable& variable,
           prepared.get()->environment().variables()) {
    ASSERT_EQ(variable.type(), Environment::Variable::VALUE);
    ASSERT_TRUE(variable.has_value());
    ASSERT_EQ(variable.value(), expectedEnvironment[variable.name()]);
  }
}


// When the isolator's `prepare()` method is called and the metrics service
// returns a response which does not contain a port, the isolator should return
// a failed future.
TEST_F(MetricsTest, PrepareFailureNoPort)
{
  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_host("127.0.0.1");

  process::http::Response response(
      string(jsonify(JSON::Protobuf(responseBody))),
      process::http::Status::CREATED,
      "application/json");

  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(Return(response));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_FAILED(prepared);
}


// When the isolator's `prepare()` method is called and the metrics service
// returns a response code other than 201 CREATED, the isolator should return
// a failed future.
TEST_F(MetricsTest, PrepareFailureUnexpectedStatusCode)
{
  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_port(1111);
  responseBody.set_statsd_host("127.0.0.1");

  process::http::Response response(
      string(jsonify(JSON::Protobuf(responseBody))),
      process::http::Status::OK,
      "application/json");

  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(Return(response));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_FAILED(prepared);
}


// When the isolator's `prepare()` method is called and the metrics service
// doesn't return a response within the configured request timeout, the isolator
// should return a failed future.
TEST_F(MetricsTest, PrepareFailureRequestTimeout)
{
  Clock::pause();

  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_port(1111);
  responseBody.set_statsd_host("127.0.0.1");

  Future<process::http::Response> response;

  Future<Nothing> requestSent;
  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(DoAll(FutureSatisfy(&requestSent),
                    Return(response)));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_READY(requestSent);

  ASSERT_TRUE(prepared.isPending());

  Clock::advance(REQUEST_TIMEOUT);
  Clock::settle();

  ASSERT_TRUE(prepared.isFailed());
}


// When the isolator's `cleanup()` method is called and the module receives a
// 202 ACCEPTED response from the metrics service, the isolator should return a
// ready future.
TEST_F(MetricsTest, CleanupSuccess)
{
  MockMetricsService metricsService;

  process::http::Response response(process::http::Status::ACCEPTED);

  Future<process::http::Request> request;
  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(DoAll(FutureArg<0>(&request),
                    Return(response)));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Nothing> cleanedup = isolator->cleanup(containerId);

  AWAIT_READY(request);

  ASSERT_EQ(request->method, "DELETE");

  AWAIT_READY(cleanedup);
}


// When the isolator's `cleanup()` method is called and the module receives an
// unexpected response code from the metrics service, the isolator should return
// a failed future.
TEST_F(MetricsTest, CleanupFailureUnexpectedStatusCode)
{
  MockMetricsService metricsService;

  process::http::Response response(process::http::Status::OK);

  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(Return(response));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Nothing> cleanedup = isolator->cleanup(containerId);

  AWAIT_FAILED(cleanedup);
}


// When the isolator's `recover()` method is called and it finds legacy state
// on disk, it should make API requests to the metrics service to transfer
// ownership of the checkpointed container metrics state.
TEST_F(MetricsTest, RecoverSuccessWithLegacyState)
{
  MockMetricsService metricsService;

  const hashmap<string, int> containers =
    {{"container1", 1234}, {"container2", 5678}};

  LegacyStateStore stateStore(containers, statePath);

  // Check that the container state directory exists. We will confirm that it
  // has been moved after recovery.
  ASSERT_TRUE(os::exists(path::join(statePath, "containers")));

  process::http::Response response(process::http::Status::CREATED);

  ContainerStartResponse responseBody;
  response.body = string(jsonify(JSON::Protobuf(responseBody)));

  Future<process::http::Request> request1;
  Future<process::http::Request> request2;
  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(DoAll(FutureArg<0>(&request1),
                    Return(response)))
    .WillOnce(DoAll(FutureArg<0>(&request2),
                    Return(response)));

  // Construct the agent's recovered state, which is passed
  // into the module's `recover()` method.
  vector<mesos::slave::ContainerState> agentState;

  foreachkey (const string& containerId_, containers) {
    ContainerID containerId;
    containerId.set_value(containerId_);

    mesos::slave::ContainerState containerState;

    containerState.mutable_container_id()->CopyFrom(containerId);
    containerState.set_pid(0);
    containerState.set_directory(os::getcwd());

    agentState.push_back(containerState);
  }

  Future<Nothing> recovered = isolator->recover(agentState, {});

  // Verify the contents of the module's requests.
  vector<Future<process::http::Request>> requests{request1, request2};
  foreach (const Future<process::http::Request>& request, requests) {
    AWAIT_READY(request);

    Try<ContainerStartRequest> requestBody =
      parse<ContainerStartRequest>(request->body);

    ASSERT_SOME(requestBody);
    ASSERT_EQ(request->method, "POST");

    ASSERT_TRUE(requestBody->has_statsd_host());
    ASSERT_EQ(requestBody->statsd_host(), LEGACY_HOST);

    ASSERT_TRUE(requestBody->has_statsd_port());
    ASSERT_TRUE(containers.contains(requestBody->container_id()));
    ASSERT_EQ(
        requestBody->statsd_port(),
        containers.at(requestBody->container_id()));
  }

  AWAIT_READY(recovered);

  ASSERT_FALSE(os::exists(path::join(statePath, "containers")));
}


// When the isolator's `recover()` method is called and it finds no legacy
// state on disk, it should make no API requests and return a ready future.
TEST_F(MetricsTest, RecoverSuccessWithoutLegacyState)
{
  Clock::pause();

  MockMetricsService metricsService;

  EXPECT_CALL(*metricsService.mock, containers(_))
    .Times(0);

  vector<mesos::slave::ContainerState> agentState;

  Future<Nothing> recovered = isolator->recover(agentState, {});

  AWAIT_READY(recovered);

  // Settle the clock to ensure that the metrics API is not called.
  Clock::settle();
}


// When the isolator's `recover()` method is called and it finds no legacy
// state on disk and no containers are passed to it for recovery, it should
// return a ready future.
TEST_F(MetricsTest, RecoverSuccessWithoutLegacyDirectory)
{
  Clock::pause();

  MockMetricsService metricsService;

  os::rm(statePath);

  EXPECT_CALL(*metricsService.mock, containers(_))
    .Times(0);

  vector<mesos::slave::ContainerState> agentState;

  Future<Nothing> recovered = isolator->recover(agentState, {});

  AWAIT_READY(recovered);

  // Settle the clock to ensure that the metrics API is not called.
  Clock::settle();
}


// When the isolator's `recover()` method is called and it finds legacy state
// on disk and subsequent requests to the metrics API return an unexpected
// status code, it should return a ready future.
TEST_F(MetricsTest, RecoverSuccessUnexpectedStatusCode)
{
  MockMetricsService metricsService;

  const hashmap<string, int> containers = {{"container1", 1234}};

  LegacyStateStore stateStore(containers, statePath);

  // Check that the container state directory exists. We will confirm that it
  // has been moved after recovery.
  ASSERT_TRUE(os::exists(path::join(statePath, "containers")));

  process::http::Response response(process::http::Status::OK);

  ContainerStartResponse responseBody;
  response.body = string(jsonify(JSON::Protobuf(responseBody)));

  Future<process::http::Request> request;
  EXPECT_CALL(*metricsService.mock, containers(_))
    .WillOnce(DoAll(FutureArg<0>(&request),
                    Return(response)));

  // Construct the agent's recovered state, which is passed
  // into the module's `recover()` method.
  vector<mesos::slave::ContainerState> agentState;

  foreachkey (const string& containerId_, containers) {
    ContainerID containerId;
    containerId.set_value(containerId_);

    mesos::slave::ContainerState containerState;

    containerState.mutable_container_id()->CopyFrom(containerId);
    containerState.set_pid(0);
    containerState.set_directory(os::getcwd());

    agentState.push_back(containerState);
  }

  Future<Nothing> recovered = isolator->recover(agentState, {});

  AWAIT_READY(recovered);

  ASSERT_FALSE(os::exists(path::join(statePath, "containers")));
}

} // namespace tests {
} // namespace metrics {
} // namespace mesos {
