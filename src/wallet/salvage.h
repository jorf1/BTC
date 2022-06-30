// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Widecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef WIDECOIN_WALLET_SALVAGE_H
#define WIDECOIN_WALLET_SALVAGE_H

#include <fs.h>
#include <streams.h>

struct bilingual_str;

namespace wallet {
bool RecoverDatabaseFile(const fs::path& file_path, bilingual_str& error, std::vector<bilingual_str>& warnings);
} // namespace wallet

#endif // WIDECOIN_WALLET_SALVAGE_H
