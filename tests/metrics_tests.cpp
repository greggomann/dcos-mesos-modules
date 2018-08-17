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
const string API_PATH = "container";
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
"         \"value\": \"/" + METRICS_PROCESS + "/" + API_PATH + "\"},\n"
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

  MOCK_METHOD1(container, Future<process::http::Response>(
      const process::http::Request&));

protected:
  void initialize() override
  {
    route("/" + API_PATH, None(), &MockMetricsServiceProcess::container);
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

} // namespace tests {
} // namespace metrics {
} // namespace mesos {
