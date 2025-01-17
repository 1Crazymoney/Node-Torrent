#include "NetworkBlockSource.h"

#include "utils/FileSystem.h"
#include "log.h"
#include "check.h"
#include "parallel_for.h"
#include "convertStrings.h"

#include "blockchain_structs/BlockInfo.h"
#include "BlockchainRead.h"
#include "GetNewBlocksFromServers.h"
#include "PrivateKey.h"

#include "BlocksTimeline.h"
#include "blockchain_structs/SignBlock.h"
#include "blockchain_structs/RejectedTxsBlock.h"

using namespace common;

namespace torrent_node_lib {

const static size_t COUNT_ADVANCED_BLOCKS = 8;
    
NetworkBlockSource::AdvancedBlock::Key NetworkBlockSource::AdvancedBlock::key() const {
    return Key(header.hash, header.number, pos);
}

bool NetworkBlockSource::AdvancedBlock::Key::operator<(const Key &second) const {
    return std::make_tuple(this->number, this->pos, this->hash) < std::make_tuple(second.number, second.pos, second.hash);
}

NetworkBlockSource::NetworkBlockSource(const BlocksTimeline &timeline, const std::string &folderPath, size_t maxAdvancedLoadBlocks, size_t countBlocksInBatch, bool isCompress, P2P &p2p, bool saveAllTx, bool isValidate, bool isVerifySign, bool isPreLoad) 
    : timeline(timeline)
    , getterBlocks(maxAdvancedLoadBlocks, countBlocksInBatch, p2p, isCompress)
    , folderPath(folderPath)
    , saveAllTx(saveAllTx)
    , isValidate(isValidate)
    , isVerifySign(isVerifySign)
    , isPreLoad(isPreLoad)
{}

void NetworkBlockSource::initialize() {
    createDirectories(folderPath);
}

size_t NetworkBlockSource::doProcess(size_t countBlocks) {
    nextBlockToRead = countBlocks + 1;
    
    advancedBlocks.clear();
    getterBlocks.clearAdvanced();
    currentProcessedBlock = advancedBlocks.end();
    afterBlocksAdditings.clear();
    
    if (!isPreLoad) {
        const GetNewBlocksFromServer::LastBlockResponse lastBlock = getterBlocks.getLastBlock();
        CHECK(!lastBlock.error.has_value(), lastBlock.error.value());
        lastBlockInBlockchain = lastBlock.lastBlock;
        servers = lastBlock.servers;
        
        if (lastBlockInBlockchain == nextBlockToRead - 1) {
            afterBlocksAdditings.cleared = false;
            afterBlocksAdditings.file = lastFileName;
            afterBlocksAdditings.blockNumber = lastBlockInBlockchain;
            afterBlocksAdditings.hashes.assign(lastBlock.extraBlocks.begin(), lastBlock.extraBlocks.end());
        }
    } else {
        const GetNewBlocksFromServer::LastBlockPreLoadResponse lastBlock = getterBlocks.preLoadBlocks(countBlocks, isVerifySign);
        CHECK(!lastBlock.error.has_value(), lastBlock.error.value());
        CHECK(lastBlock.lastBlock.has_value(), "Last block not setted");
        lastBlockInBlockchain = lastBlock.lastBlock.value();
        servers = lastBlock.servers;
        
        const auto additionalBlocksHashes = getterBlocks.addPreLoadBlocks(nextBlockToRead, lastBlock.blockHeaders, lastBlock.additionalBlockHashes, lastBlock.blocksDumps);
        if (lastBlockInBlockchain == nextBlockToRead - 1) {
            afterBlocksAdditings.cleared = false;
            afterBlocksAdditings.file = lastFileName;
            afterBlocksAdditings.blockNumber = lastBlockInBlockchain;
            afterBlocksAdditings.hashes.assign(additionalBlocksHashes.begin(), additionalBlocksHashes.end());
        }
    }
    
    return lastBlockInBlockchain;
}

void NetworkBlockSource::processAdditingBlocks(std::vector<AdditingBlock> &additingBlocks) {
    std::set<std::string> existingHashs;
    additingBlocks.erase(std::remove_if(additingBlocks.begin(), additingBlocks.end(), [&existingHashs](const AdditingBlock &block) {
        if (existingHashs.find(block.hash) != existingHashs.end()) {
            return true;
        } else {
            existingHashs.insert(block.hash);
            return false;
        }
    }), additingBlocks.end());
    
    timeline.filter<AdditingBlock>(additingBlocks, [](const AdditingBlock &block) {
        return fromHex(block.hash);
    });
       
    for (const AdditingBlock &additingBlock: additingBlocks) {
        const std::string dump = getterBlocks.getBlockDump(additingBlock.hash, 0, false, true, servers, isVerifySign);
        
        AdvancedBlock advanced;
        advanced.header.blockSize = dump.size();
        advanced.header.hash = additingBlock.hash;
        advanced.header.number = additingBlock.number;
        advanced.header.fileName = additingBlock.fileName;
        advanced.dump = dump;
        advanced.pos = additingBlock.type == AdditingBlock::Type::AfterBlock ? AdvancedBlock::BlockPos::AfterBlock : AdvancedBlock::BlockPos::BeforeBlock;
        advancedBlocks.emplace(advanced.key(), advanced);
    }
}

void NetworkBlockSource::parseBlockInfo() {
    parallelFor(8, advancedBlocks.begin(), advancedBlocks.end(), [this](auto &pair) {
        AdvancedBlock &advanced = pair.second;
        if (advanced.exception) {
            return;
        }
        try {
            BlockSignatureCheckResult signBlock;
            if (isVerifySign) {
                signBlock = checkSignatureBlock(advanced.dump);
                advanced.dump = signBlock.block;
            }
            CHECK(advanced.dump.size() == advanced.header.blockSize, "binaryDump.size() == nextBlockHeader.blockSize");
            const std::vector<unsigned char> hashBlockForRequest = fromHex(advanced.header.hash);
            auto tmp = parseNextBlockInfo(advanced.dump.data(), advanced.dump.data() + advanced.dump.size(), 0, isValidate, saveAllTx, 0, 0);
            advanced.bi = std::visit([&hashBlockForRequest, &signBlock, &advanced, this](auto &element) -> std::variant<BlockInfo, SignBlockInfo> {
                using T = std::decay_t<decltype(element)>;
                if constexpr (std::is_same_v<T, BlockInfo>) {
                    CHECK(element.header.hash == hashBlockForRequest, "Incorrect block dump");
                    element.saveFilePath(getBasename(advanced.header.fileName));
                    
                    if (isVerifySign) {
                        element.saveSenderInfo(std::vector<unsigned char>(signBlock.sign.begin(), signBlock.sign.end()), std::vector<unsigned char>(signBlock.pubkey.begin(), signBlock.pubkey.end()), std::vector<unsigned char>(signBlock.address.begin(), signBlock.address.end()));
                    }

                    return element;
                } else if constexpr (std::is_same_v<T, SignBlockInfo>) {
                    CHECK(element.header.hash == hashBlockForRequest, "Incorrect block dump");
                    element.saveFilePath(getBasename(advanced.header.fileName));
                    
                    if (isVerifySign) {
                        element.saveSenderInfo(std::vector<unsigned char>(signBlock.sign.begin(), signBlock.sign.end()), std::vector<unsigned char>(signBlock.pubkey.begin(), signBlock.pubkey.end()), std::vector<unsigned char>(signBlock.address.begin(), signBlock.address.end()));
                    }

                    return element;
                } else {
                    throwErr("Incorrect block type");
                }
            }, tmp);
        } catch (...) {
            advanced.exception = std::current_exception();
        }
    });
    
    currentProcessedBlock = advancedBlocks.begin();
}

bool NetworkBlockSource::process(std::variant<std::monostate, BlockInfo, SignBlockInfo> &bi, std::string &binaryDump) {
    const bool isContinue = currentProcessedBlock != advancedBlocks.end() || !afterBlocksAdditings.isClear() || lastBlockInBlockchain >= nextBlockToRead;
    if (!isContinue) {
        return false;
    }
        
    const auto processAdvanced = [this](std::variant<std::monostate, BlockInfo, SignBlockInfo> &bi, std::string &binaryDump, std::map<AdvancedBlock::Key, AdvancedBlock>::iterator &iter) {
        const AdvancedBlock &advanced = iter->second;
        if (advanced.exception) {
            std::rethrow_exception(advanced.exception);
        }
        bi = std::visit([](const auto &element) -> std::variant<std::monostate, BlockInfo, SignBlockInfo>{
            return element;
        }, advanced.bi);
        binaryDump = advanced.dump;
        
        lastFileName = advanced.header.fileName;
        
        const bool isSimpleBlock = advanced.pos == AdvancedBlock::BlockPos::Block;
                
        iter++;
        if (isSimpleBlock) {
            nextBlockToRead++;
        }
    };
    
    if (currentProcessedBlock != advancedBlocks.end()) {
        processAdvanced(bi, binaryDump, currentProcessedBlock);
        return true;
    }
    
    std::vector<AdditingBlock> additingBlocks;
    if (afterBlocksAdditings.isClear()) {
        CHECK(lastBlockInBlockchain >= nextBlockToRead, "New blocks absent");
        
        advancedBlocks.clear();
        
        const size_t countAdvanced = std::min(COUNT_ADVANCED_BLOCKS, lastBlockInBlockchain - nextBlockToRead + 1);
        
        CHECK(!servers.empty(), "Servers empty");
        for (size_t i = 0; i < countAdvanced; i++) {
            const size_t currBlock = nextBlockToRead + i;
            AdvancedBlock advanced;
            advanced.pos = AdvancedBlock::BlockPos::Block;
            try {
                advanced.header = getterBlocks.getBlockHeader(currBlock, lastBlockInBlockchain, servers);
                advanced.dump = getterBlocks.getBlockDump(advanced.header.hash, advanced.header.blockSize, true, false, servers, isVerifySign);
            } catch (...) {
                advanced.exception = std::current_exception();
            }
            CHECK(currBlock == advanced.header.number, "Incorrect data");
            advancedBlocks.emplace(advanced.key(), advanced);
        }
        
        for (const auto &[key, advanced]: advancedBlocks) {
            for (const std::string &blockHash: advanced.header.prevExtraBlocks) {
                additingBlocks.emplace_back(AdditingBlock::Type::BeforeBlock, advanced.header.number, blockHash, advanced.header.fileName);
            }
            for (const std::string &blockHash: advanced.header.nextExtraBlocks) {
                additingBlocks.emplace_back(AdditingBlock::Type::AfterBlock, advanced.header.number, blockHash, advanced.header.fileName);
            }
        }
    } else {
        CHECK(advancedBlocks.empty(), "Incorrect advanced blocks");
        
        for (const std::string &hash: afterBlocksAdditings.hashes) {
            additingBlocks.emplace_back(AdditingBlock::Type::AfterBlock, afterBlocksAdditings.blockNumber, hash, afterBlocksAdditings.file);
        }
            
        afterBlocksAdditings.clear();
    }
    
    processAdditingBlocks(additingBlocks);
    
    parseBlockInfo();
    
    if (currentProcessedBlock != advancedBlocks.end()) {
        processAdvanced(bi, binaryDump, currentProcessedBlock);
        return true;
    } else {
        return false;
    }
}

void NetworkBlockSource::confirmBlock(const torrent_node_lib::FileInfo &filepos) {
    // empty
}

void NetworkBlockSource::getExistingBlock(const BlockHeader& bh, BlockInfo& bi, std::string &blockDump) const {
    CHECK(bh.blockNumber.has_value(), "Block number not set");
    const GetNewBlocksFromServer::LastBlockResponse lastBlock = getterBlocks.getLastBlock();
    CHECK(!lastBlock.error.has_value(), lastBlock.error.value());
    const MinimumBlockHeader nextBlockHeader = getterBlocks.getBlockHeaderWithoutAdvanceLoad(bh.blockNumber.value(), lastBlock.servers[0]);
    blockDump = getterBlocks.getBlockDumpWithoutAdvancedLoad(nextBlockHeader.hash, nextBlockHeader.blockSize, true, lastBlock.servers, isVerifySign);
    if (isVerifySign) {
        const BlockSignatureCheckResult signBlock = checkSignatureBlock(blockDump);
        blockDump = signBlock.block;
        bi.header.senderSign.assign(signBlock.sign.begin(), signBlock.sign.end());
        bi.header.senderPubkey.assign(signBlock.pubkey.begin(), signBlock.pubkey.end());
        bi.header.senderAddress.assign(signBlock.address.begin(), signBlock.address.end());
    }
    CHECK(blockDump.size() == nextBlockHeader.blockSize, "binaryDump.size() == nextBlockHeader.blockSize");
    std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsMinimumBlockHeader> b =
        parseNextBlockInfo(blockDump.data(), blockDump.data() + blockDump.size(), bh.filePos.pos, isValidate, saveAllTx, 0, 0);
    CHECK(std::holds_alternative<BlockInfo>(b), "Incorrect blockinfo");
    bi = std::get<BlockInfo>(b);
    
    bi.header.filePos.fileNameRelative = bh.filePos.fileNameRelative;
    for (auto &tx : bi.txs) {
        tx.filePos.fileNameRelative = bh.filePos.fileNameRelative;
        tx.blockNumber = bh.blockNumber.value();
    }
    bi.header.blockNumber = bh.blockNumber;
}

}
