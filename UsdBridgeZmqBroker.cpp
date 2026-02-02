
#include "UsdBridgeZmqBroker.h"

#ifdef ANARI_USD_ENABLE_MPI

#include <mpi.h>
#include <zmq.hpp>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace usd_bridge {

// ============================================================================
// Helper Functions
// ============================================================================

std::string GetInfiniBandIPImpl() {
    struct ifaddrs *ifaddr, *ifa;
    std::string ib_ip;

    if (getifaddrs(&ifaddr) == -1) {
        const char* slurm_node = getenv("SLURMD_NODENAME");
        if (slurm_node) {
            return std::string(slurm_node);
        }
        return "127.0.0.1";
    }

    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;

        // Look for InfiniBand interfaces (ib0, ib1, mlx, etc.)
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

    if (ib_ip.empty()) {
        return "0.0.0.0";
    }

    return ib_ip;
}

// ============================================================================
// MPI-Based Broker Discovery
// ============================================================================

/**
 * Broadcast rank 0's InfiniBand address to all ranks via MPI.
 * Returns: "ip:port" string for all ranks to connect to.
 */
std::string BroadcastBrokerAddress(int rank, int port) {
    char broker_address[256];
    memset(broker_address, 0, sizeof(broker_address));

    if (rank == 0) {
        // Rank 0: detect IB IP and format address
        std::string ib_ip = GetInfiniBandIPImpl();
        std::stringstream ss;
        ss << ib_ip << ":" << port;
        std::string addr = ss.str();
        strncpy(broker_address, addr.c_str(), sizeof(broker_address) - 1);

        std::cout << "[Rank 0 MPI Broker] Broadcasting InfiniBand address: "
                  << broker_address << std::endl;
    }

    // MPI_Bcast: rank 0 sends, all others receive
    MPI_Bcast(broker_address, sizeof(broker_address), MPI_CHAR, 0, MPI_COMM_WORLD);

    if (rank != 0) {
        std::cout << "[Worker Rank " << rank << "] Received broker address via MPI: "
                  << broker_address << std::endl;
    }

    return std::string(broker_address);
}

// ============================================================================
// ZmqBroker Implementation (Rank 0) - MPI Version
// ============================================================================

ZmqBroker::ZmqBroker(int workerPort, int clientPort)
    : worker_port_(workerPort)
    , client_port_(clientPort)
    , initialized_(false)
{
    context_ = std::make_unique<zmq::context_t>(1);
    router_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::router);
    rep_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::rep);
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
        // Get MPI rank
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);

        if (rank != 0) {
            std::cerr << "[ZmqBroker] ERROR: Initialize() called on non-zero rank!" << std::endl;
            return false;
        }

        // Broadcast broker address to all workers via MPI
        std::string broadcast_addr = BroadcastBrokerAddress(rank, worker_port_);

        // Extract just the IP part for binding
        size_t colon_pos = broadcast_addr.find(':');
        std::string ib_ip = (colon_pos != std::string::npos)
            ? broadcast_addr.substr(0, colon_pos)
            : broadcast_addr;

        // ========== BIND ROUTER SOCKET (for workers) ==========
        std::stringstream router_bind_addr;
        router_bind_addr << "tcp://" << ib_ip << ":" << worker_port_;

        std::cout << "[Rank 0 MPI Broker] InfiniBand IP detected: " << ib_ip << std::endl;
        std::cout << "[Rank 0 MPI Broker] Binding ROUTER (workers) to " << router_bind_addr.str() << std::endl;

        router_->bind(router_bind_addr.str());
        int linger = 0;
        router_->set(zmq::sockopt::linger, linger);

        // ========== BIND REP SOCKET (for laptop client) ==========
        std::stringstream rep_bind_addr;
        rep_bind_addr << "tcp://" << ib_ip << ":" << client_port_;

        std::cout << "[Rank 0 MPI Broker] Binding REP (laptop client) to " << rep_bind_addr.str() << std::endl;

        rep_->bind(rep_bind_addr.str());
        rep_->set(zmq::sockopt::linger, linger);

        // Print SSH tunnel command
        std::cout << std::endl;
        std::cout << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║ SSH TUNNEL COMMAND FOR LAPTOP CLIENT                                          ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;
        std::cout << "ssh -N -L " << client_port_ << ":" << ib_ip << ":" << client_port_ << " \\" << std::endl;
        std::cout << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
        std::cout << " george2@jureca04.fz-juelich.de" << std::endl;
        std::cout << std::endl;

        std::cout << "[Rank 0 MPI Broker] Waiting for " << expectedWorkers
                  << " workers to connect..." << std::endl;

        // ========== WAIT FOR WORKERS TO CONNECT ==========
        auto start_time = std::chrono::steady_clock::now();
        const int timeout_seconds = 400;

        while (workers_.size() < static_cast<size_t>(expectedWorkers)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();

            if (elapsed > timeout_seconds) {
                std::cerr << "[Rank 0 MPI Broker] Timeout waiting for workers. Only "
                          << workers_.size() << "/" << expectedWorkers << " connected." << std::endl;
                return false;
            }

            zmq::pollitem_t items[] = { { *router_, 0, ZMQ_POLLIN, 0 } };
            zmq::poll(&items[0], 1, std::chrono::milliseconds(100));

            if (!(items[0].revents & ZMQ_POLLIN)) {
                continue;
            }

            zmq::message_t identity, empty, ready_msg;
            (void)router_->recv(identity, zmq::recv_flags::none);
            (void)router_->recv(empty, zmq::recv_flags::none);
            (void)router_->recv(ready_msg, zmq::recv_flags::none);

            std::string msg(static_cast<const char*>(ready_msg.data()), ready_msg.size());

            if (msg.find("READY") == 0) {
                // Parse: "READY|rank|hostname|ip"
                std::stringstream ss(msg);
                std::string token, rank_str, hostname, ip;
                std::getline(ss, token, '|');
                std::getline(ss, rank_str, '|');
                std::getline(ss, hostname, '|');
                std::getline(ss, ip, '|');

                WorkerInfo worker;
                worker.identity = std::string(static_cast<const char*>(identity.data()),
                                             identity.size());
                worker.rank = std::stoi(rank_str);
                worker.hostname = hostname;
                worker.ib_address = ip;
                worker.ready = true;

                workers_.push_back(worker);
                worker_map_[worker.identity] = worker.rank;

                std::cout << "[Rank 0 MPI Broker] Worker rank " << worker.rank
                          << " connected from " << worker.hostname
                          << " (" << worker.ib_address << ") - "
                          << workers_.size() << "/" << expectedWorkers << std::endl;

                // Send ACK
                router_->send(identity, zmq::send_flags::sndmore);
                router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                router_->send(zmq::message_t("ACK", 3), zmq::send_flags::none);
            }
        }

        initialized_ = true;
        std::cout << "[Rank 0 MPI Broker] All " << expectedWorkers
                  << " workers connected successfully!" << std::endl;
        std::cout << "[Rank 0 MPI Broker] Broker ready on ROUTER:" << worker_port_
                  << " and REP:" << client_port_ << std::endl;

        // ========== MAIN MESSAGE LOOP - Handle BOTH workers and laptop client ==========
        while (true) {
            zmq::pollitem_t items[] = {
                { *router_, 0, ZMQ_POLLIN, 0 },  // Messages from workers (ROUTER)
                { *rep_, 0, ZMQ_POLLIN, 0 }      // Messages from laptop client (REP)
            };

            zmq::poll(&items[0], 2, std::chrono::milliseconds(100));

            // ========== Handle GUI/Laptop Client Request (REP socket) ==========
            if (items[1].revents & ZMQ_POLLIN) {
                zmq::message_t request;
                rep_->recv(request);

                std::string message(static_cast<const char*>(request.data()), request.size());
                std::cout << "[Rank 0 MPI Broker] GUI requested: " << message << std::endl;

                // Build worker list response
                std::stringstream response;
                response << "WORKER_LIST|";

                for (size_t i = 0; i < workers_.size(); ++i) {
                    if (i > 0) response << ";";
                    response << workers_[i].rank << ":"
                            << workers_[i].hostname << ":"
                            << workers_[i].ib_address;
                }

                std::string reply = response.str();
                std::cout << "[Rank 0 MPI Broker] Sending worker list to GUI: " << reply << std::endl;
                rep_->send(zmq::message_t(reply.data(), reply.size()), zmq::send_flags::none);
            }

            // ========== Handle Worker Messages (ROUTER socket) ==========
            if (items[0].revents & ZMQ_POLLIN) {
                std::string workerId;
                std::vector<uint8_t> data;

                if (ReceiveFromWorker(workerId, data)) {
                    std::string message(data.begin(), data.end());
                    std::cout << "[Rank 0 MPI Broker] Received from worker: " << message << std::endl;
                    // Handle worker message as needed
                }
            }
        }

        return true;

    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] ZMQ Error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqBroker::SendToWorker(const std::string& workerId, const void* data, size_t size) {
    try {
        zmq::message_t identity(workerId.data(), workerId.size());
        zmq::message_t empty;
        zmq::message_t payload(data, size);

        router_->send(identity, zmq::send_flags::sndmore);
        router_->send(empty, zmq::send_flags::sndmore);
        router_->send(payload, zmq::send_flags::none);

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] Send to worker error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqBroker::SendToClient(const std::string& clientId, const void* data, size_t size) {
    // REP socket automatically replies to the last requester
    // (clientId is ignored for REP-REQ pattern)
    try {
        zmq::message_t payload(data, size);
        rep_->send(payload, zmq::send_flags::none);
        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] Send to client error: " << e.what() << std::endl;
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

bool ZmqBroker::ReceiveFromWorker(std::string& workerId, std::vector<uint8_t>& data) {
    try {
        zmq::message_t identity, empty, payload;

        (void)router_->recv(identity, zmq::recv_flags::none);
        (void)router_->recv(empty, zmq::recv_flags::none);
        (void)router_->recv(payload, zmq::recv_flags::none);

        workerId = std::string(static_cast<const char*>(identity.data()), identity.size());
        data.resize(payload.size());
        memcpy(data.data(), payload.data(), payload.size());

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] Receive from worker error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqBroker::ReceiveFromClient(std::string& message) {
    try {
        zmq::message_t request;
        rep_->recv(request);

        message = std::string(static_cast<const char*>(request.data()), request.size());
        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] Receive from client error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqBroker::ReplyToClient(const std::string& reply) {
    try {
        rep_->send(zmq::message_t(reply.data(), reply.size()), zmq::send_flags::none);
        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] Reply to client error: " << e.what() << std::endl;
        return false;
    }
}

void ZmqBroker::Shutdown() {
    if (initialized_) {
        std::cout << "[Rank 0 MPI Broker] Shutting down..." << std::endl;
        router_->close();
        rep_->close();
        context_->close();
        initialized_ = false;
    }
}

// ============================================================================
// ZmqWorker Implementation (Rank 1-N) - MPI Version
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
    // MPI-based discovery: broadcast already happened during MPI initialization
    // Just receive the address via MPI_Bcast
    std::string addr = BroadcastBrokerAddress(rank_, 5555);
    std::cout << "[Worker Rank " << rank_ << "] Resolved rank 0 address via MPI: "
              << addr << std::endl;
    return addr;
}

bool ZmqWorker::Connect() {
    if (connected_) return true;

    try {
        // Get broker address via MPI broadcast
        std::string rank0_address = ResolveRank0Address();

        if (rank0_address.empty() || rank0_address == "0.0.0.0:5555") {
            std::cerr << "[Worker Rank " << rank_ << "] ERROR: Invalid broker address from MPI!" << std::endl;
            return false;
        }

        // Build connection address
        std::string connect_addr = "tcp://" + rank0_address;
        std::cout << "[Worker Rank " << rank_ << "] Connecting to broker at " << connect_addr << std::endl;

        dealer_->connect(connect_addr);
        int linger = 0;
        dealer_->set(zmq::sockopt::linger, linger);

        // Send READY message
        std::string local_ip = GetInfiniBandIP();
        char hostname[256];
        gethostname(hostname, sizeof(hostname));

        std::stringstream ready_msg;
        ready_msg << "READY|" << rank_ << "|" << hostname << "|" << local_ip;

        zmq::message_t empty;
        std::string ready_str = ready_msg.str();
        zmq::message_t ready(ready_str.data(), ready_str.size());

        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(ready, zmq::send_flags::none);

        std::cout << "[Worker Rank " << rank_ << "] Sent READY message with local IP: " << local_ip << std::endl;

        // Wait for ACK
        zmq::message_t ack_empty, ack_msg;
        (void)dealer_->recv(ack_empty, zmq::recv_flags::none);
        (void)dealer_->recv(ack_msg, zmq::recv_flags::none);

        std::string ack(static_cast<const char*>(ack_msg.data()), ack_msg.size());

        if (ack == "ACK") {
            connected_ = true;
            std::cout << "[Worker Rank " << rank_ << "] Connected to broker successfully via MPI discovery!" << std::endl;
            return true;
        }

        return false;

    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] ZMQ Error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqWorker::ReceiveTask(std::vector<uint8_t>& data) {
    try {
        zmq::message_t empty, payload;

        (void)dealer_->recv(empty, zmq::recv_flags::none);
        (void)dealer_->recv(payload, zmq::recv_flags::none);

        data.resize(payload.size());
        memcpy(data.data(), payload.data(), payload.size());

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] Receive error: " << e.what() << std::endl;
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
        std::cerr << "[Worker Rank " << rank_ << "] Send error: " << e.what() << std::endl;
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