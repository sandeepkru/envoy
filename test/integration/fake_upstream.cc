#include "fake_upstream.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "common/api/api_impl.h"
#include "common/buffer/buffer_impl.h"
#include "common/http/header_map_impl.h"
#include "common/http/http1/codec_impl.h"
#include "common/http/http2/codec_impl.h"
#include "common/network/address_impl.h"
#include "common/network/listen_socket_impl.h"
#include "common/network/utility.h"

#include "server/connection_handler_impl.h"

#include "test/test_common/network_utility.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

namespace Envoy {
FakeStream::FakeStream(FakeHttpConnection& parent, Http::StreamEncoder& encoder)
    : parent_(parent), encoder_(encoder) {
  encoder.getStream().addCallbacks(*this);
}

void FakeStream::decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) {
  std::unique_lock<std::mutex> lock(lock_);
  headers_ = std::move(headers);
  end_stream_ = end_stream;
  decoder_event_.notify_one();
}

void FakeStream::decodeData(Buffer::Instance& data, bool end_stream) {
  std::unique_lock<std::mutex> lock(lock_);
  end_stream_ = end_stream;
  body_.add(data);
  decoder_event_.notify_one();
}

void FakeStream::decodeTrailers(Http::HeaderMapPtr&& trailers) {
  std::unique_lock<std::mutex> lock(lock_);
  end_stream_ = true;
  trailers_ = std::move(trailers);
  decoder_event_.notify_one();
}

void FakeStream::encodeHeaders(const Http::HeaderMapImpl& headers, bool end_stream) {
  std::shared_ptr<Http::HeaderMapImpl> headers_copy(
      new Http::HeaderMapImpl(static_cast<const Http::HeaderMap&>(headers)));
  parent_.connection().dispatcher().post([this, headers_copy, end_stream]() -> void {
    encoder_.encodeHeaders(*headers_copy, end_stream);
  });
}

void FakeStream::encodeData(uint64_t size, bool end_stream) {
  parent_.connection().dispatcher().post([this, size, end_stream]() -> void {
    Buffer::OwnedImpl data(std::string(size, 'a'));
    encoder_.encodeData(data, end_stream);
  });
}

void FakeStream::encodeData(Buffer::Instance& data, bool end_stream) {
  std::shared_ptr<Buffer::Instance> data_copy(new Buffer::OwnedImpl(data));
  parent_.connection().dispatcher().post(
      [this, data_copy, end_stream]() -> void { encoder_.encodeData(*data_copy, end_stream); });
}

void FakeStream::encodeTrailers(const Http::HeaderMapImpl& trailers) {
  std::shared_ptr<Http::HeaderMapImpl> trailers_copy(
      new Http::HeaderMapImpl(static_cast<const Http::HeaderMap&>(trailers)));
  parent_.connection().dispatcher().post(
      [this, trailers_copy]() -> void { encoder_.encodeTrailers(*trailers_copy); });
}

void FakeStream::encodeResetStream() {
  parent_.connection().dispatcher().post(
      [this]() -> void { encoder_.getStream().resetStream(Http::StreamResetReason::LocalReset); });
}

void FakeStream::onResetStream(Http::StreamResetReason) {
  std::unique_lock<std::mutex> lock(lock_);
  saw_reset_ = true;
  decoder_event_.notify_one();
}

void FakeStream::waitForHeadersComplete() {
  std::unique_lock<std::mutex> lock(lock_);
  while (!headers_) {
    decoder_event_.wait(lock);
  }
}

void FakeStream::waitForData(Event::Dispatcher& client_dispatcher, uint64_t body_length) {
  std::unique_lock<std::mutex> lock(lock_);
  while (bodyLength() != body_length) {
    decoder_event_.wait_until(lock,
                              std::chrono::system_clock::now() + std::chrono::milliseconds(5));
    if (bodyLength() != body_length) {
      // Run the client dispatcher since we may need to process window updates, etc.
      client_dispatcher.run(Event::Dispatcher::RunType::NonBlock);
    }
  }
}

void FakeStream::waitForEndStream(Event::Dispatcher& client_dispatcher) {
  std::unique_lock<std::mutex> lock(lock_);
  while (!end_stream_) {
    decoder_event_.wait_until(lock,
                              std::chrono::system_clock::now() + std::chrono::milliseconds(5));
    if (!end_stream_) {
      // Run the client dispatcher since we may need to process window updates, etc.
      client_dispatcher.run(Event::Dispatcher::RunType::NonBlock);
    }
  }
}

void FakeStream::waitForReset() {
  std::unique_lock<std::mutex> lock(lock_);
  while (!saw_reset_) {
    decoder_event_.wait(lock);
  }
}

FakeHttpConnection::FakeHttpConnection(QueuedConnectionWrapperPtr connection_wrapper,
                                       Stats::Store& store, Type type)
    : FakeConnectionBase(std::move(connection_wrapper)) {
  if (type == Type::HTTP1) {
    codec_.reset(new Http::Http1::ServerConnectionImpl(connection_, *this, Http::Http1Settings()));
  } else {
    codec_.reset(
        new Http::Http2::ServerConnectionImpl(connection_, *this, store, Http::Http2Settings()));
    ASSERT(type == Type::HTTP2);
  }

  connection_.addReadFilter(Network::ReadFilterSharedPtr{new ReadFilter(*this)});
}

void FakeConnectionBase::close() {
  // Make sure that a close didn't already come in and destroy the connection.
  std::unique_lock<std::mutex> lock(lock_);
  if (!disconnected_) {
    connection_.dispatcher().post([this]() -> void {
      if (!disconnected_) {
        connection_.close(Network::ConnectionCloseType::FlushWrite);
      }
    });
  }
}

void FakeConnectionBase::readDisable(bool disable) {
  std::unique_lock<std::mutex> lock(lock_);
  RELEASE_ASSERT(!disconnected_);
  connection_.dispatcher().post([this, disable]() -> void { connection_.readDisable(disable); });
}

Http::StreamDecoder& FakeHttpConnection::newStream(Http::StreamEncoder& encoder) {
  std::unique_lock<std::mutex> lock(lock_);
  new_streams_.emplace_back(new FakeStream(*this, encoder));
  connection_event_.notify_one();
  return *new_streams_.back();
}

void FakeConnectionBase::onEvent(Network::ConnectionEvent event) {
  std::unique_lock<std::mutex> lock(lock_);
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    disconnected_ = true;
    connection_event_.notify_one();
  }
}

void FakeConnectionBase::waitForDisconnect(bool ignore_spurious_events) {
  std::unique_lock<std::mutex> lock(lock_);
  while (!disconnected_) {
    connection_event_.wait(lock);
    // The default behavior of waitForDisconnect is to assume the test cleanly
    // calls waitForData, waitForNewStream, etc. to handle all events on the
    // connection.  If the caller explicitly notes that other events should be
    // ignored, continue looping until a disconnect is detected.  Otherwise fall
    // through and hit the assert below.
    if (!ignore_spurious_events) {
      break;
    }
  }

  ASSERT(disconnected_);
}

FakeStreamPtr FakeHttpConnection::waitForNewStream() {
  std::unique_lock<std::mutex> lock(lock_);
  if (new_streams_.empty()) {
    connection_event_.wait(lock);
  }

  ASSERT(!new_streams_.empty());
  FakeStreamPtr stream = std::move(new_streams_.front());
  new_streams_.pop_front();
  return stream;
}

FakeUpstream::FakeUpstream(const std::string& uds_path, FakeHttpConnection::Type type)
    : FakeUpstream(nullptr, Network::ListenSocketPtr{new Network::UdsListenSocket(uds_path)},
                   type) {
  ENVOY_LOG(info, "starting fake server on unix domain socket {}", uds_path);
}

static Network::ListenSocketPtr makeTcpListenSocket(uint32_t port,
                                                    Network::Address::IpVersion version) {
  return Network::ListenSocketPtr{new Network::TcpListenSocket(
      Network::Utility::parseInternetAddressAndPort(
          fmt::format("{}:{}", Network::Test::getAnyAddressUrlString(version), port)),
      true)};
}

FakeUpstream::FakeUpstream(uint32_t port, FakeHttpConnection::Type type,
                           Network::Address::IpVersion version)
    : FakeUpstream(nullptr, makeTcpListenSocket(port, version), type) {
  ENVOY_LOG(info, "starting fake server on port {}. Address version is {}",
            this->localAddress()->ip()->port(), Network::Test::addressVersionAsString(version));
}

FakeUpstream::FakeUpstream(Ssl::ServerContext* ssl_ctx, uint32_t port,
                           FakeHttpConnection::Type type, Network::Address::IpVersion version)
    : FakeUpstream(ssl_ctx, makeTcpListenSocket(port, version), type) {
  ENVOY_LOG(info, "starting fake SSL server on port {}. Address version is {}",
            this->localAddress()->ip()->port(), Network::Test::addressVersionAsString(version));
}

FakeUpstream::FakeUpstream(Ssl::ServerContext* ssl_ctx, Network::ListenSocketPtr&& listen_socket,
                           FakeHttpConnection::Type type)
    : ssl_ctx_(ssl_ctx), socket_(std::move(listen_socket)),
      api_(new Api::Impl(std::chrono::milliseconds(10000))),
      dispatcher_(api_->allocateDispatcher()),
      handler_(new Server::ConnectionHandlerImpl(ENVOY_LOGGER(), *dispatcher_)), http_type_(type) {
  thread_.reset(new Thread::Thread([this]() -> void { threadRoutine(); }));
  server_initialized_.waitReady();
}

FakeUpstream::~FakeUpstream() {
  dispatcher_->exit();
  thread_->join();
}

bool FakeUpstream::createFilterChain(Network::Connection& connection) {
  std::unique_lock<std::mutex> lock(lock_);
  connection.readDisable(true);
  new_connections_.emplace_back(new QueuedConnectionWrapper(connection));
  new_connection_event_.notify_one();
  return true;
}

void FakeUpstream::threadRoutine() {
  if (ssl_ctx_) {
    handler_->addSslListener(*this, *ssl_ctx_, *socket_, stats_store_, 0,
                             Network::ListenerOptions::listenerOptionsWithBindToPort());
  } else {
    handler_->addListener(*this, *socket_, stats_store_, 0,
                          Network::ListenerOptions::listenerOptionsWithBindToPort());
  }

  server_initialized_.setReady();
  dispatcher_->run(Event::Dispatcher::RunType::Block);
  handler_.reset();
}

FakeHttpConnectionPtr FakeUpstream::waitForHttpConnection(Event::Dispatcher& client_dispatcher) {
  std::unique_lock<std::mutex> lock(lock_);
  while (new_connections_.empty()) {
    new_connection_event_.wait_until(lock, std::chrono::system_clock::now() +
                                               std::chrono::milliseconds(5));
    if (new_connections_.empty()) {
      // Run the client dispatcher since we may need to process window updates, etc.
      client_dispatcher.run(Event::Dispatcher::RunType::NonBlock);
    }
  }

  ASSERT(!new_connections_.empty());
  FakeHttpConnectionPtr connection(
      new FakeHttpConnection(std::move(new_connections_.front()), stats_store_, http_type_));
  connection->initialize();
  new_connections_.pop_front();
  connection->readDisable(false);
  return connection;
}

FakeRawConnectionPtr FakeUpstream::waitForRawConnection() {
  std::unique_lock<std::mutex> lock(lock_);
  if (new_connections_.empty()) {
    ENVOY_LOG(debug, "waiting for raw connection");
    new_connection_event_.wait(lock);
  }

  ASSERT(!new_connections_.empty());
  FakeRawConnectionPtr connection(new FakeRawConnection(std::move(new_connections_.front())));
  connection->initialize();
  new_connections_.pop_front();
  connection->readDisable(false);
  return connection;
}

void FakeRawConnection::waitForData(uint64_t num_bytes) {
  std::unique_lock<std::mutex> lock(lock_);
  while (data_.size() != num_bytes) {
    ENVOY_LOG(debug, "waiting for {} bytes of data", num_bytes);
    connection_event_.wait(lock);
  }
}

void FakeRawConnection::write(const std::string& data) {
  connection_.dispatcher().post([data, this]() -> void {
    Buffer::OwnedImpl to_write(data);
    connection_.write(to_write);
  });
}

Network::FilterStatus FakeRawConnection::ReadFilter::onData(Buffer::Instance& data) {
  std::unique_lock<std::mutex> lock(parent_.lock_);
  ENVOY_LOG(debug, "got {} bytes", data.length());
  parent_.data_.append(TestUtility::bufferToString(data));
  data.drain(data.length());
  parent_.connection_event_.notify_one();
  return Network::FilterStatus::StopIteration;
}
} // namespace Envoy
