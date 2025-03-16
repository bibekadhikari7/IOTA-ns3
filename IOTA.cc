
 #include "ns3/core-module.h"
 #include "ns3/network-module.h"
 #include "ns3/wifi-module.h"
 #include "ns3/internet-module.h"
 #include "ns3/mobility-module.h"
 #include "ns3/applications-module.h"
 #include "ns3/flow-monitor-helper.h"
 #include "ns3/ipv4-flow-classifier.h"
 #include "ns3/netanim-module.h"
 
 #include <unordered_map>
 #include <map>
 #include <set>
 #include <string>
 #include <vector>
 #include <fstream>
 #include <random>
 #include <sstream>
 #include <ctime>
 #include <algorithm>
 #include <queue>
 #include <iomanip>
 #include <cmath>
 #include <chrono>
 
 using namespace ns3;
 
 NS_LOG_COMPONENT_DEFINE("IOTAsimulation");
 
 // Store simulation start time for wall-clock formatting
 static std::chrono::system_clock::time_point g_simulationStartTime;
 
 //Format simulation time to wall-clock time (HH:MM:SS.ss)

 static std::string FormatHms(double simulationSeconds) {
     auto realTime = g_simulationStartTime + std::chrono::milliseconds(static_cast<long>(simulationSeconds * 1000));
     auto timeT = std::chrono::system_clock::to_time_t(realTime);
     auto millisPart = std::chrono::duration_cast<std::chrono::milliseconds>(realTime.time_since_epoch()).count() % 1000;
     std::tm* localTime = std::localtime(&timeT);
     std::ostringstream oss;
     oss << std::setw(2) << std::setfill('0') << localTime->tm_hour << ":"
         << std::setw(2) << std::setfill('0') << localTime->tm_min << ":"
         << std::setw(2) << std::setfill('0') << localTime->tm_sec << "."
         << std::setw(2) << std::setfill('0') << (millisPart / 10);
     return oss.str();
 }
 
 //Transaction structure for IOTA network

 struct Tx {
     Tx() : id(""), issuerNode(999999), isReal(false), confirmed(false) {}
     Tx(const std::string &txid,
        const std::vector<std::string> &pars,
        uint32_t issuer,
        bool real)
       : id(txid),
         parents(pars),
         issuerNode(issuer),
         isReal(real),
         confirmed(false) {}
     std::string id;                  // Transaction ID
     std::vector<std::string> parents; // Parent transactions
     uint32_t issuerNode;             // Node that issued the transaction
     bool isReal;                     // Whether it's a real data transaction
     bool confirmed;                  // Whether it's confirmed
 };
 
 // Tangle 
 class Tangle {
 public:
     Tangle() {
         // Insert  genesis transactions as starting points
         AddTransaction(Tx("154safc4a", {}, 0, false));
         AddTransaction(Tx("1544a64fd6a", {}, 0, false));
     }
     
     // Add a transaction to the tangle
     bool AddTransaction(const Tx &tx) {
         if (m_store.find(tx.id) != m_store.end())
             return false; // Already exists
         m_store[tx.id] = tx;
         m_tips.insert(tx.id);
         // Remove parents from tips list
         for (auto &p: tx.parents) {
             m_tips.erase(p);
         }
         return true;
     }
     
     // Validate parent transactions
     bool ValidateParents(const Tx &tx) const {
         for (auto &p: tx.parents) {
             if (m_store.find(p) == m_store.end())
                 return false; // Missing parent
         }
         return true;
     }
     
     // Get all tip transactions
     std::vector<std::string> GetAllTips() const {
         return std::vector<std::string>(m_tips.begin(), m_tips.end());
     }
     
     // Pick 2 tips using Monte Carlo Markov Chain algorithm
     std::vector<std::string> Pick2TipsMCMC() {
         std::vector<std::string> selected;
         auto tips = GetAllTips();
         if (tips.empty()) return selected;
         
         // Try to select 2 tips
         for (int i = 0; i < 2; i++) {
             std::string t = PerformRandomWalk();
             if (!t.empty()) selected.push_back(t);
         }
         
         // Ensure we have 2 tips (use genesis if needed)
         while (selected.size() < 2) {
             selected.push_back("154safc4a");
         }
         return selected;
     }
     
 private:
     std::unordered_map<std::string, Tx> m_store;  // Transaction storage
     std::set<std::string> m_tips;                 // Current tips
     
     // Perform a random walk through the tangle to select a tip
     std::string PerformRandomWalk() {
         auto tips = GetAllTips();
         if (tips.empty()) return "";
         
         static std::random_device rd;
         static std::mt19937 rng(rd());
         std::uniform_int_distribution<> dist(0, tips.size() - 1);
         std::string current = tips[dist(rng)];
         
         // Walk backward from tip for up to 3 steps
         int maxDepth = 3;
         for (int d = 0; d < maxDepth; d++) {
             auto it = m_store.find(current);
             if (it == m_store.end()) break;
             if (it->second.parents.empty()) break; // Genesis
             std::uniform_int_distribution<> pd(0, it->second.parents.size() - 1);
             current = it->second.parents[pd(rng)];
         }
         return current;
     }
 };
 
 // Structure for pending transactions in queue

 struct PendingTx {
     Tx transaction;
     std::string payload;
 };
 
 //IOTA Simulation Application
 class IOTAsimulation : public Application {
 public:
     IOTAsimulation();
     
     // Setup the simulation parameters
     void Setup(uint32_t nodeId,
                Ipv4InterfaceContainer* ifaces,
                uint32_t nNodes,
                const std::string &csvFile,
                double txRate);
                
     // Print summary of real transactions
     void PrintRealTxSummary() const;
     
 private:
     // Application lifecycle
     virtual void StartApplication() override;
     virtual void StopApplication() override;
     
     // Tangle management
     Tangle m_tangle;
     std::unordered_map<std::string, Tx> m_realTxStore;  // Store for real transactions from node0
     std::unordered_map<std::string, uint32_t> m_fallbackCount;  // Track fallback attempts
     uint32_t m_maxFallback;  // Max fallback attempts
     
     // Transaction scheduling
     void ScheduleNextVirtualTx();
     void CreateVirtualTx();
     
     // CSV data processing
     void LoadCsv();
     void ScheduleNextCsvTx();
     void CreateRealDataTx();
     
     // Network communication
     void HandleRead(Ptr<Socket> socket);
     void HandleTx(const std::string &ser);
     void HandleAck(const std::string &ackType, const std::string &ser);
     void HandleConfirmation(const std::string &txId);
     
     // Transaction broadcasting
     void BroadcastTx(const Tx &tx, const std::string &payload);
     void ReBroadcastTx(const Tx &tx, const std::string &payload);
     void BroadcastConfirmation(const std::string &txId);
     void BroadcastCongCtrl(const std::string &ctrlMsg);
     void HandleCongCtrl(const std::string &msg);
     
     // Acknowledgment handling
     void SendRefAck(const std::string &txId);
     void SendViewAck(const std::string &txId);
     void TrackRefAck(const std::string &txId, uint32_t fromNode);
     void TrackViewAck(const std::string &txId, uint32_t fromNode);
     bool IsDoubleSpending(const std::string &txId);
     
     // Logging and metrics
     std::string NowStr();
     void Log(const std::string &s);
     void Log(const std::string &s, bool highlight);
     void RecordPropagationDelay(const std::string &txId, double delay);
     void RecordAckTime(const std::string &txId, double delay);
     
     // Queue management & congestion control
     void ScheduleProcessQueue();
     void ProcessQueue();
     void CheckQueueState();
     
     // Fallback mechanism for unconfirmed transactions
     void ScheduleFallback();
     void FallbackCheck();
     
     // Neighbor selection for broadcasting
     std::vector<uint32_t> GetFanOutNeighbors();
     
 private:
     // Network and communication variables
     Ptr<Socket> m_socket;
     Ipv4InterfaceContainer* m_ifaces;
     uint32_t m_nodeId, m_nNodes;
     
     // Data source
     std::string m_csvFile;
     std::vector<std::string> m_csvData;
     uint32_t m_csvIndex;
     
     // Event scheduling
     EventId m_csvEvent;
     EventId m_virtualEvent;
     EventId m_processEvent;
     
     // Rates and parameters
     double m_txRate;
     double m_processRate;
     uint32_t m_queueCapacity;
     
     // Acknowledgment tracking
     std::map<std::string, std::set<uint32_t>> m_refAckMap;
     std::map<std::string, std::set<uint32_t>> m_viewAckMap;
     
     // Random number generation
     std::random_device m_rd;
     std::mt19937 m_rng;
     
     // Transaction timing metrics
     std::unordered_map<std::string, double> m_txCreationTime;
     std::unordered_map<std::string, double> m_txAttachTime;
     std::unordered_map<std::string, double> m_txConfirmationTime;
     
     // Incoming transaction queue
     std::queue<PendingTx> m_incomingQueue;
 };
 
 //Constructor - Initialize default values

 IOTAsimulation::IOTAsimulation()
     : m_socket(nullptr),
       m_ifaces(nullptr),
       m_nodeId(0),
       m_nNodes(0),
       m_csvFile(""),
       m_csvIndex(0),
       m_txRate(1.0),
       m_rng(m_rd())
 {
     m_processRate = 1000.0;      // Process up to 1000 transactions per second
     m_queueCapacity = 9999;      // Queue capacity
     m_maxFallback = 10;          // Maximum fallback attempts
 }
 
 
  //Setup the simulation parameters
  
 void IOTAsimulation::Setup(uint32_t nodeId,
                            Ipv4InterfaceContainer* ifaces,
                            uint32_t nNodes,
                            const std::string &csvFile,
                            double txRate)
 {
     m_nodeId = nodeId;
     m_ifaces = ifaces;
     m_nNodes = nNodes;
     m_csvFile = csvFile;
     m_txRate = txRate;
 }
 
 // Start the application - called by NS3 scheduler
 void IOTAsimulation::StartApplication() {
     // Create UDP socket if it doesn't exist
     if (!m_socket) {
         TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
         m_socket = Socket::CreateSocket(GetNode(), tid);
         m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 9999));
         m_socket->SetRecvCallback(MakeCallback(&IOTAsimulation::HandleRead, this));
     }
     
     // Node 0 loads the CSV data and schedules fallback checks
     if (m_nodeId == 0) {
         LoadCsv();
         if (!m_csvData.empty()) {
             m_csvEvent = Simulator::Schedule(Seconds(5.0),
                                              &IOTAsimulation::CreateRealDataTx,
                                              this);
         }
         ScheduleFallback();
     }
     
     // Schedule virtual transactions if rate > 0
     if (m_txRate > 0) ScheduleNextVirtualTx();
     
     // Schedule queue processing
     ScheduleProcessQueue();
 }
 
 //Stop the application - cleanup resources

 void IOTAsimulation::StopApplication() {
     // Close socket
     if (m_socket) {
         m_socket->Close();
         m_socket = nullptr;
     }
     
     // Cancel pending events
     if (m_csvEvent.IsPending()) Simulator::Cancel(m_csvEvent);
     if (m_virtualEvent.IsPending()) Simulator::Cancel(m_virtualEvent);
     if (m_processEvent.IsPending()) Simulator::Cancel(m_processEvent);
 }
 
 //Schedule fallback check for unconfirmed transactions
  
 void IOTAsimulation::ScheduleFallback() {
     Simulator::Schedule(Seconds(3.0),
                         &IOTAsimulation::FallbackCheck,
                         this);
 }
 
 // Check and Gossip/rebroadcast transactions that haven't received enough acknowledgments
  
 void IOTAsimulation::FallbackCheck() {

     if (m_nodeId != 0) {
         ScheduleFallback();
         return;
     }
     
     // Check each real transaction
     for (auto &kv : m_realTxStore) {
         const std::string &txId = kv.first;
         const Tx &theTx = kv.second;
         
         // Skip if already attached
         if (m_txAttachTime.find(txId) != m_txAttachTime.end())
             continue;
             
         // Count references
         uint32_t refCount = 0;
         if (m_refAckMap.find(txId) != m_refAckMap.end()) {
             refCount = (uint32_t)m_refAckMap[txId].size();
         }
         
         // Rebroadcast if fewer than 5 references
         if (refCount < 5) {
             uint32_t &count = m_fallbackCount[txId];
             if (count < m_maxFallback) {
                 count++;
                 NS_LOG_INFO("Node0 fallback re-broadcast " << txId
                             << " references=" << refCount
                             << " attempt=" << count);
                 ReBroadcastTx(theTx, "fallback");
             }
         }
     }
     
     // Schedule next check
     ScheduleFallback();
 }
 
 // Schedule next virtual transaction based on tx rate
  
 void IOTAsimulation::ScheduleNextVirtualTx() {
     if (m_txRate <= 0.0) return;
     
     // Exponential distribution for inter-arrival times
     std::exponential_distribution<double> ed(m_txRate);
     double d = ed(m_rng);
     
     m_virtualEvent = Simulator::Schedule(Seconds(d),
                                          &IOTAsimulation::CreateVirtualTx,
                                          this);
 }
 
 // Create a virtual transaction and broadcast it
  
 void IOTAsimulation::CreateVirtualTx() {
     // Select tips using MCMC
     auto tips = m_tangle.Pick2TipsMCMC();
     
     // Generate transaction ID
     std::stringstream ss;
     ss << "VR-" << m_nodeId << "-" << std::hex << m_rng();
     std::string txId = ss.str();
     
     // Create transaction
     Tx tx(txId, tips, m_nodeId, false);
     
     // Validate parents
     if (!m_tangle.ValidateParents(tx)) {
         ScheduleNextVirtualTx();
         return;
     }
     
     // Add to tangle
     bool added = m_tangle.AddTransaction(tx);
     if (!added) {
         ScheduleNextVirtualTx();
         return;
     }
     
     // 99% chance to reference a transaction, 1% to keep in tip pool
     std::uniform_real_distribution<double> dd(0.0, 1.0);
     bool ref = (dd(m_rng) < 0.99);
     
     if (ref) {
         // Try to reference a real transaction if possible
         std::string realTxId;
         for (auto &t: tips) {
             if (t.rfind("RT-", 0) == 0) {
                 realTxId = t;
                 break;
             }
         }
         
         // Update a parent to reference the real tx
         if (!realTxId.empty()) {
             tx.parents[0] = realTxId;
             m_tangle.AddTransaction(tx);
             SendRefAck(realTxId);
         }
         
         // Log and broadcast
         std::stringstream logMsg;
         logMsg << "node" << m_nodeId
                << " created transaction " << tx.id
                << " : References: " << tx.parents[0] << ", " << tx.parents[1];
         Log(logMsg.str());
         BroadcastTx(tx, "");
     }
     else {
         // Keep in tip pool
         std::stringstream logMsg;
         logMsg << "node" << m_nodeId
                << " keeps transaction " << tx.id << " in tip pool";
         Log(logMsg.str());
         ReBroadcastTx(tx, "");
     }
     
     // Schedule next transaction
     ScheduleNextVirtualTx();
 }
 
 //Load data from CSV file
  
 void IOTAsimulation::LoadCsv() {
     std::ifstream inf(m_csvFile.c_str());
     if (!inf.is_open()) {
         Log("node0: Could not open CSV file: " + m_csvFile);
         return;
     }
     
     // Read all non-empty lines
     std::string line;
     while (std::getline(inf, line)) {
         if (!line.empty()) m_csvData.push_back(line);
     }
     inf.close();
     
     // Log success
     std::stringstream msg;
     msg << "node0 loaded " << m_csvData.size()
         << " lines from CSV: " << m_csvFile;
     Log(msg.str());
 }
 
 // Schedule next real data transaction
  
 void IOTAsimulation::ScheduleNextCsvTx() {
     if (m_csvIndex < m_csvData.size()) {
         // Fixed delay if txRate is 0
         if (m_txRate <= 0.0) {
             m_csvEvent = Simulator::Schedule(Seconds(5.0),
                                              &IOTAsimulation::CreateRealDataTx,
                                              this);
             return;
         }
         
         // Otherwise use exponential distribution
         std::exponential_distribution<double> ed(m_txRate);
         double d = ed(m_rng);
         m_csvEvent = Simulator::Schedule(Seconds(d),
                                          &IOTAsimulation::CreateRealDataTx,
                                          this);
     }
     else {
         // End simulation when all data is processed
         Simulator::Schedule(Seconds(10.0), &Simulator::Stop);
     }
 }
 
 // Create a transaction with real data from CSV

 void IOTAsimulation::CreateRealDataTx() {
     if (m_csvIndex >= m_csvData.size()) return;
     
     // Get next CSV line
     std::string data = m_csvData[m_csvIndex];
     m_csvIndex++;
     
     // Log
     {
         std::stringstream msg;
         msg << "node0 picks up REAL DATA: " << data;
         Log(msg.str());
     }
     
     // Select tips using MCMC
     auto tips = m_tangle.Pick2TipsMCMC();
     if (tips.size() < 2) {
         tips.clear();
         tips.push_back("154safc4a");
         tips.push_back("1544a64fd6a");
     }
     
     // Create transaction ID
     std::stringstream ss;
     ss << "RT-" << m_nodeId << "-" << std::hex << m_rng();
     std::string txId = ss.str();
     
     // Create real transaction
     Tx tx(txId, tips, m_nodeId, true);
     
     // Validate parents
     if (!m_tangle.ValidateParents(tx)) {
         Log("node0: unknown parents => skipping real Tx " + tx.id);
         ScheduleNextCsvTx();
         return;
     }
     
     // Add to tangle
     bool added = m_tangle.AddTransaction(tx);
     if (!added) {
         Log("node0: duplicate real Tx " + tx.id + " => skipping");
         ScheduleNextCsvTx();
         return;
     }
     
     // Record creation time
     double pickTime = Simulator::Now().GetSeconds();
     m_txCreationTime[tx.id] = pickTime;
     m_realTxStore[txId] = tx;
     
     // Log with highlighting
     const std::string redStart = "\033[1;31m";
     const std::string colorEnd = "\033[0m";
     NS_LOG_UNCOND(redStart << "Transaction " << tx.id
                   << " picked up by node0 at time "
                   << FormatHms(pickTime)
                   << colorEnd);
     
     // Log parent references
     {
         std::stringstream ps;
         for (size_t i = 0; i < tips.size(); i++) {
             ps << tips[i];
             if (i + 1 < tips.size()) ps << ", ";
         }
         std::stringstream logMsg;
         logMsg << "node0 created transaction " << tx.id
                << " : References: " << ps.str();
         Log(logMsg.str());
     }
     
     // Broadcast transaction
     BroadcastTx(tx, data);
     
     // Schedule next transaction
     ScheduleNextCsvTx();
 }
 
 // Schedule queue processing

 void IOTAsimulation::ScheduleProcessQueue() {
     m_processEvent = Simulator::Schedule(Seconds(1.0),
                                          &IOTAsimulation::ProcessQueue,
                                          this);
 }
 
 //Process the incoming transaction queue
 void IOTAsimulation::ProcessQueue() {
     // Record batch start time
     Time batchStart = Simulator::Now();
 
     // Process up to m_processRate (1000) transactions
     uint32_t n = (uint32_t)m_processRate;
     while (!m_incomingQueue.empty() && n > 0) {
         PendingTx front = m_incomingQueue.front();
         m_incomingQueue.pop();
         n--;
 
         const Tx &tx = front.transaction;
         
         // Skip invalid transactions
         if (IsDoubleSpending(tx.id)) continue;
         if (!m_tangle.ValidateParents(tx)) continue;
         
         // Add to tangle
         bool added = m_tangle.AddTransaction(tx);
         if (!added) continue;
         
         // Send acknowledgment for real transactions
         if (tx.isReal && m_nodeId != 0) {
             SendRefAck(tx.id);
         }
         
         // Rebroadcast to neighbors
         ReBroadcastTx(tx, front.payload);
     }
 
     // Check queue state for congestion control
     CheckQueueState();
 
     // Schedule next batch to start on 1-second boundary
     Time batchEnd = Simulator::Now();
     Time elapsed = batchEnd - batchStart;
     Time delay = Seconds(1.0) - elapsed;
     if (delay < Seconds(0)) {
         delay = Seconds(0);
     }
     Simulator::Schedule(delay, &IOTAsimulation::ProcessQueue, this);
 }
 
 //Check queue state and broadcast congestion control messages if needed
  
 void IOTAsimulation::CheckQueueState() {
     double usage = (double)m_incomingQueue.size() / (double)m_queueCapacity;
     
     // High queue utilization - slow down
     if (usage > 0.9) {
         BroadcastCongCtrl("CCTRL:SLOW");
     }
     // Low queue utilization - speed up
     else if (usage < 0.3) {
         BroadcastCongCtrl("CCTRL:SPEED");
     }
 }
 
 //Select fan-out neighbors for broadcasting (3 closest nodes)
  
 std::vector<uint32_t> IOTAsimulation::GetFanOutNeighbors() {
     std::vector<std::pair<double, uint32_t>> distVec;
     
     // Get current node position
     Ptr<Node> me = NodeList::GetNode(m_nodeId);
     Ptr<MobilityModel> mm = me->GetObject<MobilityModel>();
     Vector myPos = mm->GetPosition();
     
     // Calculate distance to all other nodes
     for (uint32_t i = 0; i < m_nNodes; i++) {
         if (i == m_nodeId) continue;
         
         Ptr<Node> other = NodeList::GetNode(i);
         Ptr<MobilityModel> om = other->GetObject<MobilityModel>();
         Vector op = om->GetPosition();
         
         double d = std::sqrt((myPos.x - op.x) * (myPos.x - op.x) +
                              (myPos.y - op.y) * (myPos.y - op.y));
         distVec.push_back({d, i});
     }
     
     // Sort by distance
     std::sort(distVec.begin(), distVec.end(),
               [](auto &a, auto &b) { return a.first < b.first; });
     
     // Take 3 closest neighbors
     std::vector<uint32_t> neighbors;
     for (size_t idx = 0; idx < distVec.size() && neighbors.size() < 3; idx++) {
         neighbors.push_back(distVec[idx].second);
     }
     
     return neighbors;
 }
 
 //Broadcast a new transaction to neighbors

 void IOTAsimulation::BroadcastTx(const Tx &tx, const std::string &payload) {
     // Serialize transaction
     std::stringstream ss;
     ss << "T:" << tx.id << "##";
     for (size_t i = 0; i < tx.parents.size(); i++) {
         ss << tx.parents[i];
         if (i + 1 < tx.parents.size()) ss << ";";
     }
     ss << "##" << tx.issuerNode << "##" << payload;
     std::string out = ss.str();
     
     // Create packet
     Ptr<Packet> p = Create<Packet>((uint8_t*)out.data(), out.size());
     
     // Send to 3 closest neighbors
     auto neigh = GetFanOutNeighbors();
     for (auto n: neigh) {
         InetSocketAddress addr(m_ifaces->GetAddress(n), 9999);
         m_socket->SendTo(p, 0, addr);
     }
     
     // Log
     std::stringstream msg;
     msg << "node" << m_nodeId << " broadcasts Transaction " << tx.id;
     Log(msg.str());
 }
 
 // Rebroadcast a transaction to neighbors
  
 void IOTAsimulation::ReBroadcastTx(const Tx &tx, const std::string &payload) {
     // Log for real transactions
     if (tx.isReal) {
         std::stringstream msg;
         msg << "node" << m_nodeId
             << " re-broadcasts Transaction " << tx.id;
         Log(msg.str());
     }
     
     // Serialize transaction
     std::stringstream ss;
     ss << "T:" << tx.id << "##";
     for (size_t i = 0; i < tx.parents.size(); i++) {
         ss << tx.parents[i];
         if (i + 1 < tx.parents.size()) ss << ";";
     }
     ss << "##" << tx.issuerNode << "##" << payload;
     std::string out = ss.str();
     
     // Create packet
     Ptr<Packet> p = Create<Packet>((uint8_t*)out.data(), out.size());
     
     // Send to 3 closest neighbors
     auto neigh = GetFanOutNeighbors();
     for (auto n: neigh) {
         InetSocketAddress addr(m_ifaces->GetAddress(n), 9999);
         m_socket->SendTo(p, 0, addr);
     }
 }
 
 //Broadcast confirmation to all nodes

 void IOTAsimulation::BroadcastConfirmation(const std::string &txId) {
     // Serialize confirmation
     std::stringstream ss;
     ss << "C:" << txId;
     std::string out = ss.str();
     
     // Create packet
     Ptr<Packet> p = Create<Packet>((uint8_t*)out.data(), out.size());
     
     // Send to all other nodes
     for (uint32_t i = 0; i < m_nNodes; i++) {
         if (i == m_nodeId) continue;
         InetSocketAddress addr(m_ifaces->GetAddress(i), 9999);
         m_socket->SendTo(p, 0, addr);
     }
     
     // Log
     std::stringstream msg;
     msg << "node" << m_nodeId
         << " broadcasts Confirmation for Transaction " << txId;
     Log(msg.str());
 }
 
 // Broadcast congestion control message 
  
 void IOTAsimulation::BroadcastCongCtrl(const std::string &ctrlMsg) {
     // Create and send packet to nodes
     Ptr<Packet> p = Create<Packet>((uint8_t*)ctrlMsg.data(), ctrlMsg.size());
     for (uint32_t i = 0; i < m_nNodes; i++) {
         if (i == m_nodeId) continue;
         InetSocketAddress addr(m_ifaces->GetAddress(i), 9999);
         m_socket->SendTo(p, 0, addr);
     }
 }
 
 //Handle received congestion control message
  
 void IOTAsimulation::HandleCongCtrl(const std::string &msg) {
     std::string ctrl = msg.substr(6);
     
     // Slow down if networks is congested
     if (ctrl == "SLOW") {
         m_txRate = std::max(1.0, m_txRate * 0.8);
     }
     // Speed up if network has capacity
     else if (ctrl == "SPEED") {
         m_txRate = std::min(m_txRate + 0.5, 10.0);
     }
 }
 
 // Handle incoming packets
  
 void IOTAsimulation::HandleRead(Ptr<Socket> socket) {
     Address from;
     Ptr<Packet> packet;
     
     while ((packet = socket->RecvFrom(from))) {
         uint32_t sz = packet->GetSize();
         if (sz == 0) continue;
         
         // Extract packet data
         std::vector<uint8_t> buf(sz);
         packet->CopyData(buf.data(), sz);
         std::string data((char*)buf.data(), sz);
         
         // Route to appropriate handler based on message type
         if (data.rfind("T:", 0) == 0) {
             // Transaction
             std::string c = data.substr(2);
             HandleTx(c);
         }
         else if (data.rfind("AR:", 0) == 0) {
             // Reference acknowledgment
             std::string c = data.substr(3);
             HandleAck("AR", c);
         }
         else if (data.rfind("C:", 0) == 0) {
             // Confirmation
             std::string txId = data.substr(2);
             HandleConfirmation(txId);
         }
         else if (data.rfind("AV:", 0) == 0) {
             // View acknowledgment
             std::string c = data.substr(3);
             HandleAck("AV", c);
         }
         else if (data.rfind("CCTRL:", 0) == 0) {
             // Congestion control
             HandleCongCtrl(data);
         }
     }
 }
 
 // Handle incoming transaction message
  
 void IOTAsimulation::HandleTx(const std::string &ser) {
     // Parse serialized transaction
     std::vector<std::string> parts;
     {
         std::stringstream ss(ser);
         std::string item;
         while (std::getline(ss, item, '#')) {
             if (!item.empty()) parts.push_back(item);
         }
     }
     
     // Ensure  have all parts
     if (parts.size() < 4) return;
     
     // Extract transaction components
     std::string txId    = parts[0];
     std::string pStr    = parts[1];
     uint32_t issuer     = std::stoi(parts[2]);
     std::string payload = parts[3];
     
     // Parse parent transactions
     std::vector<std::string> pars;
     {
         std::stringstream s2(pStr);
         std::string x;
         while (std::getline(s2, x, ';')) {
             if (!x.empty()) pars.push_back(x);
         }
     }
     
     // Create transaction object
     bool isReal = (txId.rfind("RT-", 0) == 0);
     Tx newTx(txId, pars, issuer, isReal);
     
     // Add to queue if space available
     if (m_incomingQueue.size() >= m_queueCapacity) return;
     
     PendingTx pq;
     pq.transaction = newTx;
     pq.payload = payload;
     m_incomingQueue.push(pq);
     
     // Check queue congestion
     CheckQueueState();
 }
 
 //Handle acknowledgment messages
  
 void IOTAsimulation::HandleAck(const std::string &ackType, const std::string &ser) {
     // Parse acknowledgment
     size_t bar = ser.find('|');
     if (bar == std::string::npos) return;
     
     std::string txId = ser.substr(0, bar);
     uint32_t fromNode = std::stoi(ser.substr(bar + 1));
     
     //  processes acknowledgments
     if (m_nodeId != 0) return;
     
     if (ackType == "AR") {
         // Handle reference acknowledgment
         TrackRefAck(txId, fromNode);
         
         // If all nodes have acknowledged, broadcast confirmation
         if (m_refAckMap[txId].size() == (m_nNodes - 1)) {
             BroadcastConfirmation(txId);
         }
         
         // Log reach 5 references (minimum for attachment)
         if (m_refAckMap[txId].size() == 5) {
             std::stringstream msg;
             msg << "Transaction " << txId << " status is conformed";
             Log(msg.str());
         }
     }
     else if (ackType == "AV") {
         // Handle view acknowledgment
         TrackViewAck(txId, fromNode);
     }
 }
 
 //Handle confirmation message
  
 void IOTAsimulation::HandleConfirmation(const std::string &txId) {
     // Send view acknowledgment
     SendViewAck(txId);
     
     // Log
     std::stringstream msg;
     msg << "node" << m_nodeId
         << " received Confirmation for Transaction " << txId;
     Log(msg.str());
     
     // Record acknowledgment time
     if (m_txCreationTime.find(txId) != m_txCreationTime.end()) {
         double ackTime = Simulator::Now().GetSeconds() - m_txCreationTime[txId];
         RecordAckTime(txId, ackTime);
     }
 }
 
 // Send reference acknowledgment to node 0
  
 void IOTAsimulation::SendRefAck(const std::string &txId) {
     // Log validation
     {
         std::stringstream logMsg;
         logMsg << "node" << m_nodeId
                << " validated the Transaction " << txId;
         Log(logMsg.str(), true);
     }
     
     // Serialize and send acknowledgment
     std::stringstream ss;
     ss << "AR:" << txId << "|" << m_nodeId;
     std::string out = ss.str();
     
     Ptr<Packet> p = Create<Packet>((uint8_t*)out.data(), out.size());
     InetSocketAddress addr(m_ifaces->GetAddress(0), 9999);
     m_socket->SendTo(p, 0, addr);
 }
 
 // Send view acknowledgment to node 0

 void IOTAsimulation::SendViewAck(const std::string &txId) {
     // Log receipt
     {
         std::stringstream logMsg;
         logMsg << "node" << m_nodeId
                << " received the Transaction " << txId;
         Log(logMsg.str());
     }
     
     // Serialize and send acknowledgment
     std::stringstream ss;
     ss << "AV:" << txId << "|" << m_nodeId;
     std::string out = ss.str();
     
     Ptr<Packet> p = Create<Packet>((uint8_t*)out.data(), out.size());
     InetSocketAddress addr(m_ifaces->GetAddress(0), 9999);
     m_socket->SendTo(p, 0, addr);
 }
 
 // Track reference acknowledgments from nodes

 void IOTAsimulation::TrackRefAck(const std::string &txId, uint32_t fromNode) {
     auto &s = m_refAckMap[txId];
     
     // Only track each node once
     if (s.find(fromNode) == s.end()) {
         s.insert(fromNode);
         
         // Log
         {
             std::stringstream ss;
             ss << "node0 sees validated ack from node " << fromNode
                << " for transaction " << txId;
             Log(ss.str());
         }
         
         // When 5 nodes have referenced (minimum for attachment or to reach consensus)
         if (s.size() == 5) {
             double attachTime = Simulator::Now().GetSeconds();
             m_txAttachTime[txId] = attachTime;
             
             double creationTime = 0.0;
             if (m_txCreationTime.find(txId) != m_txCreationTime.end()) {
                 creationTime = m_txCreationTime[txId];
             }
             
             double afterCreation = attachTime - creationTime;
             
             const std::string redStart = "\033[1;31m";
             const std::string colorEnd = "\033[0m";
             NS_LOG_UNCOND(redStart << "Transaction " << txId
                           << " attached to Tangle at time "
                           << FormatHms(attachTime)
                           << " (" << afterCreation << " s after creation)"
                           << colorEnd);
         }
         
         // When all nodes have referenced (full availability)
         if (s.size() == (m_nNodes - 1)) {
             double now = Simulator::Now().GetSeconds();
             double creationTime = 0.0;
             double attachTime = 0.0;
             
             if (m_txCreationTime.find(txId) != m_txCreationTime.end()) {
                 creationTime = m_txCreationTime[txId];
             }
             
             if (m_txAttachTime.find(txId) != m_txAttachTime.end()) {
                 attachTime = m_txAttachTime[txId];
             }
             
             double afterCreation = now - creationTime;
             double afterAttach = (attachTime > 0.0) ? (now - attachTime) : 0.0;
             
             const std::string redStart = "\033[1;31m";
             const std::string colorEnd = "\033[0m";
             NS_LOG_UNCOND(redStart << "Transaction " << txId
                           << " fully available on IOTA at time "
                           << FormatHms(now)
                           << " (" << afterCreation
                           << " s after creation"
                           << ((attachTime > 0.0)
                              ? (" and " + std::to_string(afterAttach) + " s after attachment)")
                              : ")")
                           << colorEnd);
         }
     }
 }
 
 //Track view acknowledgments from nodes
  
 void IOTAsimulation::TrackViewAck(const std::string &txId, uint32_t fromNode) {
     auto &sv = m_viewAckMap[txId];
     
     // Only track each node once
     if (sv.find(fromNode) == sv.end()) {
         sv.insert(fromNode);
         
         // Log
         {
             std::stringstream msg;
             msg << "node0 sees 'received' ack from node " << fromNode
                 << " for transaction " << txId;
             Log(msg.str());
         }
         
         // When all nodes have acknowledged viewing
         if (sv.size() == (m_nNodes - 1)) {
             std::stringstream info;
             info << "Transaction " << txId
                  << " status is conformed and attached to tangle.";
             Log(info.str(), true);
         }
     }
 }
 
 //Check for double spending 
  
 bool IOTAsimulation::IsDoubleSpending(const std::string &txId) {
     return false;
 }
 
 //Get current time as string for logging
  
 std::string IOTAsimulation::NowStr() {
     std::time_t t = std::time(nullptr);
     std::tm* loc = std::localtime(&t);
     char buf[16];
     std::strftime(buf, sizeof(buf), "%H:%M:%S", loc);
     return std::string(buf);
 }
 
 //Log  message with  coloring for real data
  
 void IOTAsimulation::Log(const std::string &s) {
     bool isRealDataLog = (s.find("REAL DATA") != std::string::npos ||
                           s.find("RT-") != std::string::npos);
     std::string greenStart = "\033[1;32m";
     std::string colorEnd = "\033[0m";
     
     if (isRealDataLog) {
         NS_LOG_INFO(greenStart << "[" << NowStr() << "] " << s << colorEnd);
     }
     else {
         NS_LOG_INFO("[" << NowStr() << "] " << s);
     }
 }
 
 // Log a message with optional highlighting
  
 void IOTAsimulation::Log(const std::string &s, bool highlight) {
     if (highlight) {
         std::string greenStart = "\033[1;32m";
         std::string colorEnd = "\033[0m";
         NS_LOG_INFO(greenStart << "[" << NowStr() << "] " << s << colorEnd);
     }
     else {
         NS_LOG_INFO("[" << NowStr() << "] " << s);
     }
 }
 
 //Record propagation delay for a transaction
  
 void IOTAsimulation::RecordPropagationDelay(const std::string &txId, double delay) {
     NS_LOG_UNCOND("Propagation Delay for " + txId + ": " + std::to_string(delay) + " s");
 }
 
 
 //Record acknowledgment time for a transaction
 
 void IOTAsimulation::RecordAckTime(const std::string &txId, double delay) {
     NS_LOG_UNCOND("ACK Time for " + txId + ": " + std::to_string(delay) + " s");
 }
 
 // Print summary of real transactions

 void IOTAsimulation::PrintRealTxSummary() const {
     NS_LOG_UNCOND("\n======= Final Real Transaction Summary =======");
     NS_LOG_UNCOND("Transaction             | Transaction Pick Time | Tangle Attached Time | Delay (s)");
     NS_LOG_UNCOND("---------------------------------------------------------------");
     
     uint32_t realTxCount = 0;
     double totalDelay = 0.0;
     
     // Process each transaction
     for (auto &kv : m_txCreationTime) {
         const std::string &txId = kv.first;
         
         // Only include real transactions
         if (txId.rfind("RT-", 0) == 0) {
             double pickTime = kv.second;
             double attachTime = 0.0;
             
             // Get attach time
             auto it = m_txAttachTime.find(txId);
             if (it != m_txAttachTime.end()) {
                 attachTime = it->second;
             }
             else {
                 double now = Simulator::Now().GetSeconds();
                 attachTime = now;
             }
             
             double delay = attachTime - pickTime;
             std::string pickTimeStr = FormatHms(pickTime);
             std::string attachTimeStr = FormatHms(attachTime);
             
             // Format output line
             std::ostringstream line;
             bool neededMultipleAttempts = false;
             
             // Highlight transactions that needed fallback
             auto fbIt = m_fallbackCount.find(txId);
             if (fbIt != m_fallbackCount.end() && fbIt->second > 0) {
                 neededMultipleAttempts = true;
                 line << "\033[1;31m";
             }
             
             line << std::left << std::setw(23) << txId
                  << " | " << std::setw(20) << pickTimeStr
                  << " | " << std::setw(20) << attachTimeStr
                  << " | " << delay;
                  
             if (neededMultipleAttempts) {
                 line << "\033[0m";
             }
             
             NS_LOG_UNCOND(line.str());
             realTxCount++;
             totalDelay += delay;
         }
     }
     
     // Print summary
     NS_LOG_UNCOND("===============================================================");
     NS_LOG_UNCOND("Total Real Transactions: " << realTxCount);
     NS_LOG_UNCOND("Total Delay (s):         " << totalDelay);
     NS_LOG_UNCOND("===============================================================\n");
 }
 
 //Helper function to install the IOTAsimulation application on nodes
  
 static void InstallIOTAsimulation(NodeContainer &nodes,
                                   Ipv4InterfaceContainer &ifaces,
                                   const std::string &csvFile,
                                   double txRate) {
     uint32_t total = nodes.GetN();
     
     // Install on each node
     for (uint32_t i = 0; i < total; i++) {
         Ptr<IOTAsimulation> app = CreateObject<IOTAsimulation>();
         app->Setup(i, &ifaces, total, csvFile, txRate);
         nodes.Get(i)->AddApplication(app);
         app->SetStartTime(Seconds(0.0));
     }
 }
 
 // Main function
  
 int main(int argc, char** argv) {
     // Record simulation start time for wall-clock formatting
     g_simulationStartTime = std::chrono::system_clock::now();
     
     // Default parameters
     uint32_t nNodes = 10;
     std::string csvFile = "scratch/input.csv";
     double txRate = 1000.0;
     
     // Parse command line arguments
     CommandLine cmd;
     cmd.AddValue("nNodes", "Number of nodes", nNodes);
     cmd.AddValue("csvFile", "CSV file path", csvFile);
     cmd.AddValue("txRate", "Transaction rate (Tx/s)", txRate);
     cmd.Parse(argc, argv);
     
     // Create nodes
     NodeContainer nodes;
     nodes.Create(nNodes);
     
     // Setup mobility model
     MobilityHelper mob;
     mob.SetPositionAllocator("ns3::RandomDiscPositionAllocator",
                              "X", StringValue("100.0"),
                              "Y", StringValue("100.0"),
                              "Rho", StringValue("ns3::UniformRandomVariable[Min=0|Max=30]"));
     mob.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                          "Bounds", RectangleValue(Rectangle(0, 200, 0, 200)),
                          "Time", TimeValue(Seconds(1.0)),
                          "Speed", StringValue("ns3::UniformRandomVariable[Min=1|Max=5]"));
     mob.Install(nodes);
     
     // Setup WiFi network
     WifiHelper wifi;
     wifi.SetStandard(WIFI_STANDARD_80211b);
     
     YansWifiChannelHelper chan = YansWifiChannelHelper::Default();
     YansWifiPhyHelper phy;
     phy.SetChannel(chan.Create());
     phy.Set("TxPowerStart", DoubleValue(20.0));
     phy.Set("TxPowerEnd", DoubleValue(20.0));
     
     WifiMacHelper mac;
     mac.SetType("ns3::AdhocWifiMac");
     
     NetDeviceContainer devs = wifi.Install(phy, mac, nodes);
     
     // Setup internet stack and addressing
     InternetStackHelper inet;
     inet.Install(nodes);
     
     Ipv4AddressHelper ipv4;
     ipv4.SetBase("10.1.1.0", "255.255.255.0");
     Ipv4InterfaceContainer ifaces = ipv4.Assign(devs);
     
     // Enable packet capture
     phy.EnablePcapAll("iota");
     
     // Install the IOTA simulation application
     InstallIOTAsimulation(nodes, ifaces, csvFile, txRate);
     
     // Setup animation
     AnimationInterface anim("iota-animation.xml");
     for (uint32_t i = 0; i < nNodes; i++) {
         std::ostringstream d;
         d << "Node-" << i;
         anim.UpdateNodeDescription(nodes.Get(i), d.str());
         anim.UpdateNodeColor(nodes.Get(i), 255/(i+1), 100+(15*i), 50+(20*i));
     }
     anim.SetMobilityPollInterval(Seconds(1.0));
     anim.EnablePacketMetadata(true);
     
     // Setup flow monitoring
     FlowMonitorHelper fm;
     Ptr<FlowMonitor> flowmon = fm.InstallAll();
     
     // Enable logging
     LogComponentEnable("IOTAsimulation", LOG_LEVEL_INFO);
     
     // Run simulation
     Simulator::Stop(Seconds(1500.0));
     Simulator::Run();
     
     // Process flow statistics
     flowmon->CheckForLostPackets();
     Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fm.GetClassifier());
     if (classifier) {
         auto stats = flowmon->GetFlowStats();
         for (auto &fl: stats) {
             Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(fl.first);
             NS_LOG_UNCOND("\nFlow ID: " << fl.first
                           << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")");
             NS_LOG_UNCOND("  Tx Packets:   " << fl.second.txPackets);
             NS_LOG_UNCOND("  Rx Packets:   " << fl.second.rxPackets);
             NS_LOG_UNCOND("  Lost Packets: " << fl.second.lostPackets);
             
             double dur = (fl.second.timeLastRxPacket.GetSeconds() -
                           fl.second.timeFirstTxPacket.GetSeconds());
             double thr = 0.0;
             if (dur > 0 && fl.second.rxBytes > 0) {
                 thr = (fl.second.rxBytes * 8.0 / dur / 1000);
             }
             NS_LOG_UNCOND("  Throughput:   " << thr << " Kbps");
             
             if (fl.second.rxPackets > 0) {
                 double avgDelay = fl.second.delaySum.GetSeconds() / fl.second.rxPackets;
                 NS_LOG_UNCOND("  Avg Delay:    " << avgDelay << " s");
             }
             
             if (fl.second.rxPackets > 1) {
                 double avgJitter = fl.second.jitterSum.GetSeconds() / (fl.second.rxPackets - 1);
                 NS_LOG_UNCOND("  Avg Jitter:   " << avgJitter << " s");
             }
         }
     }
     
     // Print transaction summary from node 0
     {
         Ptr<IOTAsimulation> node0App = DynamicCast<IOTAsimulation>(nodes.Get(0)->GetApplication(0));
         if (node0App) {
             node0App->PrintRealTxSummary();
         }
     }
     
     // Save flow monitor results
     flowmon->SerializeToXmlFile("flowmon-results.xml", true, true);
     
     // Cleanup
     Simulator::Destroy();
     return 0;
 }
