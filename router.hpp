#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <thread>

class Router {
public:
    Router(const std::string &nodeID, int port, const std::string &configPath,
           double routingDelay, double updateInterval);
    ~Router();

    void start();

private:
    std::string nodeID;
    int port;
    std::string configPath;
    double routingDelay;
    double updateInterval;
    std::atomic<bool> alive;
    std::atomic<bool> shutdownFlag;
    std::atomic<bool> readyToBroadcast;
    std::unordered_set<std::string> failedNodes;
    std::unordered_map<std::string, std::unordered_map<std::string, std::pair<double,int>>> adjacencyList;
    std::unordered_map<std::string, std::pair<double,int>> directNeighbours;
    std::unordered_map<std::string, std::pair<double,int>> originalNeighbours;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> graph;
    std::unordered_map<std::string, std::pair<double, std::vector<std::string>>> routingTable;
    std::string lastBroadcast;
    int udpSocket;
    std::mutex mutex;
    std::condition_variable_any routeCv;
    bool routeNeeded;
    std::vector<std::thread> threads;
    std::deque<std::string> commandQueue;
    std::mutex cmdMutex;
    std::condition_variable cmdCv;
    void log(const std::string &msg);
    void error(const std::string &msg);
    void loadConfig(bool initial);
    void loadConfig_locked(bool initial);
    void getGraph();
    std::string asUpdatePacket(const std::string &node);
    void sendUpdate(bool force);
    void parseUpdate(const std::string &packet);
    std::pair<std::unordered_map<std::string, double>, std::unordered_map<std::string, std::string>> dijkstra(const std::string &start);
    std::vector<std::string> pathFromPrev(const std::string &start, const std::string &destination, const std::unordered_map<std::string, std::string> &previousNode);
    void printRoutingTable(bool forcePrint = false);
    void printRoutingTable_locked(bool forcePrint = false);
    void routingThread();
    void sendingThread();
    void listeningThreadStdin();
    void listeningThreadSocket();
    void processCommand(const std::string &line);
    void enqueueCommand(const std::string &line);
    void applyMerge(const std::string &nodeID1, const std::string &nodeID2);
    std::unordered_set<std::string> getComponent(const std::string &start);
    void applySplit();
    bool detectCycle();
    bool depthFirstSearch(const std::string &current, const std::string &parent, std::unordered_set<std::string> &visited);
};
