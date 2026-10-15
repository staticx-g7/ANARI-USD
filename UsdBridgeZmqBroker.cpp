
#include "UsdBridgeZmqBroker.h"
#include "UsdBridge/UsdBridgeMemoryStore.h"
#include "UsdBridge/xxhash/xxhash.h"
#include "UsdBridge/DiffCaptureStatus.h"

#include <zmq.hpp>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>

#ifdef ANARI_USD_ENABLE_MPI
#include <mpi.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace usd_bridge {

static bool BrokerVerbose() {
    static const bool verbose = []() {
        const char* env = getenv("BROKER_VERBOSE");
        return env != nullptr && env[0] != '0' && std::string(env) != "false";
    }();
    return verbose;
}

#define BVERB if (BrokerVerbose()) std::cout
#define WVERB if (BrokerVerbose()) std::cout


static std::string ExtractJsonArray(const std::string& json) {
    size_t start = json.find("\"files\":[");
    if (start == std::string::npos) {
        return "";
    }
    start = json.find('[', start);
    if (start == std::string::npos) {
        return "";
    }

    int bracket_count = 1;
    size_t end = start + 1;
    for (; end < json.size() && bracket_count > 0; ++end) {
        if (json[end] == '[') bracket_count++;
        else if (json[end] == ']') bracket_count--;
    }

    if (bracket_count != 0) {
        return "";
    }

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
    return "127.0.0.1";
#endif
}


#ifdef ANARI_USD_ENABLE_MPI
static bool EnsureMpiInitialized()
{
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (initialized)
        return true;
    int finalized = 0;
    MPI_Finalized(&finalized);
    if (finalized)
        return false;
    if (MPI_Init(nullptr, nullptr) != MPI_SUCCESS)
    {
        std::cerr << "[UsdBridge] MPI_Init failed; falling back to loopback broker address"
                  << std::endl;
        return false;
    }
    return true;
}
#endif

std::string GetBrokerAddress(int rank, int port) {
#ifdef ANARI_USD_ENABLE_MPI
    if (!EnsureMpiInitialized()) {
        std::stringstream ss;
        ss << "127.0.0.1:" << port;
        return ss.str();
    }
    char broker_address[256];
    memset(broker_address, 0, sizeof(broker_address));

    if (rank == 0) {
        std::string ib_ip = GetInfiniBandIPImpl();
        const char* bind_ip_env = getenv("DIFFCAPTURE_BIND_IP");
        if (bind_ip_env && bind_ip_env[0]) {
            ib_ip = bind_ip_env;
        }
        if (ib_ip == "0.0.0.0") {
            ib_ip = "127.0.0.1"; // workers co-located (single-node); wildcard is bind-only
        }
        std::stringstream ss;
        ss << ib_ip << ":" << port;
        std::string addr = ss.str();
        strncpy(broker_address, addr.c_str(), sizeof(broker_address) - 1);

        std::cout << "[Rank 0 MPI Broker] Broadcasting InfiniBand address: "
                  << broker_address << std::endl;
    }

    MPI_Bcast(broker_address, sizeof(broker_address), MPI_CHAR, 0, MPI_COMM_WORLD);

    if (rank != 0) {
        std::cout << "[Worker Rank " << rank << "] Received broker address via MPI: "
                  << broker_address << std::endl;
    }

    return std::string(broker_address);
#else
    std::stringstream ss;
    ss << "127.0.0.1:" << port;
    std::string addr = ss.str();

    std::cout << "[Non-MPI ZMQ Broker] Using localhost address: " << addr << std::endl;
    return addr;
#endif
}


ZmqBroker::ZmqBroker(int workerPort, int clientPort)
    : worker_port_(workerPort)
    , client_port_(clientPort)
    , initialized_(false)
{
    unsigned hw = std::thread::hardware_concurrency();
    int ioThreads = static_cast<int>(std::min<size_t>(std::max<size_t>(hw, 8) / 4, 8));
    context_ = std::make_unique<zmq::context_t>(ioThreads);
    router_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::router);
    client_router_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::router);
    client_router_->set(zmq::sockopt::sndhwm, 1000);
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
#ifdef ANARI_USD_ENABLE_MPI
        int rank = 0;
        {
          int initialized = 0;
          MPI_Initialized(&initialized);
          if (initialized)
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
          else
          {
            const char* e_slurm = getenv("SLURM_PROCID");
            const char* e_ompi = getenv("OMPI_COMM_WORLD_RANK");
            const char* e_pmi = getenv("PMI_RANK");
            if (e_slurm)
              rank = std::atoi(e_slurm);
            else if (e_ompi)
              rank = std::atoi(e_ompi);
            else if (e_pmi)
              rank = std::atoi(e_pmi);
          }
        }

        const char* slurm_procid = getenv("SLURM_PROCID");
        const char* ompi_rank = getenv("OMPI_COMM_WORLD_RANK");
        const char* pmi_rank = getenv("PMI_RANK");
        std::cerr << "[Rank " << rank << "/MPI_COMM_WORLD] ZmqBroker::Initialize() called"
                  << " | SLURM_PROCID=" << (slurm_procid ? slurm_procid : "?")
                  << " | OMPI_RANK=" << (ompi_rank ? ompi_rank : "?")
                  << " | PMI_RANK=" << (pmi_rank ? pmi_rank : "?")
                  << " | expectedWorkers=" << expectedWorkers
                  << std::endl << std::flush;

        if (rank != 0) {
            std::cerr << "[ZmqBroker] ERROR: Initialize() called on non-zero rank!" << std::endl;
            return false;
        }

        bool isSingleRankMode = (expectedWorkers == 0);
        std::string ib_ip;  // Declare here so it's available in both branches
        std::string bind_ip;  // interface the ROUTER sockets actually bind

        if (isSingleRankMode) {
            const char* bind_ip_env = getenv("DIFFCAPTURE_BIND_IP");
            ib_ip = (bind_ip_env && bind_ip_env[0]) ? bind_ip_env : "0.0.0.0";
            bind_ip = ib_ip;
            broker_ip_ = ib_ip;
            std::cout << "[Rank 0 MPI Broker] Single-rank mode detected - using bind IP: " << ib_ip << std::endl;
        } else {
            std::string broadcast_addr = GetBrokerAddress(rank, worker_port_);

            const char* bind_ip_env = getenv("DIFFCAPTURE_BIND_IP");
            bind_ip = (bind_ip_env && bind_ip_env[0]) ? bind_ip_env : "0.0.0.0";

            size_t colon_pos = broadcast_addr.find(':');
            ib_ip = (colon_pos != std::string::npos)
                ? broadcast_addr.substr(0, colon_pos)
                : broadcast_addr;

            broker_ip_ = ib_ip;

            std::cout << "[Rank 0 MPI Broker] InfiniBand IP detected: " << ib_ip
                      << " (binding clients on " << bind_ip << ":" << client_port_
                      << ")" << std::endl;
        }
#else
        std::string ib_ip = "127.0.0.1";
        std::string bind_ip = ib_ip;
        broker_ip_ = ib_ip;

        std::cout << "[Non-MPI ZMQ Broker] Using localhost IP: " << ib_ip << std::endl;
#endif

#ifndef ANARI_USD_ENABLE_MPI
        bool isSingleRankMode = true;
#else
#endif

        int linger = 0;

        if (!isSingleRankMode) {
            std::stringstream router_bind_addr;
            router_bind_addr << "tcp://" << bind_ip << ":" << worker_port_;

            std::cout << "[ZMQ Broker] Binding ROUTER (workers) to " << router_bind_addr.str() << std::endl;

            router_->bind(router_bind_addr.str());
            router_->set(zmq::sockopt::linger, linger);
        } else {
            std::cout << "[ZMQ Broker] Single-rank mode - skipping worker socket binding (no workers)" << std::endl;
        }

        std::stringstream client_bind_addr;
        client_bind_addr << "tcp://" << bind_ip << ":" << client_port_;

        std::cout << "[ZMQ Broker] Binding CLIENT ROUTER (laptop DEALER) to "
                  << client_bind_addr.str() << std::endl;

        client_router_->bind(client_bind_addr.str());
        client_router_->set(zmq::sockopt::linger, linger);

        {
            std::stringstream ssh_info;
            ssh_info << std::endl;
            ssh_info << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
            ssh_info << "║ ZMQ BROKER CONNECTION INFORMATION                                             ║" << std::endl;
            ssh_info << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;

#ifdef ANARI_USD_ENABLE_MPI
            if (expectedWorkers > 0) {
                ssh_info << "SSH TUNNEL COMMAND FOR REMOTE ACCESS:" << std::endl;
                ssh_info << "ssh -N -L " << client_port_ << ":" << ib_ip << ":" << client_port_ << " \\" << std::endl;
                ssh_info << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
                ssh_info << " george2@jureca04.fz-juelich.de" << std::endl;
            } else {
                ssh_info << "REMOTE LOCALHOST BROKER (NO MPI SPLIT):" << std::endl;
                ssh_info << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
                ssh_info << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
                ssh_info << " george2@jureca04.fz-juelich.de" << std::endl;
            }
#else
            ssh_info << "REMOTE LOCALHOST BROKER (NON-MPI / MAIN GUI):" << std::endl;
            ssh_info << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
            ssh_info << " -i ~/.ssh/ed_25519_universal_openssh \\" << std::endl;
            ssh_info << " george2@jureca04.fz-juelich.de" << std::endl;

            ssh_info << "\nDIRECT LOCAL CONNECTION (if running on same machine):" << std::endl;
            ssh_info << "Connect to: localhost:" << client_port_ << std::endl;
#endif
            ssh_info << std::flush;

            std::cerr << ssh_info.str();

            const char* output_dir = getenv("ANARI_USD_SERIALIZE_LOCATION");
            if (output_dir) {
                std::string ssh_file = std::string(output_dir) + "/_ssh_connection_info.txt";
                std::ofstream ssh_fout(ssh_file);
                if (ssh_fout) {
                    ssh_fout << ssh_info.str();
                    ssh_fout.close();
                    std::cerr << "[Rank 0 MPI Broker] SSH connection info also written to: " << ssh_file << std::endl;
                }
            }
        }

#ifdef ANARI_USD_ENABLE_MPI
        std::cout << "[MPI ZMQ Broker] Waiting for " << expectedWorkers
                  << " MPI workers to connect..." << std::endl;

        auto starttime = std::chrono::steady_clock::now();
        const int timeoutseconds = 30;
        int nonZeroWorkersConnected = 0;  // Count only ranks 1-N

        while (nonZeroWorkersConnected < expectedWorkers)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - starttime).count();
            if (elapsed >= timeoutseconds)
            {
                std::cerr << "Rank 0 (MPI Broker): Timeout waiting for workers. Only "
                          << nonZeroWorkersConnected << "/" << expectedWorkers << " connected."
                          << " Continuing with partial workers. Late joiners will be accepted by message loop."
                          << std::endl;
                break;
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

                router_->send(identity, zmq::send_flags::sndmore);
                router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                router_->send(zmq::message_t("ACK", 3), zmq::send_flags::none);
            }
        }

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

        std::cout << "[MPI ZMQ Broker] " << nonZeroWorkersConnected << "/" << expectedWorkers
                  << " MPI workers connected" << (nonZeroWorkersConnected < expectedWorkers ? " (partial)" : " successfully!")
                  << std::endl;
#else
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

        message_loop_active_ = true;
        message_loop_thread_ = std::thread(&ZmqBroker::MessageLoopThread, this);

        ssh_reminder_active_ = true;
        ssh_reminder_thread_ = std::thread(&ZmqBroker::SSHReminderThread, this);

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
        while (message_loop_active_) {
            TickPendingTransfers(2);

            zmq::pollitem_t items[] = {
                { *router_, 0, ZMQ_POLLIN, 0 },        // Messages from workers (ROUTER)
                { *client_router_, 0, ZMQ_POLLIN, 0 }  // Messages from laptop client (ROUTER)
            };

            const int pollTimeoutMs = pending_transfers_.empty() ? 100 : 1;
            zmq::poll(&items[0], 2, std::chrono::milliseconds(pollTimeoutMs));

            if (items[1].revents & ZMQ_POLLIN) {
                zmq::message_t client_identity, first, request;

                (void)client_router_->recv(client_identity, zmq::recv_flags::none);
                (void)client_router_->recv(first, zmq::recv_flags::none);
                if (first.size() == 0) {
                    (void)client_router_->recv(request, zmq::recv_flags::none);
                } else {
                    request = std::move(first);
                }

                std::string client_id(static_cast<const char*>(client_identity.data()),
                                     client_identity.size());

                BVERB << "[Rank 0 MPI Broker] DEBUG: Received client request - size="
                      << request.size() << " bytes, client_id=" << client_id.substr(0, 8) << "..."
                      << ", sizeof(ZmqFileRequest)=" << sizeof(ZmqFileRequest) << std::endl;

                if (request.size() < sizeof(ZmqFileRequest)) {
                    BVERB << "[Rank 0 MPI Broker] DEBUG: Request too small for binary message ("
                          << request.size() << " bytes), treating as string" << std::endl;
                } else {
                    ZmqFileRequest* fileReq = static_cast<ZmqFileRequest*>(request.data());

                    BVERB << "[Rank 0 MPI Broker] DEBUG: Request magic=0x"
                          << std::hex << fileReq->magic << std::dec
                          << ", type=" << fileReq->message_type
                          << ", target_rank=" << fileReq->target_rank
                          << ", expected_magic=0x" << std::hex << USD_FILE_MAGIC << std::dec << std::endl;

                    if (fileReq->magic != USD_FILE_MAGIC) {
                        std::cerr << "[Rank 0 MPI Broker] WARN: request magic MISMATCH! Got 0x"
                                  << std::hex << fileReq->magic << ", expected 0x"
                                  << USD_FILE_MAGIC << std::dec << std::endl;
                    }

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES) &&
                        workers_.size() == 1) {  // Non-MPI mode: only rank 0 exists
                        std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: Handling file list request directly" << std::endl;

                        client_map_[fileReq->request_id] = client_id;
                        client_ids_.insert(client_id);

                        if (!g_rankMemoryStore) {
                            std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: ERROR - g_rankMemoryStore is NULL!" << std::endl;
                            continue;
                        }
                        auto files = g_rankMemoryStore->ListFiles();
                        std::cout << "[Rank 0 MPI Broker] NON-MPI MODE: Found " << files.size() << " files in rank 0 memory store" << std::endl;

                        std::stringstream jsonResponse;
                        jsonResponse << "{\"rank\":" << 0 << ",\"files\":[";
                        bool first = true;
                        for (const auto& filename : files) {
                            if (filename.find(".usda.usda") != std::string::npos) continue;
                             auto entry = g_rankMemoryStore->GetFile(filename);
                             if (!entry) continue;
                             uint64_t hLo = entry->hash128[0];
                             uint64_t hHi = entry->hash128[1];
                             if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
                             if (!first) jsonResponse << ",";
                             jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << entry->size() << ",\"mime\":\"" << entry->mime_type << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";
                             first = false;
                        }
                        jsonResponse << "]}";

                        std::string jsonStr = jsonResponse.str();
                        BVERB << "[Rank 0 MPI Broker] NON-MPI MODE: Built JSON response: " << jsonStr.substr(0, 100) << "..." << std::endl;

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

                        size_t totalSize = sizeof(response) + jsonStr.size();
                        std::vector<uint8_t> msgData(totalSize);
                        memcpy(msgData.data(), &response, sizeof(response));
                        memcpy(msgData.data() + sizeof(response), jsonStr.data(), jsonStr.size());

                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);

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

                        client_map_.erase(fileReq->request_id);
                        continue;  // Done handling this request
                    }

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE) ||
                         fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES) ||
                         fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME))) {

                        BVERB << "[Rank 0 MPI Broker] ";
                        if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES)) {
                            BVERB << "File list request from laptop client "
                                  << client_id.substr(0, 8) << "...: "
                                  << "rank " << fileReq->target_rank << std::endl;
                        } else if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME)) {
                            BVERB << "Frame request from laptop client "
                                  << client_id.substr(0, 8) << "...: "
                                  << fileReq->filename << " (rank " << fileReq->target_rank << ")"
                                  << std::endl;
                        } else {
                            BVERB << "File request from laptop client "
                                  << client_id.substr(0, 8) << "...: "
                                  << fileReq->filename << " (rank " << fileReq->target_rank << ")"
                                  << std::endl;
                        }

                        client_map_[fileReq->request_id] = client_id;
                        client_ids_.insert(client_id);

                        {
                            auto now = std::chrono::steady_clock::now();
                            for (auto it = broadcast_state_.begin(); it != broadcast_state_.end();) {
                                if (now - it->second.created > std::chrono::seconds(60)) {
                                    client_map_.erase(it->first);
                                    it = broadcast_state_.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                            for (auto it = frame_requests_.begin(); it != frame_requests_.end();) {
                                if (now - it->second > std::chrono::seconds(60)) {
                                    client_map_.erase(it->first);
                                    it = frame_requests_.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                        }

                        if (fileReq->target_rank == -1)
                        {
                            const bool isList = fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES);
                            const bool isFrame = fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME);

                            if (isFrame) {
                                ZmqFileChunk errorResp;
                                memset(&errorResp, 0, sizeof(errorResp));
                                errorResp.magic = USD_FILE_MAGIC;
                                errorResp.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_ERROR);
                                errorResp.request_id = fileReq->request_id;
                                errorResp.source_rank = -1;
                                strncpy(errorResp.filename, "ERROR: broadcast frame requests not supported", sizeof(errorResp.filename) - 1);

                                client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                client_router_->send(zmq::message_t(&errorResp, sizeof(errorResp)), zmq::send_flags::none);

                                client_map_.erase(fileReq->request_id);
                                continue;
                            }

                            BVERB << "Rank 0 (MPI Broker): Broadcasting "
                                  << (isList ? "file list" : "file")
                                  << " request to ALL workers (rank -1) - Processing rank 0 locally" << std::endl;

                            int broadcastCopies = 0;
                            for (const auto& worker : workers_)
                            {
                                if (worker.rank == 0) continue; // Skip rank 0 (handled locally)
                                router_->send(zmq::buffer(worker.identity), zmq::send_flags::sndmore);
                                router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                router_->send(zmq::message_t(request.data(), request.size()), zmq::send_flags::none);
                                ++broadcastCopies;
                            }
                            if (isList && broadcastCopies > 0) {
                                broadcast_state_[fileReq->request_id] =
                                    BroadcastState{broadcastCopies, std::chrono::steady_clock::now()};
                            }

                             if (isList && !g_rankMemoryStore) {
                                 std::cerr << "[Rank 0 MPI Broker] ERROR - Rank 0 memory store not available for broadcast list!" << std::endl;
                                 client_map_.erase(fileReq->request_id);
                                 continue;
                             }
                             if (isList) {
                                std::cout << "[Rank 0 MPI Broker] Processing rank 0 file list for broadcast request ID " << fileReq->request_id << std::endl;

                                auto files = g_rankMemoryStore->ListFiles();

                                std::stringstream jsonResponse;
                                jsonResponse << "{\"rank\":" << 0 << ",\"files\":[";
                                bool first = true;
                                 for (const auto& filename : files) {
                                     if (filename.find(".usda.usda") != std::string::npos) continue;

                                     auto entry = g_rankMemoryStore->GetFile(filename);
                                     if (!entry) continue;
                                     uint64_t hLo = entry->hash128[0]; uint64_t hHi = entry->hash128[1];
                                     if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
                                     if (!first) jsonResponse << ",";
                                     jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << entry->size() << ",\"mime\":\"" << entry->mime_type << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";
                                     first = false;
                                 }
                                 jsonResponse << "]}";

                                std::string jsonStr = jsonResponse.str();

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

                                        size_t totalSize = sizeof(response) + jsonStr.size();
                                        std::vector<uint8_t> msgData(totalSize);
                                        memcpy(msgData.data(), &response, sizeof(response));
                                        memcpy(msgData.data() + sizeof(response), jsonStr.data(), jsonStr.size());

                                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);

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

                                         if (broadcast_state_.find(fileReq->request_id) == broadcast_state_.end()) {
                                             client_map_.erase(fileReq->request_id);
                                         }
                             }
                             continue; // Done with broadcast
                        }

                        if (fileReq->target_rank >= 0)
                        {
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

                                if (g_rankMemoryStore)
                                {
                                    if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_LIST_FILES))
                                    {
                                        auto files = g_rankMemoryStore->ListFiles();

                                        std::stringstream jsonResponse;
                                        jsonResponse << "{\"rank\":" << 0 << ",\"files\":[";
                                        bool first = true;
                                         for (const auto& filename : files)
                                         {
                                             if (filename.find(".usda.usda") != std::string::npos) continue;

                                             auto entry = g_rankMemoryStore->GetFile(filename);
                                             if (!entry) continue;
                                             uint64_t hLo = entry->hash128[0]; uint64_t hHi = entry->hash128[1];
                                             if (hLo == 0 && hHi == 0) { XXH128_hash_t h = XXH3_128bits(entry->data.data(), entry->data.size()); hLo = h.low64; hHi = h.high64; }
                                             if (!first) jsonResponse << ",";
                             jsonResponse << "{\"name\":\"" << filename << "\",\"size\":" << entry->size() << ",\"mime\":\"" << entry->mime_type << "\",\"hash_lo\":" << hLo << ",\"hash_hi\":" << hHi << "}";

                                             first = false;
                                         }
                                         jsonResponse << "]}";

                                        std::string jsonStr = jsonResponse.str();

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

                                        size_t totalSize = sizeof(response) + jsonStr.size();
                                        std::vector<uint8_t> msgData(totalSize);
                                        memcpy(msgData.data(), &response, sizeof(response));
                                        memcpy(msgData.data() + sizeof(response), jsonStr.data(), jsonStr.size());

                                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                        client_router_->send(zmq::message_t(msgData.data(), msgData.size()), zmq::send_flags::none);

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

                                         client_map_.erase(fileReq->request_id);
                                     }
                                     else if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE))
                                     {
                                         auto fileEntry = g_rankMemoryStore->GetFile(fileReq->filename);

                                         if (fileEntry)
                                         {
                                             QueueRank0FileTransfer(client_id, fileReq->request_id,
                                                                    fileReq->filename, fileReq->chunk_size);
                                         }
                                         else
                                         {
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

                                             client_map_.erase(fileReq->request_id);
                                         }
                                     }
                                     else if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME))
                                     {
                                         // (reuses an existing message type; no protocol change).

                                         const size_t kMaxChunkSize = 128 * 1024 * 1024;
                                         size_t chunkSize = (fileReq->chunk_size > 0)
                                             ? std::min<size_t>(fileReq->chunk_size, kMaxChunkSize)
                                             : DEFAULT_CHUNK_SIZE;

                                         PendingFileTransfer t;
                                         t.client_id = client_id;
                                         t.request_id = fileReq->request_id;
                                         t.chunk_size = chunkSize;
                                         for (const auto& filename : g_rankMemoryStore->ListFiles()) {
                                             if (filename.find(".usda.usda") != std::string::npos) continue;
                                             if (!g_rankMemoryStore->GetFile(filename)) continue;
                                             t.files_in_frame.push_back(filename);
                                         }
                                         t.marker_total = static_cast<int>(t.files_in_frame.size());
                                         pending_transfers_.push_back(std::move(t));

                                         std::cout << "Rank 0 (MPI Broker): Frame request queued: "
                                                   << t.marker_total << " files for "
                                                   << client_id.substr(0, 8) << "..." << std::endl;
                                     }
                                 }
                                 else
                                 {
                                     std::cerr << "Rank 0 (MPI Broker): ERROR - Rank 0 memory store not available!" << std::endl;

                                     if (g_rankMemoryStore == nullptr &&
                                         fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FILE)) {
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
                                     }
                                     client_map_.erase(fileReq->request_id);
                                 }

                                 continue;
                             }

                             bool found = false;
                             for (const auto& worker : workers_)
                             {
                                 if (worker.rank == fileReq->target_rank)
                                 {
                                     BVERB << "Rank 0 (MPI Broker): Routing file request to rank " << worker.rank << std::endl;

                                     router_->send(zmq::buffer(worker.identity), zmq::send_flags::sndmore);
                                     router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                                     router_->send(zmq::message_t(request.data(), request.size()), zmq::send_flags::none);

                                      if (fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_FRAME)) {
                                          frame_requests_[fileReq->request_id] = std::chrono::steady_clock::now();
                                      }

                                     found = true;
                                     break;
                                 }
                             }

                            if (!found)
                            {
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

                                client_map_.erase(fileReq->request_id);
                            }
                        }

                        continue;
                    }
                }

                if (request.size() >= sizeof(ZmqFileRequest) - 256) {  // Minimum size check
                    ZmqFileRequest* fileReq = reinterpret_cast<ZmqFileRequest*>(request.data());

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_SCENE_SNAPSHOT)) {

                        std::cout << "[Rank 0 MPI Broker] Scene snapshot request from laptop: request_id="
                                  << fileReq->request_id << std::endl;

                        std::stringstream snapshotJson;
                        snapshotJson << "{";
                        snapshotJson << "\"request_id\":" << fileReq->request_id << ",";
                        snapshotJson << "\"type\":\"scene_snapshot\",";
                        snapshotJson << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count() << ",";
                        snapshotJson << "\"num_workers\":" << workers_.size() << ",";

                        snapshotJson << "\"workers\":[";
                        for (size_t i = 0; i < workers_.size(); ++i) {
                            if (i > 0) snapshotJson << ",";
                            snapshotJson << "{\"rank\":" << workers_[i].rank
                                        << ",\"hostname\":\"" << workers_[i].hostname
                                        << "\",\"ready\":" << (workers_[i].ready ? "true" : "false") << "}";
                        }
                        snapshotJson << "],"

                            << "\"files\":[";
                        if (g_rankMemoryStore) {
                            auto files = g_rankMemoryStore->ListFiles();
                            bool firstFile = true;
                            for (const auto& f : files) {
                                auto fe = g_rankMemoryStore->GetFile(f);
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
                        uint32_t respType = static_cast<uint32_t>(ZmqMessageType::RESP_SCENE_SNAPSHOT);
                        client_router_->send(zmq::message_t(&respType, sizeof(respType)), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(reply.data(), reply.size()), zmq::send_flags::none);

                        continue;
                    }
                }

                if (request.size() >= sizeof(ZmqFileRequest) - 256) {  // Minimum size check
                    ZmqFileRequest* fileReq = reinterpret_cast<ZmqFileRequest*>(request.data());

                    if (fileReq->magic == USD_FILE_MAGIC &&
                        fileReq->message_type == static_cast<uint32_t>(ZmqMessageType::REQ_GET_PROPERTY)) {

                        client_ids_.insert(client_id);

                        std::cout << "[Rank 0 MPI Broker] Property request from laptop: request_id="
                                  << fileReq->request_id << std::endl;

                        std::string propertyName(fileReq->filename);

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
                            std::string strValue;
                            if (GetPropertyAsString(propertyName, strValue)) {
                                response.property_type = 1;  // string
                                strncpy(response.string_value, strValue.c_str(),
                                        sizeof(response.string_value) - 1);
                                std::cout << "[Rank 0 MPI Broker] Property " << propertyName
                                          << " = \"" << strValue << "\"" << std::endl;
                            } else {
                                response.property_type = -1;  // error
                                std::cerr << "[Rank 0 MPI Broker] Unknown property: "
                                          << propertyName << std::endl;
                            }
                        }

                        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                        client_router_->send(zmq::message_t(&response, sizeof(response)),
                                            zmq::send_flags::none);

                        continue;
                    }
                }

                std::string message(static_cast<const char*>(request.data()), request.size());
                std::cout << "[Rank 0 MPI Broker] Laptop requested: " << message << std::endl;

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

            if (items[0].revents & ZMQ_POLLIN) {
                std::string workerId;
                std::vector<uint8_t> data;

                if (ReceiveFromWorker(workerId, data)) {
                    if (data.size() >= sizeof(ZmqSceneUpdate)) {
                        ZmqSceneUpdate* sceneUpdate = reinterpret_cast<ZmqSceneUpdate*>(data.data());

                        if (sceneUpdate->magic == USD_FILE_MAGIC &&
                            (sceneUpdate->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_SCENE_UPDATE) ||
                             sceneUpdate->message_type == static_cast<uint32_t>(ZmqMessageType::NOTIFY_PROPERTY_UPDATE))) {

                            std::cout << "[Rank 0 MPI Broker] Received scene update from rank "
                                      << sceneUpdate->source_rank
                                      << ": prim='" << sceneUpdate->getPrimPath()
                                      << "' property='" << sceneUpdate->getPropertyName()
                                      << "' change=" << sceneUpdate->change_type << std::endl;

                            ForwardSceneUpdateToClient(*sceneUpdate);
                            continue;
                        }
                    }

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

                             ForwardNotificationToClient(*notification);

                             GetDiffCaptureStatus().OnNotification(notification->source_rank, notification->filename, notification->timestamp);

                             continue;
                        }
                    }

                    // Use smaller struct size since ZmqFileComplete (280) < ZmqFileChunk (292)

                    if (data.size() >= sizeof(ZmqFileComplete)) {
                        ZmqFileChunk* chunk = reinterpret_cast<ZmqFileChunk*>(data.data());

                        if (chunk->magic == USD_FILE_MAGIC &&
                            (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK) ||
                             chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE) ||
                             chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE) ||
                             chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_ERROR))) {

                            auto client_it = client_map_.find(chunk->request_id);

                            if (client_it == client_map_.end()) {
                                BVERB << "[Rank 0 MPI Broker] WARNING: No client found for request "
                                      << chunk->request_id << std::endl;
                                continue;
                            }

                            std::string client_id = client_it->second;

                            if (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK)) {
                                BVERB << "[Rank 0 MPI Broker] Forwarding file chunk from rank "
                                      << chunk->source_rank << " to client: " << chunk->filename
                                      << " (" << chunk->chunk_size << " bytes at offset "
                                      << chunk->chunk_offset << "/" << chunk->file_size << ")" << std::endl;
                            } else if (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE)) {
                                ZmqFileComplete* complete = reinterpret_cast<ZmqFileComplete*>(data.data());
                                BVERB << "[Rank 0 MPI Broker] Forwarding file completion from rank "
                                      << complete->source_rank << " to client: " << complete->filename
                                      << " (" << complete->total_size << " bytes total)" << std::endl;

                                const bool isListComplete = strcmp(complete->filename, "__file_list__.json") == 0;
                                const bool isFrameMarker  = strcmp(complete->filename, "__frame_complete__") == 0;
                                const bool isFrame        = frame_requests_.count(complete->request_id) > 0;

                                bool dropMapping = false;
                                if (isFrameMarker) {
                                    frame_requests_.erase(complete->request_id);
                                    dropMapping = true;
                                } else if (isListComplete) {
                                    auto bIt = broadcast_state_.find(complete->request_id);
                                    if (bIt != broadcast_state_.end()) {
                                        if (--bIt->second.pending <= 0) {
                                            dropMapping = true;
                                            broadcast_state_.erase(bIt);
                                        } else {
                                            BVERB << "[Rank 0 MPI Broker] Keeping client mapping for broadcast file list request "
                                                  << complete->request_id
                                                  << " (" << bIt->second.pending << " workers outstanding)" << std::endl;
                                        }
                                    } else {
                                        dropMapping = true;
                                    }
                                } else if (isFrame) {
                                } else {
                                    dropMapping = true;
                                }
                                if (dropMapping) {
                                    client_map_.erase(complete->request_id);
                                }
                            } else if (chunk->message_type == static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE)) {
                                BVERB << "[Rank 0 MPI Broker] Forwarding 'file not found' from rank "
                                      << chunk->source_rank << " to client: " << chunk->filename << std::endl;
                                client_map_.erase(chunk->request_id);
                                frame_requests_.erase(chunk->request_id);
                            } else {
                                BVERB << "[Rank 0 MPI Broker] Forwarding error from rank "
                                      << chunk->source_rank << " to client" << std::endl;
                                client_map_.erase(chunk->request_id);
                                frame_requests_.erase(chunk->request_id);
                            }

                            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
                            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                            client_router_->send(zmq::message_t(data.data(), data.size()), zmq::send_flags::none);

                            continue;
                        }
                    }

                    std::string message(data.begin(), data.end());

                    if (message.find("READY") == 0) {
                        std::stringstream ss(message);
                        std::string token, rank_str, hostname, ip;
                        std::getline(ss, token, '|');
                        std::getline(ss, rank_str, '|');
                        std::getline(ss, hostname, '|');
                        std::getline(ss, ip, '|');

                        int rank = std::stoi(rank_str);

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

void ZmqBroker::QueueRank0FileTransfer(const std::string& client_id, uint32_t request_id,
                                       const char* filename, uint32_t requestedChunkSize)
{
    if (!g_rankMemoryStore) {
        client_map_.erase(request_id);
        return;
    }

    auto entry = g_rankMemoryStore->GetFile(filename);
    if (!entry) {
        ZmqFileComplete nofile;
        memset(&nofile, 0, sizeof(nofile));
        nofile.magic = USD_FILE_MAGIC;
        nofile.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE);
        nofile.request_id = request_id;
        nofile.source_rank = 0;
        strncpy(nofile.filename, filename, sizeof(nofile.filename) - 1);

        client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
        client_router_->send(zmq::message_t(&nofile, sizeof(nofile)), zmq::send_flags::none);

        std::cout << "Rank 0 (MPI Broker): Rank 0 file not found: " << filename << std::endl;
        client_map_.erase(request_id);
        return;
    }

    const size_t kMaxChunkSize = 128 * 1024 * 1024;
    PendingFileTransfer t;
    t.client_id = client_id;
    t.request_id = request_id;
    t.entry = std::move(entry);  // immutable shared snapshot; valid for the transfer's life
    strncpy(t.filename, filename, sizeof(t.filename) - 1);
    t.chunk_size = (requestedChunkSize > 0)
        ? std::min<size_t>(requestedChunkSize, kMaxChunkSize)
        : DEFAULT_CHUNK_SIZE;
    pending_transfers_.push_back(std::move(t));
}

void ZmqBroker::TickPendingTransfers(int maxTransfers)
{
    if (pending_transfers_.empty() || maxTransfers <= 0) {
        return;
    }

    int advanced = 0;

    for (auto it = pending_transfers_.begin(); it != pending_transfers_.end() && advanced < maxTransfers; ) {
        PendingFileTransfer& t = *it;

        if (t.is_marker) {
            ZmqFileComplete complete;
            memset(&complete, 0, sizeof(complete));
            complete.magic = USD_FILE_MAGIC;
            complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
            complete.request_id = t.request_id;
            complete.source_rank = 0;
            strncpy(complete.filename, "__frame_complete__", sizeof(complete.filename) - 1);
            complete.total_size = t.marker_total;

            client_router_->send(zmq::buffer(t.client_id), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);

            std::cout << "Rank 0 (MPI Broker): Frame complete (" << t.marker_total
                      << " files) for client " << t.client_id.substr(0, 8) << "..." << std::endl;

            client_map_.erase(t.request_id);
            it = pending_transfers_.erase(it);
            continue;
        }

        if (!t.entry) {
            while (!t.entry && !t.files_in_frame.empty()) {
                std::string next = std::move(t.files_in_frame.front());
                t.files_in_frame.erase(t.files_in_frame.begin());
                auto e = g_rankMemoryStore ? g_rankMemoryStore->GetFile(next) : nullptr;
                if (e) {
                    strncpy(t.filename, next.c_str(), sizeof(t.filename) - 1);
                    t.entry = std::move(e);
                    t.offset = 0;
                }
            }
            if (!t.entry) {
                t.is_marker = true;
                continue;  // re-enter the same slot once so the marker is sent/erased
            }
        }

        const size_t total = t.entry->data.size();
        const uint8_t* buf = t.entry->data.data();

        if (total == 0) {
            ZmqFileComplete complete;
            memset(&complete, 0, sizeof(complete));
            complete.magic = USD_FILE_MAGIC;
            complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
            complete.request_id = t.request_id;
            complete.source_rank = 0;
            strncpy(complete.filename, t.filename, sizeof(complete.filename) - 1);
            complete.total_size = 0;

            client_router_->send(zmq::buffer(t.client_id), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);

            if (t.marker_total > 0) {
                t.entry.reset();
                t.filename[0] = '\0';
                t.offset = 0;
                ++advanced;
                ++it;
                continue;
            }

            client_map_.erase(t.request_id);
            it = pending_transfers_.erase(it);
            continue;
        }

        const size_t sendSize = std::min(t.chunk_size, total - t.offset);

        ZmqFileChunk response;
        memset(&response, 0, sizeof(response));
        response.magic = USD_FILE_MAGIC;
        response.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_CHUNK);
        response.request_id = t.request_id;
        response.source_rank = 0;
        strncpy(response.filename, t.filename, sizeof(response.filename) - 1);
        response.file_size = total;
        response.chunk_offset = t.offset;
        response.chunk_size = sendSize;

        const size_t totalSize = sizeof(response) + sendSize;

        zmq::message_t message(totalSize);
        memcpy(message.data(), &response, sizeof(response));
        memcpy(static_cast<uint8_t*>(message.data()) + sizeof(response),
               buf + t.offset, sendSize);

        client_router_->send(zmq::buffer(t.client_id), zmq::send_flags::sndmore);
        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
        client_router_->send(message, zmq::send_flags::none);

        t.offset += sendSize;
        ++advanced;

        if (t.offset < total) {
            ++it;
            continue;
        }

        ZmqFileComplete complete;
        memset(&complete, 0, sizeof(complete));
        complete.magic = USD_FILE_MAGIC;
        complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_FILE_COMPLETE);
        complete.request_id = t.request_id;
        complete.source_rank = 0;
        strncpy(complete.filename, t.filename, sizeof(complete.filename) - 1);
        complete.total_size = total;

        client_router_->send(zmq::buffer(t.client_id), zmq::send_flags::sndmore);
        client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
        client_router_->send(zmq::message_t(&complete, sizeof(complete)), zmq::send_flags::none);

        BVERB << "Rank 0 (MPI Broker): Sent rank 0 file: " << t.filename
              << " (" << total << " bytes)" << std::endl;

        if (t.marker_total > 0) {
            t.entry.reset();
            t.filename[0] = '\0';
            t.offset = 0;
            ++it;
            continue;
        }

        client_map_.erase(t.request_id);
        it = pending_transfers_.erase(it);
    }
}

void ZmqBroker::StatusPrinterThread() {
    int intervalSec = 10;

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

        std::cerr << std::endl;
        std::cerr << "╔════════════════════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cerr << "║ SSH TUNNEL REMINDER #" << std::left << std::setw(58) << reminder_count << "║" << std::endl;
        std::cerr << "╠════════════════════════════════════════════════════════════════════════════════╣" << std::endl;
        std::cerr << "║ Run this command on your laptop to connect:                                   ║" << std::endl;
        std::cerr << "╚════════════════════════════════════════════════════════════════════════════════╝" << std::endl;

#ifdef ANARI_USD_ENABLE_MPI
        if (workers_.size() > 1) {
            std::cerr << "ssh -N -L " << client_port_ << ":" << broker_ip_ << ":" << client_port_ << " \\" << std::endl;
        } else {
            std::cerr << "ssh -N -L " << client_port_ << ":localhost:" << client_port_ << " \\" << std::endl;
        }
#else
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


void ZmqBroker::Shutdown() {
    if (initialized_) {
        std::cout << "[Rank 0 MPI Broker] Shutting down..." << std::endl;

        ssh_reminder_active_ = false;
        if (ssh_reminder_thread_.joinable()) {
            ssh_reminder_thread_.join();
            std::cout << "[Rank 0 MPI Broker] SSH reminder thread stopped" << std::endl;
        }

        status_printer_active_ = false;
        if (status_printer_thread_.joinable()) {
            status_printer_thread_.join();
            std::cout << "[Rank 0 MPI Broker] Status printer thread stopped" << std::endl;
        }

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


bool ZmqBroker::GetPropertyAsInt32(const std::string& propertyName, int32_t& value) {
    if (propertyName == "workerCount") {
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
        value = 0;
        return true;
    } else if (propertyName == "totalWorkerCount") {
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
    std::string addr = GetBrokerAddress(rank_, 5555);
    std::cout << "[Worker Rank " << rank_ << "] Resolved rank 0 address via MPI: "
              << addr << std::endl;
    return addr;
#else
    std::string addr = "127.0.0.1:5555";
    std::cout << "[Non-MPI Worker] Connecting to localhost: " << addr << std::endl;
    return addr;
#endif
}

bool ZmqWorker::Connect() {
    if (connected_) return true;

    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        std::string rank0_address = ResolveRank0Address();

        if (rank0_address.empty() || rank0_address == "0.0.0.0:5555") {
            std::cerr << "[Worker Rank " << rank_ << "] ERROR: Invalid broker address from MPI!" << std::endl;
            return false;
        }

        std::string connect_addr = "tcp://" + rank0_address;
        std::cout << "[Worker Rank " << rank_ << "] Connecting to broker at " << connect_addr << std::endl;

        dealer_->connect(connect_addr);
        int linger = 0;
        dealer_->set(zmq::sockopt::linger, linger);

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


bool ZmqWorker::CheckForFileRequest(ZmqFileRequest& request, bool blocking) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        zmq::message_t empty, payload;

        auto flags = blocking ? zmq::recv_flags::none : zmq::recv_flags::dontwait;

        auto result = dealer_->recv(empty, flags);
        if (!result) {
            return false;  // No message available
        }

        WVERB << "[DEBUG Worker Rank " << rank_ << "] Received message on DEALER socket" << std::endl;

        result = dealer_->recv(payload, zmq::recv_flags::none);
        if (!result || payload.size() < sizeof(ZmqFileRequest)) {
            std::cerr << "[Worker Rank " << rank_ << "] Invalid payload size: " << payload.size()
                      << " (expected at least " << sizeof(ZmqFileRequest) << ")" << std::endl;
            return false;
        }

        memcpy(&request, payload.data(), sizeof(ZmqFileRequest));

        if (request.magic != USD_FILE_MAGIC) {
            std::cerr << "[Worker Rank " << rank_ << "] Invalid file request magic: 0x"
                      << std::hex << request.magic << " (expected 0x" << USD_FILE_MAGIC << ")"
                      << std::dec << std::endl;
            return false;
        }

        WVERB << "[DEBUG Worker Rank " << rank_ << "] Valid file request: "
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

        size_t totalMsgSize = sizeof(header) + dataSize;
        zmq::message_t message(totalMsgSize);

        memcpy(message.data(), &header, sizeof(header));
        memcpy(static_cast<uint8_t*>(message.data()) + sizeof(header), data, dataSize);

        zmq::message_t empty;
        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(message, zmq::send_flags::none);

        WVERB << "[Worker Rank " << rank_ << "] Sent file chunk: " << filename
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


bool ZmqWorker::SendFileNotification(const std::string& filename, uint64_t fileSize, uint64_t timestamp, const uint64_t* hash128) {
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
    bool success = true;

    for (const auto& client_id : client_ids_) {
        zmq::pollitem_t pollItems[] = { { static_cast<void*>(*client_router_), 0, ZMQ_POLLOUT, 0 } };
        try { zmq::poll(pollItems, 1, std::chrono::milliseconds(0)); } catch (const zmq::error_t&) {}
        if (!(pollItems[0].revents & ZMQ_POLLOUT)) {
            std::cerr << "[Rank 0 MPI Broker] client back-pressure - dropping notification '"
                      << notification.filename << "'" << std::endl;
            success = false;
            continue;
        }

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

bool ZmqBroker::ForwardSceneUpdateToClient(const ZmqSceneUpdate& update) {
    bool success = true;

    for (const auto& client_id : client_ids_) {
        zmq::pollitem_t pollItems[] = { { static_cast<void*>(*client_router_), 0, ZMQ_POLLOUT, 0 } };
        try { zmq::poll(pollItems, 1, std::chrono::milliseconds(0)); } catch (const zmq::error_t&) {}
        if (!(pollItems[0].revents & ZMQ_POLLOUT)) {
            std::cerr << "[Rank 0 MPI Broker] client back-pressure - dropping scene update '"
                      << update.getPrimPath() << "'" << std::endl;
            success = false;
            continue;
        }

        try {
            client_router_->send(zmq::buffer(client_id), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(), zmq::send_flags::sndmore);
            client_router_->send(zmq::message_t(&update, sizeof(update)), zmq::send_flags::none);

            std::cout << "[Rank 0 MPI Broker] Forwarded scene update to client: prim='"
                      << update.getPrimPath()
                      << "' property='" << update.getPropertyName()
                      << "' change=" << update.change_type
                      << " rank " << update.source_rank << std::endl;
        } catch (const zmq::error_t& e) {
            std::cerr << "[Rank 0 MPI Broker] ForwardSceneUpdateToClient error: " << e.what() << std::endl;
            success = false;
        }
    }

    return success;
}

bool ZmqWorker::SendSceneUpdate(const ZmqSceneUpdate& updateIn) {
    std::lock_guard<std::mutex> lock(socket_mutex_);

    try {
        ZmqSceneUpdate msg = updateIn;
        msg.magic = USD_FILE_MAGIC;
        msg.source_rank = rank_;

        zmq::message_t empty;
        zmq::message_t payload(&msg, sizeof(msg));

        dealer_->send(empty, zmq::send_flags::sndmore);
        dealer_->send(payload, zmq::send_flags::none);

        std::cout << "[Worker Rank " << rank_ << "] Sent scene update: prim='"
                  << msg.getPrimPath()
                  << "' property='" << msg.getPropertyName()
                  << "' change=" << msg.change_type
                  << " type=" << msg.value_type
                  << " commit=" << msg.commit_id
                  << " revision=" << msg.revision << std::endl;

        return true;
    } catch (const zmq::error_t& e) {
        std::cerr << "[Worker Rank " << rank_ << "] SendSceneUpdate error: " << e.what() << std::endl;
        return false;
    }
}

bool ZmqWorker::SendCommitSceneUpdate(uint64_t commitId, uint64_t revision, uint64_t timestamp) {
    ZmqSceneUpdate msg;
    msg.message_type = static_cast<uint32_t>(ZmqMessageType::NOTIFY_SCENE_UPDATE);
    msg.timestamp = timestamp;
    msg.commit_id = commitId;
    msg.revision = revision;
    msg.change_type = static_cast<int32_t>(ZmqSceneChangeType::Commit);
    msg.value_type = static_cast<int32_t>(ZmqPropertyValuetype::None);
    msg.setPrimPath("/Scene");
    msg.setPropertyName("usd::commit");
    return SendSceneUpdate(msg);
}


bool ZmqBroker::SendFileNotificationToClients(const std::string& filename, uint64_t fileSize, uint64_t timestamp,
                                              uint32_t messageType, const uint64_t* hash128,
                                              const uint64_t* hashPrev128, bool hasOldData) {
    ZmqFileNotification msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = USD_FILE_MAGIC;
    msg.message_type = messageType;
    msg.source_rank = 0;
    strncpy(msg.filename, filename.c_str(), sizeof(msg.filename) - 1);
    msg.filename[sizeof(msg.filename) - 1] = '\0';
    msg.file_size = fileSize;
    msg.timestamp = timestamp;
    msg.hash128[0] = hash128 ? hash128[0] : 0;
    msg.hash128[1] = hash128 ? hash128[1] : 0;
    msg.hashPrev128[0] = (hashPrev128 && hasOldData) ? hashPrev128[0] : 0;
    msg.hashPrev128[1] = (hashPrev128 && hasOldData) ? hashPrev128[1] : 0;
    msg.hasOldData = hasOldData;

    bool ok = ForwardNotificationToClient(msg);
    GetDiffCaptureStatus().OnNotification(msg.source_rank, msg.filename, msg.timestamp);
    return ok;
}

bool ZmqBroker::SendSceneUpdateToClients(uint64_t commitId, uint64_t revision, uint64_t timestamp) {
    ZmqSceneUpdate msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = USD_FILE_MAGIC;
    msg.message_type = static_cast<uint32_t>(ZmqMessageType::NOTIFY_SCENE_UPDATE);
    msg.timestamp = timestamp;
    msg.commit_id = commitId;
    msg.revision = revision;
    msg.change_type = static_cast<int32_t>(ZmqSceneChangeType::Commit);
    msg.value_type = static_cast<int32_t>(ZmqPropertyValuetype::None);
    msg.setPrimPath("/Scene");
    msg.setPropertyName("usd::commit");
    return ForwardSceneUpdateToClient(msg);
}

} // namespace usd_bridge