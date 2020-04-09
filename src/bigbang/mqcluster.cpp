// Copyright (c) 2019-2020 The Bigbang developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mqcluster.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread> // For sleep

#include "async_client.h"

using namespace std;

namespace bigbang
{

CMQCluster::CMQCluster(int catNodeIn)
  : thrMqttClient("mqttcli", boost::bind(&CMQCluster::MqttThreadFunc, this)),
    pCoreProtocol(nullptr),
    pBlockChain(nullptr),
    pService(nullptr),
    fAuth(false),
    fAbort(false),
    addrBroker("tcp://localhost:1883"),
    nReqBlkTimerID(0),
    nRollNum(0),
    clientID("")
{
    switch (catNodeIn)
    {
    case 0:
        catNode = NODE_CATEGORY::BBCNODE;
        break;
    case 1:
        catNode = NODE_CATEGORY::FORKNODE;
        break;
    case 2:
        catNode = NODE_CATEGORY::DPOSNODE;
        break;
    }
}

bool CMQCluster::IsAuthenticated()
{
    return fAuth;
}

bool CMQCluster::HandleInitialize()
{
    if (NODE_CATEGORY::BBCNODE == catNode)
    {
        Log("CMQCluster::HandleInitialize(): bbc node so bypass");
        return true;
    }

    if (!GetObject("coreprotocol", pCoreProtocol))
    {
        Error("Failed to request coreprotocol");
        return false;
    }

    if (!GetObject("blockchain", pBlockChain))
    {
        Error("Failed to request blockchain");
        return false;
    }

    if (!GetObject("dispatcher", pDispatcher))
    {
        Error("Failed to request dispatcher\n");
        return false;
    }

    if (!GetObject("service", pService))
    {
        Error("Failed to request service");
        return false;
    }

    if (!GetObject("forkmanager", pForkManager))
    {
        Error("Failed to request forkmanager");
        return false;
    }

    Log("CMQCluster::HandleInitialize() successfully");
    return true;
}

void CMQCluster::HandleDeinitialize()
{
    pCoreProtocol = nullptr;
    pBlockChain = nullptr;
    pDispatcher = nullptr;
    pService = nullptr;
    pForkManager = nullptr;
}

bool CMQCluster::HandleInvoke()
{
    if (NODE_CATEGORY::BBCNODE == catNode)
    {
        Log("CMQCluster::HandleInvoke(): bbc node so bypass");
        return true;
    }

    std::vector<storage::CSuperNode> nodes;
    if (!pBlockChain->FetchSuperNode(nodes))
    {
        Log("CMQCluster::HandleInvoke(): list super node failed");
        return false;
    }
    for (const auto& node : nodes)
    {
        mapSuperNode.insert(make_pair(node.superNodeID, node.vecOwnedForks));
        if (1 == node.vecOwnedForks.size()
            && node.vecOwnedForks[0] == pCoreProtocol->GetGenesisBlockHash())
        {
            Log("dpos node of MQ: [%s] [%d]", node.superNodeID.c_str(), node.ipAddr);
        }
        else if (0 != node.ipAddr)
        {
            Log("fork node of MQ: [%s] [%d]", node.superNodeID.c_str(), node.ipAddr);
        }
        for (const auto& fork : node.vecOwnedForks)
        {
            Log("CMQCluster::HandleInvoke(): list fork/dpos node [%s] with fork [%s]",
                node.superNodeID.c_str(), fork.ToString().c_str());
        }
    }

    if (NODE_CATEGORY::FORKNODE == catNode)
    {
        lastHeightResp = pBlockChain->GetBlockCount(pCoreProtocol->GetGenesisBlockHash()) - 1;

        if (mapSuperNode.size() == 0)
        {
            Log("CMQCluster::HandleInvoke(): this fork node has not enrolled "
                "itself to dpos node yet[%d]",
                mapSuperNode.size());
        }

        if (mapSuperNode.size() > 1)
        {
            Error("CMQCluster::HandleInvoke(): fork node should only have one "
                  "single enrollment but [%d]",
                  mapSuperNode.size());
            return false;
        }

        if (1 == mapSuperNode.size())
        {
            pForkManager->SetForkFilter(mapSuperNode.begin()->second);

            clientID = mapSuperNode.begin()->first;
            topicReqBlk = "Cluster01/" + clientID + "/SyncBlockReq";
            topicRespBlk = "Cluster01/" + clientID + "/SyncBlockResp";
            topicRbBlk = "Cluster01/DPOSNODE/UpdateBlock";
            Log("CMQCluster::HandleInvoke(): fork node clientid [%s] with topics:"
                "\t[%s]\n\t[%s]",
                clientID.c_str(),
                topicRespBlk.c_str(), topicRbBlk.c_str());
            for (const auto& fork : mapSuperNode.begin()->second)
            {
                Log("CMQCluster::HandleInvoke(): fork [%s] intended to be produced "
                    "by this node [%s]:",
                    fork.ToString().c_str(), clientID.c_str());
            }

/*            if (!PostBlockRequest(-1))
            {
                Error("CMQCluster::HandleInvoke(): failed to post requesting block");
                return false;
            }*/
        }
    }
    else if (NODE_CATEGORY::DPOSNODE == catNode)
    {
        lastHeightResp = -1;
        for (const auto& node : mapSuperNode)
        {
            if (1 == node.second.size()
                && node.second[0] == pCoreProtocol->GetGenesisBlockHash())
            { //dpos node
                clientID = node.first;
                topicReqBlk = "Cluster01/+/SyncBlockReq";
                topicRbBlk = "Cluster01/DPOSNODE/UpdateBlock";
                Log("CMQCluster::HandleInvoke(): dpos node clientid [%s] with topic [%s]",
                    clientID.c_str(), topicReqBlk.c_str());
            }
        }
    }

    if (!ThreadStart(thrMqttClient))
    {
        return false;
    }
    return IIOModule::HandleInvoke();
}

void CMQCluster::HandleHalt()
{
    if (NODE_CATEGORY::BBCNODE == catNode)
    {
        Log("CMQCluster::HandleHalt(): bbc node so go passby");
        return;
    }

    IIOModule::HandleHalt();

    fAbort = true;

    condMQ.notify_all();
    if (thrMqttClient.IsRunning())
    {
        thrMqttClient.Interrupt();
    }
    ThreadExit(thrMqttClient);
}

bool CMQCluster::HandleEvent(CEventMQSyncBlock& eventMqSyncBlock)
{
    return true;
}

bool CMQCluster::HandleEvent(CEventMQChainUpdate& eventMqUpdateChain)
{
    Log("CMQCluster::HandleEvent(): entering forking event handler");
    CMqRollbackUpdate& update = eventMqUpdateChain.data;

    if (catNode != NODE_CATEGORY::DPOSNODE)
    {
        Error("CMQCluster::HandleEvent(): only dpos node should receive this kind of event");
        return false;
    }

    CRollbackBlock rbc;
    rbc.rbHeight = update.triHeight;
    rbc.rbHash = update.triHash;
    rbc.rbSize = update.actRollBackLen;
    rbc.hashList = update.vShort;

    CBufferPtr spRBC(new CBufStream);
    *spRBC.get() << rbc;

    Log("CMQCluster::HandleEvent(): rollback-topic[%s]:"
        "forkheight[%d] forkhash[%s] shortlen[%d]",
        topicRbBlk.c_str(), rbc.rbHeight, rbc.rbHash.ToString().c_str(), rbc.rbSize);

    {
        boost::unique_lock<boost::mutex> lock(mtxSend);
        deqSendBuff.emplace_back(make_pair(topicRbBlk, spRBC));
    }
    condSend.notify_all();

    Log("CMQCluster::HandleEvent(): exiting forking event handler");
    return true;
}

bool CMQCluster::HandleEvent(CEventMQEnrollUpdate& eventMqUpdateEnroll)
{
    string id = eventMqUpdateEnroll.data.superNodeClientID;
    vector<uint256> forks = eventMqUpdateEnroll.data.vecForksOwned;
    if (NODE_CATEGORY::FORKNODE == catNode)
    {
        clientID = id;
        ipAddr = eventMqUpdateEnroll.data.ipAddr;
        topicReqBlk = "Cluster01/" + clientID + "/SyncBlockReq";
        topicRespBlk = "Cluster01/" + clientID + "/SyncBlockResp";
        topicRbBlk = "Cluster01/DPOSNODE/UpdateBlock";
        Log("CMQCluster::HandleEvent(): fork node clientid [%s] ip [%d] with topics:"
            "\n[%s]\n[%s]",
            clientID.c_str(), ipAddr,
            topicRespBlk.c_str(), topicRbBlk.c_str());
        for (const auto& fork : forks)
        {
            Log("CMQCluster::HandleEvent(): fork [%s] intended to be produced "
                "by this node [%s]:",
                fork.ToString().c_str(), clientID.c_str());
        }

        {
            boost::unique_lock<boost::mutex> lock(mtxStatus);
            mapSuperNode.clear();
            mapSuperNode.insert(make_pair(id, forks));
        }
        condStatus.notify_all();

        if (!PostBlockRequest(-1))
        {
            Error("CMQCluster::HandleEvent(): failed to post requesting block");
            return false;
        }
    }
    else if (NODE_CATEGORY::DPOSNODE == catNode)
    {
        if (1 == forks.size() && 0 == eventMqUpdateEnroll.data.ipAddr
            && forks[0] == pCoreProtocol->GetGenesisBlockHash())
        { //dpos node
            clientID = id;
            topicReqBlk = "Cluster01/+/SyncBlockReq";
            topicRbBlk = "Cluster01/DPOSNODE/UpdateBlock";
            Log("CMQCluster::HandleEvent(): dpos node clientid [%s] with topic [%s]",
                clientID.c_str(), topicReqBlk.c_str());

            {
                boost::unique_lock<boost::mutex> lock(mtxStatus);
                mapSuperNode.insert(make_pair(id, forks));
            }
            condStatus.notify_all();
        }
        else
        { //fork nodes either enrolled or p2p
            //mapActiveSuperNode[ip] = storage::CSuperNode();
            Log("CMQCluster::HandleEvent(): dpos node register clientid [%s] with topic [%s]",
                clientID.c_str(), topicReqBlk.c_str());
        }
    }

    return true;
}

bool CMQCluster::HandleEvent(CEventMQAgreement& eventMqAgreement)
{
    return true;
}

bool CMQCluster::LogEvent(const string& info)
{
    cout << "callback to CMQCluster when MQ-EVENT" << info << endl;
    Log("CMQCluster::LogMQEvent[%s]", info.c_str());
    return true;
}

bool CMQCluster::PostBlockRequest(int syncHeight)
{
    Log("CMQCluster::PostBlockRequest(): posting request for block #%d", syncHeight);

    if (mapSuperNode.empty())
    {
        Log("CMQCluster::PostBlockRequest(): enrollment is empty for this fork node");
        return true;
    }

    if (mapSuperNode.size() > 1)
    {
        Error("CMQCluster::PostBlockRequest(): enrollment is incorrect for this fork node");
        return false;
    }

    uint256 hash;
    int height;
    if (-1 == syncHeight)
    {
        int64 ts;
        if (!pBlockChain->GetLastBlock(pCoreProtocol->GetGenesisBlockHash(), hash, height, ts))
        {
            Error("CMQCluster::PostBlockRequest(): failed to get last block");
            return false;
        }
    }
    else
    {
        if (nRollNum)
        {
            boost::unique_lock<boost::mutex> lock(mtxRoll);
            hash = vLongFork.back();
        }
        else
        {
            if (!pBlockChain->GetBlockHash(pCoreProtocol->GetGenesisBlockHash(), syncHeight, hash))
            {
                Error("CMQCluster::PostBlockRequest(): failed to get specific block");
                return false;
            }
        }
        height = syncHeight;
    }
    Log("CMQCluster::PostBlockRequest(): posting request for block hash[%s]", hash.ToString().c_str());

    CSyncBlockRequest req;
    req.ipAddr = ipAddr; //16777343 - 127.0.0.1; 1111638320 - "0ABB"
    req.forkNodeIdLen = clientID.size();
    req.forkNodeId = clientID;
    auto enroll = mapSuperNode.begin();
    req.forkNum = (*enroll).second.size();
    req.forkList = (*enroll).second;
    req.lastHeight = height;
    req.lastHash = hash;
    req.tsRequest = GetTime();
    req.nonce = 1;

    CBufferPtr spSS(new CBufStream);
    *spSS.get() << req;

    AppendSendQueue(topicReqBlk, spSS);
    return true;
}

bool CMQCluster::AppendSendQueue(const std::string& topic,
                                 CBufferPtr payload)
{
    {
        boost::unique_lock<boost::mutex> lock(mtxSend);
        deqSendBuff.emplace_back(make_pair(topic, payload));
    }
    condSend.notify_all();

    return true;
}

void CMQCluster::RequestBlockTimerFunc(uint32 nTimer)
{
    if (nReqBlkTimerID == nTimer)
    {
        if (!PostBlockRequest(-1))
        {
            Error("CMQCluster::RequestBlockTimerFunc(): failed to post request");
        }
        nReqBlkTimerID = SetTimer(1000 * 60,
                                  boost::bind(&CMQCluster::RequestBlockTimerFunc, this, _1));
    }
}

void CMQCluster::OnReceiveMessage(const std::string& topic, CBufStream& payload)
{
    payload.Dump();

    switch (catNode)
    {
    case NODE_CATEGORY::BBCNODE:
        Error("CMQCluster::OnReceiveMessage(): bbc node should not come here!");
        return;
    case NODE_CATEGORY::FORKNODE:
    {
        Log("CMQCluster::OnReceiveMessage(): current sync height is [%d]", int(lastHeightResp));
        if (topicRbBlk != topic)
        { //respond to request block of main chain
            //unpack payload
            CSyncBlockResponse resp;
            try
            {
                payload >> resp;
            }
            catch (exception& e)
            {
                StdError(__PRETTY_FUNCTION__, e.what());
                Error("CMQCluster::OnReceiveMessage(): failed to unpack respond msg");
                return;
            }

            if (-1 == resp.height)
            { //has reached the best height for the first time communication,
                // then set timer to process the following business rather than req/resp model
                nReqBlkTimerID = SetTimer(1000 * 30,
                                          boost::bind(&CMQCluster::RequestBlockTimerFunc, this, _1));
                return;
            }

            //check if this msg is just for me
            //if (topicReqBlk != clientID)

            //validate this coming block
            Errno err = pCoreProtocol->ValidateBlock(resp.block);
            if (OK != err)
            {
                Error("CMQCluster::OnReceiveMessage(): failed to validate block");
                return;
            }

            //notify to add new block
            err = pDispatcher->AddNewBlock(resp.block);
            if (err != OK)
            {
                Error("CMQCluster::OnReceiveMessage(): failed to add new block (%d) : %s\n", err, ErrorString(err));
                if (ERR_ALREADY_HAVE == err)
                {
                    lastHeightResp = resp.height;
                    //check if there are rollbacked blocks
                    if (nRollNum)
                    {
                        boost::unique_lock<boost::mutex> lock(mtxRoll);
                        if (vLongFork.size() < nRollNum)
                        {
                            vLongFork.emplace_back(resp.hash);
                        }
                        else
                        {
                            vLongFork.clear();
                            nRollNum = 0;
                        }
                    }
                    if (!PostBlockRequest(lastHeightResp))
                    {
                        Error("CMQCluster::OnReceiveMessage(): failed to post request on response due to duplication");
                        return;
                    }
                }
                return;
            }
            lastHeightResp = resp.height;

            //check if there are rollbacked blocks
            if (nRollNum)
            {
                boost::unique_lock<boost::mutex> lock(mtxRoll);
                if (vLongFork.size() < nRollNum)
                {
                    vLongFork.emplace_back(resp.hash);
                }
                else
                {
                    vLongFork.clear();
                    nRollNum = 0;
                }
            }

            //iterate to retrieve next one

            if (resp.isBest)
            { //when reach best height, send request by timer
                nReqBlkTimerID = SetTimer(1000 * 60,
                                          boost::bind(&CMQCluster::RequestBlockTimerFunc, this, _1));
                return;
            }
            else
            {
                if (0 != nReqBlkTimerID)
                {
                    CancelTimer(nReqBlkTimerID);
                    nReqBlkTimerID = 0;
                }
                if (!PostBlockRequest(lastHeightResp))
                {
                    Error("CMQCluster::OnReceiveMessage(): failed to post request on response");
                    return;
                }
            }
        }
        else
        { //roll back blocks on main chain
            //unpack payload
            CRollbackBlock rb;
            try
            {
                payload >> rb;
            }
            catch (exception& e)
            {
                StdError(__PRETTY_FUNCTION__, e.what());
                Error("CMQCluster::OnReceiveMessage(): failed to unpack rollback msg");
                return;
            }

            if (rb.rbHeight < lastHeightResp)
            {
                Log("CMQCluster::OnReceiveMessage(): rbheight[%d], lastheight[%d]",
                    rb.rbHeight, int(lastHeightResp));

                //cancel request sync timer
                if (nReqBlkTimerID != 0)
                {
                    CancelTimer(nReqBlkTimerID);
                    nReqBlkTimerID = 0;
                }

                //empty sending buffer
                {
                    boost::unique_lock<boost::mutex> lock(mtxSend);
                    deqSendBuff.clear();
                }

                //check hard fork point
                uint256 hash;
                if (!pBlockChain->GetBlockHash(pCoreProtocol->GetGenesisBlockHash(), rb.rbHeight, hash))
                {
                    Error("CMQCluster::OnReceiveMessage(): failed to get hard fork block hash or dismatch"
                          "then re-synchronize block from genesis one");
                    if (!PostBlockRequest(0)) //re-sync from genesis block
                    {
                        Error("CMQCluster::OnReceiveMessage(): failed to post request while re-sync");
                    }
                    return;
                }

                if (hash != rb.rbHash)
                {
                    Error("CMQCluster::OnReceiveMessage(): hashes do not match - rbhash[%s], lasthash[%s]",
                          rb.rbHash.ToString().c_str(), hash.ToString().c_str());
                    if (!PostBlockRequest(0)) //re-sync from genesis block
                    {
                        Error("CMQCluster::OnReceiveMessage(): failed to post request while re-sync");
                    }
                    return;
                }

                bool fMatch = false;
                Log("CMQCluster::OnReceiveMessage(): rbhash[%s], lasthash[%s]",
                    rb.rbHash.ToString().c_str(), hash.ToString().c_str());

                //check blocks in rollback
                int nShort = 0;
                for (int i = 0; i < rb.rbSize; ++i)
                {
                    if (!pBlockChain->GetBlockHash(pCoreProtocol->GetGenesisBlockHash(),
                                                   rb.rbHeight + i + 1, hash))
                    {
                        if (i != 0)
                        {
                            Log("CMQCluster::OnReceiveMessage(): exceed to get rollback block hash");
                            break;
                        }
                        else
                        {
                            Error("CMQCluster::OnReceiveMessage(): short chain does not match for one on dpos node:1");
                            return;
                        }
                    }
                    Log("CMQCluster::OnReceiveMessage(): fork node blkhsh[%s] vs. dpos node blkhsh[%s] "
                        "at height of [%d]", rb.hashList[i].ToString().c_str(), hash.ToString().c_str(),
                        rb.rbHeight + i + 1);
                    if (hash == rb.hashList[i])
                    {
                        Log("CMQCluster::OnReceiveMessage(): fork node has not been rolled back yet"
                            " with hash [%s]",
                            hash.ToString().c_str());
                        fMatch = true;
                        ++nShort;
                        continue;
                    }
                    else
                    {
                        Error("CMQCluster::OnReceiveMessage(): short chain does not match for one on dpos node:2");
                        if (!PostBlockRequest(0)) //re-sync from genesis block
                        {
                            Error("CMQCluster::OnReceiveMessage(): failed to post request while re-sync");
                        }
                        return;
                    }
                }
                Log("CMQCluster::OnReceiveMessage(): fork node rb[%d] against dpos node rb[%d]", nShort, rb.rbSize);

                //re-request from hard fork point to sync the best chain
                if (fMatch)
                {
                    lastHeightResp = rb.rbHeight;
                    Log("CMQCluster::OnReceiveMessage(): match to prepare rollback: rb.rbHeight[%d] against lastHeightResp[%d]", rb.rbHeight, int(lastHeightResp));
                    if (!PostBlockRequest(lastHeightResp))
                    {
                        Error("CMQCluster::OnReceiveMessage(): failed to post request on rollback");
                        nRollNum = rb.rbSize;
                        return;
                    }
                    nRollNum = rb.rbSize;
                }
            }
        }

        break;
    }
    case NODE_CATEGORY::DPOSNODE:
    {
        //unpack payload
        CSyncBlockRequest req;
        try
        {
            payload >> req;
        }
        catch (exception& e)
        {
            StdError(__PRETTY_FUNCTION__, e.what());
            Error("CMQCluster::OnReceiveMessage(): failed to unpack request msg");
            return;
        }

        //check if requesting fork node has been enrolled
        auto node = mapSuperNode.find(req.forkNodeId);
        if (node == mapSuperNode.end())
        {
            Error("CMQCluster::OnReceiveMessage(): requesting fork node has not enrolled yet");
            return;
        }
        //check if requesting fork node matches the corresponding one enrolled
        if (node->second.size() != req.forkNum)
        {
            Error("CMQCluster::OnReceiveMessage(): requesting fork node number does not match");
            return;
        }
        for (const auto& fork : req.forkList)
        {
            auto pos = find(node->second.begin(), node->second.end(), fork);
            if (pos == node->second.end())
            {
                Error("CMQCluster::OnReceiveMessage(): requesting fork node detailed forks does not match");
                return;
            }
        }

        //add this requesting fork node to active list
        if (!mapActiveSuperNode.count(req.ipAddr))
        {
            mapActiveSuperNode[req.ipAddr] = storage::CSuperNode(req.forkNodeId, req.ipAddr, req.forkList);
        }

        //processing request from fork node

        //check height requested
        int best = pBlockChain->GetBlockCount(pCoreProtocol->GetGenesisBlockHash()) - 1;
        CSyncBlockResponse resp;
        if (req.lastHeight > best)
        {
            Error("CMQCluster::OnReceiveMessage(): block height owned by fork node "
                  "should not be greater than the best one on dpos node");
            return;
        }
        else if (req.lastHeight == best)
        {
            Log("CMQCluster::OnReceiveMessage(): block height owned by fork node "
                "has reached the best one on dpos node, please wait...");
            resp.height = -1;
            resp.hash = uint256();
            resp.isBest = 1;
            resp.block = CBlock();
            resp.blockSize = xengine::GetSerializeSize(resp.block);
        }
        else
        {
            //check height and hash are matched
            uint256 hash;
            if (!pBlockChain->GetBlockHash(pCoreProtocol->GetGenesisBlockHash(), req.lastHeight, hash))
            {
                Error("CMQCluster::OnReceiveMessage(): failed to get checking height and hash match "
                      "at height of #%d", req.lastHeight);
                return;
            }
            if (hash != req.lastHash)
            {
                Error("CMQCluster::OnReceiveMessage(): height and hash do not match hash[%s] vs. req.lastHash[%s] "
                      "at height of [%d]", hash.ToString().c_str(), req.lastHash.ToString().c_str(), req.lastHeight);
                return;
            }
            if (!pBlockChain->GetBlockHash(pCoreProtocol->GetGenesisBlockHash(), req.lastHeight + 1, hash))
            {
                Error("CMQCluster::OnReceiveMessage(): failed to get next block hash at height of #%d", req.lastHeight + 1);
                return;
            }
            CBlock block;
            if (!pBlockChain->GetBlock(hash, block))
            {
                Error("CMQCluster::OnReceiveMessage(): failed to get next block");
                return;
            }

            //reply block requested
            resp.height = req.lastHeight + 1;
            resp.hash = hash;
            resp.isBest = req.lastHeight + 1 < best
                              ? 0
                              : 1;
            Log("CMQCluster::OnReceiveMessage(): request[%d] best[%d] isBest[%d]",
                req.lastHeight + 1, best, resp.isBest);
            resp.blockSize = xengine::GetSerializeSize(block);
            resp.block = move(block);
        }

        CBufferPtr spSS(new CBufStream);
        *spSS.get() << resp;
        string topicRsp = "Cluster01/" + req.forkNodeId + "/SyncBlockResp";
        {
            boost::unique_lock<boost::mutex> lock(mtxSend);
            deqSendBuff.emplace_back(make_pair(topicRsp, spSS));
        }
        condSend.notify_all();

        break;
    }
    }
}

class CActionListener : public virtual mqtt::iaction_listener
{
    std::string strName;

    void on_failure(const mqtt::token& tok) override
    {
        std::cout << strName << " failure";
        if (tok.get_message_id() != 0)
            std::cout << " for token: [" << tok.get_message_id() << "]" << std::endl;
        std::cout << std::endl;
    }

    void on_success(const mqtt::token& tok) override
    {
        std::cout << strName << " success";
        if (tok.get_message_id() != 0)
            std::cout << " for token: [" << tok.get_message_id() << "]" << std::endl;
        auto top = tok.get_topics();
        if (top && !top->empty())
            std::cout << "\ttoken topic: '" << (*top)[0] << "', ..." << std::endl;
        std::cout << std::endl;
    }

public:
    CActionListener(const std::string& name)
      : strName(name) {}
};

const int RETRY_ATTEMPTS = 3;
class CMQCallback :
  public virtual mqtt::callback,
  public virtual mqtt::iaction_listener
{
    mqtt::async_client& asynCli;
    mqtt::connect_options& connOpts;
    CMQCluster& mqCluster;
    uint8 nRetry;
    CActionListener subListener;

public:
    CMQCallback(mqtt::async_client& cli, mqtt::connect_options& connOpts, CMQCluster& clusterIn)
      : asynCli(cli), connOpts(connOpts), mqCluster(clusterIn), nRetry(0), subListener("sublistener") {}

    void reconnect()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        try
        {
            asynCli.connect(connOpts, nullptr, *this);
            mqCluster.LogEvent("[reconnect...]");
        }
        catch (const mqtt::exception& exc)
        {
            cerr << "Error: " << exc.what() << std::endl;
            mqCluster.Error("[MQTT_reconnect_ERROR!]");
        }
        mqCluster.LogEvent("[reconnected]");
    }

    void on_failure(const mqtt::token& tok) override
    {
        cout << "\tListener failure for token: [multiple]"
             << tok.get_message_id() << endl;
        mqCluster.LogEvent("[on_failure]");
        if (++nRetry > RETRY_ATTEMPTS)
        {
            mqCluster.Error("[MQTT_retry_to_reconnect_FAILURE!]");
        }
        reconnect();
    }

    void on_success(const mqtt::token& tok) override
    {
        cout << "\tListener success for token: [multiple]"
             << tok.get_message_id() << endl;
        mqCluster.LogEvent("[on_success]");
    }

    void connected(const string& cause) override
    {
        cout << "\nConnection success" << endl;
        if (!cause.empty())
        {
            cout << "\tcause: " << cause << endl;
        }
        mqCluster.LogEvent("[connected]");
        if (CMQCluster::NODE_CATEGORY::FORKNODE == mqCluster.catNode)
        {
            asynCli.subscribe(mqCluster.topicRespBlk, CMQCluster::QOS1, nullptr, subListener);
            cout << "\nSubscribing to topic '" << mqCluster.topicRespBlk << "'\n"
                 << "\tfor client " << mqCluster.clientID
                 << " using QoS" << CMQCluster::QOS1 << endl;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            asynCli.subscribe(mqCluster.topicRbBlk, CMQCluster::QOS1, nullptr, subListener);
            cout << "\nSubscribing to topic '" << mqCluster.topicRbBlk << "'\n"
                 << "\tfor client " << mqCluster.clientID
                 << " using QoS" << CMQCluster::QOS1 << endl;
        }
        else if (CMQCluster::NODE_CATEGORY::DPOSNODE == mqCluster.catNode)
        {
            asynCli.subscribe(mqCluster.topicReqBlk, CMQCluster::QOS1, nullptr, subListener);
            cout << "\nSubscribing to topic '" << mqCluster.topicReqBlk << "'\n"
                 << "\tfor client " << mqCluster.clientID
                 << " using QoS" << CMQCluster::QOS1 << endl;
        }
        cout << endl;
        mqCluster.LogEvent("[subscribed]");
        cout << endl;
    }

    void connection_lost(const string& cause) override
    {
        cout << "\nConnection lost" << endl;
        if (!cause.empty())
        {
            cout << "\tcause: " << cause << endl;
        }
        mqCluster.LogEvent("[connection_lost]");
        nRetry = 0;
        reconnect();
    }

    void message_arrived(mqtt::const_message_ptr msg) override
    {
        cout << "Message arrived" << endl;
        cout << "\ttopic: '" << msg->get_topic() << "'" << endl;
        cout << "\tpayload: '" << msg->to_string() << "'\n"
             << endl;
        mqCluster.LogEvent("[message_arrived]");
        xengine::CBufStream ss;
        ss.Write((const char*)&msg->get_payload()[0], msg->get_payload().size());
        mqCluster.OnReceiveMessage(msg->get_topic(), ss);
    }

    void delivery_complete(mqtt::delivery_token_ptr tok) override
    {
        cout << "\tDelivery complete for token: "
             << (tok ? tok->get_message_id() : -1) << endl;
        mqCluster.LogEvent("[delivery_complete]");
    }
};

bool CMQCluster::ClientAgent(MQ_CLI_ACTION action)
{
    try
    {
        static mqtt::async_client client(addrBroker, clientID);

        static mqtt::connect_options connOpts;
        connOpts.set_keep_alive_interval(20);
        connOpts.set_clean_session(true);

        static mqtt::token_ptr conntok;
        static mqtt::delivery_token_ptr delitok;

        static CMQCallback cb(client, connOpts, *this);

        switch (action)
        {
        case MQ_CLI_ACTION::CONN:
        {
            cout << "Initializing for server '" << addrBroker << "'..." << endl;
            client.set_callback(cb);
            cout << "  ...OK" << endl;

            cout << "\nConnecting..." << endl;
            conntok = client.connect();
            cout << "Waiting for the connection..." << endl;
            conntok->wait();
            cout << "  ...OK" << endl;
            break;
        }
        case MQ_CLI_ACTION::SUB:
        {
            break;
        }
        case MQ_CLI_ACTION::PUB:
        {
            {
                boost::unique_lock<boost::mutex> lock(mtxSend);
                while (!deqSendBuff.empty())
                {
                    pair<string, CBufferPtr> buf = deqSendBuff.front();
                    cout << "\nSending message to [" << buf.first << "]..." << endl;
                    buf.second->Dump();

                    mqtt::message_ptr pubmsg = mqtt::make_message(
                        buf.first, buf.second->GetData(), buf.second->GetSize());
                    pubmsg->set_qos(QOS1);
                    pubmsg->set_retained(mqtt::message::DFLT_RETAINED);
                    delitok = client.publish(pubmsg, nullptr, cb);
                    delitok->wait_for(100);
                    cout << "_._._OK" << endl;

                    deqSendBuff.pop_front();
                }
            }
            condSend.notify_all();
            break;
        }
        case MQ_CLI_ACTION::DISCONN:
        {
            // Double check that there are no pending tokens

            auto toks = client.get_pending_delivery_tokens();
            if (!toks.empty())
                cout << "Error: There are pending delivery tokens!" << endl;

            // Disconnect
            cout << "\nDisconnecting..." << endl;
            conntok = client.disconnect();
            conntok->wait();
            cout << "  ...OK" << endl;
            break;
        }
        }
    }
    catch (const mqtt::exception& exc)
    {
        cerr << exc.what() << endl;
        return false;
    }

    return true;
}

void CMQCluster::MqttThreadFunc()
{
    Log("entering thread function of MQTT");

    //wait for supernode itself status available
    if (!fAbort)
    {
        boost::unique_lock<boost::mutex> lock(mtxStatus);
        while ("" == clientID)
        {
            Log("there is no enrollment info, waiting for it coming...");
            condStatus.wait(lock);
        }
    }

    //establish connection
    ClientAgent(MQ_CLI_ACTION::CONN);

    //subscribe topics
    ClientAgent(MQ_CLI_ACTION::SUB);

    //publish topics
    while (!fAbort)
    {
        {
            boost::unique_lock<boost::mutex> lock(mtxSend);
            while (deqSendBuff.empty())
            {
                condSend.wait(lock);
            }
        }
        ClientAgent(MQ_CLI_ACTION::PUB);
        Log("thread function of MQTT: go through an iteration");
    }

    //disconnect to broker
    ClientAgent(MQ_CLI_ACTION::DISCONN);

    Log("exiting thread function of MQTT");
}

} // namespace bigbang
