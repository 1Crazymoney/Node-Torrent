#ifndef FILE_BLOCK_SOURCE_H_
#define FILE_BLOCK_SOURCE_H_

#include "BlockSource.h"

#include "OopUtils.h"
#include "utils/FileSystem.h"

#include "blockchain_structs/BlockInfo.h"

#include <string>
#include <fstream>
#include <unordered_map>

#include "utils/IfStream.h"
#include "blockchain_structs/SignBlock.h"
#include "blockchain_structs/RejectedTxsBlock.h"
#include "blockchain_structs/BlocksMetadata.h"

namespace torrent_node_lib {
   
class LevelDb;

class FileRejectedBlockSource;

class FileBlockSource final: public BlockSource, common::no_copyable, common::no_moveable {
public:
    
    FileBlockSource(FileRejectedBlockSource &rejectedBlockSource, LevelDb &leveldb, const std::string &folderPath, bool isValidate);
    
    void initialize() override;
    
    size_t doProcess(size_t countBlocks) override;
       
    bool process(std::variant<std::monostate, BlockInfo, SignBlockInfo> &bi, std::string &binaryDump) override;

    void confirmBlock(const FileInfo &filepos) override;

    static void getExistingBlockS(const std::string &folder, const BlockHeader &bh, BlockInfo &bi, std::string &blockDump, bool isValidate);
    
    void getExistingBlock(const BlockHeader &bh, BlockInfo &bi, std::string &blockDump) const override;
    
    ~FileBlockSource() override = default;

private:

    void confirmBlockImpl(const FileInfo &filepos);

private:

    FileRejectedBlockSource &rejectedBlockSource;

    LevelDb &leveldb;
    
    const std::string folderPath;
       
    std::unordered_map<CroppedFileName, FileInfo> allFiles;
    
    size_t currPos = 0;
    IfStream file;
    std::string fileName;
    
    const bool isValidate;
    
};

}

#endif // FILE_BLOCK_SOURCE_H_
