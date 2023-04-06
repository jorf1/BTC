#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tool to convert a compact-serialized UTXO set to a SQLite3 database.

The input UTXO set can be generated by Bitcoin Core with the `dumptxoutset` RPC:
$ bitcoin-cli dumptxoutset ~/utxos.dat

The created database contains a table `utxos` with the following schema:
(txid TEXT, vout INT, value INT, coinbase INT, height INT, scriptpubkey TEXT)
"""
import argparse
import os
import sqlite3
import sys
import time


UTXO_DUMP_MAGIC = b'utxo\xff'
UTXO_DUMP_VERSION = 2
NET_MAGIC_BYTES = {
    b"\xf9\xbe\xb4\xd9": "Mainnet",
    b"\x0a\x03\xcf\x40": "Signet",
    b"\x0b\x11\x09\x07": "Testnet3",
    b"\x1c\x16\x3f\x28": "Testnet4",
    b"\xfa\xbf\xb5\xda": "Regtest",
}


def read_varint(f):
    """Equivalent of `ReadVarInt()` (see serialization module)."""
    n = 0
    while True:
        dat = f.read(1)[0]
        n = (n << 7) | (dat & 0x7f)
        if (dat & 0x80) > 0:
            n += 1
        else:
            return n


def read_compactsize(f):
    """Equivalent of `ReadCompactSize()` (see serialization module)."""
    n = f.read(1)[0]
    if n == 253:
        n = int.from_bytes(f.read(2), "little")
    elif n == 254:
        n = int.from_bytes(f.read(4), "little")
    elif n == 255:
        n = int.from_bytes(f.read(8), "little")
    return n


def decompress_amount(x):
    """Equivalent of `DecompressAmount()` (see compressor module)."""
    if x == 0:
        return 0
    x -= 1
    e = x % 10
    x //= 10
    n = 0
    if e < 9:
        d = (x % 9) + 1
        x //= 9
        n = x * 10 + d
    else:
        n = x + 1
    while e > 0:
        n *= 10
        e -= 1
    return n


def decompress_script(f):
    """Equivalent of `DecompressScript()` (see compressor module)."""
    size = read_varint(f)  # sizes 0-5 encode compressed script types
    if size == 0:  # P2PKH
        return bytes([0x76, 0xa9, 20]) + f.read(20) + bytes([0x88, 0xac])
    elif size == 1:  # P2SH
        return bytes([0xa9, 20]) + f.read(20) + bytes([0x87])
    elif size in (2, 3):  # P2PK (compressed)
        return bytes([33, size]) + f.read(32) + bytes([0xac])
    elif size in (4, 5):  # P2PK (uncompressed)
        compressed_pubkey = bytes([size - 2]) + f.read(32)
        return bytes([65]) + decompress_pubkey(compressed_pubkey) + bytes([0xac])
    else:  # others (bare multisig, segwit etc.)
        size -= 6
        assert size <= 10000, f"too long script with size {size}"
        return f.read(size)


def decompress_pubkey(compressed_pubkey):
    """Decompress pubkey by calculating y = sqrt(x^3 + 7) % p
       (see functions `secp256k1_eckey_pubkey_parse` and `secp256k1_ge_set_xo_var`).
    """
    P = 2**256 - 2**32 - 977  # secp256k1 field size
    assert len(compressed_pubkey) == 33 and compressed_pubkey[0] in (2, 3)
    x = int.from_bytes(compressed_pubkey[1:], 'big')
    rhs = (x**3 + 7) % P
    y = pow(rhs, (P + 1)//4, P)  # get sqrt using Tonelli-Shanks algorithm (for p % 4 = 3)
    assert pow(y, 2, P) == rhs, f"pubkey is not on curve ({compressed_pubkey.hex()})"
    tag_is_odd = compressed_pubkey[0] == 3
    y_is_odd = (y & 1) == 1
    if tag_is_odd != y_is_odd:  # fix parity (even/odd) if necessary
        y = P - y
    return bytes([4]) + x.to_bytes(32, 'big') + y.to_bytes(32, 'big')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('infile', help='filename of compact-serialized UTXO set (input)')
    parser.add_argument('outfile', help='filename of created SQLite3 database (output)')
    parser.add_argument('-v', '--verbose', action='store_true', help='show details about each UTXO')
    args = parser.parse_args()

    if not os.path.exists(args.infile):
        print(f"Error: provided input file '{args.infile}' doesn't exist.")
        sys.exit(1)

    if os.path.exists(args.outfile):
        print(f"Error: provided output file '{args.outfile}' already exists.")
        sys.exit(1)

    # create database table
    con = sqlite3.connect(args.outfile)
    con.execute("CREATE TABLE utxos(txid TEXT, vout INT, value INT, coinbase INT, height INT, scriptpubkey TEXT)")

    # read metadata (magic bytes, version, network magic, block height, block hash, UTXO count)
    f = open(args.infile, 'rb')
    magic_bytes = f.read(5)
    version = int.from_bytes(f.read(2), 'little')
    network_magic = f.read(4)
    block_hash = f.read(32)
    num_utxos = int.from_bytes(f.read(8), 'little')
    if magic_bytes != UTXO_DUMP_MAGIC:
        print(f"Error: provided input file is not an UTXO dump.")
        sys.exit(1)
    if version != UTXO_DUMP_VERSION:
        print(f"Error: provided input file has unknown UTXO dump version {version} "
              f"(only version {UTXO_DUMP_VERSION} supported)")
        sys.exit(1)
    network_string = NET_MAGIC_BYTES.get(network_magic, f"unknown network ({network_magic.hex()})")
    print(f"UTXO Snapshot for {network_string} at block hash "
          f"{block_hash[::-1].hex()[:32]}..., contains {num_utxos} coins")

    start_time = time.time()
    write_batch = []
    coins_per_hash_left = 0
    prevout_hash = None
    max_height = 0

    for coin_idx in range(1, num_utxos+1):
        # read key (COutPoint)
        if coins_per_hash_left == 0:  # read next prevout hash
            prevout_hash = f.read(32)[::-1].hex()
            coins_per_hash_left = read_compactsize(f)
        prevout_index = read_compactsize(f)
        # read value (Coin)
        code = read_varint(f)
        height = code >> 1
        is_coinbase = code & 1
        amount = decompress_amount(read_varint(f))
        scriptpubkey = decompress_script(f).hex()
        write_batch.append((prevout_hash, prevout_index, amount, is_coinbase, height, scriptpubkey))
        if height > max_height:
            max_height = height
        coins_per_hash_left -= 1

        if args.verbose:
            print(f"Coin {coin_idx}/{num_utxos}:")
            print(f"    prevout = {prevout_hash}:{prevout_index}")
            print(f"    amount = {amount}, height = {height}, coinbase = {is_coinbase}")
            print(f"    scriptPubKey = {scriptpubkey}\n")

        if coin_idx % (16*1024) == 0 or coin_idx == num_utxos:
            # write utxo batch to database
            con.executemany("INSERT INTO utxos VALUES(?, ?, ?, ?, ?, ?)", write_batch)
            con.commit()
            write_batch.clear()

        if coin_idx % (1024*1024) == 0:
            elapsed = time.time() - start_time
            print(f"{coin_idx} coins converted [{coin_idx/num_utxos*100:.2f}%], " +
                  f"{elapsed:.3f}s passed since start")
    con.close()

    print(f"TOTAL: {num_utxos} coins written to {args.outfile}, snapshot height is {max_height}.")
    if f.read(1) != b'':  # EOF should be reached by now
        print(f"WARNING: input file {args.infile} has not reached EOF yet!")
        sys.exit(1)


if __name__ == '__main__':
    main()
