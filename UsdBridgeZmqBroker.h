#ifndef USD_BRIDGE_ZMQ_BROKER_H
#define USD_BRIDGE_ZMQ_BROKER_H

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <zmq.hpp>

namespace usd_bridge {

struct WorkerInfo {
    std::string identity;
    int rank;
    std::string hostname;
    std::string ib_address;
    bool ready;
};

class ZmqBroker {
public:
    explicit ZmqBroker(int workerPort = 5555, int clientPort = 5556);
    ~ZmqBroker();

    bool Initialize(int expectedWorkers);
    void Shutdown();

    bool SendToWorker(const std::string& workerId, const void* data, size_t size);
    bool SendToClient(const std::string& clientId, const void* data, size_t size);
    bool BroadcastToWorkers(const void* data, size_t size);
    bool ReceiveFromWorker(std::string& workerId, std::vector<uint8_t>& data);
    bool ReceiveFromClient(std::string& message);
    bool ReplyToClient(const std::string& reply);

    std::string GetInfiniBandIP();
    bool IsInitialized() const { return initialized_; }
    const std::vector<WorkerInfo>& GetConnectedWorkers() const { return workers_; }

private:
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> router_;  // Port worker_port_ - for workers (DEALER)
    std::unique_ptr<zmq::socket_t> rep_;     // Port client_port_ - for laptop clients (REQ)

    int worker_port_;
    int client_port_;
    bool initialized_;

    std::vector<WorkerInfo> workers_;
    std::map<std::string, int> worker_map_;
};

class ZmqWorker {
public:
    ZmqWorker(const std::string& brokerAddress, int rank);
    ~ZmqWorker();

    bool Connect();
    void Disconnect();

    bool ReceiveTask(std::vector<uint8_t>& data);
    bool SendResult(const void* data, size_t size);

    std::string GetInfiniBandIP();
    bool IsConnected() const { return connected_; }

private:
    std::string ResolveRank0Address();

    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> dealer_;
    std::string broker_address_;
    int rank_;
    bool connected_;
};

} // namespace usd_bridge

#endif // USD_BRIDGE_ZMQ_BROKER_H