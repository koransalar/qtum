// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The BlackCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include <pos.h>
#include <txdb.h>
#include <validation.h>
#include <arith_uint256.h>
#include <hash.h>
#include <timedata.h>
#include <chainparams.h>
#include <script/sign.h>
#include <consensus/consensus.h>
#include <util/signstr.h>
#include <qtum/qtumdelegation.h>

using namespace std;

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256();  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->nStakeModifier;
    return Hash(ss.begin(), ss.end());
}

// BlackCoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + blockFrom.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   blockFrom.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t blockFromTime, CAmount prevoutValue, const COutPoint& prevout, unsigned int nTimeBlock, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    if (nTimeBlock < blockFromTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = prevoutValue;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    targetProofOfStake = ArithToUint256(bnTarget);

    uint256 nStakeModifier = pindexPrev->nStakeModifier;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << blockFromTime << prevout.hash << prevout.n << nTimeBlock;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : check modifier=%s nTimeBlockFrom=%u nPrevout=%u nTimeBlock=%u hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            blockFromTime, prevout.n, nTimeBlock,
            hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget)
        return false;

    if (LogInstance().WillLogCategory(BCLog::COINSTAKE) && !fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : check modifier=%s nTimeBlockFrom=%u nPrevout=%u nTimeBlock=%u hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            blockFromTime, prevout.n, nTimeBlock,
            hashProofOfStake.ToString());
    }

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, CValidationState& state, const CTransaction& tx, unsigned int nBits, uint32_t nTimeBlock, const std::vector<unsigned char>& vchPoD, uint256& hashProofOfStake, uint256& targetProofOfStake, CCoinsViewCache& view)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target (nBits)
    const CTxIn& txin = tx.vin[0];

    Coin coinPrev;

    if(!view.GetCoin(txin.prevout, coinPrev)){
        return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-prevout-not-exist", strprintf("CheckProofOfStake() : Stake prevout does not exist %s", txin.prevout.hash.ToString()));
    }

    int nHeight = pindexPrev->nHeight + 1;
    if(nHeight - coinPrev.nHeight < COINBASE_MATURITY){
        return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-prevout-not-mature", strprintf("CheckProofOfStake() : Stake prevout is not mature, expecting %i and only matured to %i", COINBASE_MATURITY, nHeight - coinPrev.nHeight));
    }
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-prevout-not-loaded", strprintf("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));
    }

    bool checkDelegation = false;
    int nOfflineStakeHeight = Params().GetConsensus().nOfflineStakeHeight;
    if (nHeight >= nOfflineStakeHeight)
    {
        static QtumDelegation qtumDelegation;

        // Check if the delegation contract exist
        if(!qtumDelegation.ExistDelegationContract())
            return state.Invalid(ValidationInvalidReason::BLOCK_HEADER_REJECT, false, REJECT_INVALID, "stake-delegation-contract-not-exist", strprintf("CheckProofOfStake() : The delegation contract doesn't exist, block height %i", nOfflineStakeHeight)); // Internal error, delegation contract not exist

        // Get the delegation from the contract
        uint160 address = uint160(ExtractPublicKeyHash(coinPrev.out.scriptPubKey));
        Delegation delegation;
        if(!qtumDelegation.GetDelegation(address, delegation)) {
            return state.Invalid(ValidationInvalidReason::BLOCK_HEADER_REJECT, false, REJECT_INVALID, "stake-get-delegation-failed", strprintf("CheckProofOfStake() : Failed to get delegation from the delegation contract")); // Internal error, get delegation from the delegation contract
        }

        // Verify delegation received from the contract
        bool verifiedDelegation = qtumDelegation.VerifyDelegation(address, delegation);
        bool hasDelegationProof = vchPoD.size() > 0;

        // Check that if PoD is present then the delegation received from the contract can be verified
        if(hasDelegationProof && hasDelegationProof != verifiedDelegation) {
            return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-delegation-not-verified", strprintf("CheckProofOfStake() : Delegation for block at height %i cannot be verified", nHeight));
        }

        // Check that if PoD is not present then the delegation received from the contract is null
        if(!hasDelegationProof && !delegation.IsNull()) {
            return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-delegation-not-used", strprintf("CheckProofOfStake() : Delegation for block at height %i is present but not used to create the block", nHeight));
        }

        checkDelegation = hasDelegationProof;
        if(checkDelegation)
        {
            // Verify the transaction signature is from the staker
            CScript stakerPubKey = tx.vout[1].scriptPubKey;
            if (!VerifySignature(stakerPubKey, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
                return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-verify-signature-failed", strprintf("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

            // Check that the staker have the permission to use that coin to create the coinstake transaction
            uint160 staker = uint160(ExtractPublicKeyHash(stakerPubKey));
            if(!SignStr::VerifyMessage(CKeyID(address), staker.GetReverseHex(), vchPoD))
                return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-verify-delegation-failed", strprintf("CheckProofOfStake() : VerifyDelegation failed on coinstake %s", tx.GetHash().ToString()));

            // Check that the block delegation data is the same as the data received from the contract, this is to avoid using old/removed delegation
            int fee = GetDelegationFeeTx(tx, coinPrev);
            if(delegation.staker != staker ||
                    (int)delegation.fee != fee ||
                    (int)delegation.blockHeight > nHeight ||
                    delegation.PoD != vchPoD) {
                return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-delegation-not-match", strprintf("CheckProofOfStake() : Delegation for block at height %i is not the same with the delegation received from the delegation contract", nHeight));
            }
        }
    }

    if(!checkDelegation)
    {
        // Verify signature
        if (!VerifySignature(coinPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
            return state.Invalid(ValidationInvalidReason::BLOCK_INVALID_HEADER, false, REJECT_INVALID, "stake-verify-signature-failed", strprintf("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));
    }

    if (!CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, txin.prevout, nTimeBlock, hashProofOfStake, targetProofOfStake, LogInstance().WillLogCategory(BCLog::COINSTAKE)))
        return state.Invalid(ValidationInvalidReason::BLOCK_HEADER_SYNC, false, REJECT_INVALID, "stake-check-kernel-failed", strprintf("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(uint32_t nTimeBlock)
{
    return (nTimeBlock & STAKE_TIMESTAMP_MASK) == 0;
}

bool CheckBlockInputPubKeyMatchesOutputPubKey(const CBlock& block, CCoinsViewCache& view) {
    Coin coinIn;
    if(!view.GetCoin(block.prevoutStake, coinIn)) {
        return error("%s: Could not fetch prevoutStake from UTXO set", __func__);
    }

    uint hasDelegation = block.HasProofOfDelegation() ? 1 : 0;
    CTransactionRef coinstakeTx = block.vtx[1];
    if(coinstakeTx->vout.size() < 2 + hasDelegation) {
        return error("%s: coinstake transaction does not have the minimum number of outputs", __func__);
    }

    const CTxOut& txout = coinstakeTx->vout[1 + hasDelegation];

    if(coinIn.out.scriptPubKey == txout.scriptPubKey) {
        return true;
    }

    // If the input does not exactly match the output, it MUST be on P2PKH spent and P2PK out.
    CTxDestination inputAddress;
    txnouttype inputTxType=TX_NONSTANDARD;
    if(!ExtractDestination(coinIn.out.scriptPubKey, inputAddress, &inputTxType)) {
        return error("%s: Could not extract address from input", __func__);
    }

    if(inputTxType != TX_PUBKEYHASH || inputAddress.type() != typeid(PKHash)) {
        return error("%s: non-exact match input must be P2PKH", __func__);
    }

    CTxDestination outputAddress;
    txnouttype outputTxType=TX_NONSTANDARD;
    if(!ExtractDestination(txout.scriptPubKey, outputAddress, &outputTxType)) {
        return error("%s: Could not extract address from output", __func__);
    }

    if(outputTxType != TX_PUBKEY || outputAddress.type() != typeid(PKHash)) {
        return error("%s: non-exact match output must be P2PK", __func__);
    }

    if(boost::get<PKHash>(inputAddress) != boost::get<PKHash>(outputAddress)) {
        return error("%s: input P2PKH pubkey does not match output P2PK pubkey", __func__);
    }

    return true;
}

bool CheckRecoveredPubKeyFromBlockSignature(CBlockIndex* pindexPrev, const CBlockHeader& block, CCoinsViewCache& view) {
    Coin coinPrev;
    if(!view.GetCoin(block.prevoutStake, coinPrev)){
        if(!GetSpentCoinFromMainChain(pindexPrev, block.prevoutStake, &coinPrev)) {
            return error("CheckRecoveredPubKeyFromBlockSignature(): Could not find %s and it was not at the tip", block.prevoutStake.hash.GetHex());
        }
    }

    uint256 hash = block.GetHashWithoutSign();
    CPubKey pubkey;
    std::vector<unsigned char> vchBlockSig = block.GetBlockSignature();
    std::vector<unsigned char> vchPoD = block.GetProofOfDelegation();
    bool hasDelegation = block.HasProofOfDelegation();

    if(vchBlockSig.empty()) {
        return error("CheckRecoveredPubKeyFromBlockSignature(): Signature is empty\n");
    }

    // Recover the public key
    if (pindexPrev->nHeight + 1 >= Params().GetConsensus().nOfflineStakeHeight)
    {
        // Recover the public key from compact signature
        if(hasDelegation)
        {
            // Has delegation
            CTxDestination address;
            txnouttype txType=TX_NONSTANDARD;
            if(pubkey.RecoverCompact(hash, vchBlockSig) &&
                    ExtractDestination(coinPrev.out.scriptPubKey, address, &txType)){
                if ((txType == TX_PUBKEY || txType == TX_PUBKEYHASH) && address.type() == typeid(PKHash)) {
                    if(SignStr::VerifyMessage(CKeyID(boost::get<PKHash>(address)), pubkey.GetID().GetReverseHex(), vchPoD)) {
                        return true;
                    }
                }
            }
        }
        else
        {
            // No delegation
            CTxDestination address;
            txnouttype txType=TX_NONSTANDARD;
            if(pubkey.RecoverCompact(hash, vchBlockSig) &&
                    ExtractDestination(coinPrev.out.scriptPubKey, address, &txType)){
                if ((txType == TX_PUBKEY || txType == TX_PUBKEYHASH) && address.type() == typeid(PKHash)) {
                    if(pubkey.GetID() == boost::get<PKHash>(address)) {
                        return true;
                    }
                }
            }
        }
    }
    else
    {
        // Recover the public key from LowS signature
        for(uint8_t recid = 0; recid <= 3; ++recid) {
            for(uint8_t compressed = 0; compressed < 2; ++compressed) {
                if(!pubkey.RecoverLaxDER(hash, vchBlockSig, recid, compressed)) {
                    continue;
                }

                CTxDestination address;
                txnouttype txType=TX_NONSTANDARD;
                if(ExtractDestination(coinPrev.out.scriptPubKey, address, &txType)){
                    if ((txType == TX_PUBKEY || txType == TX_PUBKEYHASH) && address.type() == typeid(PKHash)) {
                        if(pubkey.GetID() == boost::get<PKHash>(address)) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view)
{
    std::map<COutPoint, CStakeCache> tmp;
    return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view, tmp);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view, const std::map<COutPoint, CStakeCache>& cache)
{
    uint256 hashProofOfStake, targetProofOfStake;
    auto it=cache.find(prevout);
    if(it == cache.end()) {
        //not found in cache (shouldn't happen during staking, only during verification which does not use cache)
        Coin coinPrev;
        if(!view.GetCoin(prevout, coinPrev)){
            if(!GetSpentCoinFromMainChain(pindexPrev, prevout, &coinPrev)) {
                return error("CheckKernel(): Could not find coin and it was not at the tip");
            }
        }

        if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY){
            return error("CheckKernel(): Coin not matured");
        }
        CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
        if(!blockFrom) {
            return error("CheckKernel(): Could not find block");
        }
        if(coinPrev.IsSpent()){
            return error("CheckKernel(): Coin is spent");
        }

        return CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, prevout,
                                    nTimeBlock, hashProofOfStake, targetProofOfStake);
    }else{
        //found in cache
        const CStakeCache& stake = it->second;
        if(CheckStakeKernelHash(pindexPrev, nBits, stake.blockFromTime, stake.amount, prevout,
                                    nTimeBlock, hashProofOfStake, targetProofOfStake)){
            //Cache could potentially cause false positive stakes in the event of deep reorgs, so check without cache also
            return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view);
        }
    }
    return false;
}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view){
    if(cache.find(prevout) != cache.end()){
        //already in cache
        return;
    }

    Coin coinPrev;
    if(!view.GetCoin(prevout, coinPrev)){
        return;
    }

    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY){
        return;
    }
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return;
    }

    CStakeCache c(blockFrom->nTime, coinPrev.out.nValue);
    cache.insert({prevout, c});
}

/**
 * Proof-of-stake functions needed in the wallet but wallet independent
 */
struct BlockScript{
    CScript stakerScript;
    CScript delegateScript;
    uint8_t fee;
    bool hasDelegate;

    BlockScript(const CScript& _stakerScript = CScript()):
        stakerScript(_stakerScript),
        fee(0),
        hasDelegate(false)
    {}
};

struct ScriptsElement{
    BlockScript script;
    uint256 hash;
};

/**
 * Cache of the recent mpos scripts for the block reward recipients
 * The max size of the map is 2 * nCacheScripts - nMPoSRewardRecipients, so in this case it is 20
 */
std::map<int, ScriptsElement> scriptsMap;

unsigned int GetStakeMaxCombineInputs() { return 100; }

int64_t GetStakeCombineThreshold() { return 100 * COIN; }

unsigned int GetStakeSplitOutputs() { return 2; }

int64_t GetStakeSplitThreshold() { return GetStakeSplitOutputs() * GetStakeCombineThreshold(); }

bool SplitOfflineStakeReward(const int64_t& nReward, const uint8_t& fee, int64_t& nRewardOffline, int64_t& nRewardStaker)
{
    if(fee > 100) return false;
    double percentage = fee / 100.0;
    nRewardStaker = percentage * nReward;
    nRewardOffline = nReward - nRewardStaker;
    return true;
}

int GetDelegationFeeTx(const CTransaction& tx, const Coin& coin)
{
    CAmount nValueCoin = coin.out.nValue;
    if(!tx.IsCoinStake() || tx.vout.size() < 3 || nValueCoin <= 0)
        return -1;

    CAmount nValueStaker = tx.vout[1].nValue;
    CAmount nValueDelegate = tx.vout[2].nValue;
    CAmount nReward = nValueStaker + nValueDelegate - nValueCoin;
    if(nReward < 0)
        return -1;

    return std::lround(nValueStaker * 100.0 / nReward);
}

bool NeedToEraseScriptFromCache(int nBlockHeight, int nCacheScripts, int nScriptHeight, const ScriptsElement& scriptElement)
{
    // Erase element from cache if not in range [nBlockHeight - nCacheScripts, nBlockHeight + nCacheScripts]
    if(nScriptHeight < (nBlockHeight - nCacheScripts) ||
            nScriptHeight > (nBlockHeight + nCacheScripts))
        return true;

    // Erase element from cache if hash different
    CBlockIndex* pblockindex = ChainActive()[nScriptHeight];
    if(pblockindex && pblockindex->GetBlockHash() != scriptElement.hash)
        return true;

    return false;
}

void CleanScriptCache(int nHeight, const Consensus::Params& consensusParams)
{
    int nCacheScripts = consensusParams.nMPoSRewardRecipients * 1.5;

    // Remove the scripts from cache that are not used
    for (std::map<int, ScriptsElement>::iterator it=scriptsMap.begin(); it!=scriptsMap.end();){
        if(NeedToEraseScriptFromCache(nHeight, nCacheScripts, it->first, it->second))
        {
            it = scriptsMap.erase(it);
        }
        else{
            it++;
        }
    }
}

bool ReadFromScriptCache(BlockScript &script, CBlockIndex* pblockindex, int nHeight, const Consensus::Params& consensusParams)
{
    CleanScriptCache(nHeight, consensusParams);

    // Find the script in the cache
    std::map<int, ScriptsElement>::iterator it = scriptsMap.find(nHeight);
    if(it != scriptsMap.end())
    {
        if(it->second.hash == pblockindex->GetBlockHash())
        {
            script = it->second.script;
            return true;
        }
    }

    return false;
}

void AddToScriptCache(BlockScript script, CBlockIndex* pblockindex, int nHeight, const Consensus::Params& consensusParams)
{
    CleanScriptCache(nHeight, consensusParams);

    // Add the script into the cache
    ScriptsElement listElement;
    listElement.script = script;
    listElement.hash = pblockindex->GetBlockHash();
    scriptsMap.insert(std::pair<int, ScriptsElement>(nHeight, listElement));
}

bool AddMPoSScript(std::vector<BlockScript> &mposScriptList, int nHeight, const Consensus::Params& consensusParams)
{
    // Check if the block index exist into the active chain
    CBlockIndex* pblockindex = ChainActive()[nHeight];
    if(!pblockindex)
    {
        LogPrint(BCLog::COINSTAKE, "Block index not found\n");
        return false;
    }

    // Try find the script from the cache
    BlockScript blockScript;
    if(ReadFromScriptCache(blockScript, pblockindex, nHeight, consensusParams))
    {
        mposScriptList.push_back(blockScript);
        return true;
    }

    // Read the block
    uint160 stakeAddress;
    if(!pblocktree->ReadStakeIndex(nHeight, stakeAddress)){
        return false;
    }

    // The block reward for PoS is in the second transaction (coinstake) and the second or third output
    if(pblockindex->IsProofOfStake())
    {
        if(stakeAddress == uint160())
        {
            LogPrint(BCLog::COINSTAKE, "Fail to solve script for mpos reward recipient\n");
            //This should never fail, but in case it somehow did we don't want it to bring the network to a halt
            //So, use an OP_RETURN script to burn the coins for the unknown staker
            blockScript = CScript() << OP_RETURN;
        }else{
            // Make public key hash script
            blockScript = CScript() << OP_DUP << OP_HASH160 << ToByteVector(stakeAddress) << OP_EQUALVERIFY << OP_CHECKSIG;
        }

        if(pblockindex->HasProofOfDelegation())
        {
            uint160 delegateAddress;
            uint8_t fee;
            if(!pblocktree->ReadDelegateIndex(nHeight, delegateAddress, fee)){
                return false;
            }

            if(delegateAddress == uint160())
            {
                LogPrint(BCLog::COINSTAKE, "Fail to solve script for mpos delegate reward recipient\n");
                blockScript.delegateScript = CScript() << OP_RETURN;
            }else{
                // Make public key hash script
                blockScript.delegateScript = CScript() << OP_DUP << OP_HASH160 << ToByteVector(delegateAddress) << OP_EQUALVERIFY << OP_CHECKSIG;
            }

            blockScript.fee = fee;
            blockScript.hasDelegate = true;
        }

        // Add the script into the list
        mposScriptList.push_back(blockScript);

        // Update script cache
        AddToScriptCache(blockScript, pblockindex, nHeight, consensusParams);
    }
    else
    {
        if(Params().MineBlocksOnDemand()){
            //this could happen in regtest. Just ignore and add an empty script
            blockScript = CScript() << OP_RETURN;
            mposScriptList.push_back(blockScript);
            return true;

        }
        LogPrint(BCLog::COINSTAKE, "The block is not proof-of-stake\n");
        return false;
    }

    return true;
}

bool GetMPoSOutputScripts(std::vector<BlockScript>& mposScriptList, int nHeight, const Consensus::Params& consensusParams)
{
    bool ret = true;
    nHeight -= COINBASE_MATURITY;

    // Populate the list of scripts for the reward recipients
    for(int i = 0; (i < consensusParams.nMPoSRewardRecipients - 1) && ret; i++)
    {
        ret &= AddMPoSScript(mposScriptList, nHeight - i, consensusParams);
    }

    return ret;
}

bool GetMPoSOutputs(std::vector<CTxOut>& mposOutputList, int64_t nRewardPiece, int nHeight, const Consensus::Params& consensusParams)
{
    std::vector<BlockScript> mposScriptList;
    if(!GetMPoSOutputScripts(mposScriptList, nHeight, consensusParams))
    {
        LogPrint(BCLog::COINSTAKE, "Fail to get the list of recipients\n");
        return false;
    }

    // Create the outputs for the recipients
    for(unsigned int i = 0; i < mposScriptList.size(); i++)
    {
        BlockScript blockScript = mposScriptList[i];
        if(blockScript.hasDelegate)
        {
            int64_t nRewardDelegate, nRewardStaker;
            if(!SplitOfflineStakeReward(nRewardPiece, blockScript.fee, nRewardDelegate, nRewardStaker))
            {
                LogPrint(BCLog::COINSTAKE, "Fail to to split the offline staking reward\n");
                return false;
            }

            mposOutputList.push_back(CTxOut(nRewardStaker, blockScript.stakerScript));
            mposOutputList.push_back(CTxOut(nRewardDelegate, blockScript.delegateScript));
        }
        else
        {
            mposOutputList.push_back(CTxOut(nRewardPiece, blockScript.stakerScript));
        }
    }

    return true;
}

bool CreateMPoSOutputs(CMutableTransaction& txNew, int64_t nRewardPiece, int nHeight, const Consensus::Params& consensusParams)
{
    std::vector<CTxOut> mposOutputList;
    if(!GetMPoSOutputs(mposOutputList, nRewardPiece, nHeight, consensusParams))
    {
        return false;
    }

    // Split the block reward with the recipients
    for(unsigned int i = 0; i < mposOutputList.size(); i++)
    {
        txNew.vout.push_back(mposOutputList[i]);
    }

    return true;
}

