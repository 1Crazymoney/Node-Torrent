#ifndef P2P_GRAPH_H_
#define P2P_GRAPH_H_

#include "P2P.h"

#include "P2P_Impl.h"

#include <vector>

#include "utils/Graph.h"

#include "curlWrapper.h"

#include "LimitArray.h"

using GraphString = Graph<std::string>;

class P2P_Graph: public torrent_node_lib::P2P {   
public:
    
    P2P_Graph(const std::vector<std::pair<std::string, std::string>> &graphVec, const std::string &thisIp, size_t countConnections);
    
    /**
     * c Выполняет запрос по всем серверам. Результаты возвращает в callback.
     *c callback должен быть готов к тому, что его будут вызывать из нескольких потоков.
     */
    void broadcast(const std::string &qs, const std::string &postData, const std::string &header, const torrent_node_lib::BroadcastResult &callback) const override;
    
    std::string request(size_t responseSize, bool isPrecisionSize, const torrent_node_lib::MakeQsAndPostFunction &makeQsAndPost, const std::string &header, const torrent_node_lib::ResponseParseFunction &responseParse, const std::vector<std::string> &hintsServers) override;
       
    std::vector<std::string> requests(size_t countRequests, const torrent_node_lib::MakeQsAndPostFunction2 &makeQsAndPost, const std::string &header, const torrent_node_lib::ResponseParseFunction &responseParse, const std::vector<std::string> &hintsServers) override;
    
    std::string runOneRequest(const std::string &server, const std::string &qs, const std::string &postData, const std::string &header) const override;
   
    torrent_node_lib::SendAllResult requestAll(const std::string &qs, const std::string &postData, const std::string &header, const std::set<std::string> &additionalServers) const override;
    
private:
    
    size_t getMaxServersCount(const std::string &srvr) const;
    
    std::vector<torrent_node_lib::P2P_Impl::ThreadDistribution> getServersList(const std::string &server, size_t countSegments) const;
        
    std::vector<std::string> requestImpl(size_t responseSize, size_t minResponseSize, bool isPrecisionSize, const torrent_node_lib::MakeQsAndPostFunction &makeQsAndPost, const std::string &header, const torrent_node_lib::ResponseParseFunction &responseParse, const std::vector<std::string> &hintsServers); 
    
private:
    
    torrent_node_lib::P2P_Impl p2p;
    
    GraphString graph;
    
    const GraphString::Element *parent;
    
    common::CurlInstance curlBroadcast;
    
    size_t countConnections;
    
};

#endif // P2P_GRAPH_H_
