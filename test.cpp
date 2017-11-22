#include <is/msgs/camera.pb.h>
#include <is/msgs/common.pb.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <is/is.hpp>

#include "sync.hpp"
using namespace is::common;
using namespace is::vision;

namespace std {
std::ostream& operator<<(std::ostream& os, const std::vector<std::string>& vec) {
  for (auto item : vec) {
    os << item << " ";
  }
  return os;
}
}

int main(int argc, char* argv[]) {
  std::string uri;
  std::vector<std::string> entities;

  is::po::options_description opts("Options");
  auto&& opt_add = opts.add_options();
  opt_add("uri,u", is::po::value<std::string>(&uri)->default_value("amqp://rmq.is:30000"),
          "broker uri");
  opt_add("entity_list,l",
          is::po::value<std::vector<std::string>>(&entities)->multitoken()->default_value(
              {"CameraGateway.0", "CameraGateway.1", "CameraGateway.2", "CameraGateway.3"}),
          "entity list");

  is::parse_program_options(argc, argv, opts);

  auto channel = is::rmq::Channel::CreateFromUri(uri);
  auto tag = is::declare_queue(channel);

  SyncRequest sync_request;
  std::transform(entities.begin(), entities.end(),
                 is::pb::RepeatedFieldBackInserter(sync_request.mutable_entities()),
                 [](auto& entity) { return entity; });
  sync_request.mutable_sampling()->set_frequency(5.0);

  // broke current sync
  arma::vec delays({0.01, 0.02, 0.0, 0.03});
  is::set_delays(channel, tag, sync_request, delays);

  auto id = is::request(channel, tag, "Time.Sync", sync_request);
  auto envelope = is::consume_for(channel, tag, is::pb::TimeUtil::SecondsToDuration(60));
  is::info("{}", is::rpc_status(envelope));

  return 0;
}