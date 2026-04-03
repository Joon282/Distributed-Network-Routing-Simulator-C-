#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include "router.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>


Router::Router(const std::string &nodeID, int port, const std::string &configPath, double routingDelay, double updateInterval){
    this->nodeID = nodeID;
    this->port = port;
    this->configPath = configPath;
    this->routingDelay = routingDelay;
    this->updateInterval = updateInterval;
    this->alive = true;
    this->shutdownFlag = false;
    this->routeNeeded = false;
    this->readyToBroadcast = false;
    this->udpSocket = -1;
    //std::cout << "starting node " << nodeID << " on port " << port << std::endl;
    loadConfig(true);
}

Router::~Router() {
    //std::cout << "shutting down " << nodeID << std::endl;
    shutdownFlag = true;
    if (udpSocket >= 0) {
        close(udpSocket);

        udpSocket = -1;
    }
    cmdCv.notify_all();
    for (auto &t : threads) {
        if (t.joinable()) t.join();
    }
}

void Router::log(const std::string &msg) {
    std::cout << msg << std::endl;
}

void Router::error(const std::string &msg) {
    std::cerr << msg << std::endl;
}

void Router::loadConfig(bool initial) {
    std::lock_guard<std::mutex> lock(mutex);
    loadConfig_locked(initial);
}

void Router::loadConfig_locked(bool initial) {
    //std::cout << "reading config: " << configPath << std::endl;
    std::ifstream ifs(configPath);
    if (!ifs.is_open()) {
        throw std::runtime_error("Configuration file " + configPath + " not found.");
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string stripped;
        for (char c : line) {
            if (!std::isspace((unsigned char)c)) {
                stripped.push_back(c);
            }
        }
        if (!stripped.empty()) {
            std::stringstream ss(line);
            std::string token;
            std::string assembled;
            bool first = true;
            while (ss >> token) {
                if (!first) {
                    assembled += " ";
                }
                assembled += token;
                first = false;
            }
            lines.push_back(assembled);
        }
    }

    if (lines.empty()) {
        throw std::runtime_error("Invalid configuration file format. (First line must be an integer.)");
    }

    int n;
    try {
        n = std::stoi(lines[0]);
    } catch (...) {
        throw std::runtime_error("Invalid configuration file format. (First line must be an integer.)");
    }

    if (n != (int)lines.size() - 1) {
        throw std::runtime_error("Invalid configuration file format. (Neighbour count mismatch.)");
    }

    std::unordered_map<std::string, std::pair<double, int>> neighboursInfo;
    for (int i = 1; i <= n; ++i) {
        std::istringstream iss(lines[i]);
        std::string neighbourID;
        std::string costString;
        std::string portString;
        if (!(iss >> neighbourID >> costString >> portString)) {
            throw std::runtime_error("Invalid configuration file format. (Each neighbour entry must have exactly three tokens; cost must be numeric.)");
        }
        if (neighbourID == nodeID) {
            continue;
        }
        double cost;
        int portVal;
        try {
            cost = std::stod(costString);
            portVal = std::stoi(portString);
        } catch (...) {
            throw std::runtime_error("Invalid configuration file format. (Each neighbour entry must have exactly three tokens; cost must be numeric.)");
        }
        //std::cout << "  neighbour " << neighbourID << " cost=" << cost << " port=" << portVal << std::endl;
        neighboursInfo[neighbourID] = std::make_pair(cost, portVal);
    }
    directNeighbours.clear();
    adjacencyList.clear();
    for (auto &it : neighboursInfo) {
        directNeighbours[it.first] = it.second;
    }
    if (initial) {
        originalNeighbours = directNeighbours;
        failedNodes.clear();
    }
    std::unordered_map<std::string, std::pair<double, int>> nodeAdj;
    for (auto &it : neighboursInfo) {
        nodeAdj[it.first] = it.second;
    }
    adjacencyList[nodeID] = nodeAdj;

    getGraph();
}

void Router::getGraph() {
    graph.clear();
    for (const auto &it : adjacencyList) {
        const std::string &node = it.first;
        if (failedNodes.count(node)){
            continue;
        } 
        for (const auto &it2 : it.second) {
            const std::string &neighbour = it2.first;
            double cost = it2.second.first;
            if (failedNodes.count(neighbour)) {
                continue;
            }
            graph[node][neighbour] = cost;
            graph[neighbour][node] = cost;
        }
    }
    //std::cout << "graph rebuilt, " << graph.size() << " nodes" << std::endl;
}

std::string Router::asUpdatePacket(const std::string &node) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = adjacencyList.find(node);
    if (it == adjacencyList.end()) {
        return std::string();
    }
    std::vector<std::string> keys;
    for (auto const &it2 : it->second) {
        keys.push_back(it2.first);
    }
    std::sort(keys.begin(), keys.end());
    std::ostringstream out;
    out << "UPDATE " << node;
    if (!keys.empty()) {
        out << " ";
        bool first = true;
        for (auto const &key : keys) {
            auto info = it->second.at(key);
            if (!first) {
                out << ",";
            }
            out << std::fixed << std::setprecision(1);
            out << key << ":" << info.first << ":" << info.second;
            first = false;
        }
    }
    return out.str();
}

void Router::sendUpdate(bool force) {
    if (!alive.load()) {
        return;
    }
    std::string packet = asUpdatePacket(nodeID);
    if (packet.empty()) {
        return;
    }
    if (!force && packet == lastBroadcast) {
        return;
    }
    lastBroadcast = packet;
    log(packet);
    std::lock_guard<std::mutex> lock(mutex);
    for (auto const &it : directNeighbours) {
        const std::string &neighbour = it.first;
        if (failedNodes.count(neighbour)) {
            continue;
        }
        if (!alive.load()) {
            continue;
        }
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(it.second.second);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ssize_t sent = sendto(udpSocket, packet.c_str(), packet.size(), 0,
            (struct sockaddr *)&addr, sizeof(addr));
        //std::cout << "sent " << sent << " bytes to " << neighbour << std::endl;

    }
}

void Router::parseUpdate(const std::string &packetRaw) {
    std::string packet = packetRaw;
    while (!packet.empty() && std::isspace((unsigned char)packet.back())) packet.pop_back();
    if (packet.size() < 7 || packet.rfind("UPDATE ", 0) != 0) {
        error("Error: Invalid update packet format.");
        return;
    }
    std::string rest = packet.substr(7);
    std::string source;
    std::string neighboursString;
    std::istringstream iss(rest);
    if (!(iss >> source)) {
        error("Error: Invalid update packet.");
        return;
    }
    std::getline(iss, neighboursString);
    if (!neighboursString.empty()) {
        size_t pos = neighboursString.find_first_not_of(' ');
        if (pos != std::string::npos) neighboursString = neighboursString.substr(pos);
        else neighboursString.clear();
    }
    //std::cout << "got update from " << source << std::endl;
    std::unordered_map<std::string, std::pair<double,int>> neighbours;
    if (!neighboursString.empty()) {
        std::istringstream parts(neighboursString);
        std::string item;
        size_t start = 0;
        while (start < neighboursString.size()) {
            size_t comma = neighboursString.find(',', start);
            if (comma == std::string::npos) comma = neighboursString.size();
            item = neighboursString.substr(start, comma - start);
            if (!item.empty()) {
                size_t p1 = item.find(':');
                size_t p2 = (p1==std::string::npos) ? std::string::npos : item.find(':', p1+1);
                if (p1 == std::string::npos || p2 == std::string::npos) {
                    error("Error: Invalid update packet format.");
                    return;
                }
                std::string nid = item.substr(0,p1);
                std::string costStr = item.substr(p1+1, p2-p1-1);
                std::string portStr = item.substr(p2+1);
                try {
                    double cost = std::stod(costStr);
                    int portVal = std::stoi(portStr);
                    neighbours[nid] = std::make_pair(cost, portVal);
                } catch (...) {
                    error("Error: Invalid update packet format.");
                    return;
                }
            }
            start = comma + 1;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool changed = false;
        auto existingIt = adjacencyList.find(source);
        if (existingIt == adjacencyList.end() || existingIt->second != neighbours) {
            adjacencyList[source] = neighbours;
            changed = true;
        }
        auto it = neighbours.find(nodeID);
        if (it != neighbours.end()) {
            if (directNeighbours.count(source)) {
                double oldCost = directNeighbours[source].first;
                double newCost = it->second.first;
                directNeighbours[source].first = newCost;
                adjacencyList[nodeID][source].first = newCost;
                if (oldCost != newCost) changed = true;
            }
        }
        if (changed) {
            getGraph();
            routeNeeded = true;
        }
    }
    if (routeNeeded) cmdCv.notify_one();
}

std::pair<std::unordered_map<std::string, double>, std::unordered_map<std::string, std::string>> Router::dijkstra(const std::string &start) {
    std::vector<std::string> allNodes;
    allNodes.reserve(graph.size());
    for (const auto &p : graph) {
        allNodes.push_back(p.first);
    }
    std::unordered_map<std::string, double> dist;
    std::unordered_map<std::string, std::string> prevNode;
    for (const auto &node : allNodes) {
        dist[node] = std::numeric_limits<double>::infinity();
        prevNode[node] = "";
    }
    if (graph.count(start) == 0) {
        return {dist, prevNode};
    }
    dist[start] = 0.0;
    std::priority_queue<std::pair<double, std::string>, std::vector<std::pair<double, std::string>>, std::greater<std::pair<double, std::string>>> pq;
    pq.push(std::make_pair(0.0, start));
    while (!pq.empty()) {
        auto [currentDist, currentNode] = pq.top();
        pq.pop();
        if (currentDist > dist[currentNode]) {
            continue;
        }
        for (const auto &neighbour : graph[currentNode]) {
            double cost = currentDist + neighbour.second;
            if (cost < dist[neighbour.first]) {
                dist[neighbour.first] = cost;
                prevNode[neighbour.first] = currentNode;
                pq.push({cost, neighbour.first});
            }
        }
    }

    return {dist, prevNode};
}

std::vector<std::string> Router::pathFromPrev(const std::string &start, const std::string &dest, const std::unordered_map<std::string, std::string> &prev) {
    if (dest == start) {
        return {start};
    } 
    if (!prev.count(dest) || prev.at(dest).empty()) {
        return {};
    }
    std::vector<std::string> path;
    std::string current = dest;
    while (current != start) {
        if (current.empty()) return {};
        path.push_back(current);
        auto it = prev.find(current);
        if (it == prev.end()) return {};
        current = it->second;
    }
    path.push_back(start);
    std::reverse(path.begin(), path.end());
    return path;
}

static std::string formatCost(double cost) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << cost;
    return oss.str();
}

void Router::printRoutingTable(bool resetPrint) {
    std::lock_guard<std::mutex> lock(mutex);
    printRoutingTable_locked(resetPrint);
}

void Router::printRoutingTable_locked(bool resetPrint) {
    getGraph();
    std::unordered_map<std::string, std::pair<double, std::vector<std::string>>> newRoutingTable;
    auto [dist, prev] = dijkstra(nodeID);
    std::unordered_set<std::string> allNodes;
    for (auto &it : graph) {
        allNodes.insert(it.first);
    }
    for (auto &it : dist) {
        allNodes.insert(it.first);
    }
    for (auto &dest : allNodes) {
        if (dest == nodeID) {
            continue;
        }
        auto it = dist.find(dest);
        if (it == dist.end() || it->second == std::numeric_limits<double>::infinity()) {
            continue;
        }
        auto path = pathFromPrev(nodeID, dest, prev);
        if (path.empty()) {
            continue;
        }
        newRoutingTable[dest] = std::make_pair(it->second, path);
    }
    bool checkChanged = resetPrint || (newRoutingTable.size() != routingTable.size());
    if (!checkChanged){
        for (auto& it : newRoutingTable){
            auto it2 = routingTable.find(it.first);
            if (it2 == routingTable.end() || it2->second.first != it.second.first || it2->second.second != it.second.second){
                checkChanged = true;
                break;
            }
        }
    }
    if (!checkChanged){
        routeNeeded = false;
        return;
    }
    routeNeeded = false;
    routingTable = std::move(newRoutingTable);
    std::vector<std::string> destinations;
    destinations.reserve(routingTable.size());
    for (auto &entry : routingTable) {
        destinations.push_back(entry.first);
    }
    std::sort(destinations.begin(), destinations.end());

    for (auto &dest : destinations) {
        const auto &entry = routingTable.at(dest);
        auto cost = entry.first;
        const auto &it = entry.second;
        std::string pathstr;
        for (auto const &node : it) {
            pathstr += node;
        }
        log("Least cost path from " + nodeID + " to " + dest + ": " + pathstr + ", link cost: " + formatCost(cost));
    }
}

void Router::routingThread() {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration<double>(routingDelay * 3);
    while (std::chrono::steady_clock::now() < deadline && !shutdownFlag.load()) {
        bool allSeen = true;
        {
            std::lock_guard<std::mutex> lk(mutex);
            for (auto const &nb : directNeighbours) {
                if (!adjacencyList.count(nb.first)) { allSeen = false; break; }
            }
        }
        if (allSeen) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    while (!shutdownFlag.load()) {
        std::deque<std::string> deferred;
        bool processedAny = false;
        {
            std::unique_lock<std::mutex> lock2(cmdMutex);
            cmdCv.wait_for(lock2,
                std::chrono::duration<double>(routingDelay),
                [this] { return !commandQueue.empty() || shutdownFlag.load(); });
            while (!commandQueue.empty()) {
                std::string cmd = std::move(commandQueue.front());
                commandQueue.pop_front();
                if (cmd.rfind("UPDATE", 0) == 0) {
                    processedAny = true;
                    lock2.unlock();
                    parseUpdate(cmd);
                    lock2.lock();
                } else if(cmd.rfind("CHANGE", 0) == 0) {
                    std::istringstream iss (cmd);
                    std::vector<std::string> vec;
                    std::string t;
                    while (iss >> t){
                        vec.push_back(t);
                    }
                    if (vec.size() == 3){
                        processedAny = true;
                        lock2.unlock();
                        processCommand(cmd);
                        lock2.lock();
                    }else {
                        deferred.push_back(std::move(cmd));
                    }
                }else {
                    deferred.push_back(std::move(cmd));
                }
            }
            for (auto it = deferred.rbegin(); it != deferred.rend(); ++it)
                commandQueue.push_front(std::move(*it));
        }
        if (!processedAny) break;
    }
    printRoutingTable(true);
    {
        std::lock_guard<std::mutex> lk(mutex);
        routeNeeded = false;
    }
    readyToBroadcast = true;
    sendUpdate(true);

    while (!shutdownFlag.load()) {
        std::unique_lock<std::mutex> lock2(cmdMutex);
        if (!cmdCv.wait_for(lock2, std::chrono::seconds(1),
                [this] { return !commandQueue.empty() || shutdownFlag.load(); })) {
            lock2.unlock();
            {
                std::lock_guard<std::mutex> lk(mutex);
                if (!routeNeeded) continue;
            }
            printRoutingTable();
            {
                std::lock_guard<std::mutex> lk(mutex);
                routeNeeded = false;
            }
            continue;
        }
        if (shutdownFlag.load()) break;
        while (!commandQueue.empty()) {
            std::string cmd = std::move(commandQueue.front());
            commandQueue.pop_front();
            lock2.unlock();
            if (cmd.rfind("UPDATE", 0) == 0) parseUpdate(cmd);
            else processCommand(cmd);
            lock2.lock();
        }
        lock2.unlock();
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (!routeNeeded) continue;
        }
        printRoutingTable();
        {
            std::lock_guard<std::mutex> lk(mutex);
            routeNeeded = false;
        }
    }
}

void Router::sendingThread() {
    while (!shutdownFlag.load()) {
        std::this_thread::sleep_for(std::chrono::duration<double>(updateInterval));
        if (readyToBroadcast.load()) sendUpdate(false);
    }
}

void Router::listeningThreadStdin() {
    std::string line;
    while (!shutdownFlag.load() && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        //std::cout << "stdin: " << line << std::endl;
        enqueueCommand(line);
    }
}

void Router::listeningThreadSocket() {
    //std::cout << "socket listener ready on port " << port << std::endl;
    while (!shutdownFlag.load()) {
        char buffer[65536];
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);

        ssize_t received = recvfrom(udpSocket, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&addr, &addrlen);
        if (received < 0) {

            if (shutdownFlag.load()) {
                break;
            }
            std::cerr << "recvfrom error: " << errno << std::endl;
            continue;
        }
        buffer[received] = '\0';
        std::string message(buffer);
        //std::cout << "received packet: " << message << std::endl;
        enqueueCommand(message);
    }
}

void Router::enqueueCommand(const std::string &line) {
    {
        std::lock_guard<std::mutex> lk(cmdMutex);
        commandQueue.push_back(line);
    }
    cmdCv.notify_one();
}

void Router::processCommand(const std::string &lineRaw) {
    std::string line = lineRaw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    std::istringstream iss(line);
    std::vector<std::string> parts;
    std::string token;
    while (iss >> token) {
        parts.push_back(token);
    }
    if (parts.empty()) {
        return;
    }
    auto isValidNodeID = [](const std::string &s) {
        return s.size() == 1 && std::isalpha((unsigned char)s[0]) && std::isupper((unsigned char)s[0]);
    };
    std::string cmd = parts[0];
    for (auto &c : cmd) {
        c = std::toupper((unsigned char)c);
    }
    try {
        if (cmd == "CHANGE") {
            if (parts.size() < 3) {
                error("Error: Invalid command format. Expected numeric cost value.");
                return;
            }
            if (parts.size() > 3) {
                error("Error: Invalid command format. Expected exactly two tokens after CHANGE.");
                return;
            }
            std::string neighbour = parts[1];
            double newCost;
            try {
                newCost = std::stod(parts[2]);
            } catch (...) {
                error("Error: Invalid command format. Expected numeric cost value.");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!directNeighbours.count(neighbour)) {
                    error("Error: Neighbour " + neighbour + " not found.");
                    return;
                }
                double oldCost = directNeighbours[neighbour].first;
                directNeighbours[neighbour].first = newCost;
                adjacencyList[nodeID][neighbour].first = newCost;
                //std::cout << "edge " << nodeID << "-" << neighbour << " changed: " << oldCost << " -> " << newCost << std::endl;
                getGraph();
                routeNeeded = true;
            }
            cmdCv.notify_one();
            sendUpdate(true);
        } else if (cmd == "FAIL") {
            if (parts.size() < 2) {
                error("Error: Invalid command format. Expected: FAIL <Node-ID>.");
                return;
            }
            if (parts.size() != 2 || !isValidNodeID(parts[1])) {
                error("Error: Invalid command format. Expected a valid Node-ID.");
                return;
            }
            std::string target = parts[1];
            //std::cout << "marking " << target << " as failed" << std::endl;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (target == nodeID) {
                    alive = false;
                    failedNodes.insert(nodeID);
                    log("Node " + nodeID + " is now DOWN.");
                } else {
                    failedNodes.insert(target);
                    getGraph();
                }
            }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    routeNeeded = true;
                }
                cmdCv.notify_one();
        } else if (cmd == "RECOVER") {
            if (parts.size() != 2 || !isValidNodeID(parts[1])) {
                error("Error: Invalid command format. Expected a valid Node-ID.");
                return;
            }
            std::string target = parts[1];
            //std::cout << "recovering " << target << std::endl;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (target == nodeID) {
                    alive = true;
                    failedNodes.erase(nodeID);
                    log("Node " + nodeID + " is now UP.");
                    routeNeeded = true;
                } else {
                    failedNodes.erase(target);
                    getGraph();
                    routeNeeded = true;
                }
            }
            cmdCv.notify_one();
            sendUpdate(true);
        } else if (cmd == "QUERY") {
            if (parts.size() >= 2 && parts[1] == "PATH") {
                if (parts.size() != 4) {
                    error("Error: Invalid command format. Expected two valid identifiers for Source and Destination.");
                    return;
                }
                std::string sourceNode = parts[2];
                std::string destinationNode = parts[3];
                if (!isValidNodeID(sourceNode) || !isValidNodeID(destinationNode)) {
                    error("Error: Invalid command format. Expected two valid identifiers for Source and Destination.");
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex);
                getGraph();
                auto [distances, previousNodes] = dijkstra(sourceNode);
                if (!distances.count(destinationNode) || distances[destinationNode] == std::numeric_limits<double>::infinity()) {
                    log("Least cost path from " + sourceNode + " to " + destinationNode + ": , link cost: inf");
                } else {
                    auto routePath = pathFromPrev(sourceNode, destinationNode, previousNodes);
                    if (routePath.empty()) {
                        log("Least cost path from " + sourceNode + " to " + destinationNode + ": , link cost: inf");
                    } else {
                        std::string pathString;
                        for (auto const &neighbourNode : routePath) {
                            pathString += neighbourNode;
                        }
                        log("Least cost path from " + sourceNode + " to " + destinationNode + ": " + pathString + ", link cost: " + formatCost(distances[destinationNode]));
                    }
                }
            } else if (parts.size() == 2) {
                std::string dest = parts[1];
                if (!isValidNodeID(dest)) {
                    error("Error: Invalid command format. Expected a valid Destination.");
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex);
                printRoutingTable_locked();
                auto it = routingTable.find(dest);
                if (it == routingTable.end()) {
                    log("Least cost path from " + nodeID + " to " + dest + ": , link cost: inf");
                } else {
                    double cost = it->second.first;
                    const auto &path = it->second.second;
                    std::string pathstr;
                    for (auto const &node : path) {
                        pathstr += node;
                    }
                    log("Least cost path from " + nodeID + " to " + dest + ": " + pathstr + ", link cost: " + formatCost(cost));
                }
            } else {
                error("Error: Invalid command format. Expected a valid Destination.");
                return;
            }
        } else if (cmd == "RESET") {
            if (parts.size() != 1){
                error("Error: Invalid command format. Expected exactly: RESET.");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                loadConfig_locked(false);
                alive = true;
                failedNodes.erase(nodeID);
                routeNeeded = false;
                log("Node " + nodeID + " has been reset.");
                log("I am Node " + nodeID);
                printRoutingTable_locked(true);
            }
            sendUpdate(false);
        } else if (cmd == "BATCH") {
            if (parts.size() != 3 || parts[1] != "UPDATE") {
                error("Error: Invalid command format. Expected: BATCH UPDATE <Filename>.");
                return;
            }
            std::string filename = parts[2];
            //std::cout << "batch file: " << filename << std::endl;
            std::ifstream f(filename);
            if (!f.is_open()) {
                error("Error: Batch file " + filename + " not found.");
                return;
            }
            std::string line2;
            while (std::getline(f, line2)) {
                if (line2.empty()) {
                    continue;
                }
                processCommand(line2);
            }
            log("Batch update complete.");
            routeNeeded = true;
            cmdCv.notify_one();
        } else if (cmd == "MERGE") {
            if (parts.size() != 3) {
                error("Error: Invalid command format. Expected two valid identifiers for MERGE.");
                return;
            }
            std::string nodeID1 = parts[1];
            std::string nodeID2 = parts[2];
            if (!isValidNodeID(nodeID1) || !isValidNodeID(nodeID2)) {
                error("Error: Invalid command format. Expected two valid identifiers for MERGE.");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                applyMerge(nodeID1, nodeID2);
            }
            log("Graph merged successfully.");
            routeNeeded = true;
            cmdCv.notify_one();
        } else if (cmd == "SPLIT") {
            if (parts.size() != 1) {
                error("Error: Invalid command format. Expected exactly: SPLIT.");
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            applySplit();
            log("Graph partitioned successfully.");
            routeNeeded = true;
            cmdCv.notify_one();
        } else if (cmd == "CYCLE") {
            if (parts.size() >= 2 && parts[1] == "DETECT") {
                if (parts.size() != 2) {
                    error("Error: Invalid command format. Expected exactly: CYCLE DETECT.");
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex);
                getGraph();
                if (detectCycle()) {
                    log("Cycle detected.");
                }
                else log("No cycle found.");
            } else {
                error("Error: Invalid command format.");
                return;
            }
        } else {
            error("Error: Invalid command format.");
        }
    } catch (const std::exception &ex) {
        error(std::string("Error processing command: ") + ex.what());
    }
}

void Router::applyMerge(const std::string &nodeID1, const std::string &nodeID2) {
    for (const auto& node : adjacencyList[nodeID2]){
        const std::string neighbour = node.first;
        double cost = node.second.first;
        int port = node.second.second;

        if (neighbour == nodeID1){
            continue;
        }
        auto it = adjacencyList[nodeID1].find(neighbour);
        if (it != adjacencyList[nodeID1].end()){
            if (cost < it->second.first){
                it->second = {cost, port}; 
            }
        } else {
            adjacencyList[nodeID1][neighbour] = {cost, port};
        }
    }

    for (auto& node : adjacencyList){
        if (node.first == nodeID2){
            auto it = adjacencyList[nodeID2].find(node.first);
            double cost = it->second.first;
            int port = it->second.second;
            if (node.first == nodeID1){
                auto it2 = adjacencyList[nodeID2].find(node.first);
                double cost2 = it2->second.first;
                if (cost < cost2){
                    it2->second = {cost, port};
                }
            }else {
                node.second[nodeID1] = {cost, port};
            }
            node.second.erase(it);
        } 
    }
    adjacencyList.erase(nodeID2);
    getGraph();
}

std::unordered_set<std::string> Router::getComponent(const std::string &start) {
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack{start};
    while (!stack.empty()) {
        std::string u = stack.back();
        stack.pop_back();
        if (visited.count(u)) {
            continue;
        }
        visited.insert(u);
        for (auto &it : graph[u]) {
            if (!visited.count(it.first)) {
                stack.push_back(it.first);
            }
        }
    }
    return visited;
}

void Router::applySplit() {
   return;
}

bool Router::depthFirstSearch(const std::string &current, const std::string &parent, std::unordered_set<std::string> &visited) {
    visited.insert(current);
    for (auto &neighbour : graph[current]) {
        const std::string &next = neighbour.first;

        if (next == parent) {
            continue;
        }
        if (visited.count(next)) {
            //std::cout << "cycle: back edge " << current << " -> " << next << std::endl;
            return true; 
        }
        if (depthFirstSearch(next, current, visited)) {
            return true; 
        }
    }
    return false;
}

bool Router::detectCycle() {
    std::unordered_set<std::string> visited;
    for (auto &it : graph) {
        const std::string &node = it.first;
        if (visited.count(node)) {
            continue;
        }
        if (depthFirstSearch(node, "", visited)) {
            return true;
        }
    }
    return false;
}

void Router::start() {
    log("I am Node " + nodeID);
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocket < 0) {
        error("Error: Unable to create UDP socket.");
        std::exit(1);
    }

    int opt = 1;
    setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(udpSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));


    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);


    if (bind(udpSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error("Error: Unable to bind UDP socket on port " + std::to_string(port));
        std::exit(1);
    }
    //std::cout << "bound to port " << port << std::endl;

    threads.emplace_back(&Router::routingThread, this);
    threads.emplace_back(&Router::sendingThread, this);
    threads.emplace_back(&Router::listeningThreadStdin, this);
    threads.emplace_back(&Router::listeningThreadSocket, this);

    while (!shutdownFlag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        std::cerr << "Error: Insufficient arguments provided.\nUsage: ./Routing.sh <Node-ID> <Port-NO> <Node-Config-File> <RoutingDelay> <UpdateInterval>\n";
        return 1;
    }

    std::string nodeID = argv[1];
    if (nodeID.size() != 1 || !std::isalpha(nodeID[0]) || !std::isupper(nodeID[0])) {
        std::cerr << "Error: Invalid Node-ID.\n";
        return 1;
    }

    int port;
    try {
        port = std::stoi(argv[2]);
    } catch (...) {
        std::cerr << "Error: Invalid Port number. Must be an integer.\n";
        return 1;
    }

    std::string configFile = argv[3];
    std::ifstream check(configFile);
    if (!check.is_open()) {
        std::cerr << "Error: Configuration file " << configFile << " not found.\n";
        return 1;
    }

    double routingDelay;
    double updateInterval;
    try {
        routingDelay = std::stod(argv[4]);
        updateInterval = std::stod(argv[5]);
    } catch (...) {
        std::cerr << "Error: Invalid numeric arguments for delays.\n";
        return 1;
    }

    try {
        Router router(nodeID, port, configFile, routingDelay, updateInterval);
        router.start();
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
