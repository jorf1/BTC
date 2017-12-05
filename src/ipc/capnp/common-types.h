// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_CAPNP_COMMON_TYPES_H
#define BITCOIN_IPC_CAPNP_COMMON_TYPES_H

#include <clientversion.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <univalue.h>

#include <cstddef>
#include <mp/proxy-types.h>
#include <type_traits>
#include <utility>

namespace ipc {
namespace capnp {
//! Construct a ParamStream wrapping data stream with serialization parameters
//! needed to pass transaction objects between bitcoin processes.
template<typename S>
auto Wrap(S& s)
{
    return ParamsStream{s, TX_WITH_WITNESS};
}

//! Use SFINAE to define Serializeable<T> trait which is true if type T has a
//! Serialize(stream) method, false otherwise.
template <typename T>
struct Serializable {
private:
    template <typename C>
    static std::true_type test(decltype(std::declval<C>().Serialize(std::declval<std::nullptr_t&>()))*);
    template <typename>
    static std::false_type test(...);

public:
    static constexpr bool value = decltype(test<T>(nullptr))::value;
};

//! Use SFINAE to define Unserializeable<T> trait which is true if type T has
//! an Unserialize(stream) method, false otherwise.
template <typename T>
struct Unserializable {
private:
    template <typename C>
    static std::true_type test(decltype(std::declval<C>().Unserialize(std::declval<std::nullptr_t&>()))*);
    template <typename>
    static std::false_type test(...);

public:
    static constexpr bool value = decltype(test<T>(nullptr))::value;
};

//! Trait which is true if type has a deserialize_type constructor, which is
//! used to deserialize types like CTransaction that can't be unserialized into
//! existing objects because they are immutable.
template <typename T>
using Deserializable = std::is_constructible<T, ::deserialize_type, ::DataStream&>;
} // namespace capnp
} // namespace ipc

//! Functions to serialize / deserialize common bitcoin types.
namespace mp {
//! Overload multiprocess library's CustomBuildField hook to allow any
//! serializable object to be stored in a capnproto Data field or passed to a
//! capnproto interface. Use Priority<1> so this hook has medium priority, and
//! higher priority hooks could take precedence over this one.
template <typename LocalType, typename Value, typename Output>
void CustomBuildField(
    TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context, Value&& value, Output&& output,
    // Enable if serializeable and if LocalType is not cv or reference
    // qualified. If LocalType is cv or reference qualified, it is important to
    // fall back to lower-priority Priority<0> implementation of this function
    // that strips cv references, to prevent this CustomBuildField overload from
    // taking precedence over more narrow overloads for specific LocalTypes.
    std::enable_if_t<ipc::capnp::Serializable<LocalType>::value &&
                     std::is_same_v<LocalType, std::remove_cv_t<std::remove_reference_t<LocalType>>>>* enable = nullptr)
{
    DataStream stream;
    auto wrapper{ipc::capnp::Wrap(stream)};
    value.Serialize(wrapper);
    auto result = output.init(stream.size());
    memcpy(result.begin(), stream.data(), stream.size());
}

//! Overload multiprocess library's CustomReadField hook to allow any object
//! with an Unserialize method to be read from a capnproto Data field or
//! returned from capnproto interface. Use Priority<1> so this hook has medium
//! priority, and higher priority hooks could take precedence over this one.
template <typename LocalType, typename Input, typename ReadDest>
decltype(auto)
CustomReadField(TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context, Input&& input, ReadDest&& read_dest,
                std::enable_if_t<ipc::capnp::Unserializable<LocalType>::value &&
                                 !ipc::capnp::Deserializable<LocalType>::value>* enable = nullptr)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) return;
        auto data = input.get();
        SpanReader stream({data.begin(), data.end()});
        auto wrapper{ipc::capnp::Wrap(stream)};
        value.Unserialize(wrapper);
    });
}

//! Overload multiprocess library's CustomReadField hook to allow any object
//! with an deserialize constructor to be read from a capnproto Data field or
//! returned from capnproto interface. Use Priority<1> so this hook has medium
//! priority, and higher priority hooks could take precedence over this one.
template <typename LocalType, typename Input, typename ReadDest>
decltype(auto)
CustomReadField(TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context, Input&& input, ReadDest&& read_dest,
                std::enable_if_t<ipc::capnp::Deserializable<LocalType>::value>* enable = nullptr)
{
    assert(input.has());
    auto data = input.get();
    SpanReader stream({data.begin(), data.end()});
    auto wrapper{ipc::capnp::Wrap(stream)};
    return read_dest.construct(deserialize, wrapper);
}

//! Overload CustomBuildField and CustomReadField to serialize UniValue
//! parameters and return values as JSON strings.
template <typename Value, typename Output>
void CustomBuildField(TypeList<UniValue>, Priority<1>, InvokeContext& invoke_context, Value&& value, Output&& output)
{
    std::string str = value.write();
    auto result = output.init(str.size());
    memcpy(result.begin(), str.data(), str.size());
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<UniValue>, Priority<1>, InvokeContext& invoke_context, Input&& input,
                               ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        auto data = input.get();
        value.read(std::string_view{data.begin(), data.size()});
    });
}
} // namespace mp

#endif // BITCOIN_IPC_CAPNP_COMMON_TYPES_H
