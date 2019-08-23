#include "SyncImpl.h"

#include "BlockchainRead.h"
#include "PrivateKey.h"

#include "parallel_for.h"
#include "stopProgram.h"
#include "duration.h"
#include "check.h"
#include "log.h"
#include "convertStrings.h"
#include "stringUtils.h"

#include "BlockSource/FileBlockSource.h"
#include "BlockSource/NetworkBlockSource.h"

#include "Workers/WorkerCache.h"
#include "Workers/WorkerScript.h"
#include "Workers/WorkerNodeTest.h"
#include "Workers/WorkerMain.h"

#include "Workers/ScriptBlockInfo.h"
#include "Workers/NodeTestsBlockInfo.h"

using namespace common;

namespace torrent_node_lib {
    
const static std::string VERSION_DB = "v4.4";
    
bool isInitialized = false;

SyncImpl::SyncImpl(const std::string& folderBlocks, const std::string &technicalAddress, const LevelDbOptions& leveldbOpt, const CachesOptions& cachesOpt, const GetterBlockOptions &getterBlocksOpt, const std::string &signKeyName, const TestNodesOptions &testNodesOpt, bool validateStates)
    : leveldb(leveldbOpt.writeBufSizeMb, leveldbOpt.isBloomFilter, leveldbOpt.isChecks, leveldbOpt.folderName, leveldbOpt.lruCacheMb)
    , folderBlocks(folderBlocks)
    , technicalAddress(technicalAddress)
    , caches(cachesOpt.maxCountElementsBlockCache, cachesOpt.maxCountElementsTxsCache, cachesOpt.macLocalCacheElements)
    , isValidate(getterBlocksOpt.isValidate)
    , validateStates(validateStates)
    , testNodes(getterBlocksOpt.p2p, testNodesOpt.myIp, testNodesOpt.testNodesServer, testNodesOpt.defaultPortTorrent)
{
    if (getterBlocksOpt.isValidate) {
        CHECK(!getterBlocksOpt.getBlocksFromFile, "validate and get_blocks_from_file options not compatible");
    }
    CHECK(!modules[MODULE_USERS] || !getterBlocksOpt.getBlocksFromFile, "Options saveOnlyUsers and getBlocksFromFile not compatible");
    
    if (getterBlocksOpt.getBlocksFromFile) {
        isSaveBlockToFiles = false;
        getBlockAlgorithm = std::make_unique<FileBlockSource>(leveldb, folderBlocks, isValidate);
    } else {
        CHECK(getterBlocksOpt.p2p != nullptr, "p2p nullptr");
        isSaveBlockToFiles = modules[MODULE_BLOCK_RAW];
        const bool isSaveAllTx = modules[MODULE_USERS];
        getBlockAlgorithm = std::make_unique<NetworkBlockSource>(folderBlocks, getterBlocksOpt.maxAdvancedLoadBlocks, getterBlocksOpt.countBlocksInBatch, getterBlocksOpt.isCompress, *getterBlocksOpt.p2p, isSaveAllTx, getterBlocksOpt.isValidate, getterBlocksOpt.isValidateSign);
    }
        
    if (!signKeyName.empty()) {
        const std::string signKeyPath = signKeyName + ".raw.prv";
        const std::string result = trim(loadFile("./", signKeyPath));
        CHECK(!result.empty(), "File with private key not found");
        privateKey = std::make_unique<PrivateKey>(fromHex(result), signKeyName);
        
        testNodes.setPrivateKey(privateKey.get());
    }
}

void SyncImpl::setLeveldbOptScript(const LevelDbOptions &leveldbOpt) {
    this->leveldbOptScript = leveldbOpt;
}

void SyncImpl::setLeveldbOptNodeTest(const LevelDbOptions &leveldbOpt) {
    this->leveldbOptNodeTest = leveldbOpt;
}

SyncImpl::~SyncImpl() {
    try {
        if (mainWorker != nullptr) {
            mainWorker->join();
        }
        if (cacheWorker != nullptr) {
            cacheWorker->join();
        }
        if (nodeTestWorker != nullptr) {
            nodeTestWorker->join();
        }
        if (scriptWorker != nullptr) {
            scriptWorker->join();
        }
    } catch (...) {
        LOGERR << "Error while stoped thread";
    }
}

void SyncImpl::addUsers(const std::set<Address>& addresses) {
    CHECK(modules[MODULE_USERS], "saveOnlyUsers not set");
    std::lock_guard<std::mutex> lock(usersMut);
    users.insert(addresses.begin(), addresses.end());
}

void SyncImpl::saveTransactions(BlockInfo& bi, const std::string &binaryDump, bool saveBlockToFile) {
    if (!saveBlockToFile) {
        return;
    }
    
    const std::string &fileName = bi.header.filePos.fileNameRelative;
    CHECK(!fileName.empty(), "File name not set");
    
    if (!modules[MODULE_USERS]) {
        const size_t currPos = saveBlockToFileBinary(getFullPath(fileName, folderBlocks), binaryDump);
        bi.header.filePos.pos = currPos;
        for (TransactionInfo &tx : bi.txs) {
            tx.filePos.fileNameRelative = fileName;
            tx.filePos.pos += currPos;
        }
    } else {
        std::ofstream file;
        
        openFile(file, getFullPath(fileName, folderBlocks));
        for (TransactionInfo &tx: bi.txs) {
            if (tx.isSaveToBd) {
                const size_t filePos = saveTransactionToFile(file, tx.allRawTx);
                
                tx.filePos.fileNameRelative = fileName;
                tx.filePos.pos = filePos;
            }
            tx.allRawTx.clear();
            tx.allRawTx.shrink_to_fit();
        }
    }   
}

void SyncImpl::filterTransactionsToSave(BlockInfo& bi) {
    std::unique_lock<std::mutex> lock(usersMut);
    const std::set<Address> copyUsers = users;
    lock.unlock();
    for (TransactionInfo &tx: bi.txs) {
        if (!modules[MODULE_USERS]) {
            tx.isSaveToBd = true;
        } else if (modules[MODULE_USERS] && (copyUsers.find(tx.fromAddress) != copyUsers.end() || copyUsers.find(tx.toAddress) != copyUsers.end())) {
            tx.isSaveToBd = true;
        } else if (modules[MODULE_USERS] && tx.isSignBlockTx) {
            tx.isSaveToBd = true;
        } else {
            tx.isSaveToBd = false;
        }
    }
}

void SyncImpl::saveBlockToLeveldb(const BlockInfo &bi) {
    Batch batch;
    if (modules[MODULE_BLOCK]) {
        batch.addBlockHeader(bi.header.hash, bi.header.serialize());
    }
    
    const std::string blockMetadata = findBlockMetadata(leveldb);
    const BlocksMetadata metadata = BlocksMetadata::deserialize(blockMetadata);
    BlocksMetadata newMetadata;
    newMetadata.prevBlockHash = bi.header.prevHash;
    if (metadata.prevBlockHash == bi.header.prevHash) {
        if (metadata.blockHash < bi.header.hash) {
            newMetadata.blockHash = metadata.blockHash;
        } else {
            newMetadata.blockHash = bi.header.hash;
        }
    } else {
        newMetadata.blockHash = bi.header.hash;
    }
    batch.addBlockMetadata(newMetadata.serialize());
    
    FileInfo fi;
    fi.filePos.fileNameRelative = bi.header.filePos.fileNameRelative;
    fi.filePos.pos = bi.header.endBlockPos();
    batch.addFileMetadata(CroppedFileName(fi.filePos.fileNameRelative), fi.serialize());
    
    addBatch(batch, leveldb);
}

void SyncImpl::initialize() {
    const std::string modulesStr = findModules(leveldb);
    if (!modulesStr.empty()) {
        const Modules oldModules(modulesStr);
        CHECK(modules == oldModules, "Modules changed in this database");
    } else {
        saveModules(modules.to_string(), leveldb);
    }
    
    const std::string versionDb = findVersionDb(leveldb);
    if (!versionDb.empty()) {
        CHECK(versionDb == VERSION_DB, "Version database not matches");
    } else {
        saveVersionDb(VERSION_DB, leveldb);
    }
    
    const std::string blockMetadata = findBlockMetadata(leveldb);
    const BlocksMetadata metadata = BlocksMetadata::deserialize(blockMetadata);
    
    getBlockAlgorithm->initialize();
    
    blockchain.clear();
    
    const std::set<std::string> blocksRaw = getAllBlocks(leveldb);
    for (const std::string &blockRaw: blocksRaw) {
        const BlockHeader bh = BlockHeader::deserialize(blockRaw);
        blockchain.addWithoutCalc(bh);
    }
    
    if (!metadata.blockHash.empty()) {
        const size_t countBlocks = blockchain.calcBlockchain(metadata.blockHash);
        LOGINFO << "Last block " << countBlocks << " " << toHex(metadata.blockHash);
    }
}

std::vector<Worker*> SyncImpl::makeWorkers() {
    std::vector<Worker*> workers;
    cacheWorker = std::make_unique<WorkerCache>(caches);
    workers.emplace_back(cacheWorker.get());
    mainWorker = std::make_unique<WorkerMain>(folderBlocks, leveldb, caches, blockchain, users, usersMut, countThreads, validateStates);
    workers.emplace_back(mainWorker.get());
    if (modules[MODULE_V8]) {
        CHECK(leveldbOptScript.isValid, "Leveldb script options not setted");
        scriptWorker = std::make_unique<WorkerScript>(leveldb, leveldbOptScript, modules, caches);
        workers.emplace_back(scriptWorker.get());
    }
    if (modules[MODULE_NODE_TEST]) {
        CHECK(leveldbOptNodeTest.isValid, "Leveldb node test options not setted");
        nodeTestWorker = std::make_unique<WorkerNodeTest>(blockchain, folderBlocks, leveldbOptNodeTest);
        workers.emplace_back(nodeTestWorker.get());
        testNodes.addWorkerTest(nodeTestWorker.get());
    }
    
    for (Worker* &worker: workers) {
        worker->start();
    }
    
    testNodes.start();
    
    const auto minElement = std::min_element(workers.begin(), workers.end(), [](const Worker* first, const Worker* second) {
        if (!first->getInitBlockNumber().has_value()) {
            return false;
        } else if (!second->getInitBlockNumber().has_value()) {
            return true;
        } else {
            return first->getInitBlockNumber().value() < second->getInitBlockNumber().value();
        }
    });
    if (minElement != workers.end() && minElement.operator*()->getInitBlockNumber().has_value()) {
        const size_t fromBlockNumber = minElement.operator*()->getInitBlockNumber().value() + 1;
        LOGINFO << "Retry from block " << fromBlockNumber;
        for (size_t blockNumber = fromBlockNumber; blockNumber <= blockchain.countBlocks(); blockNumber++) {
            const BlockHeader &bh = blockchain.getBlock(blockNumber);
            std::shared_ptr<BlockInfo> bi = std::make_shared<BlockInfo>();
            std::shared_ptr<std::string> blockDump = std::make_shared<std::string>();
            try {
                FileBlockSource::getExistingBlockS(folderBlocks, bh, *bi, *blockDump, isValidate);
            } catch (const exception &e) {
                LOGWARN << "Dont get existing block " << e;
                getBlockAlgorithm->getExistingBlock(bh, *bi, *blockDump);
            } catch (const std::exception &e) {
                LOGWARN << "Dont get existing block " << e.what();
                getBlockAlgorithm->getExistingBlock(bh, *bi, *blockDump);
            } catch (...) {
                LOGWARN << "Dont get existing block " << "Unknown";
                getBlockAlgorithm->getExistingBlock(bh, *bi, *blockDump);
            }
            filterTransactionsToSave(*bi);
            for (Worker* &worker: workers) {
                if (worker->getInitBlockNumber().has_value() && worker->getInitBlockNumber() < blockNumber) { // TODO добавить сюда поле getToBlockNumberRetry
                    worker->process(bi, blockDump);
                }
            }
        }
    }
    
    return workers;
}

void SyncImpl::process(const std::vector<Worker*> &workers) {
    while (true) {
        const time_point beginWhileTime = ::now();
        std::shared_ptr<BlockInfo> prevBi = nullptr;
        std::shared_ptr<std::string> prevDump = nullptr;
        try {
            auto [isContinue, knownLstBlk] = getBlockAlgorithm->doProcess(blockchain.countBlocks());
            knownLastBlock = knownLstBlk;
            while (isContinue) {
                Timer tt;
                
                std::shared_ptr<BlockInfo> nextBi = std::make_shared<BlockInfo>();
                
                nextBi->times.timeBegin = ::now();
                nextBi->times.timeBeginGetBlock = ::now();
                
                std::shared_ptr<std::string> nextBlockDump = std::make_shared<std::string>();
                isContinue = getBlockAlgorithm->process(*nextBi, *nextBlockDump);
                if (!isContinue) {
                    break;
                }
                
                if (prevBi == nullptr) {
                    CHECK(prevDump == nullptr, "Ups");
                    prevBi = nextBi;
                    prevDump = nextBlockDump;
                    
                    if (isValidate) {
                        continue;
                    }
                } else {
                    if (isValidate) {
                        const auto &thisHashFromHex = prevBi->header.hash;
                        for (const TransactionInfo &tx: nextBi->txs) {
                            if (tx.isSignBlockTx) {
                                CHECK(thisHashFromHex == tx.data, "Block signatures not confirmed");
                            }
                        }
                    } else {
                        prevBi = nextBi;
                        prevDump = nextBlockDump;
                    }
                }
                
                filterTransactionsToSave(*prevBi);
                saveTransactions(*prevBi, *prevDump, isSaveBlockToFiles);
                
                const size_t currentBlockNum = blockchain.addBlock(prevBi->header);
                CHECK(currentBlockNum != 0, "Incorrect block number");
                prevBi->header.blockNumber = currentBlockNum;
                
                for (TransactionInfo &tx: prevBi->txs) {
                    tx.blockNumber = prevBi->header.blockNumber.value();
                    
                    if (tx.fromAddress.isInitialWallet()) {
                        prevBi->txsStatistic.countInitTxs++;
                    } else {
                        prevBi->txsStatistic.countTransferTxs++;
                    }
                }
                
                tt.stop();
                
                prevBi->times.timeEndGetBlock = ::now();
                
                LOGINFO << "Block " << currentBlockNum << " getted. Count txs " << prevBi->txs.size() << ". Time ms " << tt.countMs() << " current block " << toHex(prevBi->header.hash) << ". Parent hash " << toHex(prevBi->header.prevHash);
                
                for (Worker* worker: workers) {
                    worker->process(prevBi, prevDump);
                }
                
                saveBlockToLeveldb(*prevBi);
                
                if (isValidate) {
                    prevBi = nextBi;
                    prevDump = nextBlockDump;
                }
                
                checkStopSignal();
            }
        } catch (const exception &e) {
            LOGERR << e;
        } catch (const std::exception &e) {
            LOGERR << e.what();
        }
        const time_point endWhileTime = ::now();
        
        const static milliseconds MAX_PENDING = milliseconds(1s) / 2;
        milliseconds pending = std::chrono::duration_cast<milliseconds>(endWhileTime - beginWhileTime);
        pending = MAX_PENDING - pending;
        if (pending < 0ms) {
            pending = 0ms;
        }
        
        sleepMs(pending);
    }
}

void SyncImpl::synchronize(int countThreads) {
    this->countThreads = countThreads;
    
    CHECK(isInitialized, "Not initialized");
    CHECK(modules[MODULE_BLOCK], "module " + MODULE_BLOCK_STR + " not set");
    
    try {
        initialize();
        const std::vector<Worker*> workers = makeWorkers();
        process(workers);
    } catch (const StopException &e) {
        LOGINFO << "Stop synchronize thread";
    } catch (const exception &e) {
        LOGERR << e;
    } catch (const std::exception &e) {
        LOGERR << e.what();
    } catch (...) {
        LOGERR << "Unknown error";
    }
}

std::vector<TransactionInfo> SyncImpl::getTxsForAddress(const Address &address, size_t from, size_t count, size_t limitTxs) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getTxsForAddress(address, from, count, limitTxs);
}

std::vector<TransactionInfo> SyncImpl::getTxsForAddress(const Address &address, size_t &from, size_t count, size_t limitTxs, const TransactionsFilters &filters) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getTxsForAddress(address, from, count, limitTxs, filters);
}

std::optional<TransactionInfo> SyncImpl::getTransaction(const std::string &txHash) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getTransaction(txHash);
}

BalanceInfo SyncImpl::getBalance(const Address &address) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getBalance(address);
}

std::string SyncImpl::getBlockDump(const BlockHeader &bh, size_t fromByte, size_t toByte, bool isHex, bool isSign) const {
    CHECK(modules[MODULE_BLOCK] && modules[MODULE_BLOCK_RAW] && !modules[MODULE_USERS], "modules " + MODULE_BLOCK_STR + " " + MODULE_BLOCK_RAW_STR + " not set");
       
    const std::optional<std::shared_ptr<std::string>> cache = caches.blockDumpCache.getValue(common::HashedString(bh.hash.data(), bh.hash.size()));
    std::string res;
    size_t realSizeBlock;
    std::string fullBlockDump;
    if (!cache.has_value()) {
        CHECK(!bh.filePos.fileNameRelative.empty(), "Empty file name in block header");
        IfStream file;
        openFile(file, getFullPath(bh.filePos.fileNameRelative, folderBlocks));
        const auto &[size_block, dumpBlock] = torrent_node_lib::getBlockDump(file, bh.filePos.pos, fromByte, toByte);
        res = dumpBlock;
        realSizeBlock = size_block;
        
        if (isSign) {
            if (toByte >= realSizeBlock) {
                if (fromByte == 0) {
                    fullBlockDump = res;
                } else {
                    const auto &[size_block, dumpBlock] = torrent_node_lib::getBlockDump(file, bh.filePos.pos, 0, toByte);
                    fullBlockDump = dumpBlock;
                }
            }
        }
    } else {
        std::shared_ptr<std::string> element = cache.value();
        res = element->substr(fromByte, toByte - fromByte);
        realSizeBlock = element->size();
        if (isSign) {
            if (toByte >= realSizeBlock) {
                fullBlockDump = *element;
            }
        }
    }
    
    if (isSign) {
        CHECK(privateKey != nullptr, "Private key not set");
        
        if (fromByte == 0) {
            res = makeFirstPartBlockSign(realSizeBlock) + res;
        }
        
        if (toByte >= realSizeBlock) {
            res += makeBlockSign(fullBlockDump, *privateKey);
        }
    }
    
    if (isHex) {
        return toHex(res.begin(), res.end());
    } else {
        return res;
    }
}

bool SyncImpl::verifyTechnicalAddressSign(const std::string &binary, const std::vector<unsigned char> &signature, const std::vector<unsigned char> &pubkey) const {
    const bool res = verifySignature(binary, signature, pubkey);
    if (!res) {
        return false;
    }
    return technicalAddress == getAddress(pubkey);
}

BlockInfo SyncImpl::getFullBlock(const BlockHeader &bh, size_t beginTx, size_t countTx) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getFullBlock(bh, beginTx, countTx);
}

std::vector<TransactionInfo> SyncImpl::getLastTxs() const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getLastTxs();
}

Token SyncImpl::getTokenInfo(const Address &address) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getTokenInfo(address);
}

std::vector<std::pair<Address, DelegateState>> SyncImpl::getDelegateStates(const Address &fromAddress) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getDelegateStates(fromAddress);
}

CommonBalance SyncImpl::commonBalance() const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->commonBalance();
}

ForgingSums SyncImpl::getForgingSumForLastBlock(size_t blockIndent) const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getForgingSumForLastBlock(blockIndent);
}

ForgingSums SyncImpl::getForgingSumAll() const {
    CHECK(mainWorker != nullptr, "Main worker not initialized");
    return mainWorker->getForgingSumAll();
}

size_t SyncImpl::getKnownBlock() const {
    return knownLastBlock.load();
}

V8Details SyncImpl::getContractDetails(const Address &contractAddress) const {
    CHECK(modules[MODULE_V8], MODULE_V8_STR + " module not found");
    CHECK(scriptWorker != nullptr, "Script worker not set");
    return scriptWorker->getContractDetails(contractAddress);
}

V8Code SyncImpl::getContractCode(const Address &contractAddress) const {
    CHECK(modules[MODULE_V8], MODULE_V8_STR + " module not found");
    CHECK(scriptWorker != nullptr, "Script worker not set");
    return scriptWorker->getContractCode(contractAddress);
}

std::pair<size_t, NodeTestResult> SyncImpl::getLastNodeTestResult(const std::string &address) const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->getLastNodeTestResult(address);
}

std::pair<size_t, NodeTestTrust> SyncImpl::getLastNodeTestTrust(const std::string &address) const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->getLastNodeTestTrust(address);
}

NodeTestCount SyncImpl::getLastDayNodeTestCount(const std::string &address) const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->getLastDayNodeTestCount(address);    
}

NodeTestCount SyncImpl::getLastDayNodesTestsCount() const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->getLastDayNodesTestsCount();
}

std::vector<std::pair<std::string, NodeTestExtendedStat>> SyncImpl::filterLastNodes(size_t countTests) const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->filterLastNodes(countTests);
}

std::pair<int, size_t> SyncImpl::calcNodeRaiting(const std::string &address, size_t countTests) const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->calcNodeRaiting(address, countTests);
}

size_t SyncImpl::getLastBlockDay() const {
    CHECK(modules[MODULE_NODE_TEST], "Module " + MODULE_NODE_TEST_STR + " not set");
    CHECK(nodeTestWorker != nullptr, "node test worker not init");
    return nodeTestWorker->getLastBlockDay();
}

}