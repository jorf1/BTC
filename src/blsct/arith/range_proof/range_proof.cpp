// Copyright (c) 2022 The Navcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blsct/arith/range_proof/proof.h>
#include <blsct/arith/range_proof/range_proof.h>
#include <ctokens/tokenid.h>
#include <util/strencodings.h>
#include <tinyformat.h>

Scalar RangeProof::m_one;
Scalar RangeProof::m_two;
Scalars RangeProof::m_two_pows;

RangeProof::RangeProof()
{
    if (m_is_initialized) return;
    boost::lock_guard<boost::mutex> lock(RangeProof::m_init_mutex);

    MclInitializer::Init();
    G1Point::Init();

    //RangeProof::m_gens = Generators();
    RangeProof::m_one = Scalar(1);
    RangeProof::m_two = Scalar(2);
    RangeProof::m_two_pows = Scalars::FirstNPow(m_two, Config::m_input_value_bits);
    auto ones = Scalars::RepeatN(RangeProof::m_one, Config::m_input_value_bits);
    RangeProof::m_inner_prod_ones_and_two_pows = (ones * RangeProof::m_two_pows).Sum();

    m_is_initialized = true;
}

bool RangeProof::InnerProductArgument(
    const size_t input_value_vec_len,
    const Generators& gens,
    const Scalar& x_ip,
    const Scalars& l,
    const Scalars& r,
    const Scalar& y,
    Proof& proof,
    CHashWriter& transcript
) {
    // build initial state
    Scalars scale_factors = Scalars::FirstNPow(y.Invert(), input_value_vec_len);
    G1Points g_prime = gens.Gi;
    G1Points h_prime = gens.Hi;
    Scalars a_prime = l;
    Scalars b_prime = r;

    size_t n_prime = input_value_vec_len;  // # of rounds is log2 n_prime
    size_t round = 0;
    Scalars xs;

    while (n_prime > 1) {
        // (20)
        n_prime /= 2;

        // (21)-(22)
        Scalar cL = (a_prime.To(n_prime) * b_prime.From(n_prime)).Sum();
        Scalar cR = (a_prime.From(n_prime) * b_prime.To(n_prime)).Sum();

        // (23)-(24)
        Scalar extra_scalar_cL = cL * x_ip;
        proof.Ls.Add(
            (g_prime.From(n_prime) * a_prime.To(n_prime)).Sum() +
            (h_prime.To(n_prime) * (round == 0 ? b_prime * scale_factors.To(n_prime) : b_prime.From(n_prime))).Sum() +
            (gens.H * extra_scalar_cL)
        );
        Scalar extra_scalar_cR = cR * x_ip;
        proof.Rs.Add(
            (g_prime.To(n_prime) * a_prime.From(n_prime)).Sum() +
            (h_prime.From(n_prime) * (round == 0 ? b_prime * scale_factors.From(n_prime) : b_prime.To(n_prime))).Sum() +
            (gens.H * extra_scalar_cR)
        );

        // (25)-(27)
        transcript << proof.Ls[round];
        transcript << proof.Rs[round];

        Scalar x = transcript.GetHash();
        if (x == 0)
            return false;
        Scalar x_inv = x.Invert();
        xs.Add(x);

        // (29)-(31)
        if (n_prime > 1) {
            g_prime = (g_prime.To(n_prime) * x_inv) + (g_prime.From(n_prime) * x);

            // apply scale_factors to x and x_inv
            Scalars sf_ws = scale_factors * x;
            Scalars sf_w_invs = scale_factors * x_inv;
            h_prime = (h_prime.To(n_prime) * sf_ws) + (h_prime.From(n_prime) * sf_w_invs);
        }

        // (33)-(34)
        a_prime = (a_prime.To(n_prime) * x) + (a_prime.From(n_prime) * x_inv);
        b_prime = (b_prime.To(n_prime) * x_inv) + (b_prime.From(n_prime) * x);

        ++round;
    }

    proof.a = a_prime[0];
    proof.b = b_prime[0];

    return true;
}

size_t RangeProof::GetInnerProdArgRounds(const size_t& num_input_values) const
{
    const size_t num_input_values_power_of_2 = GetFirstPowerOf2GreaterOrEqTo(num_input_values);
    const size_t rounds = std::log2(num_input_values_power_of_2) + std::log2(Config::m_input_value_bits);
    return rounds;
}

Proof RangeProof::Prove(
    Scalars vs,
    G1Point nonce,
    const std::vector<uint8_t>& message,
    const TokenId& token_id
) {
    if (message.size() > Config::m_max_message_size) {
        throw std::runtime_error(strprintf("%s: message size is too large", __func__));
    }
    if (vs.Empty()) {
        throw std::runtime_error(strprintf("%s: value vector is empty", __func__));
    }
    if (vs.Size() > Config::m_max_input_values) {
        throw std::runtime_error(strprintf("%s: number of input values exceeds the maximum", __func__));
    }

    const size_t num_input_values_power_2 = GetFirstPowerOf2GreaterOrEqTo(vs.Size());
    const size_t concat_input_values_in_bits = num_input_values_power_2 * Config::m_input_value_bits;

    ////////////// Proving steps
    Proof proof;

    // Initialize gammas
    Scalars gammas;
    for (size_t i = 0; i < vs.Size(); ++i) {
        auto hash = nonce.GetHashWithSalt(100 + i);
        gammas.Add(hash);
    }

    // Get Generators for the token_id
    Generators gens = m_gf.GetInstance(token_id);

    // This hash is updated for Fiat-Shamir throughout the proof
    CHashWriter transcript(0, 0);

    // Calculate value commitments and add them to transcript
    proof.Vs = G1Points(gens.H * gammas.m_vec) + G1Points(gens.G.get() * vs.m_vec);
    for (size_t i = 0; i < vs.Size(); ++i) {
        transcript << proof.Vs[i];
    }

    // (41)-(42)
    // Values to be obfuscated are encoded in binary and flattened to a single vector aL
    Scalars aL;   // ** size of aL can be shorter than concat_input_values_in_bits
    for(Scalar& v: vs.m_vec) {  // for each input value
        auto bits = v.GetBits();  // gets the value in binary
        for(bool bit: bits) {
            aL.Add(bit);
        }
        // fill the remaining bits if needed
        for(size_t i = 0; i < Config::m_input_value_bits - bits.size(); ++i) {
            aL.Add(false);
        }
    }
    // TODO fill bits if aL.size < concat_input_values_in_bits

    auto one_value_concat_bits = Scalars::FirstNPow(m_one, concat_input_values_in_bits);

    // aR is aL - 1
    Scalars aR = aL - one_value_concat_bits;

    size_t num_tries = 0;

try_again:  // hasher is not cleared so that different hash will be obtained upon retry

    if (++num_tries > Config::m_max_prove_tries) {
        throw std::runtime_error(strprintf("%s: exceeded maxinum number of tries", __func__));
    }

    // (43)-(44)
    // Commitment to aL and aR (obfuscated with alpha)

    // trim message to first 23 bytes if needed
    Scalar msg1_scalar(
        message.size() > 23 ?
            std::vector<uint8_t>(message.begin(), message.begin() + 23) :
            message
    );
    // first part of message + 64-byte vs[0]
    Scalar msg1_v0 = (msg1_scalar << Config::m_input_value_bits) | vs[0];

    Scalar alpha = nonce.GetHashWithSalt(1);
    alpha = alpha + msg1_v0;

    // Using generator H for alpha following the paper
    proof.A = (gens.H * alpha) + (gens.Gi.get() * aL).Sum() + (gens.Hi.get() * aR).Sum();

    // (45)-(47)
    // Commitment to blinding vectors sL and sR (obfuscated with rho)
    auto sL = Scalars::RandVec(Config::m_input_value_bits, true);
    auto sR = Scalars::RandVec(Config::m_input_value_bits, true);

    auto rho = nonce.GetHashWithSalt(2);
    // Using generator H for alpha following the paper
    proof.S = (gens.H * rho) + (gens.Gi.get() * sL).Sum() + (gens.Hi.get() * sR).Sum();

    // (48)-(50)
    transcript << proof.A;
    transcript << proof.S;

    Scalar y = transcript.GetHash();
    if (y == 0)
        goto try_again;
    transcript << y;

    Scalar z = transcript.GetHash();
    if (z == 0)
        goto try_again;
    transcript << z;

    // Polynomial construction by coefficients
    // AFTER (50)

    // l(x) = (aL - z 1^n) + sL X
    // aL is a concatination of all input value bits, so mn bits (= input_value_total_bits) are needed
    Scalars z_value_total_bits = Scalars::FirstNPow(z, concat_input_values_in_bits);
    Scalars l0 = aL - z_value_total_bits;

    // l(1) is (aL - z 1^n) + sL, but this is reduced to sL
    Scalars l1 = sL;

    // Calculation of r(0) and r(1) on page 19
    Scalars z_n_times_two_n;
    Scalars z_pows = Scalars::FirstNPow(z, concat_input_values_in_bits, 2);  // z_pows excludes 1 and z

    // The last term of r(X) on page 19
    for (size_t i = 0; i < concat_input_values_in_bits; ++i) {
        auto base_z = z_pows[i];  // change base Scalar for each input value

        for (size_t bit_idx = 0; bit_idx < Config::m_input_value_bits; ++bit_idx) {
            z_n_times_two_n.Add(base_z * m_two_pows[bit_idx]);
        }
    }

    Scalars y_value_total_bits = Scalars::FirstNPow(y, concat_input_values_in_bits);
    Scalars r0 = (y_value_total_bits * (aR + z_value_total_bits)) + z_n_times_two_n;
    Scalars r1 = y_value_total_bits * sR;

    // Polynomial construction before (51)
    Scalar t1 = (l0 * r1).Sum() + (l1 * r0).Sum();
    Scalar t2 = (l1 * r1).Sum();

    // (52)-(53)
    Scalar tau1 = nonce.GetHashWithSalt(3);
    Scalar tau2 = nonce.GetHashWithSalt(4);

    // if message size is 24-byte or bigger, treat that part as msg2
    Scalar msg2_scalar = Scalar({
        message.size() > 23 ?
            std::vector<uint8_t>(message.begin() + 23, message.end()) :
            std::vector<uint8_t>()
    });
    tau1 = tau1 + msg2_scalar;

    proof.T1 = (gens.G.get() * t1) + (gens.H * tau1);
    proof.T2 = (gens.G.get() * t2) + (gens.H * tau2);

    // (54)-(56)
    transcript << proof.T1;
    transcript << proof.T2;

    Scalar x = transcript.GetHash();
    if (x == 0)
        goto try_again;
    // x will be added to transcript later

    // (58)-(59)
    Scalars l = l0 + (l1 * x);  // l0 = aL - z_mn; l1 = sL
    Scalars r = r0 + (r1 * x);  // r0 = RHS of (58) - r1; r1 = y_mn o (sR * x)

    // LHS of (60)
    proof.t_hat = (l * r).Sum();

    // RHS of (60)
    Scalar t0 = (l0 * r0).Sum();
    Scalar t_of_x = t0 + t1 * x + t2 * x.Square();

    // (60)
    if (proof.t_hat != t_of_x)
        throw std::runtime_error(strprintf("%s: equality didn't hold in (60)", __func__));

    proof.tau_x = (tau2 * x.Square()) + (tau1 * x) + (z_pows * gammas).Sum();  // (61)
    proof.mu = alpha + (rho * x);  // (62)

    // (63)
    transcript << x;
    transcript << proof.tau_x;
    transcript << proof.mu;
    transcript << proof.t;

    Scalar x_ip = transcript.GetHash();
    if (x_ip == 0)
        goto try_again;

    if (!InnerProductArgument(concat_input_values_in_bits, gens, x_ip, l, r, y, proof, transcript)) {
        goto try_again;
    }
    return proof;
}

// Serialize given Scalar, drop preceeding 0s and return
std::vector<uint8_t> RangeProof::GetTrimmedVch(const Scalar& s)
{
    auto vch = s.GetVch();
    std::vector<uint8_t> vch_trimmed;

    bool take_char = false;
    for (auto c: vch) {
        if (!take_char && c != '\0') take_char = true;
        if (take_char) vch_trimmed.push_back(c);
    }
    return vch_trimmed;
}

bool RangeProof::ValidateProofsBySizes(
    const std::vector<std::pair<size_t, Proof>>& indexed_proofs,
    const size_t& num_rounds
) const {
    for (const std::pair<size_t, Proof>& p: indexed_proofs) {
        const Proof proof = p.second;

        // proof must contain input values
        if (proof.Vs.Size() == 0) return false;

        // invalid if # of input values are lager than maximum
        if (proof.Vs.Size() > Config::m_max_input_values) return false;

        // L,R keep track of aggregation history and the size should equal to # of rounds
        if (proof.Ls.Size() != num_rounds)
            return false;

        // if Ls and Rs should have the same size
        if (proof.Ls.Size() != proof.Rs.Size()) return false;
    }
    return true;
}

VerifyLoop1Result RangeProof::VerifyLoop1(
    const std::vector<std::pair<size_t, Proof>>& indexed_proofs,
    const size_t& num_rounds
) const {
    VerifyLoop1Result res;

    for (const std::pair<size_t, Proof>& p: indexed_proofs) {
        const Proof proof = p.second;

        // update max # of rounds and sum of all V bits
        res.max_num_rounds = std::max(res.max_num_rounds, proof.Ls.Size());
        res.Vs_size_sum += proof.Vs.Size();

        // derive required Scalars from proof
        auto proof_deriv = ProofWithDerivedValues::Build(proof, num_rounds);
        res.proof_derivs.push_back(proof_deriv);
    }
    return res;
}

VerifyLoop2Result RangeProof::VerifyLoop2(
    const std::vector<ProofWithDerivedValues>& proof_derivs
) const {
    VerifyLoop2Result res;

    for (const ProofWithDerivedValues& p: proof_derivs) {

        const size_t M = p.num_input_values_power_2;
        const size_t MN = p.concat_input_values_in_bits;

        Scalar weight_y = Scalar::Rand();
        Scalar weight_z = Scalar::Rand();

        res.y0 = res.y0 - (p.proof.tau_x * weight_y);

        Scalars z_pow = Scalars::FirstNPow(p.z, M, 3); // VectorPowers(pd.z, M+3);

        // VectorPower returns {} for MN=0, FirstNPow returns {1} for MN=0
        // VectorPower returns {1} for MN=1, FirstNPow returns {1, p.y} for MN=1
        Scalar ip1y = Scalars::FirstNPow(p.y, MN - 1).Sum(); // VectorPowerSum(p.y, MN);

        // processing z_pow[2], z_pow[3] ... originally
        Scalar k = (z_pow[0] * ip1y).Negate();  // was z_pow[2]
        for (size_t i = 1; i <= M; ++i) {
            k = k - (z_pow[i] * RangeProof::m_inner_prod_ones_and_two_pows);  // was i + 2
        }

        res.y1 = res.y1 + ((p.proof.t - (k + (p.z * ip1y))) * weight_y);

        for (size_t i = 0; i < p.proof.Vs.Size(); ++i) {
            res.multi_exp.Add(p.proof.Vs[i] * (z_pow[i] * weight_y));  // was i + 2
        }

        res.multi_exp.Add(p.proof.T1 * (p.x * weight_y));
        res.multi_exp.Add(p.proof.T2 * (p.x.Square() * weight_y));
        res.multi_exp.Add(p.proof.A * weight_z);
        res.multi_exp.Add(p.proof.S * (p.x * weight_z));

        Scalar y_inv_pow = 1;
        Scalar y_pow = 1;

        std::vector<Scalar> w_cache(1 << p.num_rounds, 1);
        w_cache[0] = p.inv_ws[0];
        w_cache[1] = p.ws[0];

        for (size_t j = 1; j < p.num_rounds; ++j) {
            const size_t sl = 1<<(j+1);

            for (size_t s = sl; s-- > 0; --s) {
                w_cache[s] = w_cache[s/2] * p.ws[j];
                w_cache[s-1] = w_cache[s/2] * p.inv_ws[j];
            }
        }

        for (size_t i = 0; i < MN; ++i) {
            Scalar g_scalar = p.proof.a;
            Scalar h_scalar;

            if (i == 0) {
                h_scalar = p.proof.b;
            } else {
                h_scalar = p.proof.b * y_inv_pow;
            }

            g_scalar = g_scalar * w_cache[i];
            h_scalar = h_scalar * w_cache[(~i) & (MN-1)];

            g_scalar = g_scalar + p.z;

            Scalar tmp =
                z_pow[2 + i / Config::m_input_value_bits] *
                m_two_pows[i % Config::m_input_value_bits];
            if (i == 0) {
                h_scalar = h_scalar - (tmp + p.z);
            } else {
                h_scalar = h_scalar - ((tmp + (p.z * y_pow)) * y_inv_pow);
            }

            res.z4[i] = res.z4[i] - (g_scalar * weight_z);
            res.z5[i] = res.z5[i] - (h_scalar * weight_z);

            if (i == 0) {
                y_inv_pow = p.inv_y;
                y_pow = p.y;
            } else if (i != MN - 1) {
                y_inv_pow = y_inv_pow * p.inv_y;
                y_pow = y_pow * p.y;
            }
        }

        res.z1 = res.z1 + (p.proof.mu * weight_z);

        for (size_t i = 0; i < p.num_rounds; ++i) {
            res.multi_exp.Add(p.proof.Ls[i] * (p.ws[i].Square() * weight_z));
            res.multi_exp.Add(p.proof.Rs[i] * (p.inv_ws[i].Square() * weight_z));
        }

        res.z3 = res.z3 + (((p.proof.t - (p.proof.a * p.proof.b)) * p.x_ip) * weight_z);
    }
}

bool RangeProof::Verify(
    const std::vector<std::pair<size_t, Proof>>& indexed_proofs,
    const TokenId& token_id
) const {
    const size_t num_rounds = GetInnerProdArgRounds(Config::m_input_value_bits);
    if (!ValidateProofsBySizes(indexed_proofs, num_rounds)) return false;

    const VerifyLoop1Result loop1_res = VerifyLoop1(
        indexed_proofs,
        num_rounds
    );

    size_t maxMN = 1u << loop1_res.max_num_rounds;

    // loop2_res.base_exps will be further enriched, so not making it const
    VerifyLoop2Result loop2_res = VerifyLoop2(
        loop1_res.proof_derivs
    );

    const Scalar y0 = loop2_res.y0;
    const Scalar y1 = loop2_res.y1;
    const Scalar z1 = loop2_res.z1;
    const Scalar z3 = loop2_res.z3;
    const Scalars z4 = loop2_res.z4;
    const Scalars z5 = loop2_res.z5;

    const Generators gens = m_gf.GetInstance(token_id);

    loop2_res.multi_exp.Add(gens.G.get() * (y0 - z1));
    loop2_res.multi_exp.Add(gens.H * (z3 - y1));

    // place Gi and Hi side by side
    // multi_exp_data needs to be maxMN * 2 long. z4 and z5 needs to be maxMN long.
    for (size_t i = 0; i < maxMN; ++i) {
        loop2_res.multi_exp.Add(gens.Gi.get()[i] * z4[i]);
        loop2_res.multi_exp.Add(gens.Hi.get()[i] * z5[i]);
    }
    G1Point m_exp = loop2_res.multi_exp.Sum();

    return m_exp.IsUnity(); // m_exp == bls::G1Element::Infinity();
}

std::vector<RecoveredTxInput> RangeProof::RecoverTxIns(
    const std::vector<TxInToRecover>& tx_ins,
    const TokenId& token_id
) const {
    const Generators gens = m_gf.GetInstance(token_id);
    std::vector<RecoveredTxInput> recovered_tx_ins;  // will contain only recovered txins

    for (const TxInToRecover& tx_in: tx_ins) {
        // unable to recover if sizes of Ls and Rs differ or Vs is empty
        auto Ls_Rs_valid = tx_in.Ls.Size() > 0 && tx_in.Ls.Size() == tx_in.Rs.Size();
        if (tx_in.Vs.Size() == 0 || !Ls_Rs_valid) {
            continue;
        }

        // derive random Scalar values from nonce
        const Scalar alpha = tx_in.nonce.GetHashWithSalt(1);  // (A)
        const Scalar rho = tx_in.nonce.GetHashWithSalt(2);
        const Scalar tau1 = tx_in.nonce.GetHashWithSalt(3);  // (C)
        const Scalar tau2 = tx_in.nonce.GetHashWithSalt(4);
        const Scalar input_value0_gamma = tx_in.nonce.GetHashWithSalt(100);  // gamma for vs[0]

        // mu = alpha + rho * x ... (62)
        // alpha = mu - rho * x ... (B)
        //
        // alpha (B) equals to alpha (A) + (message || 64-byte v[0])
        // so by subtracting alpha (A) from alpha (B), you can extract (message || 64-byte v[0])
        // then applying 64-byte mask fuether extracts 64-byte v[0]
        const Scalar message_v0 = (tx_in.mu - rho * tx_in.x) - alpha;
        const Scalar input_value0 = message_v0 & Scalar(0xFFFFFFFFFFFFFFFF);

        // recovery fails if reproduced input value 0 commitment doesn't match with Vs[0]
        G1Point input_value0_commitment = (gens.G.get() * input_value0_gamma) + (gens.H * input_value0);
        if (input_value0_commitment != tx_in.Vs[0]) {
            continue;
        }

        // generate message and set to data
        // extract the message part from (up-to-23-byte message || 64-byte v[0])
        // by 64 bytes tot the right
        std::vector<uint8_t> msg1 = GetTrimmedVch(message_v0 >> 64);

        auto tau_x = tx_in.tau_x;
        auto x = tx_in.x;
        auto z = tx_in.z;

        // tau_x = tau2 * x^2 + tau1 * x + z^2 * gamma ... (61)
        //
        // solving this equation for tau1, you get:
        //
        // tau_x - tau2 * x^2 - z^2 * gamma = tau1 * x
        // tau1 = (tau_x - tau2 * x^2 - z^2 * gamma) * x^-1 ... (D)
        //
        // since tau1 in (61) is tau1 (C) + msg2, by subtracting tau1 (C) from RHS of (D)
        // you can extract msg2
        Scalar msg2_scalar = ((tau_x - (tau2 * x.Square()) - (z.Square() * input_value0_gamma)) * x.Invert()) - tau1;
        std::vector<uint8_t> msg2 = RangeProof::GetTrimmedVch(msg2_scalar);

        RecoveredTxInput recovered_tx_in(
            tx_in.index,
            input_value0.GetUint64(),
            input_value0_gamma,
            std::string(msg1.begin(), msg1.end()) + std::string(msg2.begin(), msg2.end())
        );
        recovered_tx_ins.push_back(recovered_tx_in);
    }
    return recovered_tx_ins;
}
