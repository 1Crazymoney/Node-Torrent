#include "synchronize_blockchain.h"

#include "check.h"

#include "BlockchainUtils.h"
#include "BlockchainRead.h"

#include "SyncImpl.h"

#include "Workers/ScriptBlockInfo.h"
#include "Workers/NodeTestsBlockInfo.h"
#include "RejectedBlockSource/RejectedBlockSource.h"

using namespace common;

namespace torrent_node_lib {

struct V8Details;

void initBlockchainUtils() {
    initBlockchainUtilsImpl();

    isInitialized = true;
}

Sync::Sync(const std::string& folderPath, const std::string &technicalAddress, const LevelDbOptions& leveldbOpt, const CachesOptions& cachesOpt, const GetterBlockOptions &getterBlocksOpt, const std::string &signKeyName, const TestNodesOptions &testNodesOpt, bool validateStates)
    : impl(std::make_unique<SyncImpl>(folderPath, technicalAddress, leveldbOpt, cachesOpt, getterBlocksOpt, signKeyName, testNodesOpt, validateStates))
{}

void Sync::setLeveldbOptScript(const LevelDbOptions &leveldbOptScript) {
    impl->setLeveldbOptScript(leveldbOptScript);
}

void Sync::setLeveldbOptNodeTest(const LevelDbOptions &leveldbOptScript) {
    impl->setLeveldbOptNodeTest(leveldbOptScript);
}

BalanceInfo Sync::getBalance(const Address& address) const {
    return impl->getBalance(address);
}

std::string Sync::getBlockDump(const std::vector<unsigned char> &hash, const FilePosition &filePos, size_t fromByte, size_t toByte, bool isHex, bool isSign) const {
    return impl->getBlockDump(hash, filePos, fromByte, toByte, isHex, isSign);
}

BlockInfo Sync::getFullBlock(const BlockHeader& bh, size_t beginTx, size_t countTx) const {
    return impl->getFullBlock(bh, beginTx, countTx);
}

size_t Sync::getKnownBlock() const {
    return impl->getKnownBlock();
}

std::vector<TransactionInfo> Sync::getLastTxs() const {
    return impl->getLastTxs();
}

std::optional<TransactionInfo> Sync::getTransaction(const std::string& txHash) const {
    return impl->getTransaction(txHash);
}

Token Sync::getTokenInfo(const Address &address) const {
    return impl->getTokenInfo(address);
}

std::vector<TransactionInfo> Sync::getTxsForAddress(const Address& address, size_t from, size_t count, size_t limitTxs) const {
    return impl->getTxsForAddress(address, from, count, limitTxs);
}

std::vector<TransactionInfo> Sync::getTxsForAddress(const Address& address, size_t &from, size_t count, size_t limitTxs, const TransactionsFilters &filters) const {
    return impl->getTxsForAddress(address, from, count, limitTxs, filters);
}

std::optional<ConflictBlocksInfo> Sync::synchronize(int countThreads) {
    return impl->synchronize(countThreads);
}

const BlockChainReadInterface & Sync::getBlockchain() const {
    return impl->getBlockchain();
}

std::vector<std::pair<Address, DelegateState>> Sync::getDelegateStates(const Address &fromAddress) const {
    return impl->getDelegateStates(fromAddress);
}

V8Details Sync::getContractDetails(const Address &contractAddress) const {
    return impl->getContractDetails(contractAddress);
}

CommonBalance Sync::getCommonBalance() const {
    return impl->commonBalance();
}

V8Code Sync::getContractCode(const Address &contractAddress) const {
    return impl->getContractCode(contractAddress);
}

ForgingSums Sync::getForgingSumForLastBlock(size_t blockIndent) const {
    return impl->getForgingSumForLastBlock(blockIndent);
}

ForgingSums Sync::getForgingSumAll() const {
    return impl->getForgingSumAll();
}

std::pair<size_t, NodeTestResult> Sync::getLastNodeTestResult(const std::string &address) const {
    return impl->getLastNodeTestResult(address);
}

std::pair<size_t, NodeTestTrust> Sync::getLastNodeTestTrust(const std::string &address) const {
    return impl->getLastNodeTestTrust(address);
}

NodeTestCount2 Sync::getLastDayNodeTestCount(const std::string &address) const {
    return impl->getLastDayNodeTestCount(address);
}

std::vector<std::pair<std::string, NodeTestExtendedStat>> Sync::filterLastNodes(size_t countTests) const {
    return impl->filterLastNodes(countTests);
}

std::pair<int, size_t> Sync::calcNodeRaiting(const std::string &address, size_t countTests) const {
    return impl->calcNodeRaiting(address, countTests);
}

size_t Sync::getLastBlockDay() const {
    return impl->getLastBlockDay();
}

bool Sync::verifyTechnicalAddressSign(const std::string &binary, const std::vector<unsigned char> &signature, const std::vector<unsigned char> &pubkey) const {
    return impl->verifyTechnicalAddressSign(binary, signature, pubkey);
}

std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsMinimumBlockHeader> Sync::parseBlockDump(const std::string &binaryDump, bool isValidate) {
    const std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsMinimumBlockHeader> b =
        parseNextBlockInfo(binaryDump.data(), binaryDump.data() + binaryDump.size(), 0, isValidate, true, 0, 0);
    return b;
}

std::vector<Address> Sync::getRandomAddresses(size_t countAddresses) const {
    return impl->getRandomAddresses(countAddresses);
}

std::vector<SignTransactionInfo> Sync::findSignBlock(const BlockHeader &bh) const {
    return impl->findSignBlock(bh);
}

std::vector<MinimumSignBlockHeader> Sync::getSignaturesBetween(const std::optional<std::vector<unsigned char>> &firstBlock, const std::optional<std::vector<unsigned char>> &secondBlock) const {
    return impl->getSignaturesBetween(firstBlock, secondBlock);
}

std::optional<MinimumSignBlockHeader> Sync::findSignature(const std::vector<unsigned char> &hash) const {
    return impl->findSignature(hash);
}

std::optional<RejectedTransactionHistory> Sync::findRejectedTx(const std::vector<unsigned char> &txHash) const {
    return impl->findRejectedTx(txHash);
}

std::vector<std::string> Sync::getRejectedDumps(const std::vector<std::vector<unsigned char>> &hashes) const {
    return impl->getRejectedDumps(hashes);
}

std::vector<RejectedBlockResult> Sync::calcLastRejectedBlocks(size_t count) const {
    return impl->calcLastRejectedBlocks(count);
}

Sync::~Sync() = default;

}
