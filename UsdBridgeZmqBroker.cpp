
#include "UsdBridgeZmqBroker.h"
#include "UsdBridge/UsdBridgeMemoryStore.h"
#include "UsdBridge/xxhash/xxhash.h"
#include "UsdBridge/DiffCaptureStatus.h"

#include <zmq.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

#ifdef ANARI_USD_ENABLE_MPI
#include <mpi.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace usd_bridge {

// ============================================================================
// Helper Functions
// ============================================================================

// Simple JSON array concatenation helper
static std::string ExtractJsonArray(const std::string& json) {
    // Find the opening bracket of the files array
    size_t start = json.find("\"files\":[");
    if (start == std::string::npos) {
        return "";
    }
    start = json.find('[', start);
    if (start == std::string::npos) {
        return "";
    }
    
    // Find matching closing bracket
    int bracket_count = 1;
    size_t end = start + 1;
    for (; end < json.size() && bracket_count > 0; ++end) {
        if (json[end] == '[') bracket_count++;
        else if (json[end] == ']') bracket_count--;
    }
    
    if (bracket_count != 0) {
        return "";
    }
    
    // Extract array content (without the outer brackets)
    return json.substr(start + 1, end - start - 2);
}

std::string GetInfiniBandIPImpl() {
#ifdef ANARI_USD_ENABLE_MPI
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
#else
    // Non-MPI mode: always use localhost
    return "127.0.0.1";
#endif
}

// ============================================================================
// MPI-Based Broker Discovery
// ============================================================================

/**
 * Get broker address for connection.
 * MPI mode: Broadcast rank 0's InfiniBand address to all ranks via MPI.
 * Non-MPI mode: Return localhost address.
 * Returns: "ip:port" string to connect to.
 */
std::string GetBrokerAddress(int rank, int port) {
#ifdef ANARI_USD_ENABLE_MPI
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
#else
    // Non-MPI mode: always use localhost
    std::stringstream ss;
    ss << "127.0.0.1:" << port;
    std::string addr = ss.str();
    
    std::cout << "[Non-MPI ZMQ Broker] Using localhost address: " << addr << std::endl;
    return addr;
#endif
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
    client_router_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::router);
}

ZmqBroker::~ZmqBroker() {
    Shutdown();
}

std::string ZmqBroker::GetInfiniBandIP() {
    return GetInfiniBandIPImpl();
}

bool ZmqBroker::Initialize(int expectedWorkers) {
    if (initialized_) return true;

    std::cerr << "[Rank 0 MPI Broker] ZmqBroker::Initialize() called" << std::endl << std::flush;

    try {
#ifdef ANARI_USD_ENABLE_MPI
        // Get MPI rank
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);

        if (rank != 0) {
            std::cerr << "[ZmqBroker] ERROR: Initialize() called on non-zero rank!" << std::endl;
            return false;
        }

        // Check if we're in "MPI but 0 workers" mode (essentially non-MPI mode)
        bool isSingleRankMode = (expectedWorkers == 0);
        std::string ib_ip;  // Declare here so it's available in both branches
        
        if (isSingleRankMode) {
            // Single rank mode: use localhost for simpler SSH tunneling
            ib_ip = "127.0.0.1";
            broker_ip_ = ib_ip;
            std::cout << "[Rank 0 MPI Broker] Single-rank mode detected - using localhost IP: " << ib_ip << std::endl;
        } else {
            // Multi-rank MPI mode: use InfiniBand IP
            // Broadcast broker address to all workers via MPI
            std::string broadcast_addr = GetBrokerAddress(rank, worker_port_);

            // Extract just the IP part for binding
            size_t colon_pos = broadcast_addr.find(':');
            ib_ip = (colon_pos != std::string::npos)
                ? broadcast_addr.substr(0, colon_pos)
                : broadcast_addr;

            // Store broker IP for SSH reminder thread
            broker_ip_ = ib_ip;

            std::cout << "[Rank 0 MPI Broker] InfiniBand IP detected: " << ib_ip << std::endl;
        }
#else
        // Non-MPI mode: use localhost
        std::string ib_ip = "127.0.0.1";
        broker_ip_ = ib_ip;
        
        std::cout << "[Non-MPI ZMQ Broker] Using localhost IP: " << ib_ip << std::endl;
#endif
        
        // For non-MPI builds, always treat as single-rank mode
#ifndef ANARI_USD_ENABLE_MPI
        bool isSingleRankMode = true;
#else
        // isSingleRankMode already defined above
#endif

        // Set linger option for all sockets
        int linger = 0;

        // ========== BIND ROUTER SOCKET (for workers) ==========
        // In single-rank mode, skip binding worker socket (no workers to connect)
        if (!isSingleRankMode) {
            std::stringstream router_bind_addr;
            router_bind_addr << "tcp://" << ib_ip << ":" << worker_port_;

            std::cout << "[ZMQ Broker] Binding ROUTER (workers) to " << router_bind_addr.str() << std::endl;

            router_->bind(router_bind_addr.str());
            router_->set(zmq::sockopt::linger, linger);
        } else {
            std::cout << "[ZMQ Broker] Single-rank mode - skipping worker socket binding (no workers)" << std::endl;
        }

        // ========== BIND CLIENT ROUTER SOCKET (for laptop client - DEALER) ==========
        std::stringstream client_bind_addr;
        client_bind_addr << "tcp://" << ib_ip << ":" << client_port_;

        std::cout << "[ZMQ Broker] Binding CLIENT ROUTER (laptop DEALER) to "
                  << client_bind_addr.str() << std::endl;

        client_router_->bind(client_bind_addr.str());
        client_router_->set(zmq::sockopt::linger, linger);

        // Print connection information
        std::cerr << std::endl;
        std::cerr << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cerr << "║ ZMQ BROKER CONNECTION INFORMATION                                             ║" << std::endl;
        std::cerr << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;
        
#ifdef ANARI_USD_ENABLE_MPI
        if (expectedWorkers > 0) {
            // MPI mode: SSH tunnel for remote access
            std::cerr << "SSH TUNNEL COMMAND FOR REMOTE ACCESS:" << std::endl;
            std::cerr << "ssh -N -L " << client_port_ << ":" << ib_ip << ":" << client_port_ << " \\" << std::endl;
            std::cerr << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
            std::cerr << " george2@jureca04.fz-juelich.de" << std::endl;
        } else {
            // MPI but 0 workers case (data is too small to split)
            // In single-rank mode, broker binds to localhost
            std::cerr << "REMOTE LOCALHOST BROKER (NO MPI SPLIT):" << std::endl;
            std::cerr << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
            std::cerr << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
            std::cerr << " george2@jureca04.fz-juelich.de" << std::endl;
        }
#else
        // Non-MPI mode: Direct localhost connection or SSH tunnel if remote GUI
        std::cerr << "REMOTE LOCALHOST BROKER (NON-MPI / MAIN GUI):" << std::endl;
        std::cerr << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
        std::cerr << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
        std::cerr << " george2@jureca04.fz-juelich.de" << std::endl;
        
        std::cerr << "\nDIRECT LOCAL CONNECTION (if running on same machine):" << std::endl;
        std::cerr << "Connect to: localhost:" << client_port_ << std::endl;
#endif
        
        std::cerr << std::flush;
        std::cerr << std::endl;

#ifdef ANARI_USD_ENABLE_MPI
        // MPI mode: Wait for MPI workers to connect
        std::cout << "[MPI ZMQ Broker] Waiting for " << expectedWorkers
                  << " MPI workers to connect..." << std::endl;

        auto starttime = std::chrono::steady_clock::now();
        const int timeoutseconds = 400;
        int nonZeroWorkersConnected = 0;  // Count only ranks 1-N

        while (nonZeroWorkersConnected < expectedWorkers)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - starttime).count();
            if (elapsed >= timeoutseconds)
            {
                std::cerr << "Rank 0 (MPI Broker): Timeout waiting for workers. Only "
                          << nonZeroWorkersConnected << "/" << expectedWorkers << " connected." << std::endl;
                return false;
            }

            zmq::pollitem_t items[] = { {*router_, 0, ZMQ_POLLIN, 0} };
            zmq::poll(items, 1, std::chrono::milliseconds(100));

            if (!(items[0].revents & ZMQ_POLLIN))
                continue;

            zmq::message_t identity, empty, readymsg;
            (void)router_->recv(identity, zmq::recv_flags::none);
            (void)router_->recv(empty, zmq::recv_flags::none);
            (void)router_->recv(readymsg, zmq::recv_flags::none);

            std::string msg(static_cast<const char*>(readymsg.data()), readymsg.size());
            if (msg.find("READY") == 0)
            {
                // Parse READY|rank|hostname|ip
                std::stringstream ss(msg);
                std::string token, rankstr, hostname, ip;
                std::getline(ss, token, '|');
                std::getline(ss, rankstr, '|');
                std::getline(ss, hostname, '|');
                std::getline(ss, ip, '|');

                WorkerInfo worker;
                worker.identity = std::string(static_cast<const char*>(identity.data()), identity.size());
                worker.rank = std::stoi(rankstr);
                worker.hostname = hostname;
                worker.ib_address = ip;
                worker.ready = true;

                workers_.push_back(worker);
                worker_map_[worker.identity] = worker.rank;
                GetDiffCaptureStatus().RegisterRank(worker.rank, worker.hostname);

                // Only count non-zero ranks toward expectedWorkers
                if (worker.rank != 0)
                {
                    nonZeroWorkersConnected++;
                }

                std::cout << "Rank 0 (MPI Broker): Worker rank " << worker.rank
                          << " connected from " << worker.hostname << " (" << worker.ib_address << ")";

                if (worker.rank == 0)
                {
                    std::cout << " [UNEXPECTED - RANK 0 SHOULD NOT CONNECT]";
                }

                std::cout << " - " << nonZeroWorkersConnected << "/" << expectedWorkers
                          << " non-zero workers" << std::endl;

                // Send ACK
                router_->send(identity, zmq::send_flags::sndmore);
                router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                router_->send(zmq::message_t("ACK", 3), zmq::send_flags::none);
            }
        }

        // MANUALLY ADD RANK 0 to the workers list so laptop can discover it
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        WorkerInfo rank0Worker;
        rank0Worker.identity = "BROKER_RANK0";
        rank0Worker.rank = 0;
        rank0Worker.hostname = hostname;
        rank0Worker.ib_address = broker_ip_;
        rank0Worker.ready = true;
        workers_.push_back(rank0Worker);
        worker_map_[rank0Worker.identity] = 0;
        GetDiffCaptureStatus().RegisterRank(0, hostname);

        std::cout << "Rank 0 (MPI Broker): Added rank 0 to worker list (self-service mode)" << std::endl;
        std::cout << "Rank 0 (MPI Broker): Total workers including rank 0: " << workers_.size() << std::endl;

        std::cout << "[MPI ZMQ Broker] All " << expectedWorkers
                  << " MPI workers connected successfully!" << std::endl;
#else
        // Non-MPI mode: No workers to wait for, just add self as rank 0
        std::cout << "[Non-MPI ZMQ Broker] Running in single-process mode, no external workers." << std::endl;
        
        WorkerInfo selfWorker;
        selfWorker.identity = "BROKER_SELF";
        selfWorker.rank = 0;
        selfWorker.hostname = "localhost";
        selfWorker.ib_address = broker_ip_;
        selfWorker.ready = true;
        workers_.push_back(selfWorker);
        worker_map_[selfWorker.identity] = 0;
        GetDiffCaptureStatus().RegisterRank(0, "localhost");
        
        std::cout << "[Non-MPI ZMQ Broker] Added self as worker rank 0" << std::endl;
#endif

        initialized_ = true;
        std::cout << "[Rank 0 MPI Broker] Broker ready on WORKER_ROUTER:" << worker_port_
                  << " and CLIENT_ROUTER:" << client_port_ << std::endl;

        // ========== START BACKGROUND THREAD FOR MESSAGE ROUTING ==========
        message_loop_active_ = true;
        message_loop_thread_ = std::thread(&ZmqBroker::MessageLoopThread, this);
        
        // ========== START SSH REMINDER THREAD ==========
        ssh_reminder_active_ = true;
        ssh_reminder_thread_ = std::thread(&ZmqBroker::SSHReminderThread, this);

        // ========== START STATUS PRINTER THREAD ==========
        status_printer_active_ = true;
        status_printer_thread_ = std::thread(&ZmqBroker::StatusPrinterThread, this);

        std::cout << "[Rank 0 MPI Broker] Started background message routing thread" << std::endl;
        std::cout << "[Rank 0 MPI Broker] SSH tunnel reminder will print every 60 seconds" << std::endl;
        std::cout << "[Rank 0 MPI Broker] DiffCapture status table will print every 10 seconds" << std::endl;
        std::cout << "[Rank 0 MPI Broker] Rank 0 can now proceed to render geometry!" << std::endl;

        return true;

    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] ZMQ Error: " << e.what() << std::endl;
        return false;
    }
}

void ZmqBroker::MessageLoopThread() {
#ifdef ANARI_USD_ENABLE_MPI
    std::cout << "[MPI Broker THREAD] Message loop thread started" << std::endl;
#else
    std::cout << "[Non-MPI Broker THREAD] Message loop thread started" << std::endl;
#endif
    
    try {
        // ========== MAIN MESSAGE LOOP - Handle BOTH workers and laptop client ==========
        while (message_loop_active_) {
            zmq::pollitem_t items[] = {
                { *router_, 0, ZMQ_POLLIN, 0 },        // Messages from workers (ROUTER)
                { *client_router_, 0, ZMQ_POLLIN, 0 }  // Messages from laptop client (ROUTER)
            };

            zmq::poll(&items[0], 2, std::chrono::milliseconds(100));

            // ========== Handle Laptop Client Request (CLIENT ROUTER socket) ==========
            if (items[1].revents & ZMQ_POLLIN) {
                zmq::message_t client_identity, empty, request;
                
                (void)client_router_->recv(client_identity, zmq::recv_flags::none);
                (void)client_router_->recv(empty, zmq::recv_flags::none);
                (void)client_router_->recv(request, zmq::recv_flags::none);
                
                std::string client_id(static_cast<const char*>(client_identity.data()), 
                                     client_identity.size());

                // DEBUG: Log all incoming client requests - FIRST THING
                std::cout << "[Rank 0 MPI Broker] DEBUG: Received client request - size=" 
                          << request.size() << " bytes, client_id=" << client_id.substr(0, 8) << "..."
                          << ", sizeof(ZmqFileRequest)=" << sizeof(ZmqFileRequest) << std::endl;
                
                // Check if request is large enough to be a binary message
                if (request.size() < sizeof(ZmqFileRequest)) {
                    std::cout << "[Rank 0 MPI Broker] DEBUG: Request too small for binary message (" 
                              << request.size() << " bytes), treating as string" << std::endl;
                    // Fall through to string handling below
                } else {
                    // Check if this is a file request (structured binary message)
                    ZmqFileRequest* fileReq = static_cast<ZmqFileRequest*>(request.data());

                    std::cout << "[Rank 0 MPI Broker] DEBUG: Request magic=0x" 
                              << std::hex << fileReq->magic << std::dec
                              << ", type=" << fileReq->message_type
                              << ", target_rank=" << fileReq->target_rank
                              << ", expected_magic=0x" << std::hex << USD_FILE_MAGIC << std::dec << std::endl;
                    
                    // Check if magic matches
                    if (fileReq->magic != USD_FILE_MAGIC) {
                        std::cout << "[Rank 0 MPI Broker] DEBUG: Magic number MISMATCH! Got 0x" 
                                  << std::hex << fileReq->magic << ", expected 0x" << USD_FILE_MAGIC << std::dec << std::endl;
                    }
                    
                    // SPECIAL CASE: Non-MPI mode - handle file list requests directly
                    // In non-MPI mode, workers_.size() == 1 (only rank 0)
                    // File list requests should be handled directly from g_rankMemoryStore[0]
                    if (fileReq->magic == USD_FILE_MAGIC &&
                        fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES) &&
                        workers_.size() == 1) {  // Non-MPI mode: only rank 0 exists
                        std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: Handling file list request directly" << std::endl;
                        
                        // Store client identity for response routing
                        std::string request_key = std::to_string(fileReq->request_id);
                        client_map_[request_key] = client_id;
                        client_ids_.insert(client_id);
                        
                        // Get rank 0's files
                        // NOTE: g_rankMemoryStore is a single pointer, not an array
                        // Use *g_rankMemoryStore instead of g_rankMemoryStore[0]
                        if (!g_rankMemoryStore) {
                            std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: ERROR - g_rankMemoryStore is NULL!" << std::endl;
                            continue;
                        }
                        auto files = g_rankMemoryStore->ListFiles();
                        std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: Found " << files.size() << " files in rank 0 memory store" << std::endl;
                        
                        // Build JSON response
                        std::stringstream jsonResponse;
                        jsonResponse << "{\"rank\":" << 0 << ",\"files\":[";
                        bool first = true;
                        for (const auto& filename : files) {
                            if (filename.find(".usda.usda") != std::string::npos) continue;
                             const auto& entry = g_rankMemoryStore->GetFile(filename);
                             uint64_t hLo = entry->hash128[0];
                             uint64_t hHi = entry->hash128[1];
                             if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
                             if (!first) jsonResponse << ",";
                             jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << entry->size() << ",\"mime\":\"" << entry->mime_type << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";
                             first = false;
                        }
                        jsonResponse << "]}";
                        
                        std::string jsonStr = jsonResponse.str();
                        std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: Built JSON response: " << jsonStr.substr(0, 100) << "..." << std::endl;
                        
                        // Send as file chunk response
                        ZmqFileChunk response;
                        memset(&response, 0, sizeof(response));
                        response.magic = USD_FILE_MAGIC;
                        response.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
                        response.request_id = fileReq->request_id;
                        response.source_rank = 0;
                        strncpy(response.filename, "__file_list__.json", sizeof(response.filename) - 1);
                        response.file_size = jsonStr.size();
                        response.chunk_offset = 0;
                        response.chunk_size = jsonStr.size();
                        
                        // Allocate combined message
                        size_t totalSize = sizeof(response) + jsonStr.size();
                        std::vector<uint8_t> msgData(totalSize);
                        memcpy(msgData.data(), &response, sizeof(response));
                        memcpy(msgData.data() + sizeof(response), jsonStr.data(), jsonStr.size());
                        
                        // Send to client
                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);
                        
                        // Send completion
                        ZmqFileComplete complete;
                        memset(&complete, 0, sizeof(complete));
                        complete.magic = USD_FILE_MAGIC;
                        complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
                        complete.request_id = fileReq->request_id;
                        complete.source_rank = 0;
                        strncpy(complete.filename, "__file_list__.json", sizeof(complete.filename) - 1);
                        complete.total_size = jsonStr.size();
                        
                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);
                        
                        std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: Sent file list (" << files.size() << " files)" << std::endl;
                        
                        client_map_.erase(request_key);
                        continue;  // Done handling this request
                    }

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE) ||
                         fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES))) {

                        std::cout << "[Rank 0 MPI Broker] ";
                        if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES)) {
                            std::cout << "File list request from laptop client "
                                      << client_id.substr(0, 8) << "...: "
                                      << "rank " << fileReq->target_rank << std::endl;
                        } else {
                            std::cout << "File request from laptop client "
                                      << client_id.substr(0, 8) << "...: "
                                      << fileReq->filename << " (rank " << fileReq->target_rank << ")"
                                      << std::endl;
                        }

                        // Store client identity for response routing
                        std::string request_key = std::to_string(fileReq->request_id);
                        client_map_[request_key] = client_id;
                        client_ids_.insert(client_id);

                        // Track broadcast file list requests for aggregation
                        if (fileReq->target_rank == -1 &&
                            fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES)) {
                            // Initialize aggregation for this request
                            std::cout << "[Rank 0 MPI Broker] Setting up aggregation for broadcast file list request ID "
                                      << fileReq->request_id << std::endl;
                        }

                        // Route request to appropriate worker
                        if (fileReq->target_rank == -1)
                        {
                            // BROADCAST: Send request to ALL workers (ranks 0-15)
                            std::cout << "Rank 0 (MPI Broker): Broadcasting file request to ALL workers (rank -1) - Processing rank 0 locally" << std::endl;
                            
                            // Forward request to workers 1-15 via ZMQ
                            for (const auto& worker : workers_)
                            {
                                if (worker.rank == 0) continue; // Skip rank 0 (handled locally)
                                router_->send(zmq::buffer(worker.identity), zmq::send_flags::sndmore);
                                router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                router_->send(zmq::message_t(request.data(), request.size()), zmq::send_flags::none);
                            }
                            
                            // Process rank 0's file list locally (immediate response)
                            if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES) && g_rankMemoryStore) {
                                std::cout << "[Rank 0 MPI Broker] Processing rank 0 file list for broadcast request ID " << fileReq->request_id << std::endl;
                                
                                // Get rank 0's files
                                auto files = g_rankMemoryStore->ListFiles();
                                
                                // Build JSON response
                                std::stringstream jsonResponse;
                                jsonResponse << "{\"rank\":" << 0 << ",\"files\":[";
                                bool first = true;
                                for (const auto& filename : files) {
                                    if (filename.find(".usda.usda") != std::string::npos) continue;
                                    
                                    const auto& entry = g_rankMemoryStore->GetFile(filename);
                                    uint64_t hLo = entry->hash128[0]; uint64_t hHi = entry->hash128[1];
                                    if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
                                    if (!first) jsonResponse << ",";
                                    jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << entry->size() << ",\"mime\":\"" << entry->mime_type << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";
                                    first = false;
                                }
                                jsonResponse << "]}";
                                
                                std::string jsonStr = jsonResponse.str();
                                
                                        // Send as file chunk response
                                        ZmqFileChunk response;
                                        memset(&response, 0, sizeof(response));
                                        response.magic = USD_FILE_MAGIC;
                                        response.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
                                        response.request_id = fileReq->request_id;
                                        response.source_rank = 0;
                                        strncpy(response.filename, "__file_list__.json", sizeof(response.filename) - 1);
                                        response.file_size = jsonStr.size();
                                        response.chunk_offset = 0;
                                        response.chunk_size = jsonStr.size();
                                        
                                        // Allocate combined message
                                        size_t totalSize = sizeof(response) + jsonStr.size();
                                        std::vector<uint8_t> msgData(totalSize);
                                        memcpy(msgData.data(), &response, sizeof(response));
                                        memcpy(msgData.data() + sizeof(response), jsonStr.data(), jsonStr.size());
                                        
                                        // Send to client
                                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);

                                        // Ensure we notify the client that this rank is done
                                        ZmqFileComplete complete;
                                        memset(&complete, 0, sizeof(complete));
                                        complete.magic = USD_FILE_MAGIC;
                                        complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
                                        complete.request_id = fileReq->request_id;
                                        complete.source_rank = 0;
                                        strncpy(complete.filename, "__file_list__.json", sizeof(complete.filename) - 1);
                                        complete.total_size = jsonStr.size();

                                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);
                                        
                                        std::cout << "[Rank 0 MPI Broker] Sent rank 0 file list (" << files.size() << " files) for broadcast request" << std::endl;
                            }
                            continue; // Done with broadcast
                        }
                        
                        if (fileReq->target_rank >= 0)
                        {
                            // Special case: Rank 0 file requests are served DIRECTLY by broker from memory
                            if (fileReq->target_rank == 0)
                            {
                                std::cout << "Rank 0 (MPI Broker): Serving rank 0 file request DIRECTLY";

                                if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES))
                                {
                                    std::cout << " - LIST FILES" << std::endl;
                                }
                                else
                                {
                                    std::cout << " - FILE: " << fileReq->filename << std::endl;
                                }

                                // Access rank 0's memory store directly
                                if (g_rankMemoryStore)
                                {
                                    if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES))
                                    {
                                        // List files from rank 0
                                        auto files = g_rankMemoryStore->ListFiles();

                                        std::stringstream jsonResponse;
                                        jsonResponse << "{\"rank\":" << 0 << ",\"files\":[";
                                        bool first = true;
                                        for (const auto& filename : files)
                                        {
                                            if (filename.find(".usda.usda") != std::string::npos) continue;
                                            
                                            const auto& entry = g_rankMemoryStore->GetFile(filename);
                                            uint64_t hLo = entry->hash128[0]; uint64_t hHi = entry->hash128[1];
                                            if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
                                            if (!first) jsonResponse << ",";
                            jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << entry->size() << ",\"mime\":\"" << entry->mime_type << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";

                                            first = false;
                                        }
                                        jsonResponse << "]}";

                                        std::string jsonStr = jsonResponse.str();

                                        // Send as file chunk
                                        ZmqFileChunk response;
                                        memset(&response, 0, sizeof(response));
                                        response.magic = USD_FILE_MAGIC;
                                        response.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
                                        response.request_id = fileReq->request_id;
                                        response.source_rank = 0;
                                        strncpy(response.filename, "__file_list__.json", sizeof(response.filename) - 1);
                                        response.file_size = jsonStr.size();
                                        response.chunk_offset = 0;
                                        response.chunk_size = jsonStr.size();

                                        // Allocate combined message
                                        size_t totalSize = sizeof(response) + jsonStr.size();
                                        std::vector<uint8_t> msgData(totalSize);
                                        memcpy(msgData.data(), &response, sizeof(response));
                                        memcpy(msgData.data() + sizeof(response), jsonStr.data(), jsonStr.size());

                                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);

                                        // Send completion
                                        ZmqFileComplete complete;
                                        memset(&complete, 0, sizeof(complete));
                                        complete.magic = USD_FILE_MAGIC;
                                        complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
                                        complete.request_id = fileReq->request_id;
                                        complete.source_rank = 0;
                                        strncpy(complete.filename, "__file_list__.json", sizeof(complete.filename) - 1);
                                        complete.total_size = jsonStr.size();

                                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);

                                        std::cout << "Rank 0 (MPI Broker): Sent rank 0 file list (" << files.size() << " files)" << std::endl;
                                    }
                                    else if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE))
                                    {
                                        // Get specific file from rank 0
                                        const auto& fileEntry = g_rankMemoryStore->GetFile(fileReq->filename);

                                        if (fileEntry)
                                        {
                                            // Send file in chunks
                                            size_t chunkSize = fileReq->chunk_size > 0 ? fileReq->chunk_size : DEFAULT_CHUNK_SIZE;
                                            size_t offset = 0;

                                            while (offset < fileEntry->data.size())
                                            {
                                                size_t sendSize = std::min(chunkSize, fileEntry->data.size() - offset);

                                                ZmqFileChunk response;
                                                memset(&response, 0, sizeof(response));
                                                response.magic = USD_FILE_MAGIC;
                                                response.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
                                                response.request_id = fileReq->request_id;
                                                response.source_rank = 0;
                                                strncpy(response.filename, fileReq->filename, sizeof(response.filename) - 1);
                                                response.file_size = fileEntry->data.size();
                                                response.chunk_offset = offset;
                                                response.chunk_size = sendSize;

                                                size_t totalSize = sizeof(response) + sendSize;
                                                std::vector<uint8_t> msgData(totalSize);
                                                memcpy(msgData.data(), &response, sizeof(response));
                                                memcpy(msgData.data() + sizeof(response), fileEntry->data.data() + offset, sendSize);

                                                client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                                client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                                client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);

                                                offset += sendSize;
                                            }

                                            // Send completion
                                            ZmqFileComplete complete;
                                            memset(&complete, 0, sizeof(complete));
                                            complete.magic = USD_FILE_MAGIC;
                                            complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
                                            complete.request_id = fileReq->request_id;
                                            complete.source_rank = 0;
                                            strncpy(complete.filename, fileReq->filename, sizeof(complete.filename) - 1);
                                            complete.total_size = fileEntry->data.size();

                                            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                            client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);

                                            std::cout << "Rank 0 (MPI Broker): Sent rank 0 file: " << fileReq->filename
                                                      << " (" << fileEntry->data.size() << " bytes)" << std::endl;
                                        }
                                        else
                                        {
                                            // File not found
                                            ZmqFileComplete nofile;
                                            memset(&nofile, 0, sizeof(nofile));
                                            nofile.magic = USD_FILE_MAGIC;
                                            nofile.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE);
                                            nofile.request_id = fileReq->request_id;
                                            nofile.source_rank = 0;
                                            strncpy(nofile.filename, fileReq->filename, sizeof(nofile.filename) - 1);

                                            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                            client_router_->send(zmq::message_t(&nofile, sizeof(nofile)), zmq::send_flags::none);

                                            std::cout << "Rank 0 (MPI Broker): Rank 0 file not found: " << fileReq->filename << std::endl;
                                        }
                                    }
                                }
                                else
                                {
                                    std::cerr << "Rank 0 (MPI Broker): ERROR - Rank 0 memory store not available!" << std::endl;
                                }

                                client_map_.erase(request_key);
                                continue;
                            }

                            // For ranks 1-15, forward to workers as normal
                            bool found = false;
                            for (const auto& worker : workers_)
                            {
                                if (worker.rank == fileReq->target_rank)
                                {
                                    std::cout << "Rank 0 (MPI Broker): Routing file request to rank " << worker.rank << std::endl;

                                    // Forward request to worker via ROUTER socket
                                    router_->send(zmq::buffer(worker.identity), zmq::send_flags::sndmore);
                                    router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                    router_->send(zmq::message_t(request.data(), request.size()), zmq::send_flags::none);

                                    found = true;
                                    break;
                                }
                            }

                            if (!found)
                            {
                                // Send error response to laptop client
                                ZmqFileChunk errorResp;
                                memset(&errorResp, 0, sizeof(errorResp));
                                errorResp.magic = USD_FILE_MAGIC;
                                errorResp.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_ERROR);
                                errorResp.request_id = fileReq->request_id;
                                errorResp.source_rank = -1;
                                strncpy(errorResp.filename, "ERROR: Worker rank not found", sizeof(errorResp.filename) - 1);

                                client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                client_router_->send(zmq::message_t(&errorResp, sizeof(errorResp)), zmq::send_flags::none);

                                std::cout << "Rank 0 (MPI Broker): ERROR - Worker rank " << fileReq->target_rank << " not found" << std::endl;

                                client_map_.erase(request_key);
                            }
                        }

                        continue;
                    }
                }

                // Handle scene snapshot requests (binary format)
                if (request.size() >= sizeof(ZmqFileRequest) - 256) {  // Minimum size check
                    ZmqFileRequest* fileReq = reinterpret_cast<ZmqFileRequest*>(request.data());

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_SCENE_SNAPSHOT)) {

                        std::cout << "[Rank 0 MPI Broker] Scene snapshot request from laptop: request_id="
                                  << fileReq->request_id << std::endl;

                        // Build scene snapshot response as JSON string
                        std::stringstream snapshotJson;
                        snapshotJson << "{";
                        snapshotJson << "\"request_id\":" << fileReq->request_id << ",";
                        snapshotJson << "\"type\":\"scene_snapshot\",";
                        snapshotJson << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count() << ",";
                        snapshotJson << "\"num_workers\":" << workers_.size() << ",";
                        
                        // List connected workers
                        snapshotJson << "\"workers\":[";
                        for (size_t i = 0; i < workers_.size(); ++i) {
                            if (i > 0) snapshotJson << ",";
                            snapshotJson << "{\"rank\":" << workers_[i].rank
                                        << ",\"hostname\":\"" << workers_[i].hostname
                                        << "\",\"ready\":" << (workers_[i].ready ? "true" : "false") << "}";
                        }
                        snapshotJson << "],"
                            
                            // Memory store files (rank 0)
                            << "\"files\":[";
                        if (g_rankMemoryStore) {
                            auto files = g_rankMemoryStore->ListFiles();
                            bool firstFile = true;
                            for (const auto& f : files) {
                                auto* fe = g_rankMemoryStore->GetFile(f);
                                if (!fe) continue;
                                if (!firstFile) snapshotJson << ",";
                                firstFile = false;
                                snapshotJson << "{\"name\":\"" << f
                                            << "\",\"size\":" << fe->size()
                                            << ",\"hash_lo\":" << fe->hash128[0]
                                            << ",\"hash_hi\":" << fe->hash128[1]
                                            << ",\"mime\":\"" << fe->mime_type << "\"}";
                            }
                        }
                        snapshotJson << "]}";

                        std::string reply = snapshotJson.str();
                        std::cout << "[Rank 0 MPI Broker] Sending scene snapshot (" << reply.size() << " bytes)" << std::endl;

                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                        // Send as RESP_SCENE_SNAPSHOT
                        uint32_t respType = static_cast<uint32_t>(ZmqMessageType::RESP_SCENE_SNAPSHOT);
                        client_router_->send(zmq::message_t(&respType, sizeof(respType)), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(reply.data(), reply.size()), zmq::send_flags::none);

                        continue;
                    }
                }

                // Handle property query requests (binary format)
                if (request.size() >= sizeof(ZmqFileRequest) - 256) {  // Minimum size check
                    ZmqFileRequest* fileReq = reinterpret_cast<ZmqFileRequest*>(request.data());

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_PROPERTY)) {

                        std::cout << "[Rank 0 MPI Broker] Property request from laptop: request_id="
                                  << fileReq->request_id << std::endl;

                        // Parse property name from filename field (correct location)
                        std::string propertyName(fileReq->filename);

                        // Get property value
                        ZmqPropertyResponse response;
                        memset(&response, 0, sizeof(response));
                        response.magic = USD_FILE_MAGIC;
                        response.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_PROPERTY);
                        response.request_id = fileReq->request_id;

                        int32_t intValue = 0;
                        if (GetPropertyAsInt32(propertyName, intValue)) {
                            response.property_type = 0;  // int32
                            response.int_value = intValue;
                            std::cout << "[Rank 0 MPI Broker] Property " << propertyName
                                      << " = " << intValue << std::endl;
                        } else {
                            // Try as string property
                            std::string strValue;
                            if (GetPropertyAsString(propertyName, strValue)) {
                                response.property_type = 1;  // string
                                strncpy(response.string_value, strValue.c_str(),
                                        sizeof(response.string_value) - 1);
                                std::cout << "[Rank 0 MPI Broker] Property " << propertyName
                                          << " = \"" << strValue << "\"" << std::endl;
                            } else {
                                // Property not found
                                response.property_type = -1;  // error
                                std::cerr << "[Rank 0 MPI Broker] Unknown property: "
                                          << propertyName << std::endl;
                            }
                        }

                        // Send response
                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(&response, sizeof(response)),
                                            zmq::send_flags::none);

                        continue;
                    }
                }

                // Handle simple string requests (legacy worker list query)
                std::string message(static_cast<const char*>(request.data()), request.size());
                std::cout << "[Rank 0 MPI Broker] Laptop requested: " << message << std::endl;

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
                std::cout << "[Rank 0 MPI Broker] Sending worker list to laptop: " << reply << std::endl;

                client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                client_router_->send(zmq::message_t(reply.data(), reply.size()), zmq::send_flags::none);
            }

            // ========== Handle Worker Messages (ROUTER socket) ==========
            if (items[0].revents & ZMQ_POLLIN) {
                std::string workerId;
                std::vector<uint8_t> data;

                if (ReceiveFromWorker(workerId, data)) {
                    // Check if this is a notification message (NOTIFY_FILE_UPDATE or NOTIFY_COMMIT_COMPLETE)
                    if (data.size() >= sizeof(ZmqFileNotification)) {
                        ZmqFileNotification* notification = reinterpret_cast<ZmqFileNotification*>(data.data());

                        if (notification->magic == USD_FILE_MAGIC &&
                             (notification->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE) ||
                              notification->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE_V2) ||
                              notification->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_COMMIT_COMPLETE))) {

                             std::string notif_type = (notification->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE))
                                 ? "FILE_UPDATE" : (notification->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE_V2))
                                 ? "FILE_UPDATE_V2" : "COMMIT_COMPLETE";

                            std::cout << "[Rank 0 MPI Broker] Received " << notif_type
                                      << " notification from rank " << notification->source_rank
                                      << ": " << notification->filename << std::endl;

                             // Forward notification to all connected laptop clients
                             ForwardNotificationToClient(*notification);

                             // DIFF-CAPTURE-STATUS-HOOK: track for status table
                             GetDiffCaptureStatus().OnNotification(notification->source_rank, notification->filename, notification->timestamp);

                             continue;
                        }
                    }

                    // Check if this is a file response (file chunk, completion, or error)
                    // Use smaller struct size since ZmqFileComplete (280) < ZmqFileChunk (292)
                    if (data.size() >= sizeof(ZmqFileComplete)) {
                        ZmqFileChunk* chunk = reinterpret_cast<ZmqFileChunk*>(data.data());

                        if (chunk->magic == USD_FILE_MAGIC &&
                            (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK) ||
                             chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE) ||
                             chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE) ||
                             chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_ERROR))) {

                            // Find client identity using request_id
                            std::string request_key = std::to_string(chunk->request_id);
                            auto client_it = client_map_.find(request_key);

                            if (client_it == client_map_.end()) {
                                std::cerr << "[Rank 0 MPI Broker] WARNING: No client found for request "
                                          << chunk->request_id << std::endl;
                                continue;
                            }

                            std::string client_id = client_it->second;

                            // Log what we're forwarding
                            if (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
                                std::cout << "[Rank 0 MPI Broker] Forwarding file chunk from rank "
                                          << chunk->source_rank << " to client: " << chunk->filename
                                          << " (" << chunk->chunk_size << " bytes at offset "
                                          << chunk->chunk_offset << "/" << chunk->file_size << ")" << std::endl;
                            } else if (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE)) {
                                ZmqFileComplete* complete = reinterpret_cast<ZmqFileComplete*>(data.data());
                                std::cout << "[Rank 0 MPI Broker] Forwarding file completion from rank "
                                          << complete->source_rank << " to client: " << complete->filename
                                          << " (" << complete->total_size << " bytes total)" << std::endl;

                                // For file list responses, track broadcast completion
                                if (strcmp(complete->filename, "__file_list__.json") == 0) {
                                    // Log that we're keeping mapping for other workers
                                    std::cout << "[Rank 0 MPI Broker] Keeping client mapping for broadcast file list request "
                                              << complete->request_id << " (rank " << complete->source_rank << " responded)" << std::endl;
                                    // Don't erase - keep for other workers' responses
                                } else {
                                    // Normal file: clean up mapping
                                    client_map_.erase(request_key);
                                }
                            } else if (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE)) {
                                std::cout << "[Rank 0 MPI Broker] Forwarding 'file not found' from rank "
                                          << chunk->source_rank << " to client: " << chunk->filename << std::endl;
                                client_map_.erase(request_key);
                            } else {
                                std::cout << "[Rank 0 MPI Broker] Forwarding error from rank "
                                          << chunk->source_rank << " to client" << std::endl;
                                client_map_.erase(request_key);
                            }

                            // Forward to laptop client via CLIENT ROUTER
                            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                            client_router_->send(zmq::message_t(data.data(), data.size()), zmq::send_flags::none);

                            continue;
                        }
                    }

                    // Handle other worker messages (READY for late-joining workers like rank 0)
                    std::string message(data.begin(), data.end());

                    if (message.find("READY") == 0) {
                        // Parse: "READY|rank|hostname|ip"
                        std::stringstream ss(message);
                        std::string token, rank_str, hostname, ip;
                        std::getline(ss, token, '|');
                        std::getline(ss, rank_str, '|');
                        std::getline(ss, hostname, '|');
                        std::getline(ss, ip, '|');

                        int rank = std::stoi(rank_str);

                        // Check if worker already registered
                        bool already_registered = false;
                        for (const auto& w : workers_) {
                            if (w.rank == rank) {
                                already_registered = true;
                                break;
                            }
                        }

                        if (!already_registered) {
                            WorkerInfo worker;
                            worker.identity = workerId;
                            worker.rank = rank;
                            worker.hostname = hostname;
                            worker.ib_address = ip;
                            worker.ready = true;

                            workers_.push_back(worker);
                            worker_map_[worker.identity] = worker.rank;
                            GetDiffCaptureStatus().RegisterRank(worker.rank, worker.hostname);

                            std::cout << "[Rank 0 MPI Broker THREAD] Late-joining worker rank " << worker.rank
                                      << " connected from " << worker.hostname
                                      << " (" << worker.ib_address << ") - Total workers: "
                                      << workers_.size() << std::endl;

                            // Send ACK
                            router_->send(zmq::buffer(workerId), zmq::send_flags::sndmore);
                            router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                            router_->send(zmq::message_t("ACK", 3), zmq::send_flags::none);
                        } else {
                            std::cout << "[Rank 0 MPI Broker THREAD] Worker rank " << rank
                                      << " already registered, ignoring duplicate READY" << std::endl;
                        }
                    } else {
                        std::cout << "[Rank 0 MPI Broker THREAD] Received from worker: " << message << std::endl;
                    }
                }
            }
        }

        std::cout << "[Rank 0 MPI Broker THREAD] Message loop thread stopped cleanly" << std::endl;

    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker THREAD] ZMQ Error in message loop: " << e.what() << std::endl;
    }
}

void ZmqBroker::StatusPrinterThread() {
    int intervalSec = 10;

    // Allow override via env var
    const char* envInt = getenv("DIFFCAPTURE_STATUS_INTERVAL");
    if (envInt) {
        intervalSec = std::atoi(envInt);
        if (intervalSec <= 0) {
            std::cerr << "[DiffCapture STATUS] Disabled (DIFFCAPTURE_STATUS_INTERVAL=" << envInt << ")" << std::endl;
            GetDiffCaptureStatus().Disable();
            return;
        }
    }

    std::cerr << "[DiffCapture STATUS] Thread started - table refresh every " << intervalSec << "s" << std::endl;

    while (status_printer_active_) {
        for (int i = 0; i < intervalSec && status_printer_active_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!status_printer_active_) break;

        GetDiffCaptureStatus().PrintTable(client_port_, broker_ip_, intervalSec);
    }

    std::cerr << "[DiffCapture STATUS] Thread stopped" << std::endl;
}

void ZmqBroker::SSHReminderThread() {
    std::cerr << "[SSH REMINDER] Thread started - will print SSH command every 60 seconds" << std::endl;

    int reminder_count = 0;
    while (ssh_reminder_active_) {
        // Wait 60 seconds (check every second to allow fast shutdown)
        for (int i = 0; i < 60 && ssh_reminder_active_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!ssh_reminder_active_) break;

        reminder_count++;

        // Print the SSH tunnel command
        std::cerr << std::endl;
        std::cerr << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cerr << "║ SSH TUNNEL REMINDER #" << std::left << std::setw(58) << reminder_count << "║" << std::endl;
        std::cerr << "╠════════════════════════════════════════════════════════════════════════════════╣" << std::endl;
        std::cerr << "║ Run this command on your laptop to connect:                                   ║" << std::endl;
        std::cerr << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;
        
#ifdef ANARI_USD_ENABLE_MPI
        if (workers_.size() > 1) {
            // MPI mode with multiple workers
            std::cerr << "ssh -N -L " << client_port_ << ":" << broker_ip_ << ":" << client_port_ << " \\" << std::endl;
        } else {
            // MPI but single rank (no workers) or non-MPI mode
            std::cerr << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
        }
#else
        // Non-MPI mode: always use localhost
        std::cerr << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
#endif
        
        std::cerr << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
        std::cerr << " george2@jureca04.fz-juelich.de" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Then run: cd ~/Desktop/Github/ANARI-USD/laptop_client" << std::endl;
        std::cerr << "          python3 usd_stream_client.py --discover 0 1 2 3 --download-all" << std::endl;
        std::cerr << std::endl;
        std::cerr << std::flush;
    }

    std::cerr << "[SSH REMINDER] Thread stopped" << std::endl;
    std::cerr << std::flush;
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
    // ROUTER socket - send to specific client identity
    try {
        zmq::message_t identity(clientId.data(), clientId.size());
        zmq::message_t empty;
        zmq::message_t payload(data, size);

        client_router_->send(identity, zmq::send_flags::sndmore);
        client_router_->send(empty, zmq::send_flags::sndmore);
        client_router_->send(payload, zmq::send_flags::none);
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

// Legacy methods - not used in DEALER-ROUTER architecture
// These methods are kept for backward compatibility but are not called in the new async routing system
/*
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
*/

void ZmqBroker::Shutdown() {
    if (initialized_) {
        std::cout << "[Rank 0 MPI Broker] Shutting down..." << std::endl;

        // Stop SSH reminder thread
        ssh_reminder_active_ = false;
        if (ssh_reminder_thread_.joinable()) {
            ssh_reminder_thread_.join();
            std::cout << "[Rank 0 MPI Broker] SSH reminder thread stopped" << std::endl;
        }

        // Stop status printer thread
        status_printer_active_ = false;
        if (status_printer_thread_.joinable()) {
            status_printer_thread_.join();
            std::cout << "[Rank 0 MPI Broker] Status printer thread stopped" << std::endl;
        }

        // Stop message loop thread
        message_loop_active_ = false;
        if (message_loop_thread_.joinable()) {
            message_loop_thread_.join();
            std::cout << "[Rank 0 MPI Broker] Message loop thread stopped" << std::endl;
        }

        router_->close();
        client_router_->close();
        context_->close();
        initialized_ = false;
    }
}

// ============================================================================
// Property Query Methods
// ============================================================================

bool ZmqBroker::GetPropertyAsInt32(const std::string& propertyName, int32_t& value) {
    if (propertyName == "workerCount") {
        // Count only non-zero ranks (actual workers)
        int32_t count = 0;
        for (const auto& worker : workers_) {
            if (worker.rank != 0) count++;
        }
        value = count;
        std::cout << "[Rank 0 MPI Broker] Property workerCount = " << value
                  << " (non-zero workers, total MPI size would be " << (value + 1) << " including rank 0)" << std::endl;
        return true;
    } else if (propertyName == "mpiSize") {
        value = static_cast<int32_t>(workers_.size());
        std::cout << "[Rank 0 MPI Broker] Property mpiSize = " << value
                  << " (total workers including rank 0)" << std::endl;
        return true;
    } else if (propertyName == "mpiRank") {
        // Broker is always rank 0
        value = 0;
        return true;
    } else if (propertyName == "totalWorkerCount") {
        // Return total workers including rank 0 (for Unreal's request)
        value = static_cast<int32_t>(workers_.size());
        std::cout << "[Rank 0 MPI Broker] Property totalWorkerCount = " << value
                  << " (including rank 0)" << std::endl;
        return true;
    }
    return false;
}

bool ZmqBroker::GetPropertyAsString(const std::string& propertyName, std::string& value) {
    if (propertyName == "workerList") {
        std::stringstream ss;
        for (size_t i = 0; i < workers_.size(); ++i) {
            if (i > 0) ss << ";";
            ss << workers_[i].rank << ":"
               << workers_[i].hostname << ":"
               << workers_[i].ib_address;
        }
        value = ss.str();
        return true;
    }
    return false;
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
#ifdef ANARI_USD_ENABLE_MPI
    // MPI-based discovery: broadcast already happened during MPI initialization
    // Just receive the address via MPI_Bcast
    std::string addr = GetBrokerAddress(rank_, 5555);
    std::cout << "[Worker Rank " << rank_ << "] Resolved rank 0 address via MPI: "
              << addr << std::endl;
    return addr;
#else
    // Non-MPI mode: always connect to localhost
    std::string addr = "127.0.0.1:5555";
    std::cout << "[Non-MPI Worker] Connecting to localhost: " << addr << std::endl;
    return addr;
#endif
}

bool ZmqWorker::Connect() {
    if (connected_) return true;

    std::lock_guard<std::mutex> lock(socket_mutex_);

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
        
#ifdef ANARI_USD_ENABLE_MPI
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        std::string hostname_str = hostname;
#else
        std::string hostname_str = "localhost";
#endif

        std::stringstream ready_msg;
        ready_msg << "READY|" << rank_ << "|" << hostname_str << "|" << local_ip;

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
    std::lock_guard<std::mutex> lock(socket_mutex_);

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
    std::lock_guard<std::mutex> lock(socket_mutex_);

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
    std::lock_guard<std::mutex> lock(socket_mutex_);

    if (connected_) {
        std::cout << "[Worker Rank " << rank_ << "] Disconnecting..." << std::endl;
        dealer_->close();
        context_->close();
        connected_ = false;
    }
}

// ============================================================================
// File Request Handling Methods
// ============================================================================

bool ZmqWorker::CheckForFileRequest(ZmqFileRequest& request, bool blocking) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        zmq::message_t empty, payload;

        auto flags = blocking ? zmq::recv_flags::none : zmq::recv_flags::dontwait;

        auto result = dealer_->recv(empty, flags);
        if (!result) {
            return false;  // No message available
        }

        // DEBUG: Message received
        std::cout << "[DEBUG Worker Rank " << rank_ << "] Received message on DEALER socket" << std::endl;

        result = dealer_->recv(payload, zmq::recv_flags::none);
        if (!result || payload.size() < sizeof(ZmqFileRequest)) {
            std::cerr << "[Worker Rank " << rank_ << "] Invalid payload size: " << payload.size()
                      << " (expected at least " << sizeof(ZmqFileRequest) << ")" << std::endl;
            return false;
        }

        memcpy(&request, payload.data(), sizeof(ZmqFileRequest));

        // Validate message
        if (request.magic != USD_FILE_MAGIC) {
            std::cerr << "[Worker Rank " << rank_ << "] Invalid file request magic: 0x"
                      << std::hex << request.magic << " (expected 0x" << USD_FILE_MAGIC << ")"
                      << std::dec << std::endl;
            return false;
        }

        // DEBUG: Valid request received
        std::cout << "[DEBUG Worker Rank " << rank_ << "] Valid file request: "
                  << request.filename << " (req_id=" << request.request_id << ")" << std::endl;

        return true;
    } catch (const zmq::error_t& e) {
        if (e.num() != EAGAIN) {
            std::cerr << "[Worker Rank " << rank_ << "] CheckForFileRequest error: " << e.what() << std::endl;
        }
        return false;
    }
}

bool ZmqWorker::SendFileChunk(uint32_t requestId, const std::string& filename,
                               const void* data, size_t dataSize,
                               uint64_t totalSize, uint64_t offset) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        // Prepare header
        ZmqFileChunk header;
        header.magic = USD_FILE_MAGIC;
        header.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
        header.request_id = requestId;
        header.source_rank = rank_;
        strncpy(header.filename, filename.c_str(), sizeof(header.filename) - 1);
        header.filename[sizeof(header.filename) - 1] = '\0';
        header.file_size = totalSize;
        header.chunk_offset = offset;
        header.chunk_size = static_cast<uint32_t>(dataSize);

        // Allocate message: header + data
        size_t totalMsgSize = sizeof(header) + dataSize;
        zmq::message_t message(totalMsgSize);

        // Copy header and data
        memcpy(message.data(), &header, sizeof(header));
        memcpy(static_cast<uint8_t*>(message.data()) + sizeof(header), data, dataSize);

        // Send: empty delimiter + payload
        zmq::message_t empty;
        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(message, zmq::send_flags::none);

        std::cout << "[Worker Rank " << rank_ << "] Sent file chunk: " << filename
                  << " [" << offset << "-" << (offset + dataSize) << " / " << totalSize << "]" << std::endl;

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] SendFileChunk error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqWorker::SendFileComplete(uint32_t requestId, const std::string& filename, uint64_t totalSize) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        ZmqFileComplete msg;
        msg.magic = USD_FILE_MAGIC;
        msg.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
        msg.request_id = requestId;
        msg.source_rank = rank_;
        strncpy(msg.filename, filename.c_str(), sizeof(msg.filename) - 1);
        msg.filename[sizeof(msg.filename) - 1] = '\0';
        msg.total_size = totalSize;

        zmq::message_t empty;
        zmq::message_t payload(&msg, sizeof(msg));

        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(payload, zmq::send_flags::none);

        std::cout << "[Worker Rank " << rank_ << "] Sent file complete: " << filename
                  << " (" << (totalSize / 1024.0) << " KB)" << std::endl;

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] SendFileComplete error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqWorker::SendNoFile(uint32_t requestId, const std::string& filename) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        ZmqFileComplete msg;  // Reuse struct, set size to 0
        msg.magic = USD_FILE_MAGIC;
        msg.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE);
        msg.request_id = requestId;
        msg.source_rank = rank_;
        strncpy(msg.filename, filename.c_str(), sizeof(msg.filename) - 1);
        msg.filename[sizeof(msg.filename) - 1] = '\0';
        msg.total_size = 0;

        zmq::message_t empty;
        zmq::message_t payload(&msg, sizeof(msg));

        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(payload, zmq::send_flags::none);

        std::cout << "[Worker Rank " << rank_ << "] File not found: " << filename << std::endl;

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] SendNoFile error: " << e.what() << std::endl;
        return false;
    }
}

// ============================================================================
// Push Notification Methods
// ============================================================================

bool ZmqWorker::SendFileNotification(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128) {
    // V1: no old hash data
    return SendFileNotificationV2(filename, fileSize, timestamp, hash128, nullptr, false);
}

bool ZmqWorker::SendFileNotificationV2(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128, const uint64_t* hashPrev128, bool hasOldData) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        ZmqFileNotification msg;
        memset(&msg, 0, sizeof(msg));
        msg.magic = USD_FILE_MAGIC;
        msg.message_type = static_cast<uint32_t>(ZmqMessageType::NOTIFY_FILE_UPDATE_V2);
        msg.source_rank = rank_;
        strncpy(msg.filename, filename.c_str(), sizeof(msg.filename) - 1);
        msg.filename[sizeof(msg.filename) - 1] = '\0';
        msg.file_size = fileSize;
        msg.timestamp = timestamp;
        msg.hash128[0] = hash128 ? hash128[0] : 0;
        msg.hash128[1] = hash128 ? hash128[1] : 0;
        msg.hashPrev128[0] = (hashPrev128 && hasOldData) ? hashPrev128[0] : 0;
        msg.hashPrev128[1] = (hashPrev128 && hasOldData) ? hashPrev128[1] : 0;
        msg.hasOldData = hasOldData;

        zmq::message_t empty;
        zmq::message_t payload(&msg, sizeof(msg));

        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(payload, zmq::send_flags::none);

        if (hasOldData) {
            std::cout << "[Worker Rank " << rank_ << "] Sent file notification V2: " << filename
                      << " (has_old_data=true, " << fileSize << " bytes)" << std::endl;
        } else {
            std::cout << "[Worker Rank " << rank_ << "] Sent file notification V2: " << filename
                      << " (first_time, " << fileSize << " bytes)" << std::endl;
        }

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] SendFileNotificationV2 error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqWorker::SendCommitNotification(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        ZmqFileNotification msg;
        memset(&msg, 0, sizeof(msg));
        msg.magic = USD_FILE_MAGIC;
        msg.message_type = static_cast<uint32_t>(ZmqMessageType::NOTIFY_COMMIT_COMPLETE);
        msg.source_rank = rank_;
        strncpy(msg.filename, filename.c_str(), sizeof(msg.filename) - 1);
        msg.filename[sizeof(msg.filename) - 1] = '\0';
        msg.file_size = fileSize;
        msg.timestamp = timestamp;
        msg.hash128[0] = hash128 ? hash128[0] : 0;
        msg.hash128[1] = hash128 ? hash128[1] : 0;
        // hashPrev128 remains 0, hasOldData remains false for commit notifications

        zmq::message_t empty;
        zmq::message_t payload(&msg, sizeof(msg));

        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(payload, zmq::send_flags::none);

        std::cout << "[Worker Rank " << rank_ << "] Sent commit notification: " << filename
                  << " (" << fileSize << " bytes)" << std::endl;

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] SendCommitNotification error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqBroker::ForwardNotificationToClient(const ZmqFileNotification& notification) {
    // Forward notification to all connected laptop clients (unique identities only)
    bool success = true;

    for (const auto& client_id : client_ids_) {

        try {
            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(&notification, sizeof(notification)), zmq::send_flags::none);

            std::cout << "[Rank 0 MPI Broker] Forwarded notification to client: "
                      << notification.filename << " (rank " << notification.source_rank << ")" << std::endl;
        } catch (const zmq::error_t& e) {
            std::cerr << "[Rank 0 MPI Broker] ForwardNotificationToClient error: " << e.what() << std::endl;
            success = false;
        }
    }

    return success;
}

} // namespace usd_bridge