/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2022-present Percona and/or its affiliates. All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the Server Side Public License, version 1,
    as published by MongoDB, Inc.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Server Side Public License for more details.

    You should have received a copy of the Server Side Public License
    along with this program. If not, see
    <http://www.mongodb.com/licensing/server-side-public-license>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the Server Side Public License in all respects for
    all of the code used other than as permitted herein. If you modify file(s)
    with this exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do so,
    delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then also delete
    it in the license file.
======= */

#pragma once

#include <cstddef>
#include <string>
#include <type_traits>

#include "mongo/base/secure_allocator.h"

namespace mongo {
class SecureRandom;
namespace encryption {

class Key {
public:
    ~Key();

    Key(const Key&);
    Key& operator=(const Key&);
    Key(Key&&);
    Key& operator=(Key&&);

    Key();
    explicit Key(SecureRandom& srng);
    Key(const std::byte* keyData, std::size_t keyDataSize);

    // @note. The `enable_if_t` instantiation verifies that the container type:
    // 1. has integral value type;
    // 2. has value type of size 1;
    // 3. has the `data` and the `size` member functions.
    template <
        typename ContiguousContainer,
        typename = std::enable_if_t<
            std::is_integral_v<typename ContiguousContainer::value_type> &&
            sizeof(typename ContiguousContainer::value_type) == 1 &&
            std::is_void_v<std::void_t<decltype(std::declval<ContiguousContainer>().data()),
                                       decltype(std::declval<ContiguousContainer>().size())>>>>
    explicit Key(const ContiguousContainer& keyData)
        : Key(reinterpret_cast<const std::byte*>(keyData.data()), keyData.size()) {}


    bool operator==(const Key& other) const noexcept;
    const std::byte* data() const noexcept;
    std::size_t size() const noexcept;
    std::string base64() const;

    static constexpr std::size_t kLength = 32;

private:
    std::byte* data() noexcept;

    SecureArray<std::byte, kLength> _data;
};
}  // namespace encryption
}  // namespace mongo
