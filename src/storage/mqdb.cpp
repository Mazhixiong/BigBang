// Copyright (c) 2019-2020 The Bigbang developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mqdb.h"
#include "defs.h"
#include "leveldbeng.h"

using namespace std;
using namespace xengine;

namespace bigbang
{
namespace storage
{

//////////////////////////////
// CSuperNodeDB

bool CSuperNodeDB::Initialize(const boost::filesystem::path& pathData)
{
    CLevelDBArguments args;
    args.path = (pathData / "supernode").string();
    args.syncwrite = true;
    args.files = 16;
    args.cache = 2 << 20;

    CLevelDBEngine* engine = new CLevelDBEngine(args);

    if (!Open(engine))
    {
        delete engine;
        return false;
    }

    return true;
}

void CSuperNodeDB::Deinitialize()
{
    Close();
}

bool CSuperNodeDB::AddNewSuperNode(const CSuperNode& cli)
{
    bool ret = false;
    switch (cli.nodeCat)
    {
    case NODE_CAT_BBCNODE:
    {
        ret = Write(make_pair(CLIENT_ID_OUT_OF_MQ_CLUSTER, cli.ipAddr), cli.vecOwnedForks, true);
        break;
    }
    case NODE_CAT_FORKNODE:
    case NODE_CAT_DPOSNODE:
    {
        if (!ClearSuperNode(cli))
        {
            return false;
        }
        ret = Write(make_pair(cli.superNodeID, cli.ipAddr), cli.vecOwnedForks, true);
    }
    }
    return ret;
}

bool CSuperNodeDB::RemoveSuperNode(const string& cliID, const uint32& ipNum)
{
    return Erase(make_pair(cliID, ipNum));
}

bool CSuperNodeDB::RetrieveSuperNode(const string& cliID, const uint32& ipNum, vector<uint256>& vFork)
{
    return Read(make_pair(cliID, ipNum), vFork);
}

bool CSuperNodeDB::UpdateSuperNode(const string& cliID, const uint32& ipNum, const vector<uint256>& vFork)
{
    return Write(make_pair(cliID, ipNum), vFork, true);
}

bool CSuperNodeDB::ListSuperNode(std::vector<CSuperNode>& vCli)
{
    map<pair<string, uint32>, vector<uint256>> mapCli;

    if (!WalkThrough(boost::bind(&CSuperNodeDB::LoadSuperNodeWalker, this, _1, _2, boost::ref(mapCli))))
    {
        return false;
    }

    vCli.reserve(mapCli.size());
    for (const auto& it : mapCli)
    {
        CSuperNode node;
        node.superNodeID = it.first.first;
        node.ipAddr = it.first.second;
        node.vecOwnedForks = it.second;
        vCli.emplace_back(node);
    }
    return true;
}

void CSuperNodeDB::Clear()
{
    RemoveAll();
}

bool CSuperNodeDB::FetchSuperNodeWalker(xengine::CBufStream& ssKey, xengine::CBufStream& ssValue,
                                        map<pair<string, uint32>, vector<uint256>>& mapCli)
{
    string strCliID;
    uint32 nIP;
    ssKey >> strCliID >> nIP;

    if (strCliID == CLIENT_ID_OUT_OF_MQ_CLUSTER)
    {
        return true;
    }

    std::vector<uint256> forks;
    ssValue >> forks;
    mapCli.insert(make_pair(make_pair(strCliID, nIP), forks));
    return true;
}

bool CSuperNodeDB::FetchSuperNode(std::vector<CSuperNode>& vCli)
{
    map<pair<string, uint32>, vector<uint256>> mapCli;

    if (!WalkThrough(boost::bind(&CSuperNodeDB::FetchSuperNodeWalker, this, _1, _2, boost::ref(mapCli))))
    {
        return false;
    }

    vCli.reserve(mapCli.size());
    for (const auto& it : mapCli)
    {
        CSuperNode node;
        node.superNodeID = it.first.first;
        node.ipAddr = it.first.second;
        node.vecOwnedForks = it.second;
        vCli.emplace_back(node);
    }
    return true;
}

bool CSuperNodeDB::ClearSuperNode(const CSuperNode& cli)
{
    vector<CSuperNode> vSuperNode;
    if (!FetchSuperNode(vSuperNode))
    {
        return false;
    }

    if (NODE_CAT_FORKNODE == cli.nodeCat) //fork node updates self by remove all supernode entries
    {
        for (auto const& supernode : vSuperNode)
        {
            if (!RemoveSuperNode(supernode.superNodeID, supernode.ipAddr))
            {
                return false;
            }
        }
        return true;
    }

    if (NODE_CAT_DPOSNODE == cli.nodeCat && 0 == cli.ipAddr) //dpos node updates oneself
    {
        for (auto const& supernode : vSuperNode)
        {
            if (0 == supernode.ipAddr && !RemoveSuperNode(supernode.superNodeID, supernode.ipAddr)) //need to remove previous one first
            {
                return false;
            }
        }
    }

    return true;
}

bool CSuperNodeDB::LoadSuperNodeWalker(xengine::CBufStream& ssKey, xengine::CBufStream& ssValue,
                                       map<pair<string, uint32>, vector<uint256>>& mapCli)
{
    string strCliID;
    uint32 nIP;
    ssKey >> strCliID >> nIP;
    std::vector<uint256> forks;
    ssValue >> forks;
    mapCli.insert(make_pair(make_pair(strCliID, nIP), forks));
    return true;
}

} // namespace storage
} // namespace bigbang
