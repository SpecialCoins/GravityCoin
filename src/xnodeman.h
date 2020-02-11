// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XNODEMAN_H
#define XNODEMAN_H

#include "xnode.h"
#include "sync.h"

using namespace std;

class CXnodeMan;

extern CXnodeMan mnodeman;

/**
 * Provides a forward and reverse index between MN vin's and integers.
 *
 * This mapping is normally add-only and is expected to be permanent
 * It is only rebuilt if the size of the index exceeds the expected maximum number
 * of MN's and the current number of known MN's.
 *
 * The external interface to this index is provided via delegation by CXnodeMan
 */
class CXnodeIndex
{
public: // Types
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

    typedef std::map<int,CTxIn> rindex_m_t;

    typedef rindex_m_t::iterator rindex_m_it;

    typedef rindex_m_t::const_iterator rindex_m_cit;

private:
    int                  nSize;

    index_m_t            mapIndex;

    rindex_m_t           mapReverseIndex;

public:
    CXnodeIndex();

    int GetSize() const {
        return nSize;
    }

    /// Retrieve xnode vin by index
    bool Get(int nIndex, CTxIn& vinXnode) const;

    /// Get index of a xnode vin
    int GetXnodeIndex(const CTxIn& vinXnode) const;

    void AddXnodeVIN(const CTxIn& vinXnode);

    void Clear();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapIndex);
        if(ser_action.ForRead()) {
            RebuildIndex();
        }
    }

private:
    void RebuildIndex();

};

class CXnodeMan
{
public:
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

private:
    static const int MAX_EXPECTED_INDEX_SIZE = 30000;

    /// Only allow 1 index rebuild per hour
    static const int64_t MIN_INDEX_REBUILD_TIME = 3600;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 3 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    // map to hold all MNs
    std::vector<CXnode> vXnodes;
    // who's asked for the Xnode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForXnodeList;
    // who we asked for the Xnode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForXnodeList;
    // which Xnodes we've asked for
    std::map<COutPoint, std::map<CNetAddr, int64_t> > mWeAskedForXnodeListEntry;
    // who we asked for the xnode verification
    std::map<CNetAddr, CXnodeVerification> mWeAskedForVerification;

    // these maps are used for xnode recovery from XNODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CXnodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;

    int64_t nLastIndexRebuildTime;

    CXnodeIndex indexXnodes;

    CXnodeIndex indexXnodesOld;

    /// Set when index has been rebuilt, clear when read
    bool fIndexRebuilt;

    /// Set when xnodes are added, cleared when CGovernanceManager is notified
    bool fXnodesAdded;

    /// Set when xnodes are removed, cleared when CGovernanceManager is notified
    bool fXnodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastWatchdogVoteTime;

    friend class CXnodeSync;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CXnodeBroadcast> > mapSeenXnodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CXnodePing> mapSeenXnodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CXnodeVerification> mapSeenXnodeVerification;
    // keep track of dsq count to prevent xnodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(vXnodes);
        READWRITE(mAskedUsForXnodeList);
        READWRITE(mWeAskedForXnodeList);
        READWRITE(mWeAskedForXnodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenXnodeBroadcast);
        READWRITE(mapSeenXnodePing);
        READWRITE(indexXnodes);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CXnodeMan();

    /// Add an entry
    bool Add(CXnode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const CTxIn &vin);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    /// Check all Xnodes
    void Check();

    /// Check all Xnodes and remove inactive
    void CheckAndRemove();

    /// Clear Xnode vector
    void Clear();

    /// Count Xnodes filtered by nProtocolVersion.
    /// Xnode nProtocolVersion should match or be above the one specified in param here.
    int CountXnodes(int nProtocolVersion = -1);
    /// Count enabled Xnodes filtered by nProtocolVersion.
    /// Xnode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Xnodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode);

    /// Find an entry
    CXnode* Find(const CScript &payee);
    CXnode* Find(const CTxIn& vin);
    CXnode* Find(const CPubKey& pubKeyXnode);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CPubKey& pubKeyXnode, CXnode& xnode);
    bool Get(const CTxIn& vin, CXnode& xnode);

    /// Retrieve xnode vin by index
    bool Get(int nIndex, CTxIn& vinXnode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexXnodes.Get(nIndex, vinXnode);
    }

    bool GetIndexRebuiltFlag() {
        LOCK(cs);
        return fIndexRebuilt;
    }

    /// Get index of a xnode vin
    int GetXnodeIndex(const CTxIn& vinXnode) {
        LOCK(cs);
        return indexXnodes.GetXnodeIndex(vinXnode);
    }

    /// Get old index of a xnode vin
    int GetXnodeIndexOld(const CTxIn& vinXnode) {
        LOCK(cs);
        return indexXnodesOld.GetXnodeIndex(vinXnode);
    }

    /// Get xnode VIN for an old index value
    bool GetXnodeVinForIndexOld(int nXnodeIndex, CTxIn& vinXnodeOut) {
        LOCK(cs);
        return indexXnodesOld.Get(nXnodeIndex, vinXnodeOut);
    }

    /// Get index of a xnode vin, returning rebuild flag
    int GetXnodeIndex(const CTxIn& vinXnode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexXnodes.GetXnodeIndex(vinXnode);
    }

    void ClearOldXnodeIndex() {
        LOCK(cs);
        indexXnodesOld.Clear();
        fIndexRebuilt = false;
    }

    bool Has(const CTxIn& vin);

    xnode_info_t GetXnodeInfo(const CTxIn& vin);

    xnode_info_t GetXnodeInfo(const CPubKey& pubKeyXnode);

    char* GetNotQualifyReason(CXnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount);

    /// Find an entry in the xnode list that is next to be paid
    CXnode* GetNextXnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount);
    /// Same as above but use current block height
    CXnode* GetNextXnodeInQueueForPayment(bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CXnode* FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion = -1);

    std::vector<CXnode> GetFullXnodeVector() { LOCK(cs); return vXnodes; }

    std::vector<std::pair<int, CXnode> > GetXnodeRanks(int nBlockHeight = -1, int nMinProtocol=0);
    int GetXnodeRank(const CTxIn &vin, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);
    CXnode* GetXnodeByRank(int nRank, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);

    void ProcessXnodeConnections();
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void DoFullVerificationStep();
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<CXnode*>& vSortedByAddr);
    void SendVerifyReply(CNode* pnode, CXnodeVerification& mnv);
    void ProcessVerifyReply(CNode* pnode, CXnodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CXnodeVerification& mnv);

    /// Return the number of (unique) Xnodes
    int size() { return vXnodes.size(); }

    std::string ToString() const;

    /// Update xnode list and maps using provided CXnodeBroadcast
    void UpdateXnodeList(CXnodeBroadcast mnb);
    /// Perform complete check and only then update list and maps
    bool CheckMnbAndUpdateXnodeList(CNode* pfrom, CXnodeBroadcast mnb, int& nDos);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    void UpdateLastPaid();

    void CheckAndRebuildXnodeIndex();

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CTxIn& vin);
    bool AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckXnode(const CTxIn& vin, bool fForce = false);
    void CheckXnode(const CPubKey& pubKeyXnode, bool fForce = false);

    int GetXnodeState(const CTxIn& vin);
    int GetXnodeState(const CPubKey& pubKeyXnode);

    bool IsXnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetXnodeLastPing(const CTxIn& vin, const CXnodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the xnode index has been updated.
     * Must be called while not holding the CXnodeMan::cs mutex
     */
    void NotifyXnodeUpdates();

};

#endif
