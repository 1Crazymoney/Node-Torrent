#include "WorkerMain.h"

#include "Cache/Cache.h"
#include "LevelDb.h"
#include "BlockChain.h"

#include "BlockchainRead.h"

#include "parallel_for.h"
#include "stopProgram.h"
#include "duration.h"
#include "check.h"
#include "log.h"
#include "convertStrings.h"
#include "utils/FileSystem.h"

#include "Modules.h"
#include "blockchain_structs/AddressInfo.h"
#include "blockchain_structs/Token.h"
#include "blockchain_structs/TransactionInfo.h"
#include "blockchain_structs/BalanceInfo.h"
#include "blockchain_structs/CommonBalance.h"
#include "blockchain_structs/SignBlock.h"
#include "blockchain_structs/RejectedTxsBlock.h"
#include "MainBlockInfo.h"
#include "blockchain_structs/DelegateState.h"

#include <rapidjson/document.h>

#include <random>

using namespace common;

namespace torrent_node_lib {
   
static const Address ZERO_ADDRESS("0x00000000000000000000000000000000000000000000000000");

template<class RandomIt, class URBG>
inline void partial_shuffle(RandomIt first, RandomIt middle, RandomIt last, URBG&& g) {
    typedef typename std::iterator_traits<RandomIt>::difference_type diff_t;
    typedef std::uniform_int_distribution<diff_t> distr_t;
    typedef typename distr_t::param_type param_t;
    
    distr_t D;
    diff_t n = last - first;
    diff_t to = middle - first;
    for (diff_t i = 0; i < to; ++i) {
        using std::swap;
        swap(first[i], first[D(g, param_t(i, n - 1))]);
    }
}

WorkerMain::WorkerMain(const std::string &folderBlocks, LevelDb &leveldb, AllCaches &caches, BlockChain &blockchain, const std::set<Address> &users, std::mutex &usersMut, int countThreads, bool validateState)
    : folderBlocks(folderBlocks)
    , leveldb(leveldb)
    , caches(caches)
    , blockchain(blockchain)
    , countThreads(countThreads)
    , validateState(validateState)
    , users(users)
    , usersMut(usersMut)
{    
    const MainBlockInfo oldMetadata = leveldb.findMainBlock();
    countVal.store(oldMetadata.countVal);
    
    lastSavedBlock = oldMetadata.blockNumber;
}

WorkerMain::~WorkerMain() {
    try {
        workerThread.join();        
    } catch (...) {
        LOGERR << "Error while stoped thread";
    }
}

void WorkerMain::join() {
    workerThread.join();
}

std::optional<TransactionStatus> WorkerMain::calcTransactionStatusDelegate(const TransactionInfo &tx, size_t blockNumber, DelegateTransactionsCache &delegateCache, Batch &batch) {
    CHECK(tx.delegate.has_value(), "Is not delegate transaction");
    
    TransactionStatus txStatus(tx.hash, blockNumber);
    const std::string &delegateKey = makeKeyDelegatePair(tx.fromAddress.getBinaryString(), tx.toAddress.getBinaryString());
       
    if (tx.delegate.value().isDelegate) {
        txStatus.isSuccess = !tx.isIntStatusNotSuccess();
        txStatus.status = TransactionStatus::Delegate();
        
        if (txStatus.isSuccess) {
            const DelegateState dState(tx.delegate.value().value, tx.hash);
            const auto resultKey = batch.addDelegateKey(delegateKey, dState, countVal.get());
            delegateCache[delegateKey].push(resultKey);
        }
    } else {
        txStatus.isSuccess = true;
        DelegateState dState;
        if (!delegateCache[delegateKey].empty()) {
            const auto delegateKeyS = delegateCache[delegateKey].top();
            delegateCache[delegateKey].pop();
            const std::optional<DelegateState> foundValue = batch.findDelegateKey(delegateKeyS);
            CHECK(foundValue.has_value() && !foundValue.value().hash.empty(), "Incorrect batch delegate");
            dState = foundValue.value();
            batch.removeDelegateKey(delegateKeyS);
        } else {
            const auto &[key, value] = leveldb.findDelegateKey(delegateKey, batch.getDeletedDelegate());
            if (!key.empty()) {
                CHECK(!value.hash.empty(), "Incorrect delegated value");
                dState = value;
                batch.removeDelegateKey(std::vector<char>(key.begin(), key.end()));
            }
        }
        txStatus.status = TransactionStatus::UnDelegate(dState.value, dState.hash);
        if (!dState.hash.empty()) {
            //LOGDEBUG << "Remove delegate " << dState.value << " " << toHex(dState.hash.begin(), dState.hash.end());
        } else {
            //LOGDEBUG << "Remove zero delegate";
        }
    }
    
    return txStatus;
}

static ForgingSums makeForgingSums(const BlockInfo &bi) {
    ForgingSums result;
    
    result.blockNumber = bi.header.blockNumber.value();
    const std::set<uint64_t> forginStatuses = TransactionInfo::getForgingIntStatuses();
    for (const uint64_t status: forginStatuses) {
        result.sums[status] = 0;
    }
    
    for (const TransactionInfo &tx: bi.txs) {
        if (tx.isIntStatusForging()) {
            result.sums[tx.intStatus.value()] += tx.value;
        }
    }
    return result;
}

void WorkerMain::saveTransactionStatus(const TransactionStatus &txStatus, Batch &txsBatch, const std::string &attributeTxStatusCache) {
    txsBatch.addTransactionStatus(txStatus.transaction, txStatus);
    caches.txsStatusCache.addValue(txStatus.transaction, attributeTxStatusCache, txStatus);
}

void WorkerMain::saveTransaction(const TransactionInfo &tx, Batch &txsBatch) {
    txsBatch.addTransaction(tx.hash, tx);
}

void WorkerMain::saveAddressTransaction(const TransactionInfo &tx, const Address &address, Batch &batch) {
    AddressInfo addrInfo(tx.filePos.pos, tx.filePos.fileNameRelative, tx.blockNumber, tx.blockIndex);
    batch.addAddress(address.toBdString(), addrInfo, countVal.get());
}

void WorkerMain::saveAddressTokenTransaction(const TransactionInfo &tx, const Address &address, Batch &batch) {
    AddressInfo addrInfo(tx.filePos.pos, tx.filePos.fileNameRelative, tx.blockNumber, tx.blockIndex);
    batch.addAddressToken(address.toBdString(), addrInfo, countVal.get());
}

void WorkerMain::saveAddressStatus(const TransactionStatus &status, const Address &address, Batch &batch) {
    const std::string addressAndHash = makeAddressStatusKey(address.toBdString(), status.transaction);
    batch.addAddressStatus(addressAndHash, status);
}

void WorkerMain::saveAddressBalance(const TransactionInfo &tx, const Address &address, std::unordered_map<std::string, BalanceInfo> &balances, bool isForging) {
    balances[address.toBdString()].plusWithoutDelegate(tx, address, !tx.isIntStatusNoBalance(), isForging);
}

void WorkerMain::saveAddressBalanceDelegate(const TransactionInfo &tx, const TransactionStatus &status, const Address &address, std::unordered_map<std::string, BalanceInfo> &balances) {
    CHECK(tx.delegate.has_value(), "Transaction not delegate");
    std::optional<int64_t> undelegateValue;
    if (!tx.delegate->isDelegate) {
        CHECK(std::holds_alternative<TransactionStatus::UnDelegate>(status.status), "Transaction status incorrect");
        undelegateValue = std::get<TransactionStatus::UnDelegate>(status.status).value;
    }
    
    balances[address.toBdString()].plusWithDelegate(tx, address, undelegateValue, status.isSuccess);
}

void WorkerMain::saveAddressBalanceCreateToken(const TransactionInfo &tx, std::unordered_map<std::string, BalanceInfo> &balances) {
    if (!tx.tokenInfo.has_value()) {
        return;
    }
    
    if (!std::holds_alternative<TransactionInfo::TokenInfo::Create>(tx.tokenInfo.value().info)) {
        return;
    }
    
    const TransactionInfo::TokenInfo::Create &createTokenInfo = std::get<TransactionInfo::TokenInfo::Create>(tx.tokenInfo.value().info);
    
    size_t rest = createTokenInfo.value;
    for (const auto &[address, value]: createTokenInfo.beginDistribution) {
        balances[address.toBdString()].addTokens(tx, value, !tx.isIntStatusNotSuccess());
        
        rest -= value;
    }

    balances[createTokenInfo.owner.toBdString()].addTokens(tx, rest, !tx.isIntStatusNotSuccess());
}

void WorkerMain::saveAddressBalanceAddToken(const TransactionInfo &tx, std::unordered_map<std::string, BalanceInfo> &balances) {
    if (!tx.tokenInfo.has_value()) {
        return;
    }
    if (!std::holds_alternative<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo.value().info)) {
        return;
    }
    const TransactionInfo::TokenInfo::AddTokens &addTokens = std::get<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo.value().info);
    balances[addTokens.toAddress.toBdString()].addTokens(tx, addTokens.value, !tx.isIntStatusNotSuccess());
}

void WorkerMain::saveAddressBalanceMoveToken(const TransactionInfo &tx, std::unordered_map<std::string, BalanceInfo> &balances) {
    if (!tx.tokenInfo.has_value()) {
        return;
    }
    if (!std::holds_alternative<TransactionInfo::TokenInfo::MoveTokens>(tx.tokenInfo.value().info)) {
        return;
    }
    const TransactionInfo::TokenInfo::MoveTokens &moveTokens = std::get<TransactionInfo::TokenInfo::MoveTokens>(tx.tokenInfo.value().info);
    balances[moveTokens.toAddress.toBdString()].moveTokens(tx, moveTokens.toAddress, moveTokens.toAddress, moveTokens.value, !tx.isIntStatusNotSuccess());
    if (moveTokens.toAddress != tx.fromAddress) {
        balances[tx.fromAddress.toBdString()].moveTokens(tx, tx.fromAddress, moveTokens.toAddress, moveTokens.value, !tx.isIntStatusNotSuccess());
    }
}

void WorkerMain::saveAddressBalanceBurnToken(const TransactionInfo &tx, std::unordered_map<std::string, BalanceInfo> &balances) {
    if (!tx.tokenInfo.has_value()) {
        return;
    }
    if (!std::holds_alternative<TransactionInfo::TokenInfo::BurnTokens>(tx.tokenInfo.value().info)) {
        return;
    }
    const TransactionInfo::TokenInfo::BurnTokens &burnTokens = std::get<TransactionInfo::TokenInfo::BurnTokens>(tx.tokenInfo.value().info);
    balances[tx.fromAddress.toBdString()].moveTokens(tx, tx.fromAddress, ZERO_ADDRESS, burnTokens.value, !tx.isIntStatusNotSuccess());
}

void WorkerMain::processTokenOperation(const Address &token, Batch &txsBatch, const std::function<Token(Token token)> &process) {
    const std::optional<Token> tokenStr1 = txsBatch.findToken(token.toBdString());
    Token tokenInfo;
    if (tokenStr1.has_value()) {
        tokenInfo = tokenStr1.value();
        txsBatch.removeToken(token.toBdString());
    } else {
        tokenInfo = leveldb.findToken(token.toBdString());
    }
       
    if (tokenInfo.type.empty()) {
        return;
    }
    
    const Token newToken = process(tokenInfo);
    
    txsBatch.addToken(token.toBdString(), newToken);
}

void WorkerMain::changeTokenOwner(const TransactionInfo &tx, Batch &txsBatch) {
    if (tx.isIntStatusNotSuccess()) {
        return;
    }
    if (tx.tokenInfo.has_value()) {
        if (std::holds_alternative<TransactionInfo::TokenInfo::ChangeOwner>(tx.tokenInfo.value().info)) {
            const TransactionInfo::TokenInfo::ChangeOwner &changeOwner = std::get<TransactionInfo::TokenInfo::ChangeOwner>(tx.tokenInfo.value().info);
            const Address &token = tx.toAddress;
            
            processTokenOperation(token, txsBatch, [&](Token tokenInfo) {
                tokenInfo.owner = changeOwner.newOwner;
                return tokenInfo;
            });
        }
    }
}

void WorkerMain::changeTokenEmission(const TransactionInfo &tx, Batch &txsBatch) {
    if (tx.isIntStatusNotSuccess()) {
        return;
    }
    if (tx.tokenInfo.has_value()) {
        if (std::holds_alternative<TransactionInfo::TokenInfo::ChangeEmission>(tx.tokenInfo.value().info)) {
            const TransactionInfo::TokenInfo::ChangeEmission &changeEmission = std::get<TransactionInfo::TokenInfo::ChangeEmission>(tx.tokenInfo.value().info);
            const Address &token = tx.toAddress;

            processTokenOperation(token, txsBatch, [&](Token tokenInfo) {
                tokenInfo.emission = changeEmission.newEmission;
                return tokenInfo;
            });
        }
    }
}

void WorkerMain::changeTokenValue(const TransactionInfo &tx, Batch &txsBatch) {
    if (tx.isIntStatusNotSuccess()) {
        return;
    }
    if (tx.tokenInfo.has_value()) {
        if (std::holds_alternative<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo.value().info)) {
            const TransactionInfo::TokenInfo::AddTokens &addTokens = std::get<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo.value().info);
            const Address &token = tx.toAddress;

            processTokenOperation(token, txsBatch, [&](Token tokenInfo) {
                tokenInfo.allValue += addTokens.value;
                return tokenInfo;
            });
        } else if (std::holds_alternative<TransactionInfo::TokenInfo::BurnTokens>(tx.tokenInfo.value().info)) {
            const TransactionInfo::TokenInfo::BurnTokens &burnTokens = std::get<TransactionInfo::TokenInfo::BurnTokens>(tx.tokenInfo.value().info);
            const Address &token = tx.toAddress;
            
            processTokenOperation(token, txsBatch, [&](Token tokenInfo) {
                tokenInfo.allValue -= burnTokens.value;
                return tokenInfo;
            });
        }
    }
}

std::optional<TransactionStatus> WorkerMain::getInstantDelegateStatus(const TransactionInfo &tx, size_t blockNumber, DelegateTransactionsCache &delegateCache, Batch &batch) {
    if (tx.delegate.has_value()) {
        CHECK(tx.intStatus.has_value(), "delegate status not seted");
        return calcTransactionStatusDelegate(tx, blockNumber, delegateCache, batch);
    } else {
        return std::nullopt;
    }
}

void WorkerMain::validateStateBlock(const BlockInfo &bi) const {
    if (!validateState) {
        return;
    }
        
    for (const TransactionInfo &tx: bi.txs) {
        const Address &address = tx.toAddress;

        if (address == ZERO_ADDRESS) {
            continue;
        }
        
        const BalanceInfo balance = leveldb.findBalance(address.toBdString());
        
        std::vector<std::pair<Address, DelegateState>> delegateStates = getDelegateStates(address);
        std::stable_sort(delegateStates.begin(), delegateStates.end(), [](const auto &pair1, const auto &pair2) {
            return pair2.first < pair1.first;
        });
        
        CHECK(balance.calcBalanceWithoutDelegate() == tx.value, "Incorrect state balance value in address " + address.calcHexString() + " " + std::to_string(balance.calcBalanceWithoutDelegate()) + " " + std::to_string(tx.value));
        //CHECK(balance.countSpent + 1 == tx.nonce, "Incorrect state nonce value in address " + address.calcHexString() + " " + std::to_string(balance.countSpent) + " " + std::to_string(tx.nonce));
        
        rapidjson::Document doc;
        const std::string dataStr(tx.data.begin(), tx.data.end());
        const rapidjson::ParseResult pr = doc.Parse(dataStr.c_str());
        if(pr) {
            if (doc.HasMember("delegate_to") && doc["delegate_to"].IsArray()) {
                std::vector<std::pair<Address, DelegateState>> delegateStates2;
                const auto &delegated = doc["delegate_to"].GetArray();
                CHECK(delegateStates.size() == delegated.Size(), "Incorrect state delegate value in address " + address.calcHexString());
                
                for (size_t i = 0; i < delegateStates.size(); i++) {                   
                    const auto &delegatedJson = delegated[i];
                    CHECK(delegatedJson.IsObject(), "Incorrect state delegate value in address " + address.calcHexString());
                    CHECK(delegatedJson.HasMember("a") && delegatedJson["a"].IsString(), "Incorrect state delegate value in address " + address.calcHexString());
                    const std::string addressJson = delegatedJson["a"].GetString();
                    
                    CHECK(delegatedJson.HasMember("v") && delegatedJson["v"].IsInt64(), "Incorrect state delegate value in address " + address.calcHexString());
                    const int64_t valueJson = delegatedJson["v"].GetInt64();
                    
                    delegateStates2.emplace_back(Address(addressJson), DelegateState(valueJson, ""));                  
                }
                
                CHECK(delegateStates.size() == delegateStates2.size(), "Incorrect state delegate value in address " + address.calcHexString());
                std::stable_sort(delegateStates2.begin(), delegateStates2.end(), [](const auto &pair1, const auto &pair2) {
                    return pair1.first < pair2.first;
                });
    
                for (size_t i = 0; i < delegateStates.size(); i++) {
                    const auto &[dAddress, state] = delegateStates[delegateStates.size() - i - 1];
                    const auto &[dAddress2, state2] = delegateStates2[i];
                    
                    CHECK(dAddress2.calcHexString() == dAddress.calcHexString(), "Incorrect state delegate value in address " + address.calcHexString() + " " + dAddress.calcHexString() + " " + dAddress2.calcHexString());
                    CHECK(state.value == state2.value, "Incorrect state delegate value in address " + address.calcHexString());
                }
            }
        }
    }
}

void WorkerMain::worker() {    
    while (true) {
        try {
            std::shared_ptr<BlockInfo> biSP;
            
            const bool isStopped = !queue.pop(biSP);
            if (isStopped) {
                return;
            }
            BlockInfo &bi = *biSP;
            Timer tt;
                        
            const std::string attributeTxStatusCache = std::to_string(bi.header.blockNumber.value());
            
            const MainBlockInfo oldMetadata = leveldb.findMainBlock();
            const std::vector<unsigned char> prevHash = oldMetadata.blockHash;
            
            if (bi.header.blockNumber.value() <= oldMetadata.blockNumber) {
                continue;
            }
            
            CHECK(prevHash.empty() || prevHash == bi.header.prevHash, "Incorrect prev hash. Expected " + toHex(prevHash) + ", received " + toHex(bi.header.prevHash));
            
            CommonBalance commonBalance = leveldb.findCommonBalance();
            const bool updateCommonBalance = commonBalance.blockNumber < bi.header.blockNumber.value();
            
            Batch batch;
            DelegateTransactionsCache delegateCache;
            std::unordered_map<std::string, BalanceInfo> balances;
            if (bi.header.isForgingBlock() || bi.header.isSimpleBlock()) {
                for (const TransactionInfo &tx: bi.txs) {                   
                    const auto toLeveldb = [this, &bi](const Address &address, const TransactionInfo &tx, Batch &batch, std::unordered_map<std::string, BalanceInfo> &balances, const std::optional<TransactionStatus> &statusDelegate) {
                        if (address.isInitialWallet()) {
                            return;
                        }
                        
                        if (tx.isIntStatusNodeTest()) {
                            return;
                        }
                            
                        if (modules[MODULE_ADDR_TXS]) {
                            saveAddressTransaction(tx, address, batch); // TODO Действия по заполнению кэша должны производиться только в основном потоке
                            
                            if (statusDelegate.has_value()) {
                                saveAddressStatus(statusDelegate.value(), address, batch);
                            }
                        }
                        
                        if (modules[MODULE_BALANCE]) {
                            saveAddressBalance(tx, address, balances, bi.header.isForgingBlock());
                            
                            if (tx.delegate.has_value() && statusDelegate.has_value()) {
                                saveAddressBalanceDelegate(tx, statusDelegate.value(), address, balances);
                            }
                        }
                    };
                    
                    if (modules[MODULE_BALANCE] || modules[MODULE_TXS] || modules[MODULE_ADDR_TXS]) {
                        const std::optional<TransactionStatus> txStatusDelegate = getInstantDelegateStatus(tx, bi.header.blockNumber.value(), delegateCache, batch);
                        
                        toLeveldb(tx.fromAddress, tx, batch, balances, txStatusDelegate);
                        if (tx.fromAddress != tx.toAddress) {
                            toLeveldb(tx.toAddress, tx, batch, balances, txStatusDelegate);
                        }
                        
                        if (modules[MODULE_TXS]) {
                            saveTransaction(tx, batch);
                            
                            if (txStatusDelegate.has_value()) {
                                saveTransactionStatus(txStatusDelegate.value(), batch, attributeTxStatusCache);
                            }
                            
                            if (tx.tokenInfo.has_value()) {
                                if (!tx.isIntStatusNotSuccess()) {
                                    if (std::holds_alternative<TransactionInfo::TokenInfo::Create>(tx.tokenInfo->info)) {
                                        const TransactionInfo::TokenInfo::Create &createToken = std::get<TransactionInfo::TokenInfo::Create>(tx.tokenInfo->info);
                                                                                
                                        Token token;
                                        token.type = createToken.type;
                                        token.allValue = createToken.value;
                                        token.beginValue = createToken.value;
                                        token.decimals = createToken.decimals;
                                        token.emission = createToken.emission;
                                        token.name = createToken.name;
                                        token.owner = createToken.owner;
                                        token.symbol = createToken.symbol;
                                        token.txHash = tx.hash;
                                        
                                        batch.addToken(tx.toAddress.toBdString(), token);
                                        
                                        saveAddressTokenTransaction(tx, createToken.owner, batch);
                                        for (const auto& [addr, value]: createToken.beginDistribution) {
                                            saveAddressTokenTransaction(tx, addr, batch);
                                        }
                                    } else if (std::holds_alternative<TransactionInfo::TokenInfo::ChangeOwner>(tx.tokenInfo->info)) {
                                        const TransactionInfo::TokenInfo::ChangeOwner &changeOwner = std::get<TransactionInfo::TokenInfo::ChangeOwner>(tx.tokenInfo->info);
                                        changeTokenOwner(tx, batch);
                                        saveAddressTokenTransaction(tx, tx.fromAddress, batch);
                                        saveAddressTokenTransaction(tx, changeOwner.newOwner, batch);
                                    } else if (std::holds_alternative<TransactionInfo::TokenInfo::ChangeEmission>(tx.tokenInfo->info)) {
                                        changeTokenEmission(tx, batch);
                                        saveAddressTokenTransaction(tx, tx.fromAddress, batch);
                                    } else if (std::holds_alternative<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo->info)) {
                                        const TransactionInfo::TokenInfo::AddTokens &addTokens = std::get<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo->info);
                                        changeTokenValue(tx, batch);
                                        saveAddressTokenTransaction(tx, addTokens.toAddress, batch);
                                    } else if (std::holds_alternative<TransactionInfo::TokenInfo::MoveTokens>(tx.tokenInfo->info)) {
                                        const TransactionInfo::TokenInfo::MoveTokens &moveTokens = std::get<TransactionInfo::TokenInfo::MoveTokens>(tx.tokenInfo->info);
                                        saveAddressTokenTransaction(tx, tx.fromAddress, batch);
                                        if (tx.fromAddress != moveTokens.toAddress) {
                                            saveAddressTokenTransaction(tx, moveTokens.toAddress, batch);
                                        }
                                    } else if (std::holds_alternative<TransactionInfo::TokenInfo::BurnTokens>(tx.tokenInfo->info)) {
                                        changeTokenValue(tx, batch);
                                        saveAddressTokenTransaction(tx, tx.fromAddress, batch);
                                    } else {
                                        throwErr("Unknown token type");
                                    }
                                }
                            }
                        }
                        
                        if (modules[MODULE_BALANCE]) {
                            if (tx.tokenInfo.has_value()) {
                                if (std::holds_alternative<TransactionInfo::TokenInfo::Create>(tx.tokenInfo->info)) {
                                    saveAddressBalanceCreateToken(tx, balances);
                                } else if (std::holds_alternative<TransactionInfo::TokenInfo::AddTokens>(tx.tokenInfo->info)) {
                                    saveAddressBalanceAddToken(tx, balances);
                                } else if (std::holds_alternative<TransactionInfo::TokenInfo::MoveTokens>(tx.tokenInfo->info)) {
                                    saveAddressBalanceMoveToken(tx, balances);
                                } else if (std::holds_alternative<TransactionInfo::TokenInfo::BurnTokens>(tx.tokenInfo->info)) {
                                    saveAddressBalanceBurnToken(tx, balances);
                                }
                            }
                        }
                    }
                    if (modules[MODULE_BLOCK]) {
                        if (updateCommonBalance) {
                            if (tx.fromAddress.isInitialWallet() || bi.header.isForgingBlock()) {
                                commonBalance.money += tx.value;
                                commonBalance.blockNumber = bi.header.blockNumber.value();
                            }
                        }
                    }
                };
            } else if (bi.header.isStateBlock()) {
                validateStateBlock(bi);
            }
            
            if (bi.header.isForgingBlock()) {
                ForgingSums fs = makeForgingSums(bi);
                const ForgingSums oldForgingSums = leveldb.findForgingSumsAll();
                fs += oldForgingSums;
                batch.addAllForgedSums(fs);
            }
            
            if (modules[MODULE_BALANCE]) {
                parallelFor(countThreads, balances.begin(), balances.end(), [this, &batch, &bi](auto balanceIter){
                    BalanceInfo &currBalance = balanceIter.second;
                    const std::string &address = balanceIter.first;
                    const BalanceInfo oldBalance = leveldb.findBalance(address);
                    if (oldBalance.blockNumber < bi.header.blockNumber.value()) {
                        const BalanceInfo newBalance = oldBalance + currBalance;
                        if (newBalance.received() < newBalance.spent()) { 
                            LOGWARN << "Incorrect balance " + toHex(address.begin(), address.end());
                        }
                        batch.addBalance(address, newBalance);
                    }
                });
            }
            
            if (modules[MODULE_BLOCK]) {
                batch.addCommonBalance(commonBalance);
            }
                        
            batch.addMainBlock(MainBlockInfo(bi.header.blockNumber.value(), bi.header.hash, countVal.load()));
            
            addBatch(batch, leveldb);
            
            tt.stop();
            
            LOGINFO << "Block " << bi.header.blockNumber.value() << " saved. Count txs " << bi.txs.size() << ". Time ms " << tt.countMs();
            
            caches.txsStatusCache.remove(std::to_string(bi.header.blockNumber.value() - caches.maxCountElementsTxsCache));
                       
            std::unique_lock<std::mutex> lock(lastTxsMut);
            lastTxs.insert(lastTxs.begin(), bi.txs.begin(), bi.txs.begin() + std::min(size_t(100), bi.txs.size()));
            lastTxs.erase(lastTxs.begin() +  std::min(size_t(100), lastTxs.size()), lastTxs.end()); // Оставляем 100 последних элементов
            lock.unlock();
            
            checkStopSignal();
        } catch (const exception &e) {
            LOGERR << e;
        } catch (const StopException &e) {
            LOGINFO << "Stop processBlockInfo thread";
            return;
        } catch (const std::exception &e) {
            LOGERR << e.what();
        } catch (...) {
            LOGERR << "Unknown error";
        }
    }
}

void WorkerMain::start() {
    workerThread = Thread(&WorkerMain::worker, this);
}

void WorkerMain::process(std::shared_ptr<BlockInfo> bi, std::shared_ptr<std::string> dump) {   
    if (bi->header.blockNumber.value() > lastSavedBlock) {
        queue.push(bi);
    }
}

std::optional<size_t> WorkerMain::getInitBlockNumber() const {
    return lastSavedBlock;
}

std::vector<TransactionInfo> WorkerMain::readTxs(const std::vector<AddressInfo> &foundResults) const {
    std::vector<TransactionInfo> txs;
    std::string currFileName;
    IfStream file;
    for (const AddressInfo &addressInfo: foundResults) {      
        if (addressInfo.filePos.fileNameRelative != currFileName) {
            closeFile(file);
            openFile(file, getFullPath(addressInfo.filePos.fileNameRelative, folderBlocks));
            currFileName = addressInfo.filePos.fileNameRelative;
        }
        TransactionInfo tx;
        const bool res = readOneSimpleTransactionInfo(file, addressInfo.filePos.pos, tx, false);
        CHECK(res, "Incorrect read transaction info");
        tx.blockNumber = addressInfo.blockNumber;
        tx.blockIndex = addressInfo.blockIndex;
        if (addressInfo.undelegateValue.has_value()) {
            CHECK(tx.delegate.has_value() && !tx.delegate->isDelegate, "Incorrect delegate transaction");
            tx.delegate->value = addressInfo.undelegateValue.value();
        }
        txs.emplace_back(tx);
    }
    return txs;
}

std::vector<TransactionInfo> WorkerMain::getTxsForAddressWithoutStatuses(const Address& address, size_t from, size_t count, size_t limitTxs) const {
    const size_t countLimited = std::min((count == 0 ? limitTxs + 10 : count), limitTxs + 10);
    const std::vector<AddressInfo> foundResults = leveldb.findAddress(address.toBdString(), from, countLimited);
    CHECK(foundResults.size() < limitTxs, "Too many transactions in history. Please, request a history with chunks");
    
    std::vector<TransactionInfo> txs = readTxs(foundResults);
    
    std::sort(txs.begin(), txs.end(), [](const TransactionInfo &first, const TransactionInfo &second) {
        return first.blockNumber > second.blockNumber;
    });
    
    return txs;
}

static void filterTxs(std::vector<TransactionInfo> &txs, const TransactionsFilters &filters, const Address &address) {
    txs.erase(std::remove_if(txs.begin(), txs.end(), [&filters, &address](const TransactionInfo &tx) {
        const bool remove = true;
        if (filters.isInput == TransactionsFilters::FilterType::True && tx.toAddress != address) {
            return remove;
        }
        if (filters.isOutput == TransactionsFilters::FilterType::True && tx.fromAddress != address) {
            return remove;
        }
        if (filters.isSuccess == TransactionsFilters::FilterType::True && tx.isIntStatusNotSuccess()) {
            return remove;
        }
        
        if (filters.isTokens == TransactionsFilters::FilterType::True) {
            return !remove;
        }
        
        if (filters.isDelegate == TransactionsFilters::FilterType::None && filters.isForging == TransactionsFilters::FilterType::None && filters.isTest == TransactionsFilters::FilterType::None) {
            return !remove;
        }
        
        if (filters.isDelegate == TransactionsFilters::FilterType::True && tx.delegate.has_value()) {
            return !remove;
        }
        if (filters.isForging == TransactionsFilters::FilterType::True && tx.isIntStatusForging()) {
            return !remove;
        }
        if (filters.isTest == TransactionsFilters::FilterType::True && tx.isIntStatusNodeTest()) {
            return !remove;
        }
        
        return remove;
    }), txs.end());
}

std::vector<TransactionInfo> WorkerMain::getTxsForAddressWithoutStatuses(const Address& address, size_t &from, size_t count, size_t limitTxs, const TransactionsFilters &filters) const {
    const size_t countLimited = limitTxs;
    std::vector<TransactionInfo> result;
    while (true) {
        std::vector<AddressInfo> foundResults;
        if (filters.isTokens == TransactionsFilters::FilterType::True) {
            foundResults = leveldb.findAddressTokens(address.toBdString(), from, countLimited - result.size());
        } else {
            foundResults = leveldb.findAddress(address.toBdString(), from, countLimited - result.size());
        }
        if (foundResults.empty()) {
            break;
        }
        CHECK(foundResults.size() <= limitTxs, "Too many transactions in history. Please, request a history with chunks"); // TODO remove
        std::vector<TransactionInfo> txs = readTxs(foundResults);

        filterTxs(txs, filters, address);
        result.insert(result.end(), txs.begin(), txs.end());
        
        from += foundResults.size();
        
        if (result.size() >= count) {
            break;
        }
    }
    result.resize(std::min(result.size(), count));
    
    std::sort(result.begin(), result.end(), [](const TransactionInfo &first, const TransactionInfo &second) {
        return first.blockNumber > second.blockNumber;
    });
    
    return result;
}

std::vector<TransactionStatus> WorkerMain::getStatusesForAddress(const Address& address) const {
    return leveldb.findAddressStatus(address.toBdString());
}

std::vector<TransactionInfo> WorkerMain::getTransactionsFillCache(const Address &address, size_t from, size_t count, size_t limitTxs) const {
    CHECK(modules[MODULE_ADDR_TXS], "module " + MODULE_ADDR_TXS_STR + " not set");
    
    return getTxsForAddressWithoutStatuses(address, from, count, limitTxs);
}

std::vector<TransactionStatus> WorkerMain::getTransactionsStatusFillCache(const Address &address) const {
    CHECK(modules[MODULE_ADDR_TXS], "module " + MODULE_ADDR_TXS_STR + " not set");
    
    return getStatusesForAddress(address);
}

std::vector<TransactionInfo> WorkerMain::getTxsForAddress(const Address &address, size_t from, size_t count, size_t limitTxs) const {
    CHECK(modules[MODULE_ADDR_TXS], "module " + MODULE_ADDR_TXS_STR + " not set");
    
    std::vector<TransactionInfo> txs = getTransactionsFillCache(address, from, count, limitTxs);
    const std::vector<TransactionStatus> statusesVect = getTransactionsStatusFillCache(address);
    
    std::unordered_map<std::string, TransactionStatus> statuses;
    for (const TransactionStatus &status: statusesVect) {
        statuses.emplace(status.transaction, status);
    }
    for (TransactionInfo &tx: txs) {
        if (tx.isStatusNeed()) {
            const auto found = statuses.find(tx.hash);
            if (found != statuses.end()) {
                tx.status = found->second;
            }
        }
    }
    
    return txs;
}

std::vector<TransactionInfo> WorkerMain::getTxsForAddress(const Address &address, size_t &from, size_t count, size_t limitTxs, const TransactionsFilters &filters) const {
    CHECK(modules[MODULE_ADDR_TXS], "module " + MODULE_ADDR_TXS_STR + " not set");
    
    std::vector<TransactionInfo> txs = getTxsForAddressWithoutStatuses(address, from, count, limitTxs, filters);
    const std::vector<TransactionStatus> statusesVect = getTransactionsStatusFillCache(address);
    
    std::unordered_map<std::string, TransactionStatus> statuses;
    for (const TransactionStatus &status: statusesVect) {
        statuses.emplace(status.transaction, status);
    }
    for (TransactionInfo &tx: txs) {
        if (tx.isStatusNeed()) {
            const auto found = statuses.find(tx.hash);
            if (found != statuses.end()) {
                tx.status = found->second;
            }
        }
    }
    
    return txs;
}

void WorkerMain::readTransactionInFile(TransactionInfo& tx) const {
    IfStream file;
    const std::string hash = tx.hash;
    openFile(file, getFullPath(tx.filePos.fileNameRelative, folderBlocks));
    const bool res = readOneSimpleTransactionInfo(file, tx.filePos.pos, tx, false);
    CHECK(res, "Incorrect read transaction info");
    CHECK(hash == tx.hash, "Incorrect transaction");
}

std::optional<TransactionInfo> WorkerMain::findTransaction(const std::string &txHash) const {
    CHECK(modules[MODULE_TXS], "module " + MODULE_ADDR_TXS_STR + " not set");
    
    const std::optional<TransactionInfo> cache = caches.txsCache.getValue(txHash);
    if (!cache.has_value()) {
        const std::optional<TransactionInfo> found = leveldb.findTx(txHash);
        if (!found.has_value()) {
            return std::nullopt;
        }
        TransactionInfo txInfo = found.value();
        txInfo.hash = txHash;
        readTransactionInFile(txInfo);
        return txInfo;
    } else {
        return *cache;
    }
}

void WorkerMain::fillStatusTransaction(TransactionInfo &info) const {
    if (info.isStatusNeed()) {
        const std::optional<TransactionStatus> cacheStatus = caches.txsStatusCache.getValue(info.hash);
        if (!cacheStatus.has_value()) {
            const std::optional<TransactionStatus> foundStatus = leveldb.findTxStatus(info.hash);
            if (foundStatus.has_value()) {
                info.status = foundStatus.value();
            }
        } else {
            info.status = cacheStatus.value();
        }
    }
}

std::optional<TransactionInfo> WorkerMain::getTransaction(const std::string &txHash) const {
    CHECK(modules[MODULE_TXS], "module " + MODULE_ADDR_TXS_STR + " not set");
    
    std::optional<TransactionInfo> result = findTransaction(txHash);
    if (!result.has_value()) {
        return std::nullopt;
    }
    
    fillStatusTransaction(result.value());
    
    return result;
}

BalanceInfo WorkerMain::readBalance(const Address& address) const {
    const std::string &addressStr = address.toBdString();
    return leveldb.findBalance(addressStr);
}

BalanceInfo WorkerMain::getBalance(const Address &address) const {
    BalanceInfo balance;
    
    CHECK(modules[MODULE_BALANCE], "Module " + MODULE_BALANCE_STR + " not setted");
    balance = readBalance(address);
    
    if (address == ZERO_ADDRESS) {
        balance.balance.fill(0, 0);
    }
    
    std::vector<char> toHashString;
    balance.serialize(toHashString);
    balance.hash = std::hash<std::vector<char>>()(toHashString);
    
    return balance;
}

BlockInfo WorkerMain::getFullBlock(const BlockHeader &bh, size_t beginTx, size_t countTx) const {
    CHECK(modules[MODULE_BLOCK_RAW], "module " + MODULE_BLOCK_RAW_STR + " not set");
    
    BlockInfo bi;
    if (bh.blockNumber == 0) {
        return bi;
    }
    
    const std::optional<std::shared_ptr<std::string>> cache = caches.blockDumpCache.getValue(HashedString(bh.hash.data(), bh.hash.size()));
    if (!cache.has_value()) {
        CHECK(!bh.filePos.fileNameRelative.empty(), "Empty file name in block header");
        IfStream file;
        openFile(file, getFullPath(bh.filePos.fileNameRelative, folderBlocks));
        std::string tmp;
        const size_t nextPos = readNextBlockDump(file, bh.filePos.pos, tmp);
        std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsMinimumBlockHeader> b =
            parseNextBlockInfo(tmp.data(), tmp.data() + tmp.size(), bh.filePos.pos, false, false, beginTx, countTx);
        bi = std::get<BlockInfo>(b);
        CHECK(nextPos != bh.filePos.pos, "Ups");
    } else {
        std::shared_ptr<std::string> element = cache.value();
        std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsMinimumBlockHeader> b =
            parseNextBlockInfo(element->data(), element->data() + element->size(), bh.filePos.pos, true, false, beginTx, countTx);
        bi = std::get<BlockInfo>(b);
    }
    
    bi.header = bh;
    
    for (TransactionInfo &tx: bi.txs) {
        tx.blockNumber = bi.header.blockNumber.value();
        fillStatusTransaction(tx);
    }
    
    return bi;
}


std::vector<std::pair<Address, DelegateState>> WorkerMain::getDelegateStates(const Address &fromAddress) const {
    const std::vector<std::pair<std::string, std::string>> result = leveldb.findAllDelegatedPairKeys(fromAddress.getBinaryString());
    std::vector<std::pair<Address, DelegateState>> r;
    r.reserve(result.size());
    std::transform(result.begin(), result.end(), std::back_inserter(r), [&fromAddress](const std::pair<std::string, std::string> &element) {
        const std::string secondAddress = getSecondOnKeyDelegatePair(fromAddress.getBinaryString(), element.first);
        CHECK(!secondAddress.empty(), "Delegate pair address empty");
        return std::make_pair(Address(secondAddress.begin(), secondAddress.end()), DelegateState::deserialize(element.second));
    });
    return r;
}

CommonBalance WorkerMain::commonBalance() const {
    return leveldb.findCommonBalance();
}

ForgingSums WorkerMain::getForgingSumForLastBlock(size_t blockIndent) const {
    size_t num = 0;
    size_t currBlock = blockchain.countBlocks();
    
    std::optional<BlockHeader> bh;
    while (true) {
        if (currBlock == 0) {
            break;
        }
        
        if (blockchain.getBlock(currBlock).isForgingBlock()) {
            if (num == blockIndent) {
                bh = blockchain.getBlock(currBlock);
                break;
            } else {
                num++;
            }
        }
        currBlock--;
    }
    
    if (!bh.has_value()) {
        return ForgingSums();
    }

    const BlockInfo bi = getFullBlock(bh.value(), 0, 0);
    return makeForgingSums(bi);
}

ForgingSums WorkerMain::getForgingSumAll() const {
    return leveldb.findForgingSumsAll();
}

Token WorkerMain::getTokenInfo(const Address &address) const {
    return leveldb.findToken(address.toBdString());
}

std::vector<TransactionInfo> WorkerMain::getLastTxs() const {
    std::lock_guard<std::mutex> lock(lastTxsMut);
    return lastTxs;
}

std::vector<Address> WorkerMain::getRandomAddresses(size_t countAddresses) const {
    const BlockHeader bh = blockchain.getLastStateBlock();
    BlockInfo bi = getFullBlock(bh, 0, std::numeric_limits<size_t>::max());
    std::vector<TransactionInfo> &txs = bi.txs;
    
    std::random_device rd;
    std::mt19937 g(rd());
    
    auto begin = txs.begin() + bi.header.countSignTx;
    auto middle = begin + std::min(countAddresses, (size_t)std::distance(begin, txs.end()));
    partial_shuffle(begin, middle, txs.end(), g);
    
    std::vector<Address> result;
    std::transform(begin, middle, std::back_inserter(result), std::mem_fn(&TransactionInfo::toAddress));
    
    return result;
}

}
