#ifndef SYNC_SERVICE_UTILS_HPP
#define SYNC_SERVICE_UTILS_HPP

#include <is/msgs/camera.pb.h>
#include <is/msgs/common.pb.h>
#include <armadillo>
#include <boost/circular_buffer.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <is/core.hpp>
#include <limits>
#include <string>
#include <tuple>

namespace is {

using namespace std;
using namespace std::chrono;
using namespace std::chrono_literals;
using namespace is::common;
using namespace is::vision;
using namespace arma;

inline std::string request(rmq::Channel::ptr_t const& channel, std::string const& queue,
                           std::string const& endpoint,
                           is::rmq::BasicMessage::ptr_t const& message) {
  auto id = is::make_random_uid();
  message->ReplyTo(queue);
  message->CorrelationId(id);
  channel->BasicPublish("is", endpoint, message);
  return id;
}

std::map<std::string /* endpoint */, is::rmq::Envelope::ptr_t> batch_request(
    is::rmq::Channel::ptr_t& channel, std::string const& queue,
    std::map<std::string /* endpoint */, is::rmq::BasicMessage::ptr_t> messages, int n_tries = 1,
    is::pb::Duration const& duration = is::pb::TimeUtil::SecondsToDuration(1)) {
  /*
    Returns a map of envelopes only with the successed replies received.
  */
  std::map<std::string /* endpoint */, is::rmq::Envelope::ptr_t> envelopes;
  while (n_tries) {
    std::map<std::string /* id */, std::string /* endpoint */> ids;
    std::transform(
        messages.begin(), messages.end(), std::inserter(ids, ids.end()), [&](auto& key_value) {
          auto endpoint = key_value.first;
          auto message = key_value.second;
          return std::make_pair(is::request(channel, queue, endpoint, message), endpoint);
        });

    auto deadline = is::current_time() + duration;
    is::rmq::Envelope::ptr_t envelope;
    for (;;) {
      envelope = is::consume_until(channel, queue, deadline);
      if (envelope == nullptr)  // deadline exceeded, go to next try
        break;
      auto pos = ids.find(envelope->Message()->CorrelationId());
      if (pos != ids.end()) {
        auto endpoint = pos->second;
        envelopes[endpoint] = envelope;
        messages.erase(endpoint);
      }
      if (messages.empty())
        return envelopes;
    }
    n_tries--;
  }
  return envelopes;
}

template <typename Request>
std::map<std::string /* endpoint */, is::rmq::Envelope::ptr_t> batch_request(
    is::rmq::Channel::ptr_t& channel, std::string const& queue,
    std::map<std::string /* endpoint */, Request> requests, int n_tries = 1,
    is::pb::Duration const& duration = is::pb::TimeUtil::SecondsToDuration(1)) {
  std::map<std::string /* endpoint */, is::rmq::BasicMessage::ptr_t> messages;
  std::transform(requests.begin(), requests.end(), std::inserter(messages, messages.end()),
                 [](auto& key_value) {
                   auto endpoint = key_value.first;
                   auto message = is::pack_proto(key_value.second);
                   return std::make_pair(endpoint, message);
                 });
  return batch_request(channel, queue, messages, n_tries, duration);
}

bool set_sampling_rate(is::rmq::Channel::ptr_t& channel, std::string const& queue,
                       SyncRequest const& sync_request) {
  std::map<std::string /* endpoints */, CameraConfig> requests;
  auto entities = sync_request.entities();
  CameraConfig camera_config;
  *camera_config.mutable_sampling() = sync_request.sampling();
  std::transform(entities.begin(), entities.end(), std::inserter(requests, requests.end()),
                 [&camera_config](auto& entity) {
                   return std::make_pair(fmt::format("{}.SetConfig", entity), camera_config);
                 });

  auto replies = batch_request(channel, queue, requests);
  if (replies.size() != entities.size())
    return false;

  return std::all_of(replies.begin(), replies.end(), [](auto& key_value) {
    auto status = is::rpc_status(key_value.second);
    return status.code() == StatusCode::OK;
  });
}

bool set_delays(is::rmq::Channel::ptr_t& channel, std::string const& queue,
                SyncRequest const& sync_request, arma::vec const& delays) {
  std::map<std::string /* endpoints */, CameraConfig> requests;
  auto entities = sync_request.entities();
  if (entities.size() != delays.n_elem) {
    is::warn("Invalid size of arma::vec delays");
    return false;
  }
  for (auto i = 0; i < entities.size(); ++i) {
    CameraConfig camera_config;
    camera_config.mutable_sampling()->mutable_delay()->set_value(static_cast<float>(delays[i]));
    requests[fmt::format("{}.SetConfig", entities[i])] = camera_config;
  }

  auto replies = batch_request(channel, queue, requests);
  if (replies.size() != entities.size())
    return false;

  return std::all_of(replies.begin(), replies.end(), [](auto& key_value) {
    auto status = is::rpc_status(key_value.second);
    return status.code() == StatusCode::OK;
  });
}

arma::vec get_delays(is::rmq::Channel::ptr_t& channel, std::string const& queue,
                     SyncRequest const& sync_request) {
  std::map<std::string /* endpoints */, FieldSelector> requests;
  auto entities = sync_request.entities();
  FieldSelector field_selector;
  field_selector.add_fields(CameraConfigFields::SAMPLING_SETTINGS);
  std::transform(entities.begin(), entities.end(), std::inserter(requests, requests.end()),
                 [&field_selector](auto& entity) {
                   return std::make_pair(fmt::format("{}.GetConfig", entity), field_selector);
                 });

  auto replies = batch_request(channel, queue, requests);
  if (replies.size() != entities.size())
    return arma::vec();

  auto all_ok = std::all_of(replies.begin(), replies.end(), [](auto& key_value) {
    auto status = is::rpc_status(key_value.second);
    return status.code() == StatusCode::OK;
  });

  if (!all_ok)
    return arma::vec();

  arma::vec delays;
  for (auto entity : entities) {
    auto envelope = replies[fmt::format("{}.GetConfig", entity)];
    auto camera_config = is::unpack<CameraConfig>(envelope);
    if (camera_config) {
      auto delay = static_cast<float>(camera_config->sampling().delay().value());
      delays = arma::join_vert(delays, arma::vec({delay}));
    } else {
      return arma::vec();
    }
  }
  return delays;
}

arma::mat get_timestamps(is::rmq::Channel::ptr_t& channel, std::string const& queue,
                         SyncRequest const& sync_request, unsigned int n_samples = 13,
                         unsigned int discard_samples = 3) {
  auto entities = sync_request.entities();

  std::vector<std::string> topics;
  std::transform(entities.begin(), entities.end(), std::back_inserter(topics),
                 [](auto& entity) { return fmt::format("{}.Timestamp", entity); });

  is::subscribe(channel, queue, topics);

  arma::mat output;
  while (n_samples--) {
    std::map<std::string, double> timestamps;
    for (;;) {
      auto envelope = is::consume(channel, queue);
      auto timestamp = *is::unpack<is::pb::Timestamp>(envelope);
      auto timestamp_ns = static_cast<double>(is::pb::TimeUtil::TimestampToNanoseconds(timestamp));
      timestamps[envelope->RoutingKey()] = timestamp_ns;

      if (timestamps.size() == topics.size()) {
        arma::mat window;
        for (auto& entity : entities) {
          auto endpoint = fmt::format("{}.Timestamp", entity);
          window = arma::join_horiz(window, arma::vec({timestamps[endpoint]}));
        }
        output = arma::join_vert(output, window);
        break;
      }
    }
  }

  is::unsubscribe(channel, queue, topics);
  return output.rows(discard_samples - 1, output.n_rows - 1);
}

double get_period(SyncRequest const& sync_request) {
  if (sync_request.sampling().rate_case() == SamplingSettings::RateCase::kFrequency) {
    return 1.0 / sync_request.sampling().frequency();
  } else {
    return sync_request.sampling().period();
  }
}

arma::vec compute_delays(arma::mat const& samples) {
  std::vector<arma::uvec> n_max(samples.n_cols);
  auto n_row = 0;
  samples.each_row([&](arma::rowvec const& row) {
    n_max[arma::index_max(row)] =
        join_vert(n_max[arma::index_max(row)], arma::uvec({static_cast<arma::uword>(n_row++)}));
  });

  auto row_indexes = std::max_element(n_max.begin(), n_max.end(),
                                      [](auto lhs, auto rhs) { return lhs.n_elem < rhs.n_elem; });

  if ((*row_indexes).n_elem < 5)
    return arma::vec();

  is::info("Using {} samples", (*row_indexes).n_elem);
  arma::mat filtered_samples = samples.rows(*row_indexes);

  arma::mat samples_delays = arma::max(filtered_samples, 1) - filtered_samples.each_col();
  (samples_delays / 1e6).print("samples_delays");

  arma::vec delays;
  samples_delays.each_col([&](arma::vec col) {
    col.shed_row(col.index_min());
    col.shed_row(col.index_max());
    delays = join_vert(delays, arma::vec({arma::mean(col) / 1e9}));
  });

  delays.print("delays");
  return delays;
}

Status sync_entities(is::rmq::Channel::ptr_t& channel, std::string const& queue, SyncRequest const& request) {
  if (request.entities_size() < 2)
    return is::make_status(StatusCode::INVALID_ARGUMENT,
                           "SyncRequest must have at least 2 entities");
  if (request.sampling().rate_case() == SamplingSettings::RateCase::RATE_NOT_SET)
    return is::make_status(StatusCode::INVALID_ARGUMENT, "\'rate\' field on sampling must be set");

  if (!set_sampling_rate(channel, queue, request)) {
    return is::make_status(StatusCode::INTERNAL_ERROR, "Failed to set sampling rate");
  }

  std::this_thread::sleep_for(1s);  // trust-me, it's necessary!!

  arma::vec delays = get_delays(channel, queue, request);
  if (delays.empty()) {
    return is::make_status(StatusCode::INTERNAL_ERROR, "Failed to get current delays");
  }

  delays.print("Current Delays");

  auto sync_threshold = 3.0 / 1e3;  // seconds
  const int tries_limit = 5;

  for (int tries = 0; tries < tries_limit; ++tries) {
    is::info("Try to sync. {}/{}", tries + 1, tries_limit);

    arma::mat timestamps = get_timestamps(channel, queue, request);
    arma::vec delays_diff = compute_delays(timestamps);
    if (delays_diff.n_elem == 0) {
      is::warn("Empty delays matrix");
      continue;
    }

    if (arma::any(delays_diff > sync_threshold)) {
      auto period = get_period(request);
      delays += delays_diff;
      delays.elem(arma::find(delays >= period)) -= period;
      delays.print("New Delays");
      if (!set_delays(channel, queue, request, delays))
        is::warn("Failed to set delays");
      continue;
    }
    is::info("Sync ok");
    return is::make_status(StatusCode::OK);
  }
  is::warn("Failed to sync");
  return is::make_status(StatusCode::FAILED_PRECONDITION,
                         "Failed to sync. Number of tries exceeded.");
}

}  // ::is

#endif  // SYNC_SERVICE_UTILS_HPP