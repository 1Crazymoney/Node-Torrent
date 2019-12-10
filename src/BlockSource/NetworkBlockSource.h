#ifndef NETWORK_BLOCK_SOURCE_H_
#define NETWORK_BLOCK_SOURCE_H_

#include "BlockSource.h"

#include "OopUtils.h"

#include "BlockInfo.h"

#include "GetNewBlocksFromServers.h"

#include <string>
#include <map>
#include <vector>
#include <set>

namespace torrent_node_lib {
    
class P2P;

class BlocksTimeline;

class NetworkBlockSource final: public BlockSource, common::no_copyable, common::no_moveable {
public:
    
    NetworkBlockSource(const BlocksTimeline &timeline, const std::string &folderPath, size_t maxAdvancedLoadBlocks, size_t countBlocksInBatch, bool isCompress, P2P &p2p, bool saveAllTx, bool isValidate, bool isVerifySign, bool isPreLoad);
    
    void initialize() override;
    
    std::pair<bool, size_t> doProcess(size_t countBlocks) override;
    
    bool process(std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsBlockInfo> &bi, std::string &binaryDump) override;
    
    void getExistingBlock(const BlockHeader &bh, BlockInfo &bi, std::string &blockDump) const override;
    
    ~NetworkBlockSource() override = default;
    
private:
    
    struct AdvancedBlock {
        enum BlockPos {
            BeforeBlock = 0,
            Block = 1,
            AfterBlock = 2
        };
        
        struct Key {
            std::string hash;
            size_t number;
            BlockPos pos;
            
            Key(const std::string &hash, size_t number, BlockPos pos)
                : hash(hash)
                , number(number)
                , pos(pos)
            {}
            
            bool operator<(const Key &second) const;
        };
        
        BlockPos pos;
        MinimumBlockHeader header;
        std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsBlockInfo> bi;
        std::string dump;
        std::exception_ptr exception;
        
        Key key() const;
    };
    
private:
    
    void processAdditingBlocks(std::vector<GetNewBlocksFromServer::AdditingBlock> &additingBlocks);
    
    void parseBlockInfo();
    
private:
    const BlocksTimeline &timeline;
    
    GetNewBlocksFromServer getterBlocks;
    
    const std::string folderPath;
    
    size_t nextBlockToRead = 0;
    
    std::vector<std::string> servers;
    
    size_t lastBlockInBlockchain = 0;
    
    bool saveAllTx;
    
    const bool isValidate;
    
    const bool isVerifySign;
  
    const bool isPreLoad;
    
    std::map<AdvancedBlock::Key, AdvancedBlock> advancedBlocks;
    
    std::map<AdvancedBlock::Key, AdvancedBlock>::iterator currentProcessedBlock;
    
    std::string lastFileName;
    
};

}

#endif // NETWORK_BLOCK_SOURCE_H_
