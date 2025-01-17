#include "P2P_Ips.h"

#include "curlWrapper.h"

#include "check.h"
#include "log.h"

#include "parallel_for.h"

using namespace common;

namespace torrent_node_lib {

const static size_t SIZE_PARALLEL_BROADCAST = 8;
    
P2P_Ips::P2P_Ips(const std::vector<std::string> &servers, size_t countConnections)
    : p2p(countConnections * servers.size())
    , servers(servers.begin(), servers.end())
    , countConnections(countConnections)
{
    CHECK(countConnections != 0, "Incorrect count connections: 0");
    Curl::initialize();
    
    for (size_t i = 0; i < SIZE_PARALLEL_BROADCAST; i++) {
        curlsBroadcast.emplace_back(Curl::getInstance());
    }
}

P2P_Ips::~P2P_Ips() = default;

void P2P_Ips::broadcast(const std::string &qs, const std::string &postData, const std::string &header, const BroadcastResult& callback) const {
    parallelFor(SIZE_PARALLEL_BROADCAST, servers.begin(), servers.end(), [&qs, &postData, &header, &callback, this](size_t threadNum, const std::string &server) {
        try {
            const std::string response = P2P_Impl::request(curlsBroadcast.at(threadNum), qs, postData, header, server);
            callback(server, response, {});
        } catch (const exception &e) {
            callback(server, "", CurlException(e));
            return;
        }
        
        checkStopSignal();
    });
    
    checkStopSignal();
}

std::string P2P_Ips::runOneRequest(const std::string& server, const std::string& qs, const std::string& postData, const std::string& header) const {
    return P2P_Impl::request(Curl::getInstance(), qs, postData, header, server);
}

size_t P2P_Ips::getMaxServersCount(const std::vector<std::string> &srvrs) const {
    return countConnections * srvrs.size();
}

std::vector<P2P_Impl::ThreadDistribution> P2P_Ips::getServersList(const std::vector<std::string> &srves, size_t countSegments) const {
    std::vector<P2P_Impl::ThreadDistribution> result;
    
    const size_t currConnections = std::min((countSegments + srves.size() - 1) / srves.size(), countConnections);
    
    const size_t maxConnectionsForServer = countConnections;
    CHECK(currConnections <= maxConnectionsForServer, "Ups");
    
    for (const std::string &srv: srves) {
        const auto found = std::find(servers.begin(), servers.end(), srv);
        CHECK(found != servers.end(), "where did this come from?");
        const size_t index = std::distance(servers.begin(), found);
        result.emplace_back(index * maxConnectionsForServer, index * maxConnectionsForServer + currConnections, srv);
    }
    return result;
}

std::vector<std::string> P2P_Ips::requestImpl(size_t responseSize, size_t minResponseSize, bool isPrecisionSize, const torrent_node_lib::MakeQsAndPostFunction &makeQsAndPost, const std::string &header, const torrent_node_lib::ResponseParseFunction &responseParse, const std::vector<std::string> &/*hintsServers*/) {
    CHECK(responseSize != 0, "response size 0");
    
    const bool isMultitplyRequests = minResponseSize == 1;
    
    const size_t maxServers = getMaxServersCount(servers);
    
    const size_t countSegments = isMultitplyRequests ? responseSize : std::min((responseSize + minResponseSize - 1) / minResponseSize, maxServers);
    const auto requestServers = getServersList(servers, countSegments);
    
    std::vector<std::string> answers(countSegments);
    std::mutex answersMut;

    const ProcessResponse processResponse = [&answers, &answersMut, &header, &responseParse, isPrecisionSize, this](const std::string &response, const Segment &segment) {        
        const ResponseParse parsed = responseParse(response, segment.fromByte, segment.toByte);
        CHECK(!parsed.error.has_value(), parsed.error.value());
        if (isPrecisionSize) {
            CHECK(parsed.response.size() == segment.toByte - segment.fromByte, "Incorrect response size");
        }
        
        std::lock_guard<std::mutex> lock(answersMut);
        if (answers.at(segment.posInArray).empty()) {
            answers.at(segment.posInArray) = parsed.response;
            return true;
        } else {
            return false;
        }
    };
    
    const std::vector<Segment> segments = P2P_Impl::makeSegments(countSegments, responseSize, minResponseSize);
    const bool isSuccess = p2p.process(requestServers, segments, makeQsAndPost, processResponse);
    CHECK(isSuccess, "dont run request");
    
    return answers;
}

std::string P2P_Ips::request(size_t responseSize, bool isPrecisionSize, const MakeQsAndPostFunction& makeQsAndPost, const std::string& header, const ResponseParseFunction& responseParse, const std::vector<std::string> &hintsServers) {
    const size_t MIN_RESPONSE_SIZE = 10000;
    
    const std::vector<std::string> answers = requestImpl(responseSize, MIN_RESPONSE_SIZE, isPrecisionSize, makeQsAndPost, header, responseParse, hintsServers);
    
    std::string response;
    response.reserve(responseSize);
    for (const std::string &answer: answers) {
        response += answer;
    }
    if (isPrecisionSize) {
        CHECK(response.size() == responseSize, "response size != getted response. Getted response size: " + std::to_string(response.size()) + ". Expected response size: " + std::to_string(responseSize));
    }
    
    return response;
}

std::vector<std::string> P2P_Ips::requests(size_t countRequests, const MakeQsAndPostFunction2 &makeQsAndPost, const std::string &header, const ResponseParseFunction &responseParse, const std::vector<std::string> &hintsServers) {
    const auto makeQsAndPortImpl = [makeQsAndPost](size_t from, size_t to) {
        CHECK(to == from + 1, "Incorrect call makeQsAndPortFunc");
        return makeQsAndPost(from);
    };
    const std::vector<std::string> answers = requestImpl(countRequests, 1, false, makeQsAndPortImpl, header, responseParse, hintsServers);
    CHECK(answers.size() == countRequests, "Incorrect count answers");
    return answers;
}

SendAllResult P2P_Ips::requestAll(const std::string &qs, const std::string &postData, const std::string &header, const std::set<std::string> &additionalServers) const {
    const auto &mainElementsGraph = servers;
    //const std::vector<Server> mainServers(mainElementsGraph.begin(), mainElementsGraph.end());
    const std::vector<std::string> otherServers(additionalServers.begin(), additionalServers.end());
    std::vector<std::reference_wrapper<const std::string>> allServers;
    //allServers.insert(allServers.end(), mainServers.begin(), mainServers.end());
    allServers.insert(allServers.end(), otherServers.begin(), otherServers.end());
    
    const auto requestFunction = [this](const std::string &qs, const std::string &post, const std::string &header, const std::string &server) -> std::string {
        return P2P_Impl::request(Curl::getInstance(), qs, post, header, server);
    };
    
    return P2P_Impl::process(allServers, qs, postData, header, requestFunction);
}

}
