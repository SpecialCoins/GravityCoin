// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEXNODE_H
#define ACTIVEXNODE_H

#include "net.h"
#include "key.h"
#include "wallet/wallet.h"

class CActiveXnode;

static const int ACTIVE_XNODE_INITIAL          = 0; // initial state
static const int ACTIVE_XNODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_XNODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_XNODE_NOT_CAPABLE      = 3;
static const int ACTIVE_XNODE_STARTED          = 4;

extern CActiveXnode activeXnode;

// Responsible for activating the Xnode and pinging the network
class CActiveXnode
{
public:
    enum xnode_type_enum_t {
        XNODE_UNKNOWN = 0,
        XNODE_REMOTE  = 1,
        XNODE_LOCAL   = 2
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    xnode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Xnode
    bool SendXnodePing();

public:
    // Keys for the active Xnode
    CPubKey pubKeyXnode;
    CKey keyXnode;

    // Initialized while registering Xnode
    CTxIn vin;
    CService service;

    int nState; // should be one of ACTIVE_XNODE_XXXX
    std::string strNotCapableReason;

    CActiveXnode()
        : eType(XNODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyXnode(),
          keyXnode(),
          vin(),
          service(),
          nState(ACTIVE_XNODE_INITIAL)
    {}

    /// Manage state of active Xnode
    void ManageState();

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

private:
    void ManageStateInitial();
    void ManageStateRemote();
    void ManageStateLocal();
};

#endif
