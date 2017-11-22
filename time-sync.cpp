#include <is/msgs/common.pb.h>
#include <google/protobuf/empty.pb.h>
#include <is/is.hpp>
#include "sync.hpp"

using namespace is::common;

int main(int argc, char* argv[]) {
  std::string uri;

  is::po::options_description opts("Options");
  auto&& opt_add = opts.add_options();

  opt_add("uri,u", is::po::value<std::string>(&uri)->default_value("amqp://rmq.is:30000"),
          "broker uri");
  is::parse_program_options(argc, argv, opts);

  auto channel = is::rmq::Channel::CreateFromUri(uri);
  is::info("Connected to broker {}", uri);

  is::ServiceProvider provider;
  provider.connect(channel);
  auto queue = provider.declare_queue("Time", "");

  provider.delegate<SyncRequest, is::pb::Empty>(
      queue, "Sync", [&channel](SyncRequest const& request, is::pb::Empty*) -> Status {
        return is::sync_entities(channel, request);
      });

  provider.run();
  return 0;
}