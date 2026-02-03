
#include "UsdBridgeZmqBroker.h"
#include "UsdBridge/UsdBridgeMemoryStore.h"

#ifdef ANARI_USD_ENABLE_MPI

#include <mpi.h>
#include <zmq.hpp>
#include <iostream>
#include <iomanip>
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

        // Store broker IP for SSH reminder thread
        broker_ip_ = ib_ip;

        // ========== BIND ROUTER SOCKET (for workers) ==========
        std::stringstream router_bind_addr;
        router_bind_addr << "tcp://" << ib_ip << ":" << worker_port_;

        std::cout << "[Rank 0 MPI Broker] InfiniBand IP detected: " << ib_ip << std::endl;
        std::cout << "[Rank 0 MPI Broker] Binding ROUTER (workers) to " << router_bind_addr.str() << std::endl;

        router_->bind(router_bind_addr.str());
        int linger = 0;
        router_->set(zmq::sockopt::linger, linger);

        // ========== BIND CLIENT ROUTER SOCKET (for laptop client - DEALER) ==========
        std::stringstream client_bind_addr;
        client_bind_addr << "tcp://" << ib_ip << ":" << client_port_;

        std::cout << "[Rank 0 MPI Broker] Binding CLIENT ROUTER (laptop DEALER) to " 
                  << client_bind_addr.str() << std::endl;

        client_router_->bind(client_bind_addr.str());
        client_router_->set(zmq::sockopt::linger, linger);

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
        std::cout << "[Rank 0 MPI Broker] Broker ready on WORKER_ROUTER:" << worker_port_
                  << " and CLIENT_ROUTER:" << client_port_ << std::endl;

        // ========== START BACKGROUND THREAD FOR MESSAGE ROUTING ==========
        message_loop_active_ = true;
        message_loop_thread_ = std::thread(&ZmqBroker::MessageLoopThread, this);
        
        // ========== START SSH REMINDER THREAD ==========
        ssh_reminder_active_ = true;
        ssh_reminder_thread_ = std::thread(&ZmqBroker::SSHReminderThread, this);
        
        std::cout << "[Rank 0 MPI Broker] Started background message routing thread" << std::endl;
        std::cout << "[Rank 0 MPI Broker] SSH tunnel reminder will print every 60 seconds" << std::endl;
        std::cout << "[Rank 0 MPI Broker] Rank 0 can now proceed to render geometry!" << std::endl;

        return true;

    } catch (const zmq::error_t& e) {
        std::cerr << "[Rank 0 MPI Broker] ZMQ Error: " << e.what() << std::endl;
        return false;
    }
}

void ZmqBroker::MessageLoopThread() {
    std::cout << "[Rank 0 MPI Broker THREAD] Message loop thread started" << std::endl;
    
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

                // Check if this is a file request (structured binary message)
                if (request.size() >= sizeof(ZmqFileRequest)) {
                    ZmqFileRequest* fileReq = static_cast<ZmqFileRequest*>(request.data());
                    
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
                        
                        // Route request to appropriate worker
                        if (fileReq->target_rank >= 0) {
                            bool found = false;
                            for (const auto& worker : workers_) {
                                if (worker.rank == fileReq->target_rank) {
                                    std::cout << "[Rank 0 MPI Broker] Routing file request to rank "
                                              << worker.rank << std::endl;
                                    
                                    // Forward request to worker via ROUTER socket
                                    router_->send(zmq::buffer(worker.identity), zmq::send_flags::sndmore);
                                    router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                    router_->send(zmq::message_t(request.data(), request.size()), 
                                                zmq::send_flags::none);
                                    found = true;
                                    break;
                                }
                            }
                            
                            if (!found) {
                                // Send error response to laptop client
                                ZmqFileChunk errorResp;
                                memset(&errorResp, 0, sizeof(errorResp));
                                errorResp.magic = USD_FILE_MAGIC;
                                errorResp.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_ERROR);
                                errorResp.request_id = fileReq->request_id;
                                errorResp.source_rank = -1;
                                strncpy(errorResp.filename, "ERROR: Worker rank not found", 
                                       sizeof(errorResp.filename) - 1);
                                
                                client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                client_router_->send(zmq::message_t(&errorResp, sizeof(errorResp)), 
                                                    zmq::send_flags::none);
                                
                                std::cout << "[Rank 0 MPI Broker] ERROR: Worker rank " 
                                          << fileReq->target_rank << " not found" << std::endl;
                                
                                // Clean up client mapping
                                client_map_.erase(request_key);
                            }
                        } else {
                            // Broadcast not supported for file requests
                            ZmqFileChunk errorResp;
                            memset(&errorResp, 0, sizeof(errorResp));
                            errorResp.magic = USD_FILE_MAGIC;
                            errorResp.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_ERROR);
                            errorResp.request_id = fileReq->request_id;
                            errorResp.source_rank = -1;
                            strncpy(errorResp.filename, "ERROR: Broadcast file request not supported", 
                                   sizeof(errorResp.filename) - 1);
                            
                            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                            client_router_->send(zmq::message_t(&errorResp, sizeof(errorResp)), 
                                                zmq::send_flags::none);
                            
                            std::cout << "[Rank 0 MPI Broker] ERROR: Broadcast file request not supported"
                                      << std::endl;
                            
                            client_map_.erase(request_key);
                        }
                        
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
                                
                                // Clean up client mapping after file completion
                                client_map_.erase(request_key);
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

void ZmqBroker::SSHReminderThread() {
    std::cout << "[Rank 0 SSH REMINDER] Thread started - will print SSH command every 60 seconds" << std::endl;
    
    int reminder_count = 0;
    while (ssh_reminder_active_) {
        // Wait 60 seconds (check every second to allow fast shutdown)
        for (int i = 0; i < 60 && ssh_reminder_active_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!ssh_reminder_active_) break;
        
        reminder_count++;
        
        // Print the SSH tunnel command
        std::cout << std::endl;
        std::cout << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║ SSH TUNNEL REMINDER #" << std::left << std::setw(58) << reminder_count << "║" << std::endl;
        std::cout << "╠════════════════════════════════════════════════════════════════════════════════╣" << std::endl;
        std::cout << "║ Run this command on your laptop to connect:                                   ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;
        std::cout << "ssh -N -L " << client_port_ << ":" << broker_ip_ << ":" << client_port_ << " \\" << std::endl;
        std::cout << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
        std::cout << " george2@jureca04.fz-juelich.de" << std::endl;
        std::cout << std::endl;
        std::cout << "Then run: cd ~/Desktop/Github/ANARI-USD/laptop_client" << std::endl;
        std::cout << "          python3 usd_stream_client.py --discover 0 1 2 3 --download-all" << std::endl;
        std::cout << std::endl;
    }
    
    std::cout << "[Rank 0 SSH REMINDER] Thread stopped" << std::endl;
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

} // namespace usd_bridge

#endif // ANARI_USD_ENABLE_MPI