
// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef ANARI_USD_ENABLE_MPI

#include <zmq.hpp>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>

namespace usd_bridge {

struct WorkerInfo {
  std::string identity;
  std::string hostname;
  std::string ib_address;
  int rank;
  bool ready;
};

// ============================================================================
// ZmqBroker (Rank 0 - Server)
// ============================================================================

class ZmqBroker {
 public:
  // Constructor - port defaults to 5555, can be overridden
  ZmqBroker(int port = 5555);
  ~ZmqBroker();

  // Initialize broker and wait for workers to connect
  bool Initialize(int expectedWorkers);

  // Send geometry data to specific worker
  bool SendToWorker(const std::string& workerId, const void* data, size_t size);

  // Broadcast to all connected workers
  bool BroadcastToWorkers(const void* data, size_t size);

  // Receive result from worker
  bool ReceiveFromWorker(std::string& workerId, std::vector<char>& data);

 // Send reply to laptop client
 bool SendToClient(const std::string& clientId, const void* data, size_t size);

  // Get list of connected workers
  const std::vector<WorkerInfo>& GetConnectedWorkers() const { return workers_; }

  // Shutdown broker
  void Shutdown();

 private:
  std::unique_ptr<zmq::context_t> context_;
  std::unique_ptr<zmq::socket_t> router_;
  std::vector<WorkerInfo> workers_;
  std::map<std::string, int> worker_map_;
  int port_;
  bool initialized_;

  std::string GetInfiniBandIP();
};

// ============================================================================
// ZmqWorker (Rank 1-N - Client)
// ============================================================================

class ZmqWorker {
 public:
  // Constructor with broker address and rank
  ZmqWorker(const std::string& brokerAddress, int rank);
  ~ZmqWorker();

  // Connect to broker and send ready message
  bool Connect();

  // Receive task from broker
  bool ReceiveTask(std::vector<char>& data);

  // Send result back to broker
  bool SendResult(const void* data, size_t size);

  // Disconnect from broker
  void Disconnect();

 private:
  std::unique_ptr<zmq::context_t> context_;
  std::unique_ptr<zmq::socket_t> dealer_;
  std::string broker_address_;
  int rank_;
  bool connected_;

  std::string GetInfiniBandIP();
  std::string ResolveRank0Address();
};

} // namespace usd_bridge

#endif // ANARI_USD_ENABLE_MPI