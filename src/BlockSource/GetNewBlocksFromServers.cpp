#include "GetNewBlocksFromServers.h"

#include <functional>
using namespace std::placeholders;

#include <mutex>

#include "BlockInfo.h"

#include "check.h"
#include "log.h"

#include "get_new_blocks_messages.h"

using namespace common;

namespace torrent_node_lib {

const static size_t ESTIMATE_SIZE_SIGNATURE = 250;

const static size_t MAX_BLOCK_SIZE_WITHOUT_ADVANCE = 100 * 1000;

GetNewBlocksFromServer::LastBlockResponse GetNewBlocksFromServer::getLastBlock() const {
    std::optional<size_t> lastBlock;
    std::string error;
    std::vector<std::string> serversSave;
    std::set<std::vector<unsigned char>> extraBlocksSave;
    std::mutex mut;
    const BroadcastResult function = [&lastBlock, &error, &mut, &serversSave, &extraBlocksSave](const std::string &server, const std::string &result, const std::optional<CurlException> &curlException) {
        if (curlException.has_value()) {
            std::lock_guard<std::mutex> lock(mut);
            error = curlException.value().message;
            return;
        }
        
        try {
            const auto [countBlocks, extraBlocks] = parseCountBlocksMessage(result);
            
            std::lock_guard<std::mutex> lock(mut);
            if (!lastBlock.has_value()) {
                lastBlock = 0;
            }
            if (lastBlock < countBlocks) {
                lastBlock = countBlocks;
                serversSave.clear();
                extraBlocksSave = extraBlocks;
                serversSave.emplace_back(server);
            } else if (lastBlock == countBlocks) {
                serversSave.emplace_back(server);
                extraBlocksSave.insert(extraBlocks.begin(), extraBlocks.end());
            }
        } catch (const exception &e) {
            std::lock_guard<std::mutex> lock(mut);
            error = e;
        }
    };
    
    p2p.broadcast("", makeGetCountBlocksMessage(), "", function);
    
    LastBlockResponse response;
    if (lastBlock.has_value()) {
        response.lastBlock = lastBlock.value();
        response.servers = serversSave;
        response.extraBlocks = extraBlocksSave;
    } else {
        response.error = error;
    }
    return response;
}

GetNewBlocksFromServer::LastBlockPreLoadResponse GetNewBlocksFromServer::preLoadBlocks(size_t currentBlock, bool isSign) const {   
    const size_t PRELOAD_BLOCKS = 5;
    const size_t MAX_BLOCK_SIZE = 100000;
    
    std::string error;
    std::mutex mut;
    LastBlockPreLoadResponse answer;
    const BroadcastResult function = [&error, &mut, &answer](const std::string &server, const std::string &result, const std::optional<CurlException> &curlException) {
        if (curlException.has_value()) {
            std::lock_guard<std::mutex> lock(mut);
            error = curlException.value().message;
            return;
        }
        
        try {
            const PreloadBlocksResponse response = parsePreloadBlocksMessage(result);
            
            if (response.error.has_value()) {
                std::lock_guard<std::mutex> lock(mut);
                error = response.error.value();
                return;
            }
           
            std::lock_guard<std::mutex> lock(mut);
            if (!answer.lastBlock.has_value()) {
                answer.lastBlock = 0;
            }
            if (answer.lastBlock < response.countBlocks) {
                answer.lastBlock = response.countBlocks;
                answer.servers.clear();
                answer.servers.emplace_back(server);
                
                answer.blockHeaders = response.blockHeaders;
                answer.blocksDumps = response.blockDumps;
            } else if (answer.lastBlock == response.countBlocks) {
                answer.servers.emplace_back(server);
            }
        } catch (const exception &e) {
            std::lock_guard<std::mutex> lock(mut);
            error = e;
        }
    };
    
    p2p.broadcast("", makePreloadBlocksMessage(currentBlock, isCompress, isSign, PRELOAD_BLOCKS, MAX_BLOCK_SIZE), "", function);
    
    if (!answer.lastBlock.has_value()) {
        answer.error = error;
    }
    return answer;
}

void GetNewBlocksFromServer::clearAdvanced() {
    advancedLoadsBlocksHeaders.clear();
    advancedLoadsBlocksDumps.clear();
}

void GetNewBlocksFromServer::addPreLoadBlocks(size_t fromBlock, const std::string &blockHeadersStr, const std::string &blockDumpsStr) {
    try {
        const std::vector<MinimumBlockHeader> blocksHeaders = parseBlocksHeader(blockHeadersStr);
        const std::vector<std::string> blocksDumps = parseDumpBlocksBinary(blockDumpsStr, isCompress);
        CHECK(blocksHeaders.size() == blocksDumps.size(), "Not equals count block headers and dumps");
        
        LOGINFO << "Ya tuta 0 " << blocksDumps.size();
        
        for (size_t j = 0; j < blocksHeaders.size(); j++) {
            CHECK(blocksHeaders[j].number == fromBlock + j, "Incorrect block number in answer: " + std::to_string(blocksHeaders[j].number) + " " + std::to_string(fromBlock + j));
            advancedLoadsBlocksHeaders.emplace_back(fromBlock + j, blocksHeaders[j]);
            
            advancedLoadsBlocksDumps[blocksHeaders[j].hash] = blocksDumps[j];
        }
    } catch (const exception &e) {
        LOGWARN << "Dont added preload blocks " << e;
    }
}

MinimumBlockHeader GetNewBlocksFromServer::getBlockHeader(size_t blockNum, size_t maxBlockNum, const std::vector<std::string> &servers) {
    const auto foundBlock = std::find_if(advancedLoadsBlocksHeaders.begin(), advancedLoadsBlocksHeaders.end(), [blockNum](const auto &pair) {
        return pair.first == blockNum;
    });
    if (foundBlock != advancedLoadsBlocksHeaders.end()) {
        return foundBlock->second;
    }
    
    advancedLoadsBlocksHeaders.clear();
        
    const size_t countBlocks = std::min(maxBlockNum - blockNum + 1, maxAdvancedLoadBlocks);
    const size_t countParts = (countBlocks + countBlocksInBatch - 1) / countBlocksInBatch;
    CHECK(countBlocks != 0 && countParts != 0, "Incorrect count blocks");
    
    const auto calcBlockIndexes = [blockNum, countBlocksInBatch=this->countBlocksInBatch, maxCountBlocks=countBlocks](size_t number) {
        const size_t beginBlock = blockNum + number * countBlocksInBatch;
        const size_t countBlocks = std::min(countBlocksInBatch, maxCountBlocks - number * countBlocksInBatch);
        return std::make_pair(beginBlock, countBlocks);
    };
    
    const auto makeQsAndPost = [calcBlockIndexes](size_t number) {
        const auto [beginBlock, countBlocks] = calcBlockIndexes(number);
        if (countBlocks != 1) {
            return std::make_pair("", makeGetBlocksMessage(beginBlock, countBlocks));
        } else {
            return std::make_pair("", makeGetBlockByNumberMessage(beginBlock));
        }
    };
    
    const std::vector<std::string> answer = p2p.requests(countParts, makeQsAndPost, "", [calcBlockIndexes](const std::string &result, size_t fromIndex, size_t toIndex) {
        ResponseParse r;
        
        const auto [beginBlock, countBlocks] = calcBlockIndexes(fromIndex);
        
        const auto checked = checkErrorGetBlockResponse(result, countBlocks);
        if (checked.has_value()) {
            r.error = checked.value();
        } else {
            r.response = result;
        }
        return r;
    }, servers);
    
    CHECK(answer.size() == countParts, "Incorrect answer");
    
    for (size_t i = 0; i < answer.size(); i++) {
        const size_t currBlockNum = blockNum + i * countBlocksInBatch;
        const size_t blocksInPart = std::min(countBlocksInBatch, countBlocks - i * countBlocksInBatch);
        if (blocksInPart != 1) {
            const std::vector<MinimumBlockHeader> blocks = parseBlocksHeader(answer[i]);
            CHECK(blocks.size() == blocksInPart, "Incorrect answers");
            for (size_t j = 0; j < blocks.size(); j++) {
                CHECK(blocks[j].number == currBlockNum + j, "Incorrect block number in answer: " + std::to_string(blocks[j].number) + " " + std::to_string(currBlockNum + j));
                advancedLoadsBlocksHeaders.emplace_back(currBlockNum + j, blocks[j]);
            }
        } else {
            const MinimumBlockHeader header = parseBlockHeader(answer[i]);
            CHECK(header.number == currBlockNum, "Incorrect block number in answer: " + std::to_string(header.number) + " " + std::to_string(currBlockNum));
            advancedLoadsBlocksHeaders.emplace_back(currBlockNum, header);
        }
    }
    
    return advancedLoadsBlocksHeaders.front().second;
}

MinimumBlockHeader GetNewBlocksFromServer::getBlockHeaderWithoutAdvanceLoad(size_t blockNum, const std::string &server) const {
    const std::string response = p2p.runOneRequest(server, "", makeGetBlockByNumberMessage(blockNum), "");
    
    return parseBlockHeader(response);
}

std::pair<std::string, std::string> GetNewBlocksFromServer::makeRequestForDumpBlock(const std::string &blockHash, size_t fromByte, size_t toByte) {
    const static std::string QS = "";
    const std::string post = makeGetDumpBlockMessage(blockHash, fromByte, toByte, false, false);
    return std::make_pair(QS, post);
}

std::pair<std::string, std::string> GetNewBlocksFromServer::makeRequestForDumpBlockSign(const std::string &blockHash, size_t fromByte, size_t toByte) {
    const static std::string QS = "";
    const std::string post = makeGetDumpBlockMessage(blockHash, fromByte, toByte, true, false);
    return std::make_pair(QS, post);
}

ResponseParse GetNewBlocksFromServer::parseDumpBlockResponse(bool manyBlocks, bool isSign, bool isCompress, const std::string& result, size_t fromByte, size_t toByte) {
    ResponseParse parsed;
    if (result.empty()) {
        return parsed;
    }
    
    const auto checked = checkErrorGetBlockDumpResponse(result, manyBlocks, isSign, isCompress, toByte - fromByte);
    
    if (checked.has_value()) {
        parsed.error = checked.value();
    } else {
        parsed.response = result;
    }
    return parsed;
}

std::string GetNewBlocksFromServer::getBlockDumpWithoutAdvancedLoad(const std::string &blockHash, size_t blockSize, const std::vector<std::string> &hintsServers, bool isSign) const {
    if (!isSign) {
        return p2p.request(blockSize, true, std::bind(makeRequestForDumpBlock, std::ref(blockHash), _1, _2), "", std::bind(parseDumpBlockResponse, false, isSign, false, _1, _2, _3), hintsServers);
    } else {
        const std::string result = p2p.request(blockSize + ESTIMATE_SIZE_SIGNATURE, false, std::bind(makeRequestForDumpBlockSign, std::ref(blockHash), _1, _2), "", std::bind(parseDumpBlockResponse, false, isSign, false, _1, _2, _3), hintsServers);
        return result;
    }
}

std::string GetNewBlocksFromServer::getBlockDump(const std::string& blockHash, size_t blockSize, const std::vector<std::string> &hintsServers, bool isSign) const {
    const auto foundDump = advancedLoadsBlocksDumps.find(blockHash);
    if (foundDump != advancedLoadsBlocksDumps.end()) {
        return foundDump->second;
    }
       
    if (blockSize > MAX_BLOCK_SIZE_WITHOUT_ADVANCE) {
        return getBlockDumpWithoutAdvancedLoad(blockHash, blockSize, hintsServers, isSign);
    }
    
    advancedLoadsBlocksDumps.clear();
    
    const auto foundHeader = std::find_if(advancedLoadsBlocksHeaders.begin(), advancedLoadsBlocksHeaders.end(), [&blockHash](const auto &pair) {
        return pair.second.hash == blockHash;
    });
   
    std::vector<std::string> blocksHashs;
    for (auto iterHeader = foundHeader ; iterHeader != advancedLoadsBlocksHeaders.end() && iterHeader->second.blockSize <= MAX_BLOCK_SIZE_WITHOUT_ADVANCE; iterHeader++) {
        blocksHashs.emplace_back(iterHeader->second.hash);
    }
    
    CHECK(!blocksHashs.empty(), "advanced blocks not loaded");
    
    const size_t countParts = (blocksHashs.size() + countBlocksInBatch - 1) / countBlocksInBatch;
    
    const auto makeQsAndPost = [&blocksHashs, isSign, countBlocksInBatch=this->countBlocksInBatch, isCompress=this->isCompress](size_t number) {
        CHECK(blocksHashs.size() > number * countBlocksInBatch, "Incorrect number");
        const size_t beginBlock = number * countBlocksInBatch;
        const size_t countBlocks = std::min(countBlocksInBatch, blocksHashs.size() - beginBlock);
        if (countBlocks == 1) {
            return std::make_pair("", makeGetDumpBlockMessage(blocksHashs[beginBlock], isSign, isCompress));
        } else {           
            return std::make_pair("", makeGetDumpsBlocksMessage(blocksHashs.begin() + beginBlock, blocksHashs.begin() + beginBlock + countBlocks, isSign, isCompress));
        }
    };
    
    const std::vector<std::string> responses = p2p.requests(countParts, makeQsAndPost, "", std::bind(parseDumpBlockResponse, true, isSign, isCompress, _1, _2, _3), hintsServers);
    CHECK(responses.size() == countParts, "Incorrect responses");
    
    for (size_t i = 0; i < responses.size(); i++) {
        const size_t beginBlock = i * countBlocksInBatch;
        const size_t blocksInPart = std::min(countBlocksInBatch, blocksHashs.size() - beginBlock);
        
        if (blocksInPart == 1) {
            CHECK(beginBlock < blocksHashs.size(), "Incorrect answer");
            advancedLoadsBlocksDumps[blocksHashs[beginBlock]] = parseDumpBlockBinary(responses[i], isCompress);
        } else {
            const std::vector<std::string> blocks = parseDumpBlocksBinary(responses[i], isCompress);
            CHECK(blocks.size() == blocksInPart, "Incorrect answer");
            CHECK(beginBlock + blocks.size() <= blocksHashs.size(), "Incorrect answer");
            for (size_t j = 0; j < blocks.size(); j++) {
                advancedLoadsBlocksDumps[blocksHashs[beginBlock + j]] = blocks[j];
            }
        }
    }
    
    return advancedLoadsBlocksDumps[blockHash];
}

void GetNewBlocksFromServer::loadAdditingBlocks(std::vector<AdditingBlock> &blocks, const std::vector<std::string> &hintsServers, bool isSign) {
    const size_t countParts = (blocks.size() + countBlocksInBatch - 1) / countBlocksInBatch;
    
    const auto makeQsAndPost = [&blocks, isSign, countBlocksInBatch=this->countBlocksInBatch, isCompress=this->isCompress](size_t number) {
        CHECK(blocks.size() > number * countBlocksInBatch, "Incorrect number");
        const size_t beginBlock = number * countBlocksInBatch;
        const size_t countBlocks = std::min(countBlocksInBatch, blocks.size() - beginBlock);
        if (countBlocks == 1) {
            return std::make_pair("", makeGetDumpBlockMessage(blocks[beginBlock].hash, isSign, isCompress));
        } else {
            std::vector<std::string> hashes;
            hashes.reserve(countBlocks);
            std::transform(blocks.begin() + beginBlock, blocks.begin() + beginBlock + countBlocks, std::back_inserter(hashes), std::mem_fn(&AdditingBlock::hash));
            return std::make_pair("", makeGetDumpsBlocksMessage(hashes.begin(), hashes.end(), isSign, isCompress));
        }
    };
    
    const std::vector<std::string> responses = p2p.requests(countParts, makeQsAndPost, "", std::bind(parseDumpBlockResponse, true, isSign, isCompress, _1, _2, _3), hintsServers);
    CHECK(responses.size() == countParts, "Incorrect responses");
    
    for (size_t i = 0; i < responses.size(); i++) {
        const size_t beginBlock = i * countBlocksInBatch;
        const size_t blocksInPart = std::min(countBlocksInBatch, blocks.size() - beginBlock);
        
        if (blocksInPart == 1) {
            CHECK(beginBlock < blocks.size(), "Incorrect answer");
            blocks[beginBlock].dump = parseDumpBlockBinary(responses[i], isCompress);
        } else {
            const std::vector<std::string> bs = parseDumpBlocksBinary(responses[i], isCompress);
            CHECK(bs.size() == blocksInPart, "Incorrect answer");
            CHECK(beginBlock + bs.size() <= blocks.size(), "Incorrect answer");
            for (size_t j = 0; j < bs.size(); j++) {
                blocks[beginBlock + j].dump = bs[j];
            }
        }
    }
}

}
