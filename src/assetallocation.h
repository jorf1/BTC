// Copyright (c) 2015-2017 The Syscoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ASSETALLOCATION_H
#define ASSETALLOCATION_H

#include "rpc/server.h"
#include "dbwrapper.h"
#include "feedback.h"
#include "primitives/transaction.h"
#include "ranges.h"
#include <unordered_map>
#include "graph.h"
class CWalletTx;
class CTransaction;
class CReserveKey;
class CCoinsViewCache;
class CBlock;
class CAliasIndex;
class CAsset;

bool DecodeAssetAllocationTx(const CTransaction& tx, int& op, std::vector<std::vector<unsigned char> >& vvch);
bool DecodeAndParseAssetAllocationTx(const CTransaction& tx, int& op, std::vector<std::vector<unsigned char> >& vvch, char& type);
bool DecodeAssetAllocationScript(const CScript& script, int& op, std::vector<std::vector<unsigned char> > &vvch);
bool IsAssetAllocationOp(int op);
void AssetAllocationTxToJSON(const int op, const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash, UniValue &entry);
std::string assetAllocationFromOp(int op);
bool RemoveAssetAllocationScriptPrefix(const CScript& scriptIn, CScript& scriptOut);
class CAssetAllocationTuple {
public:
	std::vector<unsigned char> vchAsset;
	std::vector<unsigned char> vchAlias;

	ADD_SERIALIZE_METHODS;

	template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action) {
		READWRITE(vchAsset);
		READWRITE(vchAlias);
	}

	CAssetAllocationTuple(const std::vector<unsigned char> &asset, const std::vector<unsigned char> &alias) {
		vchAsset = asset;
		vchAlias = alias;
	}

	CAssetAllocationTuple() {
		SetNull();
	}
	inline CAssetAllocationTuple operator=(const CAssetAllocationTuple& other) {
		this->vchAsset = other.vchAsset;
		this->vchAlias = other.vchAlias;
		return *this;
	}
	inline bool operator==(const CAssetAllocationTuple& other) const {
		return this->vchAsset == other.vchAsset && this->vchAlias == other.vchAlias;
	}
	inline bool operator!=(const CAssetAllocationTuple& other) const {
		return (this->vchAsset != other.vchAsset || this->vchAlias != other.vchAlias);
	}
	inline bool operator< (const CAssetAllocationTuple& right) const
	{
		return ToString() < right.ToString();
	}
	inline void SetNull() {
		vchAsset.clear();
		vchAlias.clear();
	}
	std::string ToString() const;
	inline bool IsNull() {
		return (vchAsset.empty() && vchAlias.empty());
	}
};
typedef std::pair<std::vector<unsigned char>, std::vector<CRange> > InputRanges;
typedef std::vector<InputRanges> RangeInputArrayTuples;
typedef std::vector<std::pair<std::vector<unsigned char>, CAmount > > RangeAmountTuples;
typedef std::map<uint256, int64_t> ArrivalTimesMap;
typedef std::map<std::string, std::string> AssetAllocationIndexItem;
typedef std::map<int, AssetAllocationIndexItem> AssetAllocationIndexItemMap;
extern AssetAllocationIndexItemMap AssetAllocationIndex;
static const int ZDAG_MINIMUM_LATENCY_SECONDS = 10;
static const int MAX_MEMO_LENGTH = 128;
static const int ONE_YEAR_IN_BLOCKS = 525600;
static const int ONE_HOUR_IN_BLOCKS = 60;
static const int ONE_MONTH_IN_BLOCKS = 43800;
static sorted_vector<CAssetAllocationTuple> assetAllocationConflicts;
static CCriticalSection cs_assetallocation;
static CCriticalSection cs_assetallocationindex;
enum {
	ZDAG_NOT_FOUND = -1,
	ZDAG_STATUS_OK = 0,
	ZDAG_MINOR_CONFLICT_OK,
	ZDAG_MAJOR_CONFLICT_OK
};

class CAssetAllocation {
public:
	std::vector<unsigned char> vchAsset;
	std::vector<unsigned char> vchAlias;
	uint256 txHash;
	unsigned int nHeight;
	unsigned int nLastInterestClaimHeight;
	// if allocations are tracked by individual inputs
	std::vector<CRange> listAllocationInputs;
	RangeInputArrayTuples listSendingAllocationInputs;
	RangeAmountTuples listSendingAllocationAmounts;
	CAmount nBalance;
	uint64_t nAccumulatedBalanceSinceLastInterestClaim;
	float fAccumulatedInterestSinceLastInterestClaim;
	float fInterestRate;
	std::vector<unsigned char> vchMemo;
	CAssetAllocation() {
		SetNull();
	}
	CAssetAllocation(const CTransaction &tx) {
		SetNull();
		UnserializeFromTx(tx);
	}

	ADD_SERIALIZE_METHODS;
	template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action) {
		READWRITE(vchAsset);
		READWRITE(vchAlias);
		READWRITE(txHash);
		READWRITE(VARINT(nHeight));
		READWRITE(VARINT(nLastInterestClaimHeight));
		READWRITE(listAllocationInputs);
		READWRITE(listSendingAllocationInputs);
		READWRITE(listSendingAllocationAmounts);
		READWRITE(nBalance);
		READWRITE(VARINT(nAccumulatedBalanceSinceLastInterestClaim));
		READWRITE(fAccumulatedInterestSinceLastInterestClaim);
		READWRITE(fInterestRate);
		READWRITE(vchMemo);
	}
	inline friend bool operator==(const CAssetAllocation &a, const CAssetAllocation &b) {
		return (a.vchAsset == b.vchAsset && a.vchAlias == b.vchAlias
			);
	}

	inline CAssetAllocation operator=(const CAssetAllocation &b) {
		vchAsset = b.vchAsset;
		txHash = b.txHash;
		nHeight = b.nHeight;
		nLastInterestClaimHeight = b.nLastInterestClaimHeight;
		vchAlias = b.vchAlias;
		listAllocationInputs = b.listAllocationInputs;
		listSendingAllocationInputs = b.listSendingAllocationInputs;
		listSendingAllocationAmounts = b.listSendingAllocationAmounts;
		nBalance = b.nBalance;
		nAccumulatedBalanceSinceLastInterestClaim = b.nAccumulatedBalanceSinceLastInterestClaim;
		fAccumulatedInterestSinceLastInterestClaim = b.fAccumulatedInterestSinceLastInterestClaim;
		vchMemo = b.vchMemo;
		fInterestRate = b.fInterestRate;
		return *this;
	}

	inline friend bool operator!=(const CAssetAllocation &a, const CAssetAllocation &b) {
		return !(a == b);
	}
	inline void SetNull() { fInterestRate = 0; fAccumulatedInterestSinceLastInterestClaim = 0; nAccumulatedBalanceSinceLastInterestClaim = 0; vchMemo.clear(); nLastInterestClaimHeight = 0; nBalance = 0; listSendingAllocationAmounts.clear();  listSendingAllocationInputs.clear(); listAllocationInputs.clear(); vchAsset.clear(); nHeight = 0; txHash.SetNull(); vchAlias.clear(); }
	inline bool IsNull() const { return (vchAsset.empty()); }
	bool UnserializeFromTx(const CTransaction &tx);
	bool UnserializeFromData(const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash);
	void Serialize(std::vector<unsigned char>& vchData);
};


class CAssetAllocationDB : public CDBWrapper {
public:
	CAssetAllocationDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "assetallocations", nCacheSize, fMemory, fWipe, false, true) {}

    bool WriteAssetAllocation(const CAssetAllocation& assetallocation, const CAmount& nSenderBalance, const CAmount& nAmount, const CAsset& asset, const int64_t& arrivalTime, const std::vector<unsigned char>& vchSender, const std::vector<unsigned char>& vchReceiver, const bool& fJustCheck) {
		const CAssetAllocationTuple allocationTuple(assetallocation.vchAsset, assetallocation.vchAlias);
		bool writeState = false;
		{
			LOCK(cs_assetallocation);
			writeState = Write(make_pair(std::string("assetallocationi"), allocationTuple), assetallocation);
			if (!fJustCheck)
				writeState = writeState && Write(make_pair(std::string("assetallocationp"), allocationTuple), assetallocation);
			else if (fJustCheck) {
				if (arrivalTime < INT64_MAX) {
					ArrivalTimesMap arrivalTimes;
					ReadISArrivalTimes(allocationTuple, arrivalTimes);
					arrivalTimes[assetallocation.txHash] = arrivalTime;
					writeState = writeState && Write(make_pair(std::string("assetallocationa"), allocationTuple), arrivalTimes);
				}
			}
		}
		if(writeState && !vchReceiver.empty())
			WriteAssetAllocationIndex(assetallocation, asset, nSenderBalance, nAmount, vchSender, vchReceiver);
        return writeState;
    }
	bool EraseAssetAllocation(const CAssetAllocationTuple& assetAllocationTuple, bool cleanup = false) {
		LOCK(cs_assetallocation);
		bool eraseState = Erase(make_pair(std::string("assetallocationi"), assetAllocationTuple));
		if (eraseState) {
			Erase(make_pair(std::string("assetp"), assetAllocationTuple));
			EraseISArrivalTimes(assetAllocationTuple);
		}
		return eraseState;
	}
    bool ReadAssetAllocation(const CAssetAllocationTuple& assetAllocationTuple, CAssetAllocation& assetallocation) {
		LOCK(cs_assetallocation);
        return Read(make_pair(std::string("assetallocationi"), assetAllocationTuple), assetallocation);
    }
	bool ReadLastAssetAllocation(const CAssetAllocationTuple& assetAllocationTuple, CAssetAllocation& assetallocation) {
		LOCK(cs_assetallocation);
		return Read(make_pair(std::string("assetallocationp"), assetAllocationTuple), assetallocation);
	}
	bool ReadISArrivalTimes(const CAssetAllocationTuple& assetAllocationTuple, ArrivalTimesMap& arrivalTimes) {
		LOCK(cs_assetallocation);
		return Read(make_pair(std::string("assetallocationa"), assetAllocationTuple), arrivalTimes);
	}
	bool EraseISArrivalTime(const CAssetAllocationTuple& assetAllocationTuple, const uint256& txid) {
		LOCK(cs_assetallocation);
		ArrivalTimesMap arrivalTimes;
		ReadISArrivalTimes(assetAllocationTuple, arrivalTimes);
		ArrivalTimesMap::const_iterator it = arrivalTimes.find(txid);
		if (it != arrivalTimes.end())
			arrivalTimes.erase(it);
		if (arrivalTimes.size() > 0)
			return Write(make_pair(std::string("assetallocationa"), assetAllocationTuple), arrivalTimes);
		else
			return Erase(make_pair(std::string("assetallocationa"), assetAllocationTuple));
	}
	bool EraseISArrivalTimes(const CAssetAllocationTuple& assetAllocationTuple) {
		LOCK(cs_assetallocation);
		return Erase(make_pair(std::string("assetallocationa"), assetAllocationTuple));
	}
	void WriteAssetAllocationIndex(const CAssetAllocation& assetAllocationTuple, const CAsset& asset, const CAmount& nSenderBalance, const CAmount& nAmount, const std::vector<unsigned char>& vchSender, const std::vector<unsigned char>& vchReceiver);
	bool ScanAssetAllocations(const int count, const int from, const UniValue& oOptions, UniValue& oRes);
};
class CAssetAllocationTransactionsDB : public CDBWrapper {
public:
	CAssetAllocationTransactionsDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "assetallocationtransactions", nCacheSize, fMemory, fWipe, false, true) {
		ReadAssetAllocationWalletIndex(AssetAllocationIndex);
	}

	bool WriteAssetAllocationWalletIndex(const AssetAllocationIndexItemMap &valueMap) {
		LOCK(cs_assetallocationindex);
		return Write(std::string("assetallocationtxi"), valueMap, true);
	}
	bool ReadAssetAllocationWalletIndex(AssetAllocationIndexItemMap &valueMap) {
		LOCK(cs_assetallocationindex);
		return Read(std::string("assetallocationtxi"), valueMap);
	}
	bool ScanAssetAllocationIndex(const int count, const int from, const UniValue& oOptions, UniValue& oRes);
};
bool CheckAssetAllocationInputs(const CTransaction &tx, int op, const std::vector<std::vector<unsigned char> > &vvchArgs, const std::vector<unsigned char> &vvchAlias, bool fJustCheck, int nHeight, sorted_vector<CAssetAllocationTuple> &revertedAssetAllocations, std::string &errorMessage, bool bSanityCheck = false);
bool GetAssetAllocation(const CAssetAllocationTuple& assetAllocationTuple,CAssetAllocation& txPos);
bool BuildAssetAllocationJson(CAssetAllocation& assetallocation, const CAsset& asset, const bool bGetInputs, UniValue& oName);
bool BuildAssetAllocationIndexerJson(const CAssetAllocation& assetallocation, const CAsset& asset, const CAmount& nSenderBalance, const CAmount& nAmount, const std::vector<unsigned char>& strSender, const std::vector<unsigned char>& strReceiver, bool &isMine, UniValue& oAssetAllocation);
bool AccumulateInterestSinceLastClaim(CAssetAllocation & assetAllocation, const int& nHeight);
#endif // ASSETALLOCATION_H
