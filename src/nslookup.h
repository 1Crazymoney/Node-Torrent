#ifndef NS_LOOKUP_H_
#define NS_LOOKUP_H_

#include <string>
#include <vector>

struct NsResult {
    std::string server;
    unsigned long long timeout;

    NsResult()
        : timeout(0)
    {}
    NsResult(const std::string &server, unsigned long long timeout)
        : server(server)
        , timeout(timeout)
    {}
};

std::vector<NsResult> getBestIps(const std::string &address, size_t count);

void lookup_best_ip();

#endif // NS_LOOKUP_H_
