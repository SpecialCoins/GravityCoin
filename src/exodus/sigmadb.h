#ifndef ZCOIN_EXODUS_SIGMADB_H
#define ZCOIN_EXODUS_SIGMADB_H

#include "convert.h"
#include "persistence.h"
#include "sigma.h"
#include "log.h"

#include <univalue.h>

#include <boost/filesystem/path.hpp>

#include <string>
#include <vector>

#define MAX_COINS_PER_GROUP 16384 /* Limit of sigma anonimity group which is 2 ^ 14 */

template<typename T, typename = void>
struct is_iterator
{
   static constexpr bool value = false;
};

template<typename T>
struct is_iterator<T, typename std::enable_if<!std::is_same<typename std::iterator_traits<T>::iterator_category, void>::value>::type>
{
   static constexpr bool value = true;
};

/** LevelDB based storage for sigma mints, with
*/
class CMPMintList : public CDBBase
{
public:
    CMPMintList(const boost::filesystem::path& path, bool fWipe, uint16_t groupSize = MAX_COINS_PER_GROUP);
    ~CMPMintList();

    std::pair<uint32_t, uint16_t> RecordMint(uint32_t propertyId, uint8_t denomination, const exodus::SigmaPublicKey& pubKey, int32_t height);

    template<
        class OutputIt,
        typename std::enable_if<is_iterator<OutputIt>::value, void>::type* = nullptr
    > OutputIt GetAnonimityGroup(uint32_t propertyId, uint8_t denomination, uint32_t groupId, size_t count, OutputIt firstIt)
    {
        GetAnonimityGroup(propertyId, denomination, groupId, count, [&firstIt](exodus::SigmaPublicKey& pub) mutable {
            *firstIt++ = std::move(pub);
        });

        return firstIt;
    }
    size_t GetAnonimityGroup(uint32_t propertyId, uint8_t denomination, uint32_t groupId, size_t count,
        std::function<void(exodus::SigmaPublicKey&)>);

    void DeleteAll(int32_t startBlock);

private:
    void RecordMintKey(const leveldb::Slice& mintKey);

public:
    uint16_t const groupSize;

    uint32_t GetLastGroupId(uint32_t propertyId, uint8_t denomination);
    size_t GetMintCount(uint32_t propertyId, uint8_t denomination, uint32_t groupId);
    uint64_t GetNextSequence();
    std::pair<exodus::SigmaPublicKey, int32_t> GetMint(uint32_t propertyId, uint8_t denomination, uint32_t groupId, uint16_t index);
};

#endif // ZCOIN_EXODUS_SIGMADB_H