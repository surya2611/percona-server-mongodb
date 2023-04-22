/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/commit_chunk_migration_gen.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * This command takes the chunk being migrated ("migratedChunk") and generates a new version for it
 * that is written along with its new shard location ("toShard") to the chunks collection.
 *
 * The new chunk version is generated by querying the highest chunk version of the collection, and
 * then incrementing that major value for migrated chunks and setting the minor to
 * 0 for the migrated chunk. A global exclusive lock is held for the
 * duration of generating the new chunk version and writing to the chunks collection so that
 * yielding cannot occur. This assures that generated ChunkVersions are strictly monotonically
 * increasing -- a second process will not be able to query for max chunk version until the first
 * finishes writing the new highest chunk version it generated.
 *
 * Command Format:
 * {
 *   _configsvrCommitChunkMigration: <database>.<collection>,
 *   fromShard: "<from_shard_name>",
 *   toShard: "<to_shard_name>",
 *   migratedChunk: {min: <min_value>, max: <max_value>, etc. },
 *   fromShardCollectionVersion: { shardVersionField: <version> }, (for backward compatibility only)
 * }
 *
 * Returns:
 * {
 *   migratedChunkVersion: <ChunkVersion_BSON>,
 * }
 *
 */


ChunkType toChunkType(const MigratedChunkType& migratedChunk) {

    ChunkType chunk;
    chunk.setMin(migratedChunk.getMin());
    chunk.setMax(migratedChunk.getMax());
    chunk.setVersion(migratedChunk.getLastmod());
    return chunk;
}


class ConfigSvrCommitChunkMigrationCommand
    : public TypedCommand<ConfigSvrCommitChunkMigrationCommand> {
public:
    using Request = CommitChunkMigrationRequest;
    using Response = ConfigSvrCommitChunkMigrationResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "should not be calling this directly";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        ConfigSvrCommitChunkMigrationResponse typedRun(OperationContext* opCtx) {

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCommitChunkMigration can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const NamespaceString nss = ns();
            auto migratedChunk = toChunkType(request().getMigratedChunk());

            StatusWith<BSONObj> chunkVersionResponse =
                ShardingCatalogManager::get(opCtx)->commitChunkMigration(
                    opCtx,
                    nss,
                    migratedChunk,
                    request().getFromShardCollectionVersion().epoch(),
                    request().getFromShardCollectionVersion().getTimestamp(),
                    request().getFromShard(),
                    request().getToShard(),
                    request().getValidAfter());

            auto chunkVersionObj = uassertStatusOK(chunkVersionResponse);

            return Response{ChunkVersion::parse(chunkVersionObj[ChunkVersion::kChunkVersionField])};
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} configsvrCommitChunkMigrationCommand;

}  // namespace
}  // namespace mongo
