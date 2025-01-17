#ifndef BLOCKCHAIN_READ_H_
#define BLOCKCHAIN_READ_H_

#include <string>
#include <vector>
#include <fstream>
#include <variant>

#include "utils/IfStream.h"
#include "blockchain_structs/TransactionInfo.h"
#include "blockchain_structs/SignBlock.h"
#include "blockchain_structs/RejectedTxsBlock.h"

namespace torrent_node_lib {

struct TransactionInfo;
struct BlockInfo;
struct SignBlockInfo;
struct RejectedTxsBlockInfo;

/**
 *c Возвращает размер файла до записи в него
 */
size_t saveBlockToFileBinary(const std::string &fileName, const std::string &data);

void openFile(IfStream &file, const std::string &fileName);

void flushFile(IfStream &file, const std::string &fileName);

void closeFile(IfStream &file);

bool readOneSimpleTransactionInfo(IfStream &ifile, size_t currPos, TransactionInfo &txInfo, bool isSaveAllTx);

std::variant<std::monostate, BlockInfo, SignBlockInfo, RejectedTxsMinimumBlockHeader> parseNextBlockInfo(const char *begin_pos, const char *end_pos, size_t posInFile, bool isValidate, bool isSaveAllTx, size_t beginTx, size_t countTx);

RejectedTxsBlockInfo parseRejectedTxsBlockInfo(const char *begin_pos, const char *end_pos, size_t posInFile, bool isValidate);

size_t readNextBlockDump(IfStream &ifile, size_t currPos, std::string &blockDump);

std::pair<size_t, std::string> getBlockDump(IfStream &ifile, size_t currPos, size_t fromByte, size_t toByte);

}

#endif // BLOCKCHAIN_READ_H_
