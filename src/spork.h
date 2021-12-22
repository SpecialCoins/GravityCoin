// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SPORK_H
#define SPORK_H

#include "hash.h"
#include "net.h"
#include "utilstrencodings.h"
#include "util.h"

class CSporkMessage;

/*
    Don't ever reuse these IDs for other sporks
    - This would result in old clients getting confused about which spork is for what
*/
static const int SPORK_START                                            = 10001;
static const int SPORK_END                                              = 10010;

static const int SPORK_1_VERSION_ON                                        = 10001;
static const int SPORK_2_F_PAYMENT_START                                = 10002;
static const int SPORK_3_F_PAYMENT_ENFORCEMENT                          = 10003;
static const int SPORK_4_XNODE_PAYMENT_START                            = 10004;
static const int SPORK_5_XNODE_PAYMENT_ENFORCEMENT                      = 10005;
static const int SPORK_7_RECONSIDER_BLOCKS                              = 10007;
static const int SPORK_9_SIGMA_NEW                                      = 10009;
static const int SPORK_10_SIGMA                                         = 10010;

static const int64_t SPORK_1_VERSION_ON_DEFAULT                            = 4070908800;   // OFF
static const int64_t SPORK_2_F_PAYMENT_START_DEFAULT                    = 4070908800;   // OFF
static const int64_t SPORK_3_F_PAYMENT_ENFORCEMENT_DEFAULT              = 4070908800;   // OFF
static const int64_t SPORK_4_XNODE_PAYMENT_START_DEFAULT                = 4070908800;   // OFF
static const int64_t SPORK_5_XNODE_PAYMENT_ENFORCEMENT_DEFAULT          = 4070908800;   // OFF
static const int64_t SPORK_7_RECONSIDER_BLOCKS_DEFAULT                  = 0;            // 0 BLOCKS
static const int64_t SPORK_9_SIGMA_NEW_DEFAULT                          = 4070908800;   // OFF
static const int64_t SPORK_10_SIGMA_DEFAULT                             = 4070908800;   // OFF

extern std::map<uint256, CSporkMessage> mapSporks;

//
// Spork classes
// Keep track of all of the network spork settings
//

class CSporkMessage
{
private:
    std::vector<unsigned char> vchSig;

public:
    int nSporkID;
    int64_t nValue;
    int64_t nTimeSigned;

    CSporkMessage(int nSporkID, int64_t nValue, int64_t nTimeSigned) :
        nSporkID(nSporkID),
        nValue(nValue),
        nTimeSigned(nTimeSigned)
        {}

    CSporkMessage() :
        nSporkID(0),
        nValue(0),
        nTimeSigned(0)
        {}


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(nSporkID);
        READWRITE(nValue);
        READWRITE(nTimeSigned);
        READWRITE(vchSig);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << nSporkID;
        ss << nValue;
        ss << nTimeSigned;
        return ss.GetHash();
    }

    bool Sign(std::string strSignKey);
    bool CheckSignature();
    void Relay();
};


class CSporkManager
{
private:
    std::vector<unsigned char> vchSig;
    std::string strMasterPrivKey;
    std::map<int, CSporkMessage> mapSporksActive;

public:

    CSporkManager() {}

    void ProcessSpork(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    void ExecuteSpork(int nSporkID, int nValue);
    bool UpdateSpork(int nSporkID, int64_t nValue);

    bool IsSporkActive(int nSporkID);
    int64_t GetSporkValue(int nSporkID);
    int GetSporkIDByName(std::string strName);
    std::string GetSporkNameByID(int nSporkID);

    bool SetPrivKey(std::string strPrivKey);
};

extern CSporkManager sporkManager;

#endif
