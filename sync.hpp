#ifndef SYNC_SERVICE_UTILS_HPP
#define SYNC_SERVICE_UTILS_HPP

#include <armadillo>
#include <boost/circular_buffer.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <is/is.hpp>
#include <is/msgs/common.hpp>
#include <limits>
#include <string>
#include <tuple>

namespace is {

using namespace std;
using namespace std::chrono;
using namespace std::chrono_literals;
using namespace is::msg::common;
using namespace arma;

bool set_delays(is::ServiceClient &client, vector<string> const &entities,
                arma::vec const &delays) {
  if (entities.size() != delays.n_elem) {
    is::log::warn("Invalid size of arma::vec delays");
    return false;
  }

  auto n_tries = 3;
  while (n_tries) {

    std::vector<std::string> ids;
    for (size_t it = 0; it < entities.size(); ++it) {
      Delay delay;
      delay.milliseconds = static_cast<int64_t>(delays(it));
      ids.push_back(
          client.request(entities[it] + ".set_delay", is::msgpack(delay)));
    }
    is::log::info("Setting new delays. Try {}/3", 3 - n_tries + 1);

    auto msgs = client.receive_until(high_resolution_clock::now() + 2s, ids,
                                     is::policy::discard_others);
    if (msgs.size() == entities.size()) {
      return true;
    }
    n_tries--;
  }
  is::log::warn("Can't set all delays.");
  return false;
}

arma::vec get_delays(is::ServiceClient &client,
                     vector<string> const &entities) {
  auto n_tries = 3;
  while (n_tries) {
    std::vector<std::string> ids;
    for (auto &entity : entities) {
      ids.push_back(client.request(entity + ".get_delay", is::msgpack(0)));
    }
    is::log::info("Requesting currents delays. Try {}/3", 3 - n_tries + 1);

    auto msgs = client.receive_until(high_resolution_clock::now() + 2s, ids,
                                     is::policy::discard_others);

    if (msgs.size() == entities.size()) {
      arma::vec delays(msgs.size());
      std::transform(msgs.begin(), msgs.end(), delays.begin(), [&](auto msg) {
        return is::msgpack<Delay>(msg.second).milliseconds;
      });
      return delays;
    }
    n_tries--;
  }
  is::log::warn("Can't get all delays.");
  return arma::vec();
}

bool set_sampling_rate(is::ServiceClient &client, SyncRequest const &request) {
  auto n_tries = 3;
  while (n_tries) {
    std::vector<std::string> ids;
    for (auto &e : request.entities) {
      ids.push_back(client.request(e + ".set_sample_rate",
                                   is::msgpack(request.sampling_rate)));
    }
    is::log::info("Setting sampling rate. Try {}/3", 3 - n_tries + 1);

    auto msgs = client.receive_until(high_resolution_clock::now() + 2s, ids,
                                     is::policy::discard_others);
    if (msgs.size() == ids.size())
      return true;

    n_tries--;
  }
  return false;
}

arma::mat get_timestamps(is::Connection &is, vector<is::QueueInfo> tags,
                         unsigned int n_samples, unsigned int discard_samples,
                         int64_t period) {

  arma::mat timestamps(n_samples, tags.size(), arma::fill::zeros);
  is::log::info("Consuming timestamps. Period: {}", period);
  for (unsigned int r = 0; r < timestamps.n_rows; ++r) {
    auto msgs = is.consume_sync(tags, period);
    if (r < discard_samples)
      continue;
    auto first = msgs.begin();
    for (unsigned int c = 0; c < timestamps.n_cols; ++c) {
      timestamps(r, c) = is::msgpack<Timestamp>(*first++).nanoseconds;
    }
  }
  return timestamps;
}

int64_t get_period(SamplingRate sr) {
  if (sr.period) {
    return *sr.period;
  }
  if (sr.rate) {
    return static_cast<int64_t>(1000.0 / *sr.rate);
  }
  throw runtime_error("Undefined SamplingRate");
}

arma::vec compute_delays(arma::mat const &samples) {
  std::vector<arma::uvec> n_max(samples.n_cols);
  auto n_row = 0;
  samples.each_row([&](arma::rowvec const &row) {
    n_max[arma::index_max(row)] =
        join_vert(n_max[arma::index_max(row)],
                  arma::uvec({static_cast<arma::uword>(n_row++)}));
  });

  auto row_indexes =
      std::max_element(n_max.begin(), n_max.end(), [](auto lhs, auto rhs) {
        return lhs.n_elem < rhs.n_elem;
      });

  if ((*row_indexes).n_elem < 5)
    return arma::vec();

  is::log::info("Using {} samples", (*row_indexes).n_elem);
  arma::mat filtered_samples = samples.rows(*row_indexes);

  arma::mat samples_delays =
      arma::max(filtered_samples, 1) - filtered_samples.each_col();
  arma::mat diff = arma::diff(filtered_samples);
  (samples_delays / 1e6).print("samples_delays");
  (diff / 1e6).print("diff");

  arma::vec delays;
  samples_delays.each_col([&](arma::vec col) {
    col.shed_row(col.index_min());
    col.shed_row(col.index_max());
    delays = join_vert(delays, arma::vec({arma::mean(col) / 1e6}));
  });

  delays.print("delays");
  return delays;
}

Status sync_entities(string uri, SyncRequest request) {
  auto is = is::connect(uri);
  auto client = is::make_client(is);

  if (!set_sampling_rate(client, request)) {
    return status::error("Failed to set sampling rate");
  }

  std::this_thread::sleep_for(1s); // trust-me, it's necessery!!

  arma::vec delays = get_delays(client, request.entities);
  if (delays.empty()) {
    return status::error("Failed to get current delays");
  }
  delays.print("Current Delays");

  // Create timestamp subscribers
  is::log::info("Subscribe timestamps");
  vector<is::QueueInfo> tags;
  for (auto &e : request.entities) {
    tags.push_back(is.subscribe(e + ".timestamp"));
  }

  auto period = get_period(request.sampling_rate);
  auto sync_threshold = 3;
  const int tries_limit = 5;

  for (int tries = 0; tries < tries_limit; ++tries) {
    is::log::info("Try to sync. {}/{}", tries + 1, tries_limit);

    arma::mat timestamps = get_timestamps(is, tags, 13, 3, period);
    arma::vec delays_diff = compute_delays(timestamps);

    if (delays_diff.n_elem == 0) {
      is::log::warn("Empty delays matrix");
      continue;
    }

    if (arma::any(delays_diff > sync_threshold)) {
      delays += delays_diff;
      delays.elem(arma::find(delays >= period)) -= period;
      delays.print("New Delays");
      if (!set_delays(client, request.entities, delays))
        is::log::warn("Failed to set delays");
      continue;
    }
    is::log::info("Sync ok");
    return status::ok;
  }
  is::log::warn("Failed to sync");
  return status::error("Failed to sync");
}

} // ::is

#endif // SYNC_SERVICE_UTILS_HPP