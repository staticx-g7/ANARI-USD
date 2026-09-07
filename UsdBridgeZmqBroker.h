#ifndef USD_BRIDGE_ZMQ_BROKER_H
#define USD_BRIDGE_ZMQ_BROKER_H

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <zmq.hpp>

#include "AnariUsdProtocol.h"
#include "UsdBridge/UsdBridgeMemoryStore.h"

namespace usd_bridge {

// Compatibility aliases for the canonical protocol header.
using anari_usd_protocol::ZmqMessageType;
using anari_usd_protocol::ZmqSceneChangeType;
using anari_usd_protocol::ZmqPropertyValuetype;
using anari_usd_protocol::ZmqFileRequest;
using anari_usd_protocol::ZmqFileChunk;
using anari_usd_protocol::ZmqFileComplete;
using anari_usd_protocol::ZmqFileNotification;
using anari_usd_protocol::ZmqPropertyResponse;
using anari_usd_protocol::ZmqSceneUpdate;

constexpr uint32_t USD_FILE_MAGIC = anari_usd_protocol::ANARI_USD_MAGIC;
constexpr size_t DEFAULT_CHUNK_SIZE = anari_usd_protocol::ANARI_USD_DEFAULT_CHUNK_SIZE;

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
    // Legacy methods - not used in DEALER-ROUTER architecture
    // bool ReceiveFromClient(std::string& message);
    // bool ReplyToClient(const std::string& reply);

    // Notification forwarding (Worker → Laptop)
    bool ForwardNotificationToClient(const ZmqFileNotification& notification);
    bool ForwardSceneUpdateToClient(const ZmqSceneUpdate& update);

    std::string GetInfiniBandIP();
    bool IsInitialized() const { return initialized_; }
    const std::vector<WorkerInfo>& GetConnectedWorkers() const { return workers_; }

    // Property query methods
    bool GetPropertyAsInt32(const std::string& propertyName, int32_t& value);
    bool GetPropertyAsString(const std::string& propertyName, std::string& value);

    // In-flight rank-0 local file transfer (interleaved, non-blocking):
    // the request handler queues one of these and the message loop advances
    // every transfer a few chunks on each iteration, so the loop can service
    // worker traffic / other clients between chunks instead of stalling on
    // one long blocking stream.
    struct PendingFileTransfer {
        std::string client_id;
        std::shared_ptr<const UsdBridgeMemoryStore::FileEntry> entry;  // immutable snapshot
        uint32_t request_id = 0;
        char filename[256] = {0};
        size_t chunk_size = 0;
        size_t offset = 0;
        bool is_marker = false;  // zero-chunk "complete" (e.g. __frame_complete__)
        std::string marker_name;  // filename for a marker transfer
        std::vector<std::string> files_in_frame;  // frame transfers: remaining files to queue
        int marker_total = 0;  // total file count for the frame marker
    };

private:
    void MessageLoopThread();  // Background thread for message routing
    void SSHReminderThread();  // Periodic SSH command reminder
    void StatusPrinterThread();  // Periodic status table (every 10s)

    // Advance up to `maxTransfers` pending rank-0 transfers by one chunk each.
    // Called from the message loop; safe to call with an empty list.
    void TickPendingTransfers(int maxTransfers);
    void QueueRank0FileTransfer(const std::string& client_id, uint32_t request_id,
                                const char* filename, uint32_t chunk_size);

    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> router_;        // Port worker_port_ - for workers (DEALER)
    std::unique_ptr<zmq::socket_t> client_router_; // Port client_port_ - for laptop clients (DEALER)

    int worker_port_;
    int client_port_;
    bool initialized_;

    std::vector<WorkerInfo> workers_;
    std::map<std::string, int> worker_map_;
    // request_id -> client identity, for routing worker responses back to the
    // requesting laptop client. Numeric key (was std::string): no string
    // formatting per message, and request_id space is the natural identity.
    std::map<uint32_t, std::string> client_map_;
    std::set<std::string> client_ids_;              // Unique client identities for notifications

    // Broadcast LIST requests: track how many worker responses are still
    // outstanding so the mapping can be erased exactly when the last one
    // arrives (the old "keep forever" behavior grew this map unboundedly).
    struct BroadcastState {
        int pending = 0;
        std::chrono::steady_clock::time_point created;
    };
    std::map<uint32_t, BroadcastState> broadcast_state_;

    // Frame requests (REQ_GET_FRAME): the broker must keep the client mapping
    // alive across every per-file complete and only erase it at the
    // __frame_complete__ marker (tracked here, since per-file completes for a
    // frame must NOT drop the mapping). Timestamped so stale state can be reaped.
    std::map<uint32_t, std::chrono::steady_clock::time_point> frame_requests_;

    // In-flight rank-0 local file transfers (see PendingFileTransfer).
    std::vector<PendingFileTransfer> pending_transfers_;

    // Thread management
    std::thread message_loop_thread_;
    std::atomic<bool> message_loop_active_{false};
    std::thread ssh_reminder_thread_;
    std::atomic<bool> ssh_reminder_active_{false};
    std::thread status_printer_thread_;
    std::atomic<bool> status_printer_active_{false};
    std::string broker_ip_;  // Store broker IP for SSH reminder
};

class ZmqWorker {
public:
    ZmqWorker(const std::string& brokerAddress, int rank);
    ~ZmqWorker();

    bool Connect();
    void Disconnect();

    bool ReceiveTask(std::vector<uint8_t>& data);
    bool SendResult(const void* data, size_t size);

    // File request handling
    bool CheckForFileRequest(ZmqFileRequest& request, bool blocking = false);
    bool SendFileChunk(uint32_t requestId, const std::string& filename,
                       const void* data, size_t dataSize,
                       uint64_t totalSize, uint64_t offset);
    bool SendFileComplete(uint32_t requestId, const std::string& filename, uint64_t totalSize);
    bool SendNoFile(uint32_t requestId, const std::string& filename);

    // Push notification handling
    bool SendFileNotification(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128 = nullptr);
    bool SendFileNotificationV2(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128, const uint64_t* hashPrev128, bool hasOldData);
    bool SendCommitNotification(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128 = nullptr);

    // Typed scene / property update handling
    bool SendSceneUpdate(const ZmqSceneUpdate& update);
    bool SendCommitSceneUpdate(uint64_t commitId, uint64_t revision, uint64_t timestamp);

    std::string GetInfiniBandIP();
    bool IsConnected() const { return connected_; }

private:
    std::string ResolveRank0Address();

    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> dealer_;
    std::string broker_address_;
    int rank_;
    bool connected_;
    
    // Thread safety for socket operations
    std::mutex socket_mutex_;
};

} // namespace usd_bridge

#endif // USD_BRIDGE_ZMQ_BROKER_H