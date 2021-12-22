// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activexnode.h"
#include "addrman.h"
#include "darksend.h"
//#include "governance.h"
#include "xnode-payments.h"
#include "xnode-sync.h"
#include "xnodeman.h"
#include "netfulfilledman.h"
#include "util.h"

/** Xnode manager */
CXnodeMan mnodeman;

const std::string CXnodeMan::SERIALIZATION_VERSION_STRING = "CXnodeMan-Version-4";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CXnode*>& t1,
                    const std::pair<int, CXnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<int64_t, CXnode*>& t1,
                    const std::pair<int64_t, CXnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CXnodeIndex::CXnodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CXnodeIndex::Get(int nIndex, CTxIn& vinXnode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinXnode = it->second;
    return true;
}

int CXnodeIndex::GetXnodeIndex(const CTxIn& vinXnode) const
{
    index_m_cit it = mapIndex.find(vinXnode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CXnodeIndex::AddXnodeVIN(const CTxIn& vinXnode)
{
    index_m_it it = mapIndex.find(vinXnode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinXnode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinXnode;
    ++nSize;
}

void CXnodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CXnode* t1,
                    const CXnode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CXnodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CXnodeMan::CXnodeMan() : cs(),
  vXnodes(),
  mAskedUsForXnodeList(),
  mWeAskedForXnodeList(),
  mWeAskedForXnodeListEntry(),
  mWeAskedForVerification(),
  mMnbRecoveryRequests(),
  mMnbRecoveryGoodReplies(),
  listScheduledMnbRequestConnections(),
  nLastIndexRebuildTime(0),
  indexXnodes(),
  indexXnodesOld(),
  fIndexRebuilt(false),
  fXnodesAdded(false),
  fXnodesRemoved(false),
//  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenXnodeBroadcast(),
  mapSeenXnodePing(),
  nDsqCount(0)
{}

bool CXnodeMan::Add(CXnode &mn)
{
    LOCK(cs);

    CXnode *pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("xnode", "CXnodeMan::Add -- Adding new Xnode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
        vXnodes.push_back(mn);
        indexXnodes.AddXnodeVIN(mn.vin);
        fXnodesAdded = true;
        return true;
    }

    return false;
}

void CXnodeMan::AskForMN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    LOCK(cs);

    std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it1 = mWeAskedForXnodeListEntry.find(vin.prevout);
    if (it1 != mWeAskedForXnodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CXnodeMan::AskForMN -- Asking same peer %s for missing xnode entry again: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CXnodeMan::AskForMN -- Asking new peer %s for missing xnode entry: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CXnodeMan::AskForMN -- Asking peer %s for missing xnode entry for the first time: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    }
    mWeAskedForXnodeListEntry[vin.prevout][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    pnode->PushMessage(NetMsgType::DSEG, vin);
}

void CXnodeMan::Check()
{
    LOCK(cs);

//    LogPrint("xnode", "CXnodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CXnode& mn, vXnodes) {
        mn.Check();
    }
}

void CXnodeMan::CheckAndRemove()
{
    if(!xnodeSync.IsXnodeListSynced()) return;

    LogPrintf("CXnodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateXnodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent xnodes, prepare structures and make requests to reasure the state of inactive ones
        std::vector<CXnode>::iterator it = vXnodes.begin();
        std::vector<std::pair<int, CXnode> > vecXnodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES xnode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        while(it != vXnodes.end()) {
            CXnodeBroadcast mnb = CXnodeBroadcast(*it);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if ((*it).IsOutpointSpent()) {
                LogPrint("xnode", "CXnodeMan::CheckAndRemove -- Removing Xnode: %s  addr=%s  %i now\n", (*it).GetStateString(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenXnodeBroadcast.erase(hash);
                mWeAskedForXnodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
//                it->FlagGovernanceItemsAsDirty();
                it = vXnodes.erase(it);
                fXnodesRemoved = true;
            } else {
                bool fAsk = pCurrentBlockIndex &&
                            (nAskForMnbRecovery > 0) &&
                            xnodeSync.IsSynced() &&
                            it->IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // calulate only once and only when it's needed
                    if(vecXnodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(pCurrentBlockIndex->nHeight);
                        vecXnodeRanks = GetXnodeRanks(nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL xnodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecXnodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForXnodeListEntry.count(it->vin.prevout) && mWeAskedForXnodeListEntry[it->vin.prevout].count(vecXnodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecXnodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("xnode", "CXnodeMan::CheckAndRemove -- Recovery initiated, xnode=%s\n", it->vin.prevout.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for XNODE_NEW_START_REQUIRED xnodes
        LogPrint("xnode", "CXnodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CXnodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("xnode", "CXnodeMan::CheckAndRemove -- reprocessing mnb, xnode=%s\n", itMnbReplies->second[0].vin.prevout.ToStringShort());
                    // mapSeenXnodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateXnodeList(NULL, itMnbReplies->second[0], nDos);
                }
                LogPrint("xnode", "CXnodeMan::CheckAndRemove -- removing mnb recovery reply, xnode=%s, size=%d\n", itMnbReplies->second[0].vin.prevout.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in XNODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Xnode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForXnodeList.begin();
        while(it1 != mAskedUsForXnodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForXnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Xnode list
        it1 = mWeAskedForXnodeList.begin();
        while(it1 != mWeAskedForXnodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForXnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Xnodes we've asked for
        std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it2 = mWeAskedForXnodeListEntry.begin();
        while(it2 != mWeAskedForXnodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForXnodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CXnodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenXnodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenXnodePing
        std::map<uint256, CXnodePing>::iterator it4 = mapSeenXnodePing.begin();
        while(it4 != mapSeenXnodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("xnode", "CXnodeMan::CheckAndRemove -- Removing expired Xnode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenXnodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenXnodeVerification
        std::map<uint256, CXnodeVerification>::iterator itv2 = mapSeenXnodeVerification.begin();
        while(itv2 != mapSeenXnodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("xnode", "CXnodeMan::CheckAndRemove -- Removing expired Xnode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenXnodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CXnodeMan::CheckAndRemove -- %s\n", ToString());

        if(fXnodesRemoved) {
            CheckAndRebuildXnodeIndex();
        }
    }

    if(fXnodesRemoved) {
        NotifyXnodeUpdates();
    }
}

void CXnodeMan::Clear()
{
    LOCK(cs);
    vXnodes.clear();
    mAskedUsForXnodeList.clear();
    mWeAskedForXnodeList.clear();
    mWeAskedForXnodeListEntry.clear();
    mapSeenXnodeBroadcast.clear();
    mapSeenXnodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexXnodes.Clear();
    indexXnodesOld.Clear();
}

int CXnodeMan::CountXnodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinXnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CXnode& mn, vXnodes) {
        if(mn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CXnodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinXnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CXnode& mn, vXnodes) {
        if(mn.nProtocolVersion < nProtocolVersion || !mn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 xnodes are allowed in 12.1, saving this for later
int CXnodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CXnode& mn, vXnodes)
        if ((nNetworkType == NET_IPV4 && mn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CXnodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForXnodeList.find(pnode->addr);
            if(it != mWeAskedForXnodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CXnodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    pnode->PushMessage(NetMsgType::DSEG, CTxIn());
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForXnodeList[pnode->addr] = askAgain;

    LogPrint("xnode", "CXnodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CXnode* CXnodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CXnode& mn, vXnodes)
    {
        if(GetScriptForDestination(mn.pubKeyCollateralAddress.GetID()) == payee)
            return &mn;
    }
    return NULL;
}

CXnode* CXnodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CXnode& mn, vXnodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

CXnode* CXnodeMan::Find(const CPubKey &pubKeyXnode)
{
    LOCK(cs);

    BOOST_FOREACH(CXnode& mn, vXnodes)
    {
        if(mn.pubKeyXnode == pubKeyXnode)
            return &mn;
    }
    return NULL;
}

bool CXnodeMan::Get(const CPubKey& pubKeyXnode, CXnode& xnode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CXnode* pMN = Find(pubKeyXnode);
    if(!pMN)  {
        return false;
    }
    xnode = *pMN;
    return true;
}

bool CXnodeMan::Get(const CTxIn& vin, CXnode& xnode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    xnode = *pMN;
    return true;
}

xnode_info_t CXnodeMan::GetXnodeInfo(const CTxIn& vin)
{
    xnode_info_t info;
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

xnode_info_t CXnodeMan::GetXnodeInfo(const CPubKey& pubKeyXnode)
{
    xnode_info_t info;
    LOCK(cs);
    CXnode* pMN = Find(pubKeyXnode);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

bool CXnodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CXnode* pMN = Find(vin);
    return (pMN != NULL);
}

char* CXnodeMan::GetNotQualifyReason(CXnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount)
{
    if (!mn.IsValidForPayment()) {
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'not valid for payment'");
        return reasonStr;
    }
    // //check protocol version
    if (mn.nProtocolVersion < mnpayments.GetMinXnodePaymentsProto()) {
        // LogPrintf("Invalid nProtocolVersion!\n");
        // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
        // LogPrintf("mnpayments.GetMinXnodePaymentsProto=%s!\n", mnpayments.GetMinXnodePaymentsProto());
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'Invalid nProtocolVersion', nProtocolVersion=%d", mn.nProtocolVersion);
        return reasonStr;
    }
    //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    if (mnpayments.IsScheduled(mn, nBlockHeight)) {
        // LogPrintf("mnpayments.IsScheduled!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'is scheduled'");
        return reasonStr;
    }
    //it's too new, wait for a cycle
    if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
        // LogPrintf("it's too new, wait for a cycle!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'too new', sigTime=%s, will be qualifed after=%s",
                DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
        return reasonStr;
    }
    //make sure it has at least as many confirmations as there are xnodes
    if (mn.GetCollateralAge() < nMnCount) {
        // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
        // LogPrintf("nMnCount=%s!\n", nMnCount);
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'collateralAge < xnCount', collateralAge=%d, xnCount=%d", mn.GetCollateralAge(), nMnCount);
        return reasonStr;
    }
    return NULL;
}

//
// Deterministically select the oldest/best xnode to pay on the network
//
CXnode* CXnodeMan::GetNextXnodeInQueueForPayment(bool fFilterSigTime, int& nCount)
{
    if(!pCurrentBlockIndex) {
        nCount = 0;
        return NULL;
    }
    return GetNextXnodeInQueueForPayment(pCurrentBlockIndex->nHeight, fFilterSigTime, nCount);
}

CXnode* CXnodeMan::GetNextXnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    CXnode *pBestXnode = NULL;
    std::vector<std::pair<int, CXnode*> > vecXnodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */
    int nMnCount = CountEnabled();
    int index = 0;
    BOOST_FOREACH(CXnode &mn, vXnodes)
    {
        index += 1;
        // LogPrintf("index=%s, mn=%s\n", index, mn.ToString());
        /*if (!mn.IsValidForPayment()) {
            LogPrint("xnodeman", "Xnode, %s, addr(%s), not-qualified: 'not valid for payment'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        // //check protocol version
        if (mn.nProtocolVersion < mnpayments.GetMinXnodePaymentsProto()) {
            // LogPrintf("Invalid nProtocolVersion!\n");
            // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
            // LogPrintf("mnpayments.GetMinXnodePaymentsProto=%s!\n", mnpayments.GetMinXnodePaymentsProto());
            LogPrint("xnodeman", "Xnode, %s, addr(%s), not-qualified: 'invalid nProtocolVersion'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (mnpayments.IsScheduled(mn, nBlockHeight)) {
            // LogPrintf("mnpayments.IsScheduled!\n");
            LogPrint("xnodeman", "Xnode, %s, addr(%s), not-qualified: 'IsScheduled'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's too new, wait for a cycle
        if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
            // LogPrintf("it's too new, wait for a cycle!\n");
            LogPrint("xnodeman", "Xnode, %s, addr(%s), not-qualified: 'it's too new, wait for a cycle!', sigTime=%s, will be qualifed after=%s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
            continue;
        }
        //make sure it has at least as many confirmations as there are xnodes
        if (mn.GetCollateralAge() < nMnCount) {
            // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
            // LogPrintf("nMnCount=%s!\n", nMnCount);
            LogPrint("xnodeman", "Xnode, %s, addr(%s), not-qualified: 'mn.GetCollateralAge() < nMnCount', CollateralAge=%d, nMnCount=%d\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), mn.GetCollateralAge(), nMnCount);
            continue;
        }*/
        char* reasonStr = GetNotQualifyReason(mn, nBlockHeight, fFilterSigTime, nMnCount);
        if (reasonStr != NULL) {
            LogPrint("xnodeman", "Xnode, %s, addr(%s), qualify %s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), reasonStr);
            delete [] reasonStr;
            continue;
        }
        vecXnodeLastPaid.push_back(std::make_pair(mn.GetLastPaidBlock(), &mn));
    }
    nCount = (int)vecXnodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nMnCount / 3) {
        // LogPrintf("Need Return, nCount=%s, nMnCount/3=%s\n", nCount, nMnCount/3);
        return GetNextXnodeInQueueForPayment(nBlockHeight, false, nCount);
    }

    // Sort them low to high
    sort(vecXnodeLastPaid.begin(), vecXnodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CXnode::GetNextXnodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CXnode*)& s, vecXnodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestXnode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestXnode;
}

CXnode* CXnodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinXnodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CXnodeMan::FindRandomNotInVec -- %d enabled xnodes, %d xnodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CXnode*> vpXnodesShuffled;
    BOOST_FOREACH(CXnode &mn, vXnodes) {
        vpXnodesShuffled.push_back(&mn);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpXnodesShuffled.begin(), vpXnodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CXnode* pmn, vpXnodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(pmn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("xnode", "CXnodeMan::FindRandomNotInVec -- found, xnode=%s\n", pmn->vin.prevout.ToStringShort());
        return pmn;
    }

    LogPrint("xnode", "CXnodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CXnodeMan::GetXnodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CXnode*> > vecXnodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CXnode& mn, vXnodes) {
        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            if(!mn.IsEnabled()) continue;
        }
        else {
            if(!mn.IsValidForPayment()) continue;
        }
        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecXnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecXnodeScores.rbegin(), vecXnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CXnode*)& scorePair, vecXnodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CXnode> > CXnodeMan::GetXnodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CXnode*> > vecXnodeScores;
    std::vector<std::pair<int, CXnode> > vecXnodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecXnodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CXnode& mn, vXnodes) {

        if(mn.nProtocolVersion < nMinProtocol || !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecXnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecXnodeScores.rbegin(), vecXnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CXnode*)& s, vecXnodeScores) {
        nRank++;
        vecXnodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecXnodeRanks;
}

CXnode* CXnodeMan::GetXnodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CXnode*> > vecXnodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CXnode::GetXnodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CXnode& mn, vXnodes) {

        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive && !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecXnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecXnodeScores.rbegin(), vecXnodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CXnode*)& s, vecXnodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CXnodeMan::ProcessXnodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fXnode) {
            if(darkSendPool.pSubmittedToXnode != NULL && pnode->addr == darkSendPool.pSubmittedToXnode->addr) continue;
            // LogPrintf("Closing Xnode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

std::pair<CService, std::set<uint256> > CXnodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CXnodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

//    LogPrint("xnode", "CXnodeMan::ProcessMessage, strCommand=%s\n", strCommand);
    if(fLiteMode) return; // disable all GravityCoin specific functionality
    if(!xnodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::MNANNOUNCE) { //Xnode Broadcast
        CXnodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        LogPrintf("MNANNOUNCE -- Xnode announce, xnode=%s\n", mnb.vin.prevout.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateXnodeList(pfrom, mnb, nDos)) {
            // use announced Xnode as a peer
            addrman.Add(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fXnodesAdded) {
            NotifyXnodeUpdates();
        }
    } else if (strCommand == NetMsgType::MNPING) { //Xnode Ping

        CXnodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        LogPrint("xnode", "MNPING -- Xnode ping, xnode=%s\n", mnp.vin.prevout.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenXnodePing.count(nHash)) return; //seen
        mapSeenXnodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("xnode", "MNPING -- Xnode ping, xnode=%s new\n", mnp.vin.prevout.ToStringShort());

        // see if we have this Xnode
        CXnode* pmn = mnodeman.Find(mnp.vin);

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a xnode entry once
        AskForMN(pfrom, mnp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Xnode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after xnode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!xnodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("xnode", "DSEG -- Xnode list, xnode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForXnodeList.find(pfrom->addr);
                if (i != mAskedUsForXnodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForXnodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CXnode& mn, vXnodes) {
            if (vin != CTxIn() && vin != mn.vin) continue; // asked for specific vin but we are not there yet
            if (mn.addr.IsRFC1918() || mn.addr.IsLocal()) continue; // do not send local network xnode
            if (mn.IsUpdateRequired()) continue; // do not send outdated xnodes

            LogPrint("xnode", "DSEG -- Sending Xnode entry: xnode=%s  addr=%s\n", mn.vin.prevout.ToStringShort(), mn.addr.ToString());
            CXnodeBroadcast mnb = CXnodeBroadcast(mn);
            uint256 hash = mnb.GetHash();
            pfrom->PushInventory(CInv(MSG_XNODE_ANNOUNCE, hash));
            pfrom->PushInventory(CInv(MSG_XNODE_PING, mn.lastPing.GetHash()));
            nInvCount++;

            if (!mapSeenXnodeBroadcast.count(hash)) {
                mapSeenXnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));
            }

            if (vin == mn.vin) {
                LogPrintf("DSEG -- Sent 1 Xnode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            pfrom->PushMessage(NetMsgType::SYNCSTATUSCOUNT, XNODE_SYNC_LIST, nInvCount);
            LogPrintf("DSEG -- Sent %d Xnode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("xnode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::MNVERIFY) { // Xnode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CXnodeVerification mnv;
        vRecv >> mnv;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some xnode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some xnode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of xnodes via unique direct requests.

void CXnodeMan::DoFullVerificationStep()
{
    if(activeXnode.vin == CTxIn()) return;
    if(!xnodeSync.IsSynced()) return;

    std::vector<std::pair<int, CXnode> > vecXnodeRanks = GetXnodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    // Need LOCK2 here to ensure consistent locking order because the SendVerifyRequest call below locks cs_main
    // through GetHeight() signal in ConnectNode
    LOCK2(cs_main, cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecXnodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CXnode> >::iterator it = vecXnodeRanks.begin();
    while(it != vecXnodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("xnode", "CXnodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeXnode.vin) {
            nMyRank = it->first;
            LogPrint("xnode", "CXnodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d xnodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this xnode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS xnodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecXnodeRanks.size()) return;

    std::vector<CXnode*> vSortedByAddr;
    BOOST_FOREACH(CXnode& mn, vXnodes) {
        vSortedByAddr.push_back(&mn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecXnodeRanks.begin() + nOffset;
    while(it != vecXnodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("xnode", "CXnodeMan::DoFullVerificationStep -- Already %s%s%s xnode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecXnodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("xnode", "CXnodeMan::DoFullVerificationStep -- Verifying xnode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecXnodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("xnode", "CXnodeMan::DoFullVerificationStep -- Sent verification requests to %d xnodes\n", nCount);
}

// This function tries to find xnodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CXnodeMan::CheckSameAddr()
{
    if(!xnodeSync.IsSynced() || vXnodes.empty()) return;

    std::vector<CXnode*> vBan;
    std::vector<CXnode*> vSortedByAddr;

    {
        LOCK(cs);

        CXnode* pprevXnode = NULL;
        CXnode* pverifiedXnode = NULL;

        BOOST_FOREACH(CXnode& mn, vXnodes) {
            vSortedByAddr.push_back(&mn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CXnode* pmn, vSortedByAddr) {
            // check only (pre)enabled xnodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevXnode) {
                pprevXnode = pmn;
                pverifiedXnode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevXnode->addr) {
                if(pverifiedXnode) {
                    // another xnode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this xnode with the same ip is verified, ban previous one
                    vBan.push_back(pprevXnode);
                    // and keep a reference to be able to ban following xnodes with the same ip
                    pverifiedXnode = pmn;
                }
            } else {
                pverifiedXnode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevXnode = pmn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CXnode* pmn, vBan) {
        LogPrintf("CXnodeMan::CheckSameAddr -- increasing PoSe ban score for xnode %s\n", pmn->vin.prevout.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CXnodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CXnode*>& vSortedByAddr)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("xnode", "CXnodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = ConnectNode(addr, NULL, false, true);
    if(pnode == NULL) {
        LogPrintf("CXnodeMan::SendVerifyRequest -- can't connect to node to verify it, addr=%s\n", addr.ToString());
        return false;
    }

    netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
    // use random nonce, store it and require node to reply with correct one later
    CXnodeVerification mnv(addr, GetRandInt(999999), pCurrentBlockIndex->nHeight - 1);
    mWeAskedForVerification[addr] = mnv;
    LogPrintf("CXnodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);

    return true;
}

void CXnodeMan::SendVerifyReply(CNode* pnode, CXnodeVerification& mnv)
{
    // only xnodes can sign this, why would someone ask regular node?
    if(!fXNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
//        // peer should not ask us that often
        LogPrintf("XnodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("XnodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeXnode.service.ToString(), mnv.nonce, blockHash.ToString());

    if(!darkSendSigner.SignMessage(strMessage, mnv.vchSig1, activeXnode.keyXnode)) {
        LogPrintf("XnodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!darkSendSigner.VerifyMessage(activeXnode.pubKeyXnode, mnv.vchSig1, strMessage, strError)) {
        LogPrintf("XnodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CXnodeMan::ProcessVerifyReply(CNode* pnode, CXnodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CXnodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CXnodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CXnodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("XnodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

//    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CXnodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CXnode* prealXnode = NULL;
        std::vector<CXnode*> vpXnodesToBan;
        std::vector<CXnode>::iterator it = vXnodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(), mnv.nonce, blockHash.ToString());
        while(it != vXnodes.end()) {
            if(CAddress(it->addr, NODE_NETWORK) == pnode->addr) {
                if(darkSendSigner.VerifyMessage(it->pubKeyXnode, mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealXnode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated xnode
                    if(activeXnode.vin == CTxIn()) continue;
                    // update ...
                    mnv.addr = it->addr;
                    mnv.vin1 = it->vin;
                    mnv.vin2 = activeXnode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                            mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!darkSendSigner.SignMessage(strMessage2, mnv.vchSig2, activeXnode.keyXnode)) {
                        LogPrintf("XnodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!darkSendSigner.VerifyMessage(activeXnode.pubKeyXnode, mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("XnodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mnv.Relay();

                } else {
                    vpXnodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real xnode found?...
        if(!prealXnode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CXnodeMan::ProcessVerifyReply -- ERROR: no real xnode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CXnodeMan::ProcessVerifyReply -- verified real xnode %s for addr %s\n",
                    prealXnode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CXnode* pmn, vpXnodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("xnode", "CXnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealXnode->vin.prevout.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        LogPrintf("CXnodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake xnodes, addr %s\n",
                    (int)vpXnodesToBan.size(), pnode->addr.ToString());
    }
}

void CXnodeMan::ProcessVerifyBroadcast(CNode* pnode, const CXnodeVerification& mnv)
{
    std::string strError;

    if(mapSeenXnodeVerification.find(mnv.GetHash()) != mapSeenXnodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenXnodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("xnode", "XnodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.vin1.prevout == mnv.vin2.prevout) {
        LogPrint("xnode", "XnodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    mnv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("XnodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetXnodeRank(mnv.vin2, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION);

    if (nRank == -1) {
        LogPrint("xnode", "CXnodeMan::ProcessVerifyBroadcast -- Can't calculate rank for xnode %s\n",
                    mnv.vin2.prevout.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("xnode", "CXnodeMan::ProcessVerifyBroadcast -- Mastrernode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.vin2.prevout.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());

        CXnode* pmn1 = Find(mnv.vin1);
        if(!pmn1) {
            LogPrintf("CXnodeMan::ProcessVerifyBroadcast -- can't find xnode1 %s\n", mnv.vin1.prevout.ToStringShort());
            return;
        }

        CXnode* pmn2 = Find(mnv.vin2);
        if(!pmn2) {
            LogPrintf("CXnodeMan::ProcessVerifyBroadcast -- can't find xnode2 %s\n", mnv.vin2.prevout.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CXnodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", mnv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn1->pubKeyXnode, mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("XnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for xnode1 failed, error: %s\n", strError);
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn2->pubKeyXnode, mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("XnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for xnode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CXnodeMan::ProcessVerifyBroadcast -- verified xnode %s for addr %s\n",
                    pmn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CXnode& mn, vXnodes) {
            if(mn.addr != mnv.addr || mn.vin.prevout == mnv.vin1.prevout) continue;
            mn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("xnode", "CXnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mn.vin.prevout.ToStringShort(), mn.addr.ToString(), mn.nPoSeBanScore);
        }
        LogPrintf("CXnodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake xnodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CXnodeMan::ToString() const
{
    std::ostringstream info;

    info << "Xnodes: " << (int)vXnodes.size() <<
            ", peers who asked us for Xnode list: " << (int)mAskedUsForXnodeList.size() <<
            ", peers we asked for Xnode list: " << (int)mWeAskedForXnodeList.size() <<
            ", entries in Xnode list we asked for: " << (int)mWeAskedForXnodeListEntry.size() <<
            ", xnode index size: " << indexXnodes.GetSize() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CXnodeMan::UpdateXnodeList(CXnodeBroadcast mnb)
{
    try {
        LogPrintf("CXnodeMan::UpdateXnodeList\n");
        LOCK2(cs_main, cs);
        mapSeenXnodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
        mapSeenXnodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

        LogPrintf("CXnodeMan::UpdateXnodeList -- xnode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

        CXnode *pmn = Find(mnb.vin);
        if (pmn == NULL) {
            CXnode mn(mnb);
            if (Add(mn)) {
                xnodeSync.AddedXnodeList();
            }
        } else {
            CXnodeBroadcast mnbOld = mapSeenXnodeBroadcast[CXnodeBroadcast(*pmn).GetHash()].second;
            if (pmn->UpdateFromNewBroadcast(mnb)) {
                xnodeSync.AddedXnodeList();
                mapSeenXnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } catch (const std::exception &e) {
        PrintExceptionContinue(&e, "UpdateXnodeList");
    }
}

bool CXnodeMan::CheckMnbAndUpdateXnodeList(CNode* pfrom, CXnodeBroadcast mnb, int& nDos)
{
    // Need LOCK2 here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- xnode=%s\n", mnb.vin.prevout.ToStringShort());

        uint256 hash = mnb.GetHash();
        if (mapSeenXnodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- xnode=%s seen\n", mnb.vin.prevout.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenXnodeBroadcast[hash].first > XNODE_NEW_START_REQUIRED_SECONDS - XNODE_MIN_MNP_SECONDS * 2) {
                LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- xnode=%s seen update\n", mnb.vin.prevout.ToStringShort());
                mapSeenXnodeBroadcast[hash].first = GetTime();
                xnodeSync.AddedXnodeList();
            }
            // did we ask this node for it?
            if (pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- mnb=%s seen request\n", hash.ToString());
                if (mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (mnb.lastPing.sigTime > mapSeenXnodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CXnode mnTemp = CXnode(mnb);
                        mnTemp.Check();
                        LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetTime() - mnb.lastPing.sigTime) / 60, mnTemp.GetStateString());
                        if (mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- xnode=%s seen good\n", mnb.vin.prevout.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenXnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- xnode=%s new\n", mnb.vin.prevout.ToStringShort());

        if (!mnb.SimpleCheck(nDos)) {
            LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- SimpleCheck() failed, xnode=%s\n", mnb.vin.prevout.ToStringShort());
            return false;
        }

        // search Xnode list
        CXnode *pmn = Find(mnb.vin);
        if (pmn) {
            CXnodeBroadcast mnbOld = mapSeenXnodeBroadcast[CXnodeBroadcast(*pmn).GetHash()].second;
            if (!mnb.Update(pmn, nDos)) {
                LogPrint("xnode", "CXnodeMan::CheckMnbAndUpdateXnodeList -- Update() failed, xnode=%s\n", mnb.vin.prevout.ToStringShort());
                return false;
            }
            if (hash != mnbOld.GetHash()) {
                mapSeenXnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } // end of LOCK(cs);

    if(mnb.CheckOutpoint(nDos)) {
        Add(mnb);
        xnodeSync.AddedXnodeList();
        // if it matches our Xnode privkey...
        if(fXNode && mnb.pubKeyXnode == activeXnode.pubKeyXnode) {
            mnb.nPoSeBanScore = -XNODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CXnodeMan::CheckMnbAndUpdateXnodeList -- Got NEW Xnode entry: xnode=%s  sigTime=%lld  addr=%s\n",
                            mnb.vin.prevout.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeXnode.ManageState();
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CXnodeMan::CheckMnbAndUpdateXnodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.RelayXNode();
    } else {
        LogPrintf("CXnodeMan::CheckMnbAndUpdateXnodeList -- Rejected Xnode entry: %s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CXnodeMan::UpdateLastPaid()
{
    LOCK(cs);
    if(fLiteMode) return;
    if(!pCurrentBlockIndex) {
        // LogPrintf("CXnodeMan::UpdateLastPaid, pCurrentBlockIndex=NULL\n");
        return;
    }

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a xnode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fXNode) ? mnpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    LogPrint("mnpayments", "CXnodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
                             pCurrentBlockIndex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CXnode& mn, vXnodes) {
        mn.UpdateLastPaid(pCurrentBlockIndex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !xnodeSync.IsWinnersListSynced();
}

void CXnodeMan::CheckAndRebuildXnodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexXnodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexXnodes.GetSize() <= int(vXnodes.size())) {
        return;
    }

    indexXnodesOld = indexXnodes;
    indexXnodes.Clear();
    for(size_t i = 0; i < vXnodes.size(); ++i) {
        indexXnodes.AddXnodeVIN(vXnodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CXnodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CXnodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any xnodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= XNODE_WATCHDOG_MAX_SECONDS;
}

void CXnodeMan::CheckXnode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

void CXnodeMan::CheckXnode(const CPubKey& pubKeyXnode, bool fForce)
{
    LOCK(cs);
    CXnode* pMN = Find(pubKeyXnode);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

int CXnodeMan::GetXnodeState(const CTxIn& vin)
{
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN)  {
        return CXnode::XNODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

int CXnodeMan::GetXnodeState(const CPubKey& pubKeyXnode)
{
    LOCK(cs);
    CXnode* pMN = Find(pubKeyXnode);
    if(!pMN)  {
        return CXnode::XNODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

bool CXnodeMan::IsXnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN) {
        return false;
    }
    return pMN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CXnodeMan::SetXnodeLastPing(const CTxIn& vin, const CXnodePing& mnp)
{
    LOCK(cs);
    CXnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->lastPing = mnp;
    mapSeenXnodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CXnodeBroadcast mnb(*pMN);
    uint256 hash = mnb.GetHash();
    if(mapSeenXnodeBroadcast.count(hash)) {
        mapSeenXnodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CXnodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("xnode", "CXnodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();

    if(fXNode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid();
    }
}

void CXnodeMan::NotifyXnodeUpdates()
{
    // Avoid double locking
    bool fXnodesAddedLocal = false;
    bool fXnodesRemovedLocal = false;
    {
        LOCK(cs);
        fXnodesAddedLocal = fXnodesAdded;
        fXnodesRemovedLocal = fXnodesRemoved;
    }

    if(fXnodesAddedLocal) {
//        governance.CheckXnodeOrphanObjects();
//        governance.CheckXnodeOrphanVotes();
    }
    if(fXnodesRemovedLocal) {
//        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fXnodesAdded = false;
    fXnodesRemoved = false;
}
