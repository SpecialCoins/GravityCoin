// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activexnode.h"
#include "darksend.h"
#include "xnode-payments.h"
#include "xnode-sync.h"
#include "xnodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CXnodePayments mnpayments;

CCriticalSection cs_vecPayees;
CCriticalSection cs_mapXnodeBlocks;
CCriticalSection cs_mapXnodePaymentVotes;

bool IsBlockValueValid(const CBlock &block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet) {

    strErrorRet = "";
    bool isBlockRewardValueMet = (block.vtx[0].GetValueOut() <= blockReward);
        if (!isBlockRewardValueMet)
        {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d)",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
    return isBlockRewardValueMet;
}

bool IsBlockPayeeValid(const CTransaction &txNew, int nBlockHeight, CAmount blockReward) {

    if (!sporkManager.IsSporkActive(SPORK_4_XNODE_PAYMENT_START)) {
        //there is no data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- xnode isn't start\n");
        return true;
    }
    if (!xnodeSync.IsSynced()) {
        //there is no data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, skipping block payee checks\n");
        return true;
    }

    //check for xnode payee
    if (mnpayments.IsTransactionValid(txNew, nBlockHeight)) {
        return true;
    } else {
        if (sporkManager.IsSporkActive(SPORK_5_XNODE_PAYMENT_ENFORCEMENT) && xnodeSync.IsSynced()) {
            return false;
        } else {
            LogPrintf("XNode payment enforcement is disabled, accepting block\n");
            return true;
        }
    }
}

void FillBlockPayments(CMutableTransaction &txNew, int nBlockHeight, CAmount xnodePayment, CTxOut &txoutXnodeRet) {

    // FILL BLOCK PAYEE WITH XNODE PAYMENT OTHERWISE
    mnpayments.FillBlockPayee(txNew, nBlockHeight, xnodePayment, txoutXnodeRet);
    LogPrint("mnpayments", "FillBlockPayments -- nBlockHeight %d xnodePayment %lld txoutXnodeRet %s txNew %s",
             nBlockHeight, xnodePayment, txoutXnodeRet.ToString(), txNew.ToString());
}

std::string GetRequiredPaymentsString(int nBlockHeight) {

    return mnpayments.GetRequiredPaymentsString(nBlockHeight);
}

void CXnodePayments::Clear() {
    LOCK2(cs_mapXnodeBlocks, cs_mapXnodePaymentVotes);
    mapXnodeBlocks.clear();
    mapXnodePaymentVotes.clear();
}

bool CXnodePayments::CanVote(COutPoint outXnode, int nBlockHeight) {
    LOCK(cs_mapXnodePaymentVotes);

    if (mapXnodesLastVote.count(outXnode) && mapXnodesLastVote[outXnode] == nBlockHeight) {
        return false;
    }

    //record this xnode voted
    mapXnodesLastVote[outXnode] = nBlockHeight;
    return true;
}

std::string CXnodePayee::ToString() const {
    CTxDestination address1;
    ExtractDestination(scriptPubKey, address1);
    CBitcoinAddress address2(address1);
    std::string str;
    str += "(address: ";
    str += address2.ToString();
    str += ")\n";
    return str;
}

/**
*   FillBlockPayee
*
*   Fill Xnode ONLY payment block
*/

void CXnodePayments::FillBlockPayee(CMutableTransaction &txNew, int nBlockHeight, CAmount xnodePayment, CTxOut &txoutXnodeRet) {
    // make sure it's not filled yet
    txoutXnodeRet = CTxOut();

    CScript payee;
    bool foundMaxVotedPayee = true;

    if (!mnpayments.GetBlockPayee(nBlockHeight, payee)) {
        // no xnode detected...
        // LogPrintf("no xnode detected...\n");
        foundMaxVotedPayee = false;
        int nCount = 0;
        CXnode *winningNode = mnodeman.GetNextXnodeInQueueForPayment(nBlockHeight, true, nCount);
        if (!winningNode) {
            if(Params().NetworkIDString() != CBaseChainParams::REGTEST) {
                // ...and we can't calculate it on our own
                LogPrintf("CXnodePayments::FillBlockPayee -- Failed to detect xnode to pay\n");
                return;
            }
        }
        // fill payee with locally calculated winner and hope for the best
        if (winningNode) {
            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
            LogPrintf("payee=%s\n", winningNode->ToString());
        }
        else
            payee = txNew.vout[0].scriptPubKey;//This is only for unit tests scenario on REGTEST
    }
    txoutXnodeRet = CTxOut(xnodePayment, payee);
    txNew.vout.push_back(txoutXnodeRet);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);
    if (foundMaxVotedPayee) {
        LogPrintf("CXnodePayments::FillBlockPayee::foundMaxVotedPayee -- Xnode payment %lld to %s\n", xnodePayment, address2.ToString());
    } else {
        LogPrintf("CXnodePayments::FillBlockPayee -- Xnode payment %lld to %s\n", xnodePayment, address2.ToString());
    }

}

int CXnodePayments::GetMinXnodePaymentsProto() {

    int minProtocol = (sporkManager.IsSporkActive(SPORK_1_VERSION_ON)) ? PROTOCOL_VERSION : MIN_PEER_PROTO_VERSION;
    return minProtocol;
}

void CXnodePayments::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {

//    LogPrintf("CXnodePayments::ProcessMessage strCommand=%s\n", strCommand);
    // Ignore any payments messages until xnode list is synced
    if (!xnodeSync.IsXnodeListSynced()) return;

    if (fLiteMode) return; // disable all GravityCoin specific functionality

    bool fTestNet = (Params().NetworkIDString() == CBaseChainParams::TESTNET);

    if (strCommand == NetMsgType::XNODEPAYMENTSYNC) { //Xnode Payments Request Sync

        // Ignore such requests until we are fully synced.
        // We could start processing this after xnode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!xnodeSync.IsSynced()) return;

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::XNODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("XNODEPAYMENTSYNC -- peer already asked me for the list, peer=%d\n", pfrom->id);
            if (!fTestNet) Misbehaving(pfrom->GetId(), 20);
            return;
        }
        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::XNODEPAYMENTSYNC);

        Sync(pfrom);
        LogPrint("mnpayments", "XNODEPAYMENTSYNC -- Sent Xnode payment votes to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::XNODEPAYMENTVOTE) { // Xnode Payments Vote for the Winner

        CXnodePaymentVote vote;
        vRecv >> vote;

        if (pfrom->nVersion < GetMinXnodePaymentsProto()) return;

        if (!pCurrentBlockIndex) return;

        uint256 nHash = vote.GetHash();

        pfrom->setAskFor.erase(nHash);

        {
            LOCK(cs_mapXnodePaymentVotes);
            if (mapXnodePaymentVotes.count(nHash)) {
                LogPrint("mnpayments", "XNODEPAYMENTVOTE -- hash=%s, nHeight=%d seen\n", nHash.ToString(), pCurrentBlockIndex->nHeight);
                return;
            }

            // Avoid processing same vote multiple times
            mapXnodePaymentVotes[nHash] = vote;
            // but first mark vote as non-verified,
            // AddPaymentVote() below should take care of it if vote is actually ok
            mapXnodePaymentVotes[nHash].MarkAsNotVerified();
        }

        int nFirstBlock = pCurrentBlockIndex->nHeight - GetStorageLimit();
        if (vote.nBlockHeight < nFirstBlock || vote.nBlockHeight > pCurrentBlockIndex->nHeight + 20) {
            LogPrint("mnpayments", "XNODEPAYMENTVOTE -- vote out of range: nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n", nFirstBlock, vote.nBlockHeight, pCurrentBlockIndex->nHeight);
            return;
        }

        std::string strError = "";
        if (!vote.IsValid(pfrom, pCurrentBlockIndex->nHeight, strError)) {
            LogPrint("mnpayments", "XNODEPAYMENTVOTE -- invalid message, error: %s\n", strError);
            return;
        }

        if (!CanVote(vote.vinXnode.prevout, vote.nBlockHeight)) {
            LogPrintf("XNODEPAYMENTVOTE -- xnode already voted, xnode=%s\n", vote.vinXnode.prevout.ToStringShort());
            return;
        }

        xnode_info_t mnInfo = mnodeman.GetXnodeInfo(vote.vinXnode);
        if (!mnInfo.fInfoValid) {
            // mn was not found, so we can't check vote, some info is probably missing
            LogPrintf("XNODEPAYMENTVOTE -- xnode is missing %s\n", vote.vinXnode.prevout.ToStringShort());
            mnodeman.AskForMN(pfrom, vote.vinXnode);
            return;
        }

        int nDos = 0;
        if (!vote.CheckSignature(mnInfo.pubKeyXnode, pCurrentBlockIndex->nHeight, nDos)) {
            if (nDos) {
                LogPrintf("XNODEPAYMENTVOTE -- ERROR: invalid signature\n");
                if (!fTestNet) Misbehaving(pfrom->GetId(), nDos);
            } else {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                LogPrint("mnpayments", "XNODEPAYMENTVOTE -- WARNING: invalid signature\n");
            }
            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            mnodeman.AskForMN(pfrom, vote.vinXnode);
            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a mn which changed its key),
            // so just quit here.
            return;
        }

        CTxDestination address1;
        ExtractDestination(vote.payee, address1);
        CBitcoinAddress address2(address1);

        LogPrint("mnpayments", "XNODEPAYMENTVOTE -- vote: address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s\n", address2.ToString(), vote.nBlockHeight, pCurrentBlockIndex->nHeight, vote.vinXnode.prevout.ToStringShort());

        if (AddPaymentVote(vote)) {
            vote.Relay();
            xnodeSync.AddedPaymentVote();
        }
    }
}

bool CXnodePaymentVote::Sign() {
    std::string strError;
    std::string strMessage = vinXnode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, activeXnode.keyXnode)) {
        LogPrintf("CXnodePaymentVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(activeXnode.pubKeyXnode, vchSig, strMessage, strError)) {
        LogPrintf("CXnodePaymentVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CXnodePayments::GetBlockPayee(int nBlockHeight, CScript &payee) {
    if (mapXnodeBlocks.count(nBlockHeight)) {
        return mapXnodeBlocks[nBlockHeight].GetBestPayee(payee);
    }

    return false;
}

// Is this xnode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 blocks of votes
bool CXnodePayments::IsScheduled(CXnode &mn, int nNotBlockHeight) {
    LOCK(cs_mapXnodeBlocks);

    if (!pCurrentBlockIndex) return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = pCurrentBlockIndex->nHeight; h <= pCurrentBlockIndex->nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapXnodeBlocks.count(h) && mapXnodeBlocks[h].GetBestPayee(payee) && mnpayee == payee) {
            return true;
        }
    }

    return false;
}

bool CXnodePayments::AddPaymentVote(const CXnodePaymentVote &vote) {
    LogPrint("xnode-payments", "CXnodePayments::AddPaymentVote\n");
    uint256 blockHash = uint256();
    if (!GetBlockHash(blockHash, vote.nBlockHeight - 101)) return false;

    if (HasVerifiedPaymentVote(vote.GetHash())) return false;

    LOCK2(cs_mapXnodeBlocks, cs_mapXnodePaymentVotes);

    mapXnodePaymentVotes[vote.GetHash()] = vote;

    if (!mapXnodeBlocks.count(vote.nBlockHeight)) {
        CXnodeBlockPayees blockPayees(vote.nBlockHeight);
        mapXnodeBlocks[vote.nBlockHeight] = blockPayees;
    }

    mapXnodeBlocks[vote.nBlockHeight].AddPayee(vote);

    return true;
}

bool CXnodePayments::HasVerifiedPaymentVote(uint256 hashIn) {
    LOCK(cs_mapXnodePaymentVotes);
    std::map<uint256, CXnodePaymentVote>::iterator it = mapXnodePaymentVotes.find(hashIn);
    return it != mapXnodePaymentVotes.end() && it->second.IsVerified();
}

void CXnodeBlockPayees::AddPayee(const CXnodePaymentVote &vote) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CXnodePayee & payee, vecPayees)
    {
        if (payee.GetPayee() == vote.payee) {
            payee.AddVoteHash(vote.GetHash());
            return;
        }
    }
    CXnodePayee payeeNew(vote.payee, vote.GetHash());
    vecPayees.push_back(payeeNew);
}

bool CXnodeBlockPayees::GetBestPayee(CScript &payeeRet) {
    LOCK(cs_vecPayees);
    LogPrint("mnpayments", "CXnodeBlockPayees::GetBestPayee, vecPayees.size()=%s\n", vecPayees.size());
    if (!vecPayees.size()) {
        LogPrint("mnpayments", "CXnodeBlockPayees::GetBestPayee -- ERROR: couldn't find any payee\n");
        return false;
    }

    int nVotes = -1;
    BOOST_FOREACH(CXnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() > nVotes) {
            payeeRet = payee.GetPayee();
            nVotes = payee.GetVoteCount();
        }
    }

    return (nVotes > -1);
}

bool CXnodeBlockPayees::HasPayeeWithVotes(CScript payeeIn, int nVotesReq) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CXnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nVotesReq && payee.GetPayee() == payeeIn) {
            return true;
        }
    }

//    LogPrint("mnpayments", "CXnodeBlockPayees::HasPayeeWithVotes -- ERROR: couldn't find any payee with %d+ votes\n", nVotesReq);
    return false;
}

bool CXnodeBlockPayees::IsTransactionValid(const CTransaction &txNew) {
    LOCK(cs_vecPayees);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";


    CAmount nXnodePayment = GetXnodePayment(nBlockHeight);

    //require at least MNPAYMENTS_SIGNATURES_REQUIRED signatures

    BOOST_FOREACH(CXnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nMaxSignatures) {
            nMaxSignatures = payee.GetVoteCount();
        }
    }

    // if we don't have at least MNPAYMENTS_SIGNATURES_REQUIRED signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED) return true;

    bool hasValidPayee = false;

    BOOST_FOREACH(CXnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
            hasValidPayee = true;

            BOOST_FOREACH(CTxOut txout, txNew.vout) {
                if (payee.GetPayee() == txout.scriptPubKey && nXnodePayment == txout.nValue) {
                    LogPrint("mnpayments", "CXnodeBlockPayees::IsTransactionValid -- Found required payment\n");
                    return true;
                }
            }

            CTxDestination address1;
            ExtractDestination(payee.GetPayee(), address1);
            CBitcoinAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible = address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    if (!hasValidPayee) return true;

    LogPrintf("CXnodeBlockPayees::IsTransactionValid -- ERROR: Missing required payment, possible payees: '%s', amount: %f GXX\n", strPayeesPossible, (float) nXnodePayment / COIN);
    return false;
}

std::string CXnodeBlockPayees::GetRequiredPaymentsString() {
    LOCK(cs_vecPayees);

    std::string strRequiredPayments = "Unknown";

    BOOST_FOREACH(CXnodePayee & payee, vecPayees)
    {
        CTxDestination address1;
        ExtractDestination(payee.GetPayee(), address1);
        CBitcoinAddress address2(address1);

        if (strRequiredPayments != "Unknown") {
            strRequiredPayments += ", " + address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        } else {
            strRequiredPayments = address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        }
    }

    return strRequiredPayments;
}

std::string CXnodePayments::GetRequiredPaymentsString(int nBlockHeight) {
    LOCK(cs_mapXnodeBlocks);

    if (mapXnodeBlocks.count(nBlockHeight)) {
        return mapXnodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CXnodePayments::IsTransactionValid(const CTransaction &txNew, int nBlockHeight) {
    LOCK(cs_mapXnodeBlocks);

    if (mapXnodeBlocks.count(nBlockHeight)) {
        return mapXnodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CXnodePayments::CheckAndRemove() {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_mapXnodeBlocks, cs_mapXnodePaymentVotes);

    int nLimit = GetStorageLimit();

    std::map<uint256, CXnodePaymentVote>::iterator it = mapXnodePaymentVotes.begin();
    while (it != mapXnodePaymentVotes.end()) {
        CXnodePaymentVote vote = (*it).second;

        if (pCurrentBlockIndex->nHeight - vote.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CXnodePayments::CheckAndRemove -- Removing old Xnode payment: nBlockHeight=%d\n", vote.nBlockHeight);
            mapXnodePaymentVotes.erase(it++);
            mapXnodeBlocks.erase(vote.nBlockHeight);
        } else {
            ++it;
        }
    }
    LogPrintf("CXnodePayments::CheckAndRemove -- %s\n", ToString());
}

bool CXnodePaymentVote::IsValid(CNode *pnode, int nValidationHeight, std::string &strError) {

    CXnode *pmn = mnodeman.Find(vinXnode);

    if (!pmn) {
        strError = strprintf("Unknown Xnode: prevout=%s", vinXnode.prevout.ToStringShort());
        // Only ask if we are already synced and still have no idea about that Xnode
        if (xnodeSync.IsXnodeListSynced()) {
            mnodeman.AskForMN(pnode, vinXnode);
        }

        return false;
    }

    int nMinRequiredProtocol;
    nMinRequiredProtocol = mnpayments.GetMinXnodePaymentsProto();

    if (pmn->nProtocolVersion < nMinRequiredProtocol) {
        strError = strprintf("Xnode protocol is too old: nProtocolVersion=%d, nMinRequiredProtocol=%d", pmn->nProtocolVersion, nMinRequiredProtocol);
        return false;
    }

    // Only xnodes should try to check xnode rank for old votes - they need to pick the right winner for future blocks.
    // Regular clients (miners included) need to verify xnode rank for future block votes only.
    if (!fXNode && nBlockHeight < nValidationHeight) return true;

    int nRank = mnodeman.GetXnodeRank(vinXnode, nBlockHeight - 101, nMinRequiredProtocol, false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CXnodePaymentVote::IsValid -- Can't calculate rank for xnode %s\n",
                 vinXnode.prevout.ToStringShort());
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have xnodes mistakenly think they are in the top 10
        // We don't want to print all of these messages in normal mode, debug mode should print though
        strError = strprintf("Xnode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        // Only ban for new mnw which is out of bounds, for old mnw MN list itself might be way too much off
        if (nRank > MNPAYMENTS_SIGNATURES_TOTAL * 2 && nBlockHeight > nValidationHeight) {
            strError = strprintf("Xnode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, nRank);
            LogPrintf("CXnodePaymentVote::IsValid -- Error: %s\n", strError);
            Misbehaving(pnode->GetId(), 20);
        }
        // Still invalid however
        return false;
    }

    return true;
}

bool CXnodePayments::ProcessBlock(int nBlockHeight) {

    // DETERMINE IF WE SHOULD BE VOTING FOR THE NEXT PAYEE

    if (fLiteMode || !fXNode) {
        return false;
    }

    // We have little chances to pick the right winner if winners list is out of sync
    // but we have no choice, so we'll try. However it doesn't make sense to even try to do so
    // if we have not enough data about xnodes.
    if (!xnodeSync.IsXnodeListSynced()) {
        return false;
    }

    int nRank = mnodeman.GetXnodeRank(activeXnode.vin, nBlockHeight - 101, GetMinXnodePaymentsProto(), false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CXnodePayments::ProcessBlock -- Unknown Xnode\n");
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CXnodePayments::ProcessBlock -- Xnode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        return false;
    }

    // LOCATE THE NEXT XNODE WHICH SHOULD BE PAID

    LogPrintf("CXnodePayments::ProcessBlock -- Start: nBlockHeight=%d, xnode=%s\n", nBlockHeight, activeXnode.vin.prevout.ToStringShort());

    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
    int nCount = 0;
    CXnode *pmn = mnodeman.GetNextXnodeInQueueForPayment(nBlockHeight, true, nCount);

    if (pmn == NULL) {
        LogPrintf("CXnodePayments::ProcessBlock -- ERROR: Failed to find xnode to pay\n");
        return false;
    }

    LogPrintf("CXnodePayments::ProcessBlock -- Xnode found by GetNextXnodeInQueueForPayment(): %s\n", pmn->vin.prevout.ToStringShort());


    CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());

    CXnodePaymentVote voteNew(activeXnode.vin, nBlockHeight, payee);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);

    // SIGN MESSAGE TO NETWORK WITH OUR XNODE KEYS

    if (voteNew.Sign()) {
        if (AddPaymentVote(voteNew)) {
            voteNew.Relay();
            return true;
        }
    }

    return false;
}

void CXnodePaymentVote::Relay() {
    // do not relay until synced
    if (!xnodeSync.IsWinnersListSynced()) {
        LogPrint("xnode", "CXnodePaymentVote::Relay - xnodeSync.IsWinnersListSynced() not sync\n");
        return;
    }
    CInv inv(MSG_XNODE_PAYMENT_VOTE, GetHash());
    RelayInv(inv);
}

bool CXnodePaymentVote::CheckSignature(const CPubKey &pubKeyXnode, int nValidationHeight, int &nDos) {
    // do not ban by default
    nDos = 0;

    std::string strMessage = vinXnode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    std::string strError = "";
    if (!darkSendSigner.VerifyMessage(pubKeyXnode, vchSig, strMessage, strError)) {
        // Only ban for future block vote when we are already synced.
        // Otherwise it could be the case when MN which signed this vote is using another key now
        // and we have no idea about the old one.
        if (xnodeSync.IsXnodeListSynced() && nBlockHeight > nValidationHeight) {
            nDos = 20;
        }
        return error("CXnodePaymentVote::CheckSignature -- Got bad Xnode payment signature, xnode=%s, error: %s", vinXnode.prevout.ToStringShort().c_str(), strError);
    }

    return true;
}

std::string CXnodePaymentVote::ToString() const {
    std::ostringstream info;

    info << vinXnode.prevout.ToStringShort() <<
         ", " << nBlockHeight <<
         ", " << ScriptToAsmStr(payee) <<
         ", " << (int) vchSig.size();

    return info.str();
}

// Send only votes for future blocks, node should request every other missing payment block individually
void CXnodePayments::Sync(CNode *pnode) {
    LOCK(cs_mapXnodeBlocks);

    if (!pCurrentBlockIndex) return;

    int nInvCount = 0;

    for (int h = pCurrentBlockIndex->nHeight; h < pCurrentBlockIndex->nHeight + 20; h++) {
        if (mapXnodeBlocks.count(h)) {
            BOOST_FOREACH(CXnodePayee & payee, mapXnodeBlocks[h].vecPayees)
            {
                std::vector <uint256> vecVoteHashes = payee.GetVoteHashes();
                BOOST_FOREACH(uint256 & hash, vecVoteHashes)
                {
                    if (!HasVerifiedPaymentVote(hash)) continue;
                    pnode->PushInventory(CInv(MSG_XNODE_PAYMENT_VOTE, hash));
                    nInvCount++;
                }
            }
        }
    }

    LogPrintf("CXnodePayments::Sync -- Sent %d votes to peer %d\n", nInvCount, pnode->id);
    pnode->PushMessage(NetMsgType::SYNCSTATUSCOUNT, XNODE_SYNC_MNW, nInvCount);
}

// Request low data/unknown payment blocks in batches directly from some node instead of/after preliminary Sync.
void CXnodePayments::RequestLowDataPaymentBlocks(CNode *pnode) {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_main, cs_mapXnodeBlocks);

    std::vector <CInv> vToFetch;
    int nLimit = GetStorageLimit();

    const CBlockIndex *pindex = pCurrentBlockIndex;

    while (pCurrentBlockIndex->nHeight - pindex->nHeight < nLimit) {
        if (!mapXnodeBlocks.count(pindex->nHeight)) {
            // We have no idea about this block height, let's ask
            vToFetch.push_back(CInv(MSG_XNODE_PAYMENT_BLOCK, pindex->GetBlockHash()));
            // We should not violate GETDATA rules
            if (vToFetch.size() == MAX_INV_SZ) {
                LogPrintf("CXnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, MAX_INV_SZ);
                pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
                // Start filling new batch
                vToFetch.clear();
            }
        }
        if (!pindex->pprev) break;
        pindex = pindex->pprev;
    }

    std::map<int, CXnodeBlockPayees>::iterator it = mapXnodeBlocks.begin();

    while (it != mapXnodeBlocks.end()) {
        int nTotalVotes = 0;
        bool fFound = false;
        BOOST_FOREACH(CXnodePayee & payee, it->second.vecPayees)
        {
            if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
                fFound = true;
                break;
            }
            nTotalVotes += payee.GetVoteCount();
        }
        // A clear winner (MNPAYMENTS_SIGNATURES_REQUIRED+ votes) was found
        // or no clear winner was found but there are at least avg number of votes
        if (fFound || nTotalVotes >= (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2) {
            // so just move to the next block
            ++it;
            continue;
        }
        // DEBUG
//        DBG (
//            // Let's see why this failed
//            BOOST_FOREACH(CXnodePayee& payee, it->second.vecPayees) {
//                CTxDestination address1;
//                ExtractDestination(payee.GetPayee(), address1);
//                CBitcoinAddress address2(address1);
//                printf("payee %s votes %d\n", address2.ToString().c_str(), payee.GetVoteCount());
//            }
//            printf("block %d votes total %d\n", it->first, nTotalVotes);
//        )
        // END DEBUG
        // Low data block found, let's try to sync it
        uint256 hash;
        if (GetBlockHash(hash, it->first)) {
            vToFetch.push_back(CInv(MSG_XNODE_PAYMENT_BLOCK, hash));
        }
        // We should not violate GETDATA rules
        if (vToFetch.size() == MAX_INV_SZ) {
            LogPrintf("CXnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, MAX_INV_SZ);
            pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
            // Start filling new batch
            vToFetch.clear();
        }
        ++it;
    }
    // Ask for the rest of it
    if (!vToFetch.empty()) {
        LogPrintf("CXnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, vToFetch.size());
        pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
    }
}

std::string CXnodePayments::ToString() const {
    std::ostringstream info;

    info << "Votes: " << (int) mapXnodePaymentVotes.size() <<
         ", Blocks: " << (int) mapXnodeBlocks.size();

    return info.str();
}

bool CXnodePayments::IsEnoughData() {
    float nAverageVotes = (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2;
    int nStorageLimit = GetStorageLimit();
    return GetBlockCount() > nStorageLimit && GetVoteCount() > nStorageLimit * nAverageVotes;
}

int CXnodePayments::GetStorageLimit() {
    return std::max(int(mnodeman.size() * nStorageCoeff), nMinBlocksToStore);
}

void CXnodePayments::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
    LogPrint("mnpayments", "CXnodePayments::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);
    
    ProcessBlock(pindex->nHeight + 5);
}
