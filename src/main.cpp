#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <atomic>
#include <fstream>

#include <libconfig.h++>

#include "cmake_modules/GitSHA1.h"

#include "check.h"
#include "stringUtils.h"
#include "utils/utils.h"
#include "log.h"
#include "utils/FileSystem.h"
#include "curlWrapper.h"

#include "duration.h"

#include "synchronize_blockchain.h"

#include "Server.h"

#include "stopProgram.h"
#include "network_utils.h"
#include "tcmalloc_management.h"

#include "P2P/P2P.h"
#include "P2P/P2P_Ips.h"
#include "P2P/P2P_Graph.h"

#include "generate_json_v8.h"

#include "nslookup.h"

#include "Modules.h"

using namespace common;
using namespace torrent_node_lib;

static std::atomic<int> countRunningServerThreads(-1);

void crash_handler(int sig) {
    void *array[50];
    const size_t size = backtrace(array, 50);
    
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    signal(SIGINT, nullptr);
    exit(1);
}

static void serverThreadFunc(const Sync &sync, int port, const std::string &serverPrivKey) {
    try {
        Server server(sync, port, countRunningServerThreads, serverPrivKey);
        std::this_thread::sleep_for(1s); // Небольшая задержка сервера перед запуском
        const bool res = server.start("./");
        CHECK(res, "Not started server");
    } catch (const exception &e) {
        LOGERR << e;
    } catch (const std::exception &e) {
        LOGERR << e.what();
    } catch (...) {
        LOGERR << "Server thread error";
    }
    countRunningServerThreads = 0;
}

static std::vector<std::pair<std::string, std::string>> readServers(const std::string &fileName, size_t port) {
    std::ifstream file(fileName);
    std::string line;
    
    std::vector<std::pair<std::string, std::string>> result;
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty()) {
            const size_t findSpace = line.find(",");
            CHECK(findSpace != line.npos, "Incorrect graph file");
            std::string srv1 = trim(line.substr(0, findSpace));
            srv1 = addDefaultPort(srv1, port);
            std::string srv2 = trim(line.substr(findSpace + 1));
            srv2 = addDefaultPort(srv2, port);
            result.emplace_back(srv1, srv2);
        }
    }

    return result;
}

struct SettingsDb {
    size_t writeBufSizeMb;
    bool isBloomFilter;
    bool isChecks;
    size_t lruCacheMb;
    
    bool isSet = false;
};

static SettingsDb parseSettingsDb(const libconfig::Setting &allSettings, const std::string &prefix) {
    SettingsDb settings;
    if (allSettings.exists(prefix + "write_buffer_size_mb")) {
        settings.writeBufSizeMb = static_cast<int>(allSettings[prefix + "write_buffer_size_mb"]);
        settings.lruCacheMb = static_cast<int>(allSettings[prefix + "lru_cache_mb"]);
        settings.isBloomFilter = allSettings[prefix + "is_bloom_filter"];
        settings.isChecks = allSettings[prefix + "is_checks"];
        
        settings.isSet = true;
    }
    return settings;
}

static void removeFiles(const std::string &relativeFileName, size_t pos, const std::string &pathToFolder, const std::string &pathToBd) {
    const std::string file = getFullPath(relativeFileName, pathToFolder);
    resizeFile(file, pos);
    
    std::vector<std::string> filesInFolder = getAllFilesRelativeInDir(pathToFolder);
    std::sort(filesInFolder.begin(), filesInFolder.end());
    const auto found = std::upper_bound(filesInFolder.begin(), filesInFolder.end(), relativeFileName);
    for (auto it = found; it != filesInFolder.end(); it++) {
        removeFile(pathToFolder, *it);
    }
    
    removeDirectory(pathToBd);
}

int main (int argc, char *const *argv) {
    //signal(SIGSEGV, crash_handler);
    //signal(SIGABRT, crash_handler);

    initializeStopProgram();
    tcmallocMaintenance();
    
    Curl::initialize();
    
    if (argc < 2) {
        std::cout << "path_to_config isConsoleLog" << std::endl;
        return -1;
    }
    
    bool isConsoleLog = true;
    if (argc == 3 && argv[2] == std::string("true")) {
        isConsoleLog = true;
    }

    configureLog("./", true, isConsoleLog, false, true);
    
    LOGINFO << "Repository version " << g_GIT_SHA1 << " " << VERSION << " " << g_GIT_DATE;
    LOGINFO << "Is local changes " << g_GIT_IS_LOCAL_CHANGES;
    LOGINFO << "Branch " << g_GIT_REFSPEC;
    LOGINFO << "Torrent type: " << SERVER_TYPE;
   
    const std::string path_to_config(argv[1]);
    libconfig::Config config;
    try {
        config.readFile(path_to_config.c_str());
    } catch(const libconfig::FileIOException &fioex) {
        LOGERR << std::string("I/O error while reading file. ") + path_to_config;
        return -1;
    } catch(const libconfig::ParseException &pex) {
        std::stringstream sst;
        sst << "Parse error at " << pex.getFile() << ":" << pex.getLine() <<
        " - " << pex.getError() << std::endl;
        LOGERR << sst.str();
        return -1;
    }
    
    try {
        const libconfig::Setting &allSettings = config.getRoot()["Base"];
        
        const std::string pathToBd = allSettings["path_to_bd"];
        const std::string pathToFolder = allSettings["path_to_folder"];
        const size_t countWorkers = static_cast<int>(allSettings["count_threads"]);
        const SettingsDb settingsDb = parseSettingsDb(allSettings, "");
        CHECK(settingsDb.isSet, "settings db not found");
        const SettingsDb settingsStateDb = parseSettingsDb(allSettings, "st_");
        const size_t port = static_cast<int>(allSettings["port"]);
        const bool getBlocksFromFile = allSettings["get_blocks_from_file"];
        const size_t countConnections = static_cast<int>(allSettings["count_connections"]);
        const std::string thisServer = getHostName();
        size_t maxCountElementsBlockCache = 0;
        if (allSettings.exists("max_count_elements_block_cache")) {
            maxCountElementsBlockCache = static_cast<int>(allSettings["max_count_elements_block_cache"]);
        }
        size_t maxCountElementsTxsCache = 0;
        if (allSettings.exists("max_count_blocks_txs_cache")) {
            maxCountElementsTxsCache = static_cast<int>(allSettings["max_count_blocks_txs_cache"]);
        }
        size_t maxLocalCacheElements = 0;
        if (allSettings.exists("mac_local_cache_elements")) {
            maxLocalCacheElements = static_cast<int>(allSettings["mac_local_cache_elements"]);
        }
        if (allSettings.exists("max_local_cache_elements")) {
            maxLocalCacheElements = static_cast<int>(allSettings["max_local_cache_elements"]);
        }
        std::string signKey;
        if (allSettings.exists("sign_key")) {
            signKey = static_cast<const char*>(allSettings["sign_key"]);
        }
        
        initBlockchainUtils();
        
        bool isValidate = false;
        if (allSettings.exists("validate")) {
            isValidate = allSettings["validate"];
        }
        bool isValidateSign = false;
        if (allSettings.exists("validateSign")) {
            isValidateSign = allSettings["validateSign"];
        }
        
        bool isValidateState = false;
        if (allSettings.exists("validate_state")) {
            isValidateState = allSettings["validate_state"];
        }
        
        std::string v8Server;
        if (allSettings.exists("v8_server")) {
            v8Server = static_cast<const char*>(allSettings["v8_server"]);
        }
        
        std::string testNodesServer;
        if (allSettings.exists("test_nodes_result_server")) {
            testNodesServer = static_cast<const char*>(allSettings["test_nodes_result_server"]);
        }
        
        size_t maxAdvancedLoadBlocks = 10;
        if (allSettings.exists("advanced_load_blocks")) {
            maxAdvancedLoadBlocks = static_cast<int>(allSettings["advanced_load_blocks"]);
        }
        size_t countBlocksInBatch = 1;
        if (allSettings.exists("count_blocks_in_batch")) {
            countBlocksInBatch = static_cast<int>(allSettings["count_blocks_in_batch"]);
        }
        bool isCompress = false;
        if (allSettings.exists("compress_blocks")) {
            isCompress = static_cast<bool>(allSettings["compress_blocks"]);
        }
        
        std::string technicalAddress;
        if (allSettings.exists("technical_address")) {
            technicalAddress = static_cast<const char*>(allSettings["technical_address"]);
        }
        
        bool isPreLoad = false;
        if (allSettings.exists("is_preload")) {
            isPreLoad = allSettings["is_preload"];
        }
                
        std::set<std::string> modulesStr;
        for (const std::string &moduleStr: allSettings["modules"]) {
            modulesStr.insert(moduleStr);
        }
        
        parseModules(modulesStr);
        LOGINFO << "Modules " << modules;
        
        if (!v8Server.empty()) {
            setV8Server(v8Server, modules[MODULE_V8]);
        }

        std::unique_ptr<P2P> p2p = nullptr;
        std::unique_ptr<P2P> p2p2 = nullptr;
        std::unique_ptr<P2P> p2pAll = nullptr;
        
        size_t otherPortTorrent = port;
        if (allSettings.exists("other_torrent_port")) {
            otherPortTorrent = static_cast<int>(allSettings["other_torrent_port"]);
        }

        const std::string myIp = trim(getMyIp2("echo.metahash.io:7654")) + ":" + std::to_string(port);

        if (allSettings["servers"].isArray()) {
            std::vector<std::string> serversStr;
            for (const std::string &serverStr: allSettings["servers"]) {
                serversStr.push_back(serverStr);
            }
            p2p = std::make_unique<P2P_Ips>(serversStr, countConnections);
            p2p2 = std::make_unique<P2P_Ips>(serversStr, countConnections);
            p2pAll = std::make_unique<P2P_Ips>(serversStr, countConnections);
        } else {
            const std::string &fileName = allSettings["servers"];
            if (beginWith(fileName, "http")) {
                const std::vector<NsResult> bestIps = getBestIps(fileName, 3);
                CHECK(!bestIps.empty(), "Not found servers");
                for (const NsResult &r: bestIps) {
                    LOGINFO << "Node server found " << r.server << " " << r.timeout;
                }
                std::vector<std::string> serversStr;
                std::transform(bestIps.begin(), bestIps.end(), std::back_inserter(serversStr), std::mem_fn(&NsResult::server));
                p2p = std::make_unique<P2P_Ips>(serversStr, countConnections);
                p2p2 = std::make_unique<P2P_Ips>(serversStr, countConnections);
                
                const std::vector<NsResult> bestIps2 = getBestIps(fileName, 100);
                CHECK(!bestIps2.empty(), "Not found servers");
                std::vector<std::string> serversStr2;
                std::transform(bestIps2.begin(), bestIps2.end(), std::back_inserter(serversStr2), std::mem_fn(&NsResult::server));
                p2pAll = std::make_unique<P2P_Ips>(serversStr2, countConnections);                
            } else {
                const std::vector<std::pair<std::string, std::string>> serversGraph = readServers(fileName, otherPortTorrent);
                p2p = std::make_unique<P2P_Graph>(serversGraph, myIp, countConnections);
                p2p2 = std::make_unique<P2P_Graph>(serversGraph, myIp, countConnections);
                p2pAll = std::make_unique<P2P_Graph>(serversGraph, myIp, countConnections);
            }
        }
       
        Sync sync(
            pathToFolder, 
            technicalAddress,
            LevelDbOptions(settingsDb.writeBufSizeMb, settingsDb.isBloomFilter, settingsDb.isChecks, getFullPath("simple", pathToBd), settingsDb.lruCacheMb),
            CachesOptions(maxCountElementsBlockCache, maxCountElementsTxsCache, maxLocalCacheElements),
            GetterBlockOptions(maxAdvancedLoadBlocks, countBlocksInBatch, p2p.get(), p2p2.get(), p2pAll.get(), getBlocksFromFile, isValidate, isValidateSign, isCompress, isPreLoad),
            signKey,
            TestNodesOptions(otherPortTorrent, myIp, testNodesServer),
            isValidateState
        );
        if (settingsStateDb.isSet) {
            sync.setLeveldbOptScript(LevelDbOptions(settingsStateDb.writeBufSizeMb, settingsStateDb.isBloomFilter, settingsStateDb.isChecks, getFullPath("states", pathToBd), settingsStateDb.lruCacheMb));
        }
        if (settingsStateDb.isSet) {
            sync.setLeveldbOptNodeTest(LevelDbOptions(settingsStateDb.writeBufSizeMb, settingsStateDb.isBloomFilter, settingsStateDb.isChecks, getFullPath("nodeTest", pathToBd), settingsStateDb.lruCacheMb));
        }
        
        //LOGINFO << "Is virtual machine: " << sync.isVirtualMachine();
        
        std::thread serverThread(serverThreadFunc, std::cref(sync), port, signKey);
        serverThread.detach();
        
        //sync.addUsers({Address("0x0049704639387c1ae22283184e7bc52d38362ade0f977030e6"), Address("0x0034d209107371745c6f5634d6ed87199bac872c310091ca56")});
        
        const std::optional<ConflictBlocksInfo> result = sync.synchronize(countWorkers);
        if (result.has_value()) {
            if (!getBlocksFromFile) {
                LOGINFO << "Remove conflicted blocks from pos " << result->ourConflictedBlock.filePos.fileNameRelative << " " << result->ourConflictedBlock.filePos.pos;
                removeFiles(result->ourConflictedBlock.filePos.fileNameRelative, result->ourConflictedBlock.filePos.pos, pathToFolder, pathToBd);
                std::string loadedFile = loadFile(pathToFolder, result->ourConflictedBlock.filePos.fileNameRelative);
                loadedFile += result->blockDump;
                saveToFile(pathToFolder, result->ourConflictedBlock.filePos.fileNameRelative, loadedFile);
            }
            stopProgram();
        }
        
        while(countRunningServerThreads.load() != 0);
        LOGINFO << "Stop server thread";       
    } catch (const libconfig::SettingNotFoundException &e) {
        LOGERR << std::string("Attribute ") + std::string(e.getPath()) + std::string(" not found");
    } catch (const libconfig::SettingException &e) {
        LOGERR << std::string(e.what()) + " " + std::string(e.getPath());
    } catch (const libconfig::ConfigException &e) {
        LOGERR << std::string(e.what());
    } catch (const exception &e) {
        LOGERR << e;
    } catch (const std::runtime_error &e) {
        LOGERR << e.what();
    } catch (...) {
        LOGERR << "Unknown error ";
    }
    return 0;
}
