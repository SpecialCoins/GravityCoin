// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activexnode.h"
#include "consensus/consensus.h"
#include "xnode.h"
#include "xnode-sync.h"
#include "xnode-payments.h"
#include "xnodeman.h"
#include "protocol.h"

extern CWallet *pwalletMain;

// Keep track of the active Xnode
CActiveXnode activeXnode;

void CActiveXnode::ManageState() {
    LogPrint("xnode", "CActiveXnode::ManageState -- Start\n");
    if (!fXNode) {
        LogPrint("xnode", "CActiveXnode::ManageState -- Not a xnode, returning\n");
        return;
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !xnodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_XNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveXnode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if (nState == ACTIVE_XNODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_XNODE_INITIAL;
    }

    LogPrint("xnode", "CActiveXnode::ManageState -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == XNODE_UNKNOWN) {
        ManageStateInitial();
    }

    if (eType == XNODE_REMOTE) {
        ManageStateRemote();
    } else if (eType == XNODE_LOCAL) {
        // Try Remote Start first so the started local xnode can be restarted without recreate xnode broadcast.
        ManageStateRemote();
        if (nState != ACTIVE_XNODE_STARTED)
            ManageStateLocal();
    }

    SendXnodePing();
}

std::string CActiveXnode::GetStateString() const {
    switch (nState) {
        case ACTIVE_XNODE_INITIAL:
            return "INITIAL";
        case ACTIVE_XNODE_SYNC_IN_PROCESS:
            return "SYNC_IN_PROCESS";
        case ACTIVE_XNODE_INPUT_TOO_NEW:
            return "INPUT_TOO_NEW";
        case ACTIVE_XNODE_NOT_CAPABLE:
            return "NOT_CAPABLE";
        case ACTIVE_XNODE_STARTED:
            return "STARTED";
        default:
            return "UNKNOWN";
    }
}

std::string CActiveXnode::GetStatus() const {
    switch (nState) {
        case ACTIVE_XNODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_XNODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Xnode";
        case ACTIVE_XNODE_INPUT_TOO_NEW:
            return strprintf("Xnode input must have at least %d confirmations",
                             Params().GetConsensus().nXnodeMinimumConfirmations);
        case ACTIVE_XNODE_NOT_CAPABLE:
            return "Not capable xnode: " + strNotCapableReason;
        case ACTIVE_XNODE_STARTED:
            return "Xnode successfully started";
        default:
            return "Unknown";
    }
}

std::string CActiveXnode::GetTypeString() const {
    std::string strType;
    switch (eType) {
        case XNODE_UNKNOWN:
            strType = "UNKNOWN";
            break;
        case XNODE_REMOTE:
            strType = "REMOTE";
            break;
        case XNODE_LOCAL:
            strType = "LOCAL";
            break;
        default:
            strType = "UNKNOWN";
            break;
    }
    return strType;
}

bool CActiveXnode::SendXnodePing() {
    if (!fPingerEnabled) {
        LogPrint("xnode",
                 "CActiveXnode::SendXnodePing -- %s: xnode ping service is disabled, skipping...\n",
                 GetStateString());
        return false;
    }

    if (!mnodeman.Has(vin)) {
        strNotCapableReason = "Xnode not in xnode list";
        nState = ACTIVE_XNODE_NOT_CAPABLE;
        LogPrintf("CActiveXnode::SendXnodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CXnodePing mnp(vin);
    if (!mnp.Sign(keyXnode, pubKeyXnode)) {
        LogPrintf("CActiveXnode::SendXnodePing -- ERROR: Couldn't sign Xnode Ping\n");
        return false;
    }

    // Update lastPing for our xnode in Xnode list
    if (mnodeman.IsXnodePingedWithin(vin, XNODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveXnode::SendXnodePing -- Too early to send Xnode Ping\n");
        return false;
    }

    mnodeman.SetXnodeLastPing(vin, mnp);

    LogPrintf("CActiveXnode::SendXnodePing -- Relaying ping, collateral=%s\n", vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveXnode::ManageStateInitial() {
    LogPrint("xnode", "CActiveXnode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_XNODE_NOT_CAPABLE;
        strNotCapableReason = "Xnode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CXnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_XNODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveXnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CXnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }

    if (!fFoundLocal) {
        nState = ACTIVE_XNODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_XNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(),
                                            mainnetDefaultPort);
            LogPrintf("CActiveXnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_XNODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(),
                                        mainnetDefaultPort);
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveXnode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    //TODO
    if (!ConnectNode(CAddress(service, NODE_NETWORK), NULL, false, true)) {
        nState = ACTIVE_XNODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = XNODE_REMOTE;

    // Check if wallet funds are available
    if (!pwalletMain) {
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if (pwalletMain->IsLocked()) {
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if (pwalletMain->GetBalance() < XNODE_COIN_REQUIRED * COIN) {
        LogPrintf("CActiveXnode::ManageStateInitial -- %s: Wallet balance is < 1000 GXX\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if (pwalletMain->GetXnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = XNODE_LOCAL;
    }

    LogPrint("xnode", "CActiveXnode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveXnode::ManageStateRemote() {
    LogPrint("xnode",
             "CActiveXnode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyXnode.GetID() = %s\n",
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeyXnode.GetID().ToString());

    mnodeman.CheckXnode(pubKeyXnode);
    xnode_info_t infoMn = mnodeman.GetXnodeInfo(pubKeyXnode);
    if (infoMn.fInfoValid) {
        if (infoMn.nProtocolVersion < mnpayments.GetMinXnodePaymentsProto()) {
            nState = ACTIVE_XNODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveXnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (service != infoMn.addr) {
            nState = ACTIVE_XNODE_NOT_CAPABLE;
            // LogPrintf("service: %s\n", service.ToString());
            // LogPrintf("infoMn.addr: %s\n", infoMn.addr.ToString());
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this xnode changed recently.";
            LogPrintf("CActiveXnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (!CXnode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_XNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Xnode in %s state", CXnode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveXnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (nState != ACTIVE_XNODE_STARTED) {
            LogPrintf("CActiveXnode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_XNODE_STARTED;
        }
    } else {
        nState = ACTIVE_XNODE_NOT_CAPABLE;
        strNotCapableReason = "Xnode not in xnode list";
        LogPrintf("CActiveXnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}

void CActiveXnode::ManageStateLocal() {
    LogPrint("xnode", "CActiveXnode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
    if (nState == ACTIVE_XNODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if (pwalletMain->GetXnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge < Params().GetConsensus().nXnodeMinimumConfirmations) {
            nState = ACTIVE_XNODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveXnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CXnodeBroadcast mnb;
        std::string strError;
        if (!CXnodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keyXnode,
                                     pubKeyXnode, strError, mnb)) {
            nState = ACTIVE_XNODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveXnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        nState = ACTIVE_XNODE_STARTED;

        //update to xnode list
        LogPrintf("CActiveXnode::ManageStateLocal -- Update Xnode List\n");
        mnodeman.UpdateXnodeList(mnb);
        mnodeman.NotifyXnodeUpdates();

        //send to all peers
        LogPrintf("CActiveXnode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.RelayXNode();
    }
}
