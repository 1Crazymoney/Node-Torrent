#include "WorkerNodeTest.h"

#include <numeric>

#include "check.h"
#include "utils/utils.h"
#include "log.h"
#include "convertStrings.h"

#include "BlockChain.h"

#include "BlockchainRead.h"

#include "NodeTestsBlockInfo.h"

#include "jsonUtils.h"

#include "utils/FileSystem.h"
#include "blockchain_structs/TransactionInfo.h"

using namespace common;

namespace torrent_node_lib {
    
static uint64_t findAvg(const std::vector<uint64_t> &numbers) {
    CHECK(!numbers.empty(), "Empty numbers");
    return std::accumulate(numbers.begin(), numbers.end(), 0) / numbers.size();
}
    
WorkerNodeTest::WorkerNodeTest(const BlockChain &blockchain, const std::string &folderBlocks, const LevelDbOptions &leveldbOptNodeTest) 
    : blockchain(blockchain)
    , folderBlocks(folderBlocks)
    , leveldbNodeTest(leveldbOptNodeTest.writeBufSizeMb, leveldbOptNodeTest.isBloomFilter, leveldbOptNodeTest.isChecks, leveldbOptNodeTest.folderName, leveldbOptNodeTest.lruCacheMb)
{
    const NodeStatBlockInfo lastScriptBlock = leveldbNodeTest.findNodeStatBlock();
    initializeScriptBlockNumber = lastScriptBlock.blockNumber;
}

WorkerNodeTest::~WorkerNodeTest() {
    try {
        thread.join();        
    } catch (...) {
        LOGERR << "Error while stoped thread";
    }
}

void WorkerNodeTest::join() {
    thread.join();
}

std::optional<NodeTestResult> parseTestNodeTransaction(const TransactionInfo &tx) {
    rapidjson::Document doc;
    const rapidjson::ParseResult pr = doc.Parse((const char*)(tx.data.data()), tx.data.size());
    CHECK(pr, "rapidjson parse error. Data: " + std::string(tx.data.begin(), tx.data.end()));
    
    if (doc.HasMember("method") && doc["method"].IsString() && doc["method"].GetString() == std::string("mhAddNodeCheckResult")) {
        const Address &testerAddress = tx.fromAddress;
        
        const auto &paramsJson = get<JsonObject>(doc, "params");
        const std::string type = get<std::string>(paramsJson, "type");
        const std::string version = get<std::string>(paramsJson, "ver");
        const std::string serverAddress = get<std::string>(paramsJson, "address");
        const std::string ip = get<std::string>(paramsJson, "host");
        //const bool blockHeightPass = get<std::string>(paramsJson, "blockHeightCheck") == "pass";
        //const std::string requestsPerMinuteStr = get<std::string>(paramsJson, "requestsPerMinute");
        //const std::optional<size_t> requestsPerMinute = requestsPerMinuteStr == "unavailable" ? std::nullopt : std::optional<size_t>(std::stoull(requestsPerMinuteStr));

        /*if (serverAddress == "0x00918d061cc200feb7752921419ad46cc410d05abaf2ba9f6d") {
            LOGINFO << "Node test result " << serverAddress << " " << tx.hash << " " << std::string(tx.data.begin(), tx.data.end()) << " " << tx.blockNumber;
        }*/

        const std::optional<std::string> latencyStr = getOpt<std::string>(paramsJson, "latency");
        const std::optional<std::string> rpsStr = getOpt<std::string>(paramsJson, "rps");
        size_t rps = rpsStr.has_value() ? std::stoull(rpsStr.value()) : (latencyStr.has_value() ? std::stoull(latencyStr.value()) : 0);
        rps += 1;
        
        const std::string geo = get<std::string>(paramsJson, "geo");
        const bool success = get<std::string>(paramsJson, "success") == "true";
        if (!success) {
            rps = 0;
        }
        
        //LOGINFO << "Node test found " << serverAddress << " "<< rps << " " << success << " " << tx.fromAddress.calcHexString();
        
        return NodeTestResult(serverAddress, testerAddress, type, tx.data, ip, geo, rps, success, rpsStr.has_value());
    }
    return std::nullopt;
}

static void processTestTransaction(const TransactionInfo &tx, std::unordered_map<std::string, BestNodeTest> &lastNodesTests, LevelDb &leveldbNodeTest, size_t currDay, const BlockInfo &bi, std::unordered_map<std::string, NodeRps> &nodesRps, AllTestedNodes &allNodesForDay) {
    try {
        const std::optional<NodeTestResult> nodeTestResult = parseTestNodeTransaction(tx);
        
        if (nodeTestResult != std::nullopt) {
            auto found = lastNodesTests.find(nodeTestResult->serverAddress);
            if (found == lastNodesTests.end()) {
                BestNodeTest lastNodeTest = leveldbNodeTest.findNodeStatLastResults(nodeTestResult->serverAddress);
                if (lastNodeTest.tests.empty()) {
                    lastNodeTest = BestNodeTest(false);
                }
                lastNodesTests.emplace(nodeTestResult->serverAddress, lastNodeTest);
                found = lastNodesTests.find(nodeTestResult->serverAddress);
            }
            found->second.addElement(BestNodeElement(bi.header.timestamp, nodeTestResult->geo, nodeTestResult->rps, tx.filePos), currDay);
            found->second.isMaxElement = nodeTestResult->isForwardSort;

            //LOGDEBUG << "Ya tuta test2: " << serverAddress << " " << currDay << " " << found->second.getMax(currDay).rps << " " << found->second.getMax(currDay).geo << " " << success << ". " << dataStr;
            
            nodesRps[nodeTestResult->serverAddress].rps.push_back(nodeTestResult->rps);

            allNodesForDay.nodes.insert(nodeTestResult->serverAddress);
        }
    } catch (const exception &e) {
        LOGERR << "Node test exception " << e;
    } catch (const std::exception &e) {
        LOGERR << "Node test exception " << e.what();
    } catch (...) {
        LOGERR << "Node test unknown exception";
    }
}

static void processStateBlock(const TransactionInfo &tx, const BlockInfo &bi, Batch &batchStates) {
    rapidjson::Document doc;
    const rapidjson::ParseResult pr = doc.Parse((const char*)(tx.data.data()), tx.data.size());
    if (pr) {
        if (doc.HasMember("trust") && doc["trust"].IsInt()) {
            const int trust = doc["trust"].GetInt();
            const std::string serverAddress = tx.toAddress.calcHexString();
            //LOGINFO << "Node trust found " << serverAddress;
            batchStates.addNodeTestTrust(serverAddress, NodeTestTrust(bi.header.timestamp, tx.data, trust));
        }
    }
}

static void processRegisterTransaction(const TransactionInfo &tx, AllNodes &allNodes) {
    if (tx.data.size() > 0) {
        if (tx.data[0] == '{' && tx.data[tx.data.size() - 1] == '}') {
            rapidjson::Document doc;
            const rapidjson::ParseResult pr = doc.Parse((const char*)tx.data.data(), tx.data.size());
            if (pr) { // Здесь специально стоят if-ы вместо чеков, чтобы не крашится, если пользователь захочет послать какую-нибудь фигню
                if (doc.HasMember("method") && doc["method"].IsString()) {
                    const std::string method = doc["method"].GetString();
                    if (method == "mh-noderegistration") {
                        if (doc.HasMember("params") && doc["params"].IsObject()) {
                            const auto &params = doc["params"];
                            if (params.HasMember("host") && params["host"].IsString() && params.HasMember("name") && params["name"].IsString()) {
                                const std::string host = params["host"].GetString();
                                const std::string name = params["name"].GetString();
                                std::string type;
                                if (params.HasMember("type") && params["type"].IsString()) {
                                    type = params["type"].GetString();
                                }
                                //LOGINFO << "Node register found " << host;
                                allNodes.nodes[host] = AllNodesNode(name, type);
                            }
                        }
                    } else if (method == "mhRegisterNode") {	
                        if (doc.HasMember("params") && doc["params"].IsObject()) {	
                            const auto &params = doc["params"];	
                            if (params.HasMember("host") && params["host"].IsString() && params.HasMember("name") && params["name"].IsString()) {	
                                const std::string host = params["host"].GetString();	
                                const std::string name = params["name"].GetString();	
                                std::string type;	
                                if (params.HasMember("type") && params["type"].IsString()) {	
                                    type = params["type"].GetString();	
                                }	
                                //LOGINFO << "Node register found " << host;	
                                allNodes.nodes[host] = AllNodesNode(name, type);	
                            }
                        }
                    }
                }
            }
        }
    }
}

void WorkerNodeTest::work() {
    while (true) {
        try {
            std::shared_ptr<BlockInfo> biSP;
            
            const bool isStopped = !queue.pop(biSP);
            if (isStopped) {
                return;
            }
            BlockInfo &bi = *biSP;
            
            const NodeStatBlockInfo lastScriptBlock = leveldbNodeTest.findNodeStatBlock();
            const std::vector<unsigned char> &prevHash = lastScriptBlock.blockHash;

            if (bi.header.blockNumber.value() <= lastScriptBlock.blockNumber) {
                continue;
            }
            
            Timer tt;
            
            CHECK(prevHash.empty() || prevHash == bi.header.prevHash, "Incorrect prev hash. Expected " + toHex(prevHash) + ", received " + toHex(bi.header.prevHash));

            const size_t currDay = leveldbNodeTest.findNodeStatDayNumber().dayNumber;
                        
            Batch batchStates;
            
            AllTestedNodes allNodesForDay;
            AllNodes allNodes;
            std::unordered_map<std::string, NodeRps> nodesRps;
            std::unordered_map<std::string, BestNodeTest> lastNodesTests;
            for (const TransactionInfo &tx: bi.txs) {
                if (tx.isIntStatusNodeTest()) {
                    processTestTransaction(tx, lastNodesTests, leveldbNodeTest, currDay, bi, nodesRps, allNodesForDay);
                } else if (bi.header.isStateBlock()) {
                    processStateBlock(tx, bi, batchStates);
                } else {
                    processRegisterTransaction(tx, allNodes);
                }
            }
            
            if (bi.header.isStateBlock()) {
                NodeTestDayNumber dayNumber;
                dayNumber.dayNumber = currDay + 1;
                batchStates.addNodeTestDayNumber(dayNumber);
            }
            
            for (const auto &[address, rps]: nodesRps) {
                const NodeRps oldNodeRps = leveldbNodeTest.findNodeStatRps(address, currDay);
                NodeRps currNodeRps = oldNodeRps;
                currNodeRps.rps.insert(currNodeRps.rps.end(), rps.rps.begin(), rps.rps.end());
                batchStates.addNodeTestRpsForDay(address, currNodeRps, currDay);
            }
            for (const auto &[serverAddress, res]: lastNodesTests) {
                batchStates.addNodeTestLastResults(serverAddress, res);
            }
            if (!allNodesForDay.nodes.empty()) {
                AllTestedNodes allNodesForDayOld = leveldbNodeTest.findAllTestedNodesForDay(currDay);
                allNodesForDayOld.plus(allNodesForDay);
                allNodesForDayOld.day = currDay;
                batchStates.addAllTestedNodesForDay(allNodesForDayOld, currDay);
            }
            if (!allNodes.nodes.empty()) {
                AllNodes allNodesOld = leveldbNodeTest.findAllNodes();
                allNodesOld.plus(allNodes);
                batchStates.addAllNodes(allNodesOld);
            }
                        
            batchStates.addNodeStatBlock(NodeStatBlockInfo(bi.header.blockNumber.value(), bi.header.hash, 0));
            
            addBatch(batchStates, leveldbNodeTest);
            
            tt.stop();
            
            LOGINFO << "Block " << bi.header.blockNumber.value() << " saved to node test. Time: " << tt.countMs();
            
            checkStopSignal();
        } catch (const exception &e) {
            LOGERR << e;
        } catch (const StopException &e) {
            LOGINFO << "Stop fillCacheWorker thread";
            return;
        } catch (const std::exception &e) {
            LOGERR << e.what();
        } catch (...) {
            LOGERR << "Unknown error";
        }
    }
}
    
void WorkerNodeTest::start() {
    thread = Thread(&WorkerNodeTest::work, this);
}
    
void WorkerNodeTest::process(std::shared_ptr<BlockInfo> bi, std::shared_ptr<std::string> dump) {
    queue.push(bi);
}
    
std::optional<size_t> WorkerNodeTest::getInitBlockNumber() const {
    return initializeScriptBlockNumber;
}

NodeTestResult readNodeTestTransaction(const BestNodeElement &nodeTestElement, size_t currDay, const std::string &folderBlocks) {
    if (nodeTestElement.empty) {
        return NodeTestResult();
    }
    IfStream file;
    openFile(file, getFullPath(nodeTestElement.txPos.fileNameRelative, folderBlocks));
    TransactionInfo tx;
    const bool res = readOneSimpleTransactionInfo(file, nodeTestElement.txPos.pos, tx, false);
    CHECK(res, "Incorrect read transaction info");
    
    std::optional<NodeTestResult> nodeTestResult = parseTestNodeTransaction(tx);
    CHECK(nodeTestResult.has_value(), "Incorrect node test transaction");
    
    nodeTestResult->day = currDay;
    nodeTestResult->timestamp = nodeTestElement.timestamp;
    
    return *nodeTestResult;
}

std::pair<size_t, NodeTestResult> WorkerNodeTest::getLastNodeTestResult(const std::string &address) const {
    const BestNodeTest lastNodeTests = leveldbNodeTest.findNodeStatLastResults(address);
    const size_t lastTimestamp = blockchain.getLastBlock().timestamp;
    if (!lastNodeTests.deserialized) {
        return std::make_pair(lastTimestamp, NodeTestResult());
    }

    BestNodeElement nodeTestElement = lastNodeTests.getMax(getLastBlockDay());
    
    NodeTestResult nodeTestResult = readNodeTestTransaction(nodeTestElement, lastNodeTests.day, folderBlocks);
       
    const NodeRps nodeRps = leveldbNodeTest.findNodeStatRps(address, nodeTestResult.day);
    
    if (!nodeRps.rps.empty()) {
        const uint64_t avgRps = findAvg(nodeRps.rps);
        nodeTestResult.avgRps = avgRps;
    }
    
    return std::make_pair(lastTimestamp, nodeTestResult);
}

std::pair<size_t, NodeTestTrust> WorkerNodeTest::getLastNodeTestTrust(const std::string &address) const {
    const NodeTestTrust result = leveldbNodeTest.findNodeStatLastTrust(address);
    const size_t lastTimestamp = blockchain.getLastBlock().timestamp;
    return std::make_pair(lastTimestamp, result);
}

NodeTestCount2 WorkerNodeTest::getLastDayNodeTestCount(const std::string &address) const {
    const BestNodeTest lastNodeTests = leveldbNodeTest.findNodeStatLastResults(address);

    return lastNodeTests.countTests(getLastBlockDay());
}

std::vector<std::pair<std::string, NodeTestExtendedStat>> WorkerNodeTest::filterLastNodes(size_t countTests) const {
    const AllTestedNodes allNodesForDay = leveldbNodeTest.findAllTestedNodesForLastDay();
    const size_t lastBlockDay = getLastBlockDay();
    std::vector<std::pair<std::string, NodeTestExtendedStat>> result;
    for (const std::string &node: allNodesForDay.nodes) {
        const BestNodeTest lastNodeTests = leveldbNodeTest.findNodeStatLastResults(node);
        const NodeTestCount2 nodeTestCount = lastNodeTests.countTests(lastBlockDay);
        
        const BestNodeElement nodeTestElement = lastNodeTests.getMax(lastBlockDay);
        const NodeTestResult nodeTestResult = readNodeTestTransaction(nodeTestElement, lastNodeTests.day, folderBlocks);
        
        result.emplace_back(node, NodeTestExtendedStat(nodeTestCount, nodeTestResult.typeNode, nodeTestResult.ip));
    }
    result.erase(std::remove_if(result.begin(), result.end(), [countTests](const auto &pair) {
        return pair.second.count.countSuccess() < countTests;
    }), result.end());
    return result;
}

std::pair<int, size_t> WorkerNodeTest::calcNodeRaiting(const std::string &address, size_t countTests) const {
    CHECK(countTests != 0, "Incorrect countTests parameter");
    const AllTestedNodes allNodesForDay = leveldbNodeTest.findAllTestedNodesForLastDay();
    std::vector<std::pair<std::string, uint64_t>> avgs;
    for (const std::string &node: allNodesForDay.nodes) {
        const NodeRps nodeRps = leveldbNodeTest.findNodeStatRps(node, allNodesForDay.day);
        if (nodeRps.rps.size() >= countTests) {
            const uint64_t median = findAvg(nodeRps.rps);
            avgs.emplace_back(node, median);
        }
    }
    
    std::sort(avgs.begin(), avgs.end(), [](const auto &first, const auto &second) {
        return first.second < second.second;
    });
    
    /*const auto calcOne = [&address](const auto &medians) -> int {
        const size_t forging_node_units = medians.size();
        
        auto iterator = medians.cbegin();
        for (uint64_t i = 0; i < 5; i++) {
            const uint64_t add = (5 - forging_node_units % 5) > i ? 0 : 1;
            for (uint64_t j = 0; j < (forging_node_units / 5 + add); j++) {
                CHECK(iterator != medians.cend(), "iterator overflow");
                const std::string &node_addres = iterator->first;
                if (node_addres == address) {
                    return i + 1;
                }
                iterator++;
            }
        }
        
        return 0;
    };*/
    
    const size_t countNodes = avgs.size();
    const int countGroups = 5;
    
    const size_t normalGroupSize = countNodes / countGroups;
    const size_t extendenGroupSize = normalGroupSize + 1;
    const size_t countExtendedGroups = countNodes % countGroups;
    const size_t countNormalGroups = countGroups - countExtendedGroups;
    const size_t countElementsInNormalGroups = countNormalGroups * normalGroupSize;
    
    const auto found = std::find_if(avgs.begin(), avgs.end(), [&address](const auto &pair) {
        return pair.first == address;
    });
    if (found == avgs.end()) {
        return std::make_pair(0, allNodesForDay.day);
    }
    const size_t elementNumber = std::distance(avgs.begin(), found);
    if (elementNumber < countElementsInNormalGroups) {
        return std::make_pair((int)(elementNumber / normalGroupSize + 1), allNodesForDay.day);
    } else {
        return std::make_pair((int)((elementNumber - countElementsInNormalGroups) / extendenGroupSize + countNormalGroups + 1), allNodesForDay.day);
    }
}

size_t WorkerNodeTest::getLastBlockDay() const {
    const size_t currDay = leveldbNodeTest.findNodeStatDayNumber().dayNumber;
    return currDay;
}

std::map<std::string, AllNodesNode> WorkerNodeTest::getAllNodes() const {
    const AllNodes allNodes = leveldbNodeTest.findAllNodes();
    return allNodes.nodes;
}

}
