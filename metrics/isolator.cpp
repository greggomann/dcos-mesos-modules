#include <map>
#include <string>
#include <vector>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/isolator.hpp>
#include <mesos/module/module.hpp>

#include <process/address.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/assert.hpp>
#include <stout/hashset.hpp>
#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "isolator.hpp"

#include "metrics/messages.pb.h"

namespace inet = process::network::inet;
namespace unix = process::network::unix;

using namespace mesos;
using namespace process;

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using mesos::modules::metrics::ContainerStartRequest;
using mesos::modules::metrics::ContainerStartResponse;
using mesos::modules::metrics::ContainerStopRequest;
using mesos::modules::metrics::ContainerStopResponse;
using mesos::modules::metrics::LegacyState;

namespace mesosphere {
namespace dcos {
namespace metrics {

namespace internal {

// TODO(greggomann): Make use of `mesos::internal::serialize` when we get
// access to 'common/http.hpp' from the Mesos codebase.
string serialize(
    ContentType contentType,
    const google::protobuf::Message& message)
{
  switch (contentType) {
    case ContentType::PROTOBUF: {
      return message.SerializeAsString();
    }
    case ContentType::JSON: {
      return jsonify(JSON::Protobuf(message));
    }
    case ContentType::RECORDIO: {
      LOG(FATAL) << "Serializing a RecordIO stream is not supported";
    }
  }

  UNREACHABLE();
}

} // namespace internal {

class MetricsIsolatorProcess
  : public process::Process<MetricsIsolatorProcess>
{
public:
  MetricsIsolatorProcess(const isolator::Flags& _flags) : flags(_flags)
  {
    // Set `serviceScheme` based on flags.
    ASSERT(flags.service_scheme.isSome());
    ASSERT(flags.service_scheme.get() == "http" ||
           flags.service_scheme.get() == "https");
    serviceScheme = flags.service_scheme.get();

    // Set `service*Address` based on flags.
    ASSERT(flags.service_address.isSome());

    ASSERT(flags.service_network.isSome());
    ASSERT(flags.service_network.get() == "inet" ||
           flags.service_network.get() == "unix");

    if (flags.service_network.get() == "inet") {
      vector<string> hostport =
        strings::split(flags.service_address.get(), ":");
      if (hostport.size() != 2) {
        LOG(FATAL) << "Unable to split '" << flags.service_address.get() << "'"
                   << " into valid 'host:port' combination";
      }

      Try<net::IP> ip = net::IP::parse(hostport[0]);
      if (ip.isError()) {
        LOG(FATAL) << "Unable to parse '" << hostport[0] << "'"
                   << " as a valid IP address: " << ip.error();
      }

      Try<uint16_t> port = numify<uint16_t>(hostport[1]);
      if (port.isError()) {
        LOG(FATAL) << "Unable parse '" + hostport[1] + "'"
                   << " as a valid port of type 'uint16_t': " + port.error();
      }

      serviceInetAddress = inet::Address(ip.get(), port.get());
    }

    if (flags.service_network.get() == "unix") {
      Try<unix::Address> address =
        unix::Address::create(flags.service_address.get());
      if (address.isError()) {
        LOG(FATAL) << "Unable to convert '" + flags.service_address.get() + "'"
                   << " to valid 'unix' address: " + address.error();
      }

      serviceUnixAddress = address.get();
    }

    // Set `serviceEndpoint` based on flags.
    ASSERT(flags.service_endpoint.isSome());
    serviceEndpoint = flags.service_endpoint.get();

    // Set `legacyStateDir` based on flags.
    ASSERT(flags.legacy_state_path_dir.isSome());

    string path = path::join(flags.legacy_state_path_dir.get(), "containers");
    if (os::exists(path)) {
      legacyStateDir = path;
    }
  }

  // Search through `legacyStateDir` and let the DC/OS metrics service
  // know about any recovered containers that still have state in there.
  virtual Future<Nothing> recover(
      const vector<ContainerState>& states,
      const hashset<ContainerID>& orphans)
  {
    // If there is no `legacyStateDir`, we are done.
    if (legacyStateDir.isNone()) {
      return Nothing();
    }

    // Otherwise collect all containers known by either the agent
    // (states) or the containerizer (orphans).
    vector<ContainerID> containers;

    foreach(const ContainerState& state, states) {
      containers.push_back(state.container_id());
    }

    foreach(const ContainerID& container, orphans) {
      containers.push_back(container);
    }

    // Look through all of the recovered containers and see if legacy
    // metrics state for it exists. If it does, send the DC/OS metrics
    // service a `ContainerStart` message containing the existing
    // StatsD UDP host:port pair for the container.
    vector<Future<Nothing>> futures;

    foreach(const ContainerID& container, containers) {
      string statePath = path::join(legacyStateDir.get(), container.value());
      if (!os::exists(statePath)) {
        LOG(ERROR) << "Metrics isolator was told to recover container '"
                   << container << "' but the path '" << statePath
                   << "' could not be found";
        continue;
      }

      Try<std::string> read = os::read(statePath);
      if (read.isError()) {
        LOG(ERROR) << "Error reading the legacy state path"
                   << " '" << statePath << "': " << read.error();
        continue;
      }

      Try<LegacyState> legacyState = parse<LegacyState>(read.get());
      if (legacyState.isError()) {
        LOG(ERROR) << "Error parsing the legacy state at"
                   << " '" << statePath << "': " << legacyState.error();
        continue;
      }

      ContainerStartRequest containerStartRequest;
      containerStartRequest.set_container_id(container.value());
      containerStartRequest.set_statsd_host(legacyState->statsd_host());
      containerStartRequest.set_statsd_port(legacyState->statsd_port());

      Future<Nothing> future = send(containerStartRequest)
        .onAny(defer(
          self(),
          [=](const Future<http::Response>& response) -> Future<http::Response> {
            if (!response.isReady()) {
              return Failure("Error posting 'containerStartRequest' for"
                             " container '" + container.value() + "': " +
                             (response.isFailed() ?
                                 response.failure() : "Future discarded"));
            }

            return response.get();
          }))
        .then(defer(
          self(),
          [=](const http::Response& response) -> Future<Nothing> {
            if (response.code != http::Status::CREATED) {
              return Failure("Received unexpected response code "
                             " '" + stringify(response.code) + "' when"
                             " posting 'containerStartRequest' for container"
                             " '" + container.value() + "'" +
                             (response.body == "" ? "" : ": " + response.body));
            }

            Try<ContainerStartResponse> containerStartResponse =
              parse<ContainerStartResponse>(response.body);
            if (containerStartResponse.isError()) {
              return Failure("Error parsing the 'ContainerStartResponse' body"
                             " for container '" + container.value() + "': " +
                             containerStartResponse.error());
            }

            if (containerStartResponse->has_statsd_host() &&
                containerStartResponse->has_statsd_port()) {
              LOG(INFO) << "Successfully recovered StatsD metrics gathering for"
                        << " container '" << container.value() << "' on"
                        << " '" << containerStartResponse->statsd_host() << ":"
                        << containerStartResponse->statsd_port() << "'";
            } else {
              LOG(ERROR) << "Received expected status code from metrics"
                         << " service while recovering container '"
                         << container << "', but either 'statsd_host' or"
                         << " 'statsd_port' was not set in the response";
            }

            return Nothing();
          }));

      futures.push_back(future);
    }

    return await(futures)
      .then(defer(
          self(),
          [=](const vector<Future<Nothing>>& futures) {
            // Log any errors that occurred while recovering the
            // legacy state of each container.
            foreach(const Future<Nothing>& future, futures) {
              if (future.isFailed()) {
                LOG(ERROR) << future.failure();
              }
            }

            // Once all state has been recovered, move the legacy state
            // directory to a new location so that it isn't touched again on
            // the next recovery. We don't delete it, just in case there is a
            // bug and we need to get at the state again manually.
            string newLegacyStateDir = legacyStateDir.get() + ".recovered";
            Try<Nothing> rename = os::rename(legacyStateDir.get(), newLegacyStateDir);
            if (rename.isError()) {
              LOG(ERROR) << "Error renaming legacy state directory"
                         << " '" << legacyStateDir.get() << "': " << rename.error();
            }

            legacyStateDir = None();

            return Nothing();
          }));
  }

  virtual Future<Option<ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const slave::ContainerConfig& containerConfig)
  {
    // Let the metrics service know about the container being
    // launched via an HTTP request. In the response, grab the
    // STATSD_UDP_HOST and STATSD_UDP_PORT pair being returned and set
    // it in the environment of the `ContainerLaunchInfo` returned
    // from this function. On any errors, return a `Failure()`.
    ContainerLaunchInfo launchInfo;
    return launchInfo;
  }

  virtual Future<Nothing> cleanup(
      const ContainerID& containerId)
  {
    // Let the metrics service know about the container being
    // destroyed via an HTTP request. On any errors, return an
    // `Failure()`.
    return Nothing();
  }

  Future<http::Connection> connect() {
    if (serviceInetAddress.isSome()) {
      if (flags.service_scheme.get() == "http") {
        return http::connect(serviceInetAddress.get(), http::Scheme::HTTP);
      }
      return http::connect(serviceInetAddress.get(), http::Scheme::HTTPS);
    }

    if (serviceUnixAddress.isSome()) {
      if (flags.service_scheme.get() == "http") {
        return http::connect(serviceUnixAddress.get(), http::Scheme::HTTP);
      }
      return http::connect(serviceUnixAddress.get(), http::Scheme::HTTPS);
    }

    UNREACHABLE();
  }

  Future<http::Response> send(const string& body, const string& method)
  {
    return connect()
      .then(defer(
          self(),
          [=](http::Connection connection) -> Future<http::Response> {
            // Capture a reference to the connection to ensure that it remains
            // open long enough to receive the response.
            connection.disconnected()
              .onAny([connection]() {});

            http::Request request;
            request.method = method;
            request.keepAlive = true;
            request.headers = {
              {"Accept", APPLICATION_JSON},
              {"Content-Type", APPLICATION_JSON}};
            request.body = body;

            if (serviceInetAddress.isSome()) {
              request.url = http::URL(
                  serviceScheme,
                  serviceInetAddress->ip,
                  serviceInetAddress->port,
                  serviceEndpoint);
            }

            if (serviceUnixAddress.isSome()) {
              request.url.scheme = serviceScheme;
              request.url.domain = "";
              request.url.path = serviceEndpoint;
            }

            return connection.send(request);
          }))
      .after(
          flags.request_timeout,
          [](const Future<http::Response>&) {
            return Failure("Request timed out");
          });
  }

  Future<http::Response> send(const ContainerStartRequest& containerStartRequest)
  {
    string body = mesosphere::dcos::metrics::internal::serialize(
        ContentType::JSON,
        containerStartRequest);

    return send(body, "POST");
  }


  Future<http::Response> send(const ContainerStopRequest& containerStopRequest)
  {
    string body = mesosphere::dcos::metrics::internal::serialize(
        ContentType::JSON,
        containerStopRequest);

    return send(body, "DELETE");
  }

private:
  const isolator::Flags flags;
  string serviceScheme;
  string serviceEndpoint;
  Option<inet::Address> serviceInetAddress;
  Option<unix::Address> serviceUnixAddress;
  Option<string> legacyStateDir;
};


MetricsIsolator::MetricsIsolator(const isolator::Flags& flags)
  : process(new MetricsIsolatorProcess(flags))
{
  spawn(process.get());
}


MetricsIsolator::~MetricsIsolator()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> MetricsIsolator::recover(
      const vector<ContainerState>& states,
      const hashset<ContainerID>& orphans)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::recover,
                  states,
                  orphans);
}


Future<Option<ContainerLaunchInfo>> MetricsIsolator::prepare(
    const ContainerID& containerId,
    const slave::ContainerConfig& containerConfig)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::prepare,
                  containerId,
                  containerConfig);
}


Future<Nothing> MetricsIsolator::cleanup(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::cleanup,
                  containerId);
}

} // namespace metrics {
} // namespace dcos {
} // namespace mesosphere {

mesos::modules::Module<Isolator>
com_mesosphere_dcos_MetricsIsolatorModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "support@mesosphere.com",
    "Metrics Isolator Module.",
    nullptr,
    [](const Parameters& parameters) -> Isolator* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      mesosphere::dcos::metrics::isolator::Flags flags;

      // Load and validate flags from environment and map.
      Try<flags::Warnings> load = flags.load(values, false, "DCOS_");

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesosphere::dcos::metrics::MetricsIsolator(flags);
    });
