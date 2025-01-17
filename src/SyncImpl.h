#ifndef SYNC_IMPL_H_
#define SYNC_IMPL_H_

#include <atomic>
#include <memory>

#include "Cache/Cache.h"
#include "LevelDb.h"
#include "BlockChain.h"
#include "BlocksTimeline.h"

#include "ConfigOptions.h"

#include "P2P/P2P.h"

#include "TestP2PNodes.h"
#include "blockchain_structs/Token.h"
#include "blockchain_structs/TransactionInfo.h"
#include "blockchain_structs/BalanceInfo.h"
#include "blockchain_structs/CommonBalance.h"
#include "blockchain_structs/SignBlock.h"
#include "blockchain_structs/RejectedTxsBlock.h"
#include "blockchain_structs/DelegateState.h"

namespace torrent_node_lib {

extern bool isInitialized;

class Worker;
class WorkerCache;
class WorkerScript;
class WorkerNodeTest;
class WorkerMain;
class BlockSource;
class PrivateKey;
class RejectedTxsWorker;

struct V8Details;
struct V8Code;
struct NodeTestResult;
struct NodeTestTrust;
struct NodeTestExtendedStat;
struct RejectedBlockResult;
struct NodeTestCount2;

class FileBlockSource;
class FileRejectedBlockSource;

struct TransactionsFilters;
struct Token;

class RejectedBlockSource;

struct ConflictBlocksInfo;

class SyncImpl {  
public:
    
    SyncImpl(const std::string &folderBlocks, const std::string &technicalAddress, const LevelDbOptions &leveldbOpt, const CachesOptions &cachesOpt, const GetterBlockOptions &getterBlocksOpt, const std::string &signKeyName, const TestNodesOptions &testNodesOpt, bool validateStates);
       
    const BlockChainReadInterface &getBlockchain() const {
        return blockchain;
    }
    
    void setLeveldbOptScript(const LevelDbOptions &leveldbOpt);
    
    void setLeveldbOptNodeTest(const LevelDbOptions &leveldbOpt);
    
    ~SyncImpl();
    
private:
    
    void initialize();
    
    std::vector<Worker*> makeWorkers();
    
    [[nodiscard]] std::optional<ConflictBlocksInfo> process(const std::vector<Worker*> &workers);
    
public:
    
    [[nodiscard]] std::optional<ConflictBlocksInfo> synchronize(int countThreads);
       
    std::vector<TransactionInfo> getTxsForAddress(const Address &address, size_t from, size_t count, size_t limitTxs) const;
    
    std::vector<TransactionInfo> getTxsForAddress(const Address &address, size_t &from, size_t count, size_t limitTxs, const TransactionsFilters &filters) const;
    
    std::optional<TransactionInfo> getTransaction(const std::string &txHash) const;
    
    BalanceInfo getBalance(const Address &address) const;
    
    std::string getBlockDump(const std::vector<unsigned char> &hash, const FilePosition &filePos, size_t fromByte, size_t toByte, bool isHex, bool isSign) const;
    
    BlockInfo getFullBlock(const BlockHeader &bh, size_t beginTx, size_t countTx) const;
    
    std::vector<TransactionInfo> getLastTxs() const;
    
    Token getTokenInfo(const Address &address) const;
    
    size_t getKnownBlock() const;
        
    std::vector<std::pair<Address, DelegateState>> getDelegateStates(const Address &fromAddress) const;
    
    V8Details getContractDetails(const Address &contractAddress) const;
    
    CommonBalance commonBalance() const;
    
    V8Code getContractCode(const Address &contractAddress) const;
    
    ForgingSums getForgingSumForLastBlock(size_t blockIndent) const;
    
    ForgingSums getForgingSumAll() const;
    
    std::pair<size_t, NodeTestResult> getLastNodeTestResult(const std::string &address) const;
    
    std::pair<size_t, NodeTestTrust> getLastNodeTestTrust(const std::string &address) const;
    
    NodeTestCount2 getLastDayNodeTestCount(const std::string &address) const;
    
    std::vector<std::pair<std::string, NodeTestExtendedStat>> filterLastNodes(size_t countTests) const;
    
    std::pair<int, size_t> calcNodeRaiting(const std::string &address, size_t countTests) const;
    
    size_t getLastBlockDay() const;
    
    bool verifyTechnicalAddressSign(const std::string &binary, const std::vector<unsigned char> &signature, const std::vector<unsigned char> &pubkey) const;
    
    std::vector<Address> getRandomAddresses(size_t countAddresses) const;
    
    std::vector<SignTransactionInfo> findSignBlock(const BlockHeader &bh) const;
    
    std::vector<MinimumSignBlockHeader> getSignaturesBetween(const std::optional<std::vector<unsigned char>> &firstBlock, const std::optional<std::vector<unsigned char>> &secondBlock) const;
    
    std::optional<MinimumSignBlockHeader> findSignature(const std::vector<unsigned char> &hash) const;

    std::optional<RejectedTransactionHistory> findRejectedTx(const std::vector<unsigned char> &txHash) const;

    std::vector<std::string> getRejectedDumps(const std::vector<std::vector<unsigned char>> &hashes) const;

    std::vector<RejectedBlockResult> calcLastRejectedBlocks(size_t count) const;

private:
   
    void saveTransactions(BlockInfo &bi, const std::string &binaryDump, bool saveBlockToFile);
    
    void saveTransactionsSignBlock(SignBlockInfo &bi, const std::string &binaryDump, bool saveBlockToFile);
        
    void saveBlockToLeveldb(const BlockInfo &bi, size_t timeLineKey, const std::vector<char> &timelineElement);
    
    void saveSignBlockToLeveldb(const SignBlockInfo &bi, size_t timeLineKey, const std::vector<char> &timelineElement);

    SignBlockInfo readSignBlockInfo(const MinimumSignBlockHeader &header) const;
    
    std::optional<ConflictBlocksInfo> findCommonAncestor();
    
private:
    
    LevelDb leveldb;
        
    LevelDbOptions leveldbOptScript;
    
    LevelDbOptions leveldbOptNodeTest;
    
    const std::string folderBlocks;
    
    const std::string technicalAddress;
    
    mutable AllCaches caches;
    
    BlockChain blockchain;
        
    BlocksTimeline timeline;
    
    int countThreads;
        
    std::unique_ptr<BlockSource> getBlockAlgorithm;
    
    std::unique_ptr<FileRejectedBlockSource> fileRejectedBlockSource;
    std::unique_ptr<FileBlockSource> fileBlockAlgorithm;
    
    std::set<Address> users;
    mutable std::mutex usersMut;
    
    bool isSaveBlockToFiles;
    
    const bool isValidate;

    const bool validateStates;
    
    std::atomic<size_t> knownLastBlock = 0;
    
    std::unique_ptr<WorkerCache> cacheWorker;
    std::unique_ptr<WorkerScript> scriptWorker;
    std::unique_ptr<WorkerNodeTest> nodeTestWorker;
    std::unique_ptr<WorkerMain> mainWorker;

    std::unique_ptr<RejectedTxsWorker> rejectedTxsWorker;

    std::unique_ptr<PrivateKey> privateKey;
        
    TestP2PNodes testNodes;

    std::unique_ptr<RejectedBlockSource> rejectedBlockSource;

    P2P* p2pAll;
};

}

#endif // SYNC_IMPL_H_
