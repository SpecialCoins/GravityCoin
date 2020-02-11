// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activexnode.h"
#include "checkpoints.h"
#include "main.h"
#include "xnode.h"
#include "xnode-payments.h"
#include "xnode-sync.h"
#include "xnodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

class CXnodeSync;

CXnodeSync xnodeSync;

bool CXnodeSync::CheckNodeHeight(CNode *pnode, bool fDisconnectStuckNodes) {
    CNodeStateStats stats;
    if (!GetNodeStateStats(pnode->id, stats) || stats.nCommonHeight == -1 || stats.nSyncHeight == -1) return false; // not enough info about this peer

    // Check blocks and headers, allow a small error margin of 1 block
    if (pCurrentBlockIndex->nHeight - 1 > stats.nCommonHeight) {
        // This peer probably stuck, don't sync any additional data from it
        if (fDisconnectStuckNodes) {
            // Disconnect to free this connection slot for another peer.
            pnode->fDisconnect = true;
            LogPrintf("CXnodeSync::CheckNodeHeight -- disconnecting from stuck peer, nHeight=%d, nCommonHeight=%d, peer=%d\n",
                      pCurrentBlockIndex->nHeight, stats.nCommonHeight, pnode->id);
        } else {
            LogPrintf("CXnodeSync::CheckNodeHeight -- skipping stuck peer, nHeight=%d, nCommonHeight=%d, peer=%d\n",
                      pCurrentBlockIndex->nHeight, stats.nCommonHeight, pnode->id);
        }
        return false;
    } else if (pCurrentBlockIndex->nHeight < stats.nSyncHeight - 1) {
        // This peer announced more headers than we have blocks currently
        LogPrint("xnode", "CXnodeSync::CheckNodeHeight -- skipping peer, who announced more headers than we have blocks currently, nHeight=%d, nSyncHeight=%d, peer=%d\n",
                  pCurrentBlockIndex->nHeight, stats.nSyncHeight, pnode->id);
        return false;
    }

    return true;
}

bool CXnodeSync::IsBlockchainSynced(bool fBlockAccepted) {
    static bool fBlockchainSynced = false;
    static int64_t nTimeLastProcess = GetTime();
    static int nSkipped = 0;
    static bool fFirstBlockAccepted = false;

    // If the last call to this function was more than 60 minutes ago 
    // (client was in sleep mode) reset the sync process
    if (GetTime() - nTimeLastProcess > 60 * 60) {
        LogPrintf("CXnodeSync::IsBlockchainSynced time-check fBlockchainSynced=%s\n", 
                  fBlockchainSynced);
        Reset();
        fBlockchainSynced = false;
    }

    if (!pCurrentBlockIndex || !pindexBestHeader || fImporting || fReindex) 
        return false;

    if (fBlockAccepted) {
        // This should be only triggered while we are still syncing.
        if (!IsSynced()) {
            // We are trying to download smth, reset blockchain sync status.
            fFirstBlockAccepted = true;
            fBlockchainSynced = false;
            nTimeLastProcess = GetTime();
            return false;
        }
    } else {
        // Dont skip on REGTEST to make the tests run faster.
        if(Params().NetworkIDString() != CBaseChainParams::REGTEST) {
            // skip if we already checked less than 1 tick ago.
            if (GetTime() - nTimeLastProcess < XNODE_SYNC_TICK_SECONDS) {
                nSkipped++;
                return fBlockchainSynced;
            }
        }
    }

    LogPrint("xnode-sync", 
             "CXnodeSync::IsBlockchainSynced -- state before check: %ssynced, skipped %d times\n", 
             fBlockchainSynced ? "" : "not ", 
             nSkipped);

    nTimeLastProcess = GetTime();
    nSkipped = 0;

    if (fBlockchainSynced){
        return true;
    }

    if (fCheckpointsEnabled && 
        pCurrentBlockIndex->nHeight < Checkpoints::GetTotalBlocksEstimate(Params().Checkpoints())) {
        
        return false;
    }

    std::vector < CNode * > vNodesCopy = CopyNodeVector();
    // We have enough peers and assume most of them are synced
    if (vNodesCopy.size() >= XNODE_SYNC_ENOUGH_PEERS) {
        // Check to see how many of our peers are (almost) at the same height as we are
        int nNodesAtSameHeight = 0;
        BOOST_FOREACH(CNode * pnode, vNodesCopy)
        {
            // Make sure this peer is presumably at the same height
            if (!CheckNodeHeight(pnode)) {
                continue;
            }
            nNodesAtSameHeight++;
            // if we have decent number of such peers, most likely we are synced now
            if (nNodesAtSameHeight >= XNODE_SYNC_ENOUGH_PEERS) {
                LogPrintf("CXnodeSync::IsBlockchainSynced -- found enough peers on the same height as we are, done\n");
                fBlockchainSynced = true;
                ReleaseNodeVector(vNodesCopy);
                return true;
            }
        }
    }
    ReleaseNodeVector(vNodesCopy);

    // wait for at least one new block to be accepted
    if (!fFirstBlockAccepted) return false;

    // same as !IsInitialBlockDownload() but no cs_main needed here
    int64_t nMaxBlockTime = std::max(pCurrentBlockIndex->GetBlockTime(), pindexBestHeader->GetBlockTime());
    fBlockchainSynced = pindexBestHeader->nHeight - pCurrentBlockIndex->nHeight < 24 * 6 &&
                        GetTime() - nMaxBlockTime < Params().MaxTipAge();
    return fBlockchainSynced;
}

void CXnodeSync::Fail() {
    nTimeLastFailure = GetTime();
    nRequestedXnodeAssets = XNODE_SYNC_FAILED;
}

void CXnodeSync::Reset() {
    nRequestedXnodeAssets = XNODE_SYNC_INITIAL;
    nRequestedXnodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastXnodeList = GetTime();
    nTimeLastPaymentVote = GetTime();
    nTimeLastGovernanceItem = GetTime();
    nTimeLastFailure = 0;
    nCountFailures = 0;
}

std::string CXnodeSync::GetAssetName() {
    switch (nRequestedXnodeAssets) {
        case (XNODE_SYNC_INITIAL):
            return "XNODE_SYNC_INITIAL";
        case (XNODE_SYNC_SPORKS):
            return "XNODE_SYNC_SPORKS";
        case (XNODE_SYNC_LIST):
            return "XNODE_SYNC_LIST";
        case (XNODE_SYNC_MNW):
            return "XNODE_SYNC_MNW";
        case (XNODE_SYNC_FAILED):
            return "XNODE_SYNC_FAILED";
        case XNODE_SYNC_FINISHED:
            return "XNODE_SYNC_FINISHED";
        default:
            return "UNKNOWN";
    }
}

void CXnodeSync::SwitchToNextAsset() {
    switch (nRequestedXnodeAssets) {
        case (XNODE_SYNC_FAILED):
            throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
            break;
        case (XNODE_SYNC_INITIAL):
            ClearFulfilledRequests();
            nRequestedXnodeAssets = XNODE_SYNC_SPORKS;
            LogPrintf("CXnodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case (XNODE_SYNC_SPORKS):
            nTimeLastXnodeList = GetTime();
            nRequestedXnodeAssets = XNODE_SYNC_LIST;
            LogPrintf("CXnodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case (XNODE_SYNC_LIST):
            nTimeLastPaymentVote = GetTime();
            nRequestedXnodeAssets = XNODE_SYNC_MNW;
            LogPrintf("CXnodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;

        case (XNODE_SYNC_MNW):
            nTimeLastGovernanceItem = GetTime();
            LogPrintf("CXnodeSync::SwitchToNextAsset -- Sync has finished\n");
            nRequestedXnodeAssets = XNODE_SYNC_FINISHED;
            break;
    }
    nRequestedXnodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
}

std::string CXnodeSync::GetSyncStatus() {
    switch (xnodeSync.nRequestedXnodeAssets) {
        case XNODE_SYNC_INITIAL:
            return _("Synchronization pending...");
        case XNODE_SYNC_SPORKS:
            return _("Synchronizing sporks...");
        case XNODE_SYNC_LIST:
            return _("Synchronizing xnodes...");
        case XNODE_SYNC_MNW:
            return _("Synchronizing xnode payments...");
        case XNODE_SYNC_FAILED:
            return _("Synchronization failed");
        case XNODE_SYNC_FINISHED:
            return _("Synchronization finished");
        default:
            return "";
    }
}

void CXnodeSync::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if (IsSynced() || IsFailed()) return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrintf("SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->id);
    }
}

void CXnodeSync::ClearFulfilledRequests() {
    TRY_LOCK(cs_vNodes, lockRecv);
    if (!lockRecv) return;

    BOOST_FOREACH(CNode * pnode, vNodes)
    {
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "spork-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "xnode-list-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "xnode-payment-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "full-sync");
    }
}

void CXnodeSync::ProcessTick() {
    static int nTick = 0;
    if (nTick++ % XNODE_SYNC_TICK_SECONDS != 0) return;
    if (!pCurrentBlockIndex) return;

    //the actual count of xnodes we have currently
    int nMnCount = mnodeman.CountXnodes();

    LogPrint("ProcessTick", "CXnodeSync::ProcessTick -- nTick %d nMnCount %d\n", nTick, nMnCount);

    // INITIAL SYNC SETUP / LOG REPORTING
    double nSyncProgress = double(nRequestedXnodeAttempt + (nRequestedXnodeAssets - 1) * 8) / (8 * 4);
    LogPrint("ProcessTick", "CXnodeSync::ProcessTick -- nTick %d nRequestedXnodeAssets %d nRequestedXnodeAttempt %d nSyncProgress %f\n", nTick, nRequestedXnodeAssets, nRequestedXnodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(pCurrentBlockIndex->nHeight, nSyncProgress);

    // RESET SYNCING INCASE OF FAILURE
    {
        if (IsSynced()) {
            /*
                Resync if we lost all xnodes from sleep/wake or failed to sync originally
            */
            if (nMnCount == 0) {
                LogPrintf("CXnodeSync::ProcessTick -- WARNING: not enough data, restarting sync\n");
                Reset();
            } else {
                std::vector < CNode * > vNodesCopy = CopyNodeVector();
                ReleaseNodeVector(vNodesCopy);
                return;
            }
        }

        //try syncing again
        if (IsFailed()) {
            if (nTimeLastFailure + (1 * 60) < GetTime()) { // 1 minute cooldown after failed sync
                Reset();
            }
            return;
        }
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !IsBlockchainSynced() && nRequestedXnodeAssets > XNODE_SYNC_SPORKS) {
        nTimeLastXnodeList = GetTime();
        nTimeLastPaymentVote = GetTime();
        nTimeLastGovernanceItem = GetTime();
        return;
    }
    if (nRequestedXnodeAssets == XNODE_SYNC_INITIAL || (nRequestedXnodeAssets == XNODE_SYNC_SPORKS && IsBlockchainSynced())) {
        SwitchToNextAsset();
    }

    std::vector < CNode * > vNodesCopy = CopyNodeVector();

    BOOST_FOREACH(CNode * pnode, vNodesCopy)
    {
        // Don't try to sync any data from outbound "xnode" connections -
        // they are temporary and should be considered unreliable for a sync process.
        // Inbound connection this early is most likely a "xnode" connection
        // initialted from another node, so skip it too.
        if (pnode->fXnode || (fXNode && pnode->fInbound)) continue;

        // QUICK MODE (REGTEST ONLY!)
        if (Params().NetworkIDString() == CBaseChainParams::REGTEST) {
            if (nRequestedXnodeAttempt <= 2) {
                pnode->PushMessage(NetMsgType::GETSPORKS); //get current network sporks
            } else if (nRequestedXnodeAttempt < 4) {
                mnodeman.DsegUpdate(pnode);
            } else if (nRequestedXnodeAttempt < 6) {
                int nMnCount = mnodeman.CountXnodes();
                pnode->PushMessage(NetMsgType::XNODEPAYMENTSYNC, nMnCount); //sync payment votes
            } else {
                nRequestedXnodeAssets = XNODE_SYNC_FINISHED;
            }
            nRequestedXnodeAttempt++;
            ReleaseNodeVector(vNodesCopy);
            return;
        }

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if (netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrintf("CXnodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->id);
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC (we skip this mode now)

            if (!netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // only request once from each peer
                netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                pnode->PushMessage(NetMsgType::GETSPORKS);
                LogPrintf("CXnodeSync::ProcessTick -- nTick %d nRequestedXnodeAssets %d -- requesting sporks from peer %d\n", nTick, nRequestedXnodeAssets, pnode->id);
                continue; // always get sporks first, switch to the next node without waiting for the next tick
            }

            // MNLIST : SYNC XNODE LIST FROM OTHER CONNECTED CLIENTS

            if (nRequestedXnodeAssets == XNODE_SYNC_LIST) {
                // check for timeout first
                if (nTimeLastXnodeList < GetTime() - XNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CXnodeSync::ProcessTick -- nTick %d nRequestedXnodeAssets %d -- timeout\n", nTick, nRequestedXnodeAssets);
                    if (nRequestedXnodeAttempt == 0) {
                        LogPrintf("CXnodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without xnode list, fail here and try later
                        Fail();
                        ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "xnode-list-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "xnode-list-sync");

                if (pnode->nVersion < mnpayments.GetMinXnodePaymentsProto()) continue;
                nRequestedXnodeAttempt++;

                mnodeman.DsegUpdate(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // MNW : SYNC XNODE PAYMENT VOTES FROM OTHER CONNECTED CLIENTS

            if (nRequestedXnodeAssets == XNODE_SYNC_MNW) {
                LogPrint("mnpayments", "CXnodeSync::ProcessTick -- nTick %d nRequestedXnodeAssets %d nTimeLastPaymentVote %lld GetTime() %lld diff %lld\n", nTick, nRequestedXnodeAssets, nTimeLastPaymentVote, GetTime(), GetTime() - nTimeLastPaymentVote);
                // check for timeout first
                // This might take a lot longer than XNODE_SYNC_TIMEOUT_SECONDS minutes due to new blocks,
                // but that should be OK and it should timeout eventually.
                if (nTimeLastPaymentVote < GetTime() - XNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CXnodeSync::ProcessTick -- nTick %d nRequestedXnodeAssets %d -- timeout\n", nTick, nRequestedXnodeAssets);
                    if (nRequestedXnodeAttempt == 0) {
                        LogPrintf("CXnodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // probably not a good idea to proceed without winner list
                        Fail();
                        ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // check for data
                // if mnpayments already has enough blocks and votes, switch to the next asset
                // try to fetch data from at least two peers though
                if (nRequestedXnodeAttempt > 1 && mnpayments.IsEnoughData()) {
                    LogPrintf("CXnodeSync::ProcessTick -- nTick %d nRequestedXnodeAssets %d -- found enough data\n", nTick, nRequestedXnodeAssets);
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "xnode-payment-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "xnode-payment-sync");

                if (pnode->nVersion < mnpayments.GetMinXnodePaymentsProto()) continue;
                nRequestedXnodeAttempt++;

                // ask node for all payment votes it has (new nodes will only return votes for future payments)
                pnode->PushMessage(NetMsgType::XNODEPAYMENTSYNC, mnpayments.GetStorageLimit());
                // ask node for missing pieces only (old nodes will not be asked)
                mnpayments.RequestLowDataPaymentBlocks(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

        }
    }
    // looped through all nodes, release them
    ReleaseNodeVector(vNodesCopy);
}

void CXnodeSync::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
}
