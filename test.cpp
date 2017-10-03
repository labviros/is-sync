#include <boost/program_options.hpp>
#include <iostream>
#include <is/is.hpp>
#include <is/msgs/common.hpp>

using namespace is::msg::common;
using namespace std::chrono_literals;
namespace po = boost::program_options;

int main(int argc, char *argv[]) {
  std::string uri;
  std::vector<std::string> entities;
  int64_t period;

  po::options_description description("Allowed options");
  auto &&options = description.add_options();
  options("help,", "show available options");
  options("entity_list,l",
          po::value<std::vector<std::string>>(&entities)->multitoken(),
          "entity list");
  options("period,p", po::value<int64_t>(&period)->default_value(1000),
          "sampling period");
  options("uri,u",
          po::value<std::string>(&uri)->default_value("amqp://localhost"),
          "broker uri");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, description), vm);
  po::notify(vm);

  if (vm.count("help") || !vm.count("entity_list")) {
    std::cout << description << std::endl;
    return 1;
  }

  auto is = is::connect(uri);
  auto client = is::make_client(is);

  SyncRequest request;
  request.entities = entities;
  SamplingRate sr;
  sr.period = period;
  request.sampling_rate = sr;
  auto id = client.request("is.sync", is::msgpack(request));
  is::log::info("Waiting 1 minute for reply");
  auto msg = client.receive_for(1min, id, is::policy::discard_others);
  if (msg != nullptr) {
    auto reply = is::msgpack<Status>(msg);
    if (reply.value == "ok") {
      is::log::info("Sync ok");
      std::vector<std::string> topics;
      std::transform(entities.begin(), entities.end(),
                     std::back_inserter(topics),
                     [](auto e) { return e + ".timestamp"; });
      auto tag = is.subscribe(topics);

      std::map<std::string, uint64_t> timestamps;
      for (auto n = 0; n < 100; n++) {

        while (1) {
          auto msg = is.consume(tag);
          auto ts = is::msgpack<Timestamp>(msg);
          timestamps[msg->RoutingKey()] = ts.nanoseconds;

          if (timestamps.size() == topics.size()) {
            auto minmax_ts = std::minmax_element(
                timestamps.begin(), timestamps.end(),
                [](auto lhs, auto rhs) { return lhs.second < rhs.second; });

            auto min = (*(minmax_ts.first)).second;
            auto max = (*(minmax_ts.second)).second;
            auto diff = (max - min) / 1e6; // [ms]
            if (diff < 5) {
              auto diffs_str = std::accumulate(
                  timestamps.begin(), timestamps.end(), std::string(""),
                  [&](auto a, auto b) {
                    return a + fmt::format("{:.1f} | ", (b.second - min) / 1e6);
                  });
              is::log::info("{}{}", diffs_str, n);
              break;
            }
          }
        }
      }
      is.unsubscribe(tag);

    } else
      is::log::warn("Sync failed. Why: {}", reply.why);
  } else {
    is::log::warn("No reply received");
  }
  return 0;
}