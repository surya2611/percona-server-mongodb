/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/client/sdam/mock_topology_manager.h"

#include <string>

#include "mongo/client/sdam/topology_state_machine.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/topology_version_gen.h"

namespace mongo::sdam {

MockTopologyManager::MockTopologyManager() {}

bool MockTopologyManager::onServerDescription(const HelloOutcome& helloOutcome) {
    fassert(5429100, "MockTopologyManager does not support onServerDescription");
    return true;
}

std::shared_ptr<TopologyDescription> MockTopologyManager::getTopologyDescription() const {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    return _topologyDescription;
}

void MockTopologyManager::onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) {}

void MockTopologyManager::setTopologyDescription(TopologyDescriptionPtr newDescription) {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    _topologyDescription = newDescription;
}

SemiFuture<std::vector<HostAndPort>> MockTopologyManager::executeWithLock(
    std::function<SemiFuture<std::vector<HostAndPort>>(const TopologyDescriptionPtr&)> func) {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    return func(_topologyDescription);
}

}  // namespace mongo::sdam
