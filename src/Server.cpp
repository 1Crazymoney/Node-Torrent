#include "Server.h"

#include <string_view>
#include <variant>

#include "synchronize_blockchain.h"
#include "blockchain_structs/BlockInfo.h"
#include "Workers/ScriptBlockInfo.h"
#include "Workers/NodeTestsBlockInfo.h"

#include "check.h"
#include "duration.h"
#include "stringUtils.h"
#include "log.h"
#include "jsonUtils.h"
#include "convertStrings.h"

#include "TransactionFilters.h"

#include "BlockChainReadInterface.h"

#include "cmake_modules/GitSHA1.h"

#include "generate_json.h"

#include "stopProgram.h"
#include "utils/SystemInfo.h"
#include "blockchain_structs/Token.h"
#include "blockchain_structs/TransactionInfo.h"
#include "blockchain_structs/BalanceInfo.h"
#include "blockchain_structs/SignBlock.h"
#include "blockchain_structs/Address.h"
#include "blockchain_structs/DelegateState.h"
#include "blockchain_structs/CommonBalance.h"
#include "blockchain_structs/RejectedTxsBlock.h"

#include "RejectedBlockSource/RejectedBlockSource.h"

using namespace common;
using namespace torrent_node_lib;

const static std::string GET_ADDRESS_HISTORY = "fetch-history";
const static std::string GET_ADDRESS_HISTORY_FILTER = "fetch-history-filter";
const static std::string GET_ADDRESS_BALANCE = "fetch-balance";
const static std::string GET_ADDRESS_TOKENS = "address-get-tokens";
const static std::string GET_ADDRESS_BALANCES = "fetch-balances";
const static std::string GET_BLOCK_BY_HASH = "get-block-by-hash";
const static std::string GET_BLOCK_BY_NUMBER = "get-block-by-number";
const static std::string GET_BLOCKS = "get-blocks";
const static std::string GET_LAST_TXS = "get-last-txs";
const static std::string GET_COUNT_BLOCKS = "get-count-blocks";
const static std::string PRE_LOAD_BLOCKS = "pre-load";
const static std::string GET_DUMP_BLOCK_BY_HASH = "get-dump-block-by-hash";
const static std::string GET_DUMP_BLOCK_BY_NUMBER = "get-dump-block-by-number";
const static std::string GET_DUMPS_BLOCKS_BY_HASH = "get-dumps-blocks-by-hash";
const static std::string GET_DUMPS_BLOCKS_BY_NUMBER = "get-dumps-blocks-by-number";
const static std::string GET_TRANSACTION_INFO = "get-tx";
const static std::string GET_TOKEN_INFO = "token-get-info";
const static std::string GET_TRANSACTIONS_INFO = "get-txs";
const static std::string GET_ADDRESS_DELEGATIONS = "get-address-delegations";
const static std::string GET_CONTRACT_DETAILS = "get-contract-details";
const static std::string GET_CONTRACT_CODE = "get-contract-code";
const static std::string GET_COMMON_BALANCE = "get-common-balance";
const static std::string GET_FORGING_SUM = "get-forging-sum";
const static std::string GET_FORGING_SUM_ALL = "get-forging-sum-all";
const static std::string GET_LAST_NODE_STAT_RESULT = "get-last-node-stat-result";
const static std::string GET_LAST_NODE_STAT_TRUST = "get-last-node-stat-trust";
const static std::string GET_LAST_NODE_STAT_COUNT = "get-last-node-stat-count";
const static std::string GET_ALL_LAST_NODES_RESULT = "get-all-last-nodes-count";
const static std::string GET_NODE_RAITING = "get-nodes-raiting";
const static std::string GET_RANDOM_ADDRESSES = "get-random-addresses";
const static std::string GET_REJECTED_TX_INFO = "get-rejected-tx";
const static std::string GET_REJECTED_BLOCKS = "get-rejected-blocks";
const static std::string GET_REJECTED_DUMPS = "get-rejected-dumps";

const static int64_t MAX_BATCH_BLOCKS = 1000;
const static size_t MAX_BATCH_TXS = 10000;
const static size_t MAX_BATCH_BALANCES = 10000;
const static size_t MAX_HISTORY_SIZE = 10000;
const static size_t MAX_BATCH_DUMPS = 1000;
const static size_t MAX_REJECTED_BLOCKS = 200;
const static size_t MAX_PRELOAD_BLOCKS = 10;

const static int HTTP_STATUS_OK = 200;
const static int HTTP_STATUS_METHOD_NOT_ALLOWED = 405;
const static int HTTP_STATUS_BAD_REQUEST = 400;
const static int HTTP_STATUS_NO_CONTENT = 204;
const static int HTTP_STATUS_INTERNAL_SERVER_ERROR = 500;

struct IncCountRunningThread {
  
    IncCountRunningThread(std::atomic<int> &countRunningThreads)
        : countRunningThreads(countRunningThreads)
    {
        countRunningThreads++;
    }
    
    ~IncCountRunningThread() {
        countRunningThreads--;
    }
    
    std::atomic<int> &countRunningThreads;
    
};

template<typename T>
std::string to_string(const T &value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return value;
    } else {
        return std::to_string(value);
    }
}

TransactionsFilters parseFilters(const rapidjson::Value &v) {
    TransactionsFilters res;
    const auto &filtersJson = get<JsonObject>(v);
    
    const auto processFilter = [&filtersJson](const std::string &name, TransactionsFilters::FilterType &type){
        const std::optional<bool> isFilter = getOpt<bool>(filtersJson, name);
        if (isFilter.has_value()) {
            type = *isFilter ? TransactionsFilters::FilterType::True : TransactionsFilters::FilterType::False;
        }
    };
    
    processFilter("isInput", res.isInput);
    processFilter("isOutput", res.isOutput);
    processFilter("isForging", res.isForging);
    processFilter("isTest", res.isTest);
    processFilter("isSuccess", res.isSuccess);
    processFilter("isDelegate", res.isDelegate);
    processFilter("isToken", res.isTokens);
    
    return res;
}

template<typename T>
std::string getBlock(const RequestId &requestId, const rapidjson::Document &doc, const std::string &nameParam, const Sync &sync, bool isFormat, const JsonVersion &version) {   
    const auto &jsonParams = get<JsonObject>(doc, "params");
    const T &hashOrNumber = get<T>(jsonParams, nameParam);
    
    BlockTypeInfo type = BlockTypeInfo::Simple;
    if (jsonParams.HasMember("type") && jsonParams["type"].IsInt()) {
        const int typeInt = jsonParams["type"].GetInt();
        if (typeInt == 0) {
            type = BlockTypeInfo::Simple;
        } else if (typeInt == 4) {
            type = BlockTypeInfo::ForP2P;
        } else if (typeInt == 1) {
            type = BlockTypeInfo::Hashes;
        } else if (typeInt == 2) {
            type = BlockTypeInfo::Full;
        } else if (typeInt == 3) {
            type = BlockTypeInfo::Small;
        }
    } else if (jsonParams.HasMember("type") && jsonParams["type"].IsString()) {
        const std::string typeString = jsonParams["type"].GetString();
        if (typeString == "simple") {
            type = BlockTypeInfo::Simple;
        } else if (typeString == "forP2P") {
            type = BlockTypeInfo::ForP2P;
        } else if (typeString == "hashes") {
            type = BlockTypeInfo::Hashes;
        } else if (typeString == "full") {
            type = BlockTypeInfo::Full;
        } else if (typeString == "small") {
            type = BlockTypeInfo::Small;
        }
    }
        
    const size_t countTxs = getOpt<int>(jsonParams, "countTxs", 0);
    const size_t beginTx = getOpt<int>(jsonParams, "beginTx", 0);
    
    const BlockHeader bh = sync.getBlockchain().getBlock(hashOrNumber);
    
    if (!bh.blockNumber.has_value()) {
        return genErrorResponse(requestId, -32603, "block " + to_string(hashOrNumber) + " not found");
    }
    
    if (type == BlockTypeInfo::Simple) {
        const BlockHeader nextBh = sync.getBlockchain().getBlock(*bh.blockNumber + 1);
        std::variant<std::vector<TransactionInfo>, std::vector<SignTransactionInfo>> signs;
        if (nextBh.blockNumber.has_value() && nextBh.countSignTx) {
            const BlockInfo nextBi = sync.getFullBlock(nextBh, 0, nextBh.countSignTx);
            signs = nextBi.getBlockSignatures();
        } else {
            signs = sync.findSignBlock(bh);
        }
        return blockHeaderToJson(requestId, bh, signs, isFormat, type, version);
    } else if (type == BlockTypeInfo::ForP2P) {
        std::vector<std::vector<unsigned char>> prevSignatures;
        if (bh.blockNumber.has_value()) {
            const std::vector<MinimumSignBlockHeader> blockSignatures = sync.getSignaturesBetween(std::nullopt, bh.hash);
            CHECK(blockSignatures.size() <= 10, "Too many block signatures");
            std::transform(blockSignatures.begin(), blockSignatures.end(), std::back_inserter(prevSignatures), std::mem_fn(&MinimumSignBlockHeader::hash));
        }
        
        std::vector<std::vector<unsigned char>> nextSignatures;
        if (bh.blockNumber.has_value()) {
            const std::vector<MinimumSignBlockHeader> blockSignatures = sync.getSignaturesBetween(bh.hash, std::nullopt);
            CHECK(blockSignatures.size() <= 10, "Too many block signatures");
            std::transform(blockSignatures.begin(), blockSignatures.end(), std::back_inserter(nextSignatures), std::mem_fn(&MinimumSignBlockHeader::hash));
        }
        
        return blockHeaderToP2PJson(requestId, bh, prevSignatures, nextSignatures, isFormat, type, version);
    } else if (type == BlockTypeInfo::Small) {
        return blockHeaderToJson(requestId, bh, {}, isFormat, type, version);
    } else {
        const BlockHeader nextBh = sync.getBlockchain().getBlock(*bh.blockNumber + 1);
        std::variant<std::vector<TransactionInfo>, std::vector<SignTransactionInfo>> signs;
        if (nextBh.blockNumber.has_value() && nextBh.countSignTx) {
            const BlockInfo nextBi = sync.getFullBlock(nextBh, 0, nextBh.countSignTx);
            signs = nextBi.getBlockSignatures();
        } else {
            signs = sync.findSignBlock(bh);
        }
        const BlockInfo bi = sync.getFullBlock(bh, beginTx, countTxs);
        return blockInfoToJson(requestId, bi, signs, type, isFormat, version);
    }
}

static std::vector<std::vector<MinimumSignBlockHeader>> getBlocksSignaturesFull(const Sync &sync, const std::vector<BlockHeader> &bhs) {
    std::vector<std::vector<MinimumSignBlockHeader>> blockSignatures;
    
    if (!bhs.empty()) {       
        const BlockHeader &fst = bhs.front();
        CHECK(fst.blockNumber.value() != 0, "Incorrect block number");
        
        for (const BlockHeader &bh: bhs) {
            const auto blockSigns = sync.getSignaturesBetween(std::nullopt, bh.hash);
            CHECK(blockSigns.size() <= 10, "Too many block signatures");
            blockSignatures.emplace_back(blockSigns);
        }
        
        const BlockHeader bck = bhs.back();
        
        const auto blockSigns = sync.getSignaturesBetween(bck.hash, std::nullopt);
        CHECK(blockSigns.size() <= 10, "Too many block signatures");
        blockSignatures.emplace_back(blockSigns);
    }
    
    return blockSignatures;
}

static std::vector<std::vector<std::vector<unsigned char>>> blockSignaturesConvert(const std::vector<std::vector<MinimumSignBlockHeader>> &blockSignatures) {
    std::vector<std::vector<std::vector<unsigned char>>> result;
    result.reserve(blockSignatures.size());
    
    std::transform(blockSignatures.begin(), blockSignatures.end(), std::back_inserter(result), [](const std::vector<MinimumSignBlockHeader> &elements) {
        std::vector<std::vector<unsigned char>> res;
        res.reserve(elements.size());
        std::transform(elements.begin(), elements.end(), std::back_inserter(res), std::mem_fn(&MinimumSignBlockHeader::hash));
        
        return res;
    });
    
    return result;
}

static std::vector<std::vector<std::vector<unsigned char>>> getBlocksSignatures(const Sync &sync, const std::vector<BlockHeader> &bhs) {
    return blockSignaturesConvert(getBlocksSignaturesFull(sync, bhs));
}

static std::string getBlocks(const RequestId &requestId, const rapidjson::Document &doc, const Sync &sync, bool isFormat, const JsonVersion &version) {
    const auto &jsonParams = get<JsonObject>(doc, "params");
    
    const int64_t countBlocks = getOpt<int>(jsonParams, "countBlocks", 0);
    const int64_t beginBlock = getOpt<int>(jsonParams, "beginBlock", 0);
    
    CHECK_USER(countBlocks <= MAX_BATCH_BLOCKS, "Too many blocks");
    
    BlockTypeInfo type = BlockTypeInfo::Simple;
    if (jsonParams.HasMember("type") && jsonParams["type"].IsString()) {
        const std::string typeString = jsonParams["type"].GetString();
        if (typeString == "simple") {
            type = BlockTypeInfo::Simple;
        } else if (typeString == "forP2P") {
            type = BlockTypeInfo::ForP2P;
        } else if (typeString == "small") {
            type = BlockTypeInfo::Small;
        } else {
            throwUserErr("Incorrect block type: " + typeString);
        }
    }
    
    const bool isForward = getOpt<std::string>(jsonParams, "direction", "backgward") == std::string("forward");
    
    std::vector<BlockHeader> bhs;
    std::vector<std::variant<std::vector<TransactionInfo>, std::vector<SignTransactionInfo>>> signs;
    bhs.reserve(countBlocks);
    signs.reserve(countBlocks);
    
    const auto processBlock = [&bhs, &signs, &sync, type](int64_t i) {
        bhs.emplace_back(sync.getBlockchain().getBlock(i));
        if (type == BlockTypeInfo::Simple) {
            if (bhs.back().countSignTx != 0) {
                const BlockInfo bi = sync.getFullBlock(bhs.back(), 0, bhs.back().countSignTx);
                signs.emplace_back(bi.getBlockSignatures());
            } else {
                signs.emplace_back(sync.findSignBlock(bhs.back()));
            }
        } else {
            signs.push_back({});
        }
    };
    
    if (!isForward) {
        const int64_t fromBlock = sync.getBlockchain().countBlocks() - beginBlock;
        for (int64_t i = fromBlock; i > fromBlock - countBlocks && i > 0; i--) {
            processBlock(i);
        }
    } else {
        const int64_t maxBlockNum = sync.getBlockchain().countBlocks();
        for (int64_t i = beginBlock; i < std::min(maxBlockNum + 1, beginBlock + countBlocks); i++) {
            processBlock(i);
        }
    }
    signs.emplace_back();
    
    if (type != BlockTypeInfo::ForP2P) {
        return blockHeadersToJson(requestId, bhs, signs, type, isFormat, version);
    } else {
        CHECK_USER(isForward, "Incorrect direction for p2p");
        
        const std::vector<std::vector<std::vector<unsigned char>>> blockSignatures = getBlocksSignatures(sync, bhs);
        
        return blockHeadersToP2PJson(requestId, bhs, blockSignatures, isFormat, version);
    }
}

template<typename T>
std::string getBlockDump(const rapidjson::Document &doc, const RequestId &requestId, const std::string &nameParam, const Sync &sync, bool isFormat) {   
    const auto &jsonParams = get<JsonObject>(doc, "params");
    const T &hashOrNumber = get<T>(jsonParams, nameParam);
    
    const size_t fromByte = getOpt<size_t>(jsonParams, "fromByte", 0);
    const size_t toByte = getOpt<size_t>(jsonParams, "toByte", std::numeric_limits<size_t>::max());
    
    const bool isHex = getOpt<bool>(jsonParams, "isHex", false);   
    const bool isSign = getOpt<bool>(jsonParams, "isSign", false);   
    const bool isCompress = getOpt<bool>(jsonParams, "compress", false);
    
    const BlockHeader bh = sync.getBlockchain().getBlock(hashOrNumber);
    std::string blockDump;
    if constexpr (!std::is_same_v<std::decay_t<T>, std::string>) {
        CHECK_USER(bh.blockNumber.has_value(), "block " + to_string(hashOrNumber) + " not found");
        blockDump = sync.getBlockDump(bh.hash, bh.filePos, fromByte, toByte, isHex, isSign);
    } else {
        if (bh.blockNumber.has_value()) {
            blockDump = sync.getBlockDump(bh.hash, bh.filePos, fromByte, toByte, false, isSign);
        } else {
            const std::optional<MinimumSignBlockHeader> foundSignBlock = sync.findSignature(fromHex(hashOrNumber));
            CHECK(foundSignBlock.has_value(), "block " + to_string(hashOrNumber) + " not found");
            blockDump = sync.getBlockDump(foundSignBlock->hash, foundSignBlock->filePos, fromByte, toByte, isHex, isSign);
        }
    }
    const std::string res = genDumpBlockBinary(blockDump, isCompress);
    
    CHECK(!res.empty(), "block " + to_string(hashOrNumber) + " not found");
    if (isHex) {
        return genBlockDumpJson(requestId, res, isFormat);
    } else {
        return res;
    }
}

template<typename T>
std::string getBlockDumps(const rapidjson::Document &doc, const RequestId &requestId, const std::string nameParam, const Sync &sync) {   
    const auto &jsonParams = get<JsonObject>(doc, "params");
    
    const bool isSign = getOpt<bool>(jsonParams, "isSign", false);
    const bool isCompress = getOpt<bool>(jsonParams, "compress", false);
    
    const auto &jsonVals = get<JsonArray>(jsonParams, nameParam);
    CHECK_USER(jsonVals.Size() <= MAX_BATCH_DUMPS, "Too many blocks");
    std::vector<std::string> result;
    for (const auto &jsonVal: jsonVals) {
        const T &hashOrNumber = get<T>(jsonVal);
        
        const size_t fromByte = 0;
        const size_t toByte = std::numeric_limits<size_t>::max();
                
        const BlockHeader bh = sync.getBlockchain().getBlock(hashOrNumber);
        std::string blockDump;
        if constexpr (!std::is_same_v<std::decay_t<T>, std::string>) {
            CHECK_USER(bh.blockNumber.has_value(), "block " + to_string(hashOrNumber) + " not found");
            blockDump = sync.getBlockDump(bh.hash, bh.filePos, fromByte, toByte, false, isSign);
        } else {
            if (bh.blockNumber.has_value()) {
                blockDump = sync.getBlockDump(bh.hash, bh.filePos, fromByte, toByte, false, isSign);
            } else {
                const std::optional<MinimumSignBlockHeader> foundSignBlock = sync.findSignature(fromHex(hashOrNumber));
                CHECK(foundSignBlock.has_value(), "block " + to_string(hashOrNumber) + " not found");
                blockDump = sync.getBlockDump(foundSignBlock->hash, foundSignBlock->filePos, fromByte, toByte, false, isSign);
            }
        }
        const std::string &res = blockDump;
        
        CHECK(!res.empty(), "block " + to_string(hashOrNumber) + " not found");
        result.emplace_back(res);
    }
    return genDumpBlocksBinary(result, isCompress);
}

bool Server::run(int thread_number, Request& mhd_req, Response& mhd_resp) {
    mhd_resp.headers["Access-Control-Allow-Origin"] = "*";
    //mhd_resp.headers["Connection"] = "close";
    
    if (isStoped.load()) {
        return false;
    }
    
    IncCountRunningThread incCountRunningThreads(countRunningThreads);
    try {
        checkStopSignal();
    } catch (const StopException &e) {
        isStoped = true;
        return false;
    } catch (...) {
        throw;
    }
    
    const std::string &url = mhd_req.url;
    const std::string &method = mhd_req.method;
    RequestId requestId;
    
    Timer tt;
    
    smallRequestStatistics.addStatistic(1, ::now(), 1min);
        
    std::string func;
    try {
        std::string jsonRequest;
        
        if (method == "POST") {
            if (!mhd_req.post.empty()) {
                jsonRequest = mhd_req.post;
            }
        }

        if (jsonRequest.size() > 1 * 1024 * 1024) {
            LOGINFO << "Big request: " << jsonRequest.size() << " " << jsonRequest;
        }
        
        rapidjson::Document doc;
        if (!jsonRequest.empty()) {
            const rapidjson::ParseResult pr = doc.Parse(jsonRequest.c_str());
            CHECK_USER(pr, "rapidjson parse error. Data: " + jsonRequest);
        }
        
        if (url.size() > 1) {
            func = url.substr(1);        
        } else {
            CHECK_USER(doc.HasMember("method") && doc["method"].IsString(), "method field not found");
            func = doc["method"].GetString();
        }
                
        if (doc.HasMember("id") && doc["id"].IsString()) {
            requestId.id = doc["id"].GetString();
            requestId.isSet = true;
        }
        if (doc.HasMember("id") && doc["id"].IsInt64()) {
            requestId.id = doc["id"].GetInt64();
            requestId.isSet = true;
        }
        
        JsonVersion jsonVersion = JsonVersion::V1;
        if (doc.HasMember("version") && doc["version"].IsString()) {
            const std::string jsonVersionString = doc["version"].GetString();
            if (jsonVersionString == "v1" || jsonVersionString == "version1") {
                jsonVersion = JsonVersion::V1;
            } else if (jsonVersionString == "v2" || jsonVersionString == "version2") {
                jsonVersion = JsonVersion::V2;
            }
        }
        
        bool isFormatJson = false;
        if (doc.HasMember("pretty") && doc["pretty"].IsBool()) {
            isFormatJson = doc["pretty"].GetBool();
        }
        
        std::string response;
        
        if (func == "status") {
            response = genStatusResponse(requestId, VERSION, g_GIT_SHA1);
        } else if (func == "getinfo") {
            response = genInfoResponse(requestId, VERSION, SERVER_TYPE, serverPrivKey);
        } else if (func == "get-statistic") {
            const SmallStatisticElement smallStat = smallRequestStatistics.getStatistic();
            response = genStatisticResponse(smallStat.stat);
        } else if (func == "get-statistic2") {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &pubkey = get<std::string>(jsonParams, "pubkey");
            const std::string &sign = get<std::string>(jsonParams, "sign");
            const std::string &timestamp = get<std::string>(jsonParams, "timestamp");
            const long long timestampLong = std::stoll(timestamp);
            
            const auto now = nowSystem();
            const long long nowTimestamp = getTimestampMs(now);
            CHECK_USER(std::abs(nowTimestamp - timestampLong) <= milliseconds(5s).count(), "Timestamp is out");
            
            CHECK_USER(sync.verifyTechnicalAddressSign(timestamp, fromHex(sign), fromHex(pubkey)), "Incorrect signature");
            
            const SmallStatisticElement smallStat = smallRequestStatistics.getStatistic();
            response = genStatisticResponse(requestId, smallStat.stat, getProcLoad(), getTotalSystemMemory(), getOpenedConnections());
        } else if (func == GET_ADDRESS_HISTORY) {            
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");
            const Address address(addressString);
            
            const size_t countTxs = getOpt<int>(jsonParams, "countTxs", 0);
            const size_t beginTx = getOpt<int>(jsonParams, "beginTx", 0);
            
            const std::vector<TransactionInfo> txs = sync.getTxsForAddress(address, beginTx, countTxs, MAX_HISTORY_SIZE);
            CHECK(txs.size() <= MAX_HISTORY_SIZE, "Incorrect result size");
            
            response = addressesInfoToJson(requestId, addressString, txs, sync.getBlockchain(), 0, isFormatJson, jsonVersion);
        } else if (func == GET_ADDRESS_HISTORY_FILTER) {            
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");
            const Address address(addressString);
            
            const size_t countTxs = getOpt<int>(jsonParams, "countTxs", 0);
            size_t beginTx = getOpt<int>(jsonParams, "beginTx", 0);
            
            const TransactionsFilters filters = parseFilters(jsonParams["filters"]);
            
            const std::vector<TransactionInfo> txs = sync.getTxsForAddress(address, beginTx, countTxs, MAX_HISTORY_SIZE, filters);
            CHECK(txs.size() <= MAX_HISTORY_SIZE, "Incorrect result size");
            
            response = addressesInfoToJsonFilter(requestId, addressString, txs, beginTx, sync.getBlockchain(), 0, isFormatJson, jsonVersion);
        } else if (func == GET_ADDRESS_BALANCE) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");
            const Address address(addressString);
            
            const BalanceInfo balance = sync.getBalance(address);
            
            response = balanceInfoToJson(requestId, addressString, balance, sync.getBlockchain().countBlocks(), isFormatJson, jsonVersion);
        } else if (func == GET_ADDRESS_TOKENS) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");
            const Address address(addressString);
            
            const BalanceInfo balance = sync.getBalance(address);
            
            response = balanceTokenInfoToJson(requestId, addressString, balance, sync.getBlockchain().countBlocks(), isFormatJson, jsonVersion);
        } else if (func == GET_ADDRESS_BALANCES) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const auto &addressesJson = get<JsonArray>(jsonParams, "addresses");
            
            std::vector<std::pair<std::string, BalanceInfo>> balances;
            CHECK_USER(addressesJson.Size() <= MAX_BATCH_BALANCES, "Too many transactions. Please, decrease count addresses");
            for (const auto &addressJson: addressesJson) {
                const std::string addressString = get<std::string>(addressJson);
                const Address address(addressString);
                const BalanceInfo balance = sync.getBalance(address);
                balances.emplace_back(addressString, balance);
            }
            
            response = balancesInfoToJson(requestId, balances, sync.getBlockchain().countBlocks(), isFormatJson, jsonVersion);
        } else if (func == GET_BLOCK_BY_HASH) {
            response = getBlock<std::string>(requestId, doc, "hash", sync, isFormatJson, jsonVersion);
        } else if (func == GET_BLOCK_BY_NUMBER) {
            response = getBlock<size_t>(requestId, doc, "number", sync, isFormatJson, jsonVersion);
        } else if (func == GET_BLOCKS) {
            response = getBlocks(requestId, doc, sync, isFormatJson, jsonVersion);
        } else if (func == GET_DUMP_BLOCK_BY_HASH) {
            response = getBlockDump<std::string>(doc, requestId, "hash", sync, isFormatJson);
        } else if (func == GET_DUMP_BLOCK_BY_NUMBER) {
            response = getBlockDump<size_t>(doc, requestId, "number", sync, isFormatJson);
        } else if (func == GET_DUMPS_BLOCKS_BY_HASH) {
            response = getBlockDumps<std::string>(doc, requestId, "hashes", sync);
        } else if (func == GET_DUMPS_BLOCKS_BY_NUMBER) {
            response = getBlockDumps<size_t>(doc, requestId, "numbers", sync);
        } else if (func == GET_LAST_TXS) {
            response = transactionsToJson(requestId, sync.getLastTxs(), sync.getBlockchain(), isFormatJson, jsonVersion);
        } else if (func == GET_TRANSACTION_INFO) {           
            const auto &jsonParams = get<JsonObject>(doc, "params");

            const std::vector<unsigned char> &hash0 = fromHex(get<std::string>(jsonParams, "hash"));
            const std::string hash(hash0.begin(), hash0.end());
            
            const std::optional<TransactionInfo> res = sync.getTransaction(hash);
            
            if (!res.has_value()) {
                response = genTransactionNotFoundResponse(requestId, hash);
            } else {
                response = transactionToJson(requestId, res.value(), sync.getBlockchain(), sync.getBlockchain().countBlocks(), sync.getKnownBlock(), isFormatJson, jsonVersion);
            }
        } else if (func == GET_TOKEN_INFO) {           
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const Address &address = Address(get<std::string>(jsonParams, "address"));
            
            const Token res = sync.getTokenInfo(address);
            response = tokenToJson(requestId, res, isFormatJson, jsonVersion);
        } else if (func == GET_TRANSACTIONS_INFO) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const auto &hashesJson = get<JsonArray>(jsonParams, "hashes");
            CHECK_USER(hashesJson.Size() <= MAX_BATCH_TXS, "Too many transactions. Please, decrease count transactions");
            std::vector<TransactionInfo> txsResult;
            for (const auto &hashJson: hashesJson) {
                const auto &hash0 = fromHex(get<std::string>(hashJson));
                const std::string hash(hash0.begin(), hash0.end());
                
                const std::optional<TransactionInfo> res = sync.getTransaction(hash);
                
                if (res.has_value()) {
                    txsResult.emplace_back(res.value());
                }
            }
            response = transactionsToJson(requestId, txsResult, sync.getBlockchain(), isFormatJson, jsonVersion);
        } else if (func == GET_COUNT_BLOCKS) {
            bool forP2P = false;
            if (doc.HasMember("params") && doc["params"].IsObject()) {
                const auto &jsonParams = get<JsonObject>(doc, "params");
                forP2P = getOpt<std::string>(jsonParams, "type", "") == "forP2P";
            }
            
            const size_t countBlocks = sync.getBlockchain().countBlocks();
            
            if (!forP2P) {
                response = genCountBlockJson(requestId, countBlocks, isFormatJson, jsonVersion);
            } else {
                const BlockHeader header = sync.getBlockchain().getBlock(countBlocks);
                const std::vector<MinimumSignBlockHeader> signatures = sync.getSignaturesBetween(header.hash, std::nullopt);
                CHECK(signatures.size() <= 10, "Too many signatures");
                std::vector<std::vector<unsigned char>> signHashes;
                signHashes.reserve(signatures.size());
                std::transform(signatures.begin(), signatures.end(), std::back_inserter(signHashes), std::mem_fn(&MinimumSignBlockHeader::hash));
                
                response = genCountBlockForP2PJson(requestId, countBlocks, signHashes, isFormatJson, jsonVersion);
            }
        } else if (func == PRE_LOAD_BLOCKS) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const size_t currentBlock = get<int>(jsonParams, "currentBlock");
            const bool isCompress = get<bool>(jsonParams, "compress");
            const bool isSign = get<bool>(jsonParams, "isSign");
            const size_t preLoadBlocks = get<int>(jsonParams, "preLoad");
            const size_t maxBlockSize = get<int>(jsonParams, "maxBlockSize");
            
            CHECK(preLoadBlocks <= MAX_PRELOAD_BLOCKS, "Incorrect preload parameter");
            
            const size_t countBlocks = sync.getBlockchain().countBlocks();
            
            std::vector<BlockHeader> bhs;
            std::vector<std::string> blocks;
            if (countBlocks <= currentBlock + preLoadBlocks + MAX_PRELOAD_BLOCKS / 2) {
                for (size_t i = currentBlock + 1; i < std::min(currentBlock + 1 + preLoadBlocks, countBlocks + 1); i++) {
                    const BlockHeader &bh = sync.getBlockchain().getBlock(i);
                    CHECK(bh.blockNumber.has_value(), "block " + to_string(i) + " not found");
                    if (bh.blockSize > maxBlockSize) {
                        break;
                    }
                    
                    bhs.emplace_back(bh);
                    blocks.emplace_back(sync.getBlockDump(bh.hash, bh.filePos, 0, std::numeric_limits<size_t>::max(), false, isSign));
                }
            }
            
            std::vector<std::vector<std::vector<unsigned char>>> blockSignaturesHashes;
            if (!bhs.empty()) {
                const std::vector<std::vector<MinimumSignBlockHeader>> blockSignatures = getBlocksSignaturesFull(sync, bhs);
                blockSignaturesHashes = blockSignaturesConvert(blockSignatures);
                
                for (const auto &elements: blockSignatures) {
                    for (const MinimumSignBlockHeader &element: elements) {
                        blocks.emplace_back(sync.getBlockDump(element.hash, element.filePos, 0, std::numeric_limits<size_t>::max(), false, isSign));
                    }
                }
            } else {
                if (countBlocks == currentBlock) {
                    const BlockHeader &b = sync.getBlockchain().getBlock(countBlocks);
                    const std::vector<MinimumSignBlockHeader> signatures = sync.getSignaturesBetween(b.hash, std::nullopt);
                    for (const MinimumSignBlockHeader &element: signatures) {
                        blocks.emplace_back(sync.getBlockDump(element.hash, element.filePos, 0, std::numeric_limits<size_t>::max(), false, isSign));
                    }
                    
                    blockSignaturesHashes = blockSignaturesConvert({signatures});
                }
            }
            
            response = preLoadBlocksJson(requestId, countBlocks, bhs, blockSignaturesHashes, blocks, isCompress, jsonVersion);
        } else if (func == GET_ADDRESS_DELEGATIONS) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const Address address(get<std::string>(jsonParams, "address"));
            
            const auto result = sync.getDelegateStates(address);
            
            response = delegateStatesToJson(requestId, address.calcHexString(), result, isFormatJson, jsonVersion);
        } else if (func == GET_CONTRACT_DETAILS) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const Address address(get<std::string>(jsonParams, "address"));
            const std::string path = getOpt<std::string>(jsonParams, "path", "");
            
            const auto result = sync.getContractDetails(address);
            
            response = genV8DetailsJson(requestId, address.calcHexString(), result, path, isFormatJson);
        } else if (func == GET_CONTRACT_CODE) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const Address address(get<std::string>(jsonParams, "address"));
            
            const auto result = sync.getContractCode(address);
            
            response = genV8CodeJson(requestId, address.calcHexString(), result, isFormatJson);
        } else if (func == GET_COMMON_BALANCE) {
            const auto result = sync.getCommonBalance();
            response = genCommonBalanceJson(requestId, result, isFormatJson, jsonVersion);
        } else if (func == GET_FORGING_SUM) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const int blockIndent = get<int>(jsonParams, "block_indent");
            const ForgingSums forgingSum = sync.getForgingSumForLastBlock(blockIndent);
            response = genForgingSumJson(requestId, forgingSum, isFormatJson, jsonVersion);
        } else if (func == GET_FORGING_SUM_ALL) {
            const ForgingSums forgingSum = sync.getForgingSumAll();
            response = genForgingSumJson(requestId, forgingSum, isFormatJson, jsonVersion);
        } else if (func == GET_LAST_NODE_STAT_RESULT) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");
            
            const auto result = sync.getLastNodeTestResult(addressString);
            
            response = genNodeStatResultJson(requestId, addressString, result.first, result.second, isFormatJson, jsonVersion);
        } else if (func == GET_LAST_NODE_STAT_TRUST) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");
            
            const auto result = sync.getLastNodeTestTrust(addressString);
            
            response = genNodeStatTrustJson(requestId, addressString, result.first, result.second, isFormatJson, jsonVersion);
        } else if (func == GET_LAST_NODE_STAT_COUNT) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const std::string &addressString = get<std::string>(jsonParams, "address");

            const auto result = sync.getLastDayNodeTestCount(addressString);

            const size_t lastBlockDay = sync.getLastBlockDay();
            response = genNodeStatCountJson(requestId, addressString, lastBlockDay, result, isFormatJson, jsonVersion);
        } else if (func == GET_ALL_LAST_NODES_RESULT) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const size_t &countTests = get<int>(jsonParams, "count_tests");
            
            const auto result = sync.filterLastNodes(countTests);
            
            const size_t lastBlockDay = sync.getLastBlockDay();
            response = genAllNodesStatsCountJson(requestId, lastBlockDay, result, isFormatJson, jsonVersion);
        } else if (func == GET_NODE_RAITING) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const size_t countTests = getOpt<int>(jsonParams, "count_tests", 10);
            const std::string &addressString = get<std::string>(jsonParams, "address");
            
            const auto result = sync.calcNodeRaiting(addressString, countTests);
            const size_t lastBlockDay = sync.getLastBlockDay();
            response = genNodesRaitingJson(requestId, addressString, result.first, result.second, lastBlockDay, isFormatJson, jsonVersion);
        } else if (func == GET_RANDOM_ADDRESSES) {
            const auto &jsonParams = get<JsonObject>(doc, "params");
            
            const size_t countAddresses = get<size_t>(jsonParams, "count_addresses");
            
            const auto result = sync.getRandomAddresses(countAddresses);
            
            response = genRandomAddressesJson(requestId, result, isFormatJson);
        } else if (func == GET_REJECTED_TX_INFO) {
            const auto &jsonParams = get<JsonObject>(doc, "params");

            const std::string hash = get<std::string>(jsonParams, "hash");

            const auto result = sync.findRejectedTx(fromHex(hash));

            response = genRejectedTxHistoryJson(requestId, result, isFormatJson);
        } else if (func == GET_REJECTED_BLOCKS) {
            const auto &jsonParams = get<JsonObject>(doc, "params");

            const size_t count = get<size_t>(jsonParams, "count");

            CHECK_USER(count <= MAX_REJECTED_BLOCKS, "Incorrect count value");

            const auto result = sync.calcLastRejectedBlocks(count);

            response = genRejectedBlocksInfo(result);
        } else if (func == GET_REJECTED_DUMPS) {
            const auto &jsonParams = get<JsonObject>(doc, "params");

            const bool isCompress = get<bool>(jsonParams, "isCompress");
            const auto &hashesJson = get<JsonArray>(jsonParams, "hashes");
            std::vector<std::vector<unsigned char>> hashes;
            std::transform(hashesJson.Begin(), hashesJson.End(), std::back_inserter(hashes), [](const auto &hashJson) {
                return fromHex(get<std::string>(hashJson));
            });

            CHECK_USER(hashes.size() <= MAX_REJECTED_BLOCKS, "Incorrect count value");

            const auto result = sync.getRejectedDumps(hashes);

            response = genDumpBlocksBinary(result, isCompress);
        } else {
            throwUserErr("Incorrect func " + func);
        }
        
        mhd_resp.data = response;
        mhd_resp.code = HTTP_STATUS_OK;
    } catch (const exception &e) {
        LOGERR << e;
        mhd_resp.data = genErrorResponse(requestId, -32603, e);
        mhd_resp.code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    } catch (const UserException &e) {
        LOGDEBUG << e.what();
        mhd_resp.data = genErrorResponse(requestId, -32602, std::string(e.what()) + ". Url: " + url);
        mhd_resp.code = HTTP_STATUS_BAD_REQUEST;
    } catch (const std::exception &e) {
        LOGERR << e.what();
        mhd_resp.data = genErrorResponse(requestId, -32603, e.what());
        mhd_resp.code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    } catch (...) {
        LOGERR << "Unknown error";
        mhd_resp.data = genErrorResponse(requestId, -32603, "Unknown error");
        mhd_resp.code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    
    if (tt.countMs() > 2000 || mhd_resp.data.size() >= 800 * 1024 * 1024) {
        LOGINFO << "Long request time: " << tt.countMs() << ". Count threads: " << countRunningThreads.load() << ". Response size: " << mhd_resp.data.size() << ". Request: " << url << " " << mhd_req.post;
    }
    
    //LOGDEBUG << "Request: " << mhd_req.ip << ", " << url << ", " << mhd_req.post << ", " << tt.countMs() << ", " << mhd_resp.data.size();
        
    return true;
}

bool Server::init() {
    LOGINFO << "Port " << port;

    const int COUNT_THREADS = 8;
    
    countRunningThreads = 0;
    
    set_threads(COUNT_THREADS);
    set_port(port);
    
#ifndef UBUNTU14
    set_connection_limit(200000);
    set_per_ip_connection_limit(100);
#endif

    return true;
}
