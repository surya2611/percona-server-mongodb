/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2020-present Percona and/or its affiliates. All rights reserved.

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

#include <string>

#include <sasl/sasl.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"

namespace mongo {

class CyrusSASLServerSession {
public:
    CyrusSASLServerSession(const CyrusSASLServerSession&) = delete;
    CyrusSASLServerSession(CyrusSASLServerSession&&) = delete;
    CyrusSASLServerSession& operator=(const CyrusSASLServerSession&) = delete;
    CyrusSASLServerSession& operator=(CyrusSASLServerSession&&) = delete;
    explicit CyrusSASLServerSession(const StringData mechanismName);
    ~CyrusSASLServerSession();

    StatusWith<std::tuple<bool, std::string>> step(StringData inputData);
    StringData getPrincipalName() const;

private:
    int _step{0};
    sasl_conn_t* _saslConnection{nullptr};
    std::string _mechanismName;

    struct SaslServerResults {
        int result = SASL_FAIL;
        const char* output = nullptr;
        unsigned length = 0;
        inline void initialize_results() {
            result = SASL_OK;
            output = nullptr;
            length = 0;
        };
        inline bool resultsAreOK() const {
            return result == SASL_OK;
        };
        inline bool resultsShowNoError() const {
            return result == SASL_OK || result == SASL_CONTINUE;
        }
    } _results;

    Status initializeConnection();
    StatusWith<std::tuple<bool, std::string>> processInitialClientPayload(
        const StringData& payload);
    StatusWith<std::tuple<bool, std::string>> processNextClientPayload(const StringData& payload);
    StatusWith<std::tuple<bool, std::string>> getStepResult() const;
};

}  // namespace mongo
