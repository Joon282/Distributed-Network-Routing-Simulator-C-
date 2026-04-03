1. Environment Details

This program router.cpp was developed and test in the Environment.
OS - Developed on Windows 11 OS, Test in Linux (Ubuntu)
Architecture - x86-64
Compiler - g++ (GNU Compiler)
Compiler - g++ 13.2.0
C++ Standard - C++ 17

2. Libraries Required
The program uses standard system libraries and POSIX C/C++ API
// C++ Standard Library
#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

3. Compilation Instructions.
To Compile the program, run:

g++ -std=c++17 -pthread -o router router.cpp

4. Run the program using the Routing.sh

./Routing.sh <Node-ID> <Port-NO> <Node-Config-File> <RoutingDelay> <UpdateInterval>

Example: ./Routing.sh A 6000 configA.txt 1.0 5.0

Configuration Files must follow the format:

<N>
<Neighbour ID> <Cost> <Port>

5. Simulator a network by running multiple instances of the program in separate terminals
./Routing.sh A 6000 configA.txt 1.0 5.0 
./Routing.sh B 6001 configB.txt 1.0 5.0 
./Routing.sh C 6002 configC.txt 1.0 5.0

6. Dynamic Commands

Network Control

CHANGE <Node> <Cost>
FAIL <Node>
RECOVER <Node>
RESET

Queries
QUERY <Destination>
QUERY PATH <Source> <Destination>

Batch Processing
BATCH UPDATE <filename>

Graph Operations
MERGE <Node1> <Node2>
CYCLE DETECT








