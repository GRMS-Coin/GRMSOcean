// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/******************************************************************************
 * Copyright © 2014-2019 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#include "txmempool.h"

#include "clientversion.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "main.h"
#include "policy/fees.h"
#include "streams.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validationinterface.h"
#include "version.h"
#define _COINBASE_MATURITY 100

#include "cc/CCinclude.h"

using namespace std;

CTxMemPoolEntry::CTxMemPoolEntry():
    nFee(0), nTxSize(0), nModSize(0), nUsageSize(0), nTime(0), dPriority(0.0),
    hadNoDependencies(false), spendsCoinbase(false)
{
    nHeight = MEMPOOL_HEIGHT;
}

CTxMemPoolEntry::CTxMemPoolEntry(const CTransaction& _tx, const CAmount& _nFee,
                                 int64_t _nTime, double _dPriority,
                                 unsigned int _nHeight, bool poolHasNoInputsOf,
                                 bool _spendsCoinbase, uint32_t _nBranchId):
    tx(_tx), nFee(_nFee), nTime(_nTime), dPriority(_dPriority), nHeight(_nHeight),
    hadNoDependencies(poolHasNoInputsOf),
    spendsCoinbase(_spendsCoinbase), nBranchId(_nBranchId)
{
    nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
    nModSize = tx.CalculateModifiedSize(nTxSize);
    nUsageSize = RecursiveDynamicUsage(tx);
    feeRate = CFeeRate(nFee, nTxSize);
}

CTxMemPoolEntry::CTxMemPoolEntry(const CTxMemPoolEntry& other)
{
    *this = other;
}

double
CTxMemPoolEntry::GetPriority(unsigned int currentHeight) const
{
    CAmount nValueIn = tx.GetValueOut()+nFee;
    double deltaPriority = ((double)(currentHeight-nHeight)*nValueIn)/nModSize;
    double dResult = dPriority + deltaPriority;
    return dResult;
}

CTxMemPool::CTxMemPool(const CFeeRate& _minRelayFee) :
    nTransactionsUpdated(0)
{
    // Sanity checks off by default for performance, because otherwise
    // accepting transactions becomes O(N^2) where N is the number
    // of transactions in the pool
    nCheckFrequency = 0;

    minerPolicyEstimator = new CBlockPolicyEstimator(_minRelayFee);
}

CTxMemPool::~CTxMemPool()
{
    delete minerPolicyEstimator;
}

void CTxMemPool::pruneSpent(const uint256 &hashTx, CCoins &coins)
{
    LOCK(cs);

    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.lower_bound(COutPoint(hashTx, 0));

    // iterate over all COutPoints in mapNextTx whose hash equals the provided hashTx
    while (it != mapNextTx.end() && it->first.hash == hashTx) {
        coins.Spend(it->first.n); // and remove those outputs from coins
        it++;
    }
}

unsigned int CTxMemPool::GetTransactionsUpdated() const
{
    LOCK(cs);
    return nTransactionsUpdated;
}

void CTxMemPool::AddTransactionsUpdated(unsigned int n)
{
    LOCK(cs);
    nTransactionsUpdated += n;
}


bool CTxMemPool::addUnchecked(const uint256& hash, const CTxMemPoolEntry &entry, bool fCurrentEstimate)
{
    // Add to memory pool without checking anything.
    // Used by main.cpp AcceptToMemoryPool(), which DOES do
    // all the appropriate checks.
    LOCK(cs);
    mapTx.insert(entry);
    const CTransaction& tx = mapTx.find(hash)->GetTx();
    mapRecentlyAddedTx[tx.GetHash()] = &tx;
    nRecentlyAddedSequence += 1;
    if (!tx.IsCoinImport()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            if (tx.IsPegsImport() && i==0) continue;
            mapNextTx[tx.vin[i].prevout] = CInPoint(&tx, i);
        }
    }
    BOOST_FOREACH(const JSDescription &joinsplit, tx.vjoinsplit) {
        BOOST_FOREACH(const uint256 &nf, joinsplit.nullifiers) {
            mapSproutNullifiers[nf] = &tx;
        }
    }
    for (const SpendDescription &spendDescription : tx.vShieldedSpend) {
        mapSaplingNullifiers[spendDescription.nullifier] = &tx;
    }
    nTransactionsUpdated++;
    totalTxSize += entry.GetTxSize();
    cachedInnerUsage += entry.DynamicMemoryUsage();
    minerPolicyEstimator->processTransaction(entry, fCurrentEstimate);

    return true;
}

void CTxMemPool::addAddressIndex(const CTxMemPoolEntry &entry, const CCoinsViewCache &view)
{
    LOCK(cs);
    const CTransaction& tx = entry.GetTx();
    std::vector<CMempoolAddressDeltaKey> inserted;

    uint256 txhash = tx.GetHash();
    for (unsigned int j = 0; j < tx.vin.size(); j++) {
        if (tx.IsPegsImport() && j==0) continue; 
        const CTxIn input = tx.vin[j];
        const CTxOut &prevout = view.GetOutputFor(input);

        vector<vector<unsigned char>> vSols;
        txnouttype txType = TX_PUBKEYHASH;
        CTxDestination vDest;
        int keyType = GetAddressType(prevout.scriptPubKey, vDest, txType, vSols);
        if (keyType != 0) 
        {
            for (auto addr : vSols)
            {
                // add index entry for vins:
                CMempoolAddressDeltaKey key(keyType, addr.size() == 20 ? uint160(addr) : Hash160(addr), txhash, j, true);
                CMempoolAddressDelta delta(entry.GetTime(), prevout.nValue * -1, input.prevout.hash, input.prevout.n);
                mapAddress.insert(make_pair(key, delta));
                inserted.push_back(key);
            }
        }
    }

    for (unsigned int k = 0; k < tx.vout.size(); k++) {
        const CTxOut &out = tx.vout[k];

        vector<vector<unsigned char>> vSols;
        CTxDestination vDest;
        txnouttype txType = TX_PUBKEYHASH;
        int keyType = GetAddressType(out.scriptPubKey, vDest, txType, vSols);
        if (keyType != 0 && txType != TX_MULTISIG) 
        {
            for (auto addr : vSols)
            {
                // add index entry for vouts:
                CMempoolAddressDeltaKey key(keyType, addr.size() == 20 ? uint160(addr) : Hash160(addr), txhash, k, 0);
                mapAddress.insert(make_pair(key, CMempoolAddressDelta(entry.GetTime(), out.nValue)));
                inserted.push_back(key);
            }
        }
    }

    mapAddressInserted.insert(make_pair(txhash, inserted));
}

bool CTxMemPool::getAddressIndex(std::vector<std::pair<uint160, int> > &addresses,
                                 std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> > &results)
{
    LOCK(cs);
    for (std::vector<std::pair<uint160, int> >::iterator it = addresses.begin(); it != addresses.end(); it++) {
        addressDeltaMap::iterator ait = mapAddress.lower_bound(CMempoolAddressDeltaKey((*it).second, (*it).first));
        while (ait != mapAddress.end() && (*ait).first.addressBytes == (*it).first && (*ait).first.type == (*it).second) {
            results.push_back(*ait);
            ait++;
        }
    }
    return true;
}

bool CTxMemPool::removeAddressIndex(const uint256 txhash)
{
    LOCK(cs);
    addressDeltaMapInserted::iterator it = mapAddressInserted.find(txhash);

    if (it != mapAddressInserted.end()) {
        std::vector<CMempoolAddressDeltaKey> keys = (*it).second;
        for (std::vector<CMempoolAddressDeltaKey>::iterator mit = keys.begin(); mit != keys.end(); mit++) {
            mapAddress.erase(*mit);
        }
        mapAddressInserted.erase(it);
    }

    return true;
}

void CTxMemPool::addSpentIndex(const CTxMemPoolEntry &entry, const CCoinsViewCache &view)
{
    LOCK(cs);

    const CTransaction& tx = entry.GetTx();
    std::vector<CSpentIndexKey> inserted;

    uint256 txhash = tx.GetHash();
    for (unsigned int j = 0; j < tx.vin.size(); j++) {
        if (tx.IsPegsImport() && j==0) continue; 
        const CTxIn input = tx.vin[j];
        const CTxOut &prevout = view.GetOutputFor(input);

        vector<vector<unsigned char>> vSols;
        CTxDestination vDest;
        txnouttype txType = TX_PUBKEYHASH;
        int keyType = GetAddressType(prevout.scriptPubKey, vDest, txType, vSols);
        if (keyType != 0 && txType != TX_MULTISIG) 
        {
            for (auto addr : vSols)
            {
                CSpentIndexKey key = CSpentIndexKey(input.prevout.hash, input.prevout.n);
                CSpentIndexValue value = CSpentIndexValue(txhash, j, -1, prevout.nValue, keyType, addr.size() == 20 ? uint160(addr) : Hash160(addr));
                mapSpent.insert(make_pair(key, value));
                inserted.push_back(key);
            }
        }
        else
        {
            // don't know exactly how, but it was spent
            CSpentIndexKey key = CSpentIndexKey(input.prevout.hash, input.prevout.n);
            CSpentIndexValue value = CSpentIndexValue(txhash, j, -1, prevout.nValue, 0, uint160());
            mapSpent.insert(make_pair(key, value));
            inserted.push_back(key);
        }
    }
    mapSpentInserted.insert(make_pair(txhash, inserted));
}

bool CTxMemPool::getSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value)
{
    LOCK(cs);
    mapSpentIndex::iterator it;

    it = mapSpent.find(key);
    if (it != mapSpent.end()) {
        value = it->second;
        return true;
    }
    return false;
}

bool CTxMemPool::removeSpentIndex(const uint256 txhash)
{
    LOCK(cs);
    mapSpentIndexInserted::iterator it = mapSpentInserted.find(txhash);

    if (it != mapSpentInserted.end()) {
        std::vector<CSpentIndexKey> keys = (*it).second;
        for (std::vector<CSpentIndexKey>::iterator mit = keys.begin(); mit != keys.end(); mit++) {
            mapSpent.erase(*mit);
        }
        mapSpentInserted.erase(it);
    }

    return true;
}

void CTxMemPool::addUnspentCCIndex(const CTxMemPoolEntry &entry, const CCoinsViewCache &view)
{
    LOCK(cs);
    const CTransaction& tx = entry.GetTx();
    std::vector<CUnspentCCIndexKey> inserted;

    uint256 txhash = tx.GetHash();
    for (unsigned int j = 0; j < tx.vin.size(); j++) {
        if (tx.IsPegsImport() && j==0) continue; 
        const CTxIn input = tx.vin[j];
        const CTxOut &prevout = view.GetOutputFor(input);

        vector<vector<unsigned char>> vSols;
        txnouttype txType = TX_PUBKEYHASH;
        CTxDestination vDest;
        int keyType = GetAddressType(prevout.scriptPubKey, vDest, txType, vSols);
        if (keyType == 3)  // cc type
        {
            if (vSols.size() > 0)   
            {                                    
                uint160 addrHash = vSols[0].size() == 20 ? uint160(vSols[0]) : Hash160(vSols[0]); // use first vSol data as the address
                uint256 hashBlock;
                
                CTxMemPool::indexed_transaction_set::const_iterator it = mapTx.find(input.prevout.hash); // load previous mempool tx to get opreturn
                // note we add only mempool tx into the unspent cc index
                // so non-mempool txns spent by mempool txns will be found in the db cc index as unspent 
                // that is, the caller should always check that vout is not spent in mempool.
                // This limitation is because the coin cache does not store opreturns
                if (it != mapTx.end() && it->GetTx().vout.size() > 0) {  
                    const CTransaction& vintx = it->GetTx();
                    uint256 creationId;
                    uint8_t evalcode, funcid, version;
                    CScript prevOpreturn; //init as empty
                    if (vintx.vout.back().scriptPubKey.size() > 0 && vintx.vout.back().scriptPubKey[0] == OP_RETURN)
                        prevOpreturn = vintx.vout.back().scriptPubKey;

                    if (CCDecodeTxVout(vintx, input.prevout.n, evalcode, funcid, version, creationId))  {
                        CUnspentCCIndexKey key(addrHash, creationId, input.prevout.hash, input.prevout.n);
                        mapUnspentCCIndex.erase(key);
                        //std::cerr << __func__ << " removing previous from mempool cc index addrHash=" << addrHash.GetHex() << " tx=" << txhash.GetHex() << " input.prevout.hash=" << input.prevout.hash.GetHex() << " input.prevout.n=" << j << " evalcode=" << (int)evalcode << " creationId=" << creationId.GetHex() << " prevOpreturn.size()=" << prevOpreturn.size() << std::endl; 
                        inserted.push_back(key);
                    }
                }
            }
        }
    }

    for (unsigned int k = 0; k < tx.vout.size(); k++) {
        const CTxOut &out = tx.vout[k];

        vector<vector<unsigned char>> vSols;
        CTxDestination vDest;
        txnouttype txType = TX_PUBKEYHASH;
        int keyType = GetAddressType(out.scriptPubKey, vDest, txType, vSols);
        if (keyType == 3 && txType != TX_MULTISIG)  // cc vout type
        {
             if (vSols.size() > 0)   
            {                                 
                uint160 addrHash = vSols[0].size() == 20 ? uint160(vSols[0]) : Hash160(vSols[0]); // use first vSol data as the address                                    
                uint256 creationId;
                uint8_t evalcode, funcid, version;
                CScript opreturn; //init as empty
                if (tx.vout.back().scriptPubKey.size() > 0 && tx.vout.back().scriptPubKey[0] == OP_RETURN)
                    opreturn = tx.vout.back().scriptPubKey;

                if (CCDecodeTxVout(tx, k, evalcode, funcid, version, creationId))  {
                    // record cc index output with spk and opreturn
                    CUnspentCCIndexKey key(addrHash, creationId, txhash, k);
                    CUnspentCCIndexValue value(tx.vout[k].nValue, tx.vout[k].scriptPubKey, opreturn, 0, evalcode, funcid, version);
                    mapUnspentCCIndex.insert(make_pair(key, value));
                    //std::cerr << __func__ << " adding to mempool cc index addrHash=" << addrHash.GetHex() << " tx=" << txhash.GetHex() << " nvout=" << k << " evalcode=" << (int)evalcode << " creationId=" << creationId.GetHex() << " opreturn.size()=" << opreturn.size() << " mapUnspentCCIndex.size=" << mapUnspentCCIndex.size() << std::endl; 
                    inserted.push_back(key);
                }
            }
        }
    }

    mapUnspentCCIndexInserted.insert(make_pair(txhash, inserted));
}

// finds outputs by hash160 of a cc address (creationid must by null)
// or by a pair of hash160 of a cc address and creationid
bool CTxMemPool::getUnspentCCIndex(const std::vector<std::pair<uint160, uint256> > &keys, std::vector<std::pair<CUnspentCCIndexKey, CUnspentCCIndexValue> > &outputs)
{
    LOCK(cs);
    for (std::vector<std::pair<uint160, uint256> >::const_iterator it = keys.begin(); it != keys.end(); it++) {
        mapUnspentCCIndexType::iterator ait = mapUnspentCCIndex.lower_bound(CUnspentCCIndexKey((*it).first, (*it).second, zeroid, 0));        
        while (ait != mapUnspentCCIndex.end() && (*ait).first.hashBytes == (*it).first && ((*ait).first.creationid == (*it).second || (*it).second.IsNull())) {
            outputs.push_back(*ait);
            ait++;
        }

        {
            //std::cerr << __func__ << " (*it).first=" << (*it).first.GetHex() << std::endl;
            mapUnspentCCIndexType::iterator ait = mapUnspentCCIndex.lower_bound(CUnspentCCIndexKey((*it).first, (*it).second, zeroid, 0));        
            while (ait != mapUnspentCCIndex.end() ) {
                //std::cerr << __func__ << " (*ait).first.hashBytes=" << (*ait).first.hashBytes.GetHex() << " (*ait).first.creationid=" << (*ait).first.creationid.GetHex() << " txhash=" << (*ait).first.txhash.GetHex() << " index=" << (*ait).first.index << std::endl;
                ait++;
            }
        }
    }
    return true;
}

// erase tx unspent entry and restore previous as unspents
bool CTxMemPool::removeUnspentCCIndex(const CTransaction &tx)
{
    LOCK(cs);
    /* mapUnspentCCIndexInsertedType::iterator it = mapUnspentCCIndexInserted.find(txhash);

    if (it != mapUnspentCCIndexInserted.end()) {
        std::vector<CUnspentCCIndexKey> keys = (*it).second;
        for (std::vector<CUnspentCCIndexKey>::iterator mit = keys.begin(); mit != keys.end(); mit++) {
            mapUnspentCCIndex.erase(*mit);
        }
        mapUnspentCCIndexInserted.erase(it);
    } */

    uint256 txhash = tx.GetHash();
    // restore previous mempool tx as unspents
    for (unsigned int j = 0; j < tx.vin.size(); j++) {
        if (tx.IsPegsImport() && j==0) continue; 
        const CTxIn input = tx.vin[j];
        //const CTxOut &prevout = view.GetOutputFor(input);

        vector<vector<unsigned char>> vSols;
        txnouttype txType = TX_PUBKEYHASH;
        CTxDestination vDest;
        //int keyType = GetAddressType(prevout.scriptPubKey, vDest, txType, vSols);
        //if (keyType == 3)  // cc type
        if (IsCCInput(tx.vin[j].scriptSig))
        {
            if (vSols.size() > 0)   
            {                                    
                uint160 addrHash = vSols[0].size() == 20 ? uint160(vSols[0]) : Hash160(vSols[0]); // use first vSol data as the address
                uint256 hashBlock;
                
                CTxMemPool::indexed_transaction_set::const_iterator it = mapTx.find(input.prevout.hash); // load previous mempool tx to get opreturn
                // note we add only mempool tx into the unspent cc index
                // so non-mempool txns spent by mempool txns will be found in the db cc index as unspent 
                // that is, the caller should always check that vout is not spent in mempool.
                // This limitation is because the coin cache does not store opreturns
                if (it != mapTx.end() && it->GetTx().vout.size() > 0) {  
                    const CTransaction& vintx = it->GetTx();
                    uint256 creationId;
                    uint8_t evalcode, funcid, version;
                    CScript prevOpreturn; //init as empty
                    if (vintx.vout.back().scriptPubKey.size() > 0 && vintx.vout.back().scriptPubKey[0] == OP_RETURN)
                        prevOpreturn = vintx.vout.back().scriptPubKey;

                    if (CCDecodeTxVout(vintx, input.prevout.n, evalcode, funcid, version, creationId))  {
                        CUnspentCCIndexKey key(addrHash, creationId, input.prevout.hash, input.prevout.n);
                        CUnspentCCIndexValue value(vintx.vout[input.prevout.n].nValue, vintx.vout[input.prevout.n].scriptPubKey, prevOpreturn, 0, evalcode, funcid, version);
                        mapUnspentCCIndex.insert(make_pair(key, value));
                        //std::cerr << __func__ << " restoring previous to mempool cc index addrHash=" << addrHash.GetHex() << " tx=" << txhash.GetHex() << " input.prevout.hash=" << input.prevout.hash.GetHex() << " input.prevout.n=" << j << " evalcode=" << (int)evalcode << " creationId=" << creationId.GetHex() << " prevOpreturn.size()=" << prevOpreturn.size() << std::endl; 
                    }
                }
            }
        }
    }

    // eraase entries for the tx
    for (unsigned int k = 0; k < tx.vout.size(); k++) {
        const CTxOut &out = tx.vout[k];

        vector<vector<unsigned char>> vSols;
        CTxDestination vDest;
        txnouttype txType = TX_PUBKEYHASH;
        int keyType = GetAddressType(out.scriptPubKey, vDest, txType, vSols);
        if (keyType == 3 && txType != TX_MULTISIG)  // cc vout type
        {
             if (vSols.size() > 0)   
            {                                 
                uint160 addrHash = vSols[0].size() == 20 ? uint160(vSols[0]) : Hash160(vSols[0]); // use first vSol data as the address                                    
                uint256 creationId;
                uint8_t evalcode, funcid, version;
                CScript opreturn; //init as empty
                if (tx.vout.back().scriptPubKey.size() > 0 && tx.vout.back().scriptPubKey[0] == OP_RETURN)
                    opreturn = tx.vout.back().scriptPubKey;

                if (CCDecodeTxVout(tx, k, evalcode, funcid, version, creationId))  {
                    // record cc index output with spk and opreturn
                    CUnspentCCIndexKey key(addrHash, creationId, txhash, k);
                    CUnspentCCIndexValue value(tx.vout[k].nValue, tx.vout[k].scriptPubKey, opreturn, 0, evalcode, funcid, version);
                    mapUnspentCCIndex.erase(key);
                    //std::cerr << __func__ << " removing from mempool cc index addrHash=" << addrHash.GetHex() << " tx=" << txhash.GetHex() << " nvout=" << k << " evalcode=" << (int)evalcode << " creationId=" << creationId.GetHex() << " opreturn.size()=" << opreturn.size() << std::endl; 
                }
            }
        }
    }
    return true;
}

void CTxMemPool::remove(const CTransaction &origTx, std::list<CTransaction>& removed, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        std::deque<uint256> txToRemove;
        txToRemove.push_back(origTx.GetHash());
        if (fRecursive && !mapTx.count(origTx.GetHash())) {
            // If recursively removing but origTx isn't in the mempool
            // be sure to remove any children that are in the pool. This can
            // happen during chain re-orgs if origTx isn't re-accepted into
            // the mempool for any reason.
            for (unsigned int i = 0; i < origTx.vout.size(); i++) {
                std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(origTx.GetHash(), i));
                if (it == mapNextTx.end())
                    continue;
                txToRemove.push_back(it->second.ptx->GetHash());
            }
        }
        while (!txToRemove.empty())
        {
            uint256 hash = txToRemove.front();
            txToRemove.pop_front();
            if (!mapTx.count(hash))
                continue;
            const CTransaction& tx = mapTx.find(hash)->GetTx();
            const CTransaction txCopy = tx; // save for cc index clean up 
            if (fRecursive) {
                for (unsigned int i = 0; i < tx.vout.size(); i++) {
                    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
                    if (it == mapNextTx.end())
                        continue;
                    txToRemove.push_back(it->second.ptx->GetHash());
                }
            }
            mapRecentlyAddedTx.erase(hash);
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            BOOST_FOREACH(const JSDescription& joinsplit, tx.vjoinsplit) {
                BOOST_FOREACH(const uint256& nf, joinsplit.nullifiers) {
                    mapSproutNullifiers.erase(nf);
                }
            }
            for (const SpendDescription &spendDescription : tx.vShieldedSpend) {
                mapSaplingNullifiers.erase(spendDescription.nullifier);
            }
            removed.push_back(tx);
            totalTxSize -= mapTx.find(hash)->GetTxSize();
            cachedInnerUsage -= mapTx.find(hash)->DynamicMemoryUsage();
            mapTx.erase(hash);
            nTransactionsUpdated++;
            minerPolicyEstimator->removeTx(hash);
            removeAddressIndex(hash);
            removeSpentIndex(hash);
            removeUnspentCCIndex(txCopy);  // erase cc index entry if present
        }
    }
}

extern uint64_t ASSETCHAINS_TIMELOCKGTE;
int64_t komodo_block_unlocktime(uint32_t nHeight);

void CTxMemPool::removeForReorg(const CCoinsViewCache *pcoins, unsigned int nMemPoolHeight, int flags)
{
    // Remove transactions spending a coinbase which are now immature
    extern char ASSETCHAINS_SYMBOL[KOMODO_ASSETCHAIN_MAXLEN];
    if ( ASSETCHAINS_SYMBOL[0] == 0 )
        COINBASE_MATURITY = _COINBASE_MATURITY;
    // Remove transactions spending a coinbase which are now immature and no-longer-final transactions
    LOCK(cs);
    list<CTransaction> transactionsToRemove;
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        const CTransaction& tx = it->GetTx();
        if (!CheckFinalTx(tx, flags)) {
            transactionsToRemove.push_back(tx);
        } else if (it->GetSpendsCoinbase()) {
            BOOST_FOREACH(const CTxIn& txin, tx.vin) {
                indexed_transaction_set::const_iterator it2 = mapTx.find(txin.prevout.hash);
                if (it2 != mapTx.end())
                    continue;
                const CCoins *coins = pcoins->AccessCoins(txin.prevout.hash);
		        if (nCheckFrequency != 0) assert(coins);
                if (!coins || (coins->IsCoinBase() && (((signed long)nMemPoolHeight) - coins->nHeight < COINBASE_MATURITY) && 
                                                       ((signed long)nMemPoolHeight < komodo_block_unlocktime(coins->nHeight) && 
                                                         coins->IsAvailable(0) && coins->vout[0].nValue >= ASSETCHAINS_TIMELOCKGTE))) {
                    transactionsToRemove.push_back(tx);
                    break;
                }
            }
        }
    }
    BOOST_FOREACH(const CTransaction& tx, transactionsToRemove) {
        list<CTransaction> removed;
        remove(tx, removed, true);
    }
}


void CTxMemPool::removeWithAnchor(const uint256 &invalidRoot, ShieldedType type)
{
    // If a block is disconnected from the tip, and the root changed,
    // we must invalidate transactions from the mempool which spend
    // from that root -- almost as though they were spending coinbases
    // which are no longer valid to spend due to coinbase maturity.
    LOCK(cs);
    list<CTransaction> transactionsToRemove;

    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        const CTransaction& tx = it->GetTx();
        switch (type) {
            case SPROUT:
                BOOST_FOREACH(const JSDescription& joinsplit, tx.vjoinsplit) {
                    if (joinsplit.anchor == invalidRoot) {
                        transactionsToRemove.push_back(tx);
                        break;
                    }
                }
            break;
            case SAPLING:
                BOOST_FOREACH(const SpendDescription& spendDescription, tx.vShieldedSpend) {
                    if (spendDescription.anchor == invalidRoot) {
                        transactionsToRemove.push_back(tx);
                        break;
                    }
                }
            break;
            default:
                throw runtime_error("Unknown shielded type");
            break;
        }
    }

    BOOST_FOREACH(const CTransaction& tx, transactionsToRemove) {
        list<CTransaction> removed;
        remove(tx, removed, true);
    }
}

void CTxMemPool::removeConflicts(const CTransaction &tx, std::list<CTransaction>& removed)
{
    // Remove transactions which depend on inputs of tx, recursively
    list<CTransaction> result;
    LOCK(cs);
    BOOST_FOREACH(const CTxIn &txin, tx.vin) {
        std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
            {
                remove(txConflict, removed, true);
            }
        }
    }

    BOOST_FOREACH(const JSDescription &joinsplit, tx.vjoinsplit) {
        BOOST_FOREACH(const uint256 &nf, joinsplit.nullifiers) {
            std::map<uint256, const CTransaction*>::iterator it = mapSproutNullifiers.find(nf);
            if (it != mapSproutNullifiers.end()) {
                const CTransaction &txConflict = *it->second;
                if (txConflict != tx) {
                    remove(txConflict, removed, true);
                }
            }
        }
    }
    for (const SpendDescription &spendDescription : tx.vShieldedSpend) {
        std::map<uint256, const CTransaction*>::iterator it = mapSaplingNullifiers.find(spendDescription.nullifier);
        if (it != mapSaplingNullifiers.end()) {
            const CTransaction &txConflict = *it->second;
            if (txConflict != tx) {
                remove(txConflict, removed, true);
            }
        }
    }
}

int32_t komodo_validate_interest(const CTransaction &tx,int32_t txheight,uint32_t nTime,int32_t dispflag);
extern char ASSETCHAINS_SYMBOL[];

void CTxMemPool::removeExpired(unsigned int nBlockHeight)
{
    CBlockIndex *tipindex;
    // Remove expired txs from the mempool
    LOCK(cs);
    list<CTransaction> transactionsToRemove;
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++)
    {
        const CTransaction& tx = it->GetTx();
        tipindex = chainActive.LastTip();

        /* cmptime = chainActive.LastTip()->GetMedianTimePast() + 777 - here for interest validation, inside
        CTxMemPool::removeExpired. need to test, may be here better to validate against pindexNew->nTime.
        In ConnectBlock we have a condition for each tx like komodo_validate_interest(..., block.nTime), so
        blocks mined with such txes will be valid. Mean, may be chainActive.LastTip()->GetMedianTimePast() + 777
        is "too earlier" condition. [nBlockHeight should be equal tipindex->GetHeight()+1 here]
        */

        // if (IsExpiredTx(tx, nBlockHeight) || (ASSETCHAINS_SYMBOL[0] == 0 && tipindex != 0 && komodo_validate_interest(tx,tipindex->GetHeight()+1,tipindex->GetMedianTimePast() + 777,0)) < 0)
        bool fInterestNotValidated = ASSETCHAINS_SYMBOL[0] == 0 && tipindex != 0 && komodo_validate_interest(tx,tipindex->GetHeight()+1,tipindex->GetMedianTimePast() + 777,0) < 0;
        if (IsExpiredTx(tx, nBlockHeight) || fInterestNotValidated)
        {
            if (fInterestNotValidated && tipindex != 0)
                LogPrintf("Removing interest violate txid.%s nHeight.%d nTime.%u vs locktime.%u\n",tx.GetHash().ToString(),tipindex->GetHeight()+1,tipindex->GetMedianTimePast() + 777,tx.nLockTime);
            transactionsToRemove.push_back(tx);
        }
    }
    for (const CTransaction& tx : transactionsToRemove) {
        list<CTransaction> removed;
        remove(tx, removed, true);
        LogPrint("mempool", "Removing expired txid: %s\n", tx.GetHash().ToString());
    }
}

/**
 * Called when a block is connected. Removes from mempool and updates the miner fee estimator.
 */
void CTxMemPool::removeForBlock(const std::vector<CTransaction>& vtx, unsigned int nBlockHeight,
                                std::list<CTransaction>& conflicts, bool fCurrentEstimate)
{
    LOCK(cs);
    std::vector<CTxMemPoolEntry> entries;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uint256 hash = tx.GetHash();

        indexed_transaction_set::iterator i = mapTx.find(hash);
        if (i != mapTx.end())
            entries.push_back(*i);
    }
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        std::list<CTransaction> dummy;
        remove(tx, dummy, false);
        removeConflicts(tx, conflicts);
        ClearPrioritisation(tx.GetHash());
    }
    // After the txs in the new block have been removed from the mempool, update policy estimates
    minerPolicyEstimator->processBlock(nBlockHeight, entries, fCurrentEstimate);
}

/**
 * Called whenever the tip changes. Removes transactions which don't commit to
 * the given branch ID from the mempool.
 */
void CTxMemPool::removeWithoutBranchId(uint32_t nMemPoolBranchId)
{
    LOCK(cs);
    std::list<CTransaction> transactionsToRemove;

    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        const CTransaction& tx = it->GetTx();
        if (it->GetValidatedBranchId() != nMemPoolBranchId) {
            transactionsToRemove.push_back(tx);
        }
    }

    for (const CTransaction& tx : transactionsToRemove) {
        std::list<CTransaction> removed;
        remove(tx, removed, true);
    }
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    totalTxSize = 0;
    cachedInnerUsage = 0;
    ++nTransactionsUpdated;
}

void CTxMemPool::check(const CCoinsViewCache *pcoins) const
{
    if (nCheckFrequency == 0)
        return;

    if (insecure_rand() >= nCheckFrequency)
        return;

    LogPrint("mempool", "Checking mempool with %u transactions and %u inputs\n", (unsigned int)mapTx.size(), (unsigned int)mapNextTx.size());

    uint64_t checkTotal = 0;
    uint64_t innerUsage = 0;

    CCoinsViewCache mempoolDuplicate(const_cast<CCoinsViewCache*>(pcoins));
    const int64_t nSpendHeight = GetSpendHeight(mempoolDuplicate);

    LOCK(cs);
    list<const CTxMemPoolEntry*> waitingOnDependants;
    for (indexed_transaction_set::const_iterator it = mapTx.begin(); it != mapTx.end(); it++) {
        unsigned int i = 0;
        checkTotal += it->GetTxSize();
        innerUsage += it->DynamicMemoryUsage();
        const CTransaction& tx = it->GetTx();
        bool fDependsWait = false;
        BOOST_FOREACH(const CTxIn &txin, tx.vin) {
            // Check that every mempool transaction's inputs refer to available coins, or other mempool tx's.
            indexed_transaction_set::const_iterator it2 = mapTx.find(txin.prevout.hash);
            if (it2 != mapTx.end()) {
                const CTransaction& tx2 = it2->GetTx();
                assert(tx2.vout.size() > txin.prevout.n && !tx2.vout[txin.prevout.n].IsNull());
                fDependsWait = true;
            } else {
                const CCoins* coins = pcoins->AccessCoins(txin.prevout.hash);
                assert(coins && coins->IsAvailable(txin.prevout.n));
            }
            // Check whether its inputs are marked in mapNextTx.
            std::map<COutPoint, CInPoint>::const_iterator it3 = mapNextTx.find(txin.prevout);
            assert(it3 != mapNextTx.end());
            assert(it3->second.ptx == &tx);
            assert(it3->second.n == i);
            i++;
        }

        boost::unordered_map<uint256, SproutMerkleTree, CCoinsKeyHasher> intermediates;

        BOOST_FOREACH(const JSDescription &joinsplit, tx.vjoinsplit) {
            BOOST_FOREACH(const uint256 &nf, joinsplit.nullifiers) {
                assert(!pcoins->GetNullifier(nf, SPROUT));
            }

            SproutMerkleTree tree;
            auto it = intermediates.find(joinsplit.anchor);
            if (it != intermediates.end()) {
                tree = it->second;
            } else {
                assert(pcoins->GetSproutAnchorAt(joinsplit.anchor, tree));
            }

            BOOST_FOREACH(const uint256& commitment, joinsplit.commitments)
            {
                tree.append(commitment);
            }

            intermediates.insert(std::make_pair(tree.root(), tree));
        }
        for (const SpendDescription &spendDescription : tx.vShieldedSpend) {
            SaplingMerkleTree tree;

            assert(pcoins->GetSaplingAnchorAt(spendDescription.anchor, tree));
            assert(!pcoins->GetNullifier(spendDescription.nullifier, SAPLING));
        }
        if (fDependsWait)
            waitingOnDependants.push_back(&(*it));
        else {
            CValidationState state;
            bool fCheckResult = tx.IsCoinBase() ||
                Consensus::CheckTxInputs(tx, state, mempoolDuplicate, nSpendHeight, Params().GetConsensus());
            assert(fCheckResult);
            UpdateCoins(tx, mempoolDuplicate, 1000000);
        }
    }
    unsigned int stepsSinceLastRemove = 0;
    while (!waitingOnDependants.empty()) {
        const CTxMemPoolEntry* entry = waitingOnDependants.front();
        waitingOnDependants.pop_front();
        CValidationState state;
        if (!mempoolDuplicate.HaveInputs(entry->GetTx())) {
            waitingOnDependants.push_back(entry);
            stepsSinceLastRemove++;
            assert(stepsSinceLastRemove < waitingOnDependants.size());
        } else {
            bool fCheckResult = entry->GetTx().IsCoinBase() ||
                Consensus::CheckTxInputs(entry->GetTx(), state, mempoolDuplicate, nSpendHeight, Params().GetConsensus());
            assert(fCheckResult);
            UpdateCoins(entry->GetTx(), mempoolDuplicate, 1000000);
            stepsSinceLastRemove = 0;
        }
    }
    for (std::map<COutPoint, CInPoint>::const_iterator it = mapNextTx.begin(); it != mapNextTx.end(); it++) {
        uint256 hash = it->second.ptx->GetHash();
        indexed_transaction_set::const_iterator it2 = mapTx.find(hash);
        const CTransaction& tx = it2->GetTx();
        assert(it2 != mapTx.end());
        assert(&tx == it->second.ptx);
        assert(tx.vin.size() > it->second.n);
        assert(it->first == it->second.ptx->vin[it->second.n].prevout);
    }

    checkNullifiers(SPROUT);
    checkNullifiers(SAPLING);

    assert(totalTxSize == checkTotal);
    assert(innerUsage == cachedInnerUsage);
}

void CTxMemPool::checkNullifiers(ShieldedType type) const
{
    const std::map<uint256, const CTransaction*>* mapToUse;
    switch (type) {
        case SPROUT:
            mapToUse = &mapSproutNullifiers;
            break;
        case SAPLING:
            mapToUse = &mapSaplingNullifiers;
            break;
        default:
            throw runtime_error("Unknown nullifier type");
    }
    for (const auto& entry : *mapToUse) {
        uint256 hash = entry.second->GetHash();
        CTxMemPool::indexed_transaction_set::const_iterator findTx = mapTx.find(hash);
        const CTransaction& tx = findTx->GetTx();
        assert(findTx != mapTx.end());
        assert(&tx == entry.second);
    }
}

void CTxMemPool::queryHashes(vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (indexed_transaction_set::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back(mi->GetTx().GetHash());
}

bool CTxMemPool::lookup(uint256 hash, CTransaction& result) const
{
    LOCK(cs);
    indexed_transaction_set::const_iterator i = mapTx.find(hash);
    if (i == mapTx.end()) return false;
    result = i->GetTx();
    return true;
}

CFeeRate CTxMemPool::estimateFee(int nBlocks) const
{
    LOCK(cs);
    return minerPolicyEstimator->estimateFee(nBlocks);
}
double CTxMemPool::estimatePriority(int nBlocks) const
{
    LOCK(cs);
    return minerPolicyEstimator->estimatePriority(nBlocks);
}

bool
CTxMemPool::WriteFeeEstimates(CAutoFile& fileout) const
{
    try {
        LOCK(cs);
        fileout << 109900; // version required to read: 0.10.99 or later
        fileout << CLIENT_VERSION; // version that wrote the file
        minerPolicyEstimator->Write(fileout);
    }
    catch (const std::exception&) {
        LogPrintf("CTxMemPool::WriteFeeEstimates(): unable to write policy estimator data (non-fatal)\n");
        return false;
    }
    return true;
}

bool
CTxMemPool::ReadFeeEstimates(CAutoFile& filein)
{
    try {
        int nVersionRequired, nVersionThatWrote;
        filein >> nVersionRequired >> nVersionThatWrote;
        if (nVersionRequired > CLIENT_VERSION)
            return error("CTxMemPool::ReadFeeEstimates(): up-version (%d) fee estimate file", nVersionRequired);

        LOCK(cs);
        minerPolicyEstimator->Read(filein);
    }
    catch (const std::exception&) {
        LogPrintf("CTxMemPool::ReadFeeEstimates(): unable to read policy estimator data (non-fatal)\n");
        return false;
    }
    return true;
}

void CTxMemPool::PrioritiseTransaction(const uint256 hash, const string strHash, double dPriorityDelta, const CAmount& nFeeDelta)
{
    {
        LOCK(cs);
        std::pair<double, CAmount> &deltas = mapDeltas[hash];
        deltas.first += dPriorityDelta;
        deltas.second += nFeeDelta;
    }
    LogPrintf("PrioritiseTransaction: %s priority += %f, fee += %d\n", strHash, dPriorityDelta, FormatMoney(nFeeDelta));
}

void CTxMemPool::ApplyDeltas(const uint256 hash, double &dPriorityDelta, CAmount &nFeeDelta)
{
    LOCK(cs);
    std::map<uint256, std::pair<double, CAmount> >::iterator pos = mapDeltas.find(hash);
    if (pos == mapDeltas.end())
        return;
    const std::pair<double, CAmount> &deltas = pos->second;
    dPriorityDelta += deltas.first;
    nFeeDelta += deltas.second;
}

void CTxMemPool::ClearPrioritisation(const uint256 hash)
{
    LOCK(cs);
    mapDeltas.erase(hash);
}

bool CTxMemPool::HasNoInputsOf(const CTransaction &tx) const
{
    for (unsigned int i = 0; i < tx.vin.size(); i++)
        if (exists(tx.vin[i].prevout.hash))
            return false;
    return true;
}

bool CTxMemPool::nullifierExists(const uint256& nullifier, ShieldedType type) const
{
    switch (type) {
        case SPROUT:
            return mapSproutNullifiers.count(nullifier);
        case SAPLING:
            return mapSaplingNullifiers.count(nullifier);
        default:
            throw runtime_error("Unknown nullifier type");
    }
}

void CTxMemPool::NotifyRecentlyAdded()
{
    uint64_t recentlyAddedSequence;
    std::vector<CTransaction> txs;
    {
        LOCK(cs);
        recentlyAddedSequence = nRecentlyAddedSequence;
        for (const auto& kv : mapRecentlyAddedTx) {
            txs.push_back(*(kv.second));
        }
        mapRecentlyAddedTx.clear();
    }

    // A race condition can occur here between these SyncWithWallets calls, and
    // the ones triggered by block logic (in ConnectTip and DisconnectTip). It
    // is harmless because calling SyncWithWallets(_, NULL) does not alter the
    // wallet transaction's block information.
    for (auto tx : txs) {
        try {
            SyncWithWallets(tx, NULL);
        } catch (const boost::thread_interrupted&) {
            throw;
        } catch (const std::exception& e) {
            PrintExceptionContinue(&e, "CTxMemPool::NotifyRecentlyAdded()");
        } catch (...) {
            PrintExceptionContinue(NULL, "CTxMemPool::NotifyRecentlyAdded()");
        }
    }

    // Update the notified sequence number. We only need this in regtest mode,
    // and should not lock on cs after calling SyncWithWallets otherwise.
    if (Params().NetworkIDString() == "regtest") {
        LOCK(cs);
        nNotifiedSequence = recentlyAddedSequence;
    }
}

bool CTxMemPool::IsFullyNotified() {
    assert(Params().NetworkIDString() == "regtest");
    LOCK(cs);
    return nRecentlyAddedSequence == nNotifiedSequence;
}

CCoinsViewMemPool::CCoinsViewMemPool(CCoinsView *baseIn, CTxMemPool &mempoolIn) : CCoinsViewBacked(baseIn), mempool(mempoolIn) { }

bool CCoinsViewMemPool::GetNullifier(const uint256 &nf, ShieldedType type) const
{
    return mempool.nullifierExists(nf, type) || base->GetNullifier(nf, type);
}

bool CCoinsViewMemPool::GetCoins(const uint256 &txid, CCoins &coins) const {
    // If an entry in the mempool exists, always return that one, as it's guaranteed to never
    // conflict with the underlying cache, and it cannot have pruned entries (as it contains full)
    // transactions. First checking the underlying cache risks returning a pruned entry instead.
    CTransaction tx;
    if (mempool.lookup(txid, tx)) {
        coins = CCoins(tx, MEMPOOL_HEIGHT);
        return true;
    }
    return (base->GetCoins(txid, coins) && !coins.IsPruned());
}

bool CCoinsViewMemPool::HaveCoins(const uint256 &txid) const {
    return mempool.exists(txid) || base->HaveCoins(txid);
}

size_t CTxMemPool::DynamicMemoryUsage() const {
    LOCK(cs);
    // Estimate the overhead of mapTx to be 6 pointers + an allocation, as no exact formula for boost::multi_index_contained is implemented.
    return memusage::MallocUsage(sizeof(CTxMemPoolEntry) + 6 * sizeof(void*)) * mapTx.size() + memusage::DynamicUsage(mapNextTx) + memusage::DynamicUsage(mapDeltas) + cachedInnerUsage;
}
