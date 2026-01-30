
// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdBridgeZmqBroker.h"

#ifdef ANARI_USD_ENABLE_MPI

#include <zmq.hpp>
#include <iostream>
#include <sstream>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <fstream>
#include <thread>
#include <chrono>
#include <netdb.h>
#include <cstdio>

namespace usd_bridge {

// Helper function to get InfiniBand IP
std::string GetInfiniBandIPImpl() {
  struct ifaddrs *ifaddr, *ifa;
  std::string ib_ip;

  if (getifaddrs(&ifaddr) == -1) {
    // Fallback: try SLURM environment
    const char* slurm_node = getenv("SLURMD_NODENAME");
    if (slurm_node) {
      return std::string(slurm_node);
    }
    return "127.0.0.1"; // localhost fallback
  }

  for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;

    // Look for ib0, ib1, mlx interfaces (InfiniBand)
    if (strncmp(ifa->ifa_name, "ib", 2) == 0 ||
        strncmp(ifa->ifa_name, "mlx", 3) == 0) {
      if (ifa->ifa_addr->sa_family == AF_INET) {
        char addr[INET_ADDRSTRLEN];
        struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
        inet_ntop(AF_INET, &(sa->sin_addr), addr, INET_ADDRSTRLEN);
        ib_ip = addr;
        break;
      }
    }
  }

  freeifaddrs(ifaddr);

  // If no IB interface found, use first non-loopback
  if (ib_ip.empty()) {
    return "0.0.0.0"; // Bind to all interfaces
  }

  return ib_ip;
}

// ============================================================================
// ZmqBroker Implementation (Rank 0)
// ============================================================================

ZmqBroker::ZmqBroker(int port)
    : port_(port)
    , initialized_(false)
{
  context_ = std::make_unique<zmq::context_t>(1);
  router_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::router);
}

ZmqBroker::~ZmqBroker() {
  Shutdown();
}

std::string ZmqBroker::GetInfiniBandIP() {
  return GetInfiniBandIPImpl();
}

bool ZmqBroker::Initialize(int expectedWorkers) {
  if (initialized_) return true;

  try {
    // Clean up any stale discovery files from previous runs
    // This ensures workers don't connect to the wrong broker on rerun
    std::remove("zmq_rank0_ip.txt");

    std::string ib_ip = GetInfiniBandIP();

    std::stringstream bind_addr;
    bind_addr << "tcp://" << ib_ip << ":" << port_;

    std::cout << "[Rank 0 Broker] InfiniBand IP detected: " << ib_ip << std::endl;
    std::cout << "[Rank 0 Broker] Binding to " << bind_addr.str() << std::endl;
    std::cout << "[Rank 0 Broker] Port: " << port_ << " (from "
              << (getenv("ANARI_USD_ZMQ_PORT") ? "ANARI_USD_ZMQ_PORT" : "default")
              << ")" << std::endl;

    router_->bind(bind_addr.str());

    // Set socket options
    int linger = 0;
    router_->set(zmq::sockopt::linger, linger);

    // Write rank 0's InfiniBand IP and port to file for workers to discover
    // IMPORTANT: Do this IMMEDIATELY after binding, before waiting for workers
    std::ofstream ip_file("zmq_rank0_ip.txt");
    if (ip_file.is_open()) {
      ip_file << ib_ip << ":" << port_ << std::endl;
      ip_file.close();
      std::cout << "[Rank 0 Broker] Wrote InfiniBand address to zmq_rank0_ip.txt: "
                << ib_ip << ":" << port_ << std::endl;
    } else {
      std::cerr << "[Rank 0 Broker] WARNING: Could not write zmq_rank0_ip.txt" << std::endl;
    }

    // Print SSH tunnel command for easy copy-paste on laptop
    std::cout << std::endl;
    std::cout << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║ SSH TUNNEL COMMAND FOR LAPTOP CLIENT                                          ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << "ssh -N -L " << port_ << ":" << ib_ip << ":" << port_ << " \\" << std::endl;
    std::cout << "  -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
    std::cout << "  george2@jureca04.fz-juelich.de" << std::endl;
    std::cout << std::endl;


    std::cout << "[Rank 0 Broker] Waiting for " << expectedWorkers
              << " workers to connect..." << std::endl;

    // Wait for workers to connect with timeout
    auto start_time = std::chrono::steady_clock::now();
    const int timeout_seconds = 400;

    while (workers_.size() < static_cast<size_t>(expectedWorkers)) {
      // Check timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start_time).count();

      if (elapsed > timeout_seconds) {
        std::cerr << "[Rank 0 Broker] Timeout waiting for workers. Only "
                  << workers_.size() << "/" << expectedWorkers
                  << " connected." << std::endl;
        return false;
      }

      zmq::message_t identity, empty, ready_msg;

      // Non-blocking receive with timeout
      zmq::pollitem_t items[] = { { *router_, 0, ZMQ_POLLIN, 0 } };
      zmq::poll(&items[0], 1, std::chrono::milliseconds(100));

      if (!(items[0].revents & ZMQ_POLLIN)) {
        continue; // No message yet
      }

      (void)router_->recv(identity, zmq::recv_flags::none);
      (void)router_->recv(empty, zmq::recv_flags::none);
      (void)router_->recv(ready_msg, zmq::recv_flags::none);

      std::string msg(static_cast<const char*>(ready_msg.data()), ready_msg.size());

      if (msg.find("READY") == 0) {
        // Parse: "READY|rank|hostname|ip"
        std::stringstream ss(msg);
        std::string token, rank_str, hostname, ip;

        std::getline(ss, token, '|'); // "READY"
        std::getline(ss, rank_str, '|'); // rank
        std::getline(ss, hostname, '|'); // hostname
        std::getline(ss, ip, '|'); // ip

        WorkerInfo worker;
        worker.identity = std::string(static_cast<const char*>(identity.data()),
                                      identity.size());
        worker.rank = std::stoi(rank_str);
        worker.hostname = hostname;
        worker.ib_address = ip;
        worker.ready = true;

        workers_.push_back(worker);
        worker_map_[worker.identity] = worker.rank;

        std::cout << "[Rank 0 Broker] Worker rank " << worker.rank
                  << " connected from " << worker.hostname
                  << " (" << worker.ib_address << ")" << std::endl;

        // Send acknowledgment
        router_->send(identity, zmq::send_flags::sndmore);
        router_->send(zmq::message_t(), zmq::send_flags::sndmore);
        router_->send(zmq::message_t("ACK", 3), zmq::send_flags::none);
      }
    }

    initialized_ = true;
    std::cout << "[Rank 0 Broker] All " << expectedWorkers
              << " workers connected successfully!" << std::endl;

    // Ready to receive messages from laptop client
    std::cout << "[Rank 0 Broker] Ready to receive messages from laptop client..." << std::endl;

    // Message handling loop - echo messages from laptop clients
    while (true) {
      std::string clientId;
      std::vector<char> data;

      if (ReceiveFromWorker(clientId, data)) {
        std::string message(data.begin(), data.end());
        std::cout << "[Rank 0 Broker] Received from laptop client: " << message << std::endl;

        // Echo back the message with ACK prefix
        std::string reply = "ACK: " + message;
        SendToClient(clientId, reply.c_str(), reply.size());

        std::cout << "[Rank 0 Broker] Sent reply: " << reply << std::endl;
      }
    }

    return true;


  } catch (const zmq::error_t& e) {
    std::cerr << "[Rank 0 Broker] ZMQ Error: " << e.what() << std::endl;
    return false;
  }
}

  bool ZmqBroker::SendToClient(const std::string& clientId,
                              const void* data, size_t size) {
  try {
    zmq::message_t identity(clientId.data(), clientId.size());
    zmq::message_t empty;
    zmq::message_t payload(data, size);

    router_->send(identity, zmq::send_flags::sndmore);
    router_->send(empty, zmq::send_flags::sndmore);
    router_->send(payload, zmq::send_flags::none);

    return true;
  } catch (const zmq::error_t& e) {
    std::cerr << "[Rank 0 Broker] Send to client error: " << e.what() << std::endl;
    return false;
  }
}


bool ZmqBroker::SendToWorker(const std::string& workerId,
                              const void* data, size_t size) {
  try {
    zmq::message_t identity(workerId.data(), workerId.size());
    zmq::message_t empty;
    zmq::message_t payload(data, size);

    router_->send(identity, zmq::send_flags::sndmore);
    router_->send(empty, zmq::send_flags::sndmore);
    router_->send(payload, zmq::send_flags::none);

    return true;
  } catch (const zmq::error_t& e) {
    std::cerr << "[Rank 0 Broker] Send error: " << e.what() << std::endl;
    return false;
  }
}

bool ZmqBroker::BroadcastToWorkers(const void* data, size_t size) {
  bool all_success = true;
  for (const auto& worker : workers_) {
    if (!SendToWorker(worker.identity, data, size)) {
      all_success = false;
    }
  }
  return all_success;
}

bool ZmqBroker::ReceiveFromWorker(std::string& workerId,
                                   std::vector<char>& data) {
  try {
    zmq::message_t identity, empty, payload;

    (void)router_->recv(identity, zmq::recv_flags::none);
    (void)router_->recv(empty, zmq::recv_flags::none);
    (void)router_->recv(payload, zmq::recv_flags::none);

    workerId = std::string(static_cast<const char*>(identity.data()),
                           identity.size());

    data.resize(payload.size());
    memcpy(data.data(), payload.data(), payload.size());

    return true;
  } catch (const zmq::error_t& e) {
    std::cerr << "[Rank 0 Broker] Receive error: " << e.what() << std::endl;
    return false;
  }
}

void ZmqBroker::Shutdown() {
  if (initialized_) {
    std::cout << "[Rank 0 Broker] Shutting down..." << std::endl;
    router_->close();
    context_->close();
    initialized_ = false;
  }
}

// ============================================================================
// ZmqWorker Implementation (Rank 1-N)
// ============================================================================

ZmqWorker::ZmqWorker(const std::string& brokerAddress, int rank)
    : broker_address_(brokerAddress)
    , rank_(rank)
    , connected_(false)
{
  context_ = std::make_unique<zmq::context_t>(1);
  dealer_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::dealer);
}

ZmqWorker::~ZmqWorker() {
  Disconnect();
}

std::string ZmqWorker::GetInfiniBandIP() {
  return GetInfiniBandIPImpl();
}

std::string ZmqWorker::ResolveRank0Address() {
  std::cout << "[Worker Rank " << rank_ << "] Resolving rank 0 address..." << std::endl;

  // Method 1: Try to read rank 0's InfiniBand IP and port from file (MOST RELIABLE)
  // This file is written by rank 0's ZmqBroker::Initialize()
  std::ifstream ip_file("zmq_rank0_ip.txt");
  if (ip_file.is_open()) {
    std::string ip_port;
    std::getline(ip_file, ip_port);
    ip_file.close();

    if (!ip_port.empty()) {
      std::cout << "[Worker Rank " << rank_ << "] Read rank 0 address from file: "
                << ip_port << std::endl;
      return ip_port;
    }
  }

  // Method 2: Try to parse SLURM_NODELIST and resolve hostname
  // Handles formats like "jrc[0076-0077,0079-0080]" and "jrc[0076-0079]"
  const char* nodelist_env = getenv("SLURM_NODELIST");
  if (nodelist_env) {
    std::string nodelist(nodelist_env);
    std::cout << "[Worker Rank " << rank_ << "] Parsing SLURM_NODELIST: "
              << nodelist << std::endl;

    // Simple case: no brackets, e.g., "jrc0076,jrc0077"
    if (nodelist.find('[') == std::string::npos) {
      std::string first_node = nodelist.substr(0, nodelist.find(','));
      std::cout << "[Worker Rank " << rank_ << "] Using first node from list: "
                << first_node << std::endl;
      return first_node;
    }

    // Complex case: brackets, e.g., "jrc[0076-0077,0079-0080]" or "jrc[0076-0079]"
    size_t bracket_start = nodelist.find('[');
    size_t bracket_end = nodelist.find(']');

    if (bracket_start != std::string::npos && bracket_end != std::string::npos) {
      std::string prefix = nodelist.substr(0, bracket_start);
      std::string node_spec = nodelist.substr(bracket_start + 1,
                                               bracket_end - bracket_start - 1);

      std::cout << "[Worker Rank " << rank_ << "] Prefix: '" << prefix
                << "', Node spec: '" << node_spec << "'" << std::endl;

      // Handle multiple comma-separated specs: "0076-0077,0079-0080"
      // Extract first spec before any comma
      size_t comma_pos = node_spec.find(',');
      std::string first_spec = (comma_pos != std::string::npos)
                                ? node_spec.substr(0, comma_pos)
                                : node_spec;

      std::cout << "[Worker Rank " << rank_ << "] First spec: '" << first_spec << "'" << std::endl;

      // Check for range: "0076-0077" or just "0076"
      size_t dash_pos = first_spec.find('-');
      if (dash_pos != std::string::npos) {
        // It's a range, extract first number
        std::string first_id = first_spec.substr(0, dash_pos);
        std::string rank0_hostname = prefix + first_id;

        std::cout << "[Worker Rank " << rank_ << "] Parsed rank 0 hostname: "
                  << rank0_hostname << std::endl;

        // Try to resolve hostname to IP
        struct hostent* he = gethostbyname(rank0_hostname.c_str());
        if (he != nullptr && he->h_addr_list[0] != nullptr) {
          struct in_addr** addr_list = (struct in_addr**)he->h_addr_list;
          std::string resolved_ip = inet_ntoa(*addr_list[0]);

          std::cout << "[Worker Rank " << rank_ << "] Resolved " << rank0_hostname
                    << " to IP: " << resolved_ip << std::endl;
          return resolved_ip;
        }
      } else {
        // Single node number in brackets
        std::string rank0_hostname = prefix + first_spec;

        std::cout << "[Worker Rank " << rank_ << "] Single node: "
                  << rank0_hostname << std::endl;

        return rank0_hostname;
      }
    }
  }

  // Method 3: Fallback - use provided broker address
  std::cout << "[Worker Rank " << rank_ << "] Falling back to broker address: "
            << broker_address_ << std::endl;
  return broker_address_;
}

bool ZmqWorker::Connect() {
  if (connected_) return true;

  try {
    std::string rank0_host;

    // Wait for rank 0 to write the IP file (with retries)
    int retries = 50;  // ~5 seconds with 100ms sleep
    std::cout << "[Worker Rank " << rank_ << "] Attempting to discover rank 0 address..."
              << std::endl;

    while (retries > 0) {
      rank0_host = ResolveRank0Address();

      // Check if we got a valid IP (not empty or obviously wrong)
      if (!rank0_host.empty() && rank0_host != "127.0.0.1" &&
          rank0_host != "0.0.0.0" && rank0_host != broker_address_) {

        std::cout << "[Worker Rank " << rank_ << "] Found rank 0 address: "
                  << rank0_host << std::endl;
        break;
      }

      if (retries > 0) {
        std::cout << "[Worker Rank " << rank_ << "] Rank 0 address not ready, retrying... "
                  << "(attempts left: " << retries << ")" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries--;
      }
    }

    if (rank0_host.empty() || rank0_host == "127.0.0.1" || rank0_host == "0.0.0.0") {
      std::cerr << "[Worker Rank " << rank_ << "] ERROR: Could not discover rank 0 address!"
                << std::endl;
      return false;
    }

    // Extract port from address or use default
    // Address format can be "IP" or "IP:PORT" from the discovery file
    size_t colon_pos = rank0_host.rfind(':');
    std::string connect_addr;

    if (colon_pos != std::string::npos) {
      // IP:PORT format already in rank0_host
      connect_addr = "tcp://" + rank0_host;
    } else {
      // Just IP, need to add port
      std::string port = "5555"; // Default port
      const char* port_env = getenv("ANARI_USD_ZMQ_PORT");
      if (port_env) {
        port = port_env;
      }

      std::stringstream ss;
      ss << "tcp://" << rank0_host << ":" << port;
      connect_addr = ss.str();
    }

    std::cout << "[Worker Rank " << rank_ << "] Connecting to broker at "
              << connect_addr << std::endl;

    dealer_->connect(connect_addr);

    // Set socket options
    int linger = 0;
    dealer_->set(zmq::sockopt::linger, linger);

    // Get local InfiniBand IP
    std::string local_ip = GetInfiniBandIP();

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    // Send READY message with rank and network info
    std::stringstream ready_msg;
    ready_msg << "READY|" << rank_ << "|" << hostname << "|" << local_ip;

    zmq::message_t empty;
    std::string ready_str = ready_msg.str();
    zmq::message_t ready(ready_str.data(), ready_str.size());

    dealer_->send(empty, zmq::send_flags::sndmore);
    dealer_->send(ready, zmq::send_flags::none);

    std::cout << "[Worker Rank " << rank_ << "] Sent READY message with local IP: "
              << local_ip << std::endl;

    // Wait for ACK
    zmq::message_t ack_empty, ack_msg;
    (void)dealer_->recv(ack_empty, zmq::recv_flags::none);
    (void)dealer_->recv(ack_msg, zmq::recv_flags::none);

    std::string ack(static_cast<const char*>(ack_msg.data()), ack_msg.size());

    if (ack == "ACK") {
      connected_ = true;
      std::cout << "[Worker Rank " << rank_ << "] Connected to broker successfully!"
                << std::endl;
      return true;
    }

    return false;

  } catch (const zmq::error_t& e) {
    std::cerr << "[Worker Rank " << rank_ << "] ZMQ Error: "
              << e.what() << std::endl;
    return false;
  }
}

bool ZmqWorker::ReceiveTask(std::vector<char>& data) {
  try {
    zmq::message_t empty, payload;

    (void)dealer_->recv(empty, zmq::recv_flags::none);
    (void)dealer_->recv(payload, zmq::recv_flags::none);

    data.resize(payload.size());
    memcpy(data.data(), payload.data(), payload.size());

    return true;
  } catch (const zmq::error_t& e) {
    std::cerr << "[Worker Rank " << rank_ << "] Receive error: "
              << e.what() << std::endl;
    return false;
  }
}

bool ZmqWorker::SendResult(const void* data, size_t size) {
  try {
    zmq::message_t empty;
    zmq::message_t payload(data, size);

    dealer_->send(empty, zmq::send_flags::sndmore);
    dealer_->send(payload, zmq::send_flags::none);

    return true;
  } catch (const zmq::error_t& e) {
    std::cerr << "[Worker Rank " << rank_ << "] Send error: "
              << e.what() << std::endl;
    return false;
  }
}

void ZmqWorker::Disconnect() {
  if (connected_) {
    std::cout << "[Worker Rank " << rank_ << "] Disconnecting..." << std::endl;
    dealer_->close();
    context_->close();
    connected_ = false;
  }
}

} // namespace usd_bridge

#endif // ANARI_USD_ENABLE_MPI