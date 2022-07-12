// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_RESULT_H
#define BITCOIN_UTIL_RESULT_H

#include <util/translation.h>

#include <variant>

/*
 * 'BResult' is a generic class useful for wrapping a return object
 * (in case of success) or propagating the error cause.
*/
template<class T>
class BResult {
private:
    std::variant<bilingual_str, T> m_variant;

public:
    BResult() : m_variant{Untranslated("")} {}
    BResult(T obj) : m_variant{std::move(obj)} {}
    BResult(bilingual_str error) : m_variant{std::move(error)} {}

    /* Whether the function succeeded or not */
    bool HasRes() const { return std::holds_alternative<T>(m_variant); }

    /* In case of success, the result object */
    const T& GetObj() const {
        assert(HasRes());
        return std::get<T>(m_variant);
    }
    T ReleaseObj()
    {
        assert(HasRes());
        return std::move(std::get<T>(m_variant));
    }

    /* In case of failure, the error cause */
    const bilingual_str& GetError() const {
        assert(!HasRes());
        return std::get<bilingual_str>(m_variant);
    }

    explicit operator bool() const { return HasRes(); }
};

/**
 * 'StructuredResult' is a generic class useful for wrapping a return object
 * (in case of success) or propagating the error cause.
 *
 * @tparam S stands for success object.
 * @tparam E stands for error object.
 */
template <class S, class E>
class StructuredResult
{
protected:
    std::variant<std::monostate, E, S> m_variant;

public:
    StructuredResult() {}
    StructuredResult(S obj) : m_variant{std::move(obj)} {}
    StructuredResult(E error) : m_variant{std::move(error)} {}

    /* Whether the function succeeded or not */
    bool HasRes() const { return std::holds_alternative<S>(m_variant); }

    /* In case of success, the result object */
    const S& GetObj() const
    {
        assert(HasRes());
        return std::get<S>(m_variant);
    }
    S ReleaseObj()
    {
        assert(HasRes());
        return std::move(std::get<S>(m_variant));
    }

    /* In case of failure, the error cause */
    const E& GetError() const
    {
        assert(!HasRes());
        return std::get<E>(m_variant);
    }

    explicit operator bool() const { return HasRes(); }
};

template <class T>
class SingeErrorResult : public StructuredResult<T, bilingual_str>
{
public:
    SingeErrorResult() : StructuredResult<T, bilingual_str>(Untranslated("")) {}
    SingeErrorResult(bilingual_str error) : StructuredResult<T, bilingual_str>(std::move(error)) {}
    SingeErrorResult(T obj) : StructuredResult<T, bilingual_str>(std::move(obj)) {}
};

#endif // BITCOIN_UTIL_RESULT_H
